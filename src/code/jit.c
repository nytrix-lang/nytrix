/*
 * LLVM JIT engine: MCJIT and ORC backends for just-in-time compilation,
 * native code caching, perf-map generation, and runtime code execution.
 */
#include "code/jit.h"
#include "base/common.h"
#include "base/util.h"
#include "rt/runtime.h"
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define NY_APPLE_ARM64_JIT 1
#endif
#endif
#include "priv.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Support.h>
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#ifdef _WIN32
static int ny_jit_optind = 1;

static int ny_jit_snprintf(char *dst, uint64_t cap, const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  int written = vsnprintf(dst, (size_t)cap, format, ap);
  va_end(ap);
  return written;
}
#endif

extern int64_t rt_alloc_string(const char *s);
extern int64_t rt_os_name(void);
extern int64_t rt_arch_name(void);
extern int64_t rt_main(void);
extern int64_t rt_simmd_byte_class_reduce_raw(int64_t ptr_raw, int64_t len_raw,
                                              int64_t rounds_raw, int64_t class_lo_raw,
                                              int64_t class_hi_raw, int64_t hit_raw,
                                              int64_t miss_raw);
extern int64_t rt_simmd_i32_sqlscan_sum_raw(int64_t region_raw, int64_t tier_raw,
                                            int64_t amount_raw, int64_t flags_raw,
                                            int64_t n_raw, int64_t rounds_raw);
/*
 * A referenced extern symbol could not be resolved against the process image
 * or any linked library.  Report it as a proper compile diagnostic (with the
 * declaration site when known) and fail the compile instead of aborting the
 * whole process with SIGABRT.
 */
static void ny_missing_extern_fail(codegen_t *cg, token_t tok,
                                   const char *symbol) {
  ny_diag_error(tok, "unresolved external symbol '%s' (JIT could not find it "
                     "in the process image or linked libraries)",
                symbol && *symbol ? symbol : "<unknown>");
  ny_diag_hint("%s", "declare the extern with the exact C link name, link the "
                     "library with 'use' / -l, or export the symbol from a "
                     "shared object before running");
  if (cg)
    cg->had_error = 1;
}


#if defined(NY_APPLE_ARM64_JIT)
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif

typedef struct ny_apple_jit_alloc_t {
  void *base;
  size_t size;
  bool code;
  bool read_only;
  struct ny_apple_jit_alloc_t *next;
} ny_apple_jit_alloc_t;

typedef struct {
  ny_apple_jit_alloc_t *allocs;
} ny_apple_jit_mm_t;

static size_t ny_jit_page_size(void) {
  long page = sysconf(_SC_PAGESIZE);
  return page > 0 ? (size_t)page : 16384u;
}

