#ifndef CODEGEN_FFI_CLANG_IMPL_H
#define CODEGEN_FFI_CLANG_IMPL_H

#include "code.h"
#include <stdbool.h>

/* Internal implementation header for FFI clang import */
void ny_ffi_clang_import(codegen_t *cg, const char *header_path,
                         const char *prefix, bool is_std, const char *lib);

#endif
