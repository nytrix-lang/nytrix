/* Private AST fallback backend and NYR code-emission interfaces. */
#ifndef NY_CODE_BACKEND_AST_H
#define NY_CODE_BACKEND_AST_H

#include "base/options.h"
#include "code/backend/internal.h"
#include "code/lang/parse/ast.h"

/* Private AST fallback backend and NYR emission entry points. */
bool nyr_dump_function(FILE *out, const program_t *prog, const stmt_t *fn,
                       char *err, size_t err_len, const ny_options *opt);
bool nyr_dump_rt_main(FILE *out, const program_t *prog, char *err,
                      size_t err_len, const ny_options *opt);
bool nyr_dump_program_binary(FILE *out, const program_t *prog,
                             const ny_options *opt, char *err, size_t err_len);
bool nyr_write_ir_metadata_report(const program_t *prog, const ny_options *opt,
                                  char *err, size_t err_len);
bool nyr_target_eval_bool(const ny_options *opt, const expr_t *e, bool *out);
#endif
