/*
 * NYIR JIT: compiles NYIR to native code in-process, manages
 * writable-executable code pages, and provides a native call trampoline.
 */
#include "base/debug.h"
#include "base/trace.h"
#include "base/util.h"
#include "code/backend/core/abi.h"
#include "code/backend/lower/machine.h"
#include "code/backend/lower/jit.h"
#include "code/backend/core/module.h"
#include "code/backend/lower/native.h"
#include "code/backend/mach/object.h"
#include <nyr/nyr.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif

/*
 * Linux perf's JIT map format lets perf attribute anonymous executable pages
 * to generated Nytrix functions. Keep this best-effort and opt-in: profiling
 * metadata must never make compilation fail.
 */
static void nyr_native_jit_write_perf_map(const nyr_x64_obj_symbol_def_t *defs,
                                          size_t def_count,
                                          const unsigned char *memory) {
#ifndef _WIN32
  const char *enabled = getenv("NYTRIX_JIT_PERF_MAP");
  if (!enabled || !*enabled || strcmp(enabled, "0") == 0 || !defs || !memory)
    return;
  char path[4096];
  snprintf(path, sizeof(path), "%s/perf-%ld.map", ny_get_temp_dir(),
           (long)getpid());
  FILE *map = fopen(path, "a");
  if (!map)
    return;
  for (size_t i = 0; i < def_count; ++i) {
    if (defs[i].is_data || defs[i].size == 0 || !defs[i].name[0])
      continue;
    fprintf(map, "%lx %lx %s\n",
            (unsigned long)(uintptr_t)(memory + defs[i].off),
            (unsigned long)defs[i].size, defs[i].name);
  }
  fclose(map);
#else
  (void)defs;
  (void)def_count;
  (void)memory;
#endif
}

/*
 * Save the exact relocated JIT image for offline disassembly. Perf's text map
 * resolves samples to generated symbols, but perf annotate needs an ELF
 * object. This opt-in raw image plus sidecar preserves the bytes and address
 * ranges for objdump without adding work to ordinary compilation.
 */
static void nyr_native_jit_dump_code_image(const nyr_x64_obj_symbol_def_t *defs,
                                           size_t def_count,
                                           const unsigned char *memory,
                                           size_t image_size) {
#ifndef _WIN32
  const char *enabled = getenv("NYTRIX_JIT_CODE_DUMP");
  if (!enabled || !*enabled || strcmp(enabled, "0") == 0 || !defs || !memory ||
      !image_size)
    return;

  char path[4096];
  char map_path[4096];
  unsigned long pid = (unsigned long)getpid();
  unsigned long base = (unsigned long)(uintptr_t)memory;
  int path_len = snprintf(path, sizeof(path), "%s/ny-jit-%lu-%lx.bin",
                          ny_get_temp_dir(), pid, base);
  if (path_len <= 0 || (size_t)path_len >= sizeof(path))
    return;
  int map_len = snprintf(map_path, sizeof(map_path), "%s.map", path);
  if (map_len <= 0 || (size_t)map_len >= sizeof(map_path))
    return;

  FILE *image = fopen(path, "wb");
  if (!image)
    return;
  bool image_ok = fwrite(memory, 1, image_size, image) == image_size;
  if (fclose(image) != 0)
    image_ok = false;
  if (!image_ok) {
    remove(path);
    return;
  }

  FILE *map = fopen(map_path, "w");
  if (!map) {
    remove(path);
    return;
  }
  fprintf(map, "# raw relocated JIT image: %s\n# base=%lx size=%zu\n", path,
          base, image_size);
  for (size_t i = 0; i < def_count; ++i) {
    if (defs[i].is_data || defs[i].size == 0 || !defs[i].name[0])
      continue;
    fprintf(map, "%lx %lx %zx %s\n", base + defs[i].off,
            (unsigned long)defs[i].size, defs[i].off, defs[i].name);
  }
  fclose(map);
#else
  (void)defs;
  (void)def_count;
  (void)memory;
  (void)image_size;
#endif
}

static void *nyr_native_jit_alloc(size_t size) {
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__APPLE__) && defined(__aarch64__)
  flags |= MAP_JIT;
#endif
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
#if defined(__APPLE__) && defined(__aarch64__)
  if (p != MAP_FAILED)
    pthread_jit_write_protect_np(0);
#endif
  return p == MAP_FAILED ? NULL : p;
#endif
}

#ifndef NYR_NO_EVAL
static void *nyr_native_jit_alloc_data(size_t size) {
#ifdef _WIN32
  void *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    p = NULL;
#endif
  return p;
}
#endif

static void nyr_native_jit_release(void *memory, size_t size) {
  if (!memory || !size)
    return;
#ifdef _WIN32
  (void)size;
  VirtualFree(memory, 0, MEM_RELEASE);
#else
  munmap(memory, size);
#endif
}

/*
 * A PC32 relocation materializes a symbol address with `leaq sym(%rip), reg`.
 * When the target is farther than 2 GiB away, a function can be reached
 * through a nearby movabs/jmp thunk, but a data symbol must keep its real
 * address: redirecting it to a thunk would silently hand out the stub address
 * instead of the datum.  Classify the resolved target by the permissions of
 * the mapping that contains it.
 */
