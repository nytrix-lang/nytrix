#include "code/native/native.h"
#include "code/native/object/internal.h"
#include "code/native/ir/machine.h"
#include "code/jit.h"
#include "wire/cache.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif
#endif

static size_t ny_native_jit_align(size_t value, size_t align) {
  return align > 1 ? (value + align - 1) & ~(align - 1) : value;
}

static void *ny_native_jit_alloc(size_t size) {
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__APPLE__) && defined(__aarch64__)
  flags |= MAP_JIT;
  pthread_jit_write_protect_np(0);
#endif
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

static bool ny_native_jit_seal(void *memory, size_t size) {
#ifdef _WIN32
  DWORD old_protect = 0;
  return VirtualProtect(memory, size, PAGE_EXECUTE_READ, &old_protect) != 0;
#else
  __builtin___clear_cache((char *)memory, (char *)memory + size);
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(1);
  return true;
#else
  return mprotect(memory, size, PROT_READ | PROT_EXEC) == 0;
#endif
#endif
}

void ny_native_jit_image_free(ny_native_jit_image_t *image) {
  if (!image || !image->memory)
    return;
#ifdef _WIN32
  VirtualFree(image->memory, 0, MEM_RELEASE);
#else
  munmap(image->memory, image->size);
#endif
  *image = (ny_native_jit_image_t){0};
}

static void *ny_native_jit_symbol(
    unsigned char *base, const ny_x64_obj_symbol_def_t *defs,
    size_t def_count, const char *name) {
  int index = ny_x64_obj_def_index(defs, def_count, name);
  if (index >= 0)
    return base + defs[index].off;
  return ny_jit_resolve_symbol(name);
}

static void ny_native_visit_stmt_links(const stmt_t *stmt,
                                       ny_native_link_visitor_t visitor,
                                       void *ctx) {
  if (!stmt)
    return;
  switch (stmt->kind) {
  case NY_S_INCLUDE:
    if (stmt->as.inc.lib && stmt->as.inc.lib[0])
      visitor(stmt->as.inc.lib, ctx);
    return;
  case NY_S_LINK:
    if (stmt->as.link.lib && stmt->as.link.lib[0])
      visitor(stmt->as.link.lib, ctx);
    return;
  case NY_S_MODULE:
    for (size_t i = 0; i < stmt->as.module.body.len; ++i)
      ny_native_visit_stmt_links(stmt->as.module.body.data[i], visitor, ctx);
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      ny_native_visit_stmt_links(stmt->as.block.body.data[i], visitor, ctx);
    return;
  case NY_S_IF:
    ny_native_visit_stmt_links(stmt->as.iff.init, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.iff.conseq, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.iff.alt, visitor, ctx);
    return;
  case NY_S_WHILE:
    ny_native_visit_stmt_links(stmt->as.whl.init, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.whl.body, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.whl.update, visitor, ctx);
    return;
  case NY_S_FOR:
    ny_native_visit_stmt_links(stmt->as.fr.init, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.fr.body, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.fr.update, visitor, ctx);
    return;
  case NY_S_TRY:
    ny_native_visit_stmt_links(stmt->as.tr.body, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.tr.handler, visitor, ctx);
    return;
  case NY_S_DEFER:
    ny_native_visit_stmt_links(stmt->as.de.body, visitor, ctx);
    return;
  case NY_S_MATCH:
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i)
      ny_native_visit_stmt_links(stmt->as.match.arms.data[i].conseq, visitor,
                                 ctx);
    ny_native_visit_stmt_links(stmt->as.match.default_conseq, visitor, ctx);
    return;
  default:
    return;
  }
}

void ny_native_visit_program_links(const program_t *prog,
                                   ny_native_link_visitor_t visitor,
                                   void *ctx) {
  if (!prog || !visitor)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_visit_stmt_links(prog->body.data[i], visitor, ctx);
}

static void ny_native_jit_load_link(const char *library, void *ctx) {
  (void)ctx;
  (void)ny_jit_load_library(library);
}

