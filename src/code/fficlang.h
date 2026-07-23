#ifndef NYTRIX_FFICLANG_H
#define NYTRIX_FFICLANG_H

#include "code.h"
#include <stdbool.h>

void ny_ffi_clang_define(codegen_t *cg, const char *macro);
/* Legacy entry point for single-line includes */
void ny_ffi_clang_import(codegen_t *cg, const char *header_path, const char *prefix, bool is_std,
                         const char *lib);

/**
 * Set or clear a global deadline (nanoseconds from CLOCK_MONOTONIC) applied to
 * all nytrix C frontend parsers during FFI processing. Pass 0 to clear.
 */
void ny_ffi_set_global_deadline(int64_t deadline_ns);

#endif
