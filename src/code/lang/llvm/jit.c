/*
 * LLVM JIT engine: MCJIT and ORC backends for just-in-time compilation,
 * native code caching, perf-map generation, and runtime code execution.
 */
#include "code/lang/llvm/jit.h"
#include "base/common.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/lang/runtime/runtime.h"
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/mman.h>
#define NY_APPLE_ARM64_JIT 1
#endif
#endif
#include "code/lang/priv.h"
#include <inttypes.h>
#include <limits.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Object.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Support.h>
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdarg.h>
#include <stdatomic.h>
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
extern int64_t rt_bigint_add(int64_t, int64_t);
extern int64_t rt_bigint_sub(int64_t, int64_t);
extern int64_t rt_bigint_mul(int64_t, int64_t);
extern int64_t rt_bigint_div(int64_t, int64_t);
extern int64_t rt_bigint_mod(int64_t, int64_t);
extern int64_t rt_bigint_pow(int64_t, int64_t);
extern int64_t rt_bigint_xor(int64_t, int64_t);
extern int64_t rt_simmd_byte_class_reduce_raw(
    int64_t ptr_raw, int64_t len_raw, int64_t rounds_raw, int64_t class_lo_raw,
    int64_t class_hi_raw, int64_t hit_raw, int64_t miss_raw);
extern int64_t rt_simmd_i32_sqlscan_sum_raw(int64_t region_raw,
                                            int64_t tier_raw,
                                            int64_t amount_raw,
                                            int64_t flags_raw, int64_t n_raw,
                                            int64_t rounds_raw);
/*
 * A referenced extern symbol could not be resolved against the process image
 * or any linked library.  Report it as a proper compile diagnostic (with the
 * declaration site when known) and fail the compile instead of aborting the
 * whole process with SIGABRT.
 */
static void ny_missing_extern_fail(codegen_t *cg, token_t tok,
                                   const char *symbol) {
  ny_diag_error(tok,
                "unresolved external symbol '%s' (JIT could not find it "
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
        *err_msg = LLVMCreateMessage(
            a->code ? "failed to make Apple arm64 JIT code executable"
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

void ny_jit_init_options(struct LLVMMCJITCompilerOptions *options,
                         LLVMModuleRef mod) {
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

  /*
   * Large code model on every target: with the JIT default model, mcjit's
   * slab allocations can land more than 2 GiB from the host image under
   * ASLR, and finalize then fails with "PC32 relocation ... out of range"
   * for runtime symbols such as rt_zalloc_raw (the platform-ui-open probe).
   * The large model emits ABS64 addressing for external references; the
   * bench suite showed no measurable cost (havlak 22.6 vs 23.1 ms).
   */
  options->CodeModel = LLVMCodeModelLarge;
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
  kern_return_t kr = mach_vm_region(mach_task_self(), &region, &region_size,
                                    VM_REGION_BASIC_INFO_64,
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

bool ny_jit_prepare_module_execution(LLVMExecutionEngineRef ee,
                                     LLVMModuleRef mod) {
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

static bool ny_jit_apple_openssl_basename(const char *path,
                                          const char **name_out) {
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
  const char *dll_names[] = {"ucrtbase.dll", "msvcrt.dll", "kernel32.dll",
                             "ws2_32.dll"};
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

/*
 * Bind referenced declarations before handing the module to MCJIT. Late
 * mappings left extern calls in this module as indirect calls through null.
 */
static void ny_jit_bind_declaration(LLVMModuleRef mod, LLVMValueRef fn,
                                    void *address) {
  if (!mod || !fn || !address || !LLVMIsDeclaration(fn) || !LLVMGetFirstUse(fn))
    return;
  LLVMContextRef ctx = LLVMGetModuleContext(mod);
  LLVMTypeRef uintptr_ty =
      LLVMIntTypeInContext(ctx, (unsigned)(sizeof(uintptr_t) * CHAR_BIT));
  LLVMValueRef address_int =
      LLVMConstInt(uintptr_ty, (uint64_t)(uintptr_t)address, 0);
  LLVMValueRef address_fn = LLVMConstIntToPtr(address_int, LLVMTypeOf(fn));
  LLVMReplaceAllUsesWith(fn, address_fn);
}

void ny_jit_register_module_symbols(LLVMModuleRef mod, codegen_t *cg) {
  if (!mod)
    return;
  ny_jit_add_runtime_symbols();
  if (cg) {
    for (size_t i = 0; i < cg->links.len; ++i)
      (void)ny_jit_load_library(cg->links.data[i]);
    for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
      const fun_sig *sig = &cg->fun_sigs.data[i];
      if (!sig->is_extern)
        continue;
      const char *link_name = sig->link_name ? sig->link_name : sig->name;
      void *addr = ny_jit_resolve_symbol(link_name);
      if (!addr)
        continue;
      if (sig->name && *sig->name) {
        LLVMAddSymbol(sig->name, addr);
        ny_jit_bind_declaration(mod, LLVMGetNamedFunction(mod, sig->name),
                                addr);
      }
      if (sig->llvm_name && *sig->llvm_name) {
        LLVMAddSymbol(sig->llvm_name, addr);
        ny_jit_bind_declaration(mod, LLVMGetNamedFunction(mod, sig->llvm_name),
                                addr);
      }
    }
  }
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (!LLVMIsDeclaration(f) || !LLVMGetFirstUse(f))
      continue;
    const char *name = LLVMGetValueName(f);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0)
      continue;
    void *addr = ny_jit_resolve_symbol(name);
    if (addr) {
      LLVMAddSymbol(name, addr);
      ny_jit_bind_declaration(mod, f, addr);
    }
  }
  for (LLVMValueRef g = LLVMGetFirstGlobal(mod); g; g = LLVMGetNextGlobal(g)) {
    if (LLVMGetInitializer(g) || !LLVMGetFirstUse(g))
      continue;
    const char *name = LLVMGetValueName(g);
    if (!name || !*name)
      continue;
    void *addr = ny_jit_resolve_symbol(name);
    if (addr)
      LLVMAddSymbol(name, addr);
  }
}

static void *resolve_symbol_with_fallback(const char *symbol) {
  /*
   * LLVM runtime symbols are registered under their declared public names
   * in ny_jit_add_runtime_symbols().  Resolving a missing qualified name by
   * stripping its module prefix can bind an unrelated process symbol with
   * the same suffix, so keep lookup exact.
   */
  return ny_jit_resolve_symbol(symbol);
}

static LLVMValueRef find_unique_function_suffix(LLVMModuleRef mod,
                                                const char *symbol) {
  if (!mod || !symbol || strncmp(symbol, "ny_fn_", 6) != 0)
    return NULL;
  const char *short_name = symbol + 6;
  size_t short_len = strlen(short_name);
  LLVMValueRef found = NULL;
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    const char *candidate = LLVMGetValueName(f);
    if (!candidate || strncmp(candidate, "ny_fn_", 6) != 0 ||
        !LLVMGetFirstBasicBlock(f))
      continue;
    candidate += 6;
    size_t candidate_len = strlen(candidate);
    if (candidate_len <= short_len ||
        candidate[candidate_len - short_len - 1] != '.' ||
        strcmp(candidate + candidate_len - short_len, short_name) != 0)
      continue;
    if (found)
      return NULL;
    found = f;
  }
  return found;
}

void ny_jit_canonicalize_internal_calls(LLVMModuleRef mod) {
  if (!mod)
    return;
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;) {
    LLVMValueRef next = LLVMGetNextFunction(f);
    if (!LLVMGetFirstBasicBlock(f)) {
      const char *name = LLVMGetValueName(f);
      LLVMValueRef alias = find_unique_function_suffix(mod, name);
      if (alias) {
        LLVMReplaceAllUsesWith(f, alias);
        LLVMDeleteFunction(f);
      }
    }
    f = next;
  }
}

void ny_jit_map_unresolved_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                   const char *entry_name) {
  if (!ee || !mod)
    return;
  if (verbose_enabled >= 3)
    NY_LOG_V3("JIT: mapping unresolved module symbols\n");
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
    NY_LOG_V3("JIT: mapping unresolved module functions\n");
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;) {
    LLVMValueRef next = LLVMGetNextFunction(f);
    const char *name = LLVMGetValueName(f);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0) {
      f = next;
      continue;
    }
    bool has_body = LLVMGetFirstBasicBlock(f) != NULL;
    if (has_body && (!entry_name || strcmp(name, entry_name) != 0)) {
      f = next;
      continue;
    }
    LLVMValueRef alias =
        !has_body ? find_unique_function_suffix(mod, name) : NULL;
    if (alias) {
      void *alias_addr = LLVMGetPointerToGlobal(ee, alias);
      if (alias_addr) {
        LLVMAddGlobalMapping(ee, f, alias_addr);
        if (verbose_enabled >= 3)
          fprintf(stderr, "JIT: mapped alias '%s' to '%s' at %p\n", name,
                  LLVMGetValueName(alias), alias_addr);
        f = next;
        continue;
      }
    }
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
    f = next;
  }
}

