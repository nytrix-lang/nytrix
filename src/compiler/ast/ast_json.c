#include "ast.h"
#include "ast_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

// External runtime functions
extern int64_t rt_malloc(int64_t n);
extern int64_t rt_free(int64_t p);
extern int64_t rt_realloc(int64_t p, int64_t n);

static void dump_expr(nt_expr *e, char **buf, size_t *len, size_t *cap);
static void dump_stmt(nt_stmt *s, char **buf, size_t *len, size_t *cap);

static void append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (*len + n + 1 > *cap) {
		*cap = (*len + n + 1) * 2;
		*buf = (char *)(uintptr_t)rt_realloc((int64_t)(uintptr_t)*buf, *cap);
	}
	va_start(ap, fmt);
	vsnprintf(*buf + *len, n + 1, fmt, ap);
	va_end(ap);
	*len += n;
}

static void dump_literal(nt_literal *l, char **buf, size_t *len, size_t *cap) {
	append(buf, len, cap, "{\"type\":\"literal\",\"kind\":");
	switch (l->kind) {
		case NT_LIT_INT: append(buf, len, cap, "\"int\",\"value\":%ld}", l->as.i); break;
		case NT_LIT_FLOAT: append(buf, len, cap, "\"float\",\"value\":%f}", l->as.f); break;
		case NT_LIT_BOOL: append(buf, len, cap, "\"bool\",\"value\":%s}", l->as.b ? "true" : "false"); break;
		case NT_LIT_STR:
			append(buf, len, cap, "\"string\",\"value\":\"");
			for (size_t i = 0; i < l->as.s.len; ++i) {
				char c = l->as.s.data[i];
				if (c == '"') append(buf, len, cap, "\\\"");
				else if (c == '\\') append(buf, len, cap, "\\\\");
				else if (c == '\n') append(buf, len, cap, "\\n");
				else append(buf, len, cap, "%c", c);
			}
			append(buf, len, cap, "\"}");
			break;
	}
}

