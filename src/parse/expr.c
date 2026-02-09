#include "priv.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

static bool suffix_eq(const char *s, size_t len, const char *lit) {
  size_t llen = strlen(lit);
  if (len != llen)
    return false;
  for (size_t i = 0; i < llen; i++) {
    if (tolower((unsigned char)s[i]) != lit[i])
      return false;
  }
  return true;
}

static bool hint_is_float(lit_type_hint_t hint) {
  return hint == NY_LIT_HINT_F32 || hint == NY_LIT_HINT_F64 ||
         hint == NY_LIT_HINT_F128;
}

static lit_type_hint_t infer_int_hint(int64_t val) {
  if (val >= INT32_MIN && val <= INT32_MAX)
    return NY_LIT_HINT_I32;
  return NY_LIT_HINT_I64;
}

static bool check_int_range(parser_t *p, token_t tok, uint64_t val,
                            lit_type_hint_t hint) {
  switch (hint) {
  case NY_LIT_HINT_I8:
    return val <= (uint64_t)INT8_MAX;
  case NY_LIT_HINT_I16:
    return val <= (uint64_t)INT16_MAX;
  case NY_LIT_HINT_I32:
    return val <= (uint64_t)INT32_MAX;
  case NY_LIT_HINT_I64:
    return val <= (uint64_t)INT64_MAX;
  case NY_LIT_HINT_U8:
    return val <= UINT8_MAX;
  case NY_LIT_HINT_U16:
    return val <= UINT16_MAX;
  case NY_LIT_HINT_U32:
    return val <= UINT32_MAX;
  case NY_LIT_HINT_U64:
    return true;
  default:
    break;
  }
  parser_error(p, tok, "integer literal out of range for suffix", NULL);
  return false;
}

