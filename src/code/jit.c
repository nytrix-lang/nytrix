#include "code/jit.h"
#include "base/common.h"
#include "rt/runtime.h"
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <stdint.h>
#include <string.h>
#include "priv.h"

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
  return (void *)h;
#else
  void *h = dlopen(path, RTLD_GLOBAL | RTLD_LAZY);
  if (!h) {
    // Try with .so suffix if missing
    if (!strchr(path, '.')) {
      char buf[256];
      snprintf(buf, sizeof(buf), "lib%s.so", path);
      h = dlopen(buf, RTLD_GLOBAL | RTLD_LAZY);
    }
  }
  return h;
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
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    const char *name = LLVMGetValueName(f);
    if (!name || !*name)
      continue;
    if (LLVMCountBasicBlocks(f) > 0 &&
        (!entry_name || strcmp(name, entry_name) != 0))
      continue;
    void *addr = resolve_symbol_with_fallback(name);
    if (addr)
      LLVMAddGlobalMapping(ee, f, addr);
  }
}

static void register_extern_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                    codegen_t *cg) {
  if (!cg || !mod)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->is_extern)
      continue;

    const char *symbol = sig->link_name;
    if (!symbol) {
      const char *dot = strrchr(sig->name, '.');
      symbol = dot ? dot + 1 : sig->name;
    }

    /* Look up the function in the current module.
       The function might be named by its qualified name or its link name. */
    LLVMValueRef val = LLVMGetNamedFunction(mod, symbol);
    if (!val) {
      val = LLVMGetNamedFunction(mod, sig->name);
    }
    if (!val)
      continue;

    void *ptr = resolve_symbol_with_fallback(symbol);
    if (!ptr) {
      NY_LOG_WARN("extern symbol '%s' not found for %s", symbol, sig->name);
      continue;
    }

    LLVMAddGlobalMapping(ee, val, ptr);
  }
}

void register_jit_sigs(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                       codegen_t *cg) {
  (void)ee; (void)mod; (void)cg;
  // For non-extern functions, they are defined within the JIT.
}

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                          codegen_t *cg) {
  if (cg) {
    for (size_t i = 0; i < cg->links.len; i++) {
      ny_jit_load_library(cg->links.data[i]);
    }
  }
#define MAP(name, fn_ptr)                                                      \
  do {                                                                         \
    LLVMAddSymbol(name, (void *)(uintptr_t)(fn_ptr));                          \
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                        \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)(uintptr_t)fn_ptr);                \
    }                                                                          \
  } while (0)
#define MAP_GV(name, ptr)                                                      \
  do {                                                                         \
    LLVMAddSymbol(name, (void *)(ptr));                                        \
    LLVMValueRef val = LLVMGetNamedGlobal(mod, name);                          \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)ptr);                              \
    }                                                                          \
  } while (0)
#define RT_DEF(name, p, args, sig, doc)                                        \
  do {                                                                         \
    MAP(name, p);                                                              \
    if (strcmp(name, #p) != 0)                                                 \
      MAP(#p, p);                                                              \
  } while (0);
#define RT_GV(name, p, t, doc) MAP_GV(name, &p);
#include "rt/defs.h"
#undef RT_DEF
#undef RT_GV
#undef MAP
#undef MAP_GV
  register_extern_symbols(ee, mod, cg);
  register_jit_sigs(ee, mod, cg);
}