static uintptr_t ny_jit_align_up(uintptr_t value, uintptr_t alignment) {
  if (alignment <= 1)
    return value;
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static size_t ny_jit_round_page(size_t size) {
  size_t page = ny_jit_page_size();
  if (!size)
    size = 1;
  return (size + page - 1u) & ~(page - 1u);
}

static void ny_apple_jit_write_protect(int enabled) {
  pthread_jit_write_protect_np(enabled);
}

static uint8_t *ny_apple_jit_alloc_section(void *opaque, uintptr_t size,
                                           unsigned alignment, bool code,
                                           bool read_only) {
  ny_apple_jit_mm_t *mm = opaque;
  if (!mm)
    return NULL;
  uintptr_t align = alignment ? alignment : 16u;
  if (align & (align - 1u))
    align = 16u;
  size_t alloc_size = ny_jit_round_page((size_t)size + (size_t)align);
  int flags = MAP_PRIVATE | MAP_ANON | MAP_JIT;
  int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
  ny_apple_jit_write_protect(0);
  void *base = mmap(NULL, alloc_size, prot, flags, -1, 0);
  if (base == MAP_FAILED) {
    ny_apple_jit_write_protect(1);
    return NULL;
  }
  ny_apple_jit_alloc_t *node = calloc(1, sizeof(*node));
  if (!node) {
    munmap(base, alloc_size);
    ny_apple_jit_write_protect(1);
    return NULL;
  }
  node->base = base;
  node->size = alloc_size;
  node->code = code;
  node->read_only = read_only;
  node->next = mm->allocs;
  mm->allocs = node;
  return (uint8_t *)ny_jit_align_up((uintptr_t)base, align);
}

static uint8_t *ny_apple_jit_alloc_code(void *opaque, uintptr_t size,
                                        unsigned alignment, unsigned section_id,
                                        const char *section_name) {
  (void)section_id;
  (void)section_name;
  return ny_apple_jit_alloc_section(opaque, size, alignment, true, false);
}

static uint8_t *ny_apple_jit_alloc_data(void *opaque, uintptr_t size,
                                        unsigned alignment, unsigned section_id,
                                        const char *section_name,
                                        LLVMBool read_only) {
  (void)section_id;
  (void)section_name;
  return ny_apple_jit_alloc_section(opaque, size, alignment, false,
                                    read_only != 0);
}

static LLVMBool ny_apple_jit_finalize(void *opaque, char **err_msg) {
  ny_apple_jit_mm_t *mm = opaque;
  if (!mm)
    return 0;
  for (ny_apple_jit_alloc_t *a = mm->allocs; a; a = a->next) {
    int prot = PROT_READ;
    if (a->code) {
      sys_icache_invalidate(a->base, a->size);
      prot |= PROT_EXEC;
    } else if (!a->read_only) {
      prot |= PROT_WRITE;
    }
    prot |= PROT_EXEC;
    if (mprotect(a->base, a->size, prot) != 0) {
      if (err_msg)
        *err_msg = LLVMCreateMessage(a->code
                                         ? "failed to make Apple arm64 JIT code executable"
                                         : "failed to finalize Apple arm64 JIT data");
      ny_apple_jit_write_protect(1);
      return 1;
    }
  }
  ny_apple_jit_write_protect(1);
  return 0;
}

static void ny_apple_jit_destroy(void *opaque) {
  ny_apple_jit_mm_t *mm = opaque;
  if (!mm)
    return;
  ny_apple_jit_write_protect(0);
  ny_apple_jit_alloc_t *a = mm->allocs;
  while (a) {
    ny_apple_jit_alloc_t *next = a->next;
    if (a->base && a->size)
      munmap(a->base, a->size);
    free(a);
    a = next;
  }
  ny_apple_jit_write_protect(1);
  free(mm);
}

static LLVMMCJITMemoryManagerRef ny_apple_arm64_jit_memory_manager(void) {
  ny_apple_jit_mm_t *mm = calloc(1, sizeof(*mm));
  if (!mm)
    return NULL;
  LLVMMCJITMemoryManagerRef ref = LLVMCreateSimpleMCJITMemoryManager(
      mm, ny_apple_jit_alloc_code, ny_apple_jit_alloc_data,
      ny_apple_jit_finalize, ny_apple_jit_destroy);
  if (!ref)
    free(mm);
  return ref;
}
#endif

void ny_jit_init_options(struct LLVMMCJITCompilerOptions *options, LLVMModuleRef mod) {
  LLVMInitializeMCJITCompilerOptions(options, sizeof(*options));
  bool apple_arm64 = ny_module_target_is_apple_arm64(mod);

  int opt_level = 3;
  const char *opt_env = getenv("NYTRIX_JIT_OPT_LEVEL");
  if (opt_env && *opt_env) {
    int v = 0;
    if (ny_parse_int(opt_env, &v)) {
      opt_level = v;
      if (opt_level < 0)
        opt_level = 0;
      if (opt_level > 3)
        opt_level = 3;
    }
  }
  if (apple_arm64 && (!opt_env || !*opt_env)) {
    opt_level = 0;
  }

  int fast_isel = (opt_level <= 1) ? 1 : 0;
  const char *fast_isel_env = getenv("NYTRIX_JIT_FAST_ISEL");
  if (fast_isel_env && *fast_isel_env) {
    int v = 0;
    if (ny_parse_int(fast_isel_env, &v))
      fast_isel = (v != 0);
  } else if (apple_arm64) {
    fast_isel = 0;
  }

  options->CodeModel = apple_arm64 ? LLVMCodeModelLarge : LLVMCodeModelJITDefault;
  options->OptLevel = (unsigned)opt_level;
  options->EnableFastISel = fast_isel;
#if defined(NY_APPLE_ARM64_JIT)
  if (apple_arm64)
    options->MCJMM = ny_apple_arm64_jit_memory_manager();
#endif
  const char *cm = getenv("NYTRIX_JIT_CODE_MODEL");
  if (cm && *cm) {
    if (strcmp(cm, "default") == 0 || strcmp(cm, "jitdefault") == 0)
      options->CodeModel = LLVMCodeModelJITDefault;
    else if (strcmp(cm, "large") == 0)
      options->CodeModel = LLVMCodeModelLarge;
    else if (strcmp(cm, "medium") == 0)
      options->CodeModel = LLVMCodeModelMedium;
    else if (strcmp(cm, "small") == 0)
      options->CodeModel = LLVMCodeModelSmall;
  }
}

void ny_jit_init_native_once(void) {
  static int initialized = 0;
  if (initialized)
    return;
  LLVMLinkInMCJIT();
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();
  initialized = 1;
}

#if defined(NY_APPLE_ARM64_JIT)
static bool ny_apple_jit_region(uint64_t address, mach_vm_address_t *base,
                                mach_vm_size_t *size, vm_prot_t *protection) {
  mach_vm_address_t region = (mach_vm_address_t)address;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info = {0};
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(
      mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
      (vm_region_info_t)&info, &count, &object);
  if (object != MACH_PORT_NULL)
    mach_port_deallocate(mach_task_self(), object);
  if (kr != KERN_SUCCESS || address < region || address >= region + region_size)
    return false;
  if (base)
    *base = region;
  if (size)
    *size = region_size;
  if (protection)
    *protection = info.protection;
  return true;
}

static bool ny_apple_jit_prepare_address(uint64_t address) {
  if (!address)
    return true;
  mach_vm_address_t base = 0;
  mach_vm_size_t size = 0;
  vm_prot_t protection = 0;
  if (!ny_apple_jit_region(address, &base, &size, &protection))
    return false;
  if (!(protection & VM_PROT_EXECUTE)) {
    ny_apple_jit_write_protect(0);
    int ok = mprotect((void *)(uintptr_t)base, (size_t)size,
                      PROT_READ | PROT_EXEC) == 0;
    if (!ok)
      ok = mach_vm_protect(mach_task_self(), base, size, 0,
                           VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS;
    ny_apple_jit_write_protect(1);
    if (!ok || !ny_apple_jit_region(address, NULL, NULL, &protection) ||
        !(protection & VM_PROT_EXECUTE))
      return false;
  } else {
    ny_apple_jit_write_protect(1);
  }
  sys_icache_invalidate((void *)(uintptr_t)base, (size_t)size);
  ny_apple_jit_write_protect(1);
  return true;
}
#endif

bool ny_jit_prepare_execution(uint64_t address) {
#if defined(NY_APPLE_ARM64_JIT)
  return ny_apple_jit_prepare_address(address);
#else
  (void)address;
  return true;
#endif
}

bool ny_jit_prepare_module_execution(LLVMExecutionEngineRef ee, LLVMModuleRef mod) {
#if defined(NY_APPLE_ARM64_JIT)
  if (!ee || !mod)
    return false;

  size_t count = 0;
  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMCountBasicBlocks(fn) != 0)
      count++;
  }
  if (!count)
    return true;

  uint64_t *addresses = calloc(count, sizeof(*addresses));
  if (!addresses)
    return false;

  size_t used = 0;
  LLVMValueRef first = NULL;
  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMCountBasicBlocks(fn) == 0)
      continue;
    if (!first)
      first = fn;
    uint64_t addr = (uint64_t)(uintptr_t)LLVMGetPointerToGlobal(ee, fn);
    if (addr)
      addresses[used++] = addr;
  }

  if (first)
    (void)LLVMGetPointerToGlobal(ee, first);

  char *engine_error = NULL;
  if (LLVMExecutionEngineGetErrMsg(ee, &engine_error)) {
    if (engine_error)
      LLVMDisposeMessage(engine_error);
    free(addresses);
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < used; i++) {
    if (!ny_apple_jit_prepare_address(addresses[i])) {
      ok = false;
      break;
    }
  }
  free(addresses);
  return ok;
