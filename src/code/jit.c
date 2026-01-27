#include "code/jit.h"
#include "base/common.h"
#include "rt/runtime.h"
#include <dlfcn.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>
#include <string.h>

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
    void *ptr = dlsym(RTLD_DEFAULT, symbol);
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
    LLVMValueRef val = LLVMGetNamedFunction(mod, name);                        \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)fn_ptr);                           \
    }                                                                          \
  } while (0)

#define MAP_GV(name, ptr)                                                      \
  do {                                                                         \
    LLVMValueRef val = LLVMGetNamedGlobal(mod, name);                          \
    if (val) {                                                                 \
      LLVMAddGlobalMapping(ee, val, (void *)ptr);                              \
    }                                                                          \
  } while (0)

#define RT_DEF(name, p, args, sig, doc) MAP(name, p);
#define RT_GV(name, p, t, doc) MAP_GV(name, &p);

#include "rt/defs.h"

#undef RT_DEF
#undef RT_GV
#undef MAP
#undef MAP_GV

  register_extern_symbols(ee, cg);
}
