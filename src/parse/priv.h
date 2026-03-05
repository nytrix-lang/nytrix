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
  if (p->lex.error_count > p->lex_error_count_seen) {
    int delta = p->lex.error_count - p->lex_error_count_seen;
    p->lex_error_count_seen = p->lex.error_count;
    p->had_error = true;
    p->error_count += delta;
    if (p->error_limit > 0 && p->error_count >= p->error_limit)
      p->cur.kind = NY_T_EOF;
  }
  p->skipped_newline = p->lex.skipped_newline;
}

static inline bool parser_match(parser_t *p, token_kind kind) {
  if (p->cur.kind == kind) {
    parser_advance(p);
    return true;
  }
  return false;
}

static inline void parser_sync_stmt_boundary(parser_t *p) {
  while (p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_SEMI && p->cur.kind != NY_T_RBRACE) {
    parser_advance(p);
  }
  if (p->cur.kind == NY_T_SEMI)
    parser_advance(p);
}

void parser_expect_slow(parser_t *p, token_kind kind, const char *msg, const char *hint);
static inline void parser_expect(parser_t *p, token_kind kind, const char *msg, const char *hint) {
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
char *parser_unescape_string(arena_t *arena_t, const char *in_str, size_t len, size_t *out_len);
const char *parser_intern_hash(parser_t *p, const char *s, size_t len, uint64_t hash);
static inline const char *parser_intern(parser_t *p, const char *s, size_t len) {
  return parser_intern_hash(p, s, len, 0);
}

static inline bool parser_token_is_builtin_type(token_t tok) {
  if (tok.kind != NY_T_IDENT)
    return false;
  const char *s = tok.lexeme;
  size_t n = tok.len;
#define NY_PARSER_TYPE_EQ(lit) (n == sizeof(lit) - 1 && memcmp(s, lit, sizeof(lit) - 1) == 0)
  return NY_PARSER_TYPE_EQ("any") || NY_PARSER_TYPE_EQ("int") || NY_PARSER_TYPE_EQ("i8") || NY_PARSER_TYPE_EQ("i16") ||
         NY_PARSER_TYPE_EQ("i32") || NY_PARSER_TYPE_EQ("i64") || NY_PARSER_TYPE_EQ("i128") ||
         NY_PARSER_TYPE_EQ("u8") || NY_PARSER_TYPE_EQ("u16") || NY_PARSER_TYPE_EQ("u32") ||
         NY_PARSER_TYPE_EQ("u64") || NY_PARSER_TYPE_EQ("u128") || NY_PARSER_TYPE_EQ("str") ||
         NY_PARSER_TYPE_EQ("char") || NY_PARSER_TYPE_EQ("bool") || NY_PARSER_TYPE_EQ("f32") ||
         NY_PARSER_TYPE_EQ("f64") || NY_PARSER_TYPE_EQ("f128") || NY_PARSER_TYPE_EQ("ptr") ||
         NY_PARSER_TYPE_EQ("fnptr") || NY_PARSER_TYPE_EQ("number") ||
         NY_PARSER_TYPE_EQ("bigint") || NY_PARSER_TYPE_EQ("bytes") ||
         NY_PARSER_TYPE_EQ("list") || NY_PARSER_TYPE_EQ("dict") ||
         NY_PARSER_TYPE_EQ("set") || NY_PARSER_TYPE_EQ("tuple") ||
         NY_PARSER_TYPE_EQ("range") || NY_PARSER_TYPE_EQ("void") ||
         NY_PARSER_TYPE_EQ("seq") ||
         NY_PARSER_TYPE_EQ("handle") || NY_PARSER_TYPE_EQ("vec2") || NY_PARSER_TYPE_EQ("vec3") ||
         NY_PARSER_TYPE_EQ("vec4");
#undef NY_PARSER_TYPE_EQ
}

static inline bool parser_type_ref_is_builtin_type(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  const char *tail = strrchr(type_name, '.');
  tail = tail ? tail + 1 : type_name;
#define NY_PARSER_TYPE_REF_EQ(lit) (strcmp(tail, lit) == 0)
  return NY_PARSER_TYPE_REF_EQ("any") || NY_PARSER_TYPE_REF_EQ("int") || NY_PARSER_TYPE_REF_EQ("i8") ||
         NY_PARSER_TYPE_REF_EQ("i16") || NY_PARSER_TYPE_REF_EQ("i32") ||
         NY_PARSER_TYPE_REF_EQ("i64") || NY_PARSER_TYPE_REF_EQ("i128") ||
         NY_PARSER_TYPE_REF_EQ("u8") || NY_PARSER_TYPE_REF_EQ("u16") ||
         NY_PARSER_TYPE_REF_EQ("u32") || NY_PARSER_TYPE_REF_EQ("u64") ||
         NY_PARSER_TYPE_REF_EQ("u128") || NY_PARSER_TYPE_REF_EQ("str") ||
         NY_PARSER_TYPE_REF_EQ("char") || NY_PARSER_TYPE_REF_EQ("bool") ||
         NY_PARSER_TYPE_REF_EQ("f32") || NY_PARSER_TYPE_REF_EQ("f64") ||
         NY_PARSER_TYPE_REF_EQ("f128") || NY_PARSER_TYPE_REF_EQ("ptr") ||
         NY_PARSER_TYPE_REF_EQ("fnptr") || NY_PARSER_TYPE_REF_EQ("number") ||
         NY_PARSER_TYPE_REF_EQ("bigint") || NY_PARSER_TYPE_REF_EQ("bytes") ||
         NY_PARSER_TYPE_REF_EQ("list") || NY_PARSER_TYPE_REF_EQ("dict") ||
         NY_PARSER_TYPE_REF_EQ("set") || NY_PARSER_TYPE_REF_EQ("tuple") ||
         NY_PARSER_TYPE_REF_EQ("range") || NY_PARSER_TYPE_REF_EQ("void") ||
         NY_PARSER_TYPE_REF_EQ("seq") ||
         NY_PARSER_TYPE_REF_EQ("handle") || NY_PARSER_TYPE_REF_EQ("vec2") ||
         NY_PARSER_TYPE_REF_EQ("vec3") || NY_PARSER_TYPE_REF_EQ("vec4");
#undef NY_PARSER_TYPE_REF_EQ
}

typedef const char *(*parser_type_ref_fn)(parser_t *p, const char *err_msg);

static inline bool parser_parse_param_type_first(parser_t *p, param_t *pr,
                                                parser_type_ref_fn parse_type_ref) {
  if (p->cur.kind == NY_T_IDENT && parser_peek(p).kind != NY_T_COLON &&
      parser_peek(p).kind != NY_T_DOT && parser_peek(p).kind != NY_T_LT) {
    pr->name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    return true;
  }
  if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER && p->cur.kind != NY_T_QUESTION &&
      p->cur.kind != NY_T_STAR) {
    parser_error(p, p->cur, "param must be identifier or type", NULL);
    return false;
  }
  pr->type = parse_type_ref(p, "expected parameter type");
  if (!parser_match(p, NY_T_COLON)) {
    parser_error(p, p->cur, "expected ':' after parameter type",
                 "typed parameters use 'type: name', for example 'f64: spacing'");
    return false;
  }
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected parameter name after ':'", NULL);
    return false;
  }
  if (parser_token_is_builtin_type(p->cur) && !parser_type_ref_is_builtin_type(pr->type)) {
    parser_error(p, p->cur, "typed parameters are type-first",
                 "write 'int: value', not 'value: int'");
    return false;
  }
  pr->name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  return true;
}

expr_t *p_parse_expr(parser_t *p, int prec);

stmt_t *p_parse_stmt(parser_t *p);
stmt_t *p_parse_block(parser_t *p);
stmt_t *p_parse_match(parser_t *p);
stmt_t *ny_parse_stmt_or_block(parser_t *p);
stmt_t *ny_parse_if_stmt(parser_t *p);
stmt_t *ny_parse_while_stmt(parser_t *p);
stmt_t *ny_parse_for_stmt(parser_t *p);
stmt_t *ny_parse_while_stmt_with_attr(parser_t *p, const char *attr_name, size_t attr_len);
stmt_t *ny_parse_try_stmt(parser_t *p);
stmt_t *ny_parse_return_stmt(parser_t *p);
stmt_t *ny_parse_break_stmt(parser_t *p);
stmt_t *ny_parse_continue_stmt(parser_t *p);
stmt_t *ny_parse_goto_stmt(parser_t *p);

#endif