void ny_jit_add_runtime_symbols(void) {
  static bool registered = false;
  if (registered)
    return;
  registered = true;
#define RT_DEF(name, p, args, sig, doc)                                        \
  do {                                                                         \
    (void)(args);                                                              \
    (void)(sig);                                                               \
    (void)(doc);                                                               \
    LLVMAddSymbol(name, (void *)(uintptr_t)p);                                 \
    if (strcmp(name, #p) != 0)                                                 \
      LLVMAddSymbol(#p, (void *)(uintptr_t)p);                                 \
  } while (0);
#define RT_GV(name, p, t, doc)                                                 \
  do {                                                                         \
    (void)(doc);                                                               \
    LLVMAddSymbol(name, (void *)&p);                                           \
  } while (0);
#include "code/lang/runtime/defs.h"
#undef RT_DEF
#undef RT_GV
  /*
   * core.reflect.globals is a language wrapper around this exact runtime
   * getter. Some imported-module JIT paths retain the qualified function
   * symbol instead of lowering the wrapper body into the current module.
   */
  LLVMAddSymbol("ny_fn_core.reflect.globals",
                (void *)(uintptr_t)rt_globals_get);
  /*
   * core.os.file_exists is a stdlib wrapper over the raw path bridge. Some
   * imported-module JIT paths keep its qualified declaration instead of
   * lowering the wrapper body into the current module.
   */
  LLVMAddSymbol("ny_fn_core.os.file_exists",
                (void *)(uintptr_t)rt_file_exists_raw);
  /*
   * core.os.args.args wraps the raw argv snapshot. Imported stdlib wrappers
   * such as flag() can retain this qualified symbol in JIT modules.
   */
  LLVMAddSymbol("ny_fn_core.os.args.args", (void *)(uintptr_t)rt_args_raw);
  LLVMAddSymbol("rt_sqrt_f64", (void *)(uintptr_t)rt_sqrt_f64);
  LLVMAddSymbol("rt_f64_bits", (void *)(uintptr_t)rt_f64_bits);
  LLVMAddSymbol("rt_type_name", (void *)(uintptr_t)rt_type_name);
  LLVMAddSymbol("rt_type_name_tagged", (void *)(uintptr_t)rt_type_name_tagged);
  LLVMAddSymbol("rt_load32_f64", (void *)(uintptr_t)rt_load32_f64);
  LLVMAddSymbol("rt_store32_f64", (void *)(uintptr_t)rt_store32_f64);
  LLVMAddSymbol("rt_simmd_byte_class_reduce_raw",
                (void *)(uintptr_t)rt_simmd_byte_class_reduce_raw);
  LLVMAddSymbol("rt_simmd_i32_sqlscan_sum_raw",
                (void *)(uintptr_t)rt_simmd_i32_sqlscan_sum_raw);
  LLVMAddSymbol("__alloc_string", (void *)(uintptr_t)rt_alloc_string);
  LLVMAddSymbol("rt_bigint_from_i64_raw",
                (void *)(uintptr_t)rt_bigint_from_i64_raw);
  LLVMAddSymbol("rt_bigint_to_i64_raw",
                (void *)(uintptr_t)rt_bigint_to_i64_raw);
  LLVMAddSymbol("rt_bigint_to_int", (void *)(uintptr_t)rt_bigint_to_int);
  LLVMAddSymbol("rt_bigint_add", (void *)(uintptr_t)rt_bigint_add);
  LLVMAddSymbol("rt_bigint_sub", (void *)(uintptr_t)rt_bigint_sub);
  LLVMAddSymbol("rt_bigint_mul", (void *)(uintptr_t)rt_bigint_mul);
  LLVMAddSymbol("rt_bigint_div", (void *)(uintptr_t)rt_bigint_div);
  LLVMAddSymbol("rt_bigint_mod", (void *)(uintptr_t)rt_bigint_mod);
  LLVMAddSymbol("rt_bigint_pow", (void *)(uintptr_t)rt_bigint_pow);
  LLVMAddSymbol("rt_bigint_xor", (void *)(uintptr_t)rt_bigint_xor);
  LLVMAddSymbol("rt_native_has_tag", (void *)(uintptr_t)rt_native_has_tag);
  LLVMAddSymbol("rt_value_tag", (void *)(uintptr_t)rt_value_tag);
  LLVMAddSymbol("rt_result_unwrap_raw",
                (void *)(uintptr_t)rt_result_unwrap_raw);
  LLVMAddSymbol("rt_result_unwrap_or_any",
                (void *)(uintptr_t)rt_result_unwrap_or_any);
  LLVMAddSymbol("rt_result_unwrap_or_raw",
                (void *)(uintptr_t)rt_result_unwrap_or_raw);
  LLVMAddSymbol("rt_runtime_tag_raw_value",
                (void *)(uintptr_t)rt_runtime_tag_raw_value);
  LLVMAddSymbol("rt_dict_keys_raw", (void *)(uintptr_t)rt_dict_keys_raw);
  LLVMAddSymbol("rt_dict_values_raw", (void *)(uintptr_t)rt_dict_values_raw);
  LLVMAddSymbol("rt_dict_items_raw", (void *)(uintptr_t)rt_dict_items_raw);
  LLVMAddSymbol("rt_bytes_get_raw", (void *)(uintptr_t)rt_bytes_get_raw);
  LLVMAddSymbol("rt_bytes_new_raw", (void *)(uintptr_t)rt_bytes_new_raw);
  LLVMAddSymbol("rt_realloc_raw", (void *)(uintptr_t)rt_realloc_raw);
  LLVMAddSymbol("rt_bytes_set_raw", (void *)(uintptr_t)rt_bytes_set_raw);
  LLVMAddSymbol("malloc", (void *)(uintptr_t)malloc);
  LLVMAddSymbol("free", (void *)(uintptr_t)free);
  LLVMAddSymbol("calloc", (void *)(uintptr_t)calloc);
  LLVMAddSymbol("realloc", (void *)(uintptr_t)realloc);
  LLVMAddSymbol("memcpy", (void *)(uintptr_t)memcpy);
  LLVMAddSymbol("memmove", (void *)(uintptr_t)memmove);
  LLVMAddSymbol("memset", (void *)(uintptr_t)memset);
  LLVMAddSymbol("memcmp", (void *)(uintptr_t)memcmp);
  LLVMAddSymbol("strcmp", (void *)(uintptr_t)strcmp);
  LLVMAddSymbol("strchr", (void *)(uintptr_t)strchr);
  LLVMAddSymbol("rt_bigint_cmp_raw", (void *)(uintptr_t)rt_bigint_cmp_raw);
  LLVMAddSymbol("rt_bigint_bitlen_raw",
                (void *)(uintptr_t)rt_bigint_bitlen_raw);
  LLVMAddSymbol("rt_bigfloat_from_value_raw",
                (void *)(uintptr_t)rt_bigfloat_from_value_raw);
  LLVMAddSymbol("rt_bigfloat_from_f64_bits",
                (void *)(uintptr_t)rt_bigfloat_from_f64_bits);
  LLVMAddSymbol("rt_bigfloat_from_f64",
                (void *)(uintptr_t)rt_bigfloat_from_f64);
  LLVMAddSymbol("rt_bigfloat_to_f64_raw",
                (void *)(uintptr_t)rt_bigfloat_to_f64_raw);
  LLVMAddSymbol("rt_complex_re_f64", (void *)(uintptr_t)rt_complex_re_f64);
  LLVMAddSymbol("rt_complex_im_f64", (void *)(uintptr_t)rt_complex_im_f64);
  LLVMAddSymbol("rt_bigfloat_cmp_raw", (void *)(uintptr_t)rt_bigfloat_cmp_raw);
  LLVMAddSymbol("rt_bigfloat_precision_raw",
                (void *)(uintptr_t)rt_bigfloat_precision_raw);
  LLVMAddSymbol("rt_bigfloat_pow_int_raw",
                (void *)(uintptr_t)rt_bigfloat_pow_int_raw);
  LLVMAddSymbol("rt_bigfloat_zero", (void *)(uintptr_t)rt_bigfloat_zero);
  LLVMAddSymbol("rt_bigfloat_one", (void *)(uintptr_t)rt_bigfloat_one);
  LLVMAddSymbol("rt_bigfloat_add", (void *)(uintptr_t)rt_bigfloat_add);
  LLVMAddSymbol("rt_bigfloat_sub", (void *)(uintptr_t)rt_bigfloat_sub);
  LLVMAddSymbol("rt_bigfloat_mul", (void *)(uintptr_t)rt_bigfloat_mul);
  LLVMAddSymbol("rt_bigfloat_div", (void *)(uintptr_t)rt_bigfloat_div);
  LLVMAddSymbol("rt_bigfloat_neg", (void *)(uintptr_t)rt_bigfloat_neg);
  LLVMAddSymbol("rt_bigfloat_abs", (void *)(uintptr_t)rt_bigfloat_abs);
  LLVMAddSymbol("rt_bigfloat_sqrt", (void *)(uintptr_t)rt_bigfloat_sqrt);
  LLVMAddSymbol("rt_bigfloat_is", (void *)(uintptr_t)rt_bigfloat_is);
  LLVMAddSymbol("rt_bigfloat_to_str", (void *)(uintptr_t)rt_bigfloat_to_str);
  LLVMAddSymbol("rt_f64_to_i64", (void *)(uintptr_t)rt_f64_to_i64);
  LLVMAddSymbol("rt_tbuf_set_f64_bits",
                (void *)(uintptr_t)rt_tbuf_set_f64_bits);
  LLVMAddSymbol("rt_sin_f64", (void *)(uintptr_t)rt_sin_f64);
  LLVMAddSymbol("rt_cos_f64", (void *)(uintptr_t)rt_cos_f64);
  LLVMAddSymbol("rt_f64_round", (void *)(uintptr_t)rt_f64_round);
  LLVMAddSymbol("rt_f64_pow", (void *)(uintptr_t)rt_f64_pow);
  LLVMAddSymbol("rt_f64_floor", (void *)(uintptr_t)rt_f64_floor);
  LLVMAddSymbol("rt_f64_ceil", (void *)(uintptr_t)rt_f64_ceil);
  LLVMAddSymbol("rt_tag_or_raw_int", (void *)(uintptr_t)rt_tag_or_raw_int);
  LLVMAddSymbol("rt_box_i64", (void *)(uintptr_t)rt_box_i64);
  LLVMAddSymbol("rt_bounds_check", (void *)(uintptr_t)rt_bounds_check);
  LLVMAddSymbol("rt_bounds_trap", (void *)(uintptr_t)rt_bounds_trap);
  LLVMAddSymbol("rt_fail", (void *)(uintptr_t)rt_fail);
  LLVMAddSymbol("rt_trap", (void *)(uintptr_t)rt_trap);
  LLVMAddSymbol("rt_tbuf_new_raw", (void *)(uintptr_t)rt_tbuf_new_raw);
  LLVMAddSymbol("rt_tbuf_new_typed_raw",
                (void *)(uintptr_t)rt_tbuf_new_typed_raw);
  LLVMAddSymbol("rt_atoi", (void *)(uintptr_t)rt_atoi);
  LLVMAddSymbol("rt_atof", (void *)(uintptr_t)rt_atof);
  LLVMAddSymbol("rt_tbuf_get", (void *)(uintptr_t)rt_tbuf_get);
  LLVMAddSymbol("rt_tbuf_get", (void *)(uintptr_t)rt_tbuf_get);
  LLVMAddSymbol("rt_tbuf_index_read_raw",
                (void *)(uintptr_t)rt_tbuf_index_read_raw);
  LLVMAddSymbol("rt_tbuf_direct_elem8_ptr_raw",
                (void *)(uintptr_t)rt_tbuf_direct_elem8_ptr_raw);
  LLVMAddSymbol("rt_tbuf_index_f64", (void *)(uintptr_t)rt_tbuf_index_f64);
  LLVMAddSymbol("rt_long_list_raw", (void *)(uintptr_t)rt_long_list_raw);
  LLVMAddSymbol("rt_tbuf_index_any", (void *)(uintptr_t)rt_tbuf_index_any);
  LLVMAddSymbol("rt_tbuf_index_any_raw",
                (void *)(uintptr_t)rt_tbuf_index_any_raw);
  LLVMAddSymbol("rt_tbuf_get_any", (void *)(uintptr_t)rt_tbuf_get_any);
  LLVMAddSymbol("rt_index_key_error", (void *)(uintptr_t)rt_index_key_error);
  LLVMAddSymbol("rt_bytes_index_read_raw",
                (void *)(uintptr_t)rt_bytes_index_read_raw);
  LLVMAddSymbol("rt_range_index_read_raw",
                (void *)(uintptr_t)rt_range_index_read_raw);
  LLVMAddSymbol("rt_cstr_index_read_raw",
                (void *)(uintptr_t)rt_cstr_index_read_raw);
  LLVMAddSymbol("rt_value_is_ptr", (void *)(uintptr_t)rt_value_is_ptr);
  LLVMAddSymbol("rt_native_is_ptr", (void *)(uintptr_t)rt_native_is_ptr);
  LLVMAddSymbol("rt_vec_dot_raw", (void *)(uintptr_t)rt_vec_dot_raw);
  LLVMAddSymbol("rt_vec_add_raw", (void *)(uintptr_t)rt_vec_add_raw);
  LLVMAddSymbol("rt_vec_sub_raw", (void *)(uintptr_t)rt_vec_sub_raw);
  LLVMAddSymbol("rt_vec_div_component_raw",
                (void *)(uintptr_t)rt_vec_div_component_raw);
  LLVMAddSymbol("rt_vec_mul_scalar_raw",
                (void *)(uintptr_t)rt_vec_mul_scalar_raw);
  LLVMAddSymbol("rt_vec_div_scalar_raw",
                (void *)(uintptr_t)rt_vec_div_scalar_raw);
  LLVMAddSymbol("rt_any_mul", (void *)(uintptr_t)rt_any_mul);
  LLVMAddSymbol("rt_any_div", (void *)(uintptr_t)rt_any_div);
  LLVMAddSymbol("rt_any_mod", (void *)(uintptr_t)rt_any_mod);
  LLVMAddSymbol("rt_tbuf_tag", (void *)(uintptr_t)rt_tbuf_tag);
  LLVMAddSymbol("rt_zalloc_raw", (void *)(uintptr_t)rt_zalloc_raw);
  LLVMAddSymbol("rt_malloc_i64", (void *)(uintptr_t)rt_malloc_i64);
  LLVMAddSymbol("rt_malloc_raw_i64", (void *)(uintptr_t)rt_malloc_raw_i64);
  LLVMAddSymbol("rt_zfree_raw", (void *)(uintptr_t)rt_zfree_raw);
  LLVMAddSymbol("rt_list_new_raw", (void *)(uintptr_t)rt_list_new_raw);
  LLVMAddSymbol("rt_list_new_sized", (void *)(uintptr_t)rt_list_new_sized);
  LLVMAddSymbol("rt_list_new_sized_uninit",
                (void *)(uintptr_t)rt_list_new_sized_uninit);
  LLVMAddSymbol("rt_simmd_popcnt64_i64",
                (void *)(uintptr_t)rt_simmd_popcnt64_i64);
  LLVMAddSymbol("rt_simmd_ctz64_i64", (void *)(uintptr_t)rt_simmd_ctz64_i64);
  LLVMAddSymbol("rt_simmd_clz64_i64", (void *)(uintptr_t)rt_simmd_clz64_i64);
  LLVMAddSymbol("rt_simmd_bswap64_i64",
                (void *)(uintptr_t)rt_simmd_bswap64_i64);
  LLVMAddSymbol("rt_simmd_popcnt32_i64",
                (void *)(uintptr_t)rt_simmd_popcnt32_i64);
  LLVMAddSymbol("rt_simmd_ctz32_i64", (void *)(uintptr_t)rt_simmd_ctz32_i64);
  LLVMAddSymbol("rt_simmd_clz32_i64", (void *)(uintptr_t)rt_simmd_clz32_i64);
  LLVMAddSymbol("rt_simmd_bswap32_i64",
                (void *)(uintptr_t)rt_simmd_bswap32_i64);
  LLVMAddSymbol("rt_simmd_rotl32_i64", (void *)(uintptr_t)rt_simmd_rotl32_i64);
  LLVMAddSymbol("rt_simmd_rotr32_i64", (void *)(uintptr_t)rt_simmd_rotr32_i64);
  LLVMAddSymbol("rt_simmd_rotl64_i64", (void *)(uintptr_t)rt_simmd_rotl64_i64);
  LLVMAddSymbol("rt_simmd_rotr64_i64", (void *)(uintptr_t)rt_simmd_rotr64_i64);
  LLVMAddSymbol("rt_tbuf_append_raw", (void *)(uintptr_t)rt_tbuf_append_raw);
  LLVMAddSymbol("rt_tbuf_append_tagged",
                (void *)(uintptr_t)rt_tbuf_append_tagged);
  LLVMAddSymbol("rt_tbuf_append_i64_raw",
                (void *)(uintptr_t)rt_tbuf_append_i64_raw);
  LLVMAddSymbol("rt_tbuf_len_raw", (void *)(uintptr_t)rt_tbuf_len_raw);
  LLVMAddSymbol("rt_tbuf_clear_raw", (void *)(uintptr_t)rt_tbuf_clear_raw);
  LLVMAddSymbol("rt_any_eq", (void *)(uintptr_t)rt_any_eq);
  LLVMAddSymbol("rt_any_add", (void *)(uintptr_t)rt_any_add);
  LLVMAddSymbol("rt_raw_add", (void *)(uintptr_t)rt_raw_add);
  LLVMAddSymbol("rt_ptr_add_i64", (void *)(uintptr_t)rt_ptr_add_i64);
  LLVMAddSymbol("rt_ptr_sub_i64", (void *)(uintptr_t)rt_ptr_sub_i64);
  LLVMAddSymbol("rt_any_to_i64", (void *)(uintptr_t)rt_any_to_i64);
  LLVMAddSymbol("rt_native_is_int", (void *)(uintptr_t)rt_native_is_int);
  LLVMAddSymbol("rt_native_is_str", (void *)(uintptr_t)rt_native_is_str);
  LLVMAddSymbol("rt_is_str", (void *)(uintptr_t)rt_is_str);
  LLVMAddSymbol("rt_bigint_neg_raw", (void *)(uintptr_t)rt_bigint_neg_raw);
  LLVMAddSymbol("rt_tbuf_clone_raw", (void *)(uintptr_t)rt_tbuf_clone_raw);
  LLVMAddSymbol("rt_tbuf_swap", (void *)(uintptr_t)rt_tbuf_swap);
  LLVMAddSymbol("rt_tbuf_pop_raw", (void *)(uintptr_t)rt_tbuf_pop_raw);
  LLVMAddSymbol("rt_tbuf_set_tagged", (void *)(uintptr_t)rt_tbuf_set_tagged);
  LLVMAddSymbol("rt_tbuf_set_i64_raw", (void *)(uintptr_t)rt_tbuf_set_i64_raw);
  LLVMAddSymbol("rt_range_values_raw", (void *)(uintptr_t)rt_range_values_raw);
  LLVMAddSymbol("rt_tbuf_to_cstr", (void *)(uintptr_t)rt_tbuf_to_cstr);
  LLVMAddSymbol("rt_shl_raw", (void *)(uintptr_t)rt_shl_raw);
  LLVMAddSymbol("rt_bshr_raw", (void *)(uintptr_t)rt_bshr_raw);
  LLVMAddSymbol("rt_cstr_repeat", (void *)(uintptr_t)rt_cstr_repeat);
  LLVMAddSymbol("rt_cstr_slice", (void *)(uintptr_t)rt_cstr_slice);
  LLVMAddSymbol("rt_load8_raw", (void *)(uintptr_t)rt_load8_raw);
  LLVMAddSymbol("rt_store8_raw", (void *)(uintptr_t)rt_store8_raw);
  LLVMAddSymbol("rt_cstr_len", (void *)(uintptr_t)rt_cstr_len);
  LLVMAddSymbol("rt_cstr_get_raw", (void *)(uintptr_t)rt_cstr_get_raw);
  LLVMAddSymbol("rt_len", (void *)(uintptr_t)rt_len);
  LLVMAddSymbol("rt_len_strict", (void *)(uintptr_t)rt_len_strict);
  LLVMAddSymbol("rt_raw_word_tag", (void *)(uintptr_t)rt_raw_word_tag);
  LLVMAddSymbol("rt_tbuf_dyn_elem", (void *)(uintptr_t)rt_tbuf_dyn_elem);
  LLVMAddSymbol("rt_tbuf_extend", (void *)(uintptr_t)rt_tbuf_extend);
  LLVMAddSymbol("rt_tbuf_repeat", (void *)(uintptr_t)rt_tbuf_repeat);
  LLVMAddSymbol("rt_adt_alloc", (void *)(uintptr_t)rt_adt_alloc);
  LLVMAddSymbol("rt_adt_tag", (void *)(uintptr_t)rt_adt_tag);
  LLVMAddSymbol("rt_getlogin", (void *)(uintptr_t)rt_getlogin);
  LLVMAddSymbol("rt_gettimeofday", (void *)(uintptr_t)rt_gettimeofday);
  LLVMAddSymbol("rt_sequence_len_raw", (void *)(uintptr_t)rt_sequence_len_raw);
  LLVMAddSymbol("rt_cstr_builder_new", (void *)(uintptr_t)rt_cstr_builder_new);
  LLVMAddSymbol("rt_cstr_builder_append",
                (void *)(uintptr_t)rt_cstr_builder_append);
  LLVMAddSymbol("rt_cstr_builder_finalize",
                (void *)(uintptr_t)rt_cstr_builder_finalize);
  LLVMAddSymbol("rt_cstr_replace", (void *)(uintptr_t)rt_cstr_replace);
  LLVMAddSymbol("rt_bool_to_cstr", (void *)(uintptr_t)rt_bool_to_cstr);
  LLVMAddSymbol("rt_tbuf_concat_raw", (void *)(uintptr_t)rt_tbuf_concat_raw);
  LLVMAddSymbol("rt_tbuf_slice", (void *)(uintptr_t)rt_tbuf_slice);
  LLVMAddSymbol("rt_value_get_tagged", (void *)(uintptr_t)rt_value_get_tagged);
  LLVMAddSymbol("rt_value_get_index_raw",
                (void *)(uintptr_t)rt_value_get_index_raw);
  LLVMAddSymbol("rt_load_item_any", (void *)(uintptr_t)rt_load_item_any);
  LLVMAddSymbol("rt_any_eq", (void *)(uintptr_t)rt_any_eq);
  LLVMAddSymbol("rt_dict_new_raw", (void *)(uintptr_t)rt_dict_new_raw);
  LLVMAddSymbol("rt_dict_word_frequency_raw",
                (void *)(uintptr_t)rt_dict_word_frequency_raw);
  LLVMAddSymbol("rt_set_new", (void *)(uintptr_t)rt_set_new);
  LLVMAddSymbol("rt_set_from_sequence",
                (void *)(uintptr_t)rt_set_from_sequence);
  LLVMAddSymbol("rt_set_add_raw", (void *)(uintptr_t)rt_set_add_raw);
  LLVMAddSymbol("rt_set_remove", (void *)(uintptr_t)rt_set_remove);
  LLVMAddSymbol("rt_dict_get_raw", (void *)(uintptr_t)rt_dict_get_raw);
  LLVMAddSymbol("rt_dict_get_i64_raw", (void *)(uintptr_t)rt_dict_get_i64_raw);
  LLVMAddSymbol("rt_value_set_tagged", (void *)(uintptr_t)rt_value_set_tagged);
  LLVMAddSymbol("rt_call_any1", (void *)(uintptr_t)rt_call_any1);
  LLVMAddSymbol("rt_call_any2", (void *)(uintptr_t)rt_call_any2);
  LLVMAddSymbol("rt_call_any3", (void *)(uintptr_t)rt_call_any3);
  LLVMAddSymbol("rt_call_any4", (void *)(uintptr_t)rt_call_any4);
  LLVMAddSymbol("rt_call_any5", (void *)(uintptr_t)rt_call_any5);
  LLVMAddSymbol("rt_call_any6", (void *)(uintptr_t)rt_call_any6);
  LLVMAddSymbol("rt_call_any7", (void *)(uintptr_t)rt_call_any7);
  LLVMAddSymbol("rt_call_any8", (void *)(uintptr_t)rt_call_any8);
  LLVMAddSymbol("rt_call_any9", (void *)(uintptr_t)rt_call_any9);
  LLVMAddSymbol("rt_call_any10", (void *)(uintptr_t)rt_call_any10);
  LLVMAddSymbol("rt_call_any11", (void *)(uintptr_t)rt_call_any11);
  LLVMAddSymbol("rt_call_any12", (void *)(uintptr_t)rt_call_any12);
  LLVMAddSymbol("rt_call_any13", (void *)(uintptr_t)rt_call_any13);
  LLVMAddSymbol("rt_call_any14", (void *)(uintptr_t)rt_call_any14);
  LLVMAddSymbol("rt_call_any15", (void *)(uintptr_t)rt_call_any15);
  LLVMAddSymbol("rt_dict_set_raw", (void *)(uintptr_t)rt_dict_set_raw);
  LLVMAddSymbol("rt_dict_set_i64_raw", (void *)(uintptr_t)rt_dict_set_i64_raw);
  LLVMAddSymbol("rt_native_dict_set_raw_i64",
                (void *)(uintptr_t)rt_native_dict_set_raw_i64);
  LLVMAddSymbol("rt_native_dict_set_native_i64",
                (void *)(uintptr_t)rt_native_dict_set_native_i64);
  LLVMAddSymbol("rt_native_dict_set_fast_i64",
                (void *)(uintptr_t)rt_native_dict_set_fast_i64);
  LLVMAddSymbol("rt_native_dict_set_nir_i64",
                (void *)(uintptr_t)rt_native_dict_set_nir_i64);
  LLVMAddSymbol("rt_dict_merge_raw", (void *)(uintptr_t)rt_dict_merge_raw);
  LLVMAddSymbol("rt_dict_clear_raw", (void *)(uintptr_t)rt_dict_clear_raw);
  LLVMAddSymbol("rt_dict_get_str_raw", (void *)(uintptr_t)rt_dict_get_str_raw);
  LLVMAddSymbol("rt_dict_set_str_raw", (void *)(uintptr_t)rt_dict_set_str_raw);
  LLVMAddSymbol("rt_native_dict_set_str_compact",
                (void *)(uintptr_t)rt_native_dict_set_str_compact);
  LLVMAddSymbol("rt_dict_has_str_raw", (void *)(uintptr_t)rt_dict_has_str_raw);
  LLVMAddSymbol("rt_dict_delete_str_raw",
                (void *)(uintptr_t)rt_dict_delete_str_raw);
  LLVMAddSymbol("rt_dict_has_raw", (void *)(uintptr_t)rt_dict_has_raw);
  LLVMAddSymbol("rt_dict_len_raw", (void *)(uintptr_t)rt_dict_len_raw);
  LLVMAddSymbol("rt_dict_delete_raw", (void *)(uintptr_t)rt_dict_delete_raw);
  LLVMAddSymbol("rt_dict_merge_raw", (void *)(uintptr_t)rt_dict_merge_raw);
  LLVMAddSymbol("rt_dict_clone_raw", (void *)(uintptr_t)rt_dict_clone_raw);
  LLVMAddSymbol("rt_dict_clear_raw", (void *)(uintptr_t)rt_dict_clear_raw);
  LLVMAddSymbol("rt_i64_min", (void *)(uintptr_t)rt_i64_min);
  LLVMAddSymbol("rt_i64_max", (void *)(uintptr_t)rt_i64_max);
  LLVMAddSymbol("rt_f64_min", (void *)(uintptr_t)rt_f64_min);
  LLVMAddSymbol("rt_f64_max", (void *)(uintptr_t)rt_f64_max);
  LLVMAddSymbol("rt_any_to_f64", (void *)(uintptr_t)rt_any_to_f64);
  LLVMAddSymbol("rt_fmod_f64", (void *)(uintptr_t)rt_fmod_f64);
#ifndef _WIN32
  void *setjmp_ptr = dlsym(RTLD_DEFAULT, "_setjmp");
  if (setjmp_ptr)
    LLVMAddSymbol("_setjmp", setjmp_ptr);
#endif
#ifdef _WIN32
  LLVMAddSymbol("snprintf", (void *)(uintptr_t)ny_jit_snprintf);
  LLVMAddSymbol("_snprintf", (void *)(uintptr_t)ny_jit_snprintf);
  LLVMAddSymbol("optind", &ny_jit_optind);
  LLVMAddSymbol("_optind", &ny_jit_optind);
#endif
}

static void ny_jit_define_runtime_trampoline(LLVMModuleRef mod,
                                             const char *name, void *ptr) {
  if (!mod || !name || !*name || !ptr)
    return;
  /*
   * setjmp/longjmp must save and restore the frame that contains the Nytrix
   * try body.  A normal JIT trampoline would save the trampoline's frame and
   * then return from it before rt_panic longjmps, which is undefined behavior.
   * Leave these as declarations and bind them directly through the execution
   * engine/symbol table instead.
   */
  if (strcmp(name, "_setjmp") == 0 || strcmp(name, "setjmp") == 0)
    return;
  LLVMValueRef f = LLVMGetNamedFunction(mod, name);
  if (!f)
    return;
  /*
   * Cached LLVM modules may contain a trampoline materialized by an earlier
   * compiler process.  Its embedded address is process-local and is invalid
   * here.  Replace the whole function so every runtime entry is rebuilt from
   * the current process address instead of retaining a stale body.
   */
  if (!LLVMIsDeclaration(f)) {
    LLVMTypeRef old_ty = LLVMGlobalGetValueType(f);
    LLVMValueRef fresh = LLVMAddFunction(mod, "", old_ty);
    if (!fresh)
      return;
    LLVMReplaceAllUsesWith(f, fresh);
    LLVMDeleteFunction(f);
    LLVMSetValueName2(fresh, name, strlen(name));
    f = fresh;
  }
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

  LLVMValueRef addr = LLVMConstInt(LLVMInt64TypeInContext(ctx),
                                   (uint64_t)(uintptr_t)ptr, false);
  LLVMValueRef callee = LLVMBuildIntToPtr(b, addr, LLVMTypeOf(f), "ct_rt_fn");
  LLVMTypeRef retty = LLVMGetReturnType(fty);
  if (LLVMGetTypeKind(retty) == LLVMVoidTypeKind) {
    (void)LLVMBuildCall2(b, fty, callee, params, argc, "");
    LLVMBuildRetVoid(b);
  } else {
    LLVMValueRef ret =
        LLVMBuildCall2(b, fty, callee, params, argc, "ct_rt_result");
    LLVMBuildRet(b, ret);
  }
  LLVMDisposeBuilder(b);
}

void ny_jit_define_runtime_trampolines(LLVMModuleRef mod) {
#define RT_DEF(name, p, args, sig, doc)                                        \
  do {                                                                         \
    (void)(args);                                                              \
    (void)(sig);                                                               \
    (void)(doc);                                                               \
    ny_jit_define_runtime_trampoline(mod, name, (void *)(uintptr_t)p);         \
    if (strcmp(name, #p) != 0)                                                 \
      ny_jit_define_runtime_trampoline(mod, #p, (void *)(uintptr_t)p);         \
  } while (0);
#define RT_GV(name, p, t, doc)                                                 \
  do {                                                                         \
    (void)(name);                                                              \
    (void)(p);                                                                 \
    (void)(doc);                                                               \
  } while (0);
#include "code/lang/runtime/defs.h"
#undef RT_DEF
#undef RT_GV
  /*
   * Keep the qualified stdlib declaration resolvable in ORC as well as
   * MCJIT when the imported wrapper body is omitted from the module.
   */
  ny_jit_define_runtime_trampoline(mod, "ny_fn_core.os.file_exists",
                                   (void *)(uintptr_t)rt_file_exists_raw);
  ny_jit_define_runtime_trampoline(mod, "ny_fn_core.os.args.args",
                                   (void *)(uintptr_t)rt_args_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_sqrt_f64",
                                   (void *)(uintptr_t)rt_sqrt_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_bits",
                                   (void *)(uintptr_t)rt_f64_bits);
  ny_jit_define_runtime_trampoline(mod, "rt_type_name",
                                   (void *)(uintptr_t)rt_type_name);
  ny_jit_define_runtime_trampoline(mod, "rt_type_name_tagged",
                                   (void *)(uintptr_t)rt_type_name_tagged);
  ny_jit_define_runtime_trampoline(mod, "rt_load32_f64",
                                   (void *)(uintptr_t)rt_load32_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_store32_f64",
                                   (void *)(uintptr_t)rt_store32_f64);
  ny_jit_define_runtime_trampoline(
      mod, "rt_simmd_byte_class_reduce_raw",
      (void *)(uintptr_t)rt_simmd_byte_class_reduce_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_simmd_i32_sqlscan_sum_raw",
      (void *)(uintptr_t)rt_simmd_i32_sqlscan_sum_raw);
  ny_jit_define_runtime_trampoline(mod, "__alloc_string",
                                   (void *)(uintptr_t)rt_alloc_string);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_from_i64_raw",
                                   (void *)(uintptr_t)rt_bigint_from_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_to_i64_raw",
                                   (void *)(uintptr_t)rt_bigint_to_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_to_int",
                                   (void *)(uintptr_t)rt_bigint_to_int);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_add",
                                   (void *)(uintptr_t)rt_bigint_add);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_sub",
                                   (void *)(uintptr_t)rt_bigint_sub);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_mul",
                                   (void *)(uintptr_t)rt_bigint_mul);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_div",
                                   (void *)(uintptr_t)rt_bigint_div);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_mod",
                                   (void *)(uintptr_t)rt_bigint_mod);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_pow",
                                   (void *)(uintptr_t)rt_bigint_pow);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_xor",
                                   (void *)(uintptr_t)rt_bigint_xor);
  ny_jit_define_runtime_trampoline(mod, "rt_native_has_tag",
                                   (void *)(uintptr_t)rt_native_has_tag);
  ny_jit_define_runtime_trampoline(mod, "rt_value_tag",
                                   (void *)(uintptr_t)rt_value_tag);
  ny_jit_define_runtime_trampoline(mod, "rt_result_unwrap_raw",
                                   (void *)(uintptr_t)rt_result_unwrap_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_result_unwrap_or_any",
                                   (void *)(uintptr_t)rt_result_unwrap_or_any);
  ny_jit_define_runtime_trampoline(mod, "rt_result_unwrap_or_raw",
                                   (void *)(uintptr_t)rt_result_unwrap_or_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_runtime_tag_raw_value",
                                   (void *)(uintptr_t)rt_runtime_tag_raw_value);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_keys_raw",
                                   (void *)(uintptr_t)rt_dict_keys_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_values_raw",
                                   (void *)(uintptr_t)rt_dict_values_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_items_raw",
                                   (void *)(uintptr_t)rt_dict_items_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bytes_get_raw",
                                   (void *)(uintptr_t)rt_bytes_get_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bytes_new_raw",
                                   (void *)(uintptr_t)rt_bytes_new_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_realloc_raw",
                                   (void *)(uintptr_t)rt_realloc_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bytes_set_raw",
                                   (void *)(uintptr_t)rt_bytes_set_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_cmp_raw",
                                   (void *)(uintptr_t)rt_bigint_cmp_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_bitlen_raw",
                                   (void *)(uintptr_t)rt_bigint_bitlen_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_bigfloat_from_value_raw",
      (void *)(uintptr_t)rt_bigfloat_from_value_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_bigfloat_from_f64_bits",
      (void *)(uintptr_t)rt_bigfloat_from_f64_bits);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_from_f64",
                                   (void *)(uintptr_t)rt_bigfloat_from_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_to_f64_raw",
                                   (void *)(uintptr_t)rt_bigfloat_to_f64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_cmp_raw",
                                   (void *)(uintptr_t)rt_bigfloat_cmp_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_bigfloat_precision_raw",
      (void *)(uintptr_t)rt_bigfloat_precision_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_pow_int_raw",
                                   (void *)(uintptr_t)rt_bigfloat_pow_int_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_zero",
                                   (void *)(uintptr_t)rt_bigfloat_zero);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_one",
                                   (void *)(uintptr_t)rt_bigfloat_one);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_add",
                                   (void *)(uintptr_t)rt_bigfloat_add);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_sub",
                                   (void *)(uintptr_t)rt_bigfloat_sub);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_mul",
                                   (void *)(uintptr_t)rt_bigfloat_mul);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_div",
                                   (void *)(uintptr_t)rt_bigfloat_div);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_neg",
                                   (void *)(uintptr_t)rt_bigfloat_neg);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_abs",
                                   (void *)(uintptr_t)rt_bigfloat_abs);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_sqrt",
                                   (void *)(uintptr_t)rt_bigfloat_sqrt);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_is",
                                   (void *)(uintptr_t)rt_bigfloat_is);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_to_str",
                                   (void *)(uintptr_t)rt_bigfloat_to_str);
  ny_jit_define_runtime_trampoline(mod, "rt_sin_f64",
                                   (void *)(uintptr_t)rt_sin_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_cos_f64",
                                   (void *)(uintptr_t)rt_cos_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_bounds_check",
                                   (void *)(uintptr_t)rt_bounds_check);
  ny_jit_define_runtime_trampoline(mod, "rt_bounds_trap",
                                   (void *)(uintptr_t)rt_bounds_trap);
  ny_jit_define_runtime_trampoline(mod, "rt_tag_or_raw_int",
                                   (void *)(uintptr_t)rt_tag_or_raw_int);
  ny_jit_define_runtime_trampoline(mod, "rt_box_i64",
                                   (void *)(uintptr_t)rt_box_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_trap", (void *)(uintptr_t)rt_trap);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_new_raw",
                                   (void *)(uintptr_t)rt_tbuf_new_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_new_typed_raw",
                                   (void *)(uintptr_t)rt_tbuf_new_typed_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_atoi", (void *)(uintptr_t)rt_atoi);
  ny_jit_define_runtime_trampoline(mod, "rt_atof", (void *)(uintptr_t)rt_atof);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_get",
                                   (void *)(uintptr_t)rt_tbuf_get);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_get",
                                   (void *)(uintptr_t)rt_tbuf_get);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_index_read_raw",
                                   (void *)(uintptr_t)rt_tbuf_index_read_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_tbuf_direct_elem8_ptr_raw",
      (void *)(uintptr_t)rt_tbuf_direct_elem8_ptr_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_index_f64",
                                   (void *)(uintptr_t)rt_tbuf_index_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_long_list_raw",
                                   (void *)(uintptr_t)rt_long_list_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_index_any",
                                   (void *)(uintptr_t)rt_tbuf_index_any);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_index_any_raw",
                                   (void *)(uintptr_t)rt_tbuf_index_any_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_get_any",
                                   (void *)(uintptr_t)rt_tbuf_get_any);
  ny_jit_define_runtime_trampoline(mod, "rt_index_key_error",
                                   (void *)(uintptr_t)rt_index_key_error);
  ny_jit_define_runtime_trampoline(mod, "rt_bytes_index_read_raw",
                                   (void *)(uintptr_t)rt_bytes_index_read_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_range_index_read_raw",
                                   (void *)(uintptr_t)rt_range_index_read_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_index_read_raw",
                                   (void *)(uintptr_t)rt_cstr_index_read_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_value_is_ptr",
                                   (void *)(uintptr_t)rt_value_is_ptr);
  ny_jit_define_runtime_trampoline(mod, "rt_native_is_ptr",
                                   (void *)(uintptr_t)rt_native_is_ptr);
  ny_jit_define_runtime_trampoline(mod, "rt_vec_dot_raw",
                                   (void *)(uintptr_t)rt_vec_dot_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_vec_add_raw",
                                   (void *)(uintptr_t)rt_vec_add_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_vec_sub_raw",
                                   (void *)(uintptr_t)rt_vec_sub_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_vec_div_component_raw",
                                   (void *)(uintptr_t)rt_vec_div_component_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_vec_mul_scalar_raw",
                                   (void *)(uintptr_t)rt_vec_mul_scalar_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_vec_div_scalar_raw",
                                   (void *)(uintptr_t)rt_vec_div_scalar_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_any_mul",
                                   (void *)(uintptr_t)rt_any_mul);
  ny_jit_define_runtime_trampoline(mod, "rt_any_div",
                                   (void *)(uintptr_t)rt_any_div);
  ny_jit_define_runtime_trampoline(mod, "rt_any_mod",
                                   (void *)(uintptr_t)rt_any_mod);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_tag",
                                   (void *)(uintptr_t)rt_tbuf_tag);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_concat_raw",
                                   (void *)(uintptr_t)rt_tbuf_concat_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_slice",
                                   (void *)(uintptr_t)rt_tbuf_slice);
  ny_jit_define_runtime_trampoline(mod, "rt_value_get_tagged",
                                   (void *)(uintptr_t)rt_value_get_tagged);
  ny_jit_define_runtime_trampoline(mod, "rt_value_get_index_raw",
                                   (void *)(uintptr_t)rt_value_get_index_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_load_item_any",
                                   (void *)(uintptr_t)rt_load_item_any);
  ny_jit_define_runtime_trampoline(mod, "rt_any_eq",
                                   (void *)(uintptr_t)rt_any_eq);
  ny_jit_define_runtime_trampoline(mod, "rt_zalloc_raw",
                                   (void *)(uintptr_t)rt_zalloc_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_malloc_i64",
                                   (void *)(uintptr_t)rt_malloc_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_zfree_raw",
                                   (void *)(uintptr_t)rt_zfree_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_list_new_raw",
                                   (void *)(uintptr_t)rt_list_new_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_list_new_sized",
                                   (void *)(uintptr_t)rt_list_new_sized);
  ny_jit_define_runtime_trampoline(mod, "rt_list_new_sized_uninit",
                                   (void *)(uintptr_t)rt_list_new_sized_uninit);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_popcnt64_i64",
                                   (void *)(uintptr_t)rt_simmd_popcnt64_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_ctz64_i64",
                                   (void *)(uintptr_t)rt_simmd_ctz64_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_clz64_i64",
                                   (void *)(uintptr_t)rt_simmd_clz64_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_bswap64_i64",
                                   (void *)(uintptr_t)rt_simmd_bswap64_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_popcnt32_i64",
                                   (void *)(uintptr_t)rt_simmd_popcnt32_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_ctz32_i64",
                                   (void *)(uintptr_t)rt_simmd_ctz32_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_clz32_i64",
                                   (void *)(uintptr_t)rt_simmd_clz32_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_bswap32_i64",
                                   (void *)(uintptr_t)rt_simmd_bswap32_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_rotl32_i64",
                                   (void *)(uintptr_t)rt_simmd_rotl32_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_rotr32_i64",
                                   (void *)(uintptr_t)rt_simmd_rotr32_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_rotl64_i64",
                                   (void *)(uintptr_t)rt_simmd_rotl64_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_simmd_rotr64_i64",
                                   (void *)(uintptr_t)rt_simmd_rotr64_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_append_raw",
                                   (void *)(uintptr_t)rt_tbuf_append_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_append_tagged",
                                   (void *)(uintptr_t)rt_tbuf_append_tagged);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_append_i64_raw",
                                   (void *)(uintptr_t)rt_tbuf_append_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_load8_raw",
                                   (void *)(uintptr_t)rt_load8_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_store8_raw",
                                   (void *)(uintptr_t)rt_store8_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_len_raw",
                                   (void *)(uintptr_t)rt_tbuf_len_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_clear_raw",
                                   (void *)(uintptr_t)rt_tbuf_clear_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_eq_raw",
                                   (void *)(uintptr_t)rt_tbuf_eq_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_any_eq",
                                   (void *)(uintptr_t)rt_any_eq);
  ny_jit_define_runtime_trampoline(mod, "rt_any_add",
                                   (void *)(uintptr_t)rt_any_add);
  ny_jit_define_runtime_trampoline(mod, "rt_raw_add",
                                   (void *)(uintptr_t)rt_raw_add);
  ny_jit_define_runtime_trampoline(mod, "rt_ptr_add_i64",
                                   (void *)(uintptr_t)rt_ptr_add_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_ptr_sub_i64",
                                   (void *)(uintptr_t)rt_ptr_sub_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_any_to_i64",
                                   (void *)(uintptr_t)rt_any_to_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_native_is_int",
                                   (void *)(uintptr_t)rt_native_is_int);
  ny_jit_define_runtime_trampoline(mod, "rt_native_is_str",
                                   (void *)(uintptr_t)rt_native_is_str);
  ny_jit_define_runtime_trampoline(mod, "rt_is_str",
                                   (void *)(uintptr_t)rt_is_str);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_neg_raw",
                                   (void *)(uintptr_t)rt_bigint_neg_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_clone_raw",
                                   (void *)(uintptr_t)rt_tbuf_clone_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_swap",
                                   (void *)(uintptr_t)rt_tbuf_swap);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_pop_raw",
                                   (void *)(uintptr_t)rt_tbuf_pop_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_to_i64",
                                   (void *)(uintptr_t)rt_f64_to_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_round",
                                   (void *)(uintptr_t)rt_f64_round);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_pow",
                                   (void *)(uintptr_t)rt_f64_pow);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_floor",
                                   (void *)(uintptr_t)rt_f64_floor);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_ceil",
                                   (void *)(uintptr_t)rt_f64_ceil);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_set_tagged",
                                   (void *)(uintptr_t)rt_tbuf_set_tagged);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_set_i64_raw",
                                   (void *)(uintptr_t)rt_tbuf_set_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_set_f64_bits",
                                   (void *)(uintptr_t)rt_tbuf_set_f64_bits);
  ny_jit_define_runtime_trampoline(mod, "rt_range_values_raw",
                                   (void *)(uintptr_t)rt_range_values_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_to_cstr",
                                   (void *)(uintptr_t)rt_tbuf_to_cstr);
  ny_jit_define_runtime_trampoline(mod, "rt_shl_raw",
                                   (void *)(uintptr_t)rt_shl_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bshr_raw",
                                   (void *)(uintptr_t)rt_bshr_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_len",
                                   (void *)(uintptr_t)rt_cstr_len);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_get_raw",
                                   (void *)(uintptr_t)rt_cstr_get_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_len", (void *)(uintptr_t)rt_len);
  ny_jit_define_runtime_trampoline(mod, "rt_len_strict",
                                   (void *)(uintptr_t)rt_len_strict);
  ny_jit_define_runtime_trampoline(mod, "rt_raw_word_tag",
                                   (void *)(uintptr_t)rt_raw_word_tag);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_dyn_elem",
                                   (void *)(uintptr_t)rt_tbuf_dyn_elem);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_extend",
                                   (void *)(uintptr_t)rt_tbuf_extend);
  ny_jit_define_runtime_trampoline(mod, "rt_tbuf_repeat",
                                   (void *)(uintptr_t)rt_tbuf_repeat);
  ny_jit_define_runtime_trampoline(mod, "rt_adt_alloc",
                                   (void *)(uintptr_t)rt_adt_alloc);
  ny_jit_define_runtime_trampoline(mod, "rt_adt_tag",
                                   (void *)(uintptr_t)rt_adt_tag);
  ny_jit_define_runtime_trampoline(mod, "rt_getlogin",
                                   (void *)(uintptr_t)rt_getlogin);
  ny_jit_define_runtime_trampoline(mod, "rt_gettimeofday",
                                   (void *)(uintptr_t)rt_gettimeofday);
  ny_jit_define_runtime_trampoline(mod, "rt_sequence_len_raw",
                                   (void *)(uintptr_t)rt_sequence_len_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_builder_new",
                                   (void *)(uintptr_t)rt_cstr_builder_new);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_builder_append",
                                   (void *)(uintptr_t)rt_cstr_builder_append);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_builder_finalize",
                                   (void *)(uintptr_t)rt_cstr_builder_finalize);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_replace",
                                   (void *)(uintptr_t)rt_cstr_replace);
  ny_jit_define_runtime_trampoline(mod, "rt_bool_to_cstr",
                                   (void *)(uintptr_t)rt_bool_to_cstr);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_new_raw",
                                   (void *)(uintptr_t)rt_dict_new_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_dict_word_frequency_raw",
      (void *)(uintptr_t)rt_dict_word_frequency_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_set_new",
                                   (void *)(uintptr_t)rt_set_new);
  ny_jit_define_runtime_trampoline(mod, "rt_set_from_sequence",
                                   (void *)(uintptr_t)rt_set_from_sequence);
  ny_jit_define_runtime_trampoline(mod, "rt_set_add_raw",
                                   (void *)(uintptr_t)rt_set_add_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_set_remove",
                                   (void *)(uintptr_t)rt_set_remove);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_get_raw",
                                   (void *)(uintptr_t)rt_dict_get_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_get_i64_raw",
                                   (void *)(uintptr_t)rt_dict_get_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_value_set_tagged",
                                   (void *)(uintptr_t)rt_value_set_tagged);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any1",
                                   (void *)(uintptr_t)rt_call_any1);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any2",
                                   (void *)(uintptr_t)rt_call_any2);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any3",
                                   (void *)(uintptr_t)rt_call_any3);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any4",
                                   (void *)(uintptr_t)rt_call_any4);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any5",
                                   (void *)(uintptr_t)rt_call_any5);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any6",
                                   (void *)(uintptr_t)rt_call_any6);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any7",
                                   (void *)(uintptr_t)rt_call_any7);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any8",
                                   (void *)(uintptr_t)rt_call_any8);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any9",
                                   (void *)(uintptr_t)rt_call_any9);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any10",
                                   (void *)(uintptr_t)rt_call_any10);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any11",
                                   (void *)(uintptr_t)rt_call_any11);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any12",
                                   (void *)(uintptr_t)rt_call_any12);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any13",
                                   (void *)(uintptr_t)rt_call_any13);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any14",
                                   (void *)(uintptr_t)rt_call_any14);
  ny_jit_define_runtime_trampoline(mod, "rt_call_any15",
                                   (void *)(uintptr_t)rt_call_any15);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_set_raw",
                                   (void *)(uintptr_t)rt_dict_set_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_set_i64_raw",
                                   (void *)(uintptr_t)rt_dict_set_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_get_str_raw",
                                   (void *)(uintptr_t)rt_dict_get_str_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_set_str_raw",
                                   (void *)(uintptr_t)rt_dict_set_str_raw);
  ny_jit_define_runtime_trampoline(
      mod, "rt_native_dict_set_str_compact",
      (void *)(uintptr_t)rt_native_dict_set_str_compact);
  ny_jit_define_runtime_trampoline(
      mod, "rt_native_dict_set_raw_i64",
      (void *)(uintptr_t)rt_native_dict_set_raw_i64);
  ny_jit_define_runtime_trampoline(
      mod, "rt_native_dict_set_native_i64",
      (void *)(uintptr_t)rt_native_dict_set_native_i64);
  ny_jit_define_runtime_trampoline(
      mod, "rt_native_dict_set_fast_i64",
      (void *)(uintptr_t)rt_native_dict_set_fast_i64);
  ny_jit_define_runtime_trampoline(
      mod, "rt_native_dict_set_nir_i64",
      (void *)(uintptr_t)rt_native_dict_set_nir_i64);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_has_str_raw",
                                   (void *)(uintptr_t)rt_dict_has_str_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_delete_str_raw",
                                   (void *)(uintptr_t)rt_dict_delete_str_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_merge_raw",
                                   (void *)(uintptr_t)rt_dict_merge_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_clone_raw",
                                   (void *)(uintptr_t)rt_dict_clone_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_clear_raw",
                                   (void *)(uintptr_t)rt_dict_clear_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_has_raw",
                                   (void *)(uintptr_t)rt_dict_has_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_len_raw",
                                   (void *)(uintptr_t)rt_dict_len_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_dict_delete_raw",
                                   (void *)(uintptr_t)rt_dict_delete_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_assert_cstr",
                                   (void *)(uintptr_t)rt_assert_cstr);
  ny_jit_define_runtime_trampoline(mod, "rt_print_cstr",
                                   (void *)(uintptr_t)rt_print_cstr);
  ny_jit_define_runtime_trampoline(mod, "rt_print_i64_raw",
                                   (void *)(uintptr_t)rt_print_i64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_print_f64_raw",
                                   (void *)(uintptr_t)rt_print_f64_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_print_str_raw",
                                   (void *)(uintptr_t)rt_print_str_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_print_value",
                                   (void *)(uintptr_t)rt_print_value);
  ny_jit_define_runtime_trampoline(mod, "rt_print_int",
                                   (void *)(uintptr_t)rt_print_int);
  ny_jit_define_runtime_trampoline(mod, "rt_print_newline",
                                   (void *)(uintptr_t)rt_print_newline);
  ny_jit_define_runtime_trampoline(mod, "rt_long", (void *)(uintptr_t)rt_long);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_to_str",
                                   (void *)(uintptr_t)rt_bigint_to_str);
  ny_jit_define_runtime_trampoline(mod, "rt_bigint_to_cstr_raw",
                                   (void *)(uintptr_t)rt_bigint_to_cstr_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_bigfloat_to_str",
                                   (void *)(uintptr_t)rt_bigfloat_to_str);
  ny_jit_define_runtime_trampoline(mod, "rt_to_str",
                                   (void *)(uintptr_t)rt_to_str);
  ny_jit_define_runtime_trampoline(mod, "rt_async_await_blocking",
                                   (void *)(uintptr_t)rt_async_await_blocking);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_eq",
                                   (void *)(uintptr_t)rt_cstr_eq);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_cmp",
                                   (void *)(uintptr_t)rt_cstr_cmp);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_concat",
                                   (void *)(uintptr_t)rt_cstr_concat);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_repeat",
                                   (void *)(uintptr_t)rt_cstr_repeat);
  ny_jit_define_runtime_trampoline(mod, "rt_cstr_slice",
                                   (void *)(uintptr_t)rt_cstr_slice);
  ny_jit_define_runtime_trampoline(mod, "rt_any_to_cstr",
                                   (void *)(uintptr_t)rt_any_to_cstr);
  ny_jit_define_runtime_trampoline(mod, "rt_i64_to_cstr_raw",
                                   (void *)(uintptr_t)rt_i64_to_cstr_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_to_cstr_raw",
                                   (void *)(uintptr_t)rt_f64_to_cstr_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_to_json_str",
                                   (void *)(uintptr_t)rt_f64_to_json_str);
  ny_jit_define_runtime_trampoline(mod, "rt_any_to_f64",
                                   (void *)(uintptr_t)rt_any_to_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_fmod_f64",
                                   (void *)(uintptr_t)rt_fmod_f64);
  ny_jit_define_runtime_trampoline(mod, "rt_contains_raw",
                                   (void *)(uintptr_t)rt_contains_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_thread_spawn_raw",
                                   (void *)(uintptr_t)rt_thread_spawn_raw);
  ny_jit_define_runtime_trampoline(mod, "rt_alloc_string",
                                   (void *)(uintptr_t)rt_alloc_string);
  ny_jit_define_runtime_trampoline(mod, "rt_panic",
                                   (void *)(uintptr_t)rt_panic);
  ny_jit_define_runtime_trampoline(mod, "rt_i64_min",
                                   (void *)(uintptr_t)rt_i64_min);
  ny_jit_define_runtime_trampoline(mod, "rt_i64_max",
                                   (void *)(uintptr_t)rt_i64_max);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_min",
                                   (void *)(uintptr_t)rt_f64_min);
  ny_jit_define_runtime_trampoline(mod, "rt_f64_max",
                                   (void *)(uintptr_t)rt_f64_max);
