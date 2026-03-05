#include "lex/lexer.h"
#include "base/util.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static const uint8_t ny_lex_table[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, // 0-15
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 16-31
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 32-47 (space is 1)
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, // 48-63 (digits are 4)
    0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 64-79 (A-O are 2)
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 2, // 80-95 (P-Z are 2, _ is 2)
    0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 96-111 (a-o are 2)
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0  // 112-127 (p-z are 2)
};

#define IS_SPACE(c) (ny_lex_table[(uint8_t)(c)] & 1)
#define IS_ALPHA(c) (ny_lex_table[(uint8_t)(c)] & 2)
#define IS_DIGIT(c) (ny_lex_table[(uint8_t)(c)] & 4)
#define IS_ALNUM(c) (ny_lex_table[(uint8_t)(c)] & 6)

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

void lexer_init(lexer_t *lx, const char *src, const char *filename) {
  lx->src = src;
  lx->filename = filename;
  lx->len = src ? strlen(src) : 0;
  lx->pos = 0;
  lx->line = 1;
  lx->real_line = 1;
  lx->col = 1;
  lx->split_pos = 0;
  lx->split_filename = NULL;
  lx->skipped_newline = false;
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
  tok.hash = 0;
  tok.line = lx->line;
  tok.real_line = lx->real_line;
  tok.col = lx->col - (int)tok.len;
  tok.filename = lx->filename;
  return tok;
}

