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

static bool hint_allows_integer_overflow_bigint(lit_type_hint_t hint,
                                                bool hint_explicit) {
  if (!hint_explicit)
    return true;
  return hint == NY_LIT_HINT_I128 || hint == NY_LIT_HINT_U128;
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
  case NY_LIT_HINT_I128:
    return true;
  case NY_LIT_HINT_U8:
    return val <= UINT8_MAX;
  case NY_LIT_HINT_U16:
    return val <= UINT16_MAX;
  case NY_LIT_HINT_U32:
    return val <= UINT32_MAX;
  case NY_LIT_HINT_U64:
  case NY_LIT_HINT_U128:
    return true;
  default:
    break;
  }
  parser_error(p, tok, "integer literal out of range for suffix", NULL);
  return false;
}

static bool expr_is_type_like_ident(expr_t *e) {
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  unsigned char ch = (unsigned char)e->as.ident.name[0];
  return isupper(ch) || ch == '_';
}

static bool expr_lbrace_starts_named_fields(parser_t *p) {
  if (!p || p->cur.kind != NY_T_LBRACE)
    return false;
  parser_t scan = *p;
  parser_advance(&scan);
  if (scan.cur.kind != NY_T_IDENT && scan.cur.kind != NY_T_STRING)
    return false;
  parser_advance(&scan);
  return scan.cur.kind == NY_T_COLON;
}

static void expr_skip_balanced_brace(parser_t *p) {
  if (!p || p->cur.kind != NY_T_LBRACE)
    return;
  int depth = 0;
  do {
    if (p->cur.kind == NY_T_LBRACE) {
      depth++;
    } else if (p->cur.kind == NY_T_RBRACE) {
      depth--;
      parser_advance(p);
      if (depth <= 0)
        return;
      continue;
    }
    parser_advance(p);
  } while (p->cur.kind != NY_T_EOF);
}

static bool parse_numeric_suffix(const char *s, size_t len, size_t *num_len,
                                 lit_type_hint_t *hint, bool *hint_explicit) {
  *num_len = len;
  *hint = NY_LIT_HINT_NONE;
  *hint_explicit = false;
  if (len < 2)
    return true;
  size_t i = len;
  while (i > 0 && isdigit((unsigned char)s[i - 1]))
    i--;
  if (i == len)
    return true;
  if (i == 0)
    return true;
  char c = s[i - 1];
  bool is_hex = (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
  if (c != 'i' && c != 'I' && c != 'u' && c != 'U' && c != 'f' && c != 'F')
    return true;
  if (is_hex && (c == 'f' || c == 'F')) {
    return true;
  }
  size_t suffix_start = i - 1;
  size_t value_end = suffix_start;
  if (suffix_start > 0 && s[suffix_start - 1] == '_')
    value_end = suffix_start - 1;
  const char *suffix = s + suffix_start;
  size_t suffix_len = len - suffix_start;
  if (suffix_eq(suffix, suffix_len, "i8"))
    *hint = NY_LIT_HINT_I8;
  else if (suffix_eq(suffix, suffix_len, "i16"))
    *hint = NY_LIT_HINT_I16;
  else if (suffix_eq(suffix, suffix_len, "i32"))
    *hint = NY_LIT_HINT_I32;
  else if (suffix_eq(suffix, suffix_len, "i64"))
    *hint = NY_LIT_HINT_I64;
  else if (suffix_eq(suffix, suffix_len, "i128"))
    *hint = NY_LIT_HINT_I128;
  else if (suffix_eq(suffix, suffix_len, "u8"))
    *hint = NY_LIT_HINT_U8;
  else if (suffix_eq(suffix, suffix_len, "u16"))
    *hint = NY_LIT_HINT_U16;
  else if (suffix_eq(suffix, suffix_len, "u32"))
    *hint = NY_LIT_HINT_U32;
  else if (suffix_eq(suffix, suffix_len, "u64"))
    *hint = NY_LIT_HINT_U64;
  else if (suffix_eq(suffix, suffix_len, "u128"))
    *hint = NY_LIT_HINT_U128;
  else if (suffix_eq(suffix, suffix_len, "f32"))
    *hint = NY_LIT_HINT_F32;
  else if (suffix_eq(suffix, suffix_len, "f64"))
    *hint = NY_LIT_HINT_F64;
  else if (suffix_eq(suffix, suffix_len, "f128"))
    *hint = NY_LIT_HINT_F128;
  else
    return false;
  *hint_explicit = true;
  *num_len = value_end;
  return true;
}

static char *numeric_literal_value_buf(parser_t *p, const char *s, size_t len,
                                       size_t *out_len) {
  char *out = arena_alloc(p->arena, len + 1);
  size_t at = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] != '_')
      out[at++] = s[i];
  }
  out[at] = '\0';
  if (out_len)
    *out_len = at;
  return out;
}

