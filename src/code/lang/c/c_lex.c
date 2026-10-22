/*
 * Shared C lexer for the FFI header parser and the standalone nyc frontend.
 * c_parse.c consumes declarations; c/nyc_frontend.c lowers supported function
 * bodies. Both use this one token stream implementation.
 */
#include "code/lang/c/c.h"
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
  lx->line_start = 1;
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
  case NY_CTOK_ERROR:
    return "error";
  }
  return "unknown";
}

int ny_ctok_eq(ny_ctok_t tok, const char *lit) {
  return tok.start && lit && strlen(lit) == tok.len &&
         memcmp(tok.start, lit, tok.len) == 0;
}

int ny_ctok_is_ident(ny_ctok_t tok, const char *lit) {
  return tok.kind == NY_CTOK_IDENT && ny_ctok_eq(tok, lit);
}

static int lex_has(const ny_lexer_t *lx, size_t off) {
  return lx && lx->pos <= lx->len && off < lx->len - lx->pos;
}

static int lex_peek(const ny_lexer_t *lx, size_t off) {
  return lex_has(lx, off) ? (unsigned char)lx->src[lx->pos + off] : 0;
}

static int lex_get(ny_lexer_t *lx) {
  if (!lex_has(lx, 0))
    return 0;
  int c = (unsigned char)lx->src[lx->pos++];
  if (c == '\r') {
    if (lex_has(lx, 0) && lx->src[lx->pos] == '\n')
      lx->pos++;
    lx->line++;
    lx->col = 1;
    lx->line_start = 1;
    return '\n';
  }
  if (c == '\n') {
    lx->line++;
    lx->col = 1;
    lx->line_start = 1;
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
  t.error = NULL;
  if (kind != NY_CTOK_EOF)
    lx->line_start = 0;
  return t;
}

static ny_ctok_t lex_error(ny_lexer_t *lx, size_t start, unsigned line,
                           unsigned col, const char *message) {
  if (lx->pos == start && lex_has(lx, 0))
    (void)lex_get(lx);
  ny_ctok_t tok = lex_tok(lx, NY_CTOK_ERROR, start, line, col);
  tok.error = message;
  return tok;
}

static int lex_skip_ws_and_comments(ny_lexer_t *lx, ny_ctok_t *error) {
  for (;;) {
    int c = lex_peek(lx, 0);
    if (!lex_has(lx, 0))
      return 1;
    if (c == 0) {
      *error =
          lex_error(lx, lx->pos, lx->line, lx->col, "NUL byte in C source");
      return 0;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
        c == '\v') {
      (void)lex_get(lx);
      continue;
    }
    if (c == '/' && lex_peek(lx, 1) == '/') {
      while (lex_has(lx, 0)) {
        int next = lex_peek(lx, 0);
        if (next == 0) {
          *error =
              lex_error(lx, lx->pos, lx->line, lx->col, "NUL byte in C source");
          return 0;
        }
        if (next == '\n' || next == '\r')
          break;
        (void)lex_get(lx);
      }
      continue;
    }
    if (c == '/' && lex_peek(lx, 1) == '*') {
      size_t start = lx->pos;
      unsigned line = lx->line;
      unsigned col = lx->col;
      (void)lex_get(lx);
      (void)lex_get(lx);
      int closed = 0;
      while (lex_has(lx, 0)) {
        if (lex_peek(lx, 0) == 0) {
          *error =
              lex_error(lx, lx->pos, lx->line, lx->col, "NUL byte in C source");
          return 0;
        }
        if (lex_peek(lx, 0) == '*' && lex_peek(lx, 1) == '/') {
          (void)lex_get(lx);
          (void)lex_get(lx);
          closed = 1;
          break;
        }
        (void)lex_get(lx);
      }
      if (!closed) {
        *error = lex_error(lx, start, line, col, "unterminated block comment");
        return 0;
      }
      continue;
    }
    return 1;
  }
}

static const char *lex_read_quoted(ny_lexer_t *lx, int quote) {
  (void)lex_get(lx);
  while (lex_has(lx, 0)) {
    int c = lex_peek(lx, 0);
    if (c == 0) {
      (void)lex_get(lx);
      return "NUL byte in C source";
    }
    if (c == '\n' || c == '\r')
      return quote == '"' ? "newline in string literal"
                          : "newline in character constant";
    if (c == '\\') {
      (void)lex_get(lx);
      if (!lex_has(lx, 0))
        return "unterminated escape sequence in literal";
      c = lex_peek(lx, 0);
      if (c == 0) {
        (void)lex_get(lx);
        return "NUL byte in C source";
      }
      (void)lex_get(lx);
      continue;
    }
    (void)lex_get(lx);
    if (c == quote)
      return NULL;
  }
  return quote == '"' ? "unterminated string literal"
                      : "unterminated character constant";
}

static int lex_is_ident_start(int c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int lex_is_ident_continue(int c) {
  return lex_is_ident_start(c) || (c >= '0' && c <= '9');
}

static int lex_matches(const ny_lexer_t *lx, const char *s, size_t len) {
  for (size_t i = 0; i < len; ++i)
    if (!lex_has(lx, i) || lex_peek(lx, i) != (unsigned char)s[i])
      return 0;
  return 1;
}

/*
 * Longest-match punctuator table (design reference: stb_c_lexer.h's
 * longest-first operator matching). Multi-character punctuators only;
 * single characters fall through to the default one-character consume in
 * ny_lex_next. Entries are ordered longest-first so `>>=` wins over `>>`
 * and `%:%:` wins over `%:`.
 */
static int lex_punctuator_len(const ny_lexer_t *lx) {
  static const struct {
    const char *text;
    size_t len;
  } ops[] = {
      {"%:%:", 4},
      {"...", 3}, {"<<=", 3}, {">>=", 3},
      {"->", 2},  {"++", 2},  {"--", 2},  {"<<", 2},  {">>", 2},
      {"<=", 2},  {">=", 2},  {"==", 2},  {"!=", 2},  {"&&", 2},
      {"||", 2},  {"+=", 2},  {"-=", 2},  {"*=", 2},  {"/=", 2},
      {"%=", 2},  {"&=", 2},  {"|=", 2},  {"^=", 2},  {"##", 2},
      {"<:", 2},  {":>", 2},  {"<%", 2},  {"%>", 2},  {"%:", 2},
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
    if (lex_matches(lx, ops[i].text, ops[i].len))
      return ops[i].len;
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
    if (!lex_has(lx, 0))
      break;
    int c = lex_peek(lx, 0);
    if (isalnum((unsigned char)c) || c == '.' || c == '_') {
      exponent = c == 'e' || c == 'E' || c == 'p' || c == 'P';
      (void)lex_get(lx);
      continue;
    }
    /*
     * C23 digit separator: a quote between digits stays inside the numeric
     * literal (mirrors the parser's existing '_' separator handling in
     * c_parse_integer_slice). A quote not followed by a digit is left for the
     * character-literal path, so `1'x'` still lexes as `1` + `'x'`.
     */
    if (c == '\'' && lex_has(lx, 1) &&
        isdigit((unsigned char)lex_peek(lx, 1))) {
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
    return (ny_ctok_t){.kind = NY_CTOK_EOF, .start = "", .line = 0, .col = 0};
  ny_ctok_t error;
  if (!lex_skip_ws_and_comments(lx, &error))
    return error;
  size_t start = lx->pos;
  unsigned line = lx->line;
  unsigned col = lx->col;
  int c = lex_peek(lx, 0);
  if (!lex_has(lx, 0))
    return lex_tok(lx, NY_CTOK_EOF, start, line, col);
  if (c == 0)
    return lex_error(lx, start, line, col, "NUL byte in C source");

  if (c == '#' && lx->line_start) {
    while (lex_has(lx, 0)) {
      if (lex_peek(lx, 0) == 0)
        return lex_error(lx, start, line, col, "NUL byte in C source");
      if (lex_peek(lx, 0) == '\\' &&
          (lex_peek(lx, 1) == '\r' || lex_peek(lx, 1) == '\n')) {
        (void)lex_get(lx);
        (void)lex_get(lx);
        continue;
      }
      if (lex_peek(lx, 0) == '\n' || lex_peek(lx, 0) == '\r')
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
    const char *message = lex_read_quoted(lx, quote);
    if (message)
      return lex_error(lx, start, line, col, message);
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
    const char *message = lex_read_quoted(lx, '"');
    if (message)
      return lex_error(lx, start, line, col, message);
    return lex_tok(lx, NY_CTOK_STRING, start, line, col);
  }
  if (c == '\'') {
    const char *message = lex_read_quoted(lx, '\'');
    if (message)
      return lex_error(lx, start, line, col, message);
    return lex_tok(lx, NY_CTOK_CHAR, start, line, col);
  }

  int pl = lex_punctuator_len(lx);
  if (pl > 0) {
    for (int i = 0; i < pl; ++i)
      (void)lex_get(lx);
  } else {
    (void)lex_get(lx);
  }
  return lex_tok(lx, NY_CTOK_PUNCT, start, line, col);
}