static bool nyr_native_jit_target_is_code(const void *target) {
  uintptr_t p = (uintptr_t)target;
  if (p < 0x1000)
    return false;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi = {0};
  if (!VirtualQuery(target, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
    return false;
  DWORD protect = mbi.Protect & 0xff;
  return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
         protect == PAGE_EXECUTE_READWRITE ||
         protect == PAGE_EXECUTE_WRITECOPY;
#elif defined(__APPLE__)
  mach_vm_address_t region = (mach_vm_address_t)p;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(
      mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
      (vm_region_info_t)&info, &count, &object_name);
  if (object_name != MACH_PORT_NULL)
    mach_port_deallocate(mach_task_self(), object_name);
  if (kr != KERN_SUCCESS || region_size == 0 || p < region ||
      p - region >= region_size)
    return false;
  return (info.protection & VM_PROT_EXECUTE) != 0;
#else
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return false;
  char line[512];
  bool code = false;
  while (fgets(line, sizeof(line), maps)) {
    unsigned long start = 0, end = 0;
    char perms[8] = {0};
    if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
      continue;
    if (p >= (uintptr_t)start && p < (uintptr_t)end) {
      code = perms[2] == 'x';
      break;
    }
  }
  fclose(maps);
  return code;
#endif
}

static bool nyr_native_jit_def_is_data(const char *name) {
  return name &&
         (strncmp(name, ".Lnyarr.", 8) == 0 || nyr_globaltab_has(name) ||
          /*
           * String literals are emitted by the native object
           * builder into the same trailing data pool as arrays and
           * globals.  Keep their relocations in that RW mapping;
           * resolving them against the executable text mapping
           * loses the literal's actual address (and commonly turns
           * `"err"` into `"e"`).
           */
          nyr_strtab_get(name, NULL) != NULL);
}

static bool nyr_native_jit_seal(void *memory, size_t size) {
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

void nyr_jit_image_free(nyr_jit_image_t *image) {
  if (!image)
    return;
  uintptr_t base = (uintptr_t)image->memory;
  uintptr_t data = (uintptr_t)image->data_memory;
  bool shared_mapping = image->memory && data >= base &&
                        data - base < image->size;
  if (image->data_memory && image->data_size && !shared_mapping) {
    nyr_native_jit_release(image->data_memory, image->data_size);
  }
  if (image->memory)
    nyr_native_jit_release(image->memory, image->size);
#if defined(__APPLE__) && defined(__aarch64__)
  pthread_jit_write_protect_np(1);
#endif
  *image = (nyr_jit_image_t){0};
}

static void *nyr_native_jit_symbol(unsigned char *base,
                                   const nyr_x64_obj_symbol_def_t *defs,
                                   size_t def_count, int index,
                                   const char *name,
                                   const nyr_runtime_services_t *services) {
  if (index >= 0) {
    return base + defs[index].off;
  }
  /*
   * Re-export alias resolution: a caller may reference a function by its
   * imported module alias (`str.utf8_slice`) while the definition is
   * registered under its canonical module name
   * (`core.str.utf8_slice`). Resolve suffix aliases only when unique.
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
      bool matched_mod_alias = false;
      const char *last_dot = strrchr(cand, '.');
      if (last_dot && last_dot != cand) {
        const char *seg_start = last_dot;
        while (seg_start > cand && seg_start[-1] != '.')
          --seg_start;
        size_t seg_len = (size_t)(last_dot - seg_start);
        if (seg_len >= 4 && strncmp(seg_start + seg_len - 4, "_mod", 4) == 0) {
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
              matched_mod_alias = true;
            }
          }
        }
      }
      /*
       * Module imports commonly expose a short alias (`str.foo`) for a
       * canonical stdlib function (`core.str.foo`). Resolve that alias
       * only when it names a unique definition in the whole JIT bundle.
       */
      size_t cand_len = strlen(cand);
      size_t fully_len = strlen(fully);
      if (!matched_mod_alias && cand_len > fully_len &&
          cand[cand_len - fully_len - 1] == '.' &&
          strcmp(cand + cand_len - fully_len, fully) == 0) {
        match = (int)z;
        matches++;
      }
      /*
       * Mirror of the above: a caller may carry a doubly module-qualified
       * spelling (`core.str.str.strip`) while the definition pool indexes
       * the short impl-owner spelling (`str.strip`). Resolve when the
       * wanted name ends with '.' + candidate, only on a unique match, so
       * genuinely ambiguous or missing symbols still fail closed.
       */
      if (!matched_mod_alias && fully_len > cand_len &&
          fully[fully_len - cand_len - 1] == '.' &&
          strcmp(fully + fully_len - cand_len, cand) == 0) {
        match = (int)z;
        matches++;
      }
    }
    if (matches == 1 && match >= 0)
      return base + defs[match].off;
  }
  return services && services->resolve_symbol
             ? services->resolve_symbol(services->userdata, name)
             : NULL;
}

#ifndef NYR_NO_EVAL
static bool nyr_native_jit_compile_aarch64_bundle(
    const nyr_func_t *top, const nyr_func_t *funcs, const char *const *names,
    size_t func_count, const ny_native_target_info_t *target,
    const nyr_runtime_services_t *services, nyr_jit_image_t *image, char *err,
    size_t err_len) {
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  nyr_x64_obj_reloc_t *relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    defs = NULL;
    relocs = NULL;
    nyr_set_error(err, err_len,
                  "native AArch64 JIT definition allocation failed");
    return false;
  }
  if (!nyr_a64_obj_build_bundle(top, funcs, names, func_count, target,
                                "rt_main", false, &code, defs, &def_count,
                                relocs, &reloc_count, err, err_len)) {
    nyr_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  const size_t stub_size = 16;
  size_t data_start = code.len, data_end = code.len;
  bool have_data = false;
  for (size_t i = 0; i < def_count; ++i) {
    if (!memchr(defs[i].name, 0, sizeof(defs[i].name)) ||
        !nyr_jit_range(code.len, defs[i].off, defs[i].size)) {
      nyr_set_error(err, err_len, "native AArch64 JIT: invalid symbol range");
      nyr_obj_free(&code);
      free(defs);
      free(relocs);
      return false;
    }
    if (!nyr_native_jit_def_is_data(defs[i].name))
      continue;
    if (!have_data || defs[i].off < data_start)
      data_start = defs[i].off;
    size_t end = defs[i].off + defs[i].size;
    if (!have_data || end > data_end)
      data_end = end;
    have_data = true;
  }
  size_t text_len = have_data ? data_start : code.len;
  nyr_jit_layout_t layout;
  if (!nyr_jit_layout_init(text_len, reloc_count, stub_size,
                           have_data ? data_end - data_start : 0, false,
                           &layout)) {
    nyr_set_error(err, err_len, "native AArch64 JIT: image size overflow");
    nyr_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!memchr(relocs[i].symbol, 0, sizeof(relocs[i].symbol)) ||
        relocs[i].type != NY_RELOC_AARCH64_CALL26 ||
        !nyr_jit_range(text_len, relocs[i].disp_off, 4) ||
        (relocs[i].disp_off & 3)) {
      nyr_set_error(err, err_len, "native AArch64 JIT: invalid relocation");
      nyr_obj_free(&code);
      free(defs);
      free(relocs);
      return false;
    }
  }
  size_t used = layout.stub_offset;
  size_t data_len = layout.data_size;
  size_t alloc_size = layout.code_size;
  unsigned char *memory = (unsigned char *)nyr_native_jit_alloc(alloc_size);
  if (!memory) {
    nyr_set_error(err, err_len,
                  "native AArch64 JIT: executable allocation failed");
    nyr_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  memset(memory, 0, alloc_size);
  memcpy(memory, code.data, text_len);

  unsigned char *data_memory = NULL;
  if (data_len > 0) {
    data_memory = (unsigned char *)nyr_native_jit_alloc_data(data_len);
    if (!data_memory) {
      nyr_set_error(err, err_len,
                    "native AArch64 JIT: data pool allocation failed");
      nyr_obj_free(&code);
      image->memory = memory;
      image->size = alloc_size;
      nyr_jit_image_free(image);
      *image = (nyr_jit_image_t){0};
      free(defs);
      free(relocs);
      return false;
    }
    memset(data_memory, 0, data_len);
    memcpy(data_memory, code.data + data_start, data_end - data_start);
    if (services && services->register_mapping)
      services->register_mapping(services->userdata, (uintptr_t)data_memory,
                                 data_len);
    image->data_memory = data_memory;
    image->data_size = data_len;
  }
  nyr_obj_free(&code);
  for (size_t i = 0; i < reloc_count; ++i) {
    void *resolved;
    int index = nyr_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    if (data_memory && nyr_native_jit_def_is_data(relocs[i].symbol)) {
      resolved = index >= 0 && defs[index].off >= data_start &&
                         defs[index].off < data_end
                     ? data_memory + (defs[index].off - data_start)
                     : index >= 0 ? NULL
                     : (services && services->resolve_symbol
                            ? services->resolve_symbol(services->userdata,
                                                       relocs[i].symbol)
                            : NULL);
    } else {
      resolved = nyr_native_jit_symbol(memory, defs, def_count, index,
                                       relocs[i].symbol, services);
    }
    if (!resolved) {
      nyr_set_error(err, err_len, "native AArch64 JIT: unresolved symbol '%s'",
                    relocs[i].symbol);
      image->memory = memory;
      image->size = alloc_size;
      nyr_jit_image_free(image);
      *image = (nyr_jit_image_t){0};
      free(defs);
      free(relocs);
      return false;
    }
    unsigned char *branch_target = (unsigned char *)resolved;
    unsigned char *patch = memory + relocs[i].disp_off;
    uint32_t imm;
    if (index < 0 &&
        !nyr_jit_call26((uintptr_t)resolved, (uintptr_t)patch, &imm)) {
      size_t offset;
      if (!nyr_jit_take_stub(&layout, &used, &offset)) {
        nyr_set_error(err, err_len, "native AArch64 JIT: stub capacity exceeded");
        image->memory = memory;
        image->size = alloc_size;
        nyr_jit_image_free(image);
        *image = (nyr_jit_image_t){0};
        free(defs);
        free(relocs);
        return false;
      }
      unsigned char *stub = memory + offset;
      const uint32_t load_x16 = 0x58000050u;
      const uint32_t branch_x16 = 0xd61f0200u;
      uint64_t absolute = (uint64_t)(uintptr_t)resolved;
      memcpy(stub, &load_x16, sizeof(load_x16));
      memcpy(stub + 4, &branch_x16, sizeof(branch_x16));
      memcpy(stub + 8, &absolute, sizeof(absolute));
      branch_target = stub;
    }
    if (!nyr_jit_call26((uintptr_t)branch_target, (uintptr_t)patch, &imm)) {
      nyr_set_error(
          err, err_len,
          "native AArch64 JIT: CALL26 relocation for '%s' is out of range",
          relocs[i].symbol);
      image->memory = memory;
      image->size = alloc_size;
      nyr_jit_image_free(image);
      *image = (nyr_jit_image_t){0};
      free(defs);
      free(relocs);
      return false;
    }
    uint32_t insn = 0;
    memcpy(&insn, patch, sizeof(insn));
    insn = (insn & 0xfc000000u) | imm;
    memcpy(patch, &insn, sizeof(insn));
  }
  int entry_index = nyr_x64_obj_def_index(
      defs, def_count, target->symbol_prefix[0] ? "_rt_main" : "rt_main");
  if (entry_index < 0 || defs[entry_index].off >= text_len ||
      !nyr_native_jit_seal(memory, alloc_size)) {
    nyr_set_error(err, err_len,
                  "native AArch64 JIT: executable finalization failed");
    image->memory = memory;
    image->size = alloc_size;
    nyr_jit_image_free(image);
    *image = (nyr_jit_image_t){0};
    free(defs);
    free(relocs);
    return false;
  }
  image->memory = memory;
  image->size = alloc_size;
  image->entry = memory + defs[entry_index].off;
  nyr_native_jit_write_perf_map(defs, def_count, memory);
  nyr_native_jit_dump_code_image(defs, def_count, memory, alloc_size);
  free(defs);
  free(relocs);
  return true;
}
#endif

/*
 * Compiler-keyed, pointer-free cache. Atomic publication prevents readers
 * from accepting a partial image when fixture processes finish together.
 */
#if !defined(_WIN32) && defined(__x86_64__)
typedef struct {
  uint64_t magic, code_len, def_count, reloc_count, checksum;
} nyr_jit_cache_header_t;

static uint64_t nyr_native_jit_cache_checksum(
    const nyr_obj_buf_t *code, const nyr_x64_obj_symbol_def_t *defs,
    size_t def_count, const nyr_x64_obj_reloc_t *relocs, size_t reloc_count) {
  uint64_t h = ny_hash64(code->data, code->len);
  h ^= ny_hash64(defs, def_count * sizeof(*defs));
  h ^= ny_hash64(relocs, reloc_count * sizeof(*relocs));
  return h;
}

#endif

#ifndef NYR_NO_EVAL
static void nyr_native_jit_cache_save(const char *path,
                                      const nyr_obj_buf_t *code,
                                      const nyr_x64_obj_symbol_def_t *defs,
                                      size_t def_count,
                                      const nyr_x64_obj_reloc_t *relocs,
                                      size_t reloc_count) {
#if !defined(_WIN32) && defined(__x86_64__)
  if (!path || !*path || !code || !code->len)
    return;
  char object_path[4096], tmp[4096], dir[4096];
  int n = snprintf(object_path, sizeof(object_path), "%s.obj", path);
  if (n <= 0 || (size_t)n >= sizeof(object_path))
    return;
  snprintf(dir, sizeof(dir), "%s", object_path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    ny_ensure_dir_recursive(dir);
  }
  n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.XXXXXX", object_path,
               (long)getpid());
  if (n <= 0 || (size_t)n >= sizeof(tmp))
    return;
  int fd = mkstemp(tmp);
  if (fd < 0)
    return;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmp);
    return;
  }
  nyr_jit_cache_header_t header = {
      UINT64_C(0x4e594a49544f4231), code->len, def_count, reloc_count,
      nyr_native_jit_cache_checksum(code, defs, def_count, relocs,
                                    reloc_count)};
  bool ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
            fwrite(code->data, 1, code->len, f) == code->len &&
            fwrite(defs, sizeof(*defs), def_count, f) == def_count &&
            fwrite(relocs, sizeof(*relocs), reloc_count, f) == reloc_count;
  if (fclose(f) != 0)
    ok = false;
  if (!ok || !ny_cache_publish_file(tmp, object_path))
    unlink(tmp);