static bool numeric_digit_value(char c, unsigned *out) {
  if (c >= '0' && c <= '9') {
    *out = (unsigned)(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    *out = 10u + (unsigned)(c - 'a');
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    *out = 10u + (unsigned)(c - 'A');
    return true;
  }
  return false;
}

static bool parse_integer_literal_u64(const char *s, size_t len,
                                      uint64_t *out) {
  unsigned base = 10;
  size_t i = 0;
  if (len > 2 && s[0] == '0') {
    if (s[1] == 'x' || s[1] == 'X') {
      base = 16;
      i = 2;
    } else if (s[1] == 'b' || s[1] == 'B') {
      base = 2;
      i = 2;
    } else if (s[1] == 'o' || s[1] == 'O') {
      base = 8;
      i = 2;
    }
  }
  if (i >= len)
    return false;
  uint64_t acc = 0;
  for (; i < len; i++) {
    unsigned digit = 0;
    if (!numeric_digit_value(s[i], &digit) || digit >= base)
      return false;
    if (acc > (UINT64_MAX - (uint64_t)digit) / (uint64_t)base) {
      errno = ERANGE;
      return false;
    }
    acc = acc * (uint64_t)base + (uint64_t)digit;
  }
  *out = acc;
  return true;
}

static bool parse_type_name(parser_t *p, char **out_name) {
  parser_t save = *p;
  size_t nullable_depth = 0;
  while (parser_match(p, NY_T_QUESTION))
    nullable_depth++;
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
  if (p->current_impl_owner && strcmp(buf, "self") == 0) {
    const char *owner = p->current_impl_owner;
    size_t owner_len = strlen(owner);
    char *owner_buf = malloc(owner_len + 1);
    if (!owner_buf) {
      free(buf);
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(owner_buf, owner, owner_len + 1);
    free(buf);
    buf = owner_buf;
    len = owner_len;
  }
  size_t total = nullable_depth + ptr_depth + len;
  char *out = arena_alloc(p->arena, total + 1);
  size_t at = 0;
  for (size_t i = 0; i < nullable_depth; i++)
    out[at++] = '?';
  for (size_t i = 0; i < ptr_depth; i++)
    out[at++] = '*';
  memcpy(out + at, buf, len);
  out[total] = '\0';
  free(buf);
  *out_name = out;
  return true;
}

static const char *expr_parse_type_ref(parser_t *p, const char *err_msg) {
  size_t nullable_depth = 0;
  while (parser_match(p, NY_T_QUESTION))
    nullable_depth++;
  size_t ptr_depth = 0;
  while (parser_match(p, NY_T_STAR))
    ptr_depth++;
  if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER) {
    parser_error(p, p->cur, err_msg ? err_msg : "expected type name", NULL);
    return NULL;
  }
  size_t cap = (size_t)p->cur.len + 32;
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
    if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER) {
      parser_error(p, p->cur, "expected identifier after '.' in type", NULL);
      free(buf);
      return NULL;
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
  if (p->current_impl_owner && strcmp(buf, "self") == 0) {
    const char *owner = p->current_impl_owner;
    size_t owner_len = strlen(owner);
    char *owner_buf = malloc(owner_len + 1);
    if (!owner_buf) {
      free(buf);
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(owner_buf, owner, owner_len + 1);
    free(buf);
    buf = owner_buf;
    len = owner_len;
  }
  if (parser_match(p, NY_T_LT)) {
    size_t gcap = len + 32;
    char *generic = malloc(gcap);
    if (!generic) {
      free(buf);
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(generic, buf, len);
    generic[len++] = '<';
    free(buf);
    bool first = true;
    while (p->cur.kind != NY_T_GT && p->cur.kind != NY_T_RSHIFT &&
           p->cur.kind != NY_T_EOF) {
      const char *arg =
          expr_parse_type_ref(p, "expected generic type argument");
      if (!arg)
        break;
      size_t arg_len = strlen(arg);
      size_t need = len + (first ? 0 : 2) + arg_len + 2;
      if (need > gcap) {
        while (gcap < need)
          gcap *= 2;
        char *nb = realloc(generic, gcap);
        if (!nb) {
          free(generic);
          fprintf(stderr, "oom\n");
          exit(1);
        }
        generic = nb;
      }
      if (!first) {
        generic[len++] = ',';
        generic[len++] = ' ';
      }
      memcpy(generic + len, arg, arg_len);
      len += arg_len;
      first = false;
      if (!parser_match(p, NY_T_COMMA))
        break;
    }
    if (parser_match(p, NY_T_GT)) {

    } else if (p->cur.kind == NY_T_RSHIFT) {
      token_t tok = p->cur;
      p->cur.kind = NY_T_GT;
      p->cur.lexeme = tok.lexeme + 1;
      p->cur.len = 1;
      p->cur.col = tok.col + 1;
    } else {
      parser_error(p, p->cur, "'>' after generic type arguments", NULL);
    }
    if (len + 2 > gcap) {
      char *nb = realloc(generic, len + 2);
      if (!nb) {
        free(generic);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      generic = nb;
    }
    generic[len++] = '>';
    generic[len] = '\0';
    buf = generic;
  }
  size_t total = nullable_depth + ptr_depth + len;
  char *out = arena_alloc(p->arena, total + 1);
  size_t at = 0;
  for (size_t i = 0; i < nullable_depth; i++)
    out[at++] = '?';
  for (size_t i = 0; i < ptr_depth; i++)
    out[at++] = '*';
  memcpy(out + at, buf, len);
  out[total] = '\0';
  free(buf);
  return out;
}

static bool expr_parse_lambda_param_type_first(parser_t *p, param_t *pr) {
  return parser_parse_param_type_first(p, pr, expr_parse_type_ref);
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
  case NY_T_RANGE:
    return 4;
  case NY_T_PLUS:
  case NY_T_MINUS:
    return 5;
  case NY_T_STAR:
  case NY_T_SLASH:
  case NY_T_PERCENT:
    return 6;
  case NY_T_POW:
    return 8;
  case NY_T_BITOR:
  case NY_T_BITAND:
  case NY_T_BITXOR:
  case NY_T_LSHIFT:
  case NY_T_RSHIFT:
    return 7;
  case NY_T_PIPE:
    return 1;
  case NY_T_QUESTION_QUESTION:
    return 2;
  default:
    return 0;
  }
}

static const char *decode_fstring_part(parser_t *p, const char *s, size_t len,
                                       size_t *out_len) {
  return parser_unescape_string(p->arena, s, len, out_len);
}

static char *decode_fstring_expr(parser_t *p, const char *s, size_t len,
                                 char quote) {
  char *out = arena_alloc(p->arena, len + 1);
  if (!out)
    return NULL;
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\\' && i + 1 < len &&
        (s[i + 1] == quote || s[i + 1] == '{' || s[i + 1] == '}')) {
      out[j++] = s[++i];
      continue;
    }
    out[j++] = s[i];
  }
  out[j] = '\0';
  return out;
}

static bool fstring_debug_equal_pos(const char *s, size_t len, size_t *out_eq) {
  size_t end = len;
  while (end > 0 && isspace((unsigned char)s[end - 1]))
    end--;
  if (end == 0 || s[end - 1] != '=')
    return false;
  size_t eq = end - 1;
  if (eq == 0)
    return false;
  char prev = s[eq - 1];
  if (prev == '=' || prev == '!' || prev == '<' || prev == '>')
    return false;

  int paren = 0, bracket = 0, brace = 0;
  char quote = 0;
  for (size_t i = 0; i <= eq; i++) {
    char c = s[i];
    if (quote) {
      if (c == '\\' && i + 1 <= eq) {
        i++;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '(')
      paren++;
    else if (c == ')' && paren > 0)
      paren--;
    else if (c == '[')
      bracket++;
    else if (c == ']' && bracket > 0)
      bracket--;
    else if (c == '{')
      brace++;
    else if (c == '}' && brace > 0)
      brace--;
    else if (i == eq) {
      bool ok = paren == 0 && bracket == 0 && brace == 0;
      if (ok && out_eq)
        *out_eq = eq;
      return ok;
    }
  }
  return false;
}

static expr_t *parse_fstring(parser_t *p, token_t tok) {
  parser_advance(p);
  expr_t *e = expr_new(p->arena, NY_E_FSTRING, tok);
  const char *s = tok.lexeme;
  size_t len = tok.len;
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
        size_t expr_len = i - start;
        size_t debug_eq = 0;
        if (fstring_debug_equal_pos(s + start, expr_len, &debug_eq)) {
          char *debug_label =
              decode_fstring_expr(p, s + start, expr_len, quote);
          fstring_part_t label = {.kind = NY_FSP_STR};
          label.as.s.data = debug_label;
          label.as.s.len = debug_label ? strlen(debug_label) : 0;
          vec_push_arena(p->arena, &e->as.fstring.parts, label);
          expr_len = debug_eq;
        }
        char *expr_str = decode_fstring_expr(p, s + start, expr_len, quote);
        parser_t sub;
        if (p->quiet)
          parser_init_with_arena_quiet(&sub, expr_str, p->lex.filename,
                                       p->arena);
        else
          parser_init_with_arena(&sub, expr_str, p->lex.filename, p->arena);
        expr_t *sub_e = p_parse_expr(&sub, 0);
        fstring_part_t part = {.kind = NY_FSP_EXPR, .as.e = sub_e};
        vec_push_arena(p->arena, &e->as.fstring.parts, part);
        i++;
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

static const char *expr_parse_dotted_ident(parser_t *p, const char *what) {
  if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER) {
    parser_error(p, p->cur, what ? what : "expected identifier", NULL);
    return NULL;
  }
  size_t cap = p->cur.len + 32;
  size_t len = 0;
  char *buf = arena_alloc(p->arena, cap);
  memcpy(buf, p->cur.lexeme, p->cur.len);
  len += p->cur.len;
  parser_advance(p);
  while (parser_match(p, NY_T_DOT)) {
    if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER) {
      parser_error(p, p->cur, "expected identifier after '.'", NULL);
      return NULL;
    }
    size_t need = 1 + p->cur.len;
    if (len + need + 1 > cap) {
      size_t ncap = (len + need + 1) * 2;
      char *nbuf = arena_alloc(p->arena, ncap);
      memcpy(nbuf, buf, len);
      buf = nbuf;
      cap = ncap;
    }
    buf[len++] = '.';
    memcpy(buf + len, p->cur.lexeme, p->cur.len);
    len += p->cur.len;
    parser_advance(p);
  }
  buf[len] = '\0';
  return parser_intern(p, buf, len);
}

static const char *expr_comptime_table_matcher(parser_t *p,
                                               const char *table_name) {
  static const char prefix[] = "_ct_table_";
  const char *tail = strrchr(table_name, '.');
  size_t owner_len = tail ? (size_t)(tail - table_name + 1) : 0;
  const char *leaf = tail ? tail + 1 : table_name;
  size_t leaf_len = strlen(leaf);
  size_t prefix_len = sizeof(prefix) - 1;
  size_t total = owner_len + prefix_len + leaf_len;
  char *buf = arena_alloc(p->arena, total + 1);
  if (owner_len)
    memcpy(buf, table_name, owner_len);
  memcpy(buf + owner_len, prefix, prefix_len);
  memcpy(buf + owner_len + prefix_len, leaf, leaf_len);
  buf[total] = '\0';
  return parser_intern(p, buf, total);
}

static expr_t *parse_ct_table_match_expr(parser_t *p, token_t tok) {
  const char *table_name =
      expr_parse_dotted_ident(p, "expected table name after 'comptime match'");
  if (!table_name)
    return NULL;
  parser_expect(p, NY_T_LPAREN, "'(' after comptime match table name",
                "write comptime match Table(value, default)");

  expr_t *key = p_parse_expr(p, 0);
  parser_expect(p, NY_T_COMMA, "',' after comptime match key", NULL);
  expr_t *fallback = p_parse_expr(p, 0);
  parser_expect(p, NY_T_RPAREN, "')' after comptime match default", NULL);

  const char *matcher = expr_comptime_table_matcher(p, table_name);
  token_t callee_tok = tok;
  callee_tok.lexeme = matcher;
  callee_tok.len = strlen(matcher);
  expr_t *callee = expr_new(p->arena, NY_E_IDENT, callee_tok);
  callee->as.ident.name = matcher;
  expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
  call->as.call.callee = callee;
  call_arg_t key_arg = {.name = NULL, .val = key};
  call_arg_t fallback_arg = {.name = NULL, .val = fallback};
  vec_push_arena(p->arena, &call->as.call.args, key_arg);
  vec_push_arena(p->arena, &call->as.call.args, fallback_arg);
  return call;
}

static expr_t *expr_nil_literal(parser_t *p, token_t tok) {
  token_t nil_tok = tok;
  nil_tok.kind = NY_T_NIL;
  nil_tok.lexeme = "nil";
  nil_tok.len = 3;
  expr_t *nil_lit = expr_new(p->arena, NY_E_LITERAL, nil_tok);
  nil_lit->as.literal.kind = NY_LIT_INT;
  nil_lit->as.literal.as.i = 0;
  return nil_lit;
}

static expr_t *parse_if_stmt_as_expr(parser_t *p, stmt_t *s, token_t tok);

static void parse_call_arg_list(parser_t *p, ny_call_arg_list *args,
                                const char *close_msg) {
  while (p->cur.kind != NY_T_RPAREN) {
    call_arg_t arg = {0};
    if (p->cur.kind == NY_T_IDENT) {
      token_t next = parser_peek(p);
      if (next.kind == NY_T_ASSIGN || next.kind == NY_T_COLON) {
        arg.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
        parser_advance(p);
        arg.val = p_parse_expr(p, 0);
      } else {
        arg.val = p_parse_expr(p, 0);
      }
    } else {
      arg.val = p_parse_expr(p, 0);
    }
    vec_push_arena(p->arena, args, arg);
    if (!parser_match(p, NY_T_COMMA))
      break;
  }
  parser_expect(p, NY_T_RPAREN, close_msg, NULL);
}

static expr_t *stmt_value_expr(parser_t *p, stmt_t *s, token_t tok,
                               const char *branch_name) {
  if (!s)
    return expr_nil_literal(p, tok);
  switch (s->kind) {
  case NY_S_BLOCK:
    if (s->as.block.body.len == 0)
      return expr_nil_literal(p, tok);
    if (s->as.block.body.len == 1)
      return stmt_value_expr(p, s->as.block.body.data[0], tok, branch_name);
    parser_error(
        p, tok,
        "if expression branch must be a single value-producing statement",
        "use a value expression in the branch or keep the multi-statement form "
        "as an if statement");
    return expr_nil_literal(p, tok);
  case NY_S_EXPR:
    return s->as.expr.expr ? s->as.expr.expr : expr_nil_literal(p, tok);
  case NY_S_RETURN:
    return s->as.ret.value ? s->as.ret.value : expr_nil_literal(p, tok);
  case NY_S_IF:
    return parse_if_stmt_as_expr(p, s, s->tok);
  default: {
    char msg[128];
    snprintf(msg, sizeof(msg),
             "if expression %s branch does not produce a value",
             branch_name ? branch_name : "");
    parser_error(p, s->tok, msg,
                 "use an expression branch, for example "
                 "if(cond){ value } else { fallback }");
    return expr_nil_literal(p, s->tok);
  }
  }
}

static expr_t *parse_if_stmt_as_expr(parser_t *p, stmt_t *s, token_t tok) {
  if (!s || s->kind != NY_S_IF) {
    parser_error(p, tok, "expected if expression", NULL);
    return expr_nil_literal(p, tok);
  }
  if (s->as.iff.init) {
    parser_error(p, tok, "if expression does not support init bindings yet",
                 "bind the value before the if expression");
  }
  if (!s->as.iff.alt) {
    parser_error(p, tok, "if expression requires an else branch",
                 "write if(cond){ value } else { fallback }");
  }
  expr_t *then_expr =
      stmt_value_expr(p, s->as.iff.conseq, tok, "then");
  expr_t *else_expr =
      s->as.iff.alt ? stmt_value_expr(p, s->as.iff.alt, tok, "else")
                    : expr_nil_literal(p, tok);
  expr_t *tern = expr_new(p->arena, NY_E_TERNARY, tok);
  tern->as.ternary.cond = s->as.iff.test;
  tern->as.ternary.true_expr = then_expr;
  tern->as.ternary.false_expr = else_expr;
  return tern;
}

static expr_t *parse_primary(parser_t *p) {
  token_t tok = p->cur;
  switch (tok.kind) {
  case NY_T_COMPTIME: {
    parser_advance(p);
    if (p->cur.kind == NY_T_MATCH) {
      parser_advance(p);
      return parse_ct_table_match_expr(p, tok);
    }
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
    if (tok.len == 4 && strncmp(tok.lexeme, "null", 4) == 0) {
      parser_error(p, tok, "unrecognised identifier 'null'",
                   "did you mean '0' or 'nil'?");
    } else if (tok.len == 4 && strncmp(tok.lexeme, "None", 4) == 0) {
      parser_error(p, tok, "unrecognised identifier 'None'",
                   "did you mean '0' or 'nil'?");
    }
    expr_t *id = expr_new(p->arena, NY_E_IDENT, tok);
    id->as.ident.name = parser_intern_hash(p, tok.lexeme, tok.len, tok.hash);
    id->as.ident.sym_id = tok.sym_id;
    id->as.ident.hash = tok.hash;
    if (tok.len == 6 && strncmp(tok.lexeme, "expand", 6) == 0) {
      expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
      call_arg_t arg = {0};
      arg.val = p_parse_expr(p, 0);
      call->as.call.callee = id;
      vec_push_arena(p->arena, &call->as.call.args, arg);
      return call;
    }
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
    size_t clean_num_len = 0;
    char *num_buf =
        numeric_literal_value_buf(p, tok.lexeme, num_len, &clean_num_len);
    bool is_prefixed_int =
        (clean_num_len > 2 && num_buf[0] == '0' &&
         (num_buf[1] == 'x' || num_buf[1] == 'X' || num_buf[1] == 'b' ||
          num_buf[1] == 'B' || num_buf[1] == 'o' || num_buf[1] == 'O'));
    bool is_float = !is_prefixed_int && (memchr(num_buf, '.', clean_num_len) ||
                                         memchr(num_buf, 'e', clean_num_len) ||
                                         memchr(num_buf, 'E', clean_num_len));
    if (hint_is_float(hint) || (!hint_explicit && is_float)) {
      if (is_prefixed_int) {
        parser_error(p, tok, "prefixed float literals are not supported yet",
                     NULL);
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
      uint64_t uval = 0;
      bool parsed = parse_integer_literal_u64(num_buf, clean_num_len, &uval);
      bool overflow_bigint = false;
      if (errno == ERANGE) {
        if (hint_allows_integer_overflow_bigint(hint, hint_explicit)) {
          overflow_bigint = true;
          uval = 0;
        } else {
          parser_error(p, tok, "integer literal out of range for suffix", NULL);
          uval = 0;
        }
      } else if (!parsed) {
        parser_error(p, tok, "malformed integer literal", NULL);
      }
      bool forced_u64 = false;
      if (!hint_explicit && !overflow_bigint && uval > (uint64_t)INT64_MAX) {
        hint = NY_LIT_HINT_U64;
        forced_u64 = true;
      }
      lit->as.literal.kind = NY_LIT_INT;
      lit->as.literal.as.i = (overflow_bigint || uval > (uint64_t)INT64_MAX)
                                 ? INT64_MAX
                                 : (int64_t)uval;
      if (!hint_explicit && !forced_u64)
        hint = NY_LIT_HINT_NONE;
      if (hint_explicit && !overflow_bigint &&
          !check_int_range(p, tok, (uint64_t)uval, hint)) {
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
  case NY_T_IF: {
    stmt_t *s = ny_parse_if_stmt(p);
    return parse_if_stmt_as_expr(p, s, tok);
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
      expr_t *dict = expr_new(p->arena, NY_E_DICT, tok);
      return dict;
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
      if (!expr_parse_lambda_param_type_first(p, &pr)) {
        return lam;
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
      parser_error(p, p->prev, "old function return separator",
                   "write 'fn(params) RetType { ... }', without ':'");
      if (p->cur.kind != NY_T_LBRACE && p->cur.kind != NY_T_ASSIGN &&
          p->cur.kind != NY_T_EOF)
        (void)expr_parse_type_ref(p, "expected return type");
    } else if (p->cur.kind == NY_T_ARROW) {
      parser_error(p, p->cur, "function return types do not use '->'",
                   "write 'fn(params) RetType { ... }'");
      parser_advance(p);
      (void)expr_parse_type_ref(p, "expected return type after '->'");
    } else
      lam->as.lambda.return_type =
          parser_parse_return_type_suffix(p, expr_parse_type_ref, "expected return type");
    stmt_t *body = NULL;
    if (p->cur.kind == NY_T_LBRACE) {
      body = p_parse_block(p);
    } else {
      token_t body_tok = p->cur;
      expr_t *e = p_parse_expr(p, 0);
      stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, body_tok);
      vec_reserve_arena(p->arena, &blk->as.block.body, 2);
      if (e) {
        stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, body_tok);
        ret->as.ret.value = e;
        vec_push_arena(p->arena, &blk->as.block.body, ret);
      }
      body = blk;
    }
    lam->as.lambda.body = body;
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
        break;
      } else {
        parser_advance(p);
        expr_t *tr = expr_new(p->arena, NY_E_TRY, tok);
        tr->as.unary.right = expr;
        expr = tr;
        continue;
      }
    } else if (p->cur.kind == NY_T_LPAREN) {
      if (p->skipped_newline)
        break;
      parser_advance(p);
      expr_t *call = expr_new(p->arena, NY_E_CALL, p->cur);
      call->as.call.callee = expr;
      parse_call_arg_list(p, &call->as.call.args, NULL);
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
        parse_call_arg_list(p, &mc->as.memcall.args, NULL);
        expr = mc;
      } else {
        expr_t *m = expr_new(p->arena, NY_E_MEMBER, id_tok);
        m->as.member.target = expr;
        m->as.member.name = name;
        expr = m;
      }
    } else if (p->cur.kind == NY_T_QUESTION_DOT) {

      token_t qdot_tok = p->cur;
      parser_advance(p);
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "optional access expects identifier", NULL);
        return expr;
      }
      token_t id_tok = p->cur;
      char *name = arena_strndup(p->arena, id_tok.lexeme, id_tok.len);
      parser_advance(p);

      expr_t *target = expr;
      expr_t *access;

      if (p->cur.kind == NY_T_LPAREN) {

        parser_advance(p);
        expr_t *mc = expr_new(p->arena, NY_E_MEMCALL, id_tok);
        mc->as.memcall.target = target;
        mc->as.memcall.name = name;
        parse_call_arg_list(p, &mc->as.memcall.args, NULL);
        access = mc;
      } else {

        expr_t *m = expr_new(p->arena, NY_E_MEMBER, id_tok);
        m->as.member.target = target;
        m->as.member.name = name;
        access = m;
      }

      expr_t *tern = expr_new(p->arena, NY_E_TERNARY, qdot_tok);
      tern->as.ternary.cond = target;
      tern->as.ternary.true_expr = access;

      token_t nil_tok = {.kind = NY_T_NIL, .lexeme = "nil", .len = 3};
      expr_t *nil_lit = expr_new(p->arena, NY_E_LITERAL, nil_tok);
      nil_lit->as.literal.kind = NY_LIT_INT;
      nil_lit->as.literal.as.i = 0;

      tern->as.ternary.false_expr = nil_lit;
      expr = tern;
    } else if (p->cur.kind == NY_T_LBRACK) {
      if (p->skipped_newline)
        break;
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
    } else if (p->cur.kind == NY_T_LBRACE && !p->skipped_newline &&
               expr_is_type_like_ident(expr) &&
               expr_lbrace_starts_named_fields(p)) {
      parser_error(p, p->cur, "named-field struct literals are not supported",
                   "use Type(value, value) positional constructor syntax");
      expr_skip_balanced_brace(p);
      break;
    } else {
      break;
    }
  }
  return expr;
}