static bool parse_numeric_suffix(const char *s, size_t len, size_t *num_len,
                                 lit_type_hint_t *hint,
                                 bool *hint_explicit) {
  *num_len = len;
  *hint = NY_LIT_HINT_NONE;
  *hint_explicit = false;
  if (len < 2)
    return true;
  size_t i = len;
  while (i > 0 && isdigit((unsigned char)s[i - 1]))
    i--;
  if (i == len)
    return true; // no trailing digits
  if (i == 0)
    return true;
  char c = s[i - 1];
  bool is_hex = (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
  if (c != 'i' && c != 'I' && c != 'u' && c != 'U' && c != 'f' && c != 'F')
    return true;
  if (is_hex && (c == 'f' || c == 'F')) {
    return true;
  }
  const char *suffix = s + i - 1;
  size_t suffix_len = len - (i - 1);
  if (suffix_eq(suffix, suffix_len, "i8"))
    *hint = NY_LIT_HINT_I8;
  else if (suffix_eq(suffix, suffix_len, "i16"))
    *hint = NY_LIT_HINT_I16;
  else if (suffix_eq(suffix, suffix_len, "i32"))
    *hint = NY_LIT_HINT_I32;
  else if (suffix_eq(suffix, suffix_len, "i64"))
    *hint = NY_LIT_HINT_I64;
  else if (suffix_eq(suffix, suffix_len, "u8"))
    *hint = NY_LIT_HINT_U8;
  else if (suffix_eq(suffix, suffix_len, "u16"))
    *hint = NY_LIT_HINT_U16;
  else if (suffix_eq(suffix, suffix_len, "u32"))
    *hint = NY_LIT_HINT_U32;
  else if (suffix_eq(suffix, suffix_len, "u64"))
    *hint = NY_LIT_HINT_U64;
  else if (suffix_eq(suffix, suffix_len, "f32"))
    *hint = NY_LIT_HINT_F32;
  else if (suffix_eq(suffix, suffix_len, "f64"))
    *hint = NY_LIT_HINT_F64;
  else if (suffix_eq(suffix, suffix_len, "f128"))
    *hint = NY_LIT_HINT_F128;
  else
    return false;

  *hint_explicit = true;
  *num_len = i - 1;
  return true;
}

static bool parse_type_name(parser_t *p, char **out_name) {
  parser_t save = *p;
  size_t ptr_depth = 0;
  while (parser_match(p, NY_T_STAR))
    ptr_depth++;

  if (p->cur.kind != NY_T_IDENT) {
    *p = save;
    return false;
  }

  size_t cap = 64;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memcpy(buf, p->cur.lexeme, p->cur.len);
  len += p->cur.len;
  parser_advance(p);

  while (parser_match(p, NY_T_DOT)) {
    if (p->cur.kind != NY_T_IDENT) {
      free(buf);
      *p = save;
      return false;
    }
    if (len + 1 + p->cur.len + 1 > cap) {
      cap = (len + 1 + p->cur.len + 1) * 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      buf = nb;
    }
    buf[len++] = '.';
    memcpy(buf + len, p->cur.lexeme, p->cur.len);
    len += p->cur.len;
    parser_advance(p);
  }

  if (p->cur.kind != NY_T_RPAREN) {
    free(buf);
    *p = save;
    return false;
  }

  size_t total = ptr_depth + len;
  char *out = arena_alloc(p->arena, total + 1);
  for (size_t i = 0; i < ptr_depth; i++)
    out[i] = '*';
  memcpy(out + ptr_depth, buf, len);
  out[total] = '\0';
  free(buf);
  *out_name = out;
  return true;
}

static int precedence(token_kind kind) {
  switch (kind) {
  case NY_T_OR:
    return 1;
  case NY_T_AND:
    return 2;
  case NY_T_EQ:
  case NY_T_NEQ:
    return 3;
  case NY_T_LT:
  case NY_T_GT:
  case NY_T_LE:
  case NY_T_GE:
    return 4;
  case NY_T_PLUS:
  case NY_T_MINUS:
    return 5;
  case NY_T_STAR:
  case NY_T_SLASH:
  case NY_T_PERCENT:
    return 6;
  case NY_T_BITOR:
  case NY_T_BITAND:
  case NY_T_BITXOR:
  case NY_T_LSHIFT:
  case NY_T_RSHIFT:
    return 7;
  default:
    return 0;
  }
}

static const char *decode_fstring_part(parser_t *p, const char *s, size_t len,
                                       size_t *out_len) {
  return parser_unescape_string(p->arena, s, len, out_len);
}

static expr_t *parse_fstring(parser_t *p, token_t tok) {
  parser_advance(p);
  expr_t *e = expr_new(p->arena, NY_E_FSTRING, tok);
  const char *s = tok.lexeme;
  size_t len = tok.len;
  // Skip 'f' prefix
  s++;
  len--;
  char quote = *s;
  bool triple = (len >= 6 && s[1] == quote && s[2] == quote);
  s += triple ? 3 : 1;
  len -= triple ? 6 : 2;
  size_t i = 0;
  while (i < len) {
    if (s[i] == '{') {
      i++;
      size_t start = i;
      int depth = 1;
      while (i < len && depth > 0) {
        if (s[i] == '{')
          depth++;
        else if (s[i] == '}')
          depth--;
        if (depth > 0)
          i++;
      }
      if (depth == 0) {
        char *expr_str = arena_strndup(p->arena, s + start, i - start);
        parser_t sub;
        parser_init_with_arena(&sub, expr_str, p->lex.filename, p->arena);
        expr_t *sub_e = p_parse_expr(&sub, 0);
        // Keep arena_t state in sync called parser_init_with_arena which takes
        // arena_t ptr But sub.arena_t is same pointer. Allocations happened on
        // it. p->arena doesn't change typically.
        fstring_part_t part = {.kind = NY_FSP_EXPR, .as.e = sub_e};
        vec_push_arena(p->arena, &e->as.fstring.parts, part);
        i++; // skip '}'
      } else {
        parser_error(p, tok, "unterminated interpolation in f-string", NULL);
        break;
      }
    } else {
      size_t start = i;
      while (i < len && s[i] != '{') {
        if (s[i] == '\\' && i + 1 < len)
          i += 2;
        else
          i++;
      }
      fstring_part_t part;
      part.kind = NY_FSP_STR;
      part.as.s.data =
          decode_fstring_part(p, s + start, i - start, &part.as.s.len);
      vec_push_arena(p->arena, &e->as.fstring.parts, part);
    }
  }
  return e;
}

static expr_t *parse_primary(parser_t *p) {
  token_t tok = p->cur;
  switch (tok.kind) {
  case NY_T_COMPTIME: {
    parser_advance(p);
    stmt_t *body = NULL;
    if (p->cur.kind == NY_T_LBRACE) {
      body = p_parse_block(p);
    } else {
      expr_t *val = p_parse_expr(p, 0);
      stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, tok);
      ret->as.ret.value = val;
      body = stmt_new(p->arena, NY_S_BLOCK, tok);
      vec_push_arena(p->arena, &body->as.block.body, ret);
    }
    expr_t *e = expr_new(p->arena, NY_E_COMPTIME, tok);
    e->as.comptime_expr.body = body;
    return e;
  }
  case NY_T_IDENT: {
    parser_advance(p);
    /* Omniscience: Help users from other languages */
    if (tok.len == 4 && strncmp(tok.lexeme, "null", 4) == 0) {
      parser_error(p, tok, "unrecognised identifier 'null'",
                   "did you mean '0' or 'nil'?");
    } else if (tok.len == 4 && strncmp(tok.lexeme, "None", 4) == 0) {
      parser_error(p, tok, "unrecognised identifier 'None'",
                   "did you mean '0' or 'nil'?");
    }
    expr_t *id = expr_new(p->arena, NY_E_IDENT, tok);
    id->as.ident.name = arena_strndup(p->arena, tok.lexeme, tok.len);
    return id;
  }
  case NY_T_NUMBER: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    size_t num_len = tok.len;
    lit_type_hint_t hint = NY_LIT_HINT_NONE;
    bool hint_explicit = false;
    if (!parse_numeric_suffix(tok.lexeme, tok.len, &num_len, &hint,
                              &hint_explicit)) {
      parser_error(p, tok, "unknown numeric literal suffix", NULL);
    }

    char *num_buf = arena_strndup(p->arena, tok.lexeme, num_len);
    bool is_hex = (num_len > 2 && num_buf[0] == '0' &&
                   (num_buf[1] == 'x' || num_buf[1] == 'X'));
    bool is_float = !is_hex &&
                    (memchr(num_buf, '.', num_len) ||
                     memchr(num_buf, 'e', num_len) ||
                     memchr(num_buf, 'E', num_len));

    if (hint_is_float(hint) || (!hint_explicit && is_float)) {
      if (is_hex) {
        parser_error(p, tok,
                     "hexadecimal float literals are not supported yet", NULL);
        lit->as.literal.kind = NY_LIT_FLOAT;
        lit->as.literal.as.f = 0.0;
      } else {
        errno = 0;
        double val = strtod(num_buf, NULL);
        if (errno == ERANGE)
          parser_error(p, tok, "float literal out of range", NULL);
        lit->as.literal.kind = NY_LIT_FLOAT;
        lit->as.literal.as.f = val;
      }
      if (!hint_explicit)
        hint = NY_LIT_HINT_F64;
    } else {
      if (is_float) {
        parser_error(p, tok, "integer suffix used on float literal", NULL);
      }
      errno = 0;
      unsigned long long uval = strtoull(num_buf, NULL, 0);
      if (errno == ERANGE) {
        parser_error(p, tok, "integer literal out of range", NULL);
        uval = 0;
      }
      bool forced_u64 = false;
      if (!hint_explicit && uval > (unsigned long long)INT64_MAX) {
        // Treat large literals as u64 by default instead of erroring.
        hint = NY_LIT_HINT_U64;
        forced_u64 = true;
      }
      lit->as.literal.kind = NY_LIT_INT;
      lit->as.literal.as.i = (int64_t)uval;
      if (!hint_explicit && !forced_u64)
        hint = infer_int_hint((int64_t)uval);
      if (hint_explicit && !check_int_range(p, tok, (uint64_t)uval, hint)) {
        // Keep value but hint is still recorded for diagnostics.
      }
    }

    lit->as.literal.hint = hint;
    lit->as.literal.hint_explicit = hint_explicit;
    return lit;
  }
  case NY_T_TRUE:
  case NY_T_FALSE: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    lit->as.literal.kind = NY_LIT_BOOL;
    lit->as.literal.as.b = tok.kind == NY_T_TRUE;
    return lit;
  }
  case NY_T_NIL: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    lit->as.literal.kind = NY_LIT_INT;
    lit->as.literal.as.i = 0;
    return lit;
  }
  case NY_T_SIZEOF: {
    parser_advance(p);
    expr_t *e = expr_new(p->arena, NY_E_SIZEOF, tok);
    parser_expect(p, NY_T_LPAREN, "'('", NULL);
    char *type_name = NULL;
    if (parse_type_name(p, &type_name)) {
      e->as.szof.is_type = true;
      e->as.szof.type_name = type_name;
      e->as.szof.target = NULL;
      parser_expect(p, NY_T_RPAREN, "')'", NULL);
      return e;
    }
    e->as.szof.is_type = false;
    e->as.szof.type_name = NULL;
    e->as.szof.target = p_parse_expr(p, 0);
    parser_expect(p, NY_T_RPAREN, "')'", NULL);
    return e;
  }
  case NY_T_STRING: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    lit->as.literal.kind = NY_LIT_STR;
    size_t slen = 0;
    const char *sval = parser_decode_string(p, tok, &slen);
    lit->as.literal.as.s.data = sval;
    lit->as.literal.as.s.len = slen;
    return lit;
  }
  case NY_T_FSTRING:
    return parse_fstring(p, tok);
  case NY_T_MATCH: {
    stmt_t *s = p_parse_match(p);
    expr_t *e = expr_new(p->arena, NY_E_MATCH, tok);
    e->as.match = s->as.match;
    return e;
  }
  case NY_T_DOT: {
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "member access expects identifier", NULL);
      return NULL;
    }
    expr_t *e = expr_new(p->arena, NY_E_INFERRED_MEMBER, tok);
    e->as.inferred_member.name =
        arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    return e;
  }
  case NY_T_LPAREN: {
    parser_advance(p);
    if (parser_match(p, NY_T_RPAREN)) {
      return expr_new(p->arena, NY_E_TUPLE, tok);
    }
    expr_t *inner = p_parse_expr(p, 0);
    if (p->cur.kind == NY_T_COMMA) {
      expr_t *tup = expr_new(p->arena, NY_E_TUPLE, tok);
      vec_push_arena(p->arena, &tup->as.list_like, inner);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind == NY_T_RPAREN)
          break;
        vec_push_arena(p->arena, &tup->as.list_like, p_parse_expr(p, 0));
      }
      parser_expect(p, NY_T_RPAREN, NULL, NULL);
      return tup;
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    return inner;
  }
  case NY_T_LBRACK: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LIST, tok);
    if (p->cur.kind != NY_T_RBRACK) {
      while (true) {
        expr_t *item = p_parse_expr(p, 0);
        vec_push_arena(p->arena, &lit->as.list_like, item);
        if (!parser_match(p, NY_T_COMMA))
          break;
        if (p->cur.kind == NY_T_RBRACK)
          break;
      }
    }
    parser_expect(p, NY_T_RBRACK, NULL, NULL);
    return lit;
  }
  case NY_T_LBRACE: {
    parser_advance(p);
    if (p->cur.kind == NY_T_RBRACE) {
      parser_expect(p, NY_T_RBRACE, NULL, NULL);
      expr_t *set = expr_new(p->arena, NY_E_SET, tok);
      return set;
    }
    expr_t *first = p_parse_expr(p, 0);
    if (parser_match(p, NY_T_COLON)) {
      expr_t *dict = expr_new(p->arena, NY_E_DICT, tok);
      dict_pair_t pair = {first, p_parse_expr(p, 0)};
      vec_push_arena(p->arena, &dict->as.dict.pairs, pair);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind == NY_T_RBRACE)
          break;
        expr_t *k = p_parse_expr(p, 0);
        parser_expect(p, NY_T_COLON, NULL, NULL);
        expr_t *v = p_parse_expr(p, 0);
        pair.key = k;
        pair.value = v;
        vec_push_arena(p->arena, &dict->as.dict.pairs, pair);
      }
      parser_expect(p, NY_T_RBRACE, NULL, NULL);
      return dict;
    } else {
      expr_t *set = expr_new(p->arena, NY_E_SET, tok);
      vec_push_arena(p->arena, &set->as.list_like, first);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind == NY_T_RBRACE)
          break;
        vec_push_arena(p->arena, &set->as.list_like, p_parse_expr(p, 0));
      }
      parser_expect(p, NY_T_RBRACE, NULL, NULL);
      return set;
    }
  }
  case NY_T_ASM: {
    parser_advance(p);
    parser_expect(p, NY_T_LPAREN, NULL, NULL);
    token_t code_tok = p->cur;
    parser_expect(p, NY_T_STRING, "assembly code string", NULL);
    size_t code_len;
    const char *code = parser_decode_string(p, code_tok, &code_len);
    const char *constraints = "";
    if (parser_match(p, NY_T_COMMA)) {
      token_t constr_tok = p->cur;
      parser_expect(p, NY_T_STRING, "constraints string", NULL);
      size_t constr_len;
      constraints = parser_decode_string(p, constr_tok, &constr_len);
    }
    expr_t *e = expr_new(p->arena, NY_E_ASM, tok);
    e->as.as_asm.code = code;
    e->as.as_asm.constraints = constraints;
    while (parser_match(p, NY_T_COMMA)) {
      vec_push_arena(p->arena, &e->as.as_asm.args, p_parse_expr(p, 0));
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    return e;
  }
  case NY_T_EMBED: {
    parser_advance(p);
    parser_expect(p, NY_T_LPAREN, NULL, NULL);
    token_t path_tok = p->cur;
    parser_expect(p, NY_T_STRING, "file path string", NULL);
    size_t path_len;
    const char *path = parser_decode_string(p, path_tok, &path_len);
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    expr_t *e = expr_new(p->arena, NY_E_EMBED, tok);
    e->as.embed.path = path;
    return e;
  }
  case NY_T_LAMBDA:
  case NY_T_FN: {
    bool is_fn = tok.kind == NY_T_FN;
    parser_advance(p);
    parser_expect(p, NY_T_LPAREN, NULL, NULL);
    expr_t *lam = expr_new(p->arena, is_fn ? NY_E_FN : NY_E_LAMBDA, tok);
    while (p->cur.kind != NY_T_RPAREN) {
      if (parser_match(p, NY_T_ELLIPSIS)) {
        lam->as.lambda.is_variadic = true;
      }
      param_t pr = {0};
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "param must be identifier", NULL);
        return lam;
      }
      pr.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      if (parser_match(p, NY_T_COLON)) {
        if (p->cur.kind != NY_T_IDENT)
          parser_error(p, p->cur, "expected type name", NULL);
        else {
          pr.type = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
          parser_advance(p);
        }
      }
      if (parser_match(p, NY_T_ASSIGN))
        pr.def = p_parse_expr(p, 0);
      vec_push_arena(p->arena, &lam->as.lambda.params, pr);
      if (lam->as.lambda.is_variadic) {
        if (p->cur.kind == NY_T_COMMA) {
          parser_error(p, p->cur, "variadic parameter must be the last one",
                       NULL);
        }
        break;
      }
      if (!parser_match(p, NY_T_COMMA))
        break;
      if (p->cur.kind == NY_T_RPAREN)
        break;
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    if (parser_match(p, NY_T_COLON)) {
      if (p->cur.kind != NY_T_IDENT)
        parser_error(p, p->cur, "expected return type", NULL);
      else {
        lam->as.lambda.return_type =
            arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
      }
    }
    lam->as.lambda.body = p_parse_block(p);
    return lam;
  }
  default:
    if (tok.kind == NY_T_ASSIGN) {
      parser_error(p, tok, "unexpected '='", "did you mean '=='?");
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "unexpected token '%s'",
               parser_token_name(tok.kind));
      parser_error(p, tok, msg, NULL);
    }
    return NULL;
  }
}