#else
  (void)path;
  (void)code;
  (void)defs;
  (void)def_count;
  (void)relocs;
  (void)reloc_count;
#endif
}
#endif

/*
 * The same relocation path handles newly encoded and cached code. The
 * serialized image contains symbols and bytes, never process addresses.
 */
static bool nyr_native_jit_materialize_x64(
    nyr_obj_buf_t code, nyr_x64_obj_symbol_def_t *defs, size_t def_count,
    nyr_x64_obj_reloc_t *relocs, size_t reloc_count,
    const nyr_runtime_services_t *runtime_services, nyr_jit_image_t *image,
    char *err, size_t err_len) {
  const size_t stub_size = 16;
  size_t data_start = code.len, data_end = code.len;
  bool have_data = false;
  for (size_t i = 0; i < def_count; ++i) {
    if (!memchr(defs[i].name, 0, sizeof(defs[i].name)) ||
        !nyr_jit_range(code.len, defs[i].off, defs[i].size)) {
      nyr_set_error(err, err_len, "native JIT: invalid symbol range");
      goto fail_nir;
    }
    if (!defs[i].is_data)
      continue;
    if (!have_data || defs[i].off < data_start)
      data_start = defs[i].off;
    size_t end = defs[i].off + defs[i].size;
    if (!have_data || end > data_end)
      data_end = end;
    have_data = true;
  }
  size_t text_len = have_data ? data_start : code.len;
  nyr_jit_layout_t layout;
  if (!nyr_jit_layout_init(text_len, reloc_count, stub_size,
                           have_data ? data_end - data_start : 0, true,
                           &layout)) {
    nyr_set_error(err, err_len, "native JIT: image size overflow/PC32 range");
    goto fail_nir;
  }
  for (size_t i = 0; i < reloc_count; ++i) {
    if (!memchr(relocs[i].symbol, 0, sizeof(relocs[i].symbol)) ||
        (relocs[i].type != NY_RELOC_PC32 && relocs[i].type != NY_RELOC_PLT32) ||
        !nyr_jit_range(text_len, relocs[i].disp_off, 4)) {
      nyr_set_error(err, err_len, "native JIT: invalid relocation");
      goto fail_nir;
    }
  }
  size_t used = layout.stub_offset;
  size_t alloc_size = layout.code_size;
  size_t data_len = layout.data_size;
  size_t mapping_size = layout.mapping_size;
  unsigned char *memory = (unsigned char *)nyr_native_jit_alloc(mapping_size);
  if (!memory) {
    nyr_set_error(err, err_len, "native JIT: executable allocation failed");
    nyr_obj_free(&code);
    goto fail_nir;
  }
  memset(memory, 0x90, alloc_size);
  memcpy(memory, code.data, text_len);
  const char *jit_dump_path = getenv("NY_DUMP_JIT");
  if (jit_dump_path && jit_dump_path[0]) {
    FILE *jit_dump = fopen(jit_dump_path, "wb");
    if (jit_dump) {
      fwrite(memory, 1, text_len, jit_dump);
      fclose(jit_dump);
      fprintf(stderr, "[jit-dump] %zu text bytes -> %s\n", (size_t)text_len,
              jit_dump_path);
    }
  }

  unsigned char *data_memory = NULL;
  if (have_data && data_len > 0) {
    data_memory = memory + alloc_size;
    memset(data_memory, 0, data_len);
    memcpy(data_memory, code.data + data_start, data_end - data_start);
    if (runtime_services && runtime_services->register_mapping)
      runtime_services->register_mapping(runtime_services->userdata,
                                         (uintptr_t)data_memory, data_len);
    image->data_memory = data_memory;
    image->data_size = data_len;
  }
  nyr_obj_free(&code);

  /*
   * Large images repeat a few thousand symbols across tens of thousands of
   * relocations. Resolve each (symbol, call-ness) once and share one far
   * stub per target instead of a linear definition search, a runtime lookup
   * and a /proc/self/maps scan per relocation.
   */
  typedef struct {
    const char *symbol;
    bool plt;
    void *target;
    bool have_stub;
    size_t stub;
  } nyr_jit_reloc_memo_t;
  size_t memo_cap = 64;
  while (memo_cap < reloc_count * 2 && memo_cap < ((size_t)1 << 30))
    memo_cap <<= 1;
  nyr_jit_reloc_memo_t *memo = calloc(memo_cap, sizeof(*memo));
  for (size_t i = 0; i < reloc_count; ++i) {
    void *target_ptr = NULL;
    bool plt = relocs[i].type == NY_RELOC_PLT32;
    nyr_jit_reloc_memo_t *slot = NULL;
    if (memo && relocs[i].symbol[0]) {
      uint64_t h = UINT64_C(1469598103934665603) ^ (uint64_t)plt;
      for (const char *c = relocs[i].symbol; *c; ++c)
        h = (h ^ (unsigned char)*c) * UINT64_C(1099511628211);
      for (size_t k = (size_t)h & (memo_cap - 1);;
           k = (k + 1) & (memo_cap - 1)) {
        if (!memo[k].symbol ||
            (memo[k].plt == plt && strcmp(memo[k].symbol, relocs[i].symbol) == 0)) {
          slot = &memo[k];
          break;
        }
      }
      if (slot->symbol)
        target_ptr = slot->target;
    }
    if (!target_ptr) {
    int def_index = nyr_x64_obj_def_index(defs, def_count, relocs[i].symbol);
    /*
     * A call relocation never targets a data cell. A Ny global named like a
     * C function (`mut dup = ""` beside an extern bound `as "dup"`) shares
     * the symbol; resolve the call to the external function, not the cell.
     */
    if (def_index >= 0 && defs[def_index].is_data &&
        relocs[i].type == NY_RELOC_PLT32) {
      target_ptr = runtime_services && runtime_services->resolve_symbol
                       ? runtime_services->resolve_symbol(runtime_services->userdata,
                                                          relocs[i].symbol)
                       : NULL;
    } else if (data_memory && def_index >= 0 && defs[def_index].is_data) {
      target_ptr = data_memory + (defs[def_index].off - data_start);
    } else {
      target_ptr = nyr_native_jit_symbol(memory, defs, def_count, def_index,
                                         relocs[i].symbol, runtime_services);
    }
    if (slot && target_ptr) {
      slot->symbol = relocs[i].symbol;
      slot->plt = plt;
      slot->target = target_ptr;
    }
    }
    if (!target_ptr) {
      free(memo);
      nyr_set_error(err, err_len, "native JIT: unresolved symbol '%s'",
                    relocs[i].symbol);
      image->memory = memory;
      image->size = mapping_size;
      nyr_jit_image_free(image);
      goto fail_nir;
    }
    unsigned char *patch_at = memory + relocs[i].disp_off;
    uintptr_t after = (uintptr_t)(patch_at + 4);
    int32_t disp;
    if (!nyr_jit_rel32((uintptr_t)target_ptr, after, &disp) && slot &&
        slot->have_stub &&
        nyr_jit_rel32((uintptr_t)(memory + slot->stub), after, &disp)) {
      memcpy(patch_at, &disp, sizeof(disp));
      continue;
    }
    if (!nyr_jit_rel32((uintptr_t)target_ptr, after, &disp)) {
      if (relocs[i].type == NY_RELOC_PC32 &&
          !nyr_native_jit_target_is_code(target_ptr)) {
        nyr_set_error(err, err_len,
                      "native JIT: PC32 data relocation '%s' out of range",
                      relocs[i].symbol);
        free(memo);
        image->memory = memory;
        image->size = mapping_size;
        nyr_jit_image_free(image);
        goto fail_nir;
      }
      size_t offset;
      if (!nyr_jit_take_stub(&layout, &used, &offset) ||
          !nyr_jit_rel32((uintptr_t)(memory + offset), after, &disp)) {
        nyr_set_error(err, err_len,
                      "native JIT: relocation '%s' out of range",
                      relocs[i].symbol);
        free(memo);
        image->memory = memory;
        image->size = mapping_size;
        nyr_jit_image_free(image);
        goto fail_nir;
      }
      unsigned char *stub = memory + offset;
      stub[0] = 0x48;
      stub[1] = 0xb8;
      uint64_t absolute = (uint64_t)(uintptr_t)target_ptr;
      memcpy(stub + 2, &absolute, sizeof(absolute));
      stub[10] = 0xff;
      stub[11] = 0xe0;
      if (slot) {
        slot->have_stub = true;
        slot->stub = offset;
      }
    }
    memcpy(patch_at, &disp, sizeof(disp));
  }
  free(memo);

  int entry_index = nyr_x64_obj_def_index(defs, def_count, "rt_main");
  if (entry_index < 0 || defs[entry_index].off >= text_len ||
      !nyr_native_jit_seal(memory, alloc_size)) {
    nyr_set_error(err, err_len, "native JIT: executable finalization failed");
    image->memory = memory;
    image->size = mapping_size;
    nyr_jit_image_free(image);
    goto fail_nir;
  }
  image->memory = memory;
  image->size = mapping_size;
  image->entry = memory + defs[entry_index].off;
  nyr_native_jit_write_perf_map(defs, def_count, memory);
  nyr_native_jit_dump_code_image(defs, def_count, memory, alloc_size);
  free(defs);
  free(relocs);
  return true;

fail_nir:
  nyr_obj_free(&code);
  free(defs);
  free(relocs);
  *image = (nyr_jit_image_t){0};
  return false;
}