static void lexer_error(lexer_t *lx, size_t start, const char *msg,
                        const char *hint) {
  if (!ny_log_should_emit(msg))
    return;
  int col = lx->col - (int)(lx->pos - start);
  fprintf(stderr, "%s:%d:%d: \033[31merror:\033[0m %s\n",
          lx->filename ? lx->filename : "<input>", lx->line, col, msg);
  if (hint) {
    fprintf(stderr, "%s:%d:%d: \033[33mnote:\033[0m %s\n",
            lx->filename ? lx->filename : "<input>", lx->line, col, hint);
  }
  if (lx->src && lx->real_line > 0) {
    ny_print_snippet(lx->src, lx->real_line, col, 1, "\033[31m");
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
      advance(lx);
      while (peek(lx) != '\n' && peek(lx) != '\0')
        advance(lx);
    } else if (c == '#') {
      size_t start_pos = lx->pos;
      bool bol = (start_pos == 0 || lx->src[start_pos - 1] == '\n');
      if (bol) {
        // Peek ahead to see if it's a line directive or shebang
        const char *p = lx->src + lx->pos + 1;
        while (*p == ' ' || *p == '\t')
          p++;
        bool is_line_dir = (strncmp(p, "line", 4) == 0 &&
                            (p[4] == ' ' || p[4] == '\t' || p[4] == '\0')) ||
                           isdigit(*p);
        bool is_shebang = (start_pos == 0 && *p == '!');

        if (is_line_dir) {
          advance(lx); // consume '#'
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
                char *nf = malloc(flen + 1);
                memcpy(nf, f, flen);
                nf[flen] = '\0';
                lx->filename = nf;
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
      // If it's not a processed directive, let it be a token (like #link)
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
      lexer_error(lx, (size_t)(start - lx->src), "'NULL' is not used in Nytrix",
                  "use '0' or 'none' instead");
      return NY_T_IDENT;
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
    if (len == 5 && memcmp(start, "undef", 5) == 0)
      return NY_T_UNDEF;
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
    if (peek(lx) == quote && peek_next(lx) == quote) {
      advance(lx);
      advance(lx);
      while (peek(lx) != '\0') {
        if (peek(lx) == quote && peek_next(lx) == quote &&
            lx->src[lx->pos + 2] == quote) {
          advance(lx);
          advance(lx);
          advance(lx);
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
      if (peek(lx) == quote)
        advance(lx);
    }
    return make_token(lx, NY_T_FSTRING, start);
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
    size_t len = (size_t)((const char *)p - (src + lx->pos));
    lx->pos = (size_t)((const char *)p - src);
    lx->col += (int)len;
    token_t tok = make_token(lx, NY_T_IDENT, start);
    tok.hash = ny_hash64(tok.lexeme, tok.len);
    tok.kind = identifier_type(lx, tok.lexeme, tok.len);
    return tok;
  }
  if (IS_DIGIT(c)) {
    if (c == '0' && (src[lx->pos] == 'x' || src[lx->pos] == 'X')) {
      lx->pos++;
      lx->col++;
      while (isxdigit(src[lx->pos])) {
        lx->pos++;
        lx->col++;
      }
      char s = src[lx->pos];
      if ((s == 'i' || s == 'I' || s == 'u' || s == 'U' || s == 'f' ||
           s == 'F') &&
          isdigit(src[lx->pos + 1])) {
        lx->pos++;
        lx->col++;
        while (isdigit(src[lx->pos])) {
          lx->pos++;
          lx->col++;
        }
      }
      token_t tok = make_token(lx, NY_T_NUMBER, start);
      tok.hash = 0;
      return tok;
    }
    const unsigned char *end = (const unsigned char *)(src + lx->len);
    while ((const unsigned char *)(src + lx->pos) + 8 <= end &&
           ny_is_digit8((const unsigned char *)(src + lx->pos))) {
      lx->pos += 8;
      lx->col += 8;
    }
    while (IS_DIGIT(src[lx->pos])) {
      lx->pos++;
      lx->col++;
    }
    if (src[lx->pos] == '.' && IS_DIGIT(src[lx->pos + 1])) {
      lx->pos++;
      lx->col++;
      while ((const unsigned char *)(src + lx->pos) + 8 <= end &&
             ny_is_digit8((const unsigned char *)(src + lx->pos))) {
        lx->pos += 8;
        lx->col += 8;
      }
      while (IS_DIGIT(src[lx->pos])) {
        lx->pos++;
        lx->col++;
      }
    }
    if (src[lx->pos] == 'e' || src[lx->pos] == 'E') {
      size_t save_pos = lx->pos;
      int save_line = lx->line;
      int save_col = lx->col;
      lx->pos++;
      lx->col++;
      if (src[lx->pos] == '+' || src[lx->pos] == '-') {
        lx->pos++;
        lx->col++;
      }
      if (IS_DIGIT(src[lx->pos])) {
        while ((const unsigned char *)(src + lx->pos) + 8 <= end &&
               ny_is_digit8((const unsigned char *)(src + lx->pos))) {
          lx->pos += 8;
          lx->col += 8;
        }
        while (IS_DIGIT(src[lx->pos])) {
          lx->pos++;
          lx->col++;
        }
      } else {
        lx->pos = save_pos;
        lx->line = save_line;
        lx->col = save_col;
      }
    }
    char s = src[lx->pos];
    if ((s == 'i' || s == 'I' || s == 'u' || s == 'U' || s == 'f' ||
         s == 'F') &&
        IS_DIGIT(src[lx->pos + 1])) {
      lx->pos++;
      lx->col++;
      while ((const unsigned char *)(src + lx->pos) + 8 <= end &&
             ny_is_digit8((const unsigned char *)(src + lx->pos))) {
        lx->pos += 8;
        lx->col += 8;
      }
      while (IS_DIGIT(src[lx->pos])) {
        lx->pos++;
        lx->col++;
      }
    }
    token_t tok = make_token(lx, NY_T_NUMBER, start);
    tok.hash = 0;
    return tok;
  }
  if (c == '"' || c == '\'') {
    char quote = c;
    if (peek(lx) == quote && peek_next(lx) == quote) {
      advance(lx);
      advance(lx);
      while (!(peek(lx) == quote && peek_next(lx) == quote &&
               lx->src[lx->pos + 2] == quote) &&
             peek(lx) != '\0') {
        advance(lx);
      }
      if (peek(lx) == quote) {
        advance(lx);
        advance(lx);
        advance(lx);
      }
    } else {
      while (peek(lx) != quote && peek(lx) != '\0') {
        if (peek(lx) == '\\' && peek_next(lx) != '\0')
          advance(lx);
        advance(lx);
      }
      if (peek(lx) == quote)
        advance(lx);
    }
    return make_token(lx, NY_T_STRING, start);
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
      lx->pos--;
      lx->col--;
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
    if (match(lx, '='))
      return make_token(lx, NY_T_PLUS_EQ, start);
    if (match(lx, '+'))
      return make_token(lx, NY_T_PLUS_PLUS, start);
    return make_token(lx, NY_T_PLUS, start);
  case '*':
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
        advance(lx); // *
        advance(lx); // /
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
                  "use '->' for match cases");
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
    return make_token(lx, NY_T_BITXOR, start);
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
  size_t out_len = t.len - head - tail;
  char *out = malloc(out_len + 1);
  if (!out)
    return NULL;
  memcpy(out, t.lexeme + head, out_len);
  out[out_len] = '\0';
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
  if (t.kind != NY_T_IDENT)
    return NULL;
  size_t cap = 64, len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  memcpy(buf, t.lexeme, t.len);
  len += t.len;
  for (;;) {
    token_t tok = lexer_next(lx);
    if (tok.kind == NY_T_DOT) {
      token_t id = lexer_next(lx);
      if (id.kind != NY_T_IDENT) {
        free(buf);
        return NULL;
      }
      if (len + 1 + id.len + 1 > cap) {
        cap = (len + 1 + id.len + 1) * 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          free(buf);
          return NULL;
        }
        buf = nb;
      }
      buf[len++] = '.';
      memcpy(buf + len, id.lexeme, id.len);
      len += id.len;
    } else {
      if (out_last_tok)
        *out_last_tok = tok;
      break;
    }
  }
  buf[len] = '\0';
  return buf;
}
