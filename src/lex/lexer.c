#include "lex/lexer.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

void lexer_init(lexer_t *lx, const char *src, const char *filename) {
  lx->src = src;
  lx->filename = filename;
  lx->pos = 0;
  lx->line = 1;
  lx->col = 1;
  lx->split_pos = 0;
  lx->split_filename = NULL;
}

static char peek(lexer_t *lx) { return lx->src[lx->pos]; }

static char advance(lexer_t *lx) {
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
    lx->col = 1;
  } else {
    lx->col++;
  }
  return c;
}

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
  // For simplicity, line/col are at the END of token_t or current.
  // Ideally we should track start_line/col. But parsing error reporting usually
  // handles "current token_t" pointing to valid location.
  tok.line = lx->line;
  tok.col = lx->col - (int)tok.len; // Approximate start col
  tok.filename = lx->filename;
  return tok;
}

static void lexer_error(lexer_t *lx, size_t start, const char *msg,
                        const char *hint) {
  fprintf(stderr, "%s:%d:%d: \033[31merror:\033[0m %s\n",
          lx->filename ? lx->filename : "<input>", lx->line,
          lx->col - (int)(lx->pos - start), msg);
  if (hint) {
    fprintf(stderr, "%s:%d:%d: \033[33mnote:\033[0m %s\n",
            lx->filename ? lx->filename : "<input>", lx->line,
            lx->col - (int)(lx->pos - start), hint);
  }
}

static void skip_whitespace(lexer_t *lx) {
  for (;;) {
    char c = peek(lx);
    if (isspace(c)) {
      advance(lx);
    } else if (c == ';') {
      while (peek(lx) != '\n' && peek(lx) != '\0')
        advance(lx);
    } else {
      break;
    }
  }
}