static void dump_expr(nt_expr *e, char **buf, size_t *len, size_t *cap) {
	if (!e) { append(buf, len, cap, "null"); return; }
	switch (e->kind) {
		case NT_E_IDENT: append(buf, len, cap, "{\"type\":\"ident\",\"name\":\"%s\"}", e->as.ident.name); break;
		case NT_E_LITERAL: dump_literal(&e->as.literal, buf, len, cap); break;
		case NT_E_UNARY:
			append(buf, len, cap, "{\"type\":\"unary\",\"op\":\"%s\",\"right\":", e->as.unary.op);
			dump_expr(e->as.unary.right, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_E_BINARY:
			append(buf, len, cap, "{\"type\":\"binary\",\"op\":\"%s\",\"left\":", e->as.binary.op);
			dump_expr(e->as.binary.left, buf, len, cap);
			append(buf, len, cap, ",\"right\":");
			dump_expr(e->as.binary.right, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_E_LOGICAL:
			append(buf, len, cap, "{\"type\":\"logical\",\"op\":\"%s\",\"left\":", e->as.logical.op);
			dump_expr(e->as.logical.left, buf, len, cap);
			append(buf, len, cap, ",\"right\":");
			dump_expr(e->as.logical.right, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_E_CALL:
			append(buf, len, cap, "{\"type\":\"call\",\"callee\":");
			dump_expr(e->as.call.callee, buf, len, cap);
			append(buf, len, cap, ",\"args\":[");
			for (size_t i = 0; i < e->as.call.args.len; ++i) {
				// For now, ignoring arg name in JSON output to preserve structure
				dump_expr(e->as.call.args.data[i].val, buf, len, cap);
				if (i < e->as.call.args.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "]}");
			break;
		case NT_E_MEMCALL:
			append(buf, len, cap, "{\"type\":\"memcall\",\"target\":");
			dump_expr(e->as.memcall.target, buf, len, cap);
			append(buf, len, cap, ",\"name\":\"%s\",\"args\":[", e->as.memcall.name);
			for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
				dump_expr(e->as.memcall.args.data[i].val, buf, len, cap);
				if (i < e->as.memcall.args.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "]}");
			break;
		case NT_E_INDEX:
			append(buf, len, cap, "{\"type\":\"index\",\"target\":");
			dump_expr(e->as.index.target, buf, len, cap);
			append(buf, len, cap, ",\"start\":");
			dump_expr(e->as.index.start, buf, len, cap);
			append(buf, len, cap, ",\"stop\":");
			dump_expr(e->as.index.stop, buf, len, cap);
			append(buf, len, cap, ",\"step\":");
			dump_expr(e->as.index.step, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_E_LAMBDA:
			append(buf, len, cap, "{\"type\":\"lambda\",\"params\":[");
			for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
				append(buf, len, cap, "{\"name\":\"%s\"}", e->as.lambda.params.data[i].name);
				if (i < e->as.lambda.params.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "] ,\"body\":");
			dump_stmt(e->as.lambda.body, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_E_LIST:
		case NT_E_TUPLE:
		case NT_E_SET:
			append(buf, len, cap, "{\"type\":\"%s\",\"elements\":[",
				e->kind == NT_E_LIST ? "list" : (e->kind == NT_E_TUPLE ? "tuple" : "set"));
			for (size_t i = 0; i < e->as.list_like.len; ++i) {
				dump_expr(e->as.list_like.data[i], buf, len, cap);
				if (i < e->as.list_like.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "]}");
			break;
		case NT_E_DICT:
			append(buf, len, cap, "{\"type\":\"dict\",\"pairs\":[");
			for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
				append(buf, len, cap, "{\"key\":");
				dump_expr(e->as.dict.pairs.data[i].key, buf, len, cap);
				append(buf, len, cap, ",\"value\":");
				dump_expr(e->as.dict.pairs.data[i].value, buf, len, cap);
				append(buf, len, cap, "}");
				if (i < e->as.dict.pairs.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "]}");
			break;
		case NT_E_ASM:
			append(buf, len, cap, "{\"type\":\"asm\",\"code\":\"%s\",\"constraints\":\"%s\",\"args\":[", e->as.as_asm.code, e->as.as_asm.constraints);
			for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
				dump_expr(e->as.as_asm.args.data[i], buf, len, cap);
				if (i < e->as.as_asm.args.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "]}");
			break;
		default: append(buf, len, cap, "{\"type\":\"unknown\"}"); break;
	}
}

static void dump_stmt(nt_stmt *s, char **buf, size_t *len, size_t *cap) {
	if (!s) { append(buf, len, cap, "null"); return; }
	switch (s->kind) {
		case NT_S_BLOCK:
			append(buf, len, cap, "{\"type\":\"block\",\"body\":[");
			for (size_t i = 0; i < s->as.block.body.len; ++i) {
				dump_stmt(s->as.block.body.data[i], buf, len, cap);
				if (i < s->as.block.body.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "]}");
			break;
		case NT_S_USE:
			append(buf, len, cap, "{\"type\":\"use\",\"module\":\"%s\"", s->as.use.module);
			if (s->as.use.alias) {
				append(buf, len, cap, ",\"alias\":\"%s\"", s->as.use.alias);
			}
			if (s->as.use.is_local) {
				append(buf, len, cap, ",\"local\":true");
			}
			append(buf, len, cap, "}");
			break;
		case NT_S_VAR:
			append(buf, len, cap, "{\"type\":\"var\",\"names\":[");
			for (size_t i = 0; i < s->as.var.names.len; ++i) {
				append(buf, len, cap, "\"%s\"", s->as.var.names.data[i]);
				if (i < s->as.var.names.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "],\"undef\":%s,\"expr\":", s->as.var.is_undef ? "true" : "false");
			if (s->as.var.expr) dump_expr(s->as.var.expr, buf, len, cap);
			else append(buf, len, cap, "null");
			append(buf, len, cap, "}");
			break;
		case NT_S_EXPR:
			append(buf, len, cap, "{\"type\":\"expr_stmt\",\"expr\":");
			dump_expr(s->as.expr.expr, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_IF:
			append(buf, len, cap, "{\"type\":\"if\",\"test\":");
			dump_expr(s->as.iff.test, buf, len, cap);
			append(buf, len, cap, ",\"conseq\":");
			dump_stmt(s->as.iff.conseq, buf, len, cap);
			append(buf, len, cap, ",\"alt\":");
			dump_stmt(s->as.iff.alt, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_WHILE:
			append(buf, len, cap, "{\"type\":\"while\",\"test\":");
			dump_expr(s->as.whl.test, buf, len, cap);
			append(buf, len, cap, ",\"body\":");
			dump_stmt(s->as.whl.body, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_FOR:
			append(buf, len, cap, "{\"type\":\"for\",\"var\":\"%s\",\"iterable\":", s->as.fr.iter_var);
			dump_expr(s->as.fr.iterable, buf, len, cap);
			append(buf, len, cap, ",\"body\":");
			dump_stmt(s->as.fr.body, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_TRY:
			append(buf, len, cap, "{\"type\":\"try\",\"body\":");
			dump_stmt(s->as.tr.body, buf, len, cap);
			append(buf, len, cap, ",\"err\":\"%s\",\"handler\":", s->as.tr.err ? s->as.tr.err : "null");
			dump_stmt(s->as.tr.handler, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_FUNC:
			append(buf, len, cap, "{\"type\":\"func\",\"name\":\"%s\",\"params\":[", s->as.fn.name);
			for (size_t i = 0; i < s->as.fn.params.len; ++i) {
				append(buf, len, cap, "{\"name\":\"%s\"}", s->as.fn.params.data[i].name);
				if (i < s->as.fn.params.len - 1) append(buf, len, cap, ",");
			}
			append(buf, len, cap, "] ,\"body\":");
			dump_stmt(s->as.fn.body, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_RETURN:
			append(buf, len, cap, "{\"type\":\"return\",\"value\":");
			dump_expr(s->as.ret.value, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_LABEL:
			append(buf, len, cap, "{\"type\":\"label\",\"name\":\"%s\"}", s->as.label.name);
			break;
		case NT_S_GOTO:
			append(buf, len, cap, "{\"type\":\"goto\",\"name\":\"%s\"}", s->as.go.name);
			break;
		case NT_S_DEFER:
			append(buf, len, cap, "{\"type\":\"defer\",\"body\":");
			dump_stmt(s->as.de.body, buf, len, cap);
			append(buf, len, cap, "}");
			break;
		case NT_S_BREAK:
			append(buf, len, cap, "{\"type\":\"break\"}");
			break;
		case NT_S_CONTINUE:
			append(buf, len, cap, "{\"type\":\"continue\"}");
			break;
		default: append(buf, len, cap, "{\"type\":\"unknown_stmt\"}"); break;
	}
}

char *nt_ast_to_json(nt_program *prog) {
	size_t len = 0, cap = 1024;
	char *buf = (char *)(uintptr_t)rt_malloc(cap);
	append(&buf, &len, &cap, "[");
	for (size_t i = 0; i < prog->body.len; ++i) {
		dump_stmt(prog->body.data[i], &buf, &len, &cap);
		if (i < prog->body.len - 1) append(&buf, &len, &cap, ",");
	}
	append(&buf, &len, &cap, "]");
	return buf;
}
