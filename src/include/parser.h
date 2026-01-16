#ifndef NT_PARSER_H
#define NT_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct nt_parser {
	nt_lexer lex;
	nt_token cur;
	nt_token prev;
	nt_arena *arena; // Pointer to arena
	const char *src;
	const char *filename;
	char *current_module;
	int error_count;
	int error_limit;
	bool had_error;
	int last_error_line;
	int last_error_col;
	char last_error_msg[256];
	const char *error_ctx;
	int block_depth;
} nt_parser;

void nt_parser_init(nt_parser *p, const char *src, const char *filename);
void nt_parser_init_with_arena(nt_parser *p, const char *src, const char *filename, nt_arena *arena);
nt_program nt_parse_program(nt_parser *p);

#endif