#ifndef _WIN32
  /*
   * _setjmp is intentionally not materialized as a trampoline; see
   * ny_jit_define_runtime_trampoline().  ny_jit_add_runtime_symbols() and
   * register_extern_symbols() bind the declaration directly.
   */
#endif
}

static void register_extern_symbols(LLVMExecutionEngineRef ee,
                                    LLVMModuleRef mod, codegen_t *cg) {
  if (!cg || !mod)
    return;

  if (cg->fun_sigs.len == 0) {
    for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
         f = LLVMGetNextFunction(f)) {
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
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (!LLVMIsDeclaration(f))
      continue;
    if (!LLVMGetFirstUse(f))
      continue;
    const char *name = LLVMGetValueName(f);
    if (!name || strncmp(name, "llvm.", 5) == 0)
      continue;
    void *ptr = resolve_symbol_with_fallback(name);
    if (ptr) {
      LLVMAddGlobalMapping(ee, f, ptr);
      LLVMAddSymbol(name, ptr);
    }
  }
}

static void register_jit_sigs(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                              codegen_t *cg) {
  (void)ee;
  (void)mod;
  (void)cg;
}

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                          codegen_t *cg) {
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
#define MAP_FULL(name, fn_ptr, args)                                           \
  do {                                                                         \
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                        \
    if (!val && auto_declare_runtime) {                                        \
      LLVMTypeRef param_types[16];                                             \
      int n_params = (args < 0) ? 0 : (args > 16 ? 16 : args);                 \
      for (int i = 0; i < n_params; i++)                                       \
        param_types[i] = cg->type_i64;                                         \
      LLVMTypeRef ftype =                                                      \
          LLVMFunctionType(cg->type_i64, param_types, n_params, args < 0);     \
      val = LLVMAddFunction(mod, name, ftype);                                 \
      if (verbose_enabled >= 4)                                                \
        NY_LOG_V3("JIT: auto-declared runtime symbol %s\n", name);             \
    }                                                                          \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)(uintptr_t)fn_ptr);                \
      if (verbose_enabled >= 4)                                                \
        NY_LOG_V3("JIT: mapped runtime symbol %s\n", name);                    \
    }                                                                          \
  } while (0)

