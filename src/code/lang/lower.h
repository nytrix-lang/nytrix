#ifndef NY_CODE_LANG_LOWER_H
#define NY_CODE_LANG_LOWER_H

#include "base/options.h"
#include "code/lang/parse/ast.h"
#include <nyr/ir.h>
#include <stddef.h>

/* Language-owned source adapter that lowers a program into an owned NYIR
 * module. Callers release the entry and each populated function with
 * nyr_ir_func_free. */
bool nyr_build_ir(const program_t *program, const ny_options *options,
                  nyr_func_t *entry_out, nyr_func_t *functions_out,
                  size_t *function_count, const char **function_names_out,
                  size_t function_capacity, char *err, size_t err_len);

/* Resolve source-runtime aliases before the backend sees runtime symbols. */
const char *nyr_runtime_symbol(const char *name);

#endif