static expr_t *parse_unary(parser_t *p) {
  if (p->cur.kind == NY_T_IDENT) {
    token_t tok = p->cur;
    token_t next = parser_peek(p);
    bool is_async = tok.len == 5 && memcmp(tok.lexeme, "async", 5) == 0;
    bool is_await = tok.len == 5 && memcmp(tok.lexeme, "await", 5) == 0;
    bool next_starts_expr = false;
    switch (next.kind) {
    case NY_T_IDENT:
    case NY_T_NUMBER:
    case NY_T_STRING:
    case NY_T_FSTRING:
    case NY_T_TRUE:
    case NY_T_FALSE:
    case NY_T_NIL:
    case NY_T_SIZEOF:
    case NY_T_COMPTIME:
    case NY_T_MATCH:
    case NY_T_DOT:
    case NY_T_LBRACK:
    case NY_T_LBRACE:
    case NY_T_ASM:
    case NY_T_EMBED:
    case NY_T_LAMBDA:
    case NY_T_FN:
    case NY_T_MINUS:
    case NY_T_NOT:
    case NY_T_BITAND:
    case NY_T_BITNOT:
      next_starts_expr = true;
      break;
    default:
      break;
    }
    if ((is_async || is_await) && next.kind != NY_T_LPAREN &&
        next_starts_expr) {
      parser_advance(p);
      expr_t *expr = expr_new(p->arena, NY_E_UNARY, tok);
      expr->as.unary.op = is_async ? "async" : "await";
      expr->as.unary.right = parse_unary(p);
      if (!expr->as.unary.right) {
        parser_error(p, tok,
                     is_async ? "expected expression after 'async'"
                              : "expected expression after 'await'",
                     NULL);
        return NULL;
      }
      return expr;
    }
  }
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
    expr->as.unary.right = p_parse_expr(p, precedence(NY_T_POW));
    if (!expr->as.unary.right) {
      parser_error(p, tok, "expected expression after unary operator", NULL);
      return NULL;
    }
    return expr;
  }
  if (p->cur.kind == NY_T_BITAND) {
    token_t tok = p->cur;
    parser_advance(p);
    expr_t *arg = parse_unary(p);
    if (!arg) {
      parser_error(p, tok, "expected expression after borrow operator '&'",
                   NULL);
      return NULL;
    }
    expr_t *callee = expr_new(p->arena, NY_E_IDENT, tok);
    callee->as.ident.name = parser_intern(p, "borrow", 6);
    callee->as.ident.sym_id = ny_intern_str("borrow", 6);
    callee->as.ident.hash = ny_hash64("borrow", 6);
    expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
    call->as.call.callee = callee;
    call_arg_t call_arg = {.name = NULL, .val = arg};
    vec_push_arena(p->arena, &call->as.call.args, call_arg);
    return call;
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

    if (parser_match(p, NY_T_PIPE)) {
      token_t pipe_tok = op;
      if (p->cur.kind == NY_T_LBRACK) {
        token_t lbrack = p->cur;
        parser_advance(p);
        expr_t *idx = p_parse_expr(p, 0);
        parser_expect(p, NY_T_RBRACK, "']' in piped index", NULL);
        expr_t *ix = expr_new(p->arena, NY_E_INDEX, lbrack);
        ix->as.index.target = left;
        ix->as.index.start = idx;
        left = ix;
        continue;
      }
      if (p->cur.kind == NY_T_DOT) {
        parser_advance(p);
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected identifier after '|> .'", NULL);
          return left;
        }
        token_t id_tok = p->cur;
        char *name = arena_strndup(p->arena, id_tok.lexeme, id_tok.len);
        parser_advance(p);
        if (p->cur.kind == NY_T_LPAREN) {
          parser_advance(p);
          expr_t *mc = expr_new(p->arena, NY_E_MEMCALL, id_tok);
          mc->as.memcall.target = left;
          mc->as.memcall.name = name;
          parse_call_arg_list(p, &mc->as.memcall.args,
                              "')' in piped member call");
          left = mc;
        } else {
          expr_t *m = expr_new(p->arena, NY_E_MEMBER, id_tok);
          m->as.member.target = left;
          m->as.member.name = name;
          left = m;
        }
        continue;
      }
      expr_t *rhs = p_parse_expr(p, pcur + 1);
      if (rhs->kind == NY_E_CALL) {
        call_arg_t arg = {.name = NULL, .val = left};
        vec_insert_arena(p->arena, &rhs->as.call.args, 0, arg);
        left = rhs;
      } else {
        expr_t *call = expr_new(p->arena, NY_E_CALL, pipe_tok);
        call->as.call.callee = rhs;
        call_arg_t arg = {.name = NULL, .val = left};
        vec_push_arena(p->arena, &call->as.call.args, arg);
        left = call;
      }
      continue;
    }

    parser_advance(p);
    expr_t *right = p_parse_expr(p, op.kind == NY_T_POW ? pcur : pcur + 1);

    if (op.kind == NY_T_QUESTION_QUESTION) {
      expr_t *tern = expr_new(p->arena, NY_E_TERNARY, op);
      tern->as.ternary.cond = left;
      tern->as.ternary.true_expr = left;
      tern->as.ternary.false_expr = right;
      left = tern;
    } else if (op.kind == NY_T_AND || op.kind == NY_T_OR) {
      expr_t *bin = expr_new(p->arena, NY_E_LOGICAL, op);
      bin->as.logical.op = (op.kind == NY_T_AND) ? "&&" : "||";
      bin->as.logical.left = left;
      bin->as.logical.right = right;
      left = bin;
    } else {
      expr_t *bin = expr_new(p->arena, NY_E_BINARY, op);
      bin->as.binary.op = arena_strndup(p->arena, op.lexeme, op.len);
      bin->as.binary.left = left;
      bin->as.binary.right = right;
      left = bin;
    }
  }
  if (prec == 0 && p->cur.kind == NY_T_QUESTION) {
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
      ternary->as.ternary.cond = left;
      ternary->as.ternary.true_expr = true_expr;
      ternary->as.ternary.false_expr = false_expr;
      left = ternary;
    }
  }
  return left;
}