#define MAP_GV(name, ptr)                                                      \
  do {                                                                         \
    LLVMValueRef val = LLVMGetNamedGlobal(mod, name);                          \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)ptr);                              \
      if (verbose_enabled >= 3)                                                \
        NY_LOG_V3("JIT: mapped runtime global %s\n", name);                    \
    }                                                                          \
  } while (0)

#define RT_DEF(name, p, args, sig, doc)                                        \
  do {                                                                         \
    MAP_FULL(name, p, args);                                                   \
    if (strcmp(name, #p) != 0)                                                 \
      MAP_FULL(#p, p, args);                                                   \
  } while (0);
#define RT_GV(name, p, t, doc) MAP_GV(name, &p);
#include "code/lang/runtime/defs.h"
#undef RT_DEF
#undef RT_GV
  MAP_FULL("rt_simmd_byte_class_reduce_raw", rt_simmd_byte_class_reduce_raw, 7);
  MAP_FULL("rt_simmd_i32_sqlscan_sum_raw", rt_simmd_i32_sqlscan_sum_raw, 6);
  register_extern_symbols(ee, mod, cg);
  register_jit_sigs(ee, mod, cg);

apply_runtime_attrs:;

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

#ifndef _WIN32
typedef struct {
  uint64_t addr;
  uint64_t size;
  const char *name;
} ny_orc_perf_func_t;

typedef struct ny_orc_perf_module {
  LLVMOrcResourceTrackerRef tracker;
  ny_orc_perf_func_t *funcs;
  size_t count;
  struct ny_orc_perf_module *next;
} ny_orc_perf_module_t;

static ny_orc_perf_module_t *ny_orc_perf_modules;
static atomic_flag ny_orc_perf_lock = ATOMIC_FLAG_INIT;

typedef struct ny_orc_perf_size {
  LLVMOrcLLJITRef jit;
  char *name;
  uint64_t size;
  struct ny_orc_perf_size *next;
} ny_orc_perf_size_t;

static ny_orc_perf_size_t *ny_orc_perf_sizes;
static bool ny_orc_perf_map_enabled(void);

static void ny_orc_perf_lock_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&ny_orc_perf_lock,
                                           memory_order_acquire)) {
  }
}

