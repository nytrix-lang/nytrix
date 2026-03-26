#ifndef NY_JIT_SYMBOLS_H
#define NY_JIT_SYMBOLS_H

#include "code/code.h"
#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>

void ny_jit_init_native_once(void);
void ny_jit_prepare_execution(void);
void *ny_jit_resolve_symbol(const char *symbol);
void ny_jit_map_unresolved_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                   const char *entry_name);
void ny_jit_add_runtime_symbols(void);
void ny_jit_define_runtime_trampolines(LLVMModuleRef mod);

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod, codegen_t *cg);
void ny_jit_write_perf_map(LLVMExecutionEngineRef ee, LLVMModuleRef mod);
void ny_jit_init_options(struct LLVMMCJITCompilerOptions *options, LLVMModuleRef mod);
int64_t rt_set_args(int64_t argc, int64_t argv, int64_t envp);

#endif
