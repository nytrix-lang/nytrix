#ifndef NY_PARSE_PROOF_H
#define NY_PARSE_PROOF_H

#include "code/parse/ast.h"
#include "code/parse/parser.h"

/* Returns an owned normalized `proof<...>` type name for a proposition AST. */
char *ny_proof_type_from_expr(expr_t *expr);
const char *parser_parse_proof_type_arg(parser_t *p);

/* Structural match of two canonical proposition strings (with or without the
 * `proof<`/`>` wrappers). Literal tokens must be identical; `name:X` tokens
 * match any `name:Y` — they denote values whose range satisfaction is
 * enforced later by the codegen proof-parameter check. */
bool ny_proof_proposition_shape_matches(const char *a, const char *b);

#define NY_PROOF_CANON_MAX_DEPTH 64

typedef bool (*ny_proof_name_range_fn)(void *ctx, const char *name,
                                       int64_t *out_lo, int64_t *out_hi);

int ny_proof_canon_decide(const char *s, ny_proof_name_range_fn env,
                          void *ctx, int depth);

bool ny_proof_canon_range(const char *s, ny_proof_name_range_fn env,
                          void *ctx, int64_t *out_lo, int64_t *out_hi,
                          int depth);

bool ny_proof_canon_split_binary(const char *s, char *opbuf,
                                 size_t opcap, char **out_l,
                                 char **out_r);

#endif
