#include "parse/lexer.h"
#include "base/intern.h"
#include "base/util.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static const uint8_t ny_lex_table[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0,
    0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 2,
    0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0
};

#define IS_SPACE(c) (ny_lex_table[(uint8_t)(c)] & 1)
#define IS_ALPHA(c) (ny_lex_table[(uint8_t)(c)] & 2)
#define IS_DIGIT(c) (ny_lex_table[(uint8_t)(c)] & 4)
#define IS_ALNUM(c) (ny_lex_table[(uint8_t)(c)] & 6)

static token_t make_token(lexer_t *lx, token_kind kind, size_t start);
static void lexer_error(lexer_t *lx, size_t start, const char *msg,
                        const char *hint);
static void lex_number_suffix_digits(lexer_t *lx,
                                     bool underscore_allows_float);

static inline bool ny_is_alnum8(const unsigned char *p) {
  return (ny_lex_table[p[0]] & 6) && (ny_lex_table[p[1]] & 6) &&
         (ny_lex_table[p[2]] & 6) && (ny_lex_table[p[3]] & 6) &&
         (ny_lex_table[p[4]] & 6) && (ny_lex_table[p[5]] & 6) &&
         (ny_lex_table[p[6]] & 6) && (ny_lex_table[p[7]] & 6);
}

static inline bool ny_is_digit8(const unsigned char *p) {
  return (ny_lex_table[p[0]] & 4) && (ny_lex_table[p[1]] & 4) &&
         (ny_lex_table[p[2]] & 4) && (ny_lex_table[p[3]] & 4) &&
         (ny_lex_table[p[4]] & 4) && (ny_lex_table[p[5]] & 4) &&
         (ny_lex_table[p[6]] & 4) && (ny_lex_table[p[7]] & 4);
}

static inline bool ny_digit_sep_before_digit(const char *src, size_t pos) {
  return src[pos] == '_' && IS_DIGIT(src[pos + 1]);
}

static inline bool ny_digit_sep_before(const char *src, size_t pos,
                                       bool (*pred)(char)) {
  return src[pos] == '_' && pred(src[pos + 1]);
}

static inline bool ny_is_bindigit(char c) { return c == '0' || c == '1'; }

static inline bool ny_is_octdigit(char c) { return c >= '0' && c <= '7'; }

static inline bool ny_is_xdigit(char c) { return isxdigit((unsigned char)c); }

static token_t lex_radix_number(lexer_t *lx, size_t start, bool (*digit)(char),
                                bool require_digit, const char *err,
                                const char *hint) {
  const char *src = lx->src;
  lx->pos++;
  lx->col++;
  if (require_digit && !digit(src[lx->pos])) {
    lexer_error(lx, start, err, hint);
    return make_token(lx, NY_T_ERROR, start);
  }
  while (digit(src[lx->pos]) || ny_digit_sep_before(src, lx->pos, digit)) {
    lx->pos++;
    lx->col++;
  }
  lex_number_suffix_digits(lx, false);
  token_t tok = make_token(lx, NY_T_NUMBER, start);
  tok.hash = 0;
  return tok;
}

static void lex_decimal_digits(lexer_t *lx) {
  const char *src = lx->src;
  const unsigned char *end = (const unsigned char *)(src + lx->len);
  while ((const unsigned char *)(src + lx->pos) + 8 <= end &&
         ny_is_digit8((const unsigned char *)(src + lx->pos))) {
    lx->pos += 8;
    lx->col += 8;
  }
  while (IS_DIGIT(src[lx->pos]) || ny_digit_sep_before_digit(src, lx->pos)) {
    lx->pos++;
    lx->col++;
  }
}

static inline bool ny_number_suffix_letter(char c, bool allow_float) {
  return c == 'i' || c == 'I' || c == 'u' || c == 'U' ||
         (allow_float && (c == 'f' || c == 'F'));
}

static void lex_number_suffix_digits(lexer_t *lx,
                                     bool underscore_allows_float) {
  const char *src = lx->src;
  char s = src[lx->pos];
  if (s == '_' &&
      ny_number_suffix_letter(src[lx->pos + 1], underscore_allows_float)) {
    lx->pos++;
    lx->col++;
    s = src[lx->pos];
  }
  if (ny_number_suffix_letter(s, true) && IS_DIGIT(src[lx->pos + 1])) {
    lx->pos++;
    lx->col++;
    lex_decimal_digits(lx);
  }
}