static void ny_orc_perf_lock_release(void) {
  atomic_flag_clear_explicit(&ny_orc_perf_lock, memory_order_release);
}

static void ny_orc_record_object_symbol_size(LLVMOrcLLJITRef jit,
                                             const char *object_name,
                                             uint64_t size) {
  if (!jit || !object_name || !*object_name || !size)
    return;
  char prefix = LLVMOrcLLJITGetGlobalPrefix(jit);
  const char *name = object_name[0] == prefix ? object_name + 1 : object_name;
  ny_orc_perf_lock_acquire();
  for (ny_orc_perf_size_t *entry = ny_orc_perf_sizes; entry;
       entry = entry->next) {
    if (entry->jit == jit && strcmp(entry->name, name) == 0) {
      entry->size = size;
      ny_orc_perf_lock_release();
      return;
    }
  }
  ny_orc_perf_size_t *entry = calloc(1, sizeof(*entry));
  if (entry) {
    entry->name = ny_strdup(name);
    if (entry->name) {
      entry->jit = jit;
      entry->size = size;
      entry->next = ny_orc_perf_sizes;
      ny_orc_perf_sizes = entry;
      entry = NULL;
    }
  }
  if (entry) {
    free(entry->name);
    free(entry);
  }
  ny_orc_perf_lock_release();
}

