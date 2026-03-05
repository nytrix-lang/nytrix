#include "typeinfer.h"
#include "ast/ast.h"
#include "base/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
void typeinfer_walk_stmt(typeinfer_ctx_t *ctx, stmt_t *s);
static void typeinfer_walk_expr(typeinfer_ctx_t *ctx, expr_t *e);

/* Initialize type inference context */
void typeinfer_ctx_init(typeinfer_ctx_t *ctx, size_t max_vars, scope *scopes,
                        codegen_t *cg) {
  if (!ctx)
    return;
  memset(ctx, 0, sizeof(*ctx));
  ctx->var_names_cap = max_vars;
  ctx->var_names_len = 0;
  ctx->var_names = max_vars > 0
                       ? (const char **)calloc(max_vars, sizeof(const char *))
                       : NULL;
  ctx->is_i64_proven =
      max_vars > 0 ? (bool *)calloc(max_vars, sizeof(bool)) : NULL;
  ctx->is_f64_proven =
      max_vars > 0 ? (bool *)calloc(max_vars, sizeof(bool)) : NULL;
  ctx->is_used_in_dynamic =
      max_vars > 0 ? (bool *)calloc(max_vars, sizeof(bool)) : NULL;
  ctx->scopes = scopes;
  ctx->func_depth = 1;
  ctx->cg = cg;
}

/* Dispose type inference context */
void typeinfer_ctx_dispose(typeinfer_ctx_t *ctx) {
  if (!ctx)
    return;
  free((void *)ctx->var_names);
  free(ctx->is_i64_proven);
  free(ctx->is_f64_proven);
  free(ctx->is_used_in_dynamic);
  memset(ctx, 0, sizeof(*ctx));
}

/* Add a variable to the inference context */
void typeinfer_add_var(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name || !*name)
    return;
  for (size_t i = 0; i < ctx->var_names_len; i++) {
    if (ctx->var_names[i] && strcmp(ctx->var_names[i], name) == 0)
      return;
  }
  if (ctx->var_names_len >= ctx->var_names_cap)
    return;
  ctx->var_names[ctx->var_names_len++] = name;
}

/* Find variable index by name */
static int typeinfer_find_var(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name || !*name)
    return -1;
  for (size_t i = 0; i < ctx->var_names_len; i++) {
    if (ctx->var_names[i] && strcmp(ctx->var_names[i], name) == 0)
      return (int)i;
  }
  return -1;
}

/* Mark a variable as proven i64 */
void typeinfer_mark_i64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0) {
    ctx->is_i64_proven[idx] = true;
  }
}

/* Mark a variable as proven f64 */
void typeinfer_mark_f64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0) {
    ctx->is_f64_proven[idx] = true;
  }
}

/* Mark a variable as used in dynamic context (needs tags) */
void typeinfer_mark_dynamic(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0) {
    ctx->is_used_in_dynamic[idx] = true;
  }
}

/* Check if a variable is proven i64 */
bool typeinfer_is_i64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return false;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return false;
  return ctx->is_i64_proven[idx] && !ctx->is_used_in_dynamic[idx];
}

/* Check if a variable is proven f64 */
bool typeinfer_is_f64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return false;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return false;
  return ctx->is_f64_proven[idx] && !ctx->is_used_in_dynamic[idx];
}

/* Check if a variable needs dynamic tagging */
bool typeinfer_needs_dynamic(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return false;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return false;
  return ctx->is_used_in_dynamic[idx];
}

/* Check if expression is an integer literal */
static bool expr_is_int_lit(expr_t *e) {
  if (!e)
    return false;
  return e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT;
}

/* Check if expression is a float literal */
static bool expr_is_float_lit(expr_t *e) {
  if (!e)
    return false;
  return e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_FLOAT;
}

/* Quick check: is this expression provably i64? */
bool typeinfer_expr_is_i64(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return false;

  if (expr_is_int_lit(e))
    return true;

  if (e->kind == NY_E_IDENT)
    return typeinfer_is_i64(ctx, e->as.ident.name);

  if (e->kind == NY_E_BINARY) {
    const char *op = e->as.binary.op;
    bool is_int_op =
        (op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/' ||
         op[0] == '%' || op[0] == '&' || op[0] == '|' || op[0] == '^' ||
         op[0] == '<' || op[0] == '>');
    if (is_int_op) {
      return typeinfer_expr_is_i64(ctx, e->as.binary.left) &&
             typeinfer_expr_is_i64(ctx, e->as.binary.right);
    }
  }

  if (e->kind == NY_E_UNARY) {
    if (e->as.unary.op[0] == '-' && !e->as.unary.op[1])
      return typeinfer_expr_is_i64(ctx, e->as.unary.right);
  }

  return false;
}

