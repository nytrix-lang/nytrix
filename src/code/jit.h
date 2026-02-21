#ifndef NY_JIT_SYMBOLS_H
#define NY_JIT_SYMBOLS_H

#include "code/code.h"
#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>

void ny_jit_init_native_once(void);
void *ny_jit_resolve_symbol(const char *symbol);
void ny_jit_map_unresolved_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                   const char *entry_name);

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                          codegen_t *cg);
int64_t __set_args(int64_t argc, int64_t argv, int64_t envp);

#endif
