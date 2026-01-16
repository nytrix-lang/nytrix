#include "lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdbool.h>

void nt_lexer_init(nt_lexer *lx, const char *src, const char *filename) {
	lx->src = src;
	lx->filename = filename;
	lx->pos = 0;
	lx->line = 1;
	lx->col = 1;
}

static char peek(nt_lexer *lx) {
	return lx->src[lx->pos];
}

static char advance(nt_lexer *lx) {
	if (lx->src[lx->pos] == '\0') return '\0';
	char c = lx->src[lx->pos++];
	if (c == '\n') {
		lx->line++;
		lx->col = 1;
	} else {
		lx->col++;
	}
	return c;
}

static char peek_next(nt_lexer *lx) {
	if (lx->src[lx->pos] == '\0') return '\0';
	return lx->src[lx->pos + 1];
}

static bool match(nt_lexer *lx, char expected) {
	if (peek(lx) == expected) {
		advance(lx);
		return true;
	}
	return false;
}

static nt_token make_token(nt_lexer *lx, nt_token_kind kind, size_t start) {
	nt_token tok;
	tok.kind = kind;
	tok.lexeme = lx->src + start;
	tok.len = lx->pos - start;
	// For simplicity, line/col are at the END of token or current.
	// Ideally we should track start_line/col. But parsing error reporting usually handles "current token" pointing to valid location.
	tok.line = lx->line;
	tok.col = lx->col - (int)tok.len; // Approximate start col
	tok.filename = lx->filename;
	return tok;
}

static void skip_whitespace(nt_lexer *lx) {
	for (;;) {
		char c = peek(lx);
		if (isspace(c)) {
			advance(lx);
		} else if (c == ';' || c == '#') {
			 while (peek(lx) != '\n' && peek(lx) != '\0') advance(lx);
		} else {
			break;
		}
	}
}

static nt_token_kind identifier_type(const char *start, size_t len) {
	switch (start[0]) {
		case 'a':
			if (len > 1) {
				switch (start[1]) {
					case 's':
						if (len == 2) return NT_T_AS;
						if (len == 3 && start[2] == 'm') return NT_T_ASM;
						break;
					case 'n': if (len == 3 && start[2] == 'd') return NT_T_AND; break;
				}
			}
			break;
		case 'b':
			if (len == 5 && memcmp(start, "break", 5) == 0) return NT_T_BREAK;
			break;
		case 'c':
			if (len == 4 && memcmp(start, "case", 4) == 0) return NT_T_MATCH;
			if (len == 5 && memcmp(start, "catch", 5) == 0) return NT_T_CATCH;
			if (len == 8 && memcmp(start, "continue", 8) == 0) return NT_T_CONTINUE;
			if (len == 8 && memcmp(start, "comptime", 8) == 0) return NT_T_COMPTIME;
			break;
		case 'd':
			if (len == 5 && memcmp(start, "defer", 5) == 0) return NT_T_DEFER;
			if (len == 3 && memcmp(start, "def", 3) == 0) return NT_T_DEF;
			break;
		case 'e':
			if (len == 4 && memcmp(start, "else", 4) == 0) return NT_T_ELSE;
			if (len == 4 && memcmp(start, "elif", 4) == 0) return NT_T_ELIF;
			if (len == 5 && memcmp(start, "embed", 5) == 0) return NT_T_EMBED;
			break;
		case 'f':
			if (len > 1) {
				switch (start[1]) {
					case 'a': if (len == 5 && memcmp(start, "false", 5) == 0) return NT_T_FALSE; break;
					case 'n': if (len == 2) return NT_T_FN; break;
					case 'o': if (len == 3 && start[2] == 'r') return NT_T_FOR; break;
				}
			}
			break;
		case 'g':
			if (len == 4 && memcmp(start, "goto", 4) == 0) return NT_T_GOTO;
			break;
		case 'i':
			if (len == 2) {
				if (start[1] == 'f') return NT_T_IF;
				if (start[1] == 'n') return NT_T_IN;
			}
			break;
		case 'n':
			if (len == 3 && memcmp(start, "nil", 3) == 0) return NT_T_NIL;
			break;
		case 'l':
			if (len == 6 && memcmp(start, "lambda", 6) == 0) return NT_T_LAMBDA;
			if (len == 6 && memcmp(start, "layout", 6) == 0) return NT_T_LAYOUT;
			break;
		case 'm':
			if (len == 6 && memcmp(start, "module", 6) == 0) return NT_T_MODULE;
			break;
		case 'r':
			if (len == 6 && memcmp(start, "return", 6) == 0) return NT_T_RETURN;
			break;
		case 't':
			if (len == 4 && memcmp(start, "true", 4) == 0) return NT_T_TRUE;
			if (len == 3 && memcmp(start, "try", 3) == 0) return NT_T_TRY;
			break;
		case 'u':
			if (len == 5 && memcmp(start, "undef", 5) == 0) return NT_T_UNDEF;
			if (len == 3 && memcmp(start, "use", 3) == 0) return NT_T_USE;
			break;
		case 'w':
			if (len == 5 && memcmp(start, "while", 5) == 0) return NT_T_WHILE;
			break;
	}
	return NT_T_IDENT;
}

