/* Parser interfaces for proof obligations and typed logical expressions. */
#ifndef NY_PARSE_PROOF_H
#define NY_PARSE_PROOF_H

#include "code/lang/ny/parser/ast.h"
#include "code/lang/ny/parser/parser.h"

/* Returns an owned normalized `proof<...>` type name for a proposition AST. */
char *ny_proof_type_from_expr(expr_t *expr);
/* Parse the supported canonical proposition subset into arena-owned AST. */
expr_t *ny_proof_expr_from_canonical(arena_t *arena, const char *text);
/* Saturated integer payloads cannot be used as compile-time proof evidence. */
bool ny_proof_has_wide_integer(expr_t *expr);
const char *parser_parse_proof_type_arg(parser_t *p);

/* Structural match of two canonical proposition strings (with or without the
 * `proof<`/`>` wrappers). Literal tokens must be identical; `name:X` tokens
 * match any `name:Y` — they denote values whose range satisfaction is
 * enforced later by the codegen proof-parameter check. */
bool ny_proof_proposition_shape_matches(const char *a, const char *b);

#define NY_PROOF_CANON_MAX_DEPTH 64

typedef bool (*ny_proof_name_range_fn)(void *ctx, const char *name,
                                       int64_t *out_lo, int64_t *out_hi);

int ny_proof_canon_decide(const char *s, ny_proof_name_range_fn env, void *ctx,
                          int depth);

bool ny_proof_canon_range(const char *s, ny_proof_name_range_fn env, void *ctx,
                          int64_t *out_lo, int64_t *out_hi, int depth);

bool ny_proof_canon_split_binary(const char *s, char *opbuf, size_t opcap,
                                 char **out_l, char **out_r);

/* Equality kernel rules (eq_sym, eq_trans, eq_cong, transport): compute the
 * result proof type from the argument proof types; NULL + reason on misuse. */
/* `transport(p, v)` for a value: the type of `v` with one side of the
 * equation `p` rewritten to the other where it is a type argument. */
char *ny_proof_index_transport(const char *equation, const char *type,
                               const char **reason);
char *ny_proof_rule_result(const char *rule, const char *const *proofs,
                           size_t proof_count, const char *fn_name,
                           const char **reason);
bool ny_proof_rule_name(const char *leaf);

#endif