bool nyr_jit_cache_load(const char *path,
                        const nyr_runtime_services_t *runtime_services,
                        nyr_jit_image_t *image, char *err, size_t err_len) {
#if !defined(_WIN32) && defined(__x86_64__)
  if (!path || !image)
    return false;
  *image = (nyr_jit_image_t){0};
  char object_path[4096];
  int n = snprintf(object_path, sizeof(object_path), "%s.obj", path);
  if (n <= 0 || (size_t)n >= sizeof(object_path))
    return false;
  FILE *f = fopen(object_path, "rb");
  if (!f)
    return false;
  nyr_jit_cache_header_t header;
  struct stat st;
  bool valid = fstat(fileno(f), &st) == 0 &&
               fread(&header, sizeof(header), 1, f) == 1 &&
               header.magic == UINT64_C(0x4e594a49544f4231) &&
               header.code_len > 0 && header.code_len <= 64 * 1024 * 1024 &&
               header.def_count > 0 && header.def_count <= NY_NATIVE_MAX_DEFS &&
               header.reloc_count <= NY_X64_OBJ_MAX_RELOCS;
  if (!valid || (uint64_t)st.st_size !=
                    sizeof(header) + header.code_len +
                        header.def_count * sizeof(nyr_x64_obj_symbol_def_t) +
                        header.reloc_count * sizeof(nyr_x64_obj_reloc_t)) {
    fclose(f);
    return false;
  }
  nyr_obj_buf_t code = {0};
  code.len = code.cap = (size_t)header.code_len;
  code.data = malloc(code.len);
  size_t def_count = (size_t)header.def_count;
  size_t reloc_count = (size_t)header.reloc_count;
  nyr_x64_obj_symbol_def_t *defs = calloc(def_count, sizeof(*defs));
  nyr_x64_obj_reloc_t *relocs =
      calloc(reloc_count ? reloc_count : 1, sizeof(*relocs));
  valid = code.data && defs && relocs &&
          fread(code.data, 1, code.len, f) == code.len &&
          fread(defs, sizeof(*defs), def_count, f) == def_count &&
          fread(relocs, sizeof(*relocs), reloc_count, f) == reloc_count;
  fclose(f);
  if (valid)
    valid = nyr_native_jit_cache_checksum(&code, defs, def_count, relocs,
                                          reloc_count) == header.checksum;
  for (size_t i = 0; valid && i < def_count; ++i)
    valid = memchr(defs[i].name, '\0', sizeof(defs[i].name)) &&
            defs[i].off < code.len && defs[i].size <= code.len - defs[i].off;
  for (size_t i = 0; valid && i < reloc_count; ++i)
    valid =
        memchr(relocs[i].symbol, '\0', sizeof(relocs[i].symbol)) &&
        code.len >= 4 && relocs[i].disp_off <= code.len - 4 &&
        (relocs[i].type == NY_RELOC_PC32 || relocs[i].type == NY_RELOC_PLT32);
  if (!valid) {
    nyr_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  return nyr_native_jit_materialize_x64(code, defs, def_count, relocs,
                                        reloc_count, runtime_services, image,
                                        err, err_len);
#else
  (void)path;
  (void)runtime_services;
  (void)image;
  (void)err;
  (void)err_len;
  return false;
#endif
}

static nyr_abi_t nyr_native_jit_abi(nyr_abi_t abi, const char *triple) {
  /*
   * A live JIT without a target override calls the host C runtime.
   */
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
  if (abi == NY_NATIVE_ABI_AUTO && (!triple || !*triple))
    return NY_NATIVE_ABI_WIN64;
#else
  (void)triple;
#endif
  return abi;
}

#ifndef NYR_NO_EVAL
bool nyr_jit_compile(nyr_backend_module_t *module, const ny_options *opt,
                     const char *stencil_cache_path, nyr_jit_image_t *image,
                     char *err, size_t err_len) {
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t *defs = NULL;
  nyr_x64_obj_reloc_t *relocs = NULL;
  nyr_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyr_set_pass_controls(opt ? opt->nyr_disable_pass : NULL,
                        opt ? opt->nyr_stop_after : NULL);
  nyr_set_verify_each_pass(opt && opt->nyr_ir_verify);
  nyr_set_rollback_on_verify_fail(opt && opt->nyr_rollback_on_verify_fail);
  nyr_set_global_inst_cap(opt ? opt->nyr_global_inst_cap : 0);
  nyr_set_pass_growth_factor(opt ? opt->nyr_pass_growth_factor : 0.0);
  nyr_set_track_value(opt ? opt->nyr_track_value : -1);
  ny_dbg_init_from_verbosity(opt ? opt->verbosity : 0);
  nyr_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  if (image)
    *image = (nyr_jit_image_t){0};
  if (!module || !module->entry || !opt || !image) {
    nyr_set_error(err, err_len, "native JIT: missing input");
    return false;
  }
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt->native_backend,
      nyr_native_jit_abi(opt->native_abi, opt->host_triple), opt->host_triple,
      opt->opt_size);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &target_request)) {
    nyr_set_error(err, err_len, "native JIT: backend is disabled");
    return false;
  }
  if ((target.caps & (unsigned)NY_NATIVE_CAP_LIVE_JIT) == 0) {
    nyr_set_error(err, err_len,
                  "native JIT is not available for target '%s'; this "
                  "target supports NYIR text/VM paths only",
                  target.target_name ? target.target_name : "unknown");
    return false;
  }
