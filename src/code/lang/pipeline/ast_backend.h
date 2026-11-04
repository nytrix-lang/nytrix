#ifndef NY_LANG_PIPELINE_LEGACY_BACKEND_H
#define NY_LANG_PIPELINE_LEGACY_BACKEND_H

#include "base/options.h"
#include "code/backend/internal.h"
#include "code/lang/parse/ast.h"

/* Compatibility adapters that still lower source ASTs directly. Keep these
 * declarations language-private while callers migrate to neutral NYIR. */
bool nyr_dump_function(FILE *out, const program_t *prog, const stmt_t *fn,
                       char *err, size_t err_len, const ny_options *opt);
bool nyr_dump_rt_main(FILE *out, const program_t *prog, char *err,
                      size_t err_len, const ny_options *opt);
bool nyr_dump_program_binary(FILE *out, const program_t *prog,
                             const ny_options *opt, char *err, size_t err_len);
bool nyr_write_ir_metadata_report(const program_t *prog, const ny_options *opt,
                                  char *err, size_t err_len);
bool nyr_target_eval_bool(const ny_options *opt, const expr_t *e, bool *out);
bool nyr_x86_64_emit_rt_main(nyr_writer_t *w,
                             const ny_native_target_info_t *target,
                             const program_t *prog, char *err,
                             size_t err_len);

#endif