#else
  (void)ee;
  (void)mod;
  return true;
#endif
}

#if !defined(_WIN32) && defined(__APPLE__)
static const char *ny_jit_basename(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool ny_jit_apple_openssl_basename(const char *path, const char **name_out) {
  const char *base = ny_jit_basename(path);
  const char *name = NULL;
  if (strcmp(base, "crypto") == 0 || strcmp(base, "libcrypto.dylib") == 0)
    name = "crypto";
  else if (strncmp(base, "libcrypto.", 10) == 0 && strstr(base, ".dylib"))
    name = "crypto";
  else if (strcmp(base, "ssl") == 0 || strcmp(base, "libssl.dylib") == 0)
    name = "ssl";
  else if (strncmp(base, "libssl.", 7) == 0 && strstr(base, ".dylib"))
    name = "ssl";
  if (name_out)
    *name_out = name;
  return name != NULL;
}

static void *ny_jit_load_apple_openssl(const char *path) {
  const char *name = NULL;
  if (!ny_jit_apple_openssl_basename(path, &name))
    return NULL;

  const char *patterns[] = {
      "/opt/homebrew/opt/openssl@3/lib/lib%s.dylib",
      "/usr/local/opt/openssl@3/lib/lib%s.dylib",
      "/opt/homebrew/opt/openssl@1.1/lib/lib%s.dylib",
      "/usr/local/opt/openssl@1.1/lib/lib%s.dylib",
      NULL,
  };
  char buf[256];
  for (size_t i = 0; patterns[i]; i++) {
    snprintf(buf, sizeof(buf), patterns[i], name);
    void *h = dlopen(buf, RTLD_GLOBAL | RTLD_LAZY);
    if (h)
      return h;
  }
  return NULL;
}

/*
 * Homebrew is not part of dyld's default search path.  Keep this resolution
 * generic: a normal #link name may refer to any package, not just one known
 * library.  Explicit paths are intentionally left untouched.
 */
static void *ny_jit_load_apple_package_library(const char *path) {
  if (!path || strchr(path, '/') || strncmp(path, "-framework ", 11) == 0)
    return NULL;
  const char *base = ny_jit_basename(path);
  const char *stem = base;
  if (strncmp(stem, "lib", 3) == 0)
    stem += 3;
  size_t stem_len = strcspn(stem, ".");
  if (stem_len == 0 || stem_len >= 128)
    return NULL;
  char package[128];
  memcpy(package, stem, stem_len);
  package[stem_len] = '\0';

  const char *file = base;
  char synthesized[160];
  if (!strstr(file, ".dylib")) {
    snprintf(synthesized, sizeof(synthesized), "lib%s.dylib", package);
    file = synthesized;
  }
  const char *patterns[] = {
      "/opt/homebrew/opt/%s/lib/%s",
      "/usr/local/opt/%s/lib/%s",
      "/opt/homebrew/lib/%s",
      "/usr/local/lib/%s",
      NULL,
  };
  char candidate[512];
  for (size_t i = 0; patterns[i]; ++i) {
    snprintf(candidate, sizeof(candidate), patterns[i], package, file);
    void *h = dlopen(candidate, RTLD_GLOBAL | RTLD_LAZY);
    if (h)
      return h;
  }
  return NULL;
}
#endif

void *ny_jit_load_library(const char *path) {
  if (!path || !*path)
    return NULL;
  char *ensured = ny_ensure_shared_lib(path);
  const char *load_path = ensured ? ensured : path;
#ifdef _WIN32
  HMODULE h = LoadLibraryA(load_path);
  if (h) {
    free(ensured);
    return (void *)h;
  }
  if (ensured && strcmp(ensured, path) != 0) {
    h = LoadLibraryA(path);
    if (h) {
      free(ensured);
      return (void *)h;
    }
  }
  if (!strchr(path, '.') && !strchr(path, '\\') && !strchr(path, '/')) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.dll", path);
    h = LoadLibraryA(buf);
    if (h) {
      free(ensured);
      return (void *)h;
    }
    snprintf(buf, sizeof(buf), "lib%s.dll", path);
    h = LoadLibraryA(buf);
    if (h) {
      free(ensured);
      return (void *)h;
    }
  }
  free(ensured);
  return NULL;
#else
#ifdef __APPLE__
  void *apple_ssl = ny_jit_load_apple_openssl(path);
  if (apple_ssl) {
    free(ensured);
    return apple_ssl;
  }
  if (ny_jit_apple_openssl_basename(path, NULL)) {
    free(ensured);
    return NULL;
  }
  void *apple_package = ny_jit_load_apple_package_library(path);
  if (apple_package) {
    free(ensured);
    return apple_package;
  }
#endif
  void *h = dlopen(load_path, RTLD_GLOBAL | RTLD_LAZY);
  if (h) {
    if (verbose_enabled >= 2)
      fprintf(stderr, "JIT: loaded library '%s' (handle=%p)\n", load_path, h);
    free(ensured);
    return h;
  }
  if (ensured && strcmp(ensured, path) != 0) {
    h = dlopen(path, RTLD_GLOBAL | RTLD_LAZY);
    if (h) {
      if (verbose_enabled >= 2)
        fprintf(stderr, "JIT: loaded library '%s' (handle=%p)\n", path, h);
      free(ensured);
      return h;
    }
  }
  free(ensured);
  const bool has_dot = strchr(path, '.') != NULL;
  const bool has_sep = strchr(path, '/') != NULL;
#ifdef __APPLE__
  if (!has_dot && !has_sep) {
    char buf[256];
    snprintf(buf, sizeof(buf), "lib%s.dylib", path);
    h = dlopen(buf, RTLD_GLOBAL | RTLD_LAZY);
    if (h) {
      if (verbose_enabled >= 2)
        fprintf(stderr, "JIT: loaded library '%s' as '%s'\n", path, buf);
      return h;
    }
    snprintf(buf, sizeof(buf), "lib%s.0.dylib", path);
    h = dlopen(buf, RTLD_GLOBAL | RTLD_LAZY);
    if (h)
      return h;
  }
#else
  if (!has_dot && !has_sep) {
    char buf[256];
    snprintf(buf, sizeof(buf), "lib%s.so", path);
    h = dlopen(buf, RTLD_GLOBAL | RTLD_LAZY);
    if (h) {
      if (verbose_enabled >= 2)
        fprintf(stderr, "JIT: loaded library '%s' as '%s'\n", path, buf);
      return h;
    }
    const char *vers[] = {"6", "3", "2", "1", "0", "8", "12", "14", "18"};
    for (size_t i = 0; i < sizeof(vers) / sizeof(vers[0]); i++) {
      snprintf(buf, sizeof(buf), "lib%s.so.%s", path, vers[i]);
      h = dlopen(buf, RTLD_GLOBAL | RTLD_LAZY);
      if (h) {
        if (verbose_enabled >= 2)
          fprintf(stderr, "JIT: loaded library '%s' as '%s'\n", path, buf);
        return h;
      }
    }
  }
#endif
  if (verbose_enabled >= 2)
    fprintf(stderr, "JIT: failed to load library '%s': %s\n", path, dlerror());
  return NULL;
#endif
}