static uint64_t ny_orc_lookup_object_symbol_size(LLVMOrcLLJITRef jit,
                                                 const char *name) {
  uint64_t size = 0;
  ny_orc_perf_lock_acquire();
  for (ny_orc_perf_size_t *entry = ny_orc_perf_sizes; entry;
       entry = entry->next) {
    if (entry->jit == jit && strcmp(entry->name, name) == 0) {
      size = entry->size;
      break;
    }
  }
  ny_orc_perf_lock_release();
  return size;
}

static void ny_orc_forget_object_symbol_sizes(LLVMOrcLLJITRef jit) {
  ny_orc_perf_lock_acquire();
  ny_orc_perf_size_t **link = &ny_orc_perf_sizes;
  while (*link) {
    ny_orc_perf_size_t *entry = *link;
    if (entry->jit != jit) {
      link = &entry->next;
      continue;
    }
    *link = entry->next;
    free(entry->name);
    free(entry);
  }
  ny_orc_perf_lock_release();
}

static LLVMErrorRef ny_orc_perf_capture_object(void *ctx,
                                               LLVMMemoryBufferRef *obj) {
  LLVMOrcLLJITRef jit = (LLVMOrcLLJITRef)ctx;
  if (!ny_orc_perf_map_enabled() || !obj || !*obj)
    return LLVMErrorSuccess;
  char *error = NULL;
  LLVMBinaryRef binary = LLVMCreateBinary(*obj, NULL, &error);
  if (!binary) {
    if (error)
      LLVMDisposeMessage(error);
    return LLVMErrorSuccess;
  }
  LLVMSymbolIteratorRef symbol = LLVMObjectFileCopySymbolIterator(binary);
  if (symbol) {
    while (!LLVMObjectFileIsSymbolIteratorAtEnd(binary, symbol)) {
      ny_orc_record_object_symbol_size(jit, LLVMGetSymbolName(symbol),
                                       LLVMGetSymbolSize(symbol));
      LLVMMoveToNextSymbol(symbol);
    }
    LLVMDisposeSymbolIterator(symbol);
  }
  LLVMDisposeBinary(binary);
  return LLVMErrorSuccess;
}

