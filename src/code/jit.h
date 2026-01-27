#ifndef NY_JIT_SYMBOLS_H
#define NY_JIT_SYMBOLS_H

#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>
#include "code/code.h"

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                          codegen_t *cg);
int64_t __set_args(int64_t argc, int64_t argv, int64_t envp);

#endif
