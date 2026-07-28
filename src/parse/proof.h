#ifndef NY_PARSE_PROOF_H
#define NY_PARSE_PROOF_H

#include "parse/ast.h"
#include "parse/parser.h"

/* Returns an owned normalized `proof<...>` type name for a proposition AST. */
char *ny_proof_type_from_expr(expr_t *expr);
const char *parser_parse_proof_type_arg(parser_t *p);

#endif
