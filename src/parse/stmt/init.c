#include "../priv.h"
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

#include "core.c"
#include "comptime.c"
#include "dispatch.c"
