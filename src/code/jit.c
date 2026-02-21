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

void ny_jit_init_native_once(void) {
  static int initialized = 0;
  if (initialized)
    return;
  LLVMLinkInMCJIT();
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  initialized = 1;
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

  const char *dlls[] = {"ucrtbase.dll", "msvcrt.dll", "kernel32.dll",
                        "ws2_32.dll", NULL};
  for (size_t i = 0; dlls[i]; ++i) {
    HMODULE h = GetModuleHandleA(dlls[i]);
    if (!h)
      h = LoadLibraryA(dlls[i]);
    if (!h)
      continue;
    FARPROC sym = GetProcAddress(h, symbol);
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

static void register_extern_symbols(LLVMExecutionEngineRef ee, codegen_t *cg) {
  if (!cg)
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
    void *ptr = resolve_symbol_with_fallback(symbol);
    if (!ptr) {
      NY_LOG_WARN("extern symbol '%s' not found for %s", symbol, sig->name);
      continue;
    }
    LLVMAddGlobalMapping(ee, sig->value, ptr);
  }
}

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                          codegen_t *cg) {
#define MAP(name, fn_ptr)                                                      \
  do {                                                                         \
    LLVMAddSymbol(name, (void *)(uintptr_t)(fn_ptr));                          \
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                        \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)fn_ptr);                           \
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

  register_extern_symbols(ee, cg);
}
