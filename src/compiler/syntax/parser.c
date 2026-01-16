#include "parser.h"

#include <stdarg.h>
#include <stdio.h>

static void advance(nt_parser *p) { p->prev = p->cur; p->cur = nt_lex_next(&p->lex); }

static bool match(nt_parser *p, nt_token_kind kind) {
	if (p->cur.kind == kind) {
		advance(p);
		return true;
	}
	return false;
}

static nt_expr *parse_expr(nt_parser *p, int prec);
static nt_stmt *parse_stmt(nt_parser *p);
static nt_stmt *parse_block(nt_parser *p);
static nt_stmt *parse_match(nt_parser *p);
static nt_stmt *parse_stmt_or_block(nt_parser *p);

static const char *token_name(nt_token_kind k) {
	switch (k) {
	case NT_T_EOF: return "EOF";
	case NT_T_IDENT: return "identifier";
	case NT_T_NUMBER: return "number";
	case NT_T_STRING: return "string";
	case NT_T_FN: return "fn";
	case NT_T_RETURN: return "return";
	case NT_T_IF: return "if";
	case NT_T_ELSE: return "else";
	case NT_T_WHILE: return "while";
	case NT_T_FOR: return "for";
	case NT_T_IN: return "in";
	case NT_T_TRUE: return "true";
	case NT_T_FALSE: return "false";
	case NT_T_TRY: return "try";
	case NT_T_CATCH: return "catch";
	case NT_T_USE: return "use";
	case NT_T_GOTO: return "goto";
	case NT_T_LAMBDA: return "lambda";
	case NT_T_DEFER: return "defer";
	case NT_T_DEF: return "def";
	case NT_T_NIL: return "nil";
	case NT_T_UNDEF: return "undef";
	case NT_T_BREAK: return "break";
	case NT_T_CONTINUE: return "continue";
	case NT_T_ELIF: return "elif";
	case NT_T_ASM: return "asm";
	case NT_T_AS: return "as";
	case NT_T_PLUS: return "+";
	case NT_T_MINUS: return "-";
	case NT_T_STAR: return "*";
	case NT_T_SLASH: return "/";
	case NT_T_PERCENT: return "%";
	case NT_T_EQ: return "==";
	case NT_T_NEQ: return "!=";
	case NT_T_LT: return "<";
	case NT_T_GT: return ">";
	case NT_T_LE: return "<=";
	case NT_T_GE: return ">=";
	case NT_T_AND: return "&&";
	case NT_T_OR: return "||";
	case NT_T_NOT: return "!";
	case NT_T_ASSIGN: return "=";
	case NT_T_PLUS_EQ: return "+=";
	case NT_T_MINUS_EQ: return "-=";
	case NT_T_STAR_EQ: return "*=";
	case NT_T_SLASH_EQ: return "/=";
	case NT_T_ARROW: return "->";
	case NT_T_LPAREN: return "(";
	case NT_T_RPAREN: return ")";
	case NT_T_LBRACE: return "{";
	case NT_T_RBRACE: return "}";
	case NT_T_LBRACK: return "[";
	case NT_T_RBRACK: return "]";
	case NT_T_COMMA: return ",";
	case NT_T_COLON: return ":";
	case NT_T_SEMI: return ";";
	case NT_T_DOT: return ".";
	case NT_T_BITOR: return "|";
	case NT_T_BITAND: return "&";
	case NT_T_BITXOR: return "^";
	case NT_T_LSHIFT: return "<<";
	case NT_T_RSHIFT: return ">>";
	case NT_T_BITNOT: return "~";
	default: return "?";
	}
}

static void print_error_line(nt_parser *p, int line, int col, const char *msg, const char *got, const char *hint) {
	p->had_error = true;
	p->error_count++;
	p->last_error_line = line;
	p->last_error_col = col;
	snprintf(p->last_error_msg, sizeof(p->last_error_msg), "%s", msg);
	// locate line text
	const char *s = p->src;
	int cur_line = 1;
	const char *start = s;
	while (*s && cur_line < line) {
		if (*s == '\n') {
			cur_line++;
			start = s + 1;
		}
		s++;
	}
	const char *end = start;
	while (*end && *end != '\n') end++;
	fprintf(stderr, "%serror:%s:%d:%d:%s %s%s%s (got %s)\n",
		nt_clr(NT_CLR_RED), p->filename ? p->filename : "<input>", line, col, nt_clr(NT_CLR_RESET),
		nt_clr(NT_CLR_BOLD), msg, nt_clr(NT_CLR_RESET), got);
	if (hint) fprintf(stderr, "  %snote:%s %s\n", nt_clr(NT_CLR_YELLOW), nt_clr(NT_CLR_RESET), hint);
	if (p->error_limit > 0 && p->error_count >= p->error_limit) {
		fprintf(stderr, "Too many errors, aborting.\n");
		exit(1);
	}
}

static const char *token_desc(nt_token tok, char *buf, size_t cap) {
	const char *kind = token_name(tok.kind);
	if (tok.kind == NT_T_IDENT || tok.kind == NT_T_NUMBER || tok.kind == NT_T_STRING) {
		size_t n = tok.len < 24 ? tok.len : 24;
		snprintf(buf, cap, "%s '%.*s'%s", kind, (int)n, tok.lexeme, tok.len > n ? "..." : "");
		return buf;
	}
	return kind;
}

static const char *expect_hint(nt_token_kind expected, nt_token got) {
	if (expected == NT_T_SEMI && got.kind == NT_T_RBRACE) return "did you forget a ';' before '}'?";
	if (expected == NT_T_RPAREN && got.kind == NT_T_RBRACE) return "did you forget a ')' before '}'?";
	if (expected == NT_T_RBRACE && got.kind == NT_T_EOF) return "missing '}' before end of file";
	if (expected == NT_T_RPAREN && got.kind == NT_T_EOF) return "missing ')' before end of file";
	if (expected == NT_T_RBRACK && got.kind == NT_T_EOF) return "missing ']' before end of file";
	if (expected == NT_T_COLON && got.kind == NT_T_IDENT) return "use ':' after 'case'/'default' or for slices";
	return NULL;
}

static const char *decode_string(nt_parser *p, nt_token tok, size_t *out_len) {
	const char *lex = tok.lexeme;
	size_t len = tok.len;
	bool triple = len >= 6 && lex[0] == lex[1] && lex[1] == lex[2];
	size_t head = triple ? 3 : 1;
	size_t tail = triple ? 3 : 1;
	const char *cur = lex + head;
	const char *end = lex + len - tail;
	// worst case same size; allocate
	char *out = nt_arena_alloc(p->arena, (end - cur) + 1);
	size_t oi = 0;
	while (cur < end) {
		if (*cur == '\\' && cur + 1 < end) {
			cur++;
			switch (*cur) {
			case 'n': out[oi++] = '\n'; break;
			case 't': out[oi++] = '\t'; break;
			case 'r': out[oi++] = '\r'; break;
			case '\\': out[oi++] = '\\'; break;
			case '\'': out[oi++] = '\''; break;
			case '"': out[oi++] = '"'; break;
			case 'x': {
				if (cur + 2 < end) {
					char hex[3] = {cur[1], cur[2], 0};
					out[oi++] = (char)strtol(hex, NULL, 16);
					cur += 2;
				} else {
					out[oi++] = 'x';
				}
				break;
			}
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7': {
				int oct = 0;
				int count = 0;
				while (count < 3 && cur < end && *cur >= '0' && *cur <= '7') {
					oct = oct * 8 + (*cur - '0');
					cur++;
					count++;
				}
				out[oi++] = (char)oct;
				cur--; // adjustment for the outer loop cur++
				break;
			}
			default: out[oi++] = *cur; break;
			}
			cur++;
			continue;
		}
		out[oi++] = *cur++;
	}
	out[oi] = '\0';
	if (out_len) *out_len = oi;
	return out;
}

