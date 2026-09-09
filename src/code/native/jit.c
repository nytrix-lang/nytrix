/*
 * Native JIT: compiles NYIR to native code in-process, manages
 * writable-executable code pages, and provides a native call trampoline.
 */
#include "code/native/native.h"
#include "code/native/object/internal.h"
#include "code/ir/machine.h"
#include "code/native/llvm/jit.h"
#include "code/wire/cache.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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

static void *ny_native_jit_alloc_data(size_t size) {
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
#endif
}

/*
 * Native C imports must bind to the process ABI before consulting the LLVM
 * symbol table.  Unqualified names such as `div` can also exist as Nytrix
 * functions in an LLVM module; resolving LLVM first silently selects the
 * wrong calling convention and corrupts aggregate returns.
 */
static void *ny_native_jit_resolve_external(const char *name) {
#ifndef _WIN32
  if (name && *name) {
    void *ptr = dlsym(RTLD_DEFAULT, name);
    if (ptr)
      return ptr;
  }
#endif
  return ny_jit_resolve_symbol(name);
}

static bool ny_native_jit_def_is_data(const char *name) {
  return name && (strncmp(name, ".Lnyarr.", 8) == 0 ||
                  ny_native_globaltab_has(name) ||
                  /*
                   * String literals are emitted by the native object
                   * builder into the same trailing data pool as arrays and
                   * globals.  Keep their relocations in that RW mapping;
                   * resolving them against the executable text mapping
                   * loses the literal's actual address (and commonly turns
                   * `"err"` into `"e"`).
                   */
                  ny_native_strtab_get(name, NULL) != NULL);
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
  if (!image)
    return;
  if (image->data_memory && image->data_size) {
#ifdef _WIN32
    VirtualFree(image->data_memory, 0, MEM_RELEASE);
#else
    munmap(image->data_memory, image->data_size);
#endif
  }
  if (!image->memory)
    return;
#ifdef _WIN32
  VirtualFree(image->memory, 0, MEM_RELEASE);
#else
  munmap(image->memory, image->size);
#endif
}

static void *ny_native_jit_symbol(
    unsigned char *base, const ny_x64_obj_symbol_def_t *defs,
    size_t def_count, const char *name) {
  int index = ny_x64_obj_def_index(defs, def_count, name);
  if (index >= 0) {
    return base + defs[index].off;
  }
  /*
   * Re-export alias resolution: a caller may reference a re-exported
   * function under its alias (std.core.set) while the definition is
   * registered under its canonical module name (std.core.set_mod.set).
   * When exactly one definition matches either exactly or with one
   * trailing `_mod` module segment dropped, link it.
   */
  if (name && strncmp(name, "ny_fn_", 6) == 0) {
    const char *fully = name + 6;
    int match = -1;
    size_t matches = 0;
    for (size_t z = 0; z < def_count; ++z) {
      const char *cand = defs[z].name;
      if (strncmp(cand, "ny_fn_", 6) != 0)
        continue;
      cand += 6;
      if (strcmp(cand, fully) == 0) {
        match = (int)z;
        matches++;
        continue;
      }
      const char *last_dot = strrchr(cand, '.');
      if (!last_dot || last_dot == cand)
        continue;
      const char *seg_start = last_dot;
      while (seg_start > cand && seg_start[-1] != '.')
        --seg_start;
      size_t seg_len = (size_t)(last_dot - seg_start);
      if (seg_len >= 4 &&
          strncmp(seg_start + seg_len - 4, "_mod", 4) == 0) {
        size_t prefix_len = (size_t)(seg_start - cand);
        if (prefix_len >= 1 && prefix_len < 256) {
          char munged[256];
          size_t k = 0;
          if (prefix_len > 1) {
            memcpy(munged, cand, prefix_len - 1);
            k = prefix_len - 1;
          }
          munged[k] = '\0';
          if (strcmp(munged, fully) == 0) {
            match = (int)z;
            matches++;
          }
        }
      }
    }
    if (matches == 1 && match >= 0)
      return base + defs[match].off;
  }
  return ny_native_jit_resolve_external(name);
}