static token_kind identifier_type(const char *start, size_t len) {
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
        if (len == 3 && start[2] == 'd')
          return NY_T_AND;
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
    break;
  case 'n':
    if (len == 3 && memcmp(start, "nil", 3) == 0)
      return NY_T_NIL;
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
  case 'm':
    if (len == 5 && memcmp(start, "match", 5) == 0)
      return NY_T_MATCH;
    if (len == 3 && memcmp(start, "mut", 3) == 0)
      return NY_T_MUT;
    if (len == 6 && memcmp(start, "module", 6) == 0)
      return NY_T_MODULE;
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
  skip_whitespace(lx);
  size_t start = lx->pos;
  if (lx->src[lx->pos] == '\0') {
    NY_LOG_DEBUG("Lexer reached EOF at %d:%d\n", lx->line, lx->col);
    token_t tok;
    tok.kind = NY_T_EOF;
    tok.lexeme = lx->src + start;
    tok.len = 0;
    tok.line = lx->line;
    tok.col = lx->col;
    tok.filename = lx->filename;
    return tok;
  }
  char c = advance(lx);
  // FString check
  if (c == 'f' && (peek(lx) == '"' || peek(lx) == '\'')) {
    char quote = peek(lx);
    advance(lx); // consume quote
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
  if (isalpha(c) || c == '_') {
    for (;;) {
      char p = peek(lx);
      if (isalnum(p) || p == '_' || p == '?' ||
          (p == '!' && peek_next(lx) != '=')) {
        advance(lx);
      } else if (p == '-' && isalpha(peek_next(lx))) {
        advance(lx);
      } else {
        break;
      }
    }
    token_t tok = make_token(lx, NY_T_IDENT, start);
    tok.kind = identifier_type(tok.lexeme, tok.len);
    NY_LOG_DEBUG("Lexer: identifier '%.*s' resolved to kind %d\n", (int)tok.len,
                 tok.lexeme, tok.kind);
    return tok;
  }
  if (isdigit(c)) {
    if (c == '0' && (peek(lx) == 'x' || peek(lx) == 'X')) {
      advance(lx); // consume 'x'
      while (isxdigit(peek(lx)))
        advance(lx);
      char s = peek(lx);
      if ((s == 'i' || s == 'I' || s == 'u' || s == 'U' || s == 'f' ||
           s == 'F') &&
          isdigit(peek_next(lx))) {
        advance(lx);
        while (isdigit(peek(lx)))
          advance(lx);
      }
      return make_token(lx, NY_T_NUMBER, start);
    }
    while (isdigit(peek(lx)))
      advance(lx);
    if (peek(lx) == '.' && isdigit(peek_next(lx))) {
      advance(lx);
      while (isdigit(peek(lx)))
        advance(lx);
    }
    if (peek(lx) == 'e' || peek(lx) == 'E') {
      size_t save_pos = lx->pos;
      int save_line = lx->line;
      int save_col = lx->col;
      advance(lx);
      if (peek(lx) == '+' || peek(lx) == '-')
        advance(lx);
      if (isdigit(peek(lx))) {
        while (isdigit(peek(lx)))
          advance(lx);
      } else {
        lx->pos = save_pos;
        lx->line = save_line;
        lx->col = save_col;
      }
    }
    char s = peek(lx);
    if ((s == 'i' || s == 'I' || s == 'u' || s == 'U' || s == 'f' ||
         s == 'F') &&
        isdigit(peek_next(lx))) {
      advance(lx);
      while (isdigit(peek(lx)))
        advance(lx);
    }
    return make_token(lx, NY_T_NUMBER, start);
  }
  if (c == '"' || c == '\'') {
    char quote = c;
    if (peek(lx) == quote && peek_next(lx) == quote) {
      // Triple quote?
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
    }
    return make_token(lx, NY_T_DOT, start);
  case '-':
    if (match(lx, '>'))
      return make_token(lx, NY_T_ARROW, start);
    if (match(lx, '='))
      return make_token(lx, NY_T_MINUS_EQ, start);
    if (match(lx, '-')) {
      lexer_error(lx, start,
                  "decrement operator '--' is not supported in Nytrix",
                  "use '-= 1' instead");
      return lexer_next(lx);
    }
    return make_token(lx, NY_T_MINUS, start);
  case '+':
    if (match(lx, '='))
      return make_token(lx, NY_T_PLUS_EQ, start);
    if (match(lx, '+')) {
      lexer_error(lx, start,
                  "increment operator '++' is not supported in Nytrix",
                  "use '+= 1' instead");
      return lexer_next(lx);
    }
    return make_token(lx, NY_T_PLUS, start);
  case '*':
    if (match(lx, '='))
      return make_token(lx, NY_T_STAR_EQ, start);
    return make_token(lx, NY_T_STAR, start);
  case '/':
    if (match(lx, '='))
      return make_token(lx, NY_T_SLASH_EQ, start);
    if (match(lx, '/')) {
      lexer_error(lx, start, "comments in Nytrix start with ';'",
                  "use ';' instead of '//'");
      // Skip the rest of the line to avoid cascading errors
      while (peek(lx) != '\n' && peek(lx) != '\0')
        advance(lx);
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
    if (match(lx, '='))
      return make_token(lx, NY_T_NEQ, start);
    return make_token(lx, NY_T_NOT, start);
  case '=':
    if (match(lx, '='))
      return make_token(lx, NY_T_EQ, start);
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
    return make_token(lx, NY_T_BITOR, start);
  case '^':
    return make_token(lx, NY_T_BITXOR, start);
  case '~':
    return make_token(lx, NY_T_BITNOT, start);
  case ':':
    return make_token(lx, NY_T_COLON, start);
  case '?':
    return make_token(lx, NY_T_QUESTION, start);
  }
  // Unknown token_t
  char emsg[128];
  snprintf(emsg, sizeof(emsg), "unrecognised character '%c' (ascii %d)", c,
           (int)c);
  lexer_error(lx, start, emsg,
              "check for accidental non-ascii characters or typos");
  return lexer_next(lx);
}
