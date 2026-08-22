#ifndef NY_PARSE_PROOF_H
#define NY_PARSE_PROOF_H

#include "parse/ast.h"
#include "parse/parser.h"

/* Returns an owned normalized `proof<...>` type name for a proposition AST. */
char *ny_proof_type_from_expr(expr_t *expr);
const char *parser_parse_proof_type_arg(parser_t *p);

/* Structural match of two canonical proposition strings (with or without the
 * `proof<`/`>` wrappers). Literal tokens must be identical; `name:X` tokens
 * match any `name:Y` — they denote values whose range satisfaction is
 * enforced later by the codegen proof-parameter check. */
bool ny_proof_proposition_shape_matches(const char *a, const char *b);

#endif