static const char *decode_fstring_part(nt_parser *p, const char *s, size_t len, size_t *out_len) {
	const char *cur = s;
	const char *end = s + len;
	char *out = nt_arena_alloc(p->arena, len + 1);
	size_t oi = 0;
	while (cur < end) {
		if (*cur == '\\' && cur + 1 < end) {
			cur++;
			switch (*cur) {
			case 'n': out[oi++] = '\n'; break;
			case 't': out[oi++] = '\t'; break;
			case 'r': out[oi++] = '\r'; break;
			case '\\': out[oi++] = '\\'; break;
			case '\'': out[oi++] = '\''; break;
			case '"': out[oi++] = '"'; break;
			case 'x': {
				if (cur + 2 < end) {
					char hex[3] = {cur[1], cur[2], 0};
					out[oi++] = (char)strtol(hex, NULL, 16);
					cur += 2;
				} else {
					out[oi++] = 'x';
				}
				break;
			}
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7': {
				int oct = 0;
				int count = 0;
				while (count < 3 && cur < end && *cur >= '0' && *cur <= '7') {
					oct = oct * 8 + (*cur - '0');
					cur++;
					count++;
				}
				out[oi++] = (char)oct;
				cur--; // adjustment for the outer loop cur++
				break;
			}
			default: out[oi++] = *cur; break;
			}
			cur++;
			continue;
		}
		out[oi++] = *cur++;
	}
	out[oi] = '\0';
	if (out_len) *out_len = oi;
	return out;
}

static void parse_error(nt_parser *p, nt_token tok, const char *msg, const char *hint) {
	if (!hint && tok.kind == NT_T_EOF) hint = "check for missing ';' or unmatched brace";
	char buf[64];
	const char *got = token_desc(tok, buf, sizeof(buf));
	print_error_line(p, tok.line, tok.col, msg, got, hint);
}

static void parse_expect(nt_parser *p, nt_token_kind expected, nt_token got, const char *hint) {
	char msg[128];
	snprintf(msg, sizeof(msg), "expected %s", token_name(expected));
	if (!hint) hint = expect_hint(expected, got);
	char buf[64];
	const char *got_desc = token_desc(got, buf, sizeof(buf));
	print_error_line(p, got.line, got.col, msg, got_desc, hint);
}

static void synchronize(nt_parser *p) {
	while (p->cur.kind != NT_T_EOF) {
		if (p->cur.kind == NT_T_SEMI) {
			advance(p);
			return;
		}
		switch (p->cur.kind) {
			case NT_T_FN:
			case NT_T_IF:
			case NT_T_WHILE:
			case NT_T_FOR:
			case NT_T_RETURN:
			case NT_T_USE:
			case NT_T_COMMA:
				return;
			default:
				break;
		}
		advance(p);
	}
}

static void expect(nt_parser *p, nt_token_kind kind, const char *msg, const char *hint) {
	if (p->cur.kind == kind) {
		advance(p);
		return;
	}
	if (!msg) {
		parse_expect(p, kind, p->cur, hint);
	} else {
		parse_error(p, p->cur, msg, hint);
	}
}

static nt_token peek_token(nt_parser *p) {
	nt_lexer lx = p->lex;
	return nt_lex_next(&lx);
}

void nt_parser_init_with_arena(nt_parser *p, const char *src, const char *filename, nt_arena *arena_ptr) {
	memset(p, 0, sizeof(nt_parser));
	p->src = src;
	p->filename = filename ? filename : "<input>";
	nt_lexer_init(&p->lex, src, p->filename);
	p->arena = arena_ptr;
	p->had_error = false;
	p->error_count = 0;
	p->error_limit = 10;
	p->error_ctx = NULL;
	p->last_error_line = 0;
	p->last_error_col = 0;
	p->last_error_msg[0] = '\0';
	p->current_module = NULL;
	p->block_depth = 0;
	advance(p);
}

void nt_parser_init(nt_parser *p, const char *src, const char *filename) {
	nt_arena *arena = (nt_arena *)malloc(sizeof(nt_arena));
	if (!arena) {
		fprintf(stderr, "oom\n");
		exit(1);
	}
	memset(arena, 0, sizeof(nt_arena));
	nt_parser_init_with_arena(p, src, filename, arena);
}

// precedence climbing
static int precedence(nt_token_kind kind) {
	switch (kind) {
	case NT_T_OR: return 1;
	case NT_T_AND: return 2;
	case NT_T_EQ:
	case NT_T_NEQ: return 3;
	case NT_T_LT:
	case NT_T_GT:
	case NT_T_LE:
	case NT_T_GE: return 4;
	case NT_T_PLUS:
	case NT_T_MINUS: return 5;
	case NT_T_STAR:
	case NT_T_SLASH:
	case NT_T_PERCENT: return 6;
	case NT_T_BITOR:
	case NT_T_BITAND:
	case NT_T_BITXOR:
	case NT_T_LSHIFT:
	case NT_T_RSHIFT: return 7; // higher than add/sub, lower than unary? check standard C
	default: return 0;
	}
}

static nt_expr *parse_fstring(nt_parser *p, nt_token tok) {
	advance(p);
	nt_expr *e = nt_expr_new(p->arena, NT_E_FSTRING, tok);
	const char *s = tok.lexeme;
	size_t len = tok.len;
	// Skip 'f' prefix
	s++; len--;
	char quote = *s;
	bool triple = (len >= 6 && s[1] == quote && s[2] == quote);
	s += triple ? 3 : 1;
	len -= triple ? 6 : 2;
	size_t i = 0;
	while (i < len) {
		if (s[i] == '{') {
			i++;
			size_t start = i;
			int depth = 1;
			while (i < len && depth > 0) {
				if (s[i] == '{') depth++;
				else if (s[i] == '}') depth--;
				if (depth > 0) i++;
			}
			if (depth == 0) {
				char *expr_str = nt_arena_strndup(p->arena, s + start, i - start);
				nt_parser sub;
				nt_parser_init_with_arena(&sub, expr_str, p->lex.filename, p->arena);
				nt_expr *sub_e = parse_expr(&sub, 0);
				// Keep arena state in sync after sub-parse allocations.
			p->arena = sub.arena;
				nt_fstring_part part = {NT_FSP_EXPR, {.e = sub_e}};
				nt_vec_push(&e->as.fstring.parts, part);
				i++; // skip '}'
			} else {
				parse_error(p, tok, "unterminated interpolation in f-string", NULL);
				break;
			}
		} else {
			size_t start = i;
			while (i < len && s[i] != '{') {
				if (s[i] == '\\' && i + 1 < len) i += 2;
				else i++;
			}
			nt_fstring_part part;
			part.kind = NT_FSP_STR;
			part.as.s.data = decode_fstring_part(p, s + start, i - start, &part.as.s.len);
			nt_vec_push(&e->as.fstring.parts, part);
		}
	}
	return e;
}

