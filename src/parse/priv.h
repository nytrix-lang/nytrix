#ifndef NY_PARSER_INTERNAL_H
#define NY_PARSER_INTERNAL_H

#include "base/common.h"
#include "parse/parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Core Utilities
void parser_advance(parser_t *p);
bool parser_match(parser_t *p, token_kind kind);
void parser_expect(parser_t *p, token_kind kind, const char *msg,
                   const char *hint);
void parser_error(parser_t *p, token_t tok, const char *msg, const char *hint);
token_t parser_peek(parser_t *p);
const char *parser_token_name(token_kind k);
const char *parser_decode_string(parser_t *p, token_t tok, size_t *out_len);
char *parser_unescape_string(arena_t *arena_t, const char *in_str, size_t len,
                             size_t *out_len);

// Expression Parsing
expr_t *p_parse_expr(parser_t *p, int prec);

// Statement Parsing
stmt_t *p_parse_stmt(parser_t *p);
stmt_t *p_parse_block(parser_t *p);
stmt_t *p_parse_match(parser_t *p);

#endif
