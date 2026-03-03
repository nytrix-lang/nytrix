#include "code/jit.h"
#include "base/common.h"
#include "rt/runtime.h"
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "priv.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <stdint.h>
#include <string.h>
extern int64_t __rt_alloc_string(const char *s);
extern int64_t __os_name(void);
extern int64_t __arch_name(void);
extern int64_t __main(void);

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
static int64_t ny_missing_extern_stub4(int64_t a0, int64_t a1, int64_t a2,
                                       int64_t a3) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  return 0;
}
static int64_t ny_missing_extern_stub5(int64_t a0, int64_t a1, int64_t a2,
                                       int64_t a3, int64_t a4) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  return 0;
}
static int64_t ny_missing_extern_stub6(int64_t a0, int64_t a1, int64_t a2,
                                       int64_t a3, int64_t a4, int64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}
static int64_t ny_missing_extern_stub7(int64_t a0, int64_t a1, int64_t a2,
                                       int64_t a3, int64_t a4, int64_t a5,
                                       int64_t a6) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  return 0;
}
static int64_t ny_missing_extern_stub8(int64_t a0, int64_t a1, int64_t a2,
                                       int64_t a3, int64_t a4, int64_t a5,
                                       int64_t a6, int64_t a7) {
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
static int64_t ny_missing_extern_stub9(int64_t a0, int64_t a1, int64_t a2,
                                       int64_t a3, int64_t a4, int64_t a5,
                                       int64_t a6, int64_t a7, int64_t a8) {
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
static int64_t ny_missing_extern_stub10(int64_t a0, int64_t a1, int64_t a2,
                                        int64_t a3, int64_t a4, int64_t a5,
                                        int64_t a6, int64_t a7, int64_t a8,
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
static int64_t ny_missing_extern_stub11(int64_t a0, int64_t a1, int64_t a2,
                                        int64_t a3, int64_t a4, int64_t a5,
                                        int64_t a6, int64_t a7, int64_t a8,
                                        int64_t a9, int64_t a10) {
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
static int64_t ny_missing_extern_stub12(int64_t a0, int64_t a1, int64_t a2,
                                        int64_t a3, int64_t a4, int64_t a5,
                                        int64_t a6, int64_t a7, int64_t a8,
                                        int64_t a9, int64_t a10, int64_t a11) {
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
static int64_t ny_missing_extern_stub16(int64_t a0, int64_t a1, int64_t a2,
                                        int64_t a3, int64_t a4, int64_t a5,
                                        int64_t a6, int64_t a7, int64_t a8,
                                        int64_t a9, int64_t a10, int64_t a11,
                                        int64_t a12, int64_t a13, int64_t a14,
                                        int64_t a15) {
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

void ny_jit_init_options(struct LLVMMCJITCompilerOptions *options,
                         LLVMModuleRef mod) {
  LLVMInitializeMCJITCompilerOptions(options, sizeof(*options));
  options->CodeModel = LLVMCodeModelJITDefault;
  options->OptLevel = 0;
  options->EnableFastISel = 0; /* Default to off, specific modes may enable */

  if (mod) {
    const char *triple = LLVMGetTarget(mod);
    bool is_apple =
        triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
                   strstr(triple, "macos"));
    bool is_arm64 =
        triple && (strstr(triple, "arm64") || strstr(triple, "aarch64"));
    if (is_apple && is_arm64) {
      options->CodeModel = LLVMCodeModelLarge;
    }
  }

  const char *cm = getenv("NYTRIX_JIT_CODE_MODEL");
  if (cm && *cm) {
    if (strcmp(cm, "large") == 0)
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
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    const char *name = LLVMGetValueName(f);
    if (!name || !*name || strncmp(name, "llvm.", 5) == 0)
      continue;
    if (LLVMCountBasicBlocks(f) > 0 &&
        (!entry_name || strcmp(name, entry_name) != 0))
      continue;
    if (verbose_enabled >= 4)
      fprintf(stderr, "JIT: checking function '%s' (bbcount=%u)\n", name,
              (unsigned)LLVMCountBasicBlocks(f));
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

static void register_extern_symbols(LLVMExecutionEngineRef ee,
                                    LLVMModuleRef mod, codegen_t *cg) {
  if (!cg || !mod)
    return;
  /* Fallback: if cache hit skipped sig collection, map all declarations. */
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
      LLVMTypeRef fty = LLVMGetElementType(LLVMTypeOf(f));
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

    /* Look up the function in the current module.
       The function might be named by its qualified name or its link name. */
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

void register_jit_sigs(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                       codegen_t *cg) {
  (void)ee;
  (void)mod;
  (void)cg;
  // For non-extern functions, they are defined within the JIT.
}

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                          codegen_t *cg) {
  if (cg) {
    for (size_t i = 0; i < cg->links.len; i++) {
      ny_jit_load_library(cg->links.data[i]);
    }
  }
#define MAP_FULL(name, fn_ptr, args)                                           \
  do {                                                                         \
    LLVMAddSymbol(name, (void *)(uintptr_t)(fn_ptr));                          \
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                        \
    if (!val && cg) {                                                          \
      LLVMTypeRef param_types[16];                                             \
      int n_params = (args < 0) ? 0 : (args > 16 ? 16 : args);                 \
      for (int i = 0; i < n_params; i++)                                       \
        param_types[i] = cg->type_i64;                                         \
      LLVMTypeRef ftype =                                                      \
          LLVMFunctionType(cg->type_i64, param_types, n_params, args < 0);     \
      val = LLVMAddFunction(mod, name, ftype);                                 \
      if (verbose_enabled >= 4)                                                \
        fprintf(stderr, "[jit] auto-declared %s\n", name);                     \
    }                                                                          \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)(uintptr_t)fn_ptr);                \
      if (verbose_enabled >= 4)                                                \
        fprintf(stderr, "[jit] mapped %s to %p\n", name, (void *)fn_ptr);      \
    }                                                                          \
  } while (0)

#define MAP_GV(name, ptr)                                                      \
  do {                                                                         \
    LLVMAddSymbol(name, (void *)(ptr));                                        \
    LLVMValueRef val = LLVMGetNamedGlobal(mod, name);                          \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)ptr);                              \
      if (verbose_enabled >= 3)                                                \
        fprintf(stderr, "[jit] mapped global %s to %p\n", name, (void *)ptr);  \
    }                                                                          \
  } while (0)

#define RT_DEF(name, p, args, sig, doc)                                        \
  do {                                                                         \
    MAP_FULL(name, p, args);                                                   \
    if (strcmp(name, #p) != 0)                                                 \
      MAP_FULL(#p, p, args);                                                   \
  } while (0);
#define RT_GV(name, p, t, doc) MAP_GV(name, &p);
#include "rt/defs.h"
#undef RT_DEF
#undef RT_GV
  LLVMAddSymbol("__rt_alloc_string", (void *)(uintptr_t)__rt_alloc_string);
  register_extern_symbols(ee, mod, cg);
  register_jit_sigs(ee, mod, cg);

  // Apply critical attributes to runtime symbols
  LLVMValueRef panic_fn = LLVMGetNamedFunction(mod, "__panic");
  if (panic_fn) {
    unsigned nr_kind = LLVMGetEnumAttributeKindForName("noreturn", 8);
    if (nr_kind != 0) {
      LLVMAttributeRef nr_attr = LLVMCreateEnumAttribute(cg->ctx, nr_kind, 0);
      LLVMAddAttributeAtIndex(panic_fn, LLVMAttributeFunctionIndex, nr_attr);
    }
  }
}