static nt_expr *parse_primary(nt_parser *p) {
	nt_token tok = p->cur;
	switch (tok.kind) {
	case NT_T_COMPTIME: {
		advance(p);
		nt_stmt *body = NULL;
		if (p->cur.kind == NT_T_LBRACE) {
			body = parse_block(p);
		} else {
			// implicit return
			nt_expr *val = parse_expr(p, 0);
			nt_stmt *ret = nt_stmt_new(p->arena, NT_S_RETURN, tok);
			ret->as.ret.value = val;
			body = nt_stmt_new(p->arena, NT_S_BLOCK, tok);
			nt_vec_push(&body->as.block.body, ret);
		}
		nt_expr *e = nt_expr_new(p->arena, NT_E_COMPTIME, tok);
		e->as.comptime_expr.body = body;
		return e;
	}
	case NT_T_IDENT: {
		advance(p);
		nt_expr *id = nt_expr_new(p->arena, NT_E_IDENT, tok);
		id->as.ident.name = nt_arena_strndup(p->arena, tok.lexeme, tok.len);
		return id;
	}
	case NT_T_NUMBER: {
		advance(p);
		nt_expr *lit = nt_expr_new(p->arena, NT_E_LITERAL, tok);
		bool is_hex = (tok.len > 2 && tok.lexeme[0] == '0' && (tok.lexeme[1] == 'x' || tok.lexeme[1] == 'X'));
		if (!is_hex && (memchr(tok.lexeme, '.', tok.len) || memchr(tok.lexeme, 'e', tok.len) || memchr(tok.lexeme, 'E', tok.len))) {
			lit->as.literal.kind = NT_LIT_FLOAT;
			lit->as.literal.as.f = strtod(tok.lexeme, NULL);
		} else {
			lit->as.literal.kind = NT_LIT_INT;
			lit->as.literal.as.i = strtoll(tok.lexeme, NULL, 0);
		}
		return lit;
	}
	case NT_T_TRUE:
	case NT_T_FALSE: {
		advance(p);
		nt_expr *lit = nt_expr_new(p->arena, NT_E_LITERAL, tok);
		lit->as.literal.kind = NT_LIT_BOOL;
		lit->as.literal.as.b = tok.kind == NT_T_TRUE;
		return lit;
	}
	case NT_T_NIL: {
		advance(p);
		nt_expr *lit = nt_expr_new(p->arena, NT_E_LITERAL, tok);
		lit->as.literal.kind = NT_LIT_INT;
		lit->as.literal.as.i = 0;
		return lit;
	}
	case NT_T_STRING: {
		advance(p);
		nt_expr *lit = nt_expr_new(p->arena, NT_E_LITERAL, tok);
		lit->as.literal.kind = NT_LIT_STR;
		size_t slen = 0;
		const char *sval = decode_string(p, tok, &slen);
		lit->as.literal.as.s.data = sval;
		lit->as.literal.as.s.len = slen;
		return lit;
	}
	case NT_T_FSTRING: return parse_fstring(p, tok);
	case NT_T_MATCH: {
		nt_stmt *s = parse_match(p);
		nt_expr *e = nt_expr_new(p->arena, NT_E_MATCH, tok);
		e->as.match = s->as.match;
		return e;
	}
	case NT_T_DOT: {
		advance(p);
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "member access expects identifier", NULL);
			return NULL;
		}
		nt_expr *e = nt_expr_new(p->arena, NT_E_INFERRED_MEMBER, tok);
		e->as.inferred_member.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
		advance(p);
		return e;
	}
	case NT_T_LPAREN: {
		advance(p);
		if (match(p, NT_T_RPAREN)) {
			return nt_expr_new(p->arena, NT_E_TUPLE, tok);
		}
		nt_expr *inner = parse_expr(p, 0);
		if (p->cur.kind == NT_T_COMMA) {
			nt_expr *tup = nt_expr_new(p->arena, NT_E_TUPLE, tok);
			nt_vec_push(&tup->as.list_like, inner);
			while (match(p, NT_T_COMMA)) {
				if (p->cur.kind == NT_T_RPAREN) break;
				nt_vec_push(&tup->as.list_like, parse_expr(p, 0));
			}
			expect(p, NT_T_RPAREN, NULL, NULL);
			return tup;
		}
		expect(p, NT_T_RPAREN, NULL, NULL);
		return inner;
	}
	case NT_T_LBRACK: {
		advance(p);
		nt_expr *lit = nt_expr_new(p->arena, NT_E_LIST, tok);
		if (p->cur.kind != NT_T_RBRACK) {
			while (true) {
				nt_expr *item = parse_expr(p, 0);
				nt_vec_push(&lit->as.list_like, item);
				if (!match(p, NT_T_COMMA)) break;
				if (p->cur.kind == NT_T_RBRACK) break; // allow trailing comma
			}
		}
		expect(p, NT_T_RBRACK, NULL, NULL);
		return lit;
	}
	case NT_T_LBRACE: {
		// parse set or dict based on presence of colon
		advance(p);
		if (p->cur.kind == NT_T_RBRACE) {
			expect(p, NT_T_RBRACE, NULL, NULL);
			nt_expr *set = nt_expr_new(p->arena, NT_E_SET, tok);
			return set;
		}
		nt_expr *first = parse_expr(p, 0);
		if (match(p, NT_T_COLON)) {
			nt_expr *dict = nt_expr_new(p->arena, NT_E_DICT, tok);
			nt_dict_pair pair = {first, parse_expr(p, 0)};
			nt_vec_push(&dict->as.dict.pairs, pair);
			while (match(p, NT_T_COMMA)) {
				if (p->cur.kind == NT_T_RBRACE) break; // trailing comma
				nt_expr *k = parse_expr(p, 0);
				expect(p, NT_T_COLON, NULL, NULL);
				nt_expr *v = parse_expr(p, 0);
				pair.key = k;
				pair.value = v;
				nt_vec_push(&dict->as.dict.pairs, pair);
			}
			expect(p, NT_T_RBRACE, NULL, NULL);
			return dict;
		} else {
			nt_expr *set = nt_expr_new(p->arena, NT_E_SET, tok);
			nt_vec_push(&set->as.list_like, first);
			while (match(p, NT_T_COMMA)) {
				if (p->cur.kind == NT_T_RBRACE) break; // trailing comma
				nt_vec_push(&set->as.list_like, parse_expr(p, 0));
			}
			expect(p, NT_T_RBRACE, NULL, NULL);
			return set;
		}
	}
	case NT_T_ASM: {
		advance(p);
		expect(p, NT_T_LPAREN, NULL, NULL);
		nt_token code_tok = p->cur;
		expect(p, NT_T_STRING, "assembly code string", NULL);
		size_t code_len;
		const char *code = decode_string(p, code_tok, &code_len);
		const char *constraints = "";
		if (match(p, NT_T_COMMA)) {
			nt_token constr_tok = p->cur;
			expect(p, NT_T_STRING, "constraints string", NULL);
			size_t constr_len;
			constraints = decode_string(p, constr_tok, &constr_len);
		}
		nt_expr *e = nt_expr_new(p->arena, NT_E_ASM, tok);
		e->as.as_asm.code = code;
		e->as.as_asm.constraints = constraints;
		while (match(p, NT_T_COMMA)) {
			nt_vec_push(&e->as.as_asm.args, parse_expr(p, 0));
		}
		expect(p, NT_T_RPAREN, NULL, NULL);
		return e;
	}
	case NT_T_EMBED: {
		advance(p);
		expect(p, NT_T_LPAREN, NULL, NULL);
		nt_token path_tok = p->cur;
		expect(p, NT_T_STRING, "file path string", NULL);
		size_t path_len;
		const char *path = decode_string(p, path_tok, &path_len);
		expect(p, NT_T_RPAREN, NULL, NULL);
		nt_expr *e = nt_expr_new(p->arena, NT_E_EMBED, tok);
		e->as.embed.path = path;
		return e;
	}
	case NT_T_LAMBDA:
	case NT_T_FN: {
		bool is_fn = tok.kind == NT_T_FN;
		advance(p);
		expect(p, NT_T_LPAREN, NULL, NULL);
		nt_expr *lam = nt_expr_new(p->arena, is_fn ? NT_E_FN : NT_E_LAMBDA, tok);
		while (p->cur.kind != NT_T_RPAREN) {
			if (match(p, NT_T_ELLIPSIS)) {
				lam->as.lambda.is_variadic = true;
			}
			nt_param pr = {0};
			if (p->cur.kind != NT_T_IDENT) {
				parse_error(p, p->cur, "param must be identifier", NULL);
				return lam;
			}
			pr.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
			advance(p);
			if (match(p, NT_T_COLON)) {
				if (p->cur.kind != NT_T_IDENT) parse_error(p, p->cur, "expected type name", NULL);
				else { pr.type = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len); advance(p); }
			}
			if (match(p, NT_T_ASSIGN)) pr.def = parse_expr(p, 0);
			nt_vec_push(&lam->as.lambda.params, pr);
			if (lam->as.lambda.is_variadic) {
				// variadic must be the last parameter
				if (p->cur.kind == NT_T_COMMA) {
					parse_error(p, p->cur, "variadic parameter must be the last one", NULL);
				}
				break;
			}
			if (!match(p, NT_T_COMMA)) break;
			if (p->cur.kind == NT_T_RPAREN) break; // trailing comma ok
		}
		expect(p, NT_T_RPAREN, NULL, NULL);
		if (match(p, NT_T_COLON)) {
			if (p->cur.kind != NT_T_IDENT) parse_error(p, p->cur, "expected return type", NULL);
			else { lam->as.lambda.return_type = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len); advance(p); }
		}
		lam->as.lambda.body = parse_block(p);
		return lam;
	}
	default:
		parse_error(p, tok, "unexpected token", NULL);
		return NULL; // unreachable
	}
}