static bool ny_native_jit_compile_aarch64_bundle(
    const nyir_func_t *top, const nyir_func_t *funcs,
    const char *const *names, size_t func_count,
    const ny_native_target_info_t *target, ny_native_jit_image_t *image,
    char *err, size_t err_len) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[256];
  ny_x64_obj_reloc_t relocs[256];
  size_t def_count = 0, reloc_count = 0;
  if (!ny_a64_obj_build_bundle(top, funcs, names, func_count, target,
                               "rt_main", false, &code, defs, &def_count,
                               relocs, &reloc_count, err, err_len)) {
    ny_obj_free(&code);
    return false;
  }
  const size_t stub_size = 16;
  size_t used = ny_native_jit_align(code.len, 16);
  size_t alloc_size = ny_native_jit_align(used + reloc_count * stub_size, 4096);
  unsigned char *memory = (unsigned char *)ny_native_jit_alloc(alloc_size);
  if (!memory) {
    ny_native_set_err(err, err_len,
                      "native AArch64 JIT: executable allocation failed");
    ny_obj_free(&code);
    return false;
  }
  memset(memory, 0, alloc_size);
  memcpy(memory, code.data, code.len);
  ny_obj_free(&code);
  for (size_t i = 0; i < reloc_count; ++i) {
    void *resolved = ny_native_jit_symbol(memory, defs, def_count,
                                          relocs[i].symbol);
    if (!resolved) {
      ny_native_set_err(err, err_len,
                        "native AArch64 JIT: unresolved symbol '%s'",
                        relocs[i].symbol);
      image->memory = memory; image->size = alloc_size;
      ny_native_jit_image_free(image);
      return false;
    }
    unsigned char *branch_target = (unsigned char *)resolved;
    if (ny_x64_obj_def_index(defs, def_count, relocs[i].symbol) < 0) {
      unsigned char *stub = memory + used;
      const uint32_t load_x16 = 0x58000050u;
      const uint32_t branch_x16 = 0xd61f0200u;
      uint64_t absolute = (uint64_t)(uintptr_t)resolved;
      memcpy(stub, &load_x16, sizeof(load_x16));
      memcpy(stub + 4, &branch_x16, sizeof(branch_x16));
      memcpy(stub + 8, &absolute, sizeof(absolute));
      branch_target = stub;
      used += stub_size;
    }
    unsigned char *patch = memory + relocs[i].disp_off;
    intptr_t delta = branch_target - patch;
    if ((delta & 3) != 0 || delta / 4 < -(1 << 25) ||
        delta / 4 >= (1 << 25)) {
      ny_native_set_err(err, err_len,
                        "native AArch64 JIT: CALL26 relocation for '%s' is out of range",
                        relocs[i].symbol);
      image->memory = memory; image->size = alloc_size;
      ny_native_jit_image_free(image);
      return false;
    }
    uint32_t insn = 0;
    memcpy(&insn, patch, sizeof(insn));
    insn = (insn & 0xfc000000u) |
           ((uint32_t)(delta / 4) & 0x03ffffffu);
    memcpy(patch, &insn, sizeof(insn));
  }
  int entry_index = ny_x64_obj_def_index(defs, def_count,
                                          target->symbol_prefix[0]
                                              ? "_rt_main" : "rt_main");
  if (entry_index < 0 || !ny_native_jit_seal(memory, alloc_size)) {
    ny_native_set_err(err, err_len,
                      "native AArch64 JIT: executable finalization failed");
    image->memory = memory; image->size = alloc_size;
    ny_native_jit_image_free(image);
    return false;
  }
  image->memory = memory;
  image->size = alloc_size;
  image->entry = memory + defs[entry_index].off;
  return true;
}

bool ny_native_jit_compile(const program_t *prog, const ny_options *opt,
                           ny_native_jit_image_t *image, char *err,
                           size_t err_len) {
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                           opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  if (image)
    *image = (ny_native_jit_image_t){0};
  if (!prog || !opt || !image) {
    ny_native_set_err(err, err_len, "native JIT: missing input");
    return false;
  }
  ny_native_target_info_t target;
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len, "native JIT: backend is disabled");
    return false;
  }
  if ((target.caps & (unsigned)NY_NATIVE_CAP_LIVE_JIT) == 0) {
    ny_native_set_err(err, err_len,
                      "native JIT is not available for target '%s'; this target supports NYIR text/VM paths only",
                      target.target_name ? target.target_name : "unknown");
    return false;
  }
#if defined(__aarch64__) || defined(_M_ARM64)
  if (target.target != NY_NATIVE_TARGET_AARCH64) {
    ny_native_set_err(err, err_len,
                      "native JIT: host AArch64 requires the AArch64 backend");
    return false;
  }
#elif defined(__x86_64__) || defined(_M_X64)
  if (target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native JIT: host x86-64 requires the x86-64 backend");
    return false;
  }
#else
  ny_native_set_err(err, err_len,
                    "native JIT: this host architecture has no in-memory encoder");
  return false;