static int ny_orc_perf_func_cmp(const void *pa, const void *pb) {
  const ny_orc_perf_func_t *a = pa;
  const ny_orc_perf_func_t *b = pb;
  if (a->addr < b->addr)
    return -1;
  if (a->addr > b->addr)
    return 1;
  return 0;
}

static bool ny_orc_perf_map_enabled(void) {
  const char *env = getenv("NYTRIX_JIT_PERF_MAP");
  return env && *env && strcmp(env, "0") != 0;
}

static char **ny_orc_collect_perf_names(LLVMModuleRef mod, size_t *out_len) {
  *out_len = 0;
  if (!ny_orc_perf_map_enabled())
    return NULL;
  size_t len = 0, cap = 0;
  char **names = NULL;
  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMIsDeclaration(fn))
      continue;
    const char *name = LLVMGetValueName(fn);
    if (!name || !*name)
      continue;
    if (len == cap) {
      size_t next = cap ? cap * 2 : 16;
      if (next < cap || next > SIZE_MAX / sizeof(*names))
        goto fail;
      char **grown = realloc(names, next * sizeof(*names));
      if (!grown)
        goto fail;
      names = grown;
      cap = next;
    }
    names[len] = ny_strdup(name);
    if (!names[len])
      goto fail;
    ++len;
  }
  *out_len = len;
  return names;