static nt_expr *parse_postfix(nt_parser *p) {
	nt_expr *expr = parse_primary(p);
	for (;;) {
		if (p->cur.kind == NT_T_LPAREN) {
			advance(p);
			nt_expr *call = nt_expr_new(p->arena, NT_E_CALL, p->cur);
			call->as.call.callee = expr;
			while (p->cur.kind != NT_T_RPAREN) {
				nt_call_arg arg = {0};
				if (p->cur.kind == NT_T_IDENT && peek_token(p).kind == NT_T_ASSIGN) {
					arg.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
					advance(p); // name
					advance(p); // '='
					arg.val = parse_expr(p, 0);
				} else {
					arg.val = parse_expr(p, 0);
				}
				nt_vec_push(&call->as.call.args, arg);
				if (!match(p, NT_T_COMMA)) break;
			}
			expect(p, NT_T_RPAREN, NULL, NULL);
			expr = call;
		} else if (p->cur.kind == NT_T_DOT) {
			advance(p);
			if (p->cur.kind != NT_T_IDENT) {
				parse_error(p, p->cur, "member access expects identifier", NULL);
				return expr;
			}
			char *name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
			advance(p);
			nt_expr *mc = nt_expr_new(p->arena, NT_E_MEMCALL, p->cur);
			mc->as.memcall.target = expr;
			mc->as.memcall.name = name;
			if (p->cur.kind == NT_T_LPAREN) {
				advance(p);
				while (p->cur.kind != NT_T_RPAREN) {
					nt_call_arg arg = {0};
					if (p->cur.kind == NT_T_IDENT && peek_token(p).kind == NT_T_ASSIGN) {
						arg.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
						advance(p); // name
						advance(p); // '='
						arg.val = parse_expr(p, 0);
					} else {
						arg.val = parse_expr(p, 0);
					}
					nt_vec_push(&mc->as.memcall.args, arg);
					if (!match(p, NT_T_COMMA)) break;
				}
				expect(p, NT_T_RPAREN, NULL, NULL);
			}
			expr = mc;
		} else if (p->cur.kind == NT_T_LBRACK) {
			advance(p);
			nt_expr *idx = nt_expr_new(p->arena, NT_E_INDEX, p->cur);
			idx->as.index.target = expr;
			if (p->cur.kind != NT_T_RBRACK) {
				if (p->cur.kind == NT_T_COLON) {
					idx->as.index.start = NULL;
				} else {
					idx->as.index.start = parse_expr(p, 0);
				}
				if (match(p, NT_T_COLON)) {
					// Handle cases: [a:b], [a:], [a:b:c], [a::c], [:b], [:], [::]
					// Next token is either stop_expr, RBRACK, or COLON.
					if (p->cur.kind == NT_T_COLON) {
						// Double colon [a::c] -> stop is default (end of string)
						nt_expr *sent = nt_expr_new(p->arena, NT_E_LITERAL, p->cur);
						sent->as.literal.kind = NT_LIT_INT;
						sent->as.literal.as.i = 0x3fffffff;
						idx->as.index.stop = sent;
					} else if (p->cur.kind != NT_T_RBRACK) {
						idx->as.index.stop = parse_expr(p, 0);
					} else {
						// Single colon [a:] -> stop is default
						nt_expr *sent = nt_expr_new(p->arena, NT_E_LITERAL, p->cur);
						sent->as.literal.kind = NT_LIT_INT;
						sent->as.literal.as.i = 0x3fffffff;
						idx->as.index.stop = sent;
					}
					if (match(p, NT_T_COLON)) {
						if (p->cur.kind != NT_T_RBRACK) idx->as.index.step = parse_expr(p, 0);
					}
				}
			}
			expect(p, NT_T_RBRACK, NULL, NULL);
			expr = idx;
		} else {
			break;
		}
	}
	return expr;
}

static nt_expr *parse_unary(nt_parser *p) {
	if (p->cur.kind == NT_T_MINUS || p->cur.kind == NT_T_NOT || p->cur.kind == NT_T_BITNOT) {
		nt_token tok = p->cur;
		advance(p);
		nt_expr *expr = nt_expr_new(p->arena, NT_E_UNARY, tok);
		if (tok.kind == NT_T_MINUS) expr->as.unary.op = "-";
		else if (tok.kind == NT_T_NOT) expr->as.unary.op = "!";
		else expr->as.unary.op = "~";
		expr->as.unary.right = parse_unary(p);
		return expr;
	}
	return parse_postfix(p);
}

