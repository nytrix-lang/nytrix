#ifndef NY_JIT_SYMBOLS_H
#define NY_JIT_SYMBOLS_H

#include "code/code.h"
#include <llvm-c/ExecutionEngine.h>
#include <stdint.h>

void ny_jit_init_native_once(void);
bool ny_jit_prepare_execution(uint64_t address);
bool ny_jit_prepare_module_execution(LLVMExecutionEngineRef ee, LLVMModuleRef mod);
void *ny_jit_resolve_symbol(const char *symbol);
void *ny_jit_load_library(const char *path);
void ny_jit_map_unresolved_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod,
                                   const char *entry_name);
void ny_jit_add_runtime_symbols(void);
void ny_jit_define_runtime_trampolines(LLVMModuleRef mod);

void register_jit_symbols(LLVMExecutionEngineRef ee, LLVMModuleRef mod, codegen_t *cg);
void ny_jit_write_perf_map(LLVMExecutionEngineRef ee, LLVMModuleRef mod);
void ny_jit_init_options(struct LLVMMCJITCompilerOptions *options, LLVMModuleRef mod);
bool ny_orc_jit_ensure_engine(codegen_t *cg, char **error_message);
bool ny_orc_jit_execute(codegen_t *cg, LLVMModuleRef module,
                        LLVMContextRef context, uint64_t *script_addr,
                        uint64_t *main_addr, void **out_rt,
                        bool *module_consumed, char **error_message);
void ny_orc_jit_remove_module(void *rt);
void ny_orc_jit_dispose(void *jit);
int64_t rt_set_args(int64_t argc, int64_t argv, int64_t envp);

#endif