void *ny_jit_resolve_symbol(const char *symbol) {
  if (!symbol || !*symbol)
    return NULL;
  void *ptr = LLVMSearchForAddressOfSymbol(symbol);
  if (ptr)
    return ptr;
#ifdef _WIN32
  if (strcmp(symbol, "snprintf") == 0 || strcmp(symbol, "_snprintf") == 0)
    return (void *)(uintptr_t)ny_jit_snprintf;
  if (strcmp(symbol, "optind") == 0 || strcmp(symbol, "_optind") == 0)
    return &ny_jit_optind;
  if (strcmp(symbol, "getpid") == 0 || strcmp(symbol, "_getpid") == 0) {
    return (void *)(uintptr_t)_getpid;
  }
  HMODULE self = GetModuleHandleA(NULL);
  if (self) {
    FARPROC sym = GetProcAddress(self, symbol);
    if (sym)
      return (void *)sym;
  }
  static HMODULE cached_dlls[5] = {0};
  static int dlls_init = 0;
  const char *dll_names[] = {"ucrtbase.dll", "msvcrt.dll", "kernel32.dll", "ws2_32.dll"};
  if (!dlls_init) {
    for (int i = 0; i < 4; i++) {
      cached_dlls[i] = GetModuleHandleA(dll_names[i]);
      if (!cached_dlls[i])
        cached_dlls[i] = LoadLibraryA(dll_names[i]);
    }
    dlls_init = 1;
  }
  for (int i = 0; i < 4; i++) {
    if (!cached_dlls[i])
      continue;
    FARPROC sym = GetProcAddress(cached_dlls[i], symbol);
    if (sym)
      return (void *)sym;
  }
  return NULL;
#else
  return dlsym(RTLD_DEFAULT, symbol);
#endif
}

static void *resolve_symbol_with_fallback(const char *symbol) {
  void *ptr = ny_jit_resolve_symbol(symbol);
  if (!ptr && symbol && strncmp(symbol, "std.core.primitives.", 20) == 0)
    ptr = ny_jit_resolve_symbol(symbol + 20);
  return ptr;
}

void ny_jit_map_unresolved_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                   const char *entry_name) {
  if (!ee || !mod)
    return;
  if (verbose_enabled >= 3)
    fprintf(stderr, "[jit] mapping unresolved symbols for module...\n");
  for (LLVMValueRef g = LLVMGetFirstGlobal(mod); g; g = LLVMGetNextGlobal(g)) {
    if (LLVMGetInitializer(g))
      continue;
    const char *name = LLVMGetValueName(g);
    if (!name || !*name)
      continue;
    void *addr = resolve_symbol_with_fallback(name);
    if (addr)
      LLVMAddGlobalMapping(ee, g, addr);
  }
  if (verbose_enabled >= 3)
    fprintf(stderr, "[jit] mapping unresolved functions...\n");
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f; f = LLVMGetNextFunction(f)) {
    const char *name = LLVMGetValueName(f);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0)
      continue;
    bool has_body = LLVMGetFirstBasicBlock(f) != NULL;
    if (has_body && (!entry_name || strcmp(name, entry_name) != 0))
      continue;
    if (verbose_enabled >= 4)
      fprintf(stderr, "JIT: checking function '%s' (has_body=%d)\n", name,
              has_body ? 1 : 0);
    void *addr = resolve_symbol_with_fallback(name);
    if (addr) {
      if (verbose_enabled >= 3)
        fprintf(stderr, "JIT: mapped symbol '%s' to %p\n", name, addr);
      LLVMAddGlobalMapping(ee, f, addr);
    } else {
      if (verbose_enabled >= 2 && LLVMIsDeclaration(f))
        fprintf(stderr, "JIT: unresolved symbol '%s'\n", name);
    }
  }
}

