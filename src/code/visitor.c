#include "code/visitor.h"
#include <stddef.h>

void ny_visit_expr(ny_visitor_t *v, expr_t *e) {
  if (!v || !e)
    return;

  bool traverse = true;
  if (v->visit_expr_pre) {
    traverse = v->visit_expr_pre(v, e);
  }

  if (traverse) {
    switch (e->kind) {
    case NY_E_UNARY:
      ny_visit_expr(v, e->as.unary.right);
      break;
    case NY_E_BINARY:
    case NY_E_LOGICAL:
      ny_visit_expr(v, e->as.binary.left);
      ny_visit_expr(v, e->as.binary.right);
      break;
    case NY_E_TERNARY:
      ny_visit_expr(v, e->as.ternary.cond);
      ny_visit_expr(v, e->as.ternary.true_expr);
      ny_visit_expr(v, e->as.ternary.false_expr);
      break;
    case NY_E_CALL:
      ny_visit_expr(v, e->as.call.callee);
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        ny_visit_expr(v, e->as.call.args.data[i].val);
      }
      break;
    case NY_E_MEMCALL:
      ny_visit_expr(v, e->as.memcall.target);
      for (size_t i = 0; i < e->as.memcall.args.len; i++) {
        ny_visit_expr(v, e->as.memcall.args.data[i].val);
      }
      break;
    case NY_E_INDEX:
      ny_visit_expr(v, e->as.index.target);
      ny_visit_expr(v, e->as.index.start);
      ny_visit_expr(v, e->as.index.stop);
      ny_visit_expr(v, e->as.index.step);
      break;
    case NY_E_MEMBER:
      ny_visit_expr(v, e->as.member.target);
      break;
    case NY_E_PTR_TYPE:
      ny_visit_expr(v, e->as.ptr_type.target);
      break;
    case NY_E_DEREF:
      ny_visit_expr(v, e->as.deref.target);
      break;
    case NY_E_SIZEOF:
      if (!e->as.szof.is_type) {
        ny_visit_expr(v, e->as.szof.target);
      }
      break;
    case NY_E_TRY:
      ny_visit_expr(v, e->as.try_expr.target);
      break;
    case NY_E_LIST:
    case NY_E_TUPLE:
    case NY_E_SET:
      for (size_t i = 0; i < e->as.list_like.len; i++) {
        ny_visit_expr(v, e->as.list_like.data[i]);
      }
      break;
    case NY_E_DICT:
      for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
        ny_visit_expr(v, e->as.dict.pairs.data[i].key);
        ny_visit_expr(v, e->as.dict.pairs.data[i].value);
      }
      break;
    case NY_E_COMPTIME:
      ny_visit_stmt(v, e->as.comptime_expr.body);
      break;
    case NY_E_FSTRING:
      for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
        if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR) {
          ny_visit_expr(v, e->as.fstring.parts.data[i].as.e);
        }
      }
      break;
    case NY_E_MATCH:
      ny_visit_expr(v, e->as.match.test);
      for (size_t i = 0; i < e->as.match.arms.len; i++) {
        match_arm_t *arm = &e->as.match.arms.data[i];
        for (size_t j = 0; j < arm->patterns.len; j++) {
          ny_visit_expr(v, arm->patterns.data[j]);
        }
        ny_visit_expr(v, arm->guard);
        ny_visit_stmt(v, arm->conseq);
      }
      ny_visit_stmt(v, e->as.match.default_conseq);
      break;
    case NY_E_ASM:
      for (size_t i = 0; i < e->as.as_asm.args.len; i++) {
        ny_visit_expr(v, e->as.as_asm.args.data[i]);
      }
      break;
    case NY_E_LAMBDA:
    case NY_E_FN:
      // Note: We don't automatically traverse into lambda bodies
      // because they are distinct scopes/functions.
      // Callers can handle this in pre/post if needed.
      break;
    case NY_E_IDENT:
    case NY_E_LITERAL:
    case NY_E_INFERRED_MEMBER:
    case NY_E_EMBED:
      break;
    }
  }

  if (v->visit_expr_post) {
    v->visit_expr_post(v, e);
  }
}

void ny_visit_stmt(ny_visitor_t *v, stmt_t *s) {
  if (!v || !s)
    return;

  bool traverse = true;
  if (v->visit_stmt_pre) {
    traverse = v->visit_stmt_pre(v, s);
  }

  if (traverse) {
    switch (s->kind) {
    case NY_S_BLOCK:
      for (size_t i = 0; i < s->as.block.body.len; i++) {
        ny_visit_stmt(v, s->as.block.body.data[i]);
      }
      break;
    case NY_S_VAR:
      for (size_t i = 0; i < s->as.var.exprs.len; i++) {
        ny_visit_expr(v, s->as.var.exprs.data[i]);
      }
      break;
    case NY_S_EXPR:
      ny_visit_expr(v, s->as.expr.expr);
      break;
    case NY_S_RETURN:
      ny_visit_expr(v, s->as.ret.value);
      break;
    case NY_S_LINK:
      break;
    case NY_S_IF:
      ny_visit_expr(v, s->as.iff.test);
      ny_visit_stmt(v, s->as.iff.conseq);
      ny_visit_stmt(v, s->as.iff.alt);
      break;
    case NY_S_WHILE:
      ny_visit_expr(v, s->as.whl.test);
      ny_visit_stmt(v, s->as.whl.body);
      if (s->as.whl.update)
        ny_visit_stmt(v, s->as.whl.update);
      if (s->as.whl.init)
        ny_visit_stmt(v, s->as.whl.init);
      break;
    case NY_S_FOR:
      ny_visit_expr(v, s->as.fr.iterable);
      ny_visit_stmt(v, s->as.fr.body);
      break;
    case NY_S_MATCH:
      ny_visit_expr(v, s->as.match.test);
      for (size_t i = 0; i < s->as.match.arms.len; i++) {
        match_arm_t *arm = &s->as.match.arms.data[i];
        for (size_t j = 0; j < arm->patterns.len; j++) {
          ny_visit_expr(v, arm->patterns.data[j]);
        }
        ny_visit_expr(v, arm->guard);
        ny_visit_stmt(v, arm->conseq);
      }
      ny_visit_stmt(v, s->as.match.default_conseq);
      break;
    case NY_S_TRY:
      ny_visit_stmt(v, s->as.tr.body);
      ny_visit_stmt(v, s->as.tr.handler);
      break;
    case NY_S_DEFER:
      ny_visit_stmt(v, s->as.de.body);
      break;
    case NY_S_MACRO:
      for (size_t i = 0; i < s->as.macro.args.len; i++) {
        ny_visit_expr(v, s->as.macro.args.data[i]);
      }
      ny_visit_stmt(v, s->as.macro.body);
      break;
    case NY_S_FUNC:
      ny_visit_stmt(v, s->as.fn.body);
      break;
    case NY_S_USE:
    case NY_S_EXTERN:
    case NY_S_LABEL:
    case NY_S_GOTO:
    case NY_S_BREAK:
    case NY_S_CONTINUE:
    case NY_S_LAYOUT:
    case NY_S_MODULE:
    case NY_S_EXPORT:
    case NY_S_STRUCT:
    case NY_S_ENUM:
    case NY_S_INCLUDE:
      break;
    }
  }

  if (v->visit_stmt_post) {
    v->visit_stmt_post(v, s);
  }
}