static const unsigned char *scan_template_ident_tail(const unsigned char *p) {
  while (*p == '$' && p[1] == '{') {
    const unsigned char *q = p + 2;
    if (!IS_ALPHA(*q))
      break;
    while (IS_ALNUM(*q))
      q++;
    if (*q != '}')
      break;
    p = q + 1;
    while (IS_ALNUM(*p))
      p++;
  }
  return p;
}

static const char *lexer_intern_filename(const char *filename) {
  if (!filename)
    return NULL;
  ny_sym_id id = ny_intern_cstr(filename);
  return id ? ny_intern_get(id) : filename;
}

void lexer_init(lexer_t *lx, const char *src, const char *filename) {
  lx->src = src;
  lx->filename = lexer_intern_filename(filename);
  lx->len = src ? strlen(src) : 0;
  lx->source_has_newline = (src && memchr(src, '\n', lx->len) != NULL);
  lx->pos = 0;
  lx->line = 1;
  lx->real_line = 1;
  lx->col = 1;
  lx->split_pos = 0;
  lx->split_filename = NULL;
  lx->skipped_newline = false;
  lx->intern_identifiers = true;
  lx->had_error = false;
  lx->error_count = 0;
  lx->quiet = false;
}

static inline char advance(lexer_t *lx) {
  if (lx->src[lx->pos] == '\0')
    return '\0';
  if (lx->split_pos > 0 && lx->pos == lx->split_pos) {
    lx->line = 1;
    lx->col = 1;
    lx->filename = lx->split_filename;
  }
  char c = lx->src[lx->pos++];
  if (c == '\n') {
    lx->line++;
    lx->real_line++;
    lx->col = 1;
    lx->skipped_newline = true;
  } else {
    lx->col++;
  }
  return c;
}

static inline char peek(lexer_t *lx) { return lx->src[lx->pos]; }

static char peek_next(lexer_t *lx) {
  if (lx->src[lx->pos] == '\0')
    return '\0';
  return lx->src[lx->pos + 1];
}

static bool match(lexer_t *lx, char expected) {
  if (peek(lx) == expected) {
    advance(lx);
    return true;
  }
  return false;
}

static token_t make_token(lexer_t *lx, token_kind kind, size_t start) {
  token_t tok;
  tok.kind = kind;
  tok.lexeme = lx->src + start;
  tok.len = lx->pos - start;
  tok.sym_id = 0;
  tok.hash = 0;
  tok.line = lx->line;
  tok.real_line = lx->real_line;
  tok.col = lx->col - (int)tok.len;
  tok.filename = lx->filename;
  return tok;
}

static void lexer_error(lexer_t *lx, size_t start, const char *msg,
                        const char *hint) {
  lx->had_error = true;
  lx->error_count++;
  if (!ny_log_should_emit(msg))
    return;
  if (lx->quiet)
    return;
  int col = lx->col - (int)(lx->pos - start);
  fprintf(stderr, "%s:%d:%d: %s[lex]%s %serror:%s %s\n",
          lx->filename ? lx->filename : "<input>", lx->line, col,
          clr(NY_CLR_CYAN), clr(NY_CLR_RESET), clr(NY_CLR_RED),
          clr(NY_CLR_RESET), msg);
  if (hint) {
    fprintf(stderr, "       %shint:%s %s\n", clr(NY_CLR_YELLOW),
            clr(NY_CLR_RESET), hint);
  }
  if (lx->src && lx->real_line > 0) {
    const char *snippet_src = lx->src;
    int snippet_line = lx->real_line;
    if (lx->split_pos > 0 && lx->split_filename &&
        lx->filename == lx->split_filename) {
      snippet_src = lx->src + lx->split_pos;
      snippet_line = lx->line;
    }
    ny_print_snippet(snippet_src, snippet_line, col, 1, NY_CLR_RED);
  }
}