/* Quick check: is this expression provably f64? */
bool typeinfer_expr_is_f64(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return false;

  if (expr_is_float_lit(e))
    return true;

  if (e->kind == NY_E_IDENT)
    return typeinfer_is_f64(ctx, e->as.ident.name);

  if (e->kind == NY_E_BINARY) {
    const char *op = e->as.binary.op;
    bool is_float_op =
        (op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/');
    if (is_float_op) {
      return typeinfer_expr_is_f64(ctx, e->as.binary.left) ||
             typeinfer_expr_is_f64(ctx, e->as.binary.right);
    }
  }

  if (e->kind == NY_E_UNARY) {
    if (e->as.unary.op[0] == '-' && !e->as.unary.op[1])
      return typeinfer_expr_is_f64(ctx, e->as.unary.right);
  }

  return false;
}

/* Walk an expression for type inference */
static void typeinfer_walk_expr(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return;

  switch (e->kind) {
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    typeinfer_add_var(ctx, name);
    break;
  }

  case NY_E_LITERAL:
    break;

  case NY_E_UNARY: {
    if (e->as.unary.right)
      typeinfer_walk_expr(ctx, e->as.unary.right);
    break;
  }

  case NY_E_BINARY: {
    if (e->as.binary.left)
      typeinfer_walk_expr(ctx, e->as.binary.left);
    if (e->as.binary.right)
      typeinfer_walk_expr(ctx, e->as.binary.right);
    break;
  }

  case NY_E_CALL: {
    if (e->as.call.callee)
      typeinfer_walk_expr(ctx, e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (e->as.call.args.data[i].val)
        typeinfer_walk_expr(ctx, e->as.call.args.data[i].val);
    }
    break;
  }

  case NY_E_INDEX: {
    if (e->as.index.target)
      typeinfer_walk_expr(ctx, e->as.index.target);
    if (e->as.index.start)
      typeinfer_walk_expr(ctx, e->as.index.start);
    break;
  }

  case NY_E_MEMBER: {
    if (e->as.member.target)
      typeinfer_walk_expr(ctx, e->as.member.target);
    break;
  }

  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (e->as.list_like.data[i])
        typeinfer_walk_expr(ctx, e->as.list_like.data[i]);
    }
    break;
  }

  case NY_E_DICT: {
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      dict_pair_t *pair = &e->as.dict.pairs.data[i];
      if (pair->key)
        typeinfer_walk_expr(ctx, pair->key);
      if (pair->value)
        typeinfer_walk_expr(ctx, pair->value);
    }
    break;
  }

  case NY_E_LAMBDA:
  case NY_E_FN:
  case NY_E_MATCH:
  case NY_E_FSTRING:
  case NY_E_TRY:
  case NY_E_COMPTIME:
  case NY_E_EMBED:
  case NY_E_ASM:
  case NY_E_PTR_TYPE:
  case NY_E_DEREF:
  case NY_E_SIZEOF:
  case NY_E_INFERRED_MEMBER:
  case NY_E_LOGICAL:
  case NY_E_TERNARY:
  case NY_E_MEMCALL:
    /* Complex or dynamic expressions - skip for basic i64 inference */
    break;
  }
}