#if defined(__aarch64__) || defined(_M_ARM64)
  if (target.target != NY_NATIVE_TARGET_AARCH64) {
    nyr_set_error(err, err_len,
                  "native JIT: host AArch64 requires the AArch64 backend");
    return false;
  }
#elif defined(__x86_64__) || defined(_M_X64)
  if (target.target != NY_NATIVE_TARGET_X86_64) {
    nyr_set_error(err, err_len,
                  "native JIT: host x86-64 requires the x86-64 backend");
    return false;
  }
#else
  nyr_set_error(err, err_len,
                "native JIT: this host architecture has no in-memory encoder");
  return false;
#endif

  nyr_func_t *top = module->entry;
  nyr_func_t *funcs = module->functions;
  const char **names = (const char **)module->function_names;
  size_t func_count = module->function_count;
  if ((func_count && (!funcs || !names)) || top->len == 0) {
    nyr_set_error(err, err_len, "native JIT: incomplete NYIR module");
    return false;
  }
  if (opt->dump_funcs) {
    /*
     * Compact compiled-function listing: name, NYIR size, per-fn code bytes
     * once the machine form exists. Names are the qualified module views.
     */
    fprintf(stderr, "compiled functions (%zu):\n", func_count);
    for (size_t fi = 0; fi < func_count; ++fi)
      fprintf(stderr, "  %s  %zu insts\n", names[fi] ? names[fi] : "unknown_fn",
              funcs[fi].len);
    fprintf(stderr, "  rt_main  %zu insts\n", top->len);
  }
  if (opt->callgraph) {
    char callgraph_err[512] = {0};
    ny_dbg_enter_context("callgraph from compiled NYIR");
    const nyr_backend_module_t module_view = {
        .entry = top,
        .functions = funcs,
        .function_names = names,
        .function_count = func_count,
        .runtime_services = module->runtime_services,
    };
    const nyr_callgraph_options_t callgraph_options = {opt->callgraph_json,
                                                       opt->callgraph_depth};
    bool callgraph_ok = nyr_write_callgraph_for_ir(
        &module_view, &callgraph_options, callgraph_err, sizeof(callgraph_err));
    ny_dbg_exit_context("callgraph from compiled NYIR");
    if (!callgraph_ok) {
      nyr_set_error(err, err_len, "native JIT callgraph failed: %s",
                    callgraph_err[0] ? callgraph_err : "unknown error");
      goto fail_nir;
    }
  }
  /*
   * Consume the shared machine form stream as a hard gate before the host
   * in-memory encoder. The x86-64/AArch64 byte encoders still lower from the
   * verified NYIR that produced this machine form; both forms must agree.
   */
  {
    char mach_err[256] = {0};
    nyr_mach_func_t mach = {0};
    if (!nyr_mach_lower_nir(top, &mach, target.caps, mach_err,
                            sizeof(mach_err)) ||
        !nyr_mach_verify(&mach, 0, mach_err, sizeof(mach_err))) {
      if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL")) {
        ny_trace_line("MACHINE_FAIL", "machine form rejected for rt_main: %s",
                      mach_err[0] ? mach_err : "unknown error");
        nyr_mach_dump(&mach, stderr);
      }
      nyr_set_error(err, err_len,
                    "native JIT: machine form gate failed for rt_main: %s",
                    mach_err[0] ? mach_err : "verify");
      nyr_mach_func_free(&mach);
      goto fail_nir;
    }
    if (getenv("NY_DUMP_MACH")) {
      fprintf(stderr, "native JIT machine form for rt_main:\n");
      nyr_mach_dump(&mach, stderr);
    }
    nyr_mach_func_free(&mach);
    for (size_t i = 0; i < func_count; ++i) {
      mach = (nyr_mach_func_t){0};
      ny_dbg_lower_fn(names && names[i] ? names[i] : "unknown_fn");
      ny_dbg_scope_stdlib(names && names[i] && strchr(names[i], '.') != NULL);
      bool lowered = nyr_mach_lower_nir(&funcs[i], &mach, target.caps,
                                        mach_err, sizeof(mach_err));
      /*
       * An unsupported shape may fall back to the NYIR object encoder, but a
       * machine verifier rejection after successful lowering is a compiler
       * bug: fail closed instead of shipping a form the verifier rejected.
       */
      if (lowered && !nyr_mach_verify(&mach, 0, mach_err, sizeof(mach_err))) {
        if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL")) {
          ny_trace_line("MACHINE_FAIL",
                        "machine form verification failed for function "
                        "%zu: %s",
                        i, mach_err[0] ? mach_err : "unknown error");
          nyr_mach_dump(&mach, stderr);
        }
        nyr_set_error(err, err_len,
                      "native JIT: machine form verification failed for "
                      "function %zu: %s",
                      i, mach_err[0] ? mach_err : "verify");
        nyr_mach_func_free(&mach);
        goto fail_nir;
      }
      if (!lowered) {
        if (ny_trace_enabled("NY_TRACE_MACHINE_FAIL")) {
          ny_trace_line("MACHINE_FAIL",
                        "machine form unsupported for function %zu: %s", i,
                        mach_err[0] ? mach_err : "unknown error");
          nyr_mach_dump(&mach, stderr);
        }
        nyr_mach_func_free(&mach);
        mach = (nyr_mach_func_t){0};
      } else if (getenv("NY_DUMP_MACH")) {
        fprintf(stderr, "native JIT machine form for %s:\n",
                names && names[i] ? names[i] : "unknown_fn");
        nyr_mach_dump(&mach, stderr);
      }
      nyr_mach_func_free(&mach);
    }
  }
  defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  size_t def_count = 0, reloc_count = 0;
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    nyr_set_error(err, err_len, "native JIT definition allocation failed");
    goto fail_nir;
  }

  if (target.target == NY_NATIVE_TARGET_AARCH64) {
    if (!nyr_native_jit_compile_aarch64_bundle(
            top, funcs, names, func_count, &target, module->runtime_services,
            image, err, err_len))
      goto fail_nir;
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
      nyr_x64_try_stencil_bundle(top, &target, &code, defs, &def_count, relocs,
                                 &reloc_count, err, err_len)) {
    used_mir = true;
  } else if (func_count > 0 &&
             nyr_x64_try_stencil_bundle_calls(
                 top, funcs, names, func_count, &target, &code, defs,
                 &def_count, relocs, &reloc_count, err, err_len)) {
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
    if (nyr_native_x64_build_mixed_bundle(
            top, funcs, names, func_count, &target, "rt_main", false, &code,
            defs, &def_count, relocs, &reloc_count, mach_err,
            sizeof(mach_err))) {
      used_mir = true;
    } else {
      nyr_obj_free(&code);
      code = (nyr_obj_buf_t){0};
      def_count = reloc_count = 0;
      snprintf(mach_err, sizeof(mach_err), "native JIT: mixed bundle failed");
    }
  }
  if (!used_mir &&
      !nyr_x64_obj_build_bundle(top, funcs, names, func_count, &target,
                                "rt_main", false, &code, defs, &def_count,
                                relocs, &reloc_count, err, err_len)) {
    nyr_obj_free(&code);
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
        ny_native_runtime_abi_decl(symbol) != NULL ||
        nyr_x64_obj_def_index(defs, def_count, symbol) >= 0 ||
        (module->runtime_services && module->runtime_services->resolve_symbol &&
         module->runtime_services->resolve_symbol(
             module->runtime_services->userdata, symbol)))
      continue;
    if (def_count >= NY_NATIVE_MAX_DEFS)
      break;
    while (code.len & 7u)
      if (!nyr_obj_u8(&code, 0))
        break;
    size_t off = code.len;
    if (!nyr_obj_u64(&code, 0))
      break;
    snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", symbol);
    defs[def_count].off = off;
    defs[def_count].size = 8;
    defs[def_count].is_data = true;
    ++def_count;
  }
  for (size_t i = 0; i < def_count; ++i)
    /*
     * The JIT writable pool is narrower than ELF's data classification:
     * floating constants can remain among executable code bytes.
     */
    defs[i].is_data = nyr_native_jit_def_is_data(defs[i].name);
  nyr_native_jit_cache_save(stencil_cache_path, &code, defs, def_count, relocs,
                            reloc_count);
  return nyr_native_jit_materialize_x64(code, defs, def_count, relocs,
                                        reloc_count, module->runtime_services,
                                        image, err, err_len);

