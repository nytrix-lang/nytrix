#ifndef NY_JIT_SYMBOLS_H
#define NY_JIT_SYMBOLS_H

#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod);

// Expose runtime symbols for JIT driver in main.c
int64_t __set_args(int64_t argc, int64_t argv, int64_t envp);
// Add others if needed by main.c directly (most are only needed via
// dlsym/mapping)

#endif