static expr_t *parse_postfix(parser_t *p) {
  expr_t *expr = parse_primary(p);
  for (;;) {
    if (p->cur.kind == NY_T_QUESTION) {
      token_t tok = p->cur;
      const char *s = p->lex.src + p->lex.pos;
      int depth = 0;
      bool found_colon = false;
      while (*s && *s != '\n' && *s != ';') {
        if (*s == '(' || *s == '[' || *s == '{')
          depth++;
        else if (*s == ')' || *s == ']' || *s == '}')
          depth--;
        else if (*s == ':' && depth == 0) {
          found_colon = true;
          break;
        }
        s++;
      }

      if (found_colon) {
        parser_advance(p);
        expr_t *true_expr = p_parse_expr(p, 0);
        parser_expect(p, NY_T_COLON, ":", "ternary operator requires ':'");
        expr_t *false_expr = p_parse_expr(p, 0);
        expr_t *ternary = expr_new(p->arena, NY_E_TERNARY, tok);
        ternary->as.ternary.cond = expr;
        ternary->as.ternary.true_expr = true_expr;
        ternary->as.ternary.false_expr = false_expr;
        expr = ternary;
        continue;
      } else {
        parser_advance(p);
        expr_t *tr = expr_new(p->arena, NY_E_TRY, tok);
        tr->as.unary.right = expr;
        expr = tr;
        continue;
      }
    } else if (p->cur.kind == NY_T_LPAREN) {
      parser_advance(p);
      expr_t *call = expr_new(p->arena, NY_E_CALL, p->cur);
      call->as.call.callee = expr;
      while (p->cur.kind != NY_T_RPAREN) {
        call_arg_t arg = {0};
        if (p->cur.kind == NY_T_IDENT && parser_peek(p).kind == NY_T_ASSIGN) {
          arg.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
          parser_advance(p); // name
          parser_advance(p); // '='
          arg.val = p_parse_expr(p, 0);
        } else {
          arg.val = p_parse_expr(p, 0);
        }
        vec_push_arena(p->arena, &call->as.call.args, arg);
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
      parser_expect(p, NY_T_RPAREN, NULL, NULL);
      expr = call;
    } else if (p->cur.kind == NY_T_DOT) {
      parser_advance(p);
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "member access expects identifier", NULL);
        return expr;
      }
      token_t id_tok = p->cur;
      char *name = arena_strndup(p->arena, id_tok.lexeme, id_tok.len);
      parser_advance(p);
      if (p->cur.kind == NY_T_LPAREN) {
        parser_advance(p);
        expr_t *mc = expr_new(p->arena, NY_E_MEMCALL, id_tok);
        mc->as.memcall.target = expr;
        mc->as.memcall.name = name;
        while (p->cur.kind != NY_T_RPAREN) {
          call_arg_t arg = {0};
          if (p->cur.kind == NY_T_IDENT && parser_peek(p).kind == NY_T_ASSIGN) {
            arg.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
            parser_advance(p); // name
            parser_advance(p); // '='
            arg.val = p_parse_expr(p, 0);
          } else {
            arg.val = p_parse_expr(p, 0);
          }
          vec_push_arena(p->arena, &mc->as.memcall.args, arg);
          if (!parser_match(p, NY_T_COMMA))
            break;
        }
        parser_expect(p, NY_T_RPAREN, NULL, NULL);
        expr = mc;
      } else {
        expr_t *m = expr_new(p->arena, NY_E_MEMBER, id_tok);
        m->as.member.target = expr;
        m->as.member.name = name;
        expr = m;
      }
    } else if (p->cur.kind == NY_T_LBRACK) {
      parser_advance(p);
      expr_t *idx = expr_new(p->arena, NY_E_INDEX, p->cur);
      idx->as.index.target = expr;
      if (p->cur.kind != NY_T_RBRACK) {
        if (p->cur.kind == NY_T_COLON) {
          idx->as.index.start = NULL;
        } else {
          idx->as.index.start = p_parse_expr(p, 0);
        }
        if (parser_match(p, NY_T_COLON)) {
          if (p->cur.kind == NY_T_COLON) {
            expr_t *sent = expr_new(p->arena, NY_E_LITERAL, p->cur);
            sent->as.literal.kind = NY_LIT_INT;
            sent->as.literal.as.i = 0x3fffffff;
            idx->as.index.stop = sent;
          } else if (p->cur.kind != NY_T_RBRACK) {
            idx->as.index.stop = p_parse_expr(p, 0);
          } else {
            expr_t *sent = expr_new(p->arena, NY_E_LITERAL, p->cur);
            sent->as.literal.kind = NY_LIT_INT;
            sent->as.literal.as.i = 0x3fffffff;
            idx->as.index.stop = sent;
          }
          if (parser_match(p, NY_T_COLON)) {
            if (p->cur.kind != NY_T_RBRACK)
              idx->as.index.step = p_parse_expr(p, 0);
          }
        }
      }
      parser_expect(p, NY_T_RBRACK, NULL, NULL);
      expr = idx;
    } else {
      break;
    }
  }
  return expr;
}

