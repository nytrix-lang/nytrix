#ifndef NY_CACHE_H
#define NY_CACHE_H

#include <llvm-c/Core.h>
#include <stdbool.h>

bool ny_jit_cache_enabled(void);
char *ny_jit_cache_path(const char *source, const char *stdlib_path,
                        unsigned long std_src_hash, int opt_level, int opt_dce,
                        int opt_internalize, bool debug_symbols,
                        unsigned long std_latest_mtime);
bool ny_jit_cache_load(const char *cache_path, LLVMContextRef ctx,
                       LLVMModuleRef *out_module);
bool ny_jit_cache_save(const char *cache_path, LLVMModuleRef module);
bool ny_jit_cache_load_ir(const char *cache_path, LLVMContextRef ctx,
                          LLVMModuleRef *out_module);
bool ny_jit_cache_save_ir(const char *cache_path, LLVMModuleRef module);

#ifndef _WIN32
bool ny_jit_native_cache_enabled(void);
char *ny_jit_native_cache_path(const char *bc_path);
bool ny_jit_native_cache_load(const char *so_path, void **out_handle,
                              void (**out_entry)(void));
bool ny_jit_native_cache_save(const char *so_path, LLVMModuleRef module,
                              int opt_level, const char *const *link_libs,
                              size_t link_count);
#endif

#endif