#endif

  nyir_func_t top = {0};
  nyir_func_t funcs[NY_NATIVE_LIVE_MAX_FUNCS] = {{0}};
  size_t func_count = 0;
  if (!ny_native_build_nir(prog, opt, &top, funcs, &func_count,
                           NY_NATIVE_LIVE_MAX_FUNCS, err,
                           err_len) || top.len == 0)
    goto fail_nir;
  /* Consume the shared machine form stream as a hard gate before the host
   * in-memory encoder. The x86-64/AArch64 byte encoders still lower from the
   * verified NYIR that produced this machine form; both forms must agree. */
  {
    char mach_err[256] = {0};
    ny_mach_func_t mach = {0};
    if (!ny_mach_lower_nir(&top, &mach, mach_err, sizeof(mach_err)) ||
        !ny_mach_verify(&mach, mach_err, sizeof(mach_err))) {
      ny_native_set_err(err, err_len, "native JIT: machine form gate failed for rt_main: %s",
                        mach_err[0] ? mach_err : "verify");
      ny_mach_func_free(&mach);
      goto fail_nir;
    }
    ny_mach_func_free(&mach);
    for (size_t i = 0; i < func_count; ++i) {
      mach = (ny_mach_func_t){0};
      if (!ny_mach_lower_nir(&funcs[i], &mach, mach_err, sizeof(mach_err)) ||
          !ny_mach_verify(&mach, mach_err, sizeof(mach_err))) {
        ny_native_set_err(err, err_len,
                          "native JIT: machine form gate failed for function %zu: %s",
                          i, mach_err[0] ? mach_err : "verify");
        ny_mach_func_free(&mach);
        goto fail_nir;
      }
      ny_mach_func_free(&mach);
    }
  }
  const char *names[NY_NATIVE_LIVE_MAX_FUNCS] = {0};
  size_t name_count = 0;
  for (size_t i = 0; i < prog->body.len && name_count < func_count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[name_count++] = stmt->as.fn.name;
  }

  ny_jit_add_runtime_symbols();
  for (size_t i = 0; i < opt->link_libs.len; ++i)
    (void)ny_jit_load_library(opt->link_libs.data[i]);
  ny_native_visit_program_links(prog, ny_native_jit_load_link, NULL);
  if (target.target == NY_NATIVE_TARGET_AARCH64) {
    if (!ny_native_jit_compile_aarch64_bundle(
            &top, funcs, names, func_count, &target, image, err, err_len))
      goto fail_nir;
    for (size_t i = 0; i < func_count; ++i)
      nyir_func_free(&funcs[i]);
    nyir_func_free(&top);
    return true;
  }

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[256];
  ny_x64_obj_reloc_t relocs[NY_X64_OBJ_MAX_RELOCS];
  size_t def_count = 0, reloc_count = 0;
  /* Primary independence path: machine form → bytes. Fall back to the legacy
   * NYIR object encoder only when machine form encode rejects the shape. */
  bool used_mir = false;
  const char *encode_path = "machine";
  /* Tier-0 stencil: const/local shell; also fold pure helper calls. */
  if (func_count == 0 &&
      ny_x64_try_stencil_bundle(&top, &target, &code, defs, &def_count, relocs,
                                &reloc_count, err, err_len)) {
    used_mir = true;
    encode_path = "stencil";
  } else if (func_count > 0 &&
             ny_x64_try_stencil_bundle_calls(&top, funcs, names, func_count,
                                             &target, &code, defs, &def_count,
                                             relocs, &reloc_count, err,
                                             err_len)) {
    used_mir = true;
    encode_path = "stencil";
  }
  if (!used_mir) {
    ny_mach_func_t top_mach = {0};
    ny_mach_func_t *fm = NULL;
    char mach_err[256] = {0};
    bool mach_ok = ny_mach_lower_nir(&top, &top_mach, mach_err, sizeof(mach_err));
    if (mach_ok && func_count) {
      fm = calloc(func_count, sizeof(*fm));
      if (!fm)
        mach_ok = false;
      for (size_t i = 0; mach_ok && i < func_count; ++i)
        mach_ok = ny_mach_lower_nir(&funcs[i], &fm[i], mach_err, sizeof(mach_err));
    }
    if (mach_ok &&
        ny_x64_mach_build_bundle(&top_mach, fm, names, func_count, &target,
                                "rt_main", false, &code, defs, &def_count,
                                relocs, &reloc_count, mach_err,
                                sizeof(mach_err))) {
      used_mir = true;
      encode_path = "machine";
    } else {
      ny_obj_free(&code);
      code = (ny_obj_buf_t){0};
      def_count = reloc_count = 0;
    }
    ny_mach_func_free(&top_mach);
    if (fm) {
      for (size_t i = 0; i < func_count; ++i)
        ny_mach_func_free(&fm[i]);
      free(fm);
    }
  }
  if (!used_mir &&
      !ny_x64_obj_build_bundle(&top, funcs, names, func_count, &target,
                               "rt_main", false, &code, defs, &def_count,
                               relocs, &reloc_count, err, err_len)) {
    ny_obj_free(&code);
    goto fail_nir;
  }
  if (!used_mir)
    encode_path = "nyir";
  if (opt && opt->verbose >= 2)
    fprintf(stderr, "native jit encoder=%s\n", encode_path);

  const size_t stub_size = 16;
  size_t used = ny_native_jit_align(code.len, 16);
  size_t alloc_size = ny_native_jit_align(used + reloc_count * stub_size, 4096);
  unsigned char *memory = (unsigned char *)ny_native_jit_alloc(alloc_size);
  if (!memory) {
    ny_native_set_err(err, err_len, "native JIT: executable allocation failed");
    ny_obj_free(&code);
    goto fail_nir;
  }
  memset(memory, 0x90, alloc_size);
  memcpy(memory, code.data, code.len);
  /* Stencil cache: persist self-contained code (zero relocs) so subsequent
   * runs skip parse + lower + encode entirely.  The cache is an ELF shared
   * object and is therefore intentionally unavailable on Windows. */
