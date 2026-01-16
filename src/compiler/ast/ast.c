#include "ast.h"

nt_expr *nt_expr_new(nt_arena *arena, nt_expr_kind kind, nt_token tok) {
	nt_expr *e = (nt_expr *)nt_arena_alloc(arena, sizeof(nt_expr));
	e->kind = kind;
	e->tok = tok;
	memset(&e->as, 0, sizeof(e->as));
	return e;
}

nt_stmt *nt_stmt_new(nt_arena *arena, nt_stmt_kind kind, nt_token tok) {
	nt_stmt *s = (nt_stmt *)nt_arena_alloc(arena, sizeof(nt_stmt));
	s->kind = kind;
	s->tok = tok;
	memset(&s->as, 0, sizeof(s->as));
	return s;
}

static void free_expr(nt_expr *e);
static void free_stmt(nt_stmt *s);

static void free_expr_list(nt_expr_list *l) {
	for (size_t i = 0; i < l->len; ++i) free_expr(l->data[i]);
	nt_vec_free(l);
}

static void free_stmt_list(nt_stmt_list *l) {
	for (size_t i = 0; i < l->len; ++i) free_stmt(l->data[i]);
	nt_vec_free(l);
}

static void free_expr(nt_expr *e) {
	if (!e) return;
	switch (e->kind) {
	case NT_E_UNARY: free_expr(e->as.unary.right); break;
	case NT_E_BINARY: free_expr(e->as.binary.left); free_expr(e->as.binary.right); break;
	case NT_E_LOGICAL: free_expr(e->as.logical.left); free_expr(e->as.logical.right); break;
	case NT_E_CALL:
		free_expr(e->as.call.callee);
		for (size_t i = 0; i < e->as.call.args.len; ++i) free_expr(e->as.call.args.data[i].val);
		nt_vec_free(&e->as.call.args);
		break;
	case NT_E_MEMCALL:
		free_expr(e->as.memcall.target);
		for (size_t i = 0; i < e->as.memcall.args.len; ++i) free_expr(e->as.memcall.args.data[i].val);
		nt_vec_free(&e->as.memcall.args);
		break;
	case NT_E_INDEX:
		free_expr(e->as.index.target);
		free_expr(e->as.index.start);
		free_expr(e->as.index.stop);
		free_expr(e->as.index.step);
		break;
	case NT_E_LAMBDA:
	case NT_E_FN:
		for (size_t i = 0; i < e->as.lambda.params.len; ++i) free_expr(e->as.lambda.params.data[i].def);
		nt_vec_free(&e->as.lambda.params);
		free_stmt(e->as.lambda.body);
		break;
	case NT_E_LIST:
	case NT_E_TUPLE:
	case NT_E_SET: free_expr_list(&e->as.list_like); break;
	case NT_E_DICT:
		for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
			free_expr(e->as.dict.pairs.data[i].key);
			free_expr(e->as.dict.pairs.data[i].value);
		}
		nt_vec_free(&e->as.dict.pairs);
		break;
	case NT_E_ASM:
		free_expr_list(&e->as.as_asm.args);
		break;
	case NT_E_COMPTIME:
		free_stmt(e->as.comptime_expr.body);
		break;
	case NT_E_FSTRING:
		for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
			if (e->as.fstring.parts.data[i].kind == NT_FSP_EXPR) free_expr(e->as.fstring.parts.data[i].as.e);
		}
		nt_vec_free(&e->as.fstring.parts);
		break;
	default: break;
	}
}

static void free_stmt(nt_stmt *s) {
	if (!s) return;
	switch (s->kind) {
	case NT_S_MODULE:
		free_stmt_list(&s->as.module.body);
		break;
	case NT_S_BLOCK: free_stmt_list(&s->as.block.body); break;
	case NT_S_VAR:
		nt_vec_free(&s->as.var.names);
		free_expr(s->as.var.expr);
		break;
	case NT_S_EXPR: free_expr(s->as.expr.expr); break;
	case NT_S_IF:
		free_expr(s->as.iff.test);
		free_stmt(s->as.iff.conseq);
		free_stmt(s->as.iff.alt);
		break;
	case NT_S_WHILE:
		free_expr(s->as.whl.test);
		free_stmt(s->as.whl.body);
		break;
	case NT_S_FOR:
		free_expr(s->as.fr.iterable);
		free_stmt(s->as.fr.body);
		break;
	case NT_S_TRY:
		free_stmt(s->as.tr.body);
		free_stmt(s->as.tr.handler);
		break;
	case NT_S_FUNC:
		for (size_t i = 0; i < s->as.fn.params.len; ++i) free_expr(s->as.fn.params.data[i].def);
		nt_vec_free(&s->as.fn.params);
		free_stmt(s->as.fn.body);
		break;
	case NT_S_RETURN: free_expr(s->as.ret.value); break;
	case NT_S_DEFER: free_stmt(s->as.de.body); break;
	case NT_S_LAYOUT: nt_vec_free(&s->as.layout.fields); break;
	case NT_S_MATCH:
		free_expr(s->as.match.test);
		for (size_t i = 0; i < s->as.match.arms.len; ++i) {
			for (size_t j = 0; j < s->as.match.arms.data[i].patterns.len; ++j) {
				free_expr(s->as.match.arms.data[i].patterns.data[j]);
			}
			nt_vec_free(&s->as.match.arms.data[i].patterns);
			free_stmt(s->as.match.arms.data[i].conseq);
		}
		nt_vec_free(&s->as.match.arms);
		free_stmt(s->as.match.default_conseq);
		break;
	default: break;
	}
}

void nt_program_free(nt_program *prog, nt_arena *arena) {
	if (prog) {
		free_stmt_list(&prog->body);
	}
	nt_arena_free(arena);
	free(arena);
}