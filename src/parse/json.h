#pragma once

#include "parse/ast.h"
#include <stdbool.h>

char *ny_ast_to_json(program_t *prog);
char *ny_expr_to_json(expr_t *expr);
char *ny_ast_to_json_filtered(program_t *prog, const char *filename);
char *ny_ast_symbols_to_json_filtered(program_t *prog, const char *filename);
char *ny_ast_expand_report(program_t *prog, const char *source_name, const char *filter,
                           const char *explain_specialization, bool meta_trace, bool include_json);