void ny_jit_add_runtime_symbols(void) {
  static bool registered = false;
  if (registered)
    return;
  registered = true;
#define RT_DEF(name, p, args, sig, doc)                                                            \
  do {                                                                                             \
    (void)(args);                                                                                  \
    (void)(sig);                                                                                   \
    (void)(doc);                                                                                   \
    LLVMAddSymbol(name, (void *)(uintptr_t)p);                                                     \
    if (strcmp(name, #p) != 0)                                                                     \
      LLVMAddSymbol(#p, (void *)(uintptr_t)p);                                                     \
  } while (0);
#define RT_GV(name, p, t, doc)                                                                     \
  do {                                                                                             \
    (void)(doc);                                                                                   \
    LLVMAddSymbol(name, (void *)&p);                                                               \
  } while (0);
#include "rt/defs.h"
#undef RT_DEF
#undef RT_GV
  LLVMAddSymbol("rt_simmd_byte_class_reduce_raw",
                (void *)(uintptr_t)rt_simmd_byte_class_reduce_raw);
  LLVMAddSymbol("rt_simmd_i32_sqlscan_sum_raw",
                (void *)(uintptr_t)rt_simmd_i32_sqlscan_sum_raw);
  LLVMAddSymbol("__alloc_string", (void *)(uintptr_t)rt_alloc_string);
  LLVMAddSymbol("rt_bigint_from_i64_raw",
                (void *)(uintptr_t)rt_bigint_from_i64_raw);
  LLVMAddSymbol("rt_bigint_to_i64_raw",
                (void *)(uintptr_t)rt_bigint_to_i64_raw);
  LLVMAddSymbol("rt_native_has_tag",
                (void *)(uintptr_t)rt_native_has_tag);
  LLVMAddSymbol("rt_native_bigint_cmp",
                (void *)(uintptr_t)rt_native_bigint_cmp);
  LLVMAddSymbol("rt_native_bigfloat_from_value",
                (void *)(uintptr_t)rt_native_bigfloat_from_value);
  LLVMAddSymbol("rt_native_bigfloat_to_f64",
                (void *)(uintptr_t)rt_native_bigfloat_to_f64);
  LLVMAddSymbol("rt_native_bigfloat_cmp",
                (void *)(uintptr_t)rt_native_bigfloat_cmp);
  LLVMAddSymbol("rt_native_bigfloat_precision",
                (void *)(uintptr_t)rt_native_bigfloat_precision);
  LLVMAddSymbol("rt_native_bigfloat_pow_int",
                (void *)(uintptr_t)rt_native_bigfloat_pow_int);
  LLVMAddSymbol("rt_native_f64_to_i64",
                (void *)(uintptr_t)rt_native_f64_to_i64);
  LLVMAddSymbol("rt_native_sin_f64",
                (void *)(uintptr_t)rt_native_sin_f64);
  LLVMAddSymbol("rt_native_cos_f64",
                (void *)(uintptr_t)rt_native_cos_f64);
  LLVMAddSymbol("rt_native_tbuf_new",
                (void *)(uintptr_t)rt_native_tbuf_new);
  LLVMAddSymbol("rt_native_tbuf_append",
                (void *)(uintptr_t)rt_native_tbuf_append);
  LLVMAddSymbol("rt_native_tbuf_len",
                (void *)(uintptr_t)rt_native_tbuf_len);
  LLVMAddSymbol("rt_native_tbuf_pop",
                (void *)(uintptr_t)rt_native_tbuf_pop);
  LLVMAddSymbol("rt_native_load8_idx",
                (void *)(uintptr_t)rt_native_load8_idx);
  LLVMAddSymbol("rt_native_store8_idx",
                (void *)(uintptr_t)rt_native_store8_idx);
  LLVMAddSymbol("rt_native_cstr_len",
                (void *)(uintptr_t)rt_native_cstr_len);
  LLVMAddSymbol("rt_native_cstr_builder_new",
                (void *)(uintptr_t)rt_native_cstr_builder_new);
  LLVMAddSymbol("rt_native_cstr_builder_append",
                (void *)(uintptr_t)rt_native_cstr_builder_append);
  LLVMAddSymbol("rt_native_cstr_builder_finalize",
                (void *)(uintptr_t)rt_native_cstr_builder_finalize);
  LLVMAddSymbol("rt_native_bool_to_cstr",
                (void *)(uintptr_t)rt_native_bool_to_cstr);
  LLVMAddSymbol("rt_native_dict_new",
                (void *)(uintptr_t)rt_native_dict_new);
  LLVMAddSymbol("rt_native_dict_get",
                (void *)(uintptr_t)rt_native_dict_get);
  LLVMAddSymbol("rt_native_dict_set",
                (void *)(uintptr_t)rt_native_dict_set);
  LLVMAddSymbol("rt_native_dict_has",
                (void *)(uintptr_t)rt_native_dict_has);
  LLVMAddSymbol("rt_native_dict_len",
                (void *)(uintptr_t)rt_native_dict_len);
  LLVMAddSymbol("rt_native_dict_delete",
                (void *)(uintptr_t)rt_native_dict_delete);
#ifdef _WIN32
  LLVMAddSymbol("snprintf", (void *)(uintptr_t)ny_jit_snprintf);
  LLVMAddSymbol("_snprintf", (void *)(uintptr_t)ny_jit_snprintf);
  LLVMAddSymbol("optind", &ny_jit_optind);
  LLVMAddSymbol("_optind", &ny_jit_optind);
#endif
}

static void ny_jit_define_runtime_trampoline(LLVMModuleRef mod, const char *name,
                                             void *ptr) {
  if (!mod || !name || !*name || !ptr)
    return;
  LLVMValueRef f = LLVMGetNamedFunction(mod, name);
  if (!f || !LLVMIsDeclaration(f))
    return;
  LLVMTypeRef fty = LLVMGlobalGetValueType(f);
  if (!fty)
    return;
  unsigned argc = LLVMCountParams(f);
  if (argc > 32)
    return;
  LLVMContextRef ctx = LLVMGetModuleContext(mod);
  LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
  LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(ctx, f, "ct_rt");
  LLVMPositionBuilderAtEnd(b, bb);

  LLVMValueRef params[32];
  for (unsigned i = 0; i < argc; ++i)
    params[i] = LLVMGetParam(f, i);

  LLVMValueRef addr =
      LLVMConstInt(LLVMInt64TypeInContext(ctx), (uint64_t)(uintptr_t)ptr, false);
  LLVMValueRef callee = LLVMBuildIntToPtr(b, addr, LLVMTypeOf(f), "ct_rt_fn");
  LLVMValueRef ret =
      LLVMBuildCall2(b, fty, callee, params, argc, "ct_rt_result");
  LLVMBuildRet(b, ret);
  LLVMDisposeBuilder(b);
}

void ny_jit_define_runtime_trampolines(LLVMModuleRef mod) {
#define RT_DEF(name, p, args, sig, doc)                                                            \
  do {                                                                                             \
    (void)(args);                                                                                  \
    (void)(sig);                                                                                   \
    (void)(doc);                                                                                   \
    ny_jit_define_runtime_trampoline(mod, name, (void *)(uintptr_t)p);                             \
    if (strcmp(name, #p) != 0)                                                                     \
      ny_jit_define_runtime_trampoline(mod, #p, (void *)(uintptr_t)p);                             \
  } while (0);
#define RT_GV(name, p, t, doc)                                                                     \
  do {                                                                                             \
    (void)(name);                                                                                  \
    (void)(p);                                                                                     \
    (void)(doc);                                                                                   \
  } while (0);
#include "rt/defs.h"
#undef RT_DEF
#undef RT_GV
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_byte_class_reduce_raw",
                                   (void *)(uintptr_t)rt_simmd_byte_class_reduce_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_i32_sqlscan_sum_raw",
                                   (void *)(uintptr_t)rt_simmd_i32_sqlscan_sum_raw);
  ny_jit_define_runtime_trampoline(mod, "__alloc_string",
                                   (void *)(uintptr_t)rt_alloc_string);
  ny_jit_define_runtime_trampoline(mod, "rt_native_tbuf_new",
                                   (void *)(uintptr_t)rt_native_tbuf_new);
  ny_jit_define_runtime_trampoline(mod, "rt_native_tbuf_append",
                                   (void *)(uintptr_t)rt_native_tbuf_append);
  ny_jit_define_runtime_trampoline(mod, "rt_native_load8_idx",
                                   (void *)(uintptr_t)rt_native_load8_idx);
  ny_jit_define_runtime_trampoline(mod, "rt_native_store8_idx",
                                   (void *)(uintptr_t)rt_native_store8_idx);
  ny_jit_define_runtime_trampoline(mod, "rt_native_tbuf_len",
                                   (void *)(uintptr_t)rt_native_tbuf_len);
  ny_jit_define_runtime_trampoline(mod, "rt_native_tbuf_pop",
                                   (void *)(uintptr_t)rt_native_tbuf_pop);
  ny_jit_define_runtime_trampoline(mod, "rt_native_cstr_len",
                                   (void *)(uintptr_t)rt_native_cstr_len);
  ny_jit_define_runtime_trampoline(mod, "rt_native_cstr_builder_new",
                                   (void *)(uintptr_t)rt_native_cstr_builder_new);
  ny_jit_define_runtime_trampoline(mod, "rt_native_cstr_builder_append",
                                   (void *)(uintptr_t)rt_native_cstr_builder_append);
  ny_jit_define_runtime_trampoline(mod, "rt_native_cstr_builder_finalize",
                                   (void *)(uintptr_t)rt_native_cstr_builder_finalize);
  ny_jit_define_runtime_trampoline(mod, "rt_native_bool_to_cstr",
                                   (void *)(uintptr_t)rt_native_bool_to_cstr);
  ny_jit_define_runtime_trampoline(mod, "rt_native_dict_new",
                                   (void *)(uintptr_t)rt_native_dict_new);
  ny_jit_define_runtime_trampoline(mod, "rt_native_dict_get",
                                   (void *)(uintptr_t)rt_native_dict_get);
  ny_jit_define_runtime_trampoline(mod, "rt_native_dict_set",
                                   (void *)(uintptr_t)rt_native_dict_set);
  ny_jit_define_runtime_trampoline(mod, "rt_native_dict_has",
                                   (void *)(uintptr_t)rt_native_dict_has);
  ny_jit_define_runtime_trampoline(mod, "rt_native_dict_len",
                                   (void *)(uintptr_t)rt_native_dict_len);
  ny_jit_define_runtime_trampoline(mod, "rt_native_dict_delete",
                                   (void *)(uintptr_t)rt_native_dict_delete);
}

static void register_extern_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod, codegen_t *cg) {
  if (!cg || !mod)
    return;

  if (cg->fun_sigs.len == 0) {
    for (LLVMValueRef f = LLVMGetFirstFunction(mod); f; f = LLVMGetNextFunction(f)) {
      if (!LLVMIsDeclaration(f))
        continue;
      if (!LLVMGetFirstUse(f))
        continue;
      const char *name = LLVMGetValueName(f);
      if (!name || strncmp(name, "llvm.", 5) == 0)
        continue;
      void *ptr = resolve_symbol_with_fallback(name);
      if (!ptr) {
        NY_LOG_DEBUG("extern symbol '%s' not found (cache fallback)", name);
        ny_missing_extern_fail(cg, (token_t){0}, name);
      } else {
        LLVMAddGlobalMapping(ee, f, ptr);
      }
    }
    return;
  }
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->is_extern)
      continue;

    const char *symbol = sig->link_name ? sig->link_name : sig->name;

    const char *llvm_name = sig->llvm_name ? sig->llvm_name : symbol;
    LLVMValueRef val = LLVMGetNamedFunction(mod, llvm_name);
    if (!val) {
      val = LLVMGetNamedFunction(mod, sig->name);
    }
    if (!val)
      continue;
    if (!LLVMGetFirstUse(val))
      continue;

    void *ptr = resolve_symbol_with_fallback(symbol);
    if (!ptr) {
      token_t tok = sig->stmt_t ? sig->stmt_t->tok : (token_t){0};
      ny_missing_extern_fail(cg, tok, symbol);
    } else {
      LLVMAddGlobalMapping(ee, val, ptr);
    }
  }
}

static void register_jit_sigs(LLVMExecutionEngineRef ee, LLVMModuleRef mod, codegen_t *cg) {
  (void)ee;
  (void)mod;
  (void)cg;

}

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod, codegen_t *cg) {
  ny_jit_add_runtime_symbols();
  if (cg) {
    for (size_t i = 0; i < cg->links.len; i++) {
      ny_jit_load_library(cg->links.data[i]);
    }
  }
  bool auto_declare_runtime = cg && ny_env_enabled("NYTRIX_JIT_AUTODECL_RT");
  if (!auto_declare_runtime) {
    register_extern_symbols(ee, mod, cg);
    register_jit_sigs(ee, mod, cg);
    goto apply_runtime_attrs;
  }
#define MAP_FULL(name, fn_ptr, args)                                                               \
  do {                                                                                             \
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                                            \
    if (!val && auto_declare_runtime) {                                                            \
      LLVMTypeRef param_types[16];                                                                 \
      int n_params = (args < 0) ? 0 : (args > 16 ? 16 : args);                                     \
      for (int i = 0; i < n_params; i++)                                                           \
        param_types[i] = cg->type_i64;                                                             \
      LLVMTypeRef ftype = LLVMFunctionType(cg->type_i64, param_types, n_params, args < 0);         \
      val = LLVMAddFunction(mod, name, ftype);                                                     \
      if (verbose_enabled >= 4)                                                                    \
        fprintf(stderr, "[jit] auto-declared %s\n", name);                                         \
    }                                                                                              \
    if (val) {                                                                                     \
      LLVMAddGlobalMapping(ee, val, (void *)(uintptr_t)fn_ptr);                                    \
      if (verbose_enabled >= 4)                                                                    \
        fprintf(stderr, "[jit] mapped %s to %p\n", name, (void *)fn_ptr);                          \
    }                                                                                              \
  } while (0)

#define MAP_GV(name, ptr)                                                                          \
  do {                                                                                             \
    LLVMValueRef val = LLVMGetNamedGlobal(mod, name);                                              \
    if (val) {                                                                                     \
      LLVMAddGlobalMapping(ee, val, (void *)ptr);                                                  \
      if (verbose_enabled >= 3)                                                                    \
        fprintf(stderr, "[jit] mapped global %s to %p\n", name, (void *)ptr);                      \
    }                                                                                              \
  } while (0)

#define RT_DEF(name, p, args, sig, doc)                                                            \
  do {                                                                                             \
    MAP_FULL(name, p, args);                                                                       \
    if (strcmp(name, #p) != 0)                                                                     \
      MAP_FULL(#p, p, args);                                                                       \
  } while (0);
#define RT_GV(name, p, t, doc) MAP_GV(name, &p);
#include "rt/defs.h"
#undef RT_DEF
#undef RT_GV
  MAP_FULL("rt_simmd_byte_class_reduce_raw", rt_simmd_byte_class_reduce_raw, 7);
  MAP_FULL("rt_simmd_i32_sqlscan_sum_raw", rt_simmd_i32_sqlscan_sum_raw, 6);
  register_extern_symbols(ee, mod, cg);
  register_jit_sigs(ee, mod, cg);

apply_runtime_attrs:
  ;

  LLVMValueRef panic_fn = LLVMGetNamedFunction(mod, "__panic");
  if (panic_fn) {
    unsigned nr_kind = LLVMGetEnumAttributeKindForName("noreturn", 8);
    if (nr_kind != 0) {
      LLVMAttributeRef nr_attr = LLVMCreateEnumAttribute(cg->ctx, nr_kind, 0);
      LLVMAddAttributeAtIndex(panic_fn, LLVMAttributeFunctionIndex, nr_attr);
    }
  }
}

static char *ny_orc_error_message(LLVMErrorRef err) {
  if (!err)
    return NULL;
  char *llvm_msg = LLVMGetErrorMessage(err);
  char *copy = ny_strdup(llvm_msg ? llvm_msg : "unknown ORC error");
  if (llvm_msg)
    LLVMDisposeErrorMessage(llvm_msg);
  return copy;
}

static void ny_orc_register_extern_symbols(LLVMModuleRef mod, codegen_t *cg) {
  if (!mod)
    return;
  ny_jit_add_runtime_symbols();
  if (cg) {
    for (size_t i = 0; i < cg->links.len; ++i)
      ny_jit_load_library(cg->links.data[i]);
  }
  if (cg && cg->fun_sigs.len > 0) {
    for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
      fun_sig *sig = &cg->fun_sigs.data[i];
      if (!sig->is_extern)
        continue;
      const char *symbol = sig->link_name ? sig->link_name : sig->name;
      const char *llvm_name = sig->llvm_name ? sig->llvm_name : symbol;
      if (!symbol || !llvm_name)
        continue;
      void *ptr = resolve_symbol_with_fallback(symbol);
      if (ptr)
        LLVMAddSymbol(llvm_name, ptr);
    }
    return;
  }
  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (!LLVMIsDeclaration(fn) || !LLVMGetFirstUse(fn))
      continue;
    const char *name = LLVMGetValueName(fn);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0)
      continue;
    void *ptr = resolve_symbol_with_fallback(name);
    if (ptr)
      LLVMAddSymbol(name, ptr);
  }
}

bool ny_orc_jit_ensure_engine(codegen_t *cg, char **error_message) {
  if (error_message)
    *error_message = NULL;
  if (!cg)
    return false;
  if (cg->orc_jit)
    return true;

#if LLVM_VERSION_MAJOR < 21
  if (error_message)
    *error_message = ny_strdup(
        "ORC JIT requires LLVM 21 or newer; use the default MCJIT engine");
  return false;
#else
  LLVMOrcLLJITRef jit = NULL;
  LLVMErrorRef err = LLVMOrcCreateLLJIT(&jit, NULL);
  if (err) {
    if (error_message)
      *error_message = ny_orc_error_message(err);
    return false;
  }
  LLVMOrcJITDylibRef dylib = LLVMOrcLLJITGetMainJITDylib(jit);
  LLVMOrcDefinitionGeneratorRef generator = NULL;
  err = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
      &generator, LLVMOrcLLJITGetGlobalPrefix(jit), NULL, NULL);
  if (!err)
    LLVMOrcJITDylibAddGenerator(dylib, generator);
  if (err) {
    if (error_message)
      *error_message = ny_orc_error_message(err);
    (void)LLVMOrcDisposeLLJIT(jit);
    return false;
  }
  cg->orc_jit = jit;
  return true;
#endif
}

