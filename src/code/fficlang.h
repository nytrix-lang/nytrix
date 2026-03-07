#ifndef NYTRIX_FFICLANG_H
#define NYTRIX_FFICLANG_H

#include "code.h"
#include <stdbool.h>

/**
 * Accumulated FFI directives are stored in the codegen_t session.
 * Call these to add setup steps, then process them all at once.
 */

void ny_ffi_clang_define(codegen_t *cg, const char *macro);
void ny_ffi_clang_include(codegen_t *cg, const char *header_path, const char *prefix, bool is_std,
                          const char *lib);

/**
 * Parses all accumulated headers and defines using libclang.
 * This should be called after all directives in a block are collected.
 */
void ny_ffi_clang_process(codegen_t *cg);

/* Legacy entry point for single-line includes */
void ny_ffi_clang_import(codegen_t *cg, const char *header_path, const char *prefix, bool is_std,
                         const char *lib);

#endif