static nt_expr *parse_expr(nt_parser *p, int prec) {
	nt_expr *left = parse_unary(p);
	while (true) {
		// Check for ternary operator first (lowest precedence)
		if (prec < 1 && p->cur.kind == NT_T_QUESTION) {
			nt_token tok = p->cur;
			advance(p); // consume '?'
			nt_expr *true_expr = parse_expr(p, 0);
			expect(p, NT_T_COLON, ":'", "ternary operator requires ':'");
			nt_expr *false_expr = parse_expr(p, 0);
			nt_expr *ternary = nt_expr_new(p->arena, NT_E_TERNARY, tok);
			ternary->as.ternary.cond = left;
			ternary->as.ternary.true_expr = true_expr;
			ternary->as.ternary.false_expr = false_expr;
			left = ternary;
			continue;
		}
		int pcur = precedence(p->cur.kind);
		if (pcur < prec || pcur == 0) break;
		nt_token op = p->cur;
		advance(p);
		nt_expr *right = parse_expr(p, pcur + 1);
		nt_expr *bin;
		if (op.kind == NT_T_AND || op.kind == NT_T_OR) {
			bin = nt_expr_new(p->arena, NT_E_LOGICAL, op);
			bin->as.logical.op = (op.kind == NT_T_AND) ? "&&" : "||";
			bin->as.logical.left = left;
			bin->as.logical.right = right;
		} else {
			bin = nt_expr_new(p->arena, NT_E_BINARY, op);
			bin->as.binary.op = nt_arena_strndup(p->arena, op.lexeme, op.len);
			bin->as.binary.left = left;
			bin->as.binary.right = right;
		}
		left = bin;
	}
	return left;
}

static nt_stmt *parse_stmt_or_block(nt_parser *p) {
	if (p->cur.kind == NT_T_LBRACE) return parse_block(p);
	nt_token tok = p->cur;
	nt_stmt *s = parse_stmt(p);
	if (!s) return NULL;
	// Wrap single statement in a block for consistent scoping and AST structure
	nt_stmt *blk = nt_stmt_new(p->arena, NT_S_BLOCK, tok);
	nt_vec_push(&blk->as.block.body, s);
	return blk;
}

static nt_stmt *parse_if(nt_parser *p) {
	nt_token tok = p->cur;
	if (p->cur.kind == NT_T_IF || p->cur.kind == NT_T_ELIF) advance(p);
	else expect(p, NT_T_IF, "'if' or 'elif'", NULL);
	nt_expr *cond = parse_expr(p, 0);
	nt_stmt *block = parse_stmt_or_block(p);
	nt_stmt *alt = NULL;
	if (match(p, NT_T_ELSE)) {
		if (p->cur.kind == NT_T_IF) {
			alt = parse_if(p);
		} else {
			alt = parse_stmt_or_block(p);
		}
	} else if (p->cur.kind == NT_T_ELIF) {
		alt = parse_if(p);
	}
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_IF, tok);
	s->as.iff.test = cond;
	s->as.iff.conseq = block;
	s->as.iff.alt = alt;
	return s;
}

static nt_stmt *parse_while(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_WHILE, "'while'", NULL);
	nt_expr *cond = parse_expr(p, 0);
	nt_stmt *body = parse_stmt_or_block(p);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_WHILE, tok);
	s->as.whl.test = cond;
	s->as.whl.body = body;
	return s;
}

static nt_stmt *parse_for(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_FOR, "'for'", NULL);
	bool has_paren = match(p, NT_T_LPAREN);
	if (p->cur.kind != NT_T_IDENT) {
		parse_error(p, p->cur, "for expects loop variable", NULL);
		return NULL;
	}
	char *id = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
	advance(p);
	expect(p, NT_T_IN, "'in'", NULL);
	nt_expr *iter = parse_expr(p, 0);
	if (has_paren) expect(p, NT_T_RPAREN, ")' after condition", NULL);
	nt_stmt *body = parse_stmt_or_block(p);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_FOR, tok);
	s->as.fr.iter_var = id;
	s->as.fr.iterable = iter;
	s->as.fr.body = body;
	return s;
}

static nt_stmt *parse_try(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_TRY, "'try'", NULL);
	nt_stmt *body = parse_block(p);
	expect(p, NT_T_CATCH, "'catch'", NULL);
	const char *err = NULL;
	if (p->cur.kind == NT_T_LPAREN) {
		advance(p);
		if (p->cur.kind != NT_T_IDENT) parse_error(p, p->cur, "expected identifier after '(", NULL);
		else { err = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len); advance(p); }
		expect(p, NT_T_RPAREN, NULL, NULL);
	} else if (p->cur.kind == NT_T_IDENT) {
		err = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
		advance(p);
	}
	nt_stmt *handler = parse_block(p);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_TRY, tok);
	s->as.tr.body = body;
	s->as.tr.err = err;
	s->as.tr.handler = handler;
	return s;
}

static nt_stmt *parse_func(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_FN, "'fn'", NULL);
	if (p->cur.kind != NT_T_IDENT) {
		parse_error(p, p->cur, "expected function name", NULL);
		return NULL;
	}
	size_t cap = 256, len = 0;
	char *buf = malloc(cap);
	memcpy(buf, p->cur.lexeme, p->cur.len);
	len += p->cur.len;
	advance(p);
	while (match(p, NT_T_DOT)) {
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected identifier after '.'", NULL);
			free(buf); return NULL;
		}
		if (len + 1 + p->cur.len >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
		}
		buf[len++] = '.';
		memcpy(buf + len, p->cur.lexeme, p->cur.len);
		len += p->cur.len;
		advance(p);
	}
	buf[len] = '\0';
	char *final_name = buf;
	if (p->current_module) {
		// If user wrote dotted name 'mod.foo', check if it already starts with current_module?
		// For now simple prefix: module 'foo' { fn bar() } -> foo.bar
		// module 'foo' { fn foo.bar() } -> foo.foo.bar (probably not what user wants, but consistent)
		// To avoid double prefixing we could check.
		size_t clen = strlen(p->current_module);
		if (strncmp(buf, p->current_module, clen) == 0 && buf[clen] == '.') {
			// Already prefixed
		} else {
			char *prefixed = malloc(clen + 1 + len + 1);
			sprintf(prefixed, "%s.%s", p->current_module, buf);
			// free(buf);
			final_name = prefixed;
		}
	}
	char *name = nt_arena_strndup(p->arena, final_name, strlen(final_name));
	// free(buf);
	expect(p, NT_T_LPAREN, NULL, "'(' ");
	nt_param_list params = {0};
	nt_stmt *fn_stmt = nt_stmt_new(p->arena, NT_S_FUNC, tok);
	while (p->cur.kind != NT_T_RPAREN) {
		if (match(p, NT_T_ELLIPSIS)) {
				fn_stmt->as.fn.is_variadic = true;
			}
			nt_param pr = {0};
			if (p->cur.kind != NT_T_IDENT) {
				parse_error(p, p->cur, "param must be identifier", NULL);
				return NULL;
			}
			pr.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
			advance(p);
			if (match(p, NT_T_COLON)) {
				if (p->cur.kind != NT_T_IDENT) parse_error(p, p->cur, "expected type name", NULL);
				else { pr.type = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len); advance(p); }
			}
			if (match(p, NT_T_ASSIGN)) pr.def = parse_expr(p, 0);
			nt_vec_push(&params, pr);
			if (fn_stmt->as.fn.is_variadic) {
				if (p->cur.kind == NT_T_COMMA) {
					parse_error(p, p->cur, "variadic parameter must be the last one", NULL);
				}
				break;
			}
			if (!match(p, NT_T_COMMA)) break;
			if (p->cur.kind == NT_T_RPAREN) break;
		}
	expect(p, NT_T_RPAREN, NULL, NULL);
	if (match(p, NT_T_COLON)) {
		if (p->cur.kind != NT_T_IDENT) parse_error(p, p->cur, "expected return type", NULL);
		else { fn_stmt->as.fn.return_type = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len); advance(p); }
	}
	if (match(p, NT_T_SEMI)) {
		nt_stmt *s = fn_stmt;
		s->as.fn.name = name;
		s->as.fn.params = params;
		s->as.fn.body = NULL;
		s->as.fn.doc = NULL;
		s->as.fn.src_start = tok.lexeme;
		s->as.fn.src_end = p->prev.lexeme + p->prev.len;
		return s;
	}
	nt_stmt *body = parse_block(p);
	const char *doc = NULL;
	// extract docstring: if first stmt in block is expr string literal
	if (body->as.block.body.len > 0) {
		nt_stmt *s0 = body->as.block.body.data[0];
		if (s0->kind == NT_S_EXPR && s0->as.expr.expr->kind == NT_E_LITERAL &&
			s0->as.expr.expr->as.literal.kind == NT_LIT_STR) {
			doc = nt_arena_strndup(p->arena, s0->as.expr.expr->as.literal.as.s.data, s0->as.expr.expr->as.literal.as.s.len);
			// remove doc expr from block
			memmove(body->as.block.body.data, body->as.block.body.data + 1,
					(body->as.block.body.len - 1) * sizeof(nt_stmt *));
			body->as.block.body.len -= 1;
		}
	}
	nt_stmt *s = fn_stmt;
	s->as.fn.name = name;
	s->as.fn.params = params;
	s->as.fn.body = body;
	s->as.fn.doc = doc;
	s->as.fn.src_start = tok.lexeme;
	s->as.fn.src_end = p->prev.lexeme + p->prev.len;
	return s;
}

