#ifndef NY_PARSER_INTERNAL_H
#define NY_PARSER_INTERNAL_H

#include "base/common.h"
#include "parse/parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void parser_advance(parser_t *p) {
  p->prev = p->cur;
  p->cur = lexer_next(&p->lex);
  p->skipped_newline = p->lex.skipped_newline;
}

static inline bool parser_match(parser_t *p, token_kind kind) {
  if (p->cur.kind == kind) {
    parser_advance(p);
    return true;
  }
  return false;
}
void parser_expect_slow(parser_t *p, token_kind kind, const char *msg,
                        const char *hint);
static inline void parser_expect(parser_t *p, token_kind kind, const char *msg,
                                 const char *hint) {
  if (p->cur.kind == kind) {
    parser_advance(p);
    return;
  }
  parser_expect_slow(p, kind, msg, hint);
}
void parser_error(parser_t *p, token_t tok, const char *msg, const char *hint);
token_t parser_peek(parser_t *p);
const char *parser_token_name(token_kind k);
const char *parser_decode_string(parser_t *p, token_t tok, size_t *out_len);
char *parser_unescape_string(arena_t *arena_t, const char *in_str, size_t len,
                             size_t *out_len);
const char *parser_intern_hash(parser_t *p, const char *s, size_t len,
                               uint64_t hash);
static inline const char *parser_intern(parser_t *p, const char *s,
                                        size_t len) {
  return parser_intern_hash(p, s, len, 0);
}

expr_t *p_parse_expr(parser_t *p, int prec);

stmt_t *p_parse_stmt(parser_t *p);
stmt_t *p_parse_block(parser_t *p);
stmt_t *p_parse_match(parser_t *p);
stmt_t *ny_parse_stmt_or_block(parser_t *p);
stmt_t *ny_parse_if_stmt(parser_t *p);
stmt_t *ny_parse_while_stmt(parser_t *p);
stmt_t *ny_parse_for_stmt(parser_t *p);
stmt_t *ny_parse_try_stmt(parser_t *p);
stmt_t *ny_parse_return_stmt(parser_t *p);
stmt_t *ny_parse_break_stmt(parser_t *p);
stmt_t *ny_parse_continue_stmt(parser_t *p);
stmt_t *ny_parse_goto_stmt(parser_t *p);

#endif