nt_token nt_lex_next(nt_lexer *lx) {
	skip_whitespace(lx);
	size_t start = lx->pos;
	char c = advance(lx);
	if (c == '\0') {
		nt_token tok;
		tok.kind = NT_T_EOF; tok.lexeme = lx->src + start; tok.len = 0; tok.line = lx->line; tok.col = lx->col;
		return tok;
	}
	// FString check
	if (c == 'f' && (peek(lx) == '"' || peek(lx) == '\'')) {
		char quote = peek(lx);
		advance(lx); // consume quote
		if (peek(lx) == quote && peek_next(lx) == quote) {
			 advance(lx); advance(lx);
			 while (peek(lx) != '\0') {
				 if (peek(lx) == quote && peek_next(lx) == quote && lx->src[lx->pos+2] == quote) {
					 advance(lx); advance(lx); advance(lx);
					 break;
				 }
				 advance(lx);
			 }
		} else {
			while (peek(lx) != quote && peek(lx) != '\0') {
				if (peek(lx) == '\\' && peek_next(lx) != '\0') advance(lx);
				advance(lx);
			}
			if (peek(lx) == quote) advance(lx);
		}
		return make_token(lx, NT_T_FSTRING, start);
	}
	if (isalpha(c) || c == '_') {
		for (;;) {
			char p = peek(lx);
			if (isalnum(p) || p == '_' || p == '?' || (p == '!' && peek_next(lx) != '=')) {
				advance(lx);
			} else if (p == '-' && isalpha(peek_next(lx))) {
				advance(lx);
			} else {
				break;
			}
		}
		nt_token tok = make_token(lx, NT_T_IDENT, start);
		tok.kind = identifier_type(tok.lexeme, tok.len);
		return tok;
	}
	if (isdigit(c)) {
		if (c == '0' && (peek(lx) == 'x' || peek(lx) == 'X')) {
			advance(lx); // consume 'x'
			while (isxdigit(peek(lx))) advance(lx);
			return make_token(lx, NT_T_NUMBER, start);
		}
		while (isdigit(peek(lx))) advance(lx);
		if (peek(lx) == '.' && isdigit(peek_next(lx))) {
			advance(lx);
			while (isdigit(peek(lx))) advance(lx);
		}
		return make_token(lx, NT_T_NUMBER, start);
	}
	if (c == '"' || c == '\'') {
		char quote = c;
		if (peek(lx) == quote && peek_next(lx) == quote) {
			// Triple quote?
			advance(lx); advance(lx);
			while (!(peek(lx) == quote && peek_next(lx) == quote && lx->src[lx->pos+2] == quote) && peek(lx) != '\0') {
				advance(lx);
			}
			if (peek(lx) == quote) { advance(lx); advance(lx); advance(lx); }
		} else {
			while (peek(lx) != quote && peek(lx) != '\0') {
				if (peek(lx) == '\\' && peek_next(lx) != '\0') advance(lx);
				advance(lx);
			}
			if (peek(lx) == quote) advance(lx);
		}
		return make_token(lx, NT_T_STRING, start);
	}
	switch (c) {
		case '(': return make_token(lx, NT_T_LPAREN, start);
		case ')': return make_token(lx, NT_T_RPAREN, start);
		case '{': return make_token(lx, NT_T_LBRACE, start);
		case '}': return make_token(lx, NT_T_RBRACE, start);
		case '[': return make_token(lx, NT_T_LBRACK, start);
		case ']': return make_token(lx, NT_T_RBRACK, start);
		case ',': return make_token(lx, NT_T_COMMA, start);
		case '.':
			if (match(lx, '.')) {
				if (match(lx, '.')) return make_token(lx, NT_T_ELLIPSIS, start);
			}
			return make_token(lx, NT_T_DOT, start);
		case '-':
			if (match(lx, '>')) return make_token(lx, NT_T_ARROW, start);
			if (match(lx, '=')) return make_token(lx, NT_T_MINUS_EQ, start);
			return make_token(lx, NT_T_MINUS, start);
		case '+':
			if (match(lx, '=')) return make_token(lx, NT_T_PLUS_EQ, start);
			return make_token(lx, NT_T_PLUS, start);
		case '*':
			if (match(lx, '=')) return make_token(lx, NT_T_STAR_EQ, start);
			return make_token(lx, NT_T_STAR, start);
		case '/':
			if (match(lx, '=')) return make_token(lx, NT_T_SLASH_EQ, start);
			return make_token(lx, NT_T_SLASH, start);
		case '%': return make_token(lx, NT_T_PERCENT, start);
		case '!':
			if (match(lx, '=')) return make_token(lx, NT_T_NEQ, start);
			return make_token(lx, NT_T_NOT, start);
		case '=':
			if (match(lx, '=')) return make_token(lx, NT_T_EQ, start);
			return make_token(lx, NT_T_ASSIGN, start);
		case '<':
			if (match(lx, '=')) return make_token(lx, NT_T_LE, start);
			if (match(lx, '<')) return make_token(lx, NT_T_LSHIFT, start);
			return make_token(lx, NT_T_LT, start);
		case '>':
			if (match(lx, '=')) return make_token(lx, NT_T_GE, start);
			if (match(lx, '>')) return make_token(lx, NT_T_RSHIFT, start);
			return make_token(lx, NT_T_GT, start);
		case '&':
			if (match(lx, '&')) return make_token(lx, NT_T_AND, start);
			return make_token(lx, NT_T_BITAND, start);
		case '|':
			if (match(lx, '|')) return make_token(lx, NT_T_OR, start);
			return make_token(lx, NT_T_BITOR, start);
		case '^': return make_token(lx, NT_T_BITXOR, start);
		case '~': return make_token(lx, NT_T_BITNOT, start);
		case ':': return make_token(lx, NT_T_COLON, start);
		case '?': return make_token(lx, NT_T_QUESTION, start);
	}
	// Unknown token
	nt_token err_tok = make_token(lx, NT_T_EOF, start); // Placeholder
	// fprintf(stderr, "Lexer: unrecognised char %c\n", c);
	return err_tok;
}