static nt_stmt *parse_return(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_RETURN, "'return'", NULL);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_RETURN, tok);
	if (p->cur.kind != NT_T_SEMI && p->cur.kind != NT_T_RBRACE) s->as.ret.value = parse_expr(p, 0);
	match(p, NT_T_SEMI);
	return s;
}

static nt_stmt *parse_goto(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_GOTO, "'goto'", NULL);
	if (p->cur.kind != NT_T_IDENT) {
		parse_error(p, p->cur, "goto expects label", NULL);
		return NULL;
	}
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_GOTO, tok);
	s->as.go.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
	advance(p);
	match(p, NT_T_SEMI);
	return s;
}

static nt_stmt *parse_use(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_USE, "'use'", NULL);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_USE, tok);
	s->as.use.is_local = false;
	s->as.use.import_all = false;
	if (p->cur.kind == NT_T_STRING) {
		size_t slen = 0;
		const char *sval = decode_string(p, p->cur, &slen);
		s->as.use.module = sval;
		s->as.use.is_local = true;
		advance(p);
	} else if (p->cur.kind == NT_T_IDENT) {
		size_t cap = 64, len = 0;
		char *buf = malloc(cap);
		if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
		memcpy(buf, p->cur.lexeme, p->cur.len);
		len += p->cur.len;
		advance(p);
		while (match(p, NT_T_DOT)) {
			if (p->cur.kind != NT_T_IDENT) {
				parse_error(p, p->cur, "expected identifier after '.'", NULL);
				free(buf);
				return NULL;
			}
			if (len + 1 + p->cur.len + 1 > cap) {
				cap = (len + 1 + p->cur.len + 1) * 2;
				char *nb = realloc(buf, cap);
				if (!nb) { fprintf(stderr, "oom\n"); exit(1); }
				buf = nb;
			}
			buf[len++] = '.';
			memcpy(buf + len, p->cur.lexeme, p->cur.len);
			len += p->cur.len;
			advance(p);
		}
		buf[len] = '\0';
		s->as.use.module = nt_arena_strndup(p->arena, buf, len);
		free(buf);
	} else {
		parse_error(p, p->cur, "use expects module identifier or string path", NULL);
		return NULL;
	}
	if (match(p, NT_T_STAR)) {
		s->as.use.import_all = true;
	}
	if (p->cur.kind == NT_T_LPAREN) {
		if (s->as.use.import_all) {
			parse_error(p, p->cur, "use '*' cannot be combined with an import list", NULL);
		}
		advance(p);
		while (p->cur.kind != NT_T_RPAREN && p->cur.kind != NT_T_EOF) {
			if (p->cur.kind != NT_T_IDENT) {
				parse_error(p, p->cur, "expected identifier in import list", NULL);
				break;
			}
			nt_use_item item = {0};
			item.name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
			advance(p);
			if (match(p, NT_T_AS)) {
				if (p->cur.kind != NT_T_IDENT) {
					parse_error(p, p->cur, "expected identifier after 'as'", NULL);
				} else {
					item.alias = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
					advance(p);
				}
			}
			nt_vec_push(&s->as.use.imports, item);
			if (match(p, NT_T_COMMA)) {
				continue;
			}
			if (p->cur.kind == NT_T_IDENT) continue;
			break;
		}
		expect(p, NT_T_RPAREN, ")'", NULL);
	}
	s->as.use.alias = NULL;
	if (!s->as.use.import_all && s->as.use.imports.len == 0 && match(p, NT_T_AS)) {
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected identifier after 'as'", NULL);
		} else {
			s->as.use.alias = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
			advance(p);
		}
	} else if (s->as.use.import_all || s->as.use.imports.len > 0) {
		if (p->cur.kind == NT_T_AS) {
			parse_error(p, p->cur, "module alias cannot be combined with an import list", NULL);
		}
	}
	match(p, NT_T_SEMI);
	return s;
}

static nt_stmt *parse_break(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_BREAK, "'break'", NULL);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_BREAK, tok);
	match(p, NT_T_SEMI);
	return s;
}

static nt_stmt *parse_continue(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_CONTINUE, "'continue'", NULL);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_CONTINUE, tok);
	match(p, NT_T_SEMI);
	return s;
}

static nt_stmt *parse_layout(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_LAYOUT, "'layout'", NULL);
	if (p->cur.kind != NT_T_IDENT) {
		parse_error(p, p->cur, "layout expects name", NULL);
		return NULL;
	}
	const char *name = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
	advance(p);
	expect(p, NT_T_LPAREN, "'('", NULL);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_LAYOUT, tok);
	s->as.layout.name = name;
	while (p->cur.kind != NT_T_RPAREN && p->cur.kind != NT_T_EOF) {
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected field name", NULL);
			break;
		}
		const char *fname = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
		advance(p);
		expect(p, NT_T_COLON, "' :", NULL);
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected type name", NULL);
			break;
		}
		const char *tname = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
		advance(p);
		nt_layout_field f = {fname, tname, 0};
		nt_vec_push(&s->as.layout.fields, f);
		if (p->cur.kind == NT_T_COMMA) advance(p);
	}
	expect(p, NT_T_RBRACE, "'}'", NULL);
	return s;
}

