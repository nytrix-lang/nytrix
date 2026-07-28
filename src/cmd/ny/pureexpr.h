#ifndef NY_PUREEXPR_H
#define NY_PUREEXPR_H

#include <stdbool.h>
#include <stdint.h>

/* Pure i64 arithmetic (no identifiers/calls). Used by the thin `ny` launcher
 * so `--native-only -c '1+2*20+2'` never loads LLVM/Z3/libclang. */
bool ny_pure_expr_eval(const char *src, int64_t *out);

/* True when argv is only --native-only + -c EXPR (+ optional noise flags). */
bool ny_pure_native_c_match(int argc, char **argv, const char **expr_out);

#endif
