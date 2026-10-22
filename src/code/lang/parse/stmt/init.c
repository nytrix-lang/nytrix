/*
 * Statement-parser entry point: dispatches to the appropriate statement
 * sub-parser (core, comptime, dispatch) based on token kind.
 */
#include "../priv.h"
#include "base/util.h"
#include <ctype.h>
#include <stdlib.h>
static attribute_t parse_attr(parser_t *p);
static const char *parse_type_ref(parser_t *p, const char *err_msg);
static bool tok_is_ident_text(token_t tok, const char *text);
static stmt_t *parse_generated_module(parser_t *p, token_t tok,
                                      const char *mod_name, bool export_all);
static stmt_t *impl_clone_for_owner(parser_t *p, stmt_t *base,
                                    const char *from_owner,
                                    const char *to_owner);
static expr_t *ct_int_expr(parser_t *p, token_t tok, int64_t value);
static stmt_t *stmt_new_transparent_block(parser_t *p, token_t tok);
static void stmt_list_push_flat(parser_t *p, ny_stmt_list *out, stmt_t *s);
static void parse_stmt_append_or_sync(parser_t *p, ny_stmt_list *out);
static void parser_register_module_meta(parser_t *p, stmt_t *mod_stmt);
static char *parse_qualified_name(parser_t *p);
static char *parse_dotted_ident_owned(parser_t *p, const char *first_err,
                                      const char *after_dot_err);
static expr_t *make_zero_arg_call_expr(parser_t *p, token_t tok,
                                       const char *callee_name);
static stmt_t *parse_module_profile_stmt(parser_t *p, bool is_internal);

#include "comptime.c"
#include "core.c"
#include "dispatch.c"
