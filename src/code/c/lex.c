#include "code/c/c.h"
#include <ctype.h>
#include <string.h>

/*
 * Internal C frontend lexer.
 *
 * This is the Nytrix-owned C token stream foundation used by the
 * --c-frontend=nytrix/auto probe path. It intentionally covers header-import
 * syntax first: identifiers, numbers, strings/chars with prefixes, comments,
 * preprocessor lines with continuations, and common punctuators/digraphs.
 * libclang remains the production fallback for complex headers.
 */

void ny_lex_init(ny_lexer_t *lx, const char *src, size_t len) {
  if (!lx)
    return;
  lx->src = src ? src : "";
  lx->len = src ? len : 0;
  lx->pos = 0;
  lx->line = 1;
  lx->col = 1;
}

const char *ny_ctok_kind_name(ny_ctok_kind_t kind) {
  switch (kind) {
  case NY_CTOK_EOF:
    return "eof";
  case NY_CTOK_IDENT:
    return "ident";
  case NY_CTOK_NUMBER:
    return "number";
  case NY_CTOK_STRING:
    return "string";
  case NY_CTOK_CHAR:
    return "char";
  case NY_CTOK_PUNCT:
    return "punct";
  case NY_CTOK_PREPROC:
    return "preproc";
  }
  return "unknown";
}

int ny_ctok_eq(ny_ctok_t tok, const char *lit) {
  return lit && strlen(lit) == tok.len && memcmp(tok.start, lit, tok.len) == 0;
}

int ny_ctok_is_ident(ny_ctok_t tok, const char *lit) {
  return tok.kind == NY_CTOK_IDENT && ny_ctok_eq(tok, lit);
}

static int lex_peek(ny_lexer_t *lx, size_t off) {
  size_t p = lx->pos + off;
  return p < lx->len ? (unsigned char)lx->src[p] : 0;
}

static int lex_get(ny_lexer_t *lx) {
  if (lx->pos >= lx->len)
    return 0;
  int c = (unsigned char)lx->src[lx->pos++];
  if (c == '\n') {
    lx->line++;
    lx->col = 1;
  } else {
    lx->col++;
  }
  return c;
}

static ny_ctok_t lex_tok(ny_lexer_t *lx, ny_ctok_kind_t kind, size_t start,
                          unsigned line, unsigned col) {
  ny_ctok_t t;
  t.kind = kind;
  t.start = lx->src + start;
  t.len = lx->pos - start;
  t.line = line;
  t.col = col;
  return t;
}

static void lex_skip_ws_and_comments(ny_lexer_t *lx) {
  for (;;) {
    int c = lex_peek(lx, 0);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
        c == '\v') {
      (void)lex_get(lx);
      continue;
    }
    if (c == '/' && lex_peek(lx, 1) == '/') {
      while (lex_peek(lx, 0) && lex_peek(lx, 0) != '\n')
        (void)lex_get(lx);
      continue;
    }
    if (c == '/' && lex_peek(lx, 1) == '*') {
      (void)lex_get(lx);
      (void)lex_get(lx);
      while (lex_peek(lx, 0)) {
        if (lex_peek(lx, 0) == '*' && lex_peek(lx, 1) == '/') {
          (void)lex_get(lx);
          (void)lex_get(lx);
          break;
        }
        (void)lex_get(lx);
      }
      continue;
    }
    break;
  }
}

static void lex_read_quoted(ny_lexer_t *lx, int quote) {
  (void)lex_get(lx);
  while (lex_peek(lx, 0)) {
    int c = lex_get(lx);
    if (c == '\\' && lex_peek(lx, 0)) {
      (void)lex_get(lx);
      continue;
    }
    if (c == quote)
      break;
  }
}

static int lex_is_ident_start(int c) {
  return c == '_' || isalpha((unsigned char)c);
}

static int lex_is_ident_continue(int c) {
  return c == '_' || isalnum((unsigned char)c);
}

static int lex_is_three_punct(const char *s) {
  static const char *ops[] = {"...", "<<=", ">>=", "%:%:", NULL};
  for (size_t i = 0; ops[i]; ++i) {
    if (s[0] == ops[i][0] && s[1] == ops[i][1] && s[2] == ops[i][2])
      return 1;
  }
  return 0;
}