static nt_stmt *parse_match(nt_parser *p) {
	nt_token tok = p->cur;
	advance(p);
	nt_stmt *s = nt_stmt_new(p->arena, NT_S_MATCH, tok);
	s->as.match.test = parse_expr(p, 0);
	expect(p, NT_T_LBRACE, "'{'", NULL);
	s->as.match.default_conseq = NULL;
	while (p->cur.kind != NT_T_RBRACE && p->cur.kind != NT_T_EOF) {
		if (p->cur.kind == NT_T_ELSE) {
			advance(p);
			s->as.match.default_conseq = parse_block(p);
		} else {
			nt_match_arm arm;
			memset(&arm, 0, sizeof(arm));
			nt_expr *first = parse_expr(p, 0);
			nt_vec_push(&arm.patterns, first);
			while (1) {
				if (match(p, NT_T_COMMA)) {
					nt_expr *pat = parse_expr(p, 0);
					nt_vec_push(&arm.patterns, pat);
					continue;
				}
				// Allow space-separated patterns until we hit the arm separator or block.
				if (p->cur.kind == NT_T_ARROW || p->cur.kind == NT_T_COLON ||
					p->cur.kind == NT_T_LBRACE || p->cur.kind == NT_T_RBRACE ||
					p->cur.kind == NT_T_ELSE || p->cur.kind == NT_T_EOF) {
					break;
				}
				nt_expr *pat = parse_expr(p, 0);
				nt_vec_push(&arm.patterns, pat);
			}
			// Arm separator: '->' for expression or block; bare '{' allowed for block.
			if (match(p, NT_T_ARROW)) {
				if (p->cur.kind == NT_T_LBRACE) {
					arm.conseq = parse_block(p);
				} else {
					// Single-expression arm: wrap into a block
					nt_token etok = p->cur;
					nt_expr *e = parse_expr(p, 0);
					match(p, NT_T_SEMI);
					nt_stmt *blk = nt_stmt_new(p->arena, NT_S_BLOCK, etok);
					nt_stmt *es = nt_stmt_new(p->arena, NT_S_EXPR, etok);
					es->as.expr.expr = e;
					nt_vec_push(&blk->as.block.body, es);
					arm.conseq = blk;
				}
			} else if (p->cur.kind == NT_T_LBRACE) {
				arm.conseq = parse_block(p);
			} else {
				parse_error(p, p->cur, "expected '->' or block after case patterns", NULL);
				arm.conseq = parse_block(p);
			}
			nt_vec_push(&s->as.match.arms, arm);
		}
	}
	expect(p, NT_T_RBRACE, "'}'", NULL);
	return s;
}

static nt_stmt *parse_module(nt_parser *p) {
	nt_token tok = p->cur;
	advance(p);
	if (p->cur.kind != NT_T_IDENT) {
		parse_error(p, p->cur, "expected module name", NULL);
		return NULL;
	}
	// Support dotted module names
	size_t cap = 256, len = 0;
	char *buf = malloc(cap);
	memcpy(buf, p->cur.lexeme, p->cur.len);
	len += p->cur.len;
	advance(p);
	while (match(p, NT_T_DOT)) {
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected identifier after '.'", NULL);
			free(buf); return NULL;
		}
		if (len + 1 + p->cur.len >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { free(buf); return NULL; }
			buf = nb;
		}
		buf[len++] = '.';
		memcpy(buf + len, p->cur.lexeme, p->cur.len);
		len += p->cur.len;
		advance(p);
	}
	buf[len] = '\0';
	char *mod_name = nt_arena_strndup(p->arena, buf, len);
	free(buf);
	bool export_all = false;
	if (match(p, NT_T_STAR)) {
		export_all = true;
	}
	nt_token_kind end_kind = NT_T_EOF;
	if (p->cur.kind == NT_T_LPAREN) {
		advance(p);
		end_kind = NT_T_RPAREN;
	} else if (p->cur.kind == NT_T_LBRACE) {
		advance(p);
		end_kind = NT_T_RBRACE;
	}
	char *prev_mod = p->current_module;
	p->current_module = mod_name;
	nt_stmt *mod_stmt = nt_stmt_new(p->arena, NT_S_MODULE, tok);
	mod_stmt->as.module.name = mod_name;
	mod_stmt->as.module.export_all = export_all;
	while (p->cur.kind != end_kind && p->cur.kind != NT_T_EOF) {
		// Check for export list: identifiers separated by comma or just sequence
		// Caveat: variable decl 'def x = 1' or 'fn foo' start with keywords.
		// 'x = 1' starts with ident then assign.
		// 'call()' starts with ident then lparen.
			// 'export_name' then comma or another ident (if vertical/no-separator) or RPAREN.
		if (p->cur.kind == NT_T_IDENT) {
			nt_token next = peek_token(p);
			bool is_export = false;
			if (next.kind == NT_T_COMMA || next.kind == end_kind) is_export = true;
			// For vertical list: 'min \n max'. Parser doesn't see newline. Next is 'max' (IDENT).
			// But 'min max' is invalid expr/stmt anyway?
			// 'min; max;' is valid.
			// If we see IDENT IDENT, it's likely an export list or syntax error.
			// Let's treat it as export.
			if (next.kind == NT_T_IDENT) is_export = true;
			// What about 'x = 1'? Next is ASSIGN. Not export.
			// 'foo.bar'? Next is DOT.
			if (is_export) {
				nt_stmt *ex = nt_stmt_new(p->arena, NT_S_EXPORT, p->cur);
				while (p->cur.kind == NT_T_IDENT) {
					char *ename = nt_arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
					nt_vec_push(&ex->as.exprt.names, ename);
					advance(p);
					if (match(p, NT_T_COMMA)) {
						// continue
					} else {
						// If next is ident, continue (vertical/space separated)
						if (p->cur.kind == NT_T_IDENT) continue;
						break;
					}
				}
				nt_vec_push(&mod_stmt->as.module.body, ex);
				continue;
			}
		}
		nt_stmt *s = parse_stmt(p);
		if (s) {
			nt_vec_push(&mod_stmt->as.module.body, s);
		} else if (p->had_error) {
			synchronize(p);
		}
	}
	p->current_module = prev_mod;
	if (end_kind == NT_T_RPAREN) {
		expect(p, NT_T_RPAREN, ")'", NULL);
	} else if (end_kind == NT_T_RBRACE) {
		expect(p, NT_T_RBRACE, "'}'", NULL);
	}
	mod_stmt->as.module.src_start = tok.lexeme;
	mod_stmt->as.module.src_end = p->prev.lexeme + p->prev.len;
	return mod_stmt;
}