bool ny_orc_jit_execute(codegen_t *cg, LLVMModuleRef module,
                        LLVMContextRef context, uint64_t *script_addr,
                        uint64_t *main_addr, void **out_rt,
                        bool *module_consumed, char **error_message) {
  if (script_addr)
    *script_addr = 0;
  if (main_addr)
    *main_addr = 0;
  if (out_rt)
    *out_rt = NULL;
  if (module_consumed)
    *module_consumed = false;
  if (error_message)
    *error_message = NULL;

  void *orc_jit =
      cg ? (cg->orc_jit ? cg->orc_jit
                        : (cg->parent ? cg->parent->orc_jit : NULL))
         : NULL;

  if (!cg || !orc_jit || !module || !context || !out_rt)
    return false;

#if LLVM_VERSION_MAJOR < 21
  return false;
#else
  LLVMOrcLLJITRef jit = (LLVMOrcLLJITRef)orc_jit;
  ny_orc_register_extern_symbols(module, cg);

  LLVMOrcJITDylibRef dylib = LLVMOrcLLJITGetMainJITDylib(jit);
  LLVMOrcResourceTrackerRef rt = LLVMOrcJITDylibCreateResourceTracker(dylib);
  if (!rt) {
    if (error_message)
      *error_message = ny_strdup("failed to create resource tracker");
    return false;
  }

  LLVMOrcThreadSafeContextRef ts_context =
      LLVMOrcCreateNewThreadSafeContextFromLLVMContext(context);
  if (!ts_context) {
    LLVMDisposeModule(module);
    if (module_consumed)
      *module_consumed = true;
    if (error_message)
      *error_message = ny_strdup("could not create ORC thread-safe context");
    LLVMOrcReleaseResourceTracker(rt);
    return false;
  }
  LLVMOrcThreadSafeModuleRef ts_module =
      LLVMOrcCreateNewThreadSafeModule(module, ts_context);
  if (module_consumed)
    *module_consumed = true;
  LLVMOrcDisposeThreadSafeContext(ts_context);
  if (!ts_module) {
    if (error_message)
      *error_message = ny_strdup("could not create ORC thread-safe module");
    LLVMOrcReleaseResourceTracker(rt);
    return false;
  }

  LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModuleWithRT(jit, rt, ts_module);
  if (err) {
    if (error_message)
      *error_message = ny_orc_error_message(err);
    LLVMOrcReleaseResourceTracker(rt);
    return false;
  }

  LLVMOrcExecutorAddress addr = 0;
  err = LLVMOrcLLJITLookup(jit, &addr, "_ny_top_entry");
  if (err) {
    if (error_message)
      *error_message = ny_orc_error_message(err);
    err = LLVMOrcResourceTrackerRemove(rt);
    if (err) LLVMConsumeError(err);
    LLVMOrcReleaseResourceTracker(rt);
    return false;
  }

  if (script_addr)
    *script_addr = (uint64_t)addr;

  addr = 0;
  err = LLVMOrcLLJITLookup(jit, &addr, "main");
  if (!err && main_addr)
    *main_addr = (uint64_t)addr;
  else if (err)
    LLVMConsumeError(err);

  if (out_rt)
    *out_rt = rt;
  return true;
#endif
}