static void ny_native_visit_stmt_links(const stmt_t *stmt, const ny_options *opt,
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
      ny_native_visit_stmt_links(stmt->as.module.body.data[i], opt, visitor, ctx);
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      ny_native_visit_stmt_links(stmt->as.block.body.data[i], opt, visitor, ctx);
    return;
  case NY_S_IF: {
    bool selected = false;
    ny_native_visit_stmt_links(stmt->as.iff.init, opt, visitor, ctx);
    if (stmt->as.iff.test && stmt->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, stmt->as.iff.test, &selected)) {
      ny_native_visit_stmt_links(selected ? stmt->as.iff.conseq : stmt->as.iff.alt,
                                 opt, visitor, ctx);
      return;
    }
    ny_native_visit_stmt_links(stmt->as.iff.conseq, opt, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.iff.alt, opt, visitor, ctx);
    return;
  }
  case NY_S_WHILE:
    ny_native_visit_stmt_links(stmt->as.whl.init, opt, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.whl.body, opt, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.whl.update, opt, visitor, ctx);
    return;
  case NY_S_FOR:
    ny_native_visit_stmt_links(stmt->as.fr.init, opt, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.fr.body, opt, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.fr.update, opt, visitor, ctx);
    return;
  case NY_S_TRY:
    ny_native_visit_stmt_links(stmt->as.tr.body, opt, visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.tr.handler, opt, visitor, ctx);
    return;
  case NY_S_DEFER:
    ny_native_visit_stmt_links(stmt->as.de.body, opt, visitor, ctx);
    return;
  case NY_S_MATCH:
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i)
      ny_native_visit_stmt_links(stmt->as.match.arms.data[i].conseq, opt,
                                 visitor, ctx);
    ny_native_visit_stmt_links(stmt->as.match.default_conseq, opt, visitor, ctx);
    return;
  default:
    return;
  }
}

void ny_native_visit_program_links_for_options(const program_t *prog,
                                               const ny_options *opt,
                                               ny_native_link_visitor_t visitor,
                                               void *ctx) {
  if (!prog || !visitor)
    return;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_visit_stmt_links(prog->body.data[i], opt, visitor, ctx);
}

