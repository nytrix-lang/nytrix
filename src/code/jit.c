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
#endif
#if !defined(_WIN32) && defined(__APPLE__)
#include <sys/mman.h>
#endif
#include "priv.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/TargetMachine.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32) && defined(__APPLE__) && defined(__aarch64__)
extern void pthread_jit_write_protect_np(int enabled) __attribute__((weak_import));
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

static int64_t ny_missing_extern_stub0(void) { return 0; }
static int64_t ny_missing_extern_stub1(int64_t a0) {
  (void)a0;
  return 0;
}
static int64_t ny_missing_extern_stub2(int64_t a0, int64_t a1) {
  (void)a0;
  (void)a1;
  return 0;
}
static int64_t ny_missing_extern_stub3(int64_t a0, int64_t a1, int64_t a2) {
  (void)a0;
  (void)a1;
  (void)a2;
  return 0;
}
static int64_t ny_missing_extern_stub4(int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  return 0;
}
static int64_t ny_missing_extern_stub5(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  return 0;
}
static int64_t ny_missing_extern_stub6(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                       int64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}
static int64_t ny_missing_extern_stub7(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                       int64_t a5, int64_t a6) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  return 0;
}
static int64_t ny_missing_extern_stub8(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                       int64_t a5, int64_t a6, int64_t a7) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;
  return 0;
}
static int64_t ny_missing_extern_stub9(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                       int64_t a5, int64_t a6, int64_t a7, int64_t a8) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;
  (void)a8;
  return 0;
}
static int64_t ny_missing_extern_stub10(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                        int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                                        int64_t a9) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;
  (void)a8;
  (void)a9;
  return 0;
}
static int64_t ny_missing_extern_stub11(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                        int64_t a5, int64_t a6, int64_t a7, int64_t a8, int64_t a9,
                                        int64_t a10) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;
  (void)a8;
  (void)a9;
  (void)a10;
  return 0;
}
static int64_t ny_missing_extern_stub12(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                        int64_t a5, int64_t a6, int64_t a7, int64_t a8, int64_t a9,
                                        int64_t a10, int64_t a11) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;
  (void)a8;
  (void)a9;
  (void)a10;
  (void)a11;
  return 0;
}
static int64_t ny_missing_extern_stub16(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                                        int64_t a5, int64_t a6, int64_t a7, int64_t a8, int64_t a9,
                                        int64_t a10, int64_t a11, int64_t a12, int64_t a13,
                                        int64_t a14, int64_t a15) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;
  (void)a8;
  (void)a9;
  (void)a10;
  (void)a11;
  (void)a12;
  (void)a13;
  (void)a14;
  (void)a15;
  return 0;
}

static void *ny_missing_extern_stub_for_arity(int arity, bool variadic) {
  if (variadic)
    return (void *)(uintptr_t)ny_missing_extern_stub16;
  switch (arity) {
  case 0:
    return (void *)(uintptr_t)ny_missing_extern_stub0;
  case 1:
    return (void *)(uintptr_t)ny_missing_extern_stub1;
  case 2:
    return (void *)(uintptr_t)ny_missing_extern_stub2;
  case 3:
    return (void *)(uintptr_t)ny_missing_extern_stub3;
  case 4:
    return (void *)(uintptr_t)ny_missing_extern_stub4;
  case 5:
    return (void *)(uintptr_t)ny_missing_extern_stub5;
  case 6:
    return (void *)(uintptr_t)ny_missing_extern_stub6;
  case 7:
    return (void *)(uintptr_t)ny_missing_extern_stub7;
  case 8:
    return (void *)(uintptr_t)ny_missing_extern_stub8;
  case 9:
    return (void *)(uintptr_t)ny_missing_extern_stub9;
  case 10:
    return (void *)(uintptr_t)ny_missing_extern_stub10;
  case 11:
    return (void *)(uintptr_t)ny_missing_extern_stub11;
  case 12:
    return (void *)(uintptr_t)ny_missing_extern_stub12;
  default:
    return (void *)(uintptr_t)ny_missing_extern_stub16;
  }
}

#if !defined(_WIN32) && defined(__APPLE__) && defined(__aarch64__)
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_JIT
#define MAP_JIT 0
#endif

typedef struct ny_apple_jit_alloc_t {
  void *base;
  size_t size;
  bool code;
  bool read_only;
  struct ny_apple_jit_alloc_t *next;
} ny_apple_jit_alloc_t;

typedef struct ny_apple_jit_mm_t {
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
  if (size == 0)
    size = 1;
  return (size + page - 1u) & ~(page - 1u);
}

static void ny_apple_jit_write_protect(int enabled) {
  if (pthread_jit_write_protect_np)
    pthread_jit_write_protect_np(enabled);
}