static nt_stmt *parse_stmt(nt_parser *p) {
	switch (p->cur.kind) {
	case NT_T_SEMI:
		advance(p);
		return NULL;
	case NT_T_USE: return parse_use(p);
	case NT_T_MODULE: return parse_module(p);
	case NT_T_LAYOUT: return parse_layout(p);
	case NT_T_FN: return parse_func(p);
	case NT_T_IF: return parse_if(p);
	case NT_T_ELIF:
		parse_error(p, p->cur, "'elif' without 'if'", "check if you forgot the preceding 'if' block");
		advance(p);
		return NULL;
	case NT_T_WHILE: return parse_while(p);
	case NT_T_FOR: return parse_for(p);
	case NT_T_TRY: return parse_try(p);
	case NT_T_RETURN: return parse_return(p);
	case NT_T_BREAK: return parse_break(p);
	case NT_T_CONTINUE: return parse_continue(p);
	case NT_T_GOTO: return parse_goto(p);
	case NT_T_MATCH: return parse_match(p);
	case NT_T_DEFER: {
		nt_token tok = p->cur;
		advance(p);
		nt_stmt *s = nt_stmt_new(p->arena, NT_S_DEFER, tok);
		s->as.de.body = parse_block(p);
		return s;
	}
	case NT_T_DEF: {
		nt_token start_tok = p->cur;
		advance(p);
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected identifier after 'def'", NULL);
			return NULL;
		}
		nt_token ident = p->cur;
		advance(p);
		nt_expr *rhs = NULL;
		if (match(p, NT_T_ASSIGN)) {
			rhs = parse_expr(p, 0);
		} else {
			 nt_token zero_tok = {0};
			 nt_expr *zero = nt_expr_new(p->arena, NT_E_LITERAL, zero_tok);
			 zero->as.literal.kind = NT_LIT_INT;
			 zero->as.literal.as.i = 0;
			 rhs = zero;
		}
		match(p, NT_T_SEMI);
		nt_stmt *s = nt_stmt_new(p->arena, NT_S_VAR, start_tok);
		char *final_name = (char*)ident.lexeme;
		size_t nlen = ident.len;
		bool mangled = false;
		if (p->current_module && p->block_depth == 0) {
			size_t mlen = strlen(p->current_module);
			char *prefixed = malloc(mlen + 1 + nlen + 1);
			sprintf(prefixed, "%s.%.*s", p->current_module, (int)nlen, ident.lexeme);
			final_name = prefixed;
			nlen = strlen(prefixed);
			mangled = true;
		}
		const char *name_s = nt_arena_strndup(p->arena, final_name, nlen);
		if (mangled) free(final_name);
		nt_vec_push(&s->as.var.names, name_s);
		s->as.var.expr = rhs;
		s->as.var.is_decl = true;
		s->as.var.is_undef = false;
		return s;
	}
	case NT_T_UNDEF: {
		nt_token start_tok = p->cur;
		advance(p);
		if (p->cur.kind != NT_T_IDENT) {
			parse_error(p, p->cur, "expected identifier after 'undef'", NULL);
			return NULL;
		}
		nt_token ident = p->cur;
		advance(p);
		match(p, NT_T_SEMI);
		nt_stmt *s = nt_stmt_new(p->arena, NT_S_VAR, start_tok);
		const char *name_s = nt_arena_strndup(p->arena, ident.lexeme, ident.len);
		nt_vec_push(&s->as.var.names, name_s);
		s->as.var.expr = NULL;
		s->as.var.is_decl = true;
		s->as.var.is_undef = true;
		return s;
	}
	case NT_T_IDENT: {
		nt_token ident_tok = p->cur;
		nt_token next = peek_token(p);
		if (next.kind == NT_T_COLON) {
			advance(p);            // consume ident
			expect(p, NT_T_COLON, NULL, "expected ':' after case/default label");
			nt_stmt *s = nt_stmt_new(p->arena, NT_S_LABEL, ident_tok);
			s->as.label.name = nt_arena_strndup(p->arena, ident_tok.lexeme, ident_tok.len);
			return s;
		}
		nt_expr *lhs = parse_expr(p, 0);
		nt_token_kind assign_op = NT_T_EOF;
		if (p->cur.kind == NT_T_ASSIGN || p->cur.kind == NT_T_PLUS_EQ ||
			p->cur.kind == NT_T_MINUS_EQ || p->cur.kind == NT_T_STAR_EQ || p->cur.kind == NT_T_SLASH_EQ) {
			assign_op = p->cur.kind;
			advance(p);
		}
		if (assign_op != NT_T_EOF) {
			nt_expr *rhs = parse_expr(p, 0);
			match(p, NT_T_SEMI);
			if (assign_op != NT_T_ASSIGN) {
				 nt_token_kind bin_kind =
					(assign_op == NT_T_PLUS_EQ) ? NT_T_PLUS :
					(assign_op == NT_T_MINUS_EQ) ? NT_T_MINUS :
					(assign_op == NT_T_STAR_EQ) ? NT_T_STAR :
					NT_T_SLASH;
				 nt_token op_tok = {0}; // dummy
				 nt_expr *bin = nt_expr_new(p->arena, NT_E_BINARY, op_tok);
				 bin->as.binary.op = token_name(bin_kind);
				 bin->as.binary.left = lhs;
				 bin->as.binary.right = rhs;
				 rhs = bin;
			}
			if (lhs->kind == NT_E_IDENT) {
				nt_stmt *s = nt_stmt_new(p->arena, NT_S_VAR, ident_tok);
				nt_vec_push(&s->as.var.names, lhs->as.ident.name);
				s->as.var.expr = rhs;
				s->as.var.is_decl = false;
				s->as.var.is_undef = false;
				return s;
			} else if (lhs->kind == NT_E_INDEX) {
				nt_expr *callee = nt_expr_new(p->arena, NT_E_IDENT, ident_tok);
				callee->as.ident.name = nt_arena_strndup(p->arena, "set_idx", 7);
				nt_expr *call = nt_expr_new(p->arena, NT_E_CALL, ident_tok);
				call->as.call.callee = callee;
				nt_vec_push(&call->as.call.args, ((nt_call_arg){NULL, lhs->as.index.target}));
				nt_expr *idx_expr = lhs->as.index.start;
				if (!idx_expr) {
					nt_expr *zero = nt_expr_new(p->arena, NT_E_LITERAL, ident_tok);
					zero->as.literal.kind = NT_LIT_INT;
					zero->as.literal.as.i = 0;
					idx_expr = zero;
				}
				nt_vec_push(&call->as.call.args, ((nt_call_arg){NULL, idx_expr}));
				nt_vec_push(&call->as.call.args, ((nt_call_arg){NULL, rhs}));
				nt_stmt *s = nt_stmt_new(p->arena, NT_S_EXPR, ident_tok);
				s->as.expr.expr = call;
				return s;
			} else {
				parse_error(p, ident_tok, "assignment target must be identifier or index", NULL);
				return NULL;
			}
		}
		nt_stmt *s = nt_stmt_new(p->arena, NT_S_EXPR, ident_tok);
		s->as.expr.expr = lhs;
		match(p, NT_T_SEMI);
		return s;
	}
	case NT_T_LBRACE: return parse_block(p);
	default: {
		// expression stmt
		nt_token first = p->cur;
		nt_expr *e = parse_expr(p, 0);
		nt_stmt *s = nt_stmt_new(p->arena, NT_S_EXPR, first);
		s->as.expr.expr = e;
		match(p, NT_T_SEMI);
		return s;
	}
	}
}

static nt_stmt *parse_block(nt_parser *p) {
	nt_token tok = p->cur;
	expect(p, NT_T_LBRACE, "'{'", NULL);
	p->block_depth++;
	nt_stmt *blk = nt_stmt_new(p->arena, NT_S_BLOCK, tok);
	while (p->cur.kind != NT_T_RBRACE && p->cur.kind != NT_T_EOF) {
		nt_stmt *s = parse_stmt(p);
		if (s) {
			nt_vec_push(&blk->as.block.body, s);
		} else if (p->had_error) {
			synchronize(p);
		}
	}
	p->block_depth--;
	expect(p, NT_T_RBRACE, "'}'", NULL);
	return blk;
}

nt_program nt_parse_program(nt_parser *p) {
	nt_program prog = {0};
	while (p->cur.kind != NT_T_EOF) {
		nt_stmt *s = parse_stmt(p);
		if (s) {
			nt_vec_push(&prog.body, s);
		} else if (p->had_error) {
			synchronize(p);
		}
	}
	// Extract module-level docstring
	if (prog.body.len > 0) {
		nt_stmt *s0 = prog.body.data[0];
		if (s0->kind == NT_S_EXPR && s0->as.expr.expr->kind == NT_E_LITERAL &&
			s0->as.expr.expr->as.literal.kind == NT_LIT_STR) {
			prog.doc = nt_arena_strndup(p->arena, s0->as.expr.expr->as.literal.as.s.data, s0->as.expr.expr->as.literal.as.s.len);
			memmove(prog.body.data, prog.body.data + 1, (prog.body.len - 1) * sizeof(nt_stmt *));
			prog.body.len -= 1;
		}
	}
	return prog;
}
