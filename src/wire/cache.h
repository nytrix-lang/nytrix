#ifndef NY_CACHE_H
#define NY_CACHE_H

#include <llvm-c/Core.h>
#include <stdbool.h>

// JIT IR cache functions
bool ny_jit_cache_enabled(void);
char *ny_jit_cache_path(const char *source, const char *stdlib_path);
bool ny_jit_cache_load(const char *cache_path, LLVMContextRef ctx, LLVMModuleRef *out_module);
bool ny_jit_cache_save(const char *cache_path, LLVMModuleRef module);

#endif
