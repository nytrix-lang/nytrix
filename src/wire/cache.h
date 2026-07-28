#ifndef NY_CACHE_H
#define NY_CACHE_H

#include <llvm-c/Core.h>
#include <stdbool.h>
#include <stdint.h>

/* Cache path utilities */
bool ny_cache_path_is_ir(const char *cache_path);
const char *ny_cache_root_dir(void);
int ny_cache_clean(void);

bool ny_jit_cache_enabled(void);
/*
 * Return the stable, process-independent identity for source bytes.
 *
 * This deliberately hashes content rather than a path or timestamp: callers
 * use it at the semantic-cache boundary, where equivalent source must select
 * the same artifact regardless of how it was loaded.
 */
uint64_t ny_cache_semantic_fingerprint(const char *source);

/*
 * Return the cached identity of the compiler sources and build metadata that
 * determine semantic interpretation.  It is intended for artifacts that
 * reuse semantic facts, not for user-source cache keys.
 */
uint64_t ny_cache_compiler_source_fingerprint(void);

/*
 * Validate or publish the sidecar for a compiled cache artifact.
 *
 * The manifest binds the artifact to the exact source fingerprint and byte
 * length. A failed validation removes both files so no later path can consume
 * a partial, stale, or manually-corrupted executable artifact.
 */
bool ny_cache_artifact_manifest_valid(const char *cache_path, const char *source);
bool ny_cache_artifact_manifest_save(const char *cache_path, const char *source);
char *ny_jit_cache_path(const char *source, const char *stdlib_path, unsigned long std_src_hash,
                        int opt_level, int opt_dce, int opt_internalize, bool debug_symbols,
                        unsigned long std_latest_mtime,
                        uint64_t semantic_config_fingerprint);
char *ny_std_bc_cache_path(const char *stdlib_path, const char *const *uses, size_t use_count,
                           int std_mode, bool debug_symbols, unsigned long std_latest_mtime,
                           const char *argv0);
bool ny_jit_cache_load(const char *cache_path, LLVMContextRef ctx, LLVMModuleRef *out_module);
bool ny_jit_cache_save(const char *cache_path, LLVMModuleRef module);
bool ny_jit_cache_load_ir(const char *cache_path, LLVMContextRef ctx, LLVMModuleRef *out_module);
bool ny_jit_cache_save_ir(const char *cache_path, LLVMModuleRef module);

#ifndef _WIN32
bool ny_jit_native_cache_enabled(void);
char *ny_jit_native_cache_path(const char *bc_path);
bool ny_jit_native_cache_load(const char *so_path, void **out_handle, void (**out_entry)(void));
bool ny_jit_native_cache_save(const char *so_path, LLVMModuleRef module, int opt_level,
                              const char *const *link_libs, size_t link_count);

/* Stencil-native cache: persist in-memory JIT code as a minimal ELF .so
 * so subsequent runs skip parse + lower + encode entirely. */
char *ny_jit_stencil_cache_path(const char *source, int opt_level);
bool ny_jit_stencil_cache_load(const char *so_path, void **out_handle,
                                void (**out_entry)(void));
bool ny_jit_stencil_cache_save(const char *so_path, const unsigned char *code,
                                size_t code_len, const char *entry_symbol);
#endif

#endif