fail_nir:
  free(defs);
  free(relocs);
  return false;
}
#endif

static void *nyr_api_jit_resolve_symbol(void *userdata, const char *symbol) {
  (void)userdata;
#ifdef _WIN32
  HMODULE process = GetModuleHandleA(NULL);
  return process && symbol ? (void *)GetProcAddress(process, symbol) : NULL;
#else
  /*
   * JIT code is the user's own program; it may call any process export, as
   * the AOT-linked program could (Brainfuck's generated code calls calloc).
   */
  return symbol ? dlsym(RTLD_DEFAULT, symbol) : NULL;
#endif
}

bool nyr_jit_run(const nyr_func_t *func, int64_t *result, char *err,
                 size_t err_len) {
  if (!func || !result) {
    nyr_set_error(err, err_len, "native JIT requires a function and result");
    return false;
  }
  if (!nyr_verify(func, err, err_len))
    return false;

  nyr_target_t host;
  if (!nyr_host_target(&host, err, err_len))
    return false;
  nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, nyr_native_jit_abi(NY_NATIVE_ABI_AUTO, NULL),
      host.target, false);
  ny_native_target_info_t target;
  if (!nyr_target_info_init(&target, &request) ||
      target.target != NY_NATIVE_TARGET_X86_64 ||
      !(target.caps & NY_NATIVE_CAP_LIVE_JIT)) {
    nyr_set_error(err, err_len,
                  "native JIT API currently requires a host x86-64 backend");
    return false;
  }

  nyr_func_t copy;
  nyr_func_init(&copy);
  if (!nyr_func_clone(func, &copy, err, err_len))
    return false;

  nyr_obj_buf_t code = {0};
  size_t def_count = 0, reloc_count = 0;
  nyr_x64_obj_symbol_def_t *defs = calloc(NY_NATIVE_MAX_DEFS, sizeof(*defs));
  nyr_x64_obj_reloc_t *relocs = calloc(NY_X64_OBJ_MAX_RELOCS, sizeof(*relocs));
  if (!defs || !relocs) {
    free(defs);
    free(relocs);
    nyr_func_destroy(&copy);
    nyr_set_error(err, err_len, "native JIT work allocation failed");
    return false;
  }

  bool built = nyr_x64_obj_build_bundle(
      &copy, NULL, NULL, 0, &target, "rt_main", false, &code, defs, &def_count,
      relocs, &reloc_count, err, err_len);
  nyr_func_destroy(&copy);
  if (!built) {
    nyr_obj_free(&code);
    free(defs);
    free(relocs);
    return false;
  }
  for (size_t i = 0; i < def_count; ++i)
    defs[i].is_data = nyr_native_jit_def_is_data(defs[i].name);

  const nyr_runtime_services_t services = {
      .resolve_symbol = nyr_api_jit_resolve_symbol,
  };
  nyr_jit_image_t image = {0};
  if (!nyr_native_jit_materialize_x64(code, defs, def_count, relocs,
                                      reloc_count, &services, &image, err,
                                      err_len))
    return false;
  if (!image.entry) {
    nyr_jit_image_free(&image);
    nyr_set_error(err, err_len, "native JIT produced no entry point");
    return false;
  }

  typedef int64_t (*nyr_entry_fn_t)(void);
  int64_t value = ((nyr_entry_fn_t)image.entry)();
  nyr_jit_image_free(&image);
  *result = value;
  if (err && err_len)
    err[0] = '\0';
  return true;
}