static int lex_is_two_punct(const char *s) {
  static const char *ops[] = {"->", "++", "--", "<<", ">>", "<=", ">=", "==",
                              "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=",
                              "&=", "|=", "^=", "##", "<:", ":>", "<%", "%>",
                              "%:", NULL};
  for (size_t i = 0; ops[i]; ++i) {
    if (s[0] == ops[i][0] && s[1] == ops[i][1])
      return 1;
  }
  return 0;
}

static int lex_quote_prefix_len(ny_lexer_t *lx, int *quote) {
  int c0 = lex_peek(lx, 0);
  int c1 = lex_peek(lx, 1);
  int c2 = lex_peek(lx, 2);
  if ((c0 == 'L' || c0 == 'u' || c0 == 'U') && (c1 == '"' || c1 == '\'')) {
    *quote = c1;
    return 1;
  }
  if (c0 == 'u' && c1 == '8' && (c2 == '"' || c2 == '\'')) {
    *quote = c2;
    return 2;
  }
  return 0;
}

static void lex_read_number(ny_lexer_t *lx) {
  int exponent = 0;
  (void)lex_get(lx);
  for (;;) {
    int c = lex_peek(lx, 0);
    if (isalnum((unsigned char)c) || c == '.' || c == '_') {
      exponent = c == 'e' || c == 'E' || c == 'p' || c == 'P';
      (void)lex_get(lx);
      continue;
    }
    if ((c == '+' || c == '-') && exponent) {
      exponent = 0;
      (void)lex_get(lx);
      continue;
    }
    break;
  }
}

ny_ctok_t ny_lex_next(ny_lexer_t *lx) {
  if (!lx)
    return (ny_ctok_t){NY_CTOK_EOF, "", 0, 0, 0};
  lex_skip_ws_and_comments(lx);
  size_t start = lx->pos;
  unsigned line = lx->line;
  unsigned col = lx->col;
  int c = lex_peek(lx, 0);
  if (!c)
    return lex_tok(lx, NY_CTOK_EOF, start, line, col);

  if (c == '#') {
    while (lex_peek(lx, 0)) {
      if (lex_peek(lx, 0) == '\\' && lex_peek(lx, 1) == '\r' &&
          lex_peek(lx, 2) == '\n') {
        (void)lex_get(lx);
        (void)lex_get(lx);
        (void)lex_get(lx);
        continue;
      }
      if (lex_peek(lx, 0) == '\\' && lex_peek(lx, 1) == '\n') {
        (void)lex_get(lx);
        (void)lex_get(lx);
        continue;
      }
      if (lex_peek(lx, 0) == '\n')
        break;
      (void)lex_get(lx);
    }
    return lex_tok(lx, NY_CTOK_PREPROC, start, line, col);
  }

  int quote = 0;
  int prefix_len = lex_quote_prefix_len(lx, &quote);
  if (prefix_len > 0) {
    for (int i = 0; i < prefix_len; ++i)
      (void)lex_get(lx);
    lex_read_quoted(lx, quote);
    return lex_tok(lx, quote == '"' ? NY_CTOK_STRING : NY_CTOK_CHAR, start,
                    line, col);
  }

  if (lex_is_ident_start(c)) {
    (void)lex_get(lx);
    while (lex_is_ident_continue(lex_peek(lx, 0)))
      (void)lex_get(lx);
    return lex_tok(lx, NY_CTOK_IDENT, start, line, col);
  }

  if (isdigit((unsigned char)c) ||
      (c == '.' && isdigit((unsigned char)lex_peek(lx, 1)))) {
    lex_read_number(lx);
    return lex_tok(lx, NY_CTOK_NUMBER, start, line, col);
  }

  if (c == '"') {
    lex_read_quoted(lx, '"');
    return lex_tok(lx, NY_CTOK_STRING, start, line, col);
  }
  if (c == '\'') {
    lex_read_quoted(lx, '\'');
    return lex_tok(lx, NY_CTOK_CHAR, start, line, col);
  }

  if (lex_is_three_punct(lx->src + lx->pos)) {
    (void)lex_get(lx);
    (void)lex_get(lx);
    (void)lex_get(lx);
  } else if (lex_is_two_punct(lx->src + lx->pos)) {
    (void)lex_get(lx);
    (void)lex_get(lx);
  } else {
    (void)lex_get(lx);
  }
  return lex_tok(lx, NY_CTOK_PUNCT, start, line, col);
}