static void skip_whitespace(lexer_t *lx) {
  for (;;) {
    char c = peek(lx);
    if (IS_SPACE(c)) {
      if (c != '\n') {
        const char *src = lx->src;
        size_t pos = lx->pos;
        size_t limit = (lx->split_pos > 0 && pos < lx->split_pos)
                           ? lx->split_pos
                           : (size_t)-1;
        while (src[pos] != '\0' && IS_SPACE(src[pos]) && src[pos] != '\n') {
          if (pos == limit)
            break;
          pos++;
        }
        if (pos != lx->pos) {
          lx->col += (int)(pos - lx->pos);
          lx->pos = pos;
          continue;
        }
      }
      advance(lx);
    } else if (c == ';') {
      size_t semi_pos = lx->pos;
      advance(lx);
      size_t comment_start = lx->pos;
      while (peek(lx) != '\n' && peek(lx) != '\0')
        advance(lx);
      size_t comment_end = lx->pos;

      if (comment_end > comment_start) {

        if (!lx->source_has_newline) {

          const char *p = lx->src + comment_start;
          const char *end = lx->src + comment_end;
          while (p < end && (*p == ' ' || *p == '\t'))
            p++;
          if (p < end && *p != ';') {
            size_t line_start = semi_pos;
            while (line_start > 0 && lx->src[line_start - 1] != '\n')
              line_start--;
            const char *before = lx->src + semi_pos - 1;
            while (before > lx->src + line_start &&
                   (*before == ' ' || *before == '\t'))
              before--;
            if (before >= lx->src + line_start) {
              lexer_error(lx, semi_pos,
                          "';' starts a line comment — all text after it on "
                          "this line is ignored",
                          "In Nytrix, use newlines or just spaces to separate "
                          "statements, not ';' "
                          "(like Python)");
            }
          }
        }
      }
    } else if (c == '#') {
      size_t start_pos = lx->pos;
      bool bol = (start_pos == 0 || lx->src[start_pos - 1] == '\n');
      if (bol) {

        const char *p = lx->src + lx->pos + 1;
        while (*p == ' ' || *p == '\t')
          p++;
        bool is_line_dir = (strncmp(p, "line", 4) == 0 &&
                            (p[4] == ' ' || p[4] == '\t' || p[4] == '\0')) ||
                           isdigit(*p);
        bool is_shebang = (start_pos == 0 && *p == '!');

        if (is_line_dir) {
          advance(lx);
          p = lx->src + lx->pos;
          while (*p == ' ' || *p == '\t')
            p++;
          if (strncmp(p, "line", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
            p += 4;
            while (*p == ' ' || *p == '\t')
              p++;
          }
          if (isdigit(*p)) {
            char *end;
            int line_val = (int)strtoll(p, &end, 10);
            p = end;
            while (*p == ' ' || *p == '\t')
              p++;
            if (*p == '"') {
              p++;
              const char *f = p;
              while (*p && *p != '"' && *p != '\n')
                p++;
              if (*p == '"') {
                size_t flen = (size_t)(p - f);
                ny_sym_id file_id = ny_intern_str(f, flen);
                lx->filename = ny_intern_get(file_id);
                p++;
              }
            }
            lx->line = line_val;
            lx->pos = (size_t)(p - lx->src);
            lx->col = 1;
            while (peek(lx) != '\n' && peek(lx) != '\0')
              advance(lx);
            if (peek(lx) == '\n') {
              advance(lx);
              lx->line--;
              lx->real_line--;
            }
            continue;
          }
        } else if (is_shebang) {
          while (peek(lx) != '\n' && peek(lx) != '\0')
            advance(lx);
          continue;
        }
      }

      break;
    } else {
      break;
    }
  }
}

static token_kind identifier_type(lexer_t *lx, const char *start, size_t len) {
  switch (start[0]) {
  case 'a':
    if (len > 1) {
      switch (start[1]) {
      case 's':
        if (len == 2)
          return NY_T_AS;
        if (len == 3 && start[2] == 'm')
          return NY_T_ASM;
        break;
      case 'n':
        break;
      }
    }
    break;
  case 'b':
    if (len == 5 && memcmp(start, "break", 5) == 0)
      return NY_T_BREAK;
    break;
  case 'c':
    if (len == 4 && memcmp(start, "case", 4) == 0)
      return NY_T_MATCH;
    if (len == 5 && memcmp(start, "catch", 5) == 0)
      return NY_T_CATCH;
    if (len == 8 && memcmp(start, "continue", 8) == 0)
      return NY_T_CONTINUE;
    if (len == 8 && memcmp(start, "comptime", 8) == 0)
      return NY_T_COMPTIME;
    if (len == 5 && memcmp(start, "const", 5) == 0) {
      lexer_error(lx, (size_t)(start - lx->src),
                  "'const' is not a keyword in Nytrix",
                  "use 'def' for constants");
      return NY_T_IDENT;
    }
    break;
  case 'd':
    if (len == 5 && memcmp(start, "defer", 5) == 0)
      return NY_T_DEFER;
    if (len == 3 && memcmp(start, "def", 3) == 0)
      return NY_T_DEF;
    if (len == 3 && memcmp(start, "del", 3) == 0)
      return NY_T_DEL;
    break;
  case 'e':
    if (len == 4 && memcmp(start, "else", 4) == 0)
      return NY_T_ELSE;
    if (len == 4 && memcmp(start, "elif", 4) == 0)
      return NY_T_ELIF;
    if (len == 4 && memcmp(start, "enum", 4) == 0)
      return NY_T_ENUM;
    if (len == 5 && memcmp(start, "embed", 5) == 0)
      return NY_T_EMBED;
    if (len == 6 && memcmp(start, "extern", 6) == 0)
      return NY_T_EXTERN;
    break;
  case 'f':
    if (len > 1) {
      switch (start[1]) {
      case 'a':
        if (len == 5 && memcmp(start, "false", 5) == 0)
          return NY_T_FALSE;
        break;
      case 'n':
        if (len == 2)
          return NY_T_FN;
        break;
      case 'o':
        if (len == 3 && start[2] == 'r')
          return NY_T_FOR;
        break;
      }
    }
    break;
  case 'g':
    if (len == 4 && memcmp(start, "goto", 4) == 0)
      return NY_T_GOTO;
    break;
  case 'i':
    if (len == 2) {
      if (start[1] == 'f')
        return NY_T_IF;
      if (start[1] == 'n')
        return NY_T_IN;
    }
    if (len == 6 && memcmp(start, "import", 6) == 0) {
      lexer_error(lx, (size_t)(start - lx->src),
                  "'import' is not used in Nytrix", "use 'use' instead");
      return NY_T_IDENT;
    }
    if (len == 7 && memcmp(start, "include", 7) == 0) {
      return NY_T_IDENT;
    }
    break;
  case 'n':
    if (len == 3 && memcmp(start, "nil", 3) == 0) {
      return NY_T_NIL;
    }
    if (len == 4 && memcmp(start, "none", 4) == 0) {
      return NY_T_NIL;
    }
    break;
  case 'N':
    if (len == 4 && memcmp(start, "NULL", 4) == 0) {
      return NY_T_NIL;
    }
    break;
  case 'l':
    if (len == 6 && memcmp(start, "lambda", 6) == 0)
      return NY_T_LAMBDA;
    if (len == 6 && memcmp(start, "layout", 6) == 0)
      return NY_T_STRUCT;
    break;
  case 's':
    if (len == 6) {
      if (memcmp(start, "sizeof", 6) == 0)
        return NY_T_SIZEOF;
      if (memcmp(start, "struct", 6) == 0)
        return NY_T_STRUCT;
    }
    break;
  case 'v':
    if (len == 3 && memcmp(start, "var", 3) == 0) {
      lexer_error(lx, (size_t)(start - lx->src),
                  "'var' is not a keyword in Nytrix",
                  "use 'mut' or 'def' instead");
      return NY_T_IDENT;
    }
    if (len == 4 && memcmp(start, "void", 4) == 0) {
      lexer_error(lx, (size_t)(start - lx->src), "'void' is not used in Nytrix",
                  "simply omit it or use 'none'");
      return NY_T_IDENT;
    }
    break;
  case 'm':
    if (len == 5 && memcmp(start, "match", 5) == 0)
      return NY_T_MATCH;
    if (len == 3 && memcmp(start, "mut", 3) == 0)
      return NY_T_MUT;
    if (len == 6 && memcmp(start, "module", 6) == 0)
      return NY_T_MODULE;
    break;
  case 'p':
    if (len == 6 && memcmp(start, "printf", 6) == 0) {
      lexer_error(lx, (size_t)(start - lx->src),
                  "'printf' is not used in Nytrix", "use 'print' instead");
      return NY_T_IDENT;
    }
    break;
  case 'r':
    if (len == 6 && memcmp(start, "return", 6) == 0)
      return NY_T_RETURN;
    break;
  case 't':
    if (len == 4 && memcmp(start, "true", 4) == 0)
      return NY_T_TRUE;
    if (len == 3 && memcmp(start, "try", 3) == 0)
      return NY_T_TRY;
    break;
  case 'u':
    if (len == 3 && memcmp(start, "use", 3) == 0)
      return NY_T_USE;
    break;
  case 'w':
    if (len == 5 && memcmp(start, "while", 5) == 0)
      return NY_T_WHILE;
    break;
  }
  return NY_T_IDENT;
}

token_t lexer_next(lexer_t *lx) {
  lx->skipped_newline = false;
  skip_whitespace(lx);
  size_t start = lx->pos;
  const char *src = lx->src;
  size_t pos = lx->pos;
  int col = lx->col;
  if (src[pos] == '\0') {
    token_t tok;
    tok.kind = NY_T_EOF;
    tok.lexeme = src + start;
    tok.len = 0;
    tok.sym_id = 0;
    tok.line = lx->line;
    tok.col = lx->col;
    tok.filename = lx->filename;
    return tok;
  }
  char c;
  if (lx->split_pos > 0 && pos == lx->split_pos) {
    c = advance(lx);
    pos = lx->pos;
    col = lx->col;
  } else {
    c = src[pos++];
    col++;
    lx->pos = pos;
    lx->col = col;
  }
  if (c == 'f' && (peek(lx) == '"' || peek(lx) == '\'')) {
    char quote = peek(lx);
    advance(lx);
    bool terminated = false;
    if (peek(lx) == quote && peek_next(lx) == quote) {
      advance(lx);
      advance(lx);
      while (peek(lx) != '\0') {
        if (peek(lx) == quote && peek_next(lx) == quote &&
            lx->src[lx->pos + 2] == quote) {
          advance(lx);
          advance(lx);
          advance(lx);
          terminated = true;
          break;
        }
        advance(lx);
      }
    } else {
      while (peek(lx) != quote && peek(lx) != '\0') {
        if (peek(lx) == '\\' && peek_next(lx) != '\0')
          advance(lx);
        advance(lx);
      }
      if (peek(lx) == quote) {
        advance(lx);
        terminated = true;
      }
    }
    if (terminated)
      return make_token(lx, NY_T_FSTRING, start);
    lexer_error(lx, start, "unterminated f-string",
                "check for missing closing quote");
    return make_token(lx, NY_T_ERROR, start);
  }
  if (IS_ALPHA(c)) {
    const unsigned char *p = (const unsigned char *)(src + lx->pos);
    const unsigned char *end = (const unsigned char *)(src + lx->len);
    while (p + 8 <= end && ny_is_alnum8(p)) {
      p += 8;
    }
    while (IS_ALNUM(*p)) {
      p++;
    }
    p = scan_template_ident_tail(p);
    size_t len = (size_t)((const char *)p - (src + lx->pos));
    lx->pos = (size_t)((const char *)p - src);
    lx->col += (int)len;
    token_t tok = make_token(lx, NY_T_IDENT, start);
    tok.hash = ny_hash64(tok.lexeme, tok.len);
    if (lx->intern_identifiers)
      tok.sym_id = ny_intern_str(tok.lexeme, tok.len);
    tok.kind = identifier_type(lx, tok.lexeme, tok.len);
    return tok;
  }
  if (c == '$' && peek(lx) == '{') {
    const unsigned char *p = (const unsigned char *)(src + start + 2);
    if (IS_ALPHA(*p)) {
      while (IS_ALNUM(*p))
        p++;
      if (*p == '}') {
        p++;
        while (IS_ALNUM(*p))
          p++;
        p = scan_template_ident_tail(p);
        size_t token_len = (size_t)((const char *)p - (src + start));
        lx->pos = start + token_len;
        lx->col += (int)token_len - 1;
        token_t tok = make_token(lx, NY_T_IDENT, start);
        tok.hash = ny_hash64(tok.lexeme, tok.len);
        if (lx->intern_identifiers)
          tok.sym_id = ny_intern_str(tok.lexeme, tok.len);
        tok.kind = identifier_type(lx, tok.lexeme, tok.len);
        return tok;
      }
    }
  }
  if (IS_DIGIT(c)) {
    if (c == '0' && (src[lx->pos] == 'x' || src[lx->pos] == 'X')) {
      return lex_radix_number(lx, start, ny_is_xdigit, false, NULL, NULL);
    }
    if (c == '0' && (src[lx->pos] == 'b' || src[lx->pos] == 'B')) {
      return lex_radix_number(
          lx, start, ny_is_bindigit, true, "malformed binary numeric constant",
          "binary literals need at least one 0 or 1 digit after 0b");
    }
    if (c == '0' && (src[lx->pos] == 'o' || src[lx->pos] == 'O')) {
      return lex_radix_number(
          lx, start, ny_is_octdigit, true, "malformed octal numeric constant",
          "octal literals need at least one 0..7 digit after 0o");
    }
    lex_decimal_digits(lx);
    if (src[lx->pos] == '.' && IS_DIGIT(src[lx->pos + 1])) {
      lx->pos++;
      lx->col++;
      lex_decimal_digits(lx);
    }
    if (src[lx->pos] == 'e' || src[lx->pos] == 'E') {
      lx->pos++;
      lx->col++;
      if (src[lx->pos] == '+' || src[lx->pos] == '-') {
        lx->pos++;
        lx->col++;
      }
      if (IS_DIGIT(src[lx->pos])) {
        lex_decimal_digits(lx);
      } else {
        lexer_error(
            lx, start, "malformed numeric constant",
            "exponent must be followed by at least one digit (e.g. 1.0e10)");
        return make_token(lx, NY_T_ERROR, start);
      }
    }
    lex_number_suffix_digits(lx, true);
    token_t tok = make_token(lx, NY_T_NUMBER, start);
    tok.hash = 0;
    return tok;
  }
  if (c == '"' || c == '\'') {
    char quote = c;
    bool terminated = false;
    if (peek(lx) == quote && peek_next(lx) == quote) {
      advance(lx);
      advance(lx);
      while (peek(lx) != '\0') {
        if (peek(lx) == quote && peek_next(lx) == quote &&
            lx->src[lx->pos + 2] == quote) {
          advance(lx);
          advance(lx);
          advance(lx);
          terminated = true;
          break;
        }
        advance(lx);
      }
    } else {
      while (peek(lx) != quote && peek(lx) != '\0') {
        if (peek(lx) == '\\' && peek_next(lx) != '\0')
          advance(lx);
        advance(lx);
      }
      if (peek(lx) == quote) {
        advance(lx);
        terminated = true;
      }
    }
    if (terminated)
      return make_token(lx, NY_T_STRING, start);
    lexer_error(lx, start, "unterminated string literal",
                "check for missing closing quote");
    return make_token(lx, NY_T_ERROR, start);
  }
  switch (c) {
  case '(':
    return make_token(lx, NY_T_LPAREN, start);
  case ')':
    return make_token(lx, NY_T_RPAREN, start);
  case '{':
    return make_token(lx, NY_T_LBRACE, start);
  case '}':
    return make_token(lx, NY_T_RBRACE, start);
  case '[':
    return make_token(lx, NY_T_LBRACK, start);
  case ']':
    return make_token(lx, NY_T_RBRACK, start);
  case ',':
    return make_token(lx, NY_T_COMMA, start);
  case '.':
    if (match(lx, '.')) {
      if (match(lx, '.'))
        return make_token(lx, NY_T_ELLIPSIS, start);
      return make_token(lx, NY_T_RANGE, start);
    }
    return make_token(lx, NY_T_DOT, start);
  case '-':
    if (match(lx, '>'))
      return make_token(lx, NY_T_ARROW, start);
    if (match(lx, '='))
      return make_token(lx, NY_T_MINUS_EQ, start);
    if (match(lx, '-'))
      return make_token(lx, NY_T_MINUS_MINUS, start);
    return make_token(lx, NY_T_MINUS, start);
  case '+':
    if (peek(lx) == '%') {
      advance(lx);
      lexer_error(lx, start, "wrapping operator '+%' is not supported",
                  "standard operators wrap on overflow for unsigned types");
      return lexer_next(lx);
    }
    if (match(lx, '='))
      return make_token(lx, NY_T_PLUS_EQ, start);
    if (match(lx, '+'))
      return make_token(lx, NY_T_PLUS_PLUS, start);
    return make_token(lx, NY_T_PLUS, start);
  case '*':
    if (peek(lx) == '%') {
      advance(lx);
      lexer_error(lx, start, "wrapping operator '*%' is not supported",
                  "standard operators wrap on overflow for unsigned types");
      return lexer_next(lx);
    }
    if (match(lx, '='))
      return make_token(lx, NY_T_STAR_EQ, start);
    return make_token(lx, NY_T_STAR, start);
  case '/':
    if (match(lx, '='))
      return make_token(lx, NY_T_SLASH_EQ, start);
    if (match(lx, '/')) {
      lexer_error(lx, start, "C-style line comments '//' are not supported",
                  "use ';' instead");
      while (peek(lx) != '\n' && peek(lx) != '\0')
        advance(lx);
      return lexer_next(lx);
    }
    if (match(lx, '*')) {
      lexer_error(lx, start, "C-style block comments '/* */' are not supported",
                  "use ';' for line comments or spaces for inlining");
      while (peek(lx) != '\0' && !(peek(lx) == '*' && peek_next(lx) == '/'))
        advance(lx);
      if (peek(lx) == '*') {
        advance(lx);
        advance(lx);
      }
      return lexer_next(lx);
    }
    return make_token(lx, NY_T_SLASH, start);
  case '%':
    if (peek(lx) == '=') {
      advance(lx);
      return make_token(lx, NY_T_PERCENT_EQ, start);
    }
    return make_token(lx, NY_T_PERCENT, start);
  case '!':
    if (match(lx, '=')) {
      if (match(lx, '=')) {
        lexer_error(lx, start,
                    "strict inequality operator '!==' is not supported",
                    "use '!=' instead");
        return lexer_next(lx);
      }
      return make_token(lx, NY_T_NEQ, start);
    }
    return make_token(lx, NY_T_NOT, start);
  case '=':
    if (match(lx, '=')) {
      if (match(lx, '=')) {
        lexer_error(lx, start,
                    "strict equality operator '===' is not supported",
                    "use '==' instead");
        return lexer_next(lx);
      }
      return make_token(lx, NY_T_EQ, start);
    }
    if (match(lx, '>')) {
      lexer_error(lx, start, "fat arrow operator '=>' is not supported",
                  "use '->' for case and match arms");
      return lexer_next(lx);
    }
    return make_token(lx, NY_T_ASSIGN, start);
  case '<':
    if (match(lx, '='))
      return make_token(lx, NY_T_LE, start);
    if (match(lx, '<'))
      return make_token(lx, NY_T_LSHIFT, start);
    return make_token(lx, NY_T_LT, start);
  case '>':
    if (match(lx, '='))
      return make_token(lx, NY_T_GE, start);
    if (match(lx, '>'))
      return make_token(lx, NY_T_RSHIFT, start);
    return make_token(lx, NY_T_GT, start);
  case '&':
    if (match(lx, '&'))
      return make_token(lx, NY_T_AND, start);
    return make_token(lx, NY_T_BITAND, start);
  case '|':
    if (match(lx, '|'))
      return make_token(lx, NY_T_OR, start);
    if (match(lx, '>'))
      return make_token(lx, NY_T_PIPE, start);
    return make_token(lx, NY_T_BITOR, start);
  case '^':
    if (match(lx, '^'))
      return make_token(lx, NY_T_BITXOR, start);
    return make_token(lx, NY_T_POW, start);
  case '~':
    return make_token(lx, NY_T_BITNOT, start);
  case ':':
    return make_token(lx, NY_T_COLON, start);
  case '#':
    return make_token(lx, NY_T_HASH, start);
  case '?':
    if (match(lx, '?'))
      return make_token(lx, NY_T_QUESTION_QUESTION, start);
    if (match(lx, '.'))
      return make_token(lx, NY_T_QUESTION_DOT, start);
    return make_token(lx, NY_T_QUESTION, start);
  case '@':
    return make_token(lx, NY_T_AT, start);
  }
  char emsg[128];
  snprintf(emsg, sizeof(emsg), "unrecognised character '%c' (ascii %d)", c,
           (int)c);
  lexer_error(lx, start, emsg,
              "check for accidental non-ascii characters or typos");
  return lexer_next(lx);
}

char *dup_token_lexeme(token_t t) {
  if (!t.lexeme || t.len == 0)
    return NULL;
  char *out = malloc(t.len + 1);
  if (!out)
    return NULL;
  memcpy(out, t.lexeme, t.len);
  out[t.len] = '\0';
  return out;
}

char *dup_string_token(token_t t) {
  if (t.len < 2)
    return NULL;
  size_t head = 1, tail = 1;
  if (t.len >= 6 && t.lexeme[0] == t.lexeme[1] && t.lexeme[1] == t.lexeme[2]) {
    head = 3;
    tail = 3;
  }
  if (t.len < head + tail)
    return NULL;
  token_t inner = t;
  inner.lexeme += head;
  inner.len -= head + tail;
  char *out = dup_token_lexeme(inner);
  return out;
}

char *parse_use_name(lexer_t *lx, token_t *entry_tok, token_t *out_last_tok) {
  token_t t = *entry_tok;
  if (t.kind == NY_T_STRING) {
    char *name = dup_string_token(t);
    if (out_last_tok)
      *out_last_tok = lexer_next(lx);
    return name;
  }
  if (t.kind != NY_T_IDENT && t.kind != NY_T_NUMBER) {
    if (out_last_tok)
      *out_last_tok = t;
    return NULL;
  }
  size_t cap = 64, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    if (out_last_tok)
      *out_last_tok = t;
    return NULL;
  }
  memcpy(buf, t.lexeme, t.len);
  len += t.len;

  token_t next;
  while ((next = lexer_next(lx)).kind == NY_T_IDENT &&
         next.col == (int)(t.col + t.len)) {
    if (len + next.len + 1 > cap) {
      cap = (len + next.len + 1) * 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        if (out_last_tok)
          *out_last_tok = next;
        return NULL;
      }
      buf = nb;
    }
    memcpy(buf + len, next.lexeme, next.len);
    len += next.len;
    t = next;
  }

  token_t tok = next;
  for (;;) {
    if (tok.kind == NY_T_DOT) {
      token_t id = lexer_next(lx);
      if (id.kind != NY_T_IDENT && id.kind != NY_T_NUMBER) {
        free(buf);
        if (out_last_tok)
          *out_last_tok = id;
        return NULL;
      }
      if (len + 1 + id.len + 1 > cap) {
        cap = (len + 1 + id.len + 1) * 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          free(buf);
          if (out_last_tok)
            *out_last_tok = id;
          return NULL;
        }
        buf = nb;
      }
      buf[len++] = '.';
      memcpy(buf + len, id.lexeme, id.len);
      len += id.len;

      t = id;
      while ((next = lexer_next(lx)).kind == NY_T_IDENT &&
             next.col == (int)(t.col + t.len)) {
        if (len + next.len + 1 > cap) {
          cap = (len + next.len + 1) * 2;
          char *nb = realloc(buf, cap);
          if (!nb) {
            free(buf);
            if (out_last_tok)
              *out_last_tok = next;
            return NULL;
          }
          buf = nb;
        }
        memcpy(buf + len, next.lexeme, next.len);
        len += next.len;
        t = next;
      }
      tok = next;
    } else {
      if (out_last_tok)
        *out_last_tok = tok;
      break;
    }
  }
  buf[len] = '\0';
  return buf;
}
