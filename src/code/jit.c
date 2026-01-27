#include "code/jit.h"
#include "rt/runtime.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod) {
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
}