#ifndef _WIN32
  if (reloc_count == 0 && prog && prog->raw_src && prog->raw_src_len > 0) {
    char *cache_path =
        ny_jit_stencil_cache_path(prog->raw_src, opt ? opt->opt_level : 0);
    if (cache_path) {
      ny_jit_stencil_cache_save(cache_path, code.data, code.len, "rt_main");
      free(cache_path);
    }
  } else if (opt && opt->verbose >= 2) {
    fprintf(stderr, "stencil skip: relocs=%zu prog=%p raw=%p len=%zu\n",
            reloc_count, (void*)prog,
            (void*)(prog ? (const void*)prog->raw_src : 0),
            prog ? prog->raw_src_len : 0);
  }
#endif
  ny_obj_free(&code);

  for (size_t i = 0; i < reloc_count; ++i) {
    void *target_ptr = ny_native_jit_symbol(memory, defs, def_count,
                                            relocs[i].symbol);
    if (!target_ptr) {
      ny_native_set_err(err, err_len, "native JIT: unresolved symbol '%s'",
                        relocs[i].symbol);
      image->memory = memory;
      image->size = alloc_size;
      ny_native_jit_image_free(image);
      goto fail_nir;
    }
    unsigned char *patch_at = memory + relocs[i].disp_off;
    unsigned char *after = patch_at + 4;
    if (relocs[i].type == NY_RELOC_PC32) {
      /* Data address: leaq sym(%rip), reg — patch direct RIP-relative disp. */
      intptr_t delta = (unsigned char *)target_ptr - after;
      if (delta < INT32_MIN || delta > INT32_MAX) {
        ny_native_set_err(err, err_len,
                          "native JIT: PC32 relocation for '%s' is out of range",
                          relocs[i].symbol);
        image->memory = memory;
        image->size = alloc_size;
        ny_native_jit_image_free(image);
        goto fail_nir;
      }
      int32_t disp = (int32_t)delta;
      memcpy(patch_at, &disp, sizeof(disp));
    } else {
      /* Call address: use stub for external symbols that may be far away. */
      int def_index = ny_x64_obj_def_index(defs, def_count, relocs[i].symbol);
      unsigned char *branch_target = (unsigned char *)target_ptr;
      if (def_index < 0) {
        unsigned char *stub = memory + used;
        stub[0] = 0x48;
        stub[1] = 0xb8;
        uint64_t absolute = (uint64_t)(uintptr_t)target_ptr;
        memcpy(stub + 2, &absolute, sizeof(absolute));
        stub[10] = 0xff;
        stub[11] = 0xe0;
        branch_target = stub;
        used += stub_size;
      }
      intptr_t delta = branch_target - after;
      if (delta < INT32_MIN || delta > INT32_MAX) {
        ny_native_set_err(err, err_len,
                          "native JIT: relocation for '%s' is out of range",
                          relocs[i].symbol);
        image->memory = memory;
        image->size = alloc_size;
        ny_native_jit_image_free(image);
        goto fail_nir;
      }
      int32_t disp = (int32_t)delta;
      memcpy(patch_at, &disp, sizeof(disp));
    }
  }

  int entry_index = ny_x64_obj_def_index(defs, def_count, "rt_main");
  if (entry_index < 0 || !ny_native_jit_seal(memory, alloc_size)) {
    ny_native_set_err(err, err_len, "native JIT: executable finalization failed");
    image->memory = memory;
    image->size = alloc_size;
    ny_native_jit_image_free(image);
    goto fail_nir;
  }
  image->memory = memory;
  image->size = alloc_size;
  image->entry = memory + defs[entry_index].off;
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  nyir_func_free(&top);
  return true;

fail_nir:
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  nyir_func_free(&top);
  return false;
}
