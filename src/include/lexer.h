#ifndef NT_LEXER_H
#define NT_LEXER_H

#include "common.h"

typedef enum nt_token_kind {
	NT_T_EOF = 0,
	NT_T_IDENT,
	NT_T_NUMBER,
	NT_T_STRING,
	NT_T_FSTRING,
	NT_T_FN,
	NT_T_RETURN,
	NT_T_IF,
	NT_T_ELSE,
	NT_T_WHILE,
	NT_T_FOR,
	NT_T_IN,
	NT_T_TRUE,
	NT_T_FALSE,
	NT_T_TRY,
	NT_T_CATCH,
	NT_T_USE,
	NT_T_GOTO,
	NT_T_LAMBDA,
	NT_T_DEFER,
	NT_T_BREAK,
	NT_T_CONTINUE,
	NT_T_ELIF,
	NT_T_ASM,
	NT_T_AS,
	NT_T_COMPTIME,
	NT_T_LAYOUT,
	NT_T_MATCH,
	NT_T_EMBED,
	NT_T_DEF,
	NT_T_NIL,
	NT_T_UNDEF,
	NT_T_MODULE,
	NT_T_PLUS,
	NT_T_MINUS,
	NT_T_STAR,
	NT_T_SLASH,
	NT_T_PERCENT,
	NT_T_EQ,
	NT_T_NEQ,
	NT_T_LT,
	NT_T_GT,
	NT_T_LE,
	NT_T_GE,
	NT_T_AND,
	NT_T_OR,
	NT_T_NOT,
	NT_T_ASSIGN,
	NT_T_PLUS_EQ,
	NT_T_MINUS_EQ,
	NT_T_STAR_EQ,
	NT_T_SLASH_EQ,
	NT_T_ARROW,
	NT_T_BITOR,
	NT_T_BITAND,
	NT_T_BITXOR,
	NT_T_LSHIFT,
	NT_T_RSHIFT,
	NT_T_BITNOT,
	NT_T_LPAREN,
	NT_T_RPAREN,
	NT_T_LBRACE,
	NT_T_RBRACE,
	NT_T_LBRACK,
	NT_T_RBRACK,
	NT_T_COMMA,
	NT_T_COLON,
	NT_T_SEMI,
	NT_T_DOT,
	NT_T_ELLIPSIS,
	NT_T_QUESTION,
} nt_token_kind;

typedef struct nt_token {
	nt_token_kind kind;
	const char *lexeme;
	size_t len;
	int line;
	int col;
	const char *filename;
} nt_token;

typedef struct nt_lexer {
	const char *src;
	const char *filename;
	size_t pos;
	int line;
	int col;
} nt_lexer;

void nt_lexer_init(nt_lexer *lx, const char *src, const char *filename);
nt_token nt_lex_next(nt_lexer *lx);

#endif
