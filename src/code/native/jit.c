#include "code/native/native.h"
#include "code/native/object/internal.h"
#include "code/jit.h"

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

bool ny_native_jit_compile(const program_t *prog, const ny_options *opt,
                           ny_native_jit_image_t *image, char *err,
                           size_t err_len) {
  if (image)
    *image = (ny_native_jit_image_t){0};
  if (!prog || !opt || !image) {
    ny_native_set_err(err, err_len, "native JIT: missing input");
    return false;
  }
  ny_native_target_info_t target;
  if (!ny_native_target_info_init(&target, opt) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native JIT: in-memory execution is gated to x86-64");
    return false;
  }

  ny_nir_func_t top = {0};
  ny_nir_func_t funcs[64] = {{0}};
  size_t func_count = 0;
  if (!ny_native_build_nir(prog, opt, &top, funcs, &func_count, 64, err,
                           err_len) || top.len == 0)
    goto fail_nir;
  const char *names[64] = {0};
  size_t name_count = 0;
  for (size_t i = 0; i < prog->body.len && name_count < func_count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[name_count++] = stmt->as.fn.name;
  }

  ny_obj_buf_t code = {0};
  ny_x64_obj_symbol_def_t defs[256];
  ny_x64_obj_reloc_t relocs[256];
  size_t def_count = 0, reloc_count = 0;
  if (!ny_x64_obj_build_bundle(&top, funcs, names, func_count, &target,
                               "rt_main", false, &code, defs, &def_count,
                               relocs, &reloc_count, err, err_len)) {
    ny_obj_free(&code);
    goto fail_nir;
  }

  ny_jit_add_runtime_symbols();
  for (size_t i = 0; i < opt->link_libs.len; ++i)
    (void)ny_jit_load_library(opt->link_libs.data[i]);
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
    unsigned char *after = memory + relocs[i].disp_off + 4;
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
    memcpy(memory + relocs[i].disp_off, &disp, sizeof(disp));
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
    ny_nir_func_free(&funcs[i]);
  ny_nir_func_free(&top);
  return true;

fail_nir:
  for (size_t i = 0; i < func_count; ++i)
    ny_nir_func_free(&funcs[i]);
  ny_nir_func_free(&top);
  return false;
}