void ny_native_visit_program_links(const program_t *prog,
                                   ny_native_link_visitor_t visitor,
                                   void *ctx) {
  ny_native_visit_program_links_for_options(prog, NULL, visitor, ctx);
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
  ny_x64_obj_symbol_def_t *defs =
      calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  ny_x64_obj_reloc_t *relocs =
      calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    defs = NULL;
    relocs = NULL;
    ny_native_set_err(err, err_len,
                      "native AArch64 JIT definition allocation failed");
    return false;
  }
  if (!ny_a64_obj_build_bundle(top, funcs, names, func_count, target,
                               "rt_main", false, &code, defs, &def_count,
                               relocs, &reloc_count, err, err_len)) {
    ny_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  const size_t stub_size = 16;
  size_t data_start = code.len, data_end = code.len;
  bool have_data = false;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_native_jit_def_is_data(defs[i].name))
      continue;
    if (!have_data || defs[i].off < data_start)
      data_start = defs[i].off;
    size_t end = defs[i].off + defs[i].size;
    if (!have_data || end > data_end)
      data_end = end;
    have_data = true;
  }
  size_t text_len = have_data ? data_start : code.len;
  size_t data_len =
      have_data ? ny_native_jit_align(data_end - data_start, 16) : 0;
  size_t used = ny_native_jit_align(text_len, 16);
  size_t alloc_size =
      ny_native_jit_align(used + reloc_count * stub_size, 4096);
  unsigned char *memory = (unsigned char *)ny_native_jit_alloc(alloc_size);
  if (!memory) {
    ny_native_set_err(err, err_len,
                      "native AArch64 JIT: executable allocation failed");
    ny_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  memset(memory, 0, alloc_size);
  memcpy(memory, code.data, text_len);

  unsigned char *data_memory = NULL;
  if (data_len > 0) {
    data_memory = (unsigned char *)ny_native_jit_alloc_data(data_len);
    if (!data_memory) {
      ny_native_set_err(err, err_len,
                        "native AArch64 JIT: data pool allocation failed");
      ny_obj_free(&code);
      image->memory = memory;
      image->size = alloc_size;
      ny_native_jit_image_free(image);
      *image = (ny_native_jit_image_t){0};
      free(defs);
      free(relocs);
      return false;
    }
    memset(data_memory, 0, data_len);
    memcpy(data_memory, code.data + data_start, data_end - data_start);
    image->data_memory = data_memory;
    image->data_size = data_len;
  }
  ny_obj_free(&code);
  for (size_t i = 0; i < reloc_count; ++i) {
    void *resolved;
    if (data_memory && ny_native_jit_def_is_data(relocs[i].symbol)) {
      int index = ny_x64_obj_def_index(defs, def_count, relocs[i].symbol);
      resolved = index >= 0 ? data_memory + (defs[index].off - data_start)
                            : ny_native_jit_resolve_external(relocs[i].symbol);
    } else {
      resolved = ny_native_jit_symbol(memory, defs, def_count,
                                      relocs[i].symbol);
    }
    if (!resolved) {
      ny_native_set_err(err, err_len,
                        "native AArch64 JIT: unresolved symbol '%s'",
                        relocs[i].symbol);
      image->memory = memory;
      image->size = alloc_size;
      ny_native_jit_image_free(image);
      *image = (ny_native_jit_image_t){0};
      free(defs);
      free(relocs);
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
      ny_native_set_err(
          err, err_len,
          "native AArch64 JIT: CALL26 relocation for '%s' is out of range",
          relocs[i].symbol);
      image->memory = memory;
      image->size = alloc_size;
      ny_native_jit_image_free(image);
      *image = (ny_native_jit_image_t){0};
      free(defs);
      free(relocs);
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
    image->memory = memory;
    image->size = alloc_size;
    ny_native_jit_image_free(image);
    *image = (ny_native_jit_image_t){0};
    free(defs);
    free(relocs);
    return false;
  }
  image->memory = memory;
  image->size = alloc_size;
  image->entry = memory + defs[entry_index].off;
  free(defs);
  free(relocs);
  return true;
}

bool ny_native_jit_compile(const program_t *prog, const ny_options *opt,
                           ny_native_jit_image_t *image, char *err,
                           size_t err_len) {
  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t *defs = NULL;
  ny_x64_obj_reloc_t *relocs = NULL;
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
  size_t func_cap = NY_NATIVE_NIR_BUNDLE_MAX_FUNCS;
  nyir_func_t *funcs = calloc(func_cap, sizeof(*funcs));
  const char **names = calloc(func_cap, sizeof(*names));
  size_t func_count = 0;
  if (!funcs || !names) {
    free(funcs);
    free(names);
    ny_native_set_err(err, err_len, "native JIT function pool allocation failed");
    return false;
  }
  if (!ny_native_build_nir(prog, opt, &top, funcs, &func_count, names,
                           func_cap, err, err_len) ||
      top.len == 0)
    goto fail_nir;
  /*
   * Consume the shared machine form stream as a hard gate before the host
   * in-memory encoder. The x86-64/AArch64 byte encoders still lower from the
   * verified NYIR that produced this machine form; both forms must agree.
   */
  {
    char mach_err[256] = {0};
    ny_mach_func_t mach = {0};
    if (!ny_mach_lower_nir(&top, &mach, target.caps, mach_err, sizeof(mach_err)) ||
        !ny_mach_verify(&mach, 0, mach_err, sizeof(mach_err))) {
      ny_native_set_err(err, err_len, "native JIT: machine form gate failed for rt_main: %s",
                        mach_err[0] ? mach_err : "verify");
      ny_mach_func_free(&mach);
      goto fail_nir;
    }
    ny_mach_func_free(&mach);
    for (size_t i = 0; i < func_count; ++i) {
      mach = (ny_mach_func_t){0};
      if (!ny_mach_lower_nir(&funcs[i], &mach, target.caps, mach_err, sizeof(mach_err)) ||
          !ny_mach_verify(&mach, 0, mach_err, sizeof(mach_err))) {
        ny_native_set_err(err, err_len,
                          "native JIT: machine form gate failed for function %zu: %s",
                          i, mach_err[0] ? mach_err : "verify");
        ny_mach_func_free(&mach);
    }
  }
  }
  defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    ny_native_set_err(err, err_len,
                      "native JIT definition allocation failed");
    goto fail_nir;
  }

  ny_jit_add_runtime_symbols();
  for (size_t i = 0; i < opt->link_libs.len; ++i)
    (void)ny_jit_load_library(opt->link_libs.data[i]);
  ny_native_visit_program_links_for_options(prog, opt, ny_native_jit_load_link,
                                            NULL);
  if (target.target == NY_NATIVE_TARGET_AARCH64) {
    if (!ny_native_jit_compile_aarch64_bundle(
            &top, funcs, names, func_count, &target, image, err, err_len))
      goto fail_nir;
    for (size_t i = 0; i < func_count; ++i)
      nyir_func_free(&funcs[i]);
    nyir_func_free(&top);
    free(funcs);
    free(names);
    free(defs);
    free(relocs);
    return true;
  }

  /*
   * Primary independence path: machine form → bytes. Fall back to the legacy
   * NYIR object encoder only when machine form encode rejects the shape.
   */
  bool used_mir = false;
  /*
   * Tier-0 stencil: const/local shell; also fold pure helper calls.
   */
  if (func_count == 0 &&
      ny_x64_try_stencil_bundle(&top, &target, &code, defs, &def_count, relocs,
                                &reloc_count, err, err_len)) {
    used_mir = true;
  } else if (func_count > 0 &&
             ny_x64_try_stencil_bundle_calls(&top, funcs, names, func_count,
                                             &target, &code, defs, &def_count,
                                             relocs, &reloc_count, err,
                                             err_len)) {
    used_mir = true;
  }
  if (!used_mir) {
    /*
     * Primary path: machine form per function with per-function NYIR-object
     * fallback.  A single machine-form incompatibility (for example the
     * i64->f64 CONVERT of an uncolored immediate) must not sink the whole
     * image, so encode each function through the mixed bundle and only
     * reject when even the legacy NYIR writer cannot emit it.
     */
    char mach_err[256] = {0};
    if (ny_native_x64_build_mixed_bundle(
            &top, funcs, names, func_count, &target, "rt_main", false,
            &code, defs, &def_count, relocs, &reloc_count, mach_err,
            sizeof(mach_err))) {
      used_mir = true;
    } else {
      ny_obj_free(&code);
      code = (ny_obj_buf_t){0};
      def_count = reloc_count = 0;
      snprintf(mach_err, sizeof(mach_err), "native JIT: mixed bundle failed");
    }
  }
  if (!used_mir &&
      !ny_x64_obj_build_bundle(&top, funcs, names, func_count, &target,
                               "rt_main", false, &code, defs, &def_count,
                               relocs, &reloc_count, err, err_len)) {
    ny_obj_free(&code);
    goto fail_nir;
  }
  /*
   * A top-level mutable binding can survive optimization as an address
   * symbol even when the early global-registration walk did not see the
   * synthetic binding node.  Give unresolved data relocations a private
   * eight-byte cell; call relocations and named externals still fail normally.
   */
  for (size_t i = 0; i < reloc_count; ++i) {
    const char *symbol = relocs[i].symbol;
    if (relocs[i].type != NY_RELOC_PC32 || !symbol || !symbol[0] ||
        symbol[0] == '.' || strncmp(symbol, "ny_fn_", 6) == 0 ||
        strncmp(symbol, "rt_", 3) == 0 ||
        ny_x64_obj_def_index(defs, def_count, symbol) >= 0 ||
        ny_native_jit_resolve_external(symbol))
      continue;
    if (def_count >= NY_NATIVE_MAX_DEFS)
      break;
    while (code.len & 7u)
      if (!ny_obj_u8(&code, 0))
        break;
    size_t off = code.len;
    if (!ny_obj_u64(&code, 0))
      break;
    snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", symbol);
    defs[def_count].off = off;
    defs[def_count].size = 8;
    defs[def_count].is_data = true;
    ++def_count;
  }
  const size_t stub_size = 16;
  size_t used = ny_native_jit_align(code.len, 16);
  size_t alloc_size = ny_native_jit_align(used + reloc_count * stub_size, 4096);

  /*
   * The array pool (.Lnyarr.*) rides at the tail of the code bundle.  List
   * literals are mutable storage, so those bytes must live in a separate
   * region that stays RW after the code region is sealed RX.
   */
  size_t data_start = code.len, data_end = code.len;
  bool have_data = false;
  for (size_t i = 0; i < def_count; ++i) {
    if (!ny_native_jit_def_is_data(defs[i].name))
      continue;
    if (!have_data || defs[i].off < data_start)
      data_start = defs[i].off;
    size_t end = defs[i].off + defs[i].size;
    if (!have_data || end > data_end)
      data_end = end;
    have_data = true;
  }
  size_t text_len = have_data ? data_start : code.len;
  size_t data_len = have_data ? ny_native_jit_align(data_end - data_start, 16) : 0;

  unsigned char *memory = (unsigned char *)ny_native_jit_alloc(alloc_size);
  if (!memory) {
    ny_native_set_err(err, err_len, "native JIT: executable allocation failed");
    ny_obj_free(&code);
    goto fail_nir;
  }
  memset(memory, 0x90, alloc_size);
  memcpy(memory, code.data, text_len);

  unsigned char *data_memory = NULL;
  if (have_data && data_len > 0) {
    data_memory = (unsigned char *)ny_native_jit_alloc_data(data_len);
    if (!data_memory) {
      ny_native_set_err(err, err_len, "native JIT: data pool allocation failed");
      ny_obj_free(&code);
      munmap(memory, alloc_size);
      goto fail_nir;
    }
    memset(data_memory, 0, data_len);
    memcpy(data_memory, code.data + data_start, data_end - data_start);
    image->data_memory = data_memory;
    image->data_size = data_len;
  }
  /*
   * Stencil cache: persist self-contained code (zero relocs) so subsequent
   * runs skip parse + lower + encode entirely.  The cache is an ELF shared
   * object and is therefore intentionally unavailable on Windows.
   */
#ifndef _WIN32
  if (reloc_count == 0 && prog && prog->raw_src && prog->raw_src_len > 0) {
    char *cache_path = ny_jit_stencil_cache_path(
        prog->raw_src, opt ? opt->opt_level : 0,
        opt ? opt->native_backend_raw : NULL,
        opt ? opt->native_tier_raw : NULL);
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
    void *target_ptr;
    if (data_memory && ny_native_jit_def_is_data(relocs[i].symbol)) {
      int di = ny_x64_obj_def_index(defs, def_count, relocs[i].symbol);
      target_ptr = di >= 0 ? data_memory + (defs[di].off - data_start)
                           : ny_native_jit_resolve_external(relocs[i].symbol);
    } else {
      target_ptr = ny_native_jit_symbol(memory, defs, def_count,
                                        relocs[i].symbol);
    }
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
      /*
       * Data address: leaq sym(%rip), reg — patch direct RIP-relative disp.
       */
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
      /*
       * Call address: use stub for external symbols that may be far away.
       */
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
  free(defs);
  free(relocs);
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  nyir_func_free(&top);
  free(funcs);
  free(names);
  return true;

fail_nir:
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  nyir_func_free(&top);
  free(funcs);
  free(names);
  free(defs);
  free(relocs);
  return false;
}