static expr_t *parse_unary(parser_t *p) {
  if (p->cur.kind == NY_T_MINUS || p->cur.kind == NY_T_NOT ||
      p->cur.kind == NY_T_BITNOT) {
    token_t tok = p->cur;
    parser_advance(p);
    expr_t *expr = expr_new(p->arena, NY_E_UNARY, tok);
    if (tok.kind == NY_T_MINUS)
      expr->as.unary.op = "-";
    else if (tok.kind == NY_T_NOT)
      expr->as.unary.op = "!";
    else
      expr->as.unary.op = "~";
    expr->as.unary.right = parse_unary(p);
    return expr;
  }
  return parse_postfix(p);
}

expr_t *p_parse_expr(parser_t *p, int prec) {
  expr_t *left = parse_unary(p);
  while (true) {
    int pcur = precedence(p->cur.kind);
    if (pcur < prec || pcur == 0)
      break;
    token_t op = p->cur;
    parser_advance(p);
    expr_t *right = p_parse_expr(p, pcur + 1);
    expr_t *bin;
    if (op.kind == NY_T_AND || op.kind == NY_T_OR) {
      bin = expr_new(p->arena, NY_E_LOGICAL, op);
      bin->as.logical.op = (op.kind == NY_T_AND) ? "&&" : "||";
      bin->as.logical.left = left;
      bin->as.logical.right = right;
    } else {
      bin = expr_new(p->arena, NY_E_BINARY, op);
      bin->as.binary.op = arena_strndup(p->arena, op.lexeme, op.len);
      bin->as.binary.left = left;
      bin->as.binary.right = right;
    }
    left = bin;
  }
  return left;
}
