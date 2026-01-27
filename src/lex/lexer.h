#ifndef NY_LEXER_H
#define NY_LEXER_H

#include "base/common.h"

typedef enum token_kind {
  NY_T_EOF = 0,
  NY_T_IDENT,
  NY_T_NUMBER,
  NY_T_STRING,
  NY_T_FSTRING,
  NY_T_FN,
  NY_T_RETURN,
  NY_T_IF,
  NY_T_ELSE,
  NY_T_WHILE,
  NY_T_FOR,
  NY_T_IN,
  NY_T_TRUE,
  NY_T_FALSE,
  NY_T_TRY,
  NY_T_CATCH,
  NY_T_USE,
  NY_T_GOTO,
  NY_T_LAMBDA,
  NY_T_DEFER,
  NY_T_BREAK,
  NY_T_CONTINUE,
  NY_T_ELIF,
  NY_T_ASM,
  NY_T_AS,
  NY_T_COMPTIME,
  NY_T_LAYOUT,
  NY_T_MATCH,
  NY_T_EMBED,
  NY_T_EXTERN,
  NY_T_DEF,
  NY_T_MUT,
  NY_T_NIL,
  NY_T_UNDEF,
  NY_T_MODULE,
  NY_T_PLUS,
  NY_T_MINUS,
  NY_T_STAR,
  NY_T_SLASH,
  NY_T_PERCENT,
  NY_T_EQ,
  NY_T_NEQ,
  NY_T_LT,
  NY_T_GT,
  NY_T_LE,
  NY_T_GE,
  NY_T_AND,
  NY_T_OR,
  NY_T_NOT,
  NY_T_ASSIGN,
  NY_T_PLUS_EQ,
  NY_T_MINUS_EQ,
  NY_T_STAR_EQ,
  NY_T_SLASH_EQ,
  NY_T_PERCENT_EQ,
  NY_T_ARROW,
  NY_T_BITOR,
  NY_T_BITAND,
  NY_T_BITXOR,
  NY_T_LSHIFT,
  NY_T_RSHIFT,
  NY_T_BITNOT,
  NY_T_LPAREN,
  NY_T_RPAREN,
  NY_T_LBRACE,
  NY_T_RBRACE,
  NY_T_LBRACK,
  NY_T_RBRACK,
  NY_T_COMMA,
  NY_T_COLON,
  NY_T_SEMI,
  NY_T_DOT,
  NY_T_ELLIPSIS,
  NY_T_QUESTION,
  NY_T_ERROR,
} token_kind;

typedef struct token_t {
  token_kind kind;
  const char *lexeme;
  size_t len;
  int line;
  int col;
  const char *filename;
} token_t;

typedef struct lexer_t {
  const char *src;
  const char *filename;
  size_t pos;
  int line;
  int col;
  size_t split_pos;
  const char *split_filename;
} lexer_t;

void lexer_init(lexer_t *lx, const char *src, const char *filename);
token_t lexer_next(lexer_t *lx);

#endif
