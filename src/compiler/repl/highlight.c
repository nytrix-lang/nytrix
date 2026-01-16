#include "parser.h"
#include "lexer.h"
#include <readline/readline.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Enhanced syntax highlighting for Nytrix REPL
// This file provides real-time syntax highlighting using readline's redisplay hook

// Color definitions
static const char *CLR_RESET    = "\033[0m";
static const char *CLR_KEYWORD  = "\033[1;36m";    // Bright cyan for keywords
static const char *CLR_BUILTIN  = "\033[1;35m";    // Bright magenta for builtins
static const char *CLR_STRING   = "\033[33m";      // Yellow for strings
static const char *CLR_NUMBER   = "\033[32m";      // Green for numbers
static const char *CLR_OPERATOR = "\033[35m";      // Magenta for operators
static const char *CLR_FUNCTION = "\033[1;34m";    // Bright blue for functions
static const char *CLR_PAREN    = "\033[37m";      // White for parens/brackets

// Check if identifier looks like a function call
static int is_function_call(const char *line, size_t pos) {
	// Look ahead for '('
	while (line[pos] && (line[pos] == ' ' || line[pos] == '\t')) pos++;
	return line[pos] == '(';
}

// Enhanced syntax highlighter using lexer
void repl_highlight_line(const char *line) {
	if (!line || !*line) {
		fputs(line ? line : "", stdout);
		return;
	}
	// Handle REPL commands specially
	if (line[0] == ':') {
		printf("%s%s%s", CLR_BUILTIN, line, CLR_RESET);
		return;
	}
	nt_lexer lx;
	nt_lexer_init(&lx, line, "<repl>");
	size_t pos = 0;
	nt_token tok;
	while (1) {
		tok = nt_lex_next(&lx);
		// Print any whitespace/text before this token
		while (pos < (size_t)(tok.lexeme - line)) {
			fputc(line[pos++], stdout);
		}
		if (tok.kind == NT_T_EOF) break;
		const char *color = NULL;
		// Determine color based on token type
		switch (tok.kind) {
			case NT_T_STRING:
				color = CLR_STRING;
				break;
			case NT_T_NUMBER:
				color = CLR_NUMBER;
				break;
			// Keywords
			case NT_T_FN: case NT_T_IF: case NT_T_ELSE: case NT_T_ELIF:
			case NT_T_WHILE: case NT_T_FOR: case NT_T_IN: case NT_T_RETURN:
			case NT_T_USE: case NT_T_TRY: case NT_T_CATCH: case NT_T_BREAK:
			case NT_T_CONTINUE: case NT_T_LAMBDA: case NT_T_DEFER: case NT_T_UNDEF: case NT_T_NIL:
			case NT_T_TRUE: case NT_T_FALSE: case NT_T_GOTO:
				color = CLR_KEYWORD;
				break;
			case NT_T_IDENT:
				// Check if it looks like a function call
				if (is_function_call(line, pos + tok.len)) {
					color = CLR_FUNCTION;
				}
				break;
			// Operators
			case NT_T_PLUS: case NT_T_MINUS: case NT_T_STAR: case NT_T_SLASH:
			case NT_T_PERCENT: case NT_T_BITOR: case NT_T_BITAND: case NT_T_BITXOR:
			case NT_T_BITNOT: case NT_T_LSHIFT: case NT_T_RSHIFT:
			case NT_T_EQ: case NT_T_NEQ: case NT_T_LT: case NT_T_LE:
			case NT_T_GT: case NT_T_GE: case NT_T_ASSIGN:
			case NT_T_AND: case NT_T_OR: case NT_T_NOT:
			case NT_T_ARROW:
				color = CLR_OPERATOR;
				break;
			// Brackets
			case NT_T_LPAREN: case NT_T_RPAREN:
			case NT_T_LBRACK: case NT_T_RBRACK:
			case NT_T_LBRACE: case NT_T_RBRACE:
				color = CLR_PAREN;
				break;
			default:
				break;
		}
		// Print the token with color
		if (color) {
			printf("%s", color);
		}
		fwrite(tok.lexeme, 1, tok.len, stdout);
		if (color) {
			printf("%s", CLR_RESET);
		}
		pos += tok.len;
	}
	// Print any remaining text
	while (line[pos]) {
		fputc(line[pos++], stdout);
	}
}

// Initialize syntax highlighting
void repl_init_highlighting(void) {
	// Hook into readline's redisplay function
	// Note: Setting rl_redisplay_function can cause issues with output
	// For now, we use a simpler approach that highlights on display
	// rl_redisplay_function = repl_redisplay_with_highlight;
}

// Alternative: Highlight on newline/display (safer approach)
void repl_display_highlighted(const char *line) {
	if (!line) return;
	repl_highlight_line(line);
	fputc('\n', stdout);
	fflush(stdout);
}