fail:
  for (size_t i = 0; i < len; ++i)
    free(names[i]);
  free(names);
  return NULL;
}

static void ny_orc_free_perf_names(char **names, size_t len) {
  for (size_t i = 0; i < len; ++i)
    free(names[i]);
  free(names);
}

static void ny_orc_write_perf_map(LLVMOrcLLJITRef jit,
                                  LLVMOrcResourceTrackerRef tracker,
                                  char **names, size_t name_count) {
  if (!names || !name_count)
    return;
  ny_orc_perf_func_t *funcs = calloc(name_count, sizeof(*funcs));
  if (!funcs)
    return;
  size_t count = 0;
  for (size_t i = 0; i < name_count; ++i) {
    LLVMOrcExecutorAddress addr = 0;
    LLVMErrorRef err = LLVMOrcLLJITLookup(jit, &addr, names[i]);
    if (err) {
      LLVMConsumeError(err);
      continue;
    }
    if (!addr)
      continue;
    funcs[count++] = (ny_orc_perf_func_t){
        .addr = (uint64_t)addr, .size = 0, .name = names[i]};
  }
  if (!count) {
    free(funcs);
    return;
  }
  qsort(funcs, count, sizeof(*funcs), ny_orc_perf_func_cmp);
  char path[4096];
  int path_len = snprintf(path, sizeof(path), "%s/perf-%ld.map",
                          ny_get_temp_dir(), (long)getpid());
  if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
    free(funcs);
    return;
  }
  size_t sized_count = 0;
  for (size_t i = 0; i < count; ++i) {
    uint64_t size = ny_orc_lookup_object_symbol_size(jit, funcs[i].name);
    if (!size)
      continue;
    funcs[sized_count] = funcs[i];
    funcs[sized_count].size = size;
    ++sized_count;
  }
  count = sized_count;
  if (!count) {
    free(funcs);
    return;
  }
  ny_orc_perf_module_t *record = calloc(1, sizeof(*record));
  if (!record) {
    free(funcs);
    return;
  }
  record->funcs = calloc(count, sizeof(*record->funcs));
  if (!record->funcs) {
    free(record);
    free(funcs);
    return;
  }
  record->tracker = tracker;
  record->count = count;
  for (size_t i = 0; i < count; ++i) {
    record->funcs[i] = funcs[i];
    record->funcs[i].name = ny_strdup(funcs[i].name);
    if (!record->funcs[i].name) {
      for (size_t j = 0; j < i; ++j)
        free((void *)record->funcs[j].name);
      free(record->funcs);
      free(record);
      free(funcs);
      return;
    }
  }
  ny_orc_perf_lock_acquire();
  FILE *map = fopen(path, "a");
  if (map) {
    for (size_t i = 0; i < count; ++i)
      fprintf(map, "%" PRIx64 " %" PRIx64 " %s\n", funcs[i].addr, funcs[i].size,
              funcs[i].name);
    fclose(map);
    record->next = ny_orc_perf_modules;
    ny_orc_perf_modules = record;
    record = NULL;
  }
  ny_orc_perf_lock_release();
  if (record) {
    for (size_t i = 0; i < record->count; ++i)
      free((void *)record->funcs[i].name);
    free(record->funcs);
    free(record);
  }
  free(funcs);
}
#else
static char **ny_orc_collect_perf_names(LLVMModuleRef mod, size_t *out_len) {
  (void)mod;
  *out_len = 0;
  return NULL;
}
static void ny_orc_free_perf_names(char **names, size_t len) {
  (void)names;
  (void)len;
}
static void ny_orc_write_perf_map(LLVMOrcLLJITRef jit,
                                  LLVMOrcResourceTrackerRef tracker,
                                  char **names, size_t name_count) {
  (void)jit;
  (void)tracker;
  (void)names;
  (void)name_count;
}
#endif

static void ny_orc_perf_remove_tracker(LLVMOrcResourceTrackerRef tracker) {
#ifndef _WIN32
  if (!tracker)
    return;
  ny_orc_perf_lock_acquire();
  ny_orc_perf_module_t **link = &ny_orc_perf_modules;
  while (*link && (*link)->tracker != tracker)
    link = &(*link)->next;
  ny_orc_perf_module_t *record = *link;
  if (record) {
    *link = record->next;
    char path[4096], tmp_path[4120];
    int path_len = snprintf(path, sizeof(path), "%s/perf-%ld.map",
                            ny_get_temp_dir(), (long)getpid());
    int tmp_len = path_len > 0 && (size_t)path_len < sizeof(path)
                      ? snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path)
                      : -1;
    if (tmp_len > 0 && (size_t)tmp_len < sizeof(tmp_path)) {
      FILE *in = fopen(path, "r");
      FILE *out = in ? fopen(tmp_path, "w") : NULL;
      if (in && out) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, in) >= 0) {
          uint64_t addr = 0;
          char name[2048];
          if (sscanf(line, "%" SCNx64 " %*" SCNx64 " %2047s", &addr, name) !=
              2) {
            fputs(line, out);
            continue;
          }
          bool owned = false;
          for (size_t i = 0; i < record->count; ++i) {
            if (record->funcs[i].addr == addr &&
                strcmp(record->funcs[i].name, name) == 0) {
              owned = true;
              break;
            }
          }
          if (!owned)
            fputs(line, out);
        }
        free(line);
        fclose(in);
        if (fclose(out) != 0 || rename(tmp_path, path) != 0)
          (void)unlink(tmp_path);
      } else {
        if (in)
          fclose(in);
        if (out)
          fclose(out);
        (void)unlink(tmp_path);
      }
    }
    for (size_t i = 0; i < record->count; ++i)
      free((void *)record->funcs[i].name);
    free(record->funcs);
    free(record);
  }
  ny_orc_perf_lock_release();
#else
  (void)tracker;
#endif
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
#ifndef _WIN32
  if (ny_orc_perf_map_enabled())
    LLVMOrcObjectTransformLayerSetTransform(
        LLVMOrcLLJITGetObjTransformLayer(jit), ny_orc_perf_capture_object, jit);
#endif
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

  void *orc_jit = cg ? (cg->orc_jit ? cg->orc_jit
                                    : (cg->parent ? cg->parent->orc_jit : NULL))
                     : NULL;

  if (!cg || !orc_jit || !module || !context || !out_rt)
    return false;

#if LLVM_VERSION_MAJOR < 21
  return false;
#else
  LLVMOrcLLJITRef jit = (LLVMOrcLLJITRef)orc_jit;
  size_t perf_name_count = 0;
  char **perf_names = ny_orc_collect_perf_names(module, &perf_name_count);
  ny_orc_register_extern_symbols(module, cg);

  LLVMOrcJITDylibRef dylib = LLVMOrcLLJITGetMainJITDylib(jit);
  LLVMOrcResourceTrackerRef rt = LLVMOrcJITDylibCreateResourceTracker(dylib);
  if (!rt) {
    ny_orc_free_perf_names(perf_names, perf_name_count);
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
    ny_orc_free_perf_names(perf_names, perf_name_count);
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
    ny_orc_free_perf_names(perf_names, perf_name_count);
    if (error_message)
      *error_message = ny_strdup("could not create ORC thread-safe module");
    LLVMOrcReleaseResourceTracker(rt);
    return false;
  }

  LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModuleWithRT(jit, rt, ts_module);
  if (err) {
    ny_orc_free_perf_names(perf_names, perf_name_count);
    if (error_message)
      *error_message = ny_orc_error_message(err);
    LLVMOrcReleaseResourceTracker(rt);
    return false;
  }

  LLVMOrcExecutorAddress addr = 0;
  err = LLVMOrcLLJITLookup(jit, &addr, "_ny_top_entry");
  if (err) {
    ny_orc_free_perf_names(perf_names, perf_name_count);
    if (error_message)
      *error_message = ny_orc_error_message(err);
    err = LLVMOrcResourceTrackerRemove(rt);
    if (err)
      LLVMConsumeError(err);
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
  ny_orc_write_perf_map(jit, rt, perf_names, perf_name_count);
  ny_orc_free_perf_names(perf_names, perf_name_count);
  return true;
#endif
}

void ny_orc_jit_remove_module(void *rt) {
#if LLVM_VERSION_MAJOR >= 21
  if (!rt)
    return;
  LLVMOrcResourceTrackerRef tracker = (LLVMOrcResourceTrackerRef)rt;
  LLVMErrorRef err = LLVMOrcResourceTrackerRemove(tracker);
  if (err) {
    LLVMConsumeError(err);
  } else {
    ny_orc_perf_remove_tracker(tracker);
  }
  LLVMOrcReleaseResourceTracker(tracker);
#endif
}

void ny_orc_jit_dispose(void *jit) {
  if (!jit)
    return;
#ifndef _WIN32
  ny_orc_forget_object_symbol_sizes((LLVMOrcLLJITRef)jit);
#endif
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

static void ny_jit_update_perf_map(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                   bool append) {
#ifndef _WIN32
  const char *env = getenv("NYTRIX_JIT_PERF_MAP");
  if (!env || !*env || strcmp(env, "0") == 0)
    return;
  char path[4096];
  snprintf(path, sizeof(path), "%s/perf-%d.map", ny_get_temp_dir(),
           (int)getpid());

  typedef struct {
    uint64_t addr;
    const char *name;
  } func_info_t;

  func_info_t *funcs = NULL;
  size_t num_funcs = 0;
  size_t capacity = 0;

  for (LLVMValueRef fn = LLVMGetFirstFunction(mod); fn;
       fn = LLVMGetNextFunction(fn)) {
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
      func_info_t *new_funcs =
          (func_info_t *)realloc(funcs, capacity * sizeof(func_info_t));
      if (!new_funcs) {
        free(funcs);
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

  FILE *f = fopen(path, append ? "a" : "w");
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
  (void)append;
#endif
}

void ny_jit_write_perf_map(LLVMExecutionEngineRef ee, LLVMModuleRef mod) {
  ny_jit_update_perf_map(ee, mod, false);
}

void ny_jit_append_perf_map(LLVMExecutionEngineRef ee, LLVMModuleRef mod) {
  ny_jit_update_perf_map(ee, mod, true);
}