void ny_orc_jit_remove_module(void *rt) {
#if LLVM_VERSION_MAJOR >= 21
  if (!rt)
    return;
  LLVMOrcResourceTrackerRef tracker = (LLVMOrcResourceTrackerRef)rt;
  LLVMErrorRef err = LLVMOrcResourceTrackerRemove(tracker);
  if (err)
    LLVMConsumeError(err);
  LLVMOrcReleaseResourceTracker(tracker);
#endif
}

void ny_orc_jit_dispose(void *jit) {
  if (!jit)
    return;
  LLVMErrorRef err = LLVMOrcDisposeLLJIT((LLVMOrcLLJITRef)jit);
  if (err)
    LLVMConsumeError(err);
}

#ifndef _WIN32
static int compare_func_info(const void *a, const void *b) {
  const uint64_t a_addr = ((const struct {
                            uint64_t addr;
                            const char *name;
                          } *)a)
                              ->addr;
  const uint64_t b_addr = ((const struct {
                            uint64_t addr;
                            const char *name;
                          } *)b)
                              ->addr;
  if (a_addr < b_addr)
    return -1;
  if (a_addr > b_addr)
    return 1;
  return 0;
}
#endif

void ny_jit_write_perf_map(LLVMExecutionEngineRef ee, LLVMModuleRef mod) {
#ifndef _WIN32
  const char *env = getenv("NYTRIX_JIT_PERF_MAP");
  if (!env || !*env || strcmp(env, "0") == 0)
    return;
  char path[4096];
  snprintf(path, sizeof(path), "%s/perf-%d.map", ny_get_temp_dir(), (int)getpid());
  FILE *f = fopen(path, "a");
  if (!f)
    return;

  typedef struct {
    uint64_t addr;
    const char *name;
  } func_info_t;

  func_info_t *funcs = NULL;
  size_t num_funcs = 0;
  size_t capacity = 0;

  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn; fn = LLVMGetNextFunction(fn)) {
    if (LLVMIsDeclaration(fn))
      continue;
    const char *name = LLVMGetValueName(fn);
    if (!name || !*name)
      continue;
    uint64_t addr = LLVMGetFunctionAddress(ee, name);
    if (!addr)
      continue;

    if (num_funcs == capacity) {
      capacity = capacity == 0 ? 16 : capacity * 2;
      func_info_t *new_funcs = (func_info_t *)realloc(funcs, capacity * sizeof(func_info_t));
      if (!new_funcs) {
        free(funcs);
        fclose(f);
        return;
      }
      funcs = new_funcs;
    }
    funcs[num_funcs].addr = addr;
    funcs[num_funcs].name = name;
    num_funcs++;
  }

  if (num_funcs > 1) {
    qsort(funcs, num_funcs, sizeof(func_info_t), compare_func_info);
  }

  fclose(f);
  f = fopen(path, "w");
  if (!f) {
    free(funcs);
    return;
  }

  for (size_t i = 0; i < num_funcs; i++) {
    uint64_t addr = funcs[i].addr;
    const char *name = funcs[i].name;
    uintptr_t size = 100;
    if (i + 1 < num_funcs) {
      size = (uintptr_t)(funcs[i + 1].addr - addr);
    }
    fprintf(f, "%lx %lx %s\n", (unsigned long)addr, (unsigned long)size, name);
  }

  free(funcs);
  fclose(f);
#else
  (void)ee;
  (void)mod;
#endif
}