/* Walk a statement for type inference */
void typeinfer_walk_stmt(typeinfer_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s)
    return;

  switch (s->kind) {
  case NY_S_VAR: {
    /* Variable declaration - add to context and infer from initializer */
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *name = s->as.var.names.data[i];
      expr_t *init = (i < s->as.var.exprs.len) ? s->as.var.exprs.data[i] : NULL;

      typeinfer_add_var(ctx, name);

      /* Check if explicitly typed as int */
      const char *vartype =
          (i < s->as.var.types.len) ? s->as.var.types.data[i] : NULL;
      if (vartype && strcmp(vartype, "int") == 0) {
        typeinfer_mark_i64(ctx, name);
      } else if (vartype && strcmp(vartype, "f64") == 0) {
        typeinfer_mark_f64(ctx, name);
      }

      if (init) {
        typeinfer_walk_expr(ctx, init);

        /* Infer from initializer */
        if (expr_is_int_lit(init)) {
          typeinfer_mark_i64(ctx, name);
        } else if (expr_is_float_lit(init)) {
          typeinfer_mark_f64(ctx, name);
        } else if (init->kind == NY_E_IDENT) {
          /* Copy type from source variable */
          if (typeinfer_is_i64(ctx, init->as.ident.name))
            typeinfer_mark_i64(ctx, name);
          else if (typeinfer_is_f64(ctx, init->as.ident.name))
            typeinfer_mark_f64(ctx, name);
        } else if (init->kind == NY_E_BINARY) {
          /* Binary op: if both operands are same type, result is same type */
          const char *op = init->as.binary.op;
          bool is_arith =
              (op[0] == '+' || op[0] == '-' || op[0] == '*' || op[0] == '/');
          if (is_arith) {
            /* Check for f64 */
            bool left_f64 =
                init->as.binary.left->kind == NY_E_IDENT &&
                typeinfer_is_f64(ctx, init->as.binary.left->as.ident.name);
            bool right_f64 =
                init->as.binary.right->kind == NY_E_IDENT &&
                typeinfer_is_f64(ctx, init->as.binary.right->as.ident.name);
            if (left_f64 && right_f64)
              typeinfer_mark_f64(ctx, name);

            /* Check for i64 */
            bool left_i64 =
                init->as.binary.left->kind == NY_E_IDENT &&
                typeinfer_is_i64(ctx, init->as.binary.left->as.ident.name);
            bool right_i64 =
                init->as.binary.right->kind == NY_E_IDENT &&
                typeinfer_is_i64(ctx, init->as.binary.right->as.ident.name);
            if (left_i64 && right_i64)
              typeinfer_mark_i64(ctx, name);
          }
        }
      }
    }
    break;
  }

  case NY_S_EXPR: {
    typeinfer_walk_expr(ctx, s->as.expr.expr);
    break;
  }

  case NY_S_IF: {
    typeinfer_walk_expr(ctx, s->as.iff.test);
    if (s->as.iff.conseq)
      typeinfer_walk_stmt(ctx, s->as.iff.conseq);
    if (s->as.iff.alt)
      typeinfer_walk_stmt(ctx, s->as.iff.alt);
    break;
  }

  case NY_S_WHILE: {
    typeinfer_walk_expr(ctx, s->as.whl.test);
    if (s->as.whl.init)
      typeinfer_walk_stmt(ctx, s->as.whl.init);
    typeinfer_walk_stmt(ctx, s->as.whl.body);
    if (s->as.whl.update)
      typeinfer_walk_stmt(ctx, s->as.whl.update);
    break;
  }

  case NY_S_FOR: {
    typeinfer_walk_expr(ctx, s->as.fr.iterable);
    typeinfer_walk_stmt(ctx, s->as.fr.body);
    break;
  }

  case NY_S_RETURN: {
    if (s->as.ret.value)
      typeinfer_walk_expr(ctx, s->as.ret.value);
    break;
  }

  case NY_S_FUNC: {
    /* Nested function - recurse with fresh context or skip for now */
    if (s->as.fn.body)
      typeinfer_walk_stmt(ctx, s->as.fn.body);
    break;
  }

  case NY_S_EXTERN:
  case NY_S_ENUM:
  case NY_S_STRUCT:
  case NY_S_LAYOUT:
  case NY_S_USE:
  case NY_S_MODULE:
  case NY_S_EXPORT:
  case NY_S_MACRO:
  case NY_S_INCLUDE:
  case NY_S_LINK:
  case NY_S_LABEL:
  case NY_S_GOTO:
  case NY_S_DEFER:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
  case NY_S_TRY:
  case NY_S_MATCH:
    /* These don't affect local variable type inference */
    break;

  default:
    break;
  }
}

/* Run type inference on a function body */
void typeinfer_func_body(typeinfer_ctx_t *ctx, stmt_t *body) {
  if (!ctx || !body)
    return;

  /* Multiple passes for propagation */
  for (int pass = 0; pass < 3; pass++) {
    typeinfer_walk_stmt(ctx, body);
  }
}

/* Apply inferred types to scope bindings - applies to ALL scopes up to depth */
void typeinfer_apply_to_scopes(typeinfer_ctx_t *ctx, scope *scopes,
                               size_t depth) {
  if (!ctx || !scopes || depth == 0)
    return;

  /* Apply to all scopes from 0 to depth-1 */
  for (size_t d = 0; d < depth; d++) {
    scope *s = &scopes[d];
    for (size_t i = 0; i < s->vars.len; i++) {
      binding *b = &s->vars.data[i];
      if (b->name) {
        if (typeinfer_is_i64(ctx, b->name)) {
          b->is_int_slot = true;
          b->is_int_direct = !b->is_slot;
          b->is_f64_slot = false;
          b->is_f64_direct = false;
        } else if (typeinfer_is_f64(ctx, b->name)) {
          b->is_f64_slot = true;
          b->is_f64_direct = !b->is_slot;
          b->is_int_slot = false;
          b->is_int_direct = false;
        }

        /* Mark variables used in dynamic contexts */
        if (typeinfer_needs_dynamic(ctx, b->name)) {
          b->is_int_slot = false;
          b->is_f64_slot = false;
          b->is_int_direct = false;
          b->is_f64_direct = false;
        }
      }
    }
  }
}