static uint8_t *ny_apple_jit_alloc_section(void *opaque, uintptr_t size,
                                           unsigned alignment, bool code,
                                           bool read_only) {
  ny_apple_jit_mm_t *mm = (ny_apple_jit_mm_t *)opaque;
  if (!mm)
    return NULL;
  uintptr_t align = alignment ? (uintptr_t)alignment : 16u;
  if ((align & (align - 1u)) != 0)
    align = 16u;
  size_t alloc_size = ny_jit_round_page((size_t)size + (size_t)align);
  int flags = MAP_PRIVATE | MAP_ANON | (code ? MAP_JIT : 0);
  int prot = PROT_READ | PROT_WRITE | (code ? PROT_EXEC : 0);
  void *base = mmap(NULL, alloc_size, prot, flags, -1, 0);
  if (base == MAP_FAILED)
    return NULL;

  ny_apple_jit_alloc_t *node = calloc(1, sizeof(*node));
  if (!node) {
    munmap(base, alloc_size);
    return NULL;
  }
  node->base = base;
  node->size = alloc_size;
  node->code = code;
  node->read_only = read_only;
  node->next = mm->allocs;
  mm->allocs = node;
  if (code)
    ny_apple_jit_write_protect(0);
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
                                        LLVMBool is_read_only) {
  (void)section_id;
  (void)section_name;
  return ny_apple_jit_alloc_section(opaque, size, alignment, false,
                                    is_read_only != 0);
}

static LLVMBool ny_apple_jit_finalize(void *opaque, char **err_msg) {
  ny_apple_jit_mm_t *mm = (ny_apple_jit_mm_t *)opaque;
  if (!mm)
    return 0;
  for (ny_apple_jit_alloc_t *a = mm->allocs; a; a = a->next) {
    int prot = PROT_READ;
    if (a->code)
      prot |= PROT_EXEC;
    else if (!a->read_only)
      prot |= PROT_WRITE;
    if (mprotect(a->base, a->size, prot) != 0) {
      if (err_msg)
        *err_msg = LLVMCreateMessage("failed to finalize Apple arm64 JIT memory");
      return 1;
    }
    if (a->code)
      __builtin___clear_cache((char *)a->base, (char *)a->base + a->size);
  }
  ny_apple_jit_write_protect(1);
  return 0;
}

static void ny_apple_jit_destroy(void *opaque) {
  ny_apple_jit_mm_t *mm = (ny_apple_jit_mm_t *)opaque;
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
    opt_level = atoi(opt_env);
    if (opt_level < 0)
      opt_level = 0;
    if (opt_level > 3)
      opt_level = 3;
  }
  if (apple_arm64 && (!opt_env || !*opt_env)) {

    opt_level = 0;
  }

  int fast_isel = (opt_level <= 1) ? 1 : 0;
  const char *fast_isel_env = getenv("NYTRIX_JIT_FAST_ISEL");
  if (fast_isel_env && *fast_isel_env) {
    fast_isel = (atoi(fast_isel_env) != 0);
  } else if (apple_arm64) {

    fast_isel = 0;
  }

  options->CodeModel = apple_arm64 ? LLVMCodeModelLarge : LLVMCodeModelJITDefault;
  options->OptLevel = (unsigned)opt_level;
  options->EnableFastISel = fast_isel;
#if !defined(_WIN32) && defined(__APPLE__) && defined(__aarch64__)
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

void ny_jit_prepare_execution(void) {
#if !defined(_WIN32) && defined(__APPLE__) && defined(__aarch64__)
  ny_apple_jit_write_protect(1);
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
#endif

void *ny_jit_load_library(const char *path) {
  if (!path || !*path)
    return NULL;
#ifdef _WIN32
  HMODULE h = LoadLibraryA(path);
  if (h)
    return (void *)h;
  if (!strchr(path, '.') && !strchr(path, '\\') && !strchr(path, '/')) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.dll", path);
    h = LoadLibraryA(buf);
    if (h)
      return (void *)h;
    snprintf(buf, sizeof(buf), "lib%s.dll", path);
    h = LoadLibraryA(buf);
    if (h)
      return (void *)h;
  }
  return NULL;
#else
#ifdef __APPLE__
  void *apple_ssl = ny_jit_load_apple_openssl(path);
  if (apple_ssl)
    return apple_ssl;
  if (ny_jit_apple_openssl_basename(path, NULL))
    return NULL;
#endif
  void *h = dlopen(path, RTLD_GLOBAL | RTLD_LAZY);
  if (h) {
    if (verbose_enabled >= 2)
      fprintf(stderr, "JIT: loaded library '%s' (handle=%p)\n", path, h);
    return h;
  }
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
      LLVMTypeRef fty = LLVMGlobalGetValueType(f);
      unsigned arity = LLVMCountParamTypes(fty);
      bool variadic = LLVMIsFunctionVarArg(fty);
      void *ptr = resolve_symbol_with_fallback(name);
      if (!ptr) {
        NY_LOG_DEBUG("extern symbol '%s' not found (cache fallback)", name);
        ptr = ny_missing_extern_stub_for_arity((int)arity, variadic);
      }
      LLVMAddGlobalMapping(ee, f, ptr);
    }
    return;
  }
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->is_extern)
      continue;

    const char *symbol = sig->link_name ? sig->link_name : sig->name;

    LLVMValueRef val = LLVMGetNamedFunction(mod, symbol);
    if (!val) {
      val = LLVMGetNamedFunction(mod, sig->name);
    }
    if (!val)
      continue;
    if (!LLVMGetFirstUse(val))
      continue;

    void *ptr = resolve_symbol_with_fallback(symbol);
    if (!ptr) {
      ptr = ny_missing_extern_stub_for_arity(sig->arity, sig->is_variadic);
    }
    LLVMAddGlobalMapping(ee, val, ptr);
  }
}

void register_jit_sigs(LLVMExecutionEngineRef ee, LLVMModuleRef mod, codegen_t *cg) {
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
