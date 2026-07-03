#include "parse/ast.h"
#include "base/util.h"
#include "rt/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

expr_t *expr_new(arena_t *arena, expr_kind_t kind, token_t tok) {
  if (!arena) {
    NY_LOG_DEBUG("expr_new called with NULL arena!\n");
  }
  expr_t *e = NULL;
  if (arena && arena->expr_pool_left) {
    e = (expr_t *)arena->expr_pool;
    arena->expr_pool = (char *)arena->expr_pool + sizeof(expr_t);
    arena->expr_pool_left--;
  } else if (arena) {
#ifndef NY_EXPR_POOL_SIZE
#define NY_EXPR_POOL_SIZE 256
#endif
    expr_t *pool = (expr_t *)arena_alloc(arena, sizeof(expr_t) * NY_EXPR_POOL_SIZE);
    arena->expr_pool = (char *)pool + sizeof(expr_t);
    arena->expr_pool_left = NY_EXPR_POOL_SIZE - 1;
    e = pool;
  } else {
    e = (expr_t *)arena_alloc(arena, sizeof(expr_t));
  }
  e->kind = kind;
  e->tok = tok;
  return e;
}

stmt_t *stmt_new(arena_t *arena, stmt_kind_t kind, token_t tok) {
  if (!arena) {
    NY_LOG_DEBUG("stmt_new called with NULL arena!\n");
  }
  stmt_t *s = NULL;
  if (arena && arena->stmt_pool_left) {
    s = (stmt_t *)arena->stmt_pool;
    arena->stmt_pool = (char *)arena->stmt_pool + sizeof(stmt_t);
    arena->stmt_pool_left--;
  } else if (arena) {
#ifndef NY_STMT_POOL_SIZE
#define NY_STMT_POOL_SIZE 128
#endif
    stmt_t *pool = (stmt_t *)arena_alloc(arena, sizeof(stmt_t) * NY_STMT_POOL_SIZE);
    arena->stmt_pool = (char *)pool + sizeof(stmt_t);
    arena->stmt_pool_left = NY_STMT_POOL_SIZE - 1;
    s = pool;
  } else {
    s = (stmt_t *)arena_alloc(arena, sizeof(stmt_t));
  }
  s->kind = kind;
  s->tok = tok;
  s->sema = NULL;
  s->sema_kind = NY_STMT_SEMA_NONE;
  return s;
}

void expr_free_members(expr_t *e) { (void)e; }
void stmt_free_members(stmt_t *s) { (void)s; }

void program_free(program_t *prog, arena_t *arena) {
  (void)prog;
  arena_free(arena);
  free(arena);
}

static void ny_ast_verify_expr(expr_t *e, const char *phase);
static void ny_ast_verify_stmt(stmt_t *s, const char *phase);

static void ny_ast_require(bool cond, const char *phase, token_t tok, const char *fmt, ...) {
  if (cond || !ny_compiler_asserts_enabled())
    return;
  char detail[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  ny_compiler_assert_fail(__FILE__, __LINE__, __func__, "ast invariant",
                          "%s:%d:%d ast[%s]: %s",
                          tok.filename ? tok.filename : "<unknown>", tok.line, tok.col,
                          phase ? phase : "unknown", detail);
}

static void ny_ast_verify_vec_storage(const void *data, size_t len, size_t cap,
                                      const char *phase, token_t tok,
                                      const char *what) {
  ny_ast_require(len <= cap, phase, tok, "%s vector len=%zu exceeds cap=%zu",
                 what ? what : "unknown", len, cap);
  ny_ast_require(data != NULL || len == 0, phase, tok, "%s vector has len=%zu but null data",
                 what ? what : "unknown", len);
  ny_ast_require(data != NULL || cap == 0, phase, tok, "%s vector has cap=%zu but null data",
                 what ? what : "unknown", cap);
}

static void ny_ast_verify_expr_list(ny_expr_list *items, const char *phase, token_t tok) {
  if (!items)
    return;
  ny_ast_verify_vec_storage(items->data, items->len, items->cap, phase, tok, "expr list");
  for (size_t i = 0; i < items->len; ++i) {
    ny_ast_require(items->data[i] != NULL, phase, tok, "null expr at index %zu", i);
    ny_ast_verify_expr(items->data[i], phase);
  }
}

static void ny_ast_verify_call_args(ny_call_arg_list *args, const char *phase, token_t tok) {
  if (!args)
    return;
  ny_ast_verify_vec_storage(args->data, args->len, args->cap, phase, tok, "call args");
  for (size_t i = 0; i < args->len; ++i) {
    if (args->data[i].name)
      ny_ast_require(*args->data[i].name, phase, tok, "call arg %zu has empty name", i);
    ny_ast_require(args->data[i].val != NULL, phase, tok, "null call arg at index %zu", i);
    ny_ast_verify_expr(args->data[i].val, phase);
  }
}

static void ny_ast_verify_layout_fields(ny_layout_field_list *fields, const char *phase, token_t tok) {
  if (!fields)
    return;
  ny_ast_verify_vec_storage(fields->data, fields->len, fields->cap, phase, tok, "layout fields");
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    ny_ast_require(f->name && *f->name, phase, tok, "layout field %zu missing name", i);
    ny_ast_require(f->type_name && *f->type_name, phase, tok, "layout field %zu missing type", i);
    if (f->default_value)
      ny_ast_verify_expr(f->default_value, phase);
  }
}

static void ny_ast_verify_params(ny_param_list *params, const char *phase, token_t tok,
                                 const char *what) {
  if (!params)
    return;
  ny_ast_verify_vec_storage(params->data, params->len, params->cap, phase, tok, what);
  for (size_t i = 0; i < params->len; ++i) {
    ny_ast_require(params->data[i].name && *params->data[i].name, phase, tok,
                   "%s param %zu missing name", what ? what : "callable", i);
    if (params->data[i].type)
      ny_ast_require(*params->data[i].type, phase, tok, "%s param %zu has empty type",
                     what ? what : "callable", i);
    if (params->data[i].def)
      ny_ast_verify_expr(params->data[i].def, phase);
  }
}

static void ny_ast_verify_type_params(ny_type_param_list *params, const char *phase,
                                      token_t tok, const char *what) {
  if (!params)
    return;
  ny_ast_verify_vec_storage(params->data, params->len, params->cap, phase, tok, what);
  for (size_t i = 0; i < params->len; ++i)
    ny_ast_require(params->data[i] && *params->data[i], phase, tok,
                   "%s type param %zu missing name", what ? what : "node", i);
}

static void ny_ast_verify_use_items(ny_use_item_list *items, const char *phase, token_t tok) {
  if (!items)
    return;
  ny_ast_verify_vec_storage(items->data, items->len, items->cap, phase, tok, "use imports");
  for (size_t i = 0; i < items->len; ++i) {
    ny_ast_require(items->data[i].name && *items->data[i].name, phase, tok,
                   "use import %zu missing name", i);
    if (items->data[i].alias)
      ny_ast_require(*items->data[i].alias, phase, tok, "use import %zu has empty alias", i);
  }
}

static void ny_ast_verify_export_names(stmt_export_t *exprt, const char *phase, token_t tok) {
  if (!exprt)
    return;
  ny_ast_verify_vec_storage(exprt->names.data, exprt->names.len, exprt->names.cap,
                            phase, tok, "export names");
  if (exprt->profile)
    ny_ast_require(*exprt->profile, phase, tok, "export profile is empty");
  for (size_t i = 0; i < exprt->names.len; ++i)
    ny_ast_require(exprt->names.data[i] && *exprt->names.data[i], phase, tok,
                   "export name %zu missing", i);
}

static void ny_ast_verify_enum_fields(ny_enum_field_list *fields, const char *phase,
                                      token_t tok) {
  if (!fields)
    return;
  ny_ast_verify_vec_storage(fields->data, fields->len, fields->cap, phase, tok, "enum fields");
  for (size_t i = 0; i < fields->len; ++i) {
    ny_ast_require(fields->data[i].name && *fields->data[i].name, phase, tok,
                   "enum field %zu missing name", i);
    ny_ast_require(fields->data[i].type_name && *fields->data[i].type_name, phase, tok,
                   "enum field %zu missing type", i);
  }
}

static void ny_ast_verify_expr(expr_t *e, const char *phase) {
  ny_ast_require(e != NULL, phase, (token_t){0}, "null expression");
  if (!e)
    return;
  ny_ast_require(e->kind >= NY_E_IDENT && e->kind <= NY_E_TRY, phase, e->tok,
                 "expr kind out of range: %d", (int)e->kind);
  switch (e->kind) {
  case NY_E_IDENT:
    ny_ast_require(e->as.ident.name && *e->as.ident.name, phase, e->tok, "ident missing name");
    return;
  case NY_E_LITERAL:
    ny_ast_require(e->as.literal.kind >= NY_LIT_INT && e->as.literal.kind <= NY_LIT_STR,
                   phase, e->tok, "literal kind out of range: %d", (int)e->as.literal.kind);
    ny_ast_require(e->as.literal.hint >= NY_LIT_HINT_NONE &&
                       e->as.literal.hint <= NY_LIT_HINT_F128,
                   phase, e->tok, "literal hint out of range: %d", (int)e->as.literal.hint);
    if (e->as.literal.kind == NY_LIT_STR)
      ny_ast_require(e->as.literal.as.s.data != NULL || e->as.literal.as.s.len == 0, phase, e->tok,
                     "string literal missing data");
    return;
  case NY_E_UNARY:
    ny_ast_require(e->as.unary.op && *e->as.unary.op, phase, e->tok, "unary missing op");
    ny_ast_require(e->as.unary.right != NULL, phase, e->tok, "unary missing operand");
    ny_ast_verify_expr(e->as.unary.right, phase);
    return;
  case NY_E_BINARY:
    ny_ast_require(e->as.binary.op && *e->as.binary.op, phase, e->tok, "binary missing op");
    ny_ast_require(e->as.binary.left != NULL && e->as.binary.right != NULL, phase, e->tok,
                   "binary missing operand");
    ny_ast_verify_expr(e->as.binary.left, phase);
    ny_ast_verify_expr(e->as.binary.right, phase);
    return;
  case NY_E_LOGICAL:
    ny_ast_require(e->as.logical.op && *e->as.logical.op, phase, e->tok, "logical missing op");
    ny_ast_require(e->as.logical.left != NULL && e->as.logical.right != NULL, phase, e->tok,
                   "logical missing operand");
    ny_ast_verify_expr(e->as.logical.left, phase);
    ny_ast_verify_expr(e->as.logical.right, phase);
    return;
  case NY_E_TERNARY:
    ny_ast_require(e->as.ternary.cond && e->as.ternary.true_expr && e->as.ternary.false_expr, phase,
                   e->tok, "ternary missing arm");
    ny_ast_verify_expr(e->as.ternary.cond, phase);
    ny_ast_verify_expr(e->as.ternary.true_expr, phase);
    ny_ast_verify_expr(e->as.ternary.false_expr, phase);
    return;
  case NY_E_CALL:
    ny_ast_require(e->as.call.callee != NULL, phase, e->tok, "call missing callee");
    ny_ast_verify_expr(e->as.call.callee, phase);
    ny_ast_verify_call_args(&e->as.call.args, phase, e->tok);
    return;
  case NY_E_MEMCALL:
    ny_ast_require(e->as.memcall.target != NULL, phase, e->tok, "memcall missing target");
    ny_ast_require(e->as.memcall.name && *e->as.memcall.name, phase, e->tok, "memcall missing name");
    ny_ast_verify_expr(e->as.memcall.target, phase);
    ny_ast_verify_call_args(&e->as.memcall.args, phase, e->tok);
    return;
  case NY_E_INDEX:
    ny_ast_require(e->as.index.target != NULL, phase, e->tok, "index missing target");
    ny_ast_verify_expr(e->as.index.target, phase);
    if (e->as.index.start)
      ny_ast_verify_expr(e->as.index.start, phase);
    if (e->as.index.stop)
      ny_ast_verify_expr(e->as.index.stop, phase);
    if (e->as.index.step)
      ny_ast_verify_expr(e->as.index.step, phase);
    return;
  case NY_E_LAMBDA:
  case NY_E_FN:
    ny_ast_require(e->as.lambda.body != NULL, phase, e->tok, "lambda missing body");
    if (e->as.lambda.return_type)
      ny_ast_require(*e->as.lambda.return_type, phase, e->tok, "lambda has empty return type");
    ny_ast_verify_params(&e->as.lambda.params, phase, e->tok, "lambda");
    ny_ast_verify_stmt(e->as.lambda.body, phase);
    return;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    ny_ast_verify_expr_list(&e->as.list_like, phase, e->tok);
    return;
  case NY_E_DICT:
    ny_ast_verify_vec_storage(e->as.dict.pairs.data, e->as.dict.pairs.len,
                              e->as.dict.pairs.cap, phase, e->tok, "dict pairs");
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      ny_ast_require(e->as.dict.pairs.data[i].key && e->as.dict.pairs.data[i].value, phase, e->tok,
                     "dict pair %zu missing key/value", i);
      ny_ast_verify_expr(e->as.dict.pairs.data[i].key, phase);
      ny_ast_verify_expr(e->as.dict.pairs.data[i].value, phase);
    }
    return;
  case NY_E_ASM:
    ny_ast_require(e->as.as_asm.code != NULL, phase, e->tok, "asm missing code");
    ny_ast_verify_expr_list(&e->as.as_asm.args, phase, e->tok);
    return;
  case NY_E_COMPTIME:
    ny_ast_require(e->as.comptime_expr.body != NULL, phase, e->tok, "comptime missing body");
    ny_ast_verify_stmt(e->as.comptime_expr.body, phase);
    return;
  case NY_E_FSTRING:
    ny_ast_verify_vec_storage(e->as.fstring.parts.data, e->as.fstring.parts.len,
                              e->as.fstring.parts.cap, phase, e->tok, "fstring parts");
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      ny_ast_require(part->kind == NY_FSP_STR || part->kind == NY_FSP_EXPR, phase, e->tok,
                     "fstring part %zu kind out of range: %d", i, (int)part->kind);
      if (part->kind == NY_FSP_EXPR) {
        ny_ast_require(part->as.e != NULL, phase, e->tok, "fstring expr part %zu missing expr", i);
        ny_ast_verify_expr(part->as.e, phase);
      } else {
        ny_ast_require(part->as.s.data != NULL || part->as.s.len == 0, phase, e->tok,
                       "fstring str part %zu missing data", i);
      }
    }
    return;
  case NY_E_INFERRED_MEMBER:
    ny_ast_require(e->as.inferred_member.name && *e->as.inferred_member.name, phase, e->tok,
                   "inferred member missing name");
    return;
  case NY_E_EMBED:
    ny_ast_require(e->as.embed.path && *e->as.embed.path, phase, e->tok, "embed missing path");
    return;
  case NY_E_MATCH:
    ny_ast_require(e->as.match.test != NULL, phase, e->tok, "match missing test");
    ny_ast_verify_expr(e->as.match.test, phase);
    ny_ast_verify_vec_storage(e->as.match.arms.data, e->as.match.arms.len,
                              e->as.match.arms.cap, phase, e->tok, "match arms");
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      ny_ast_require(arm->patterns.len > 0, phase, e->tok, "match arm %zu missing patterns", i);
      ny_ast_require(arm->conseq != NULL, phase, e->tok, "match arm %zu missing body", i);
      ny_ast_verify_vec_storage(arm->patterns.data, arm->patterns.len, arm->patterns.cap,
                                phase, e->tok, "match patterns");
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        ny_ast_require(arm->patterns.data[j] != NULL, phase, e->tok,
                       "match arm %zu pattern %zu null", i, j);
        ny_ast_verify_expr(arm->patterns.data[j], phase);
      }
      if (arm->guard)
        ny_ast_verify_expr(arm->guard, phase);
      ny_ast_verify_stmt(arm->conseq, phase);
    }
    if (e->as.match.default_conseq)
      ny_ast_verify_stmt(e->as.match.default_conseq, phase);
    return;
  case NY_E_MEMBER:
    ny_ast_require(e->as.member.target && e->as.member.name && *e->as.member.name, phase, e->tok,
                   "member missing target/name");
    ny_ast_verify_expr(e->as.member.target, phase);
    return;
  case NY_E_PTR_TYPE:
    ny_ast_require(e->as.ptr_type.target != NULL, phase, e->tok, "ptr type missing target");
    ny_ast_verify_expr(e->as.ptr_type.target, phase);
    return;
  case NY_E_DEREF:
    ny_ast_require(e->as.deref.target != NULL, phase, e->tok, "deref missing target");
    ny_ast_verify_expr(e->as.deref.target, phase);
    return;
  case NY_E_SIZEOF:
    ny_ast_require((e->as.szof.is_type && e->as.szof.type_name) ||
                       (!e->as.szof.is_type && e->as.szof.target != NULL),
                   phase, e->tok, "sizeof missing target/type");
    if (e->as.szof.is_type)
      ny_ast_require(*e->as.szof.type_name, phase, e->tok, "sizeof type is empty");
    if (!e->as.szof.is_type)
      ny_ast_verify_expr(e->as.szof.target, phase);
    return;
  case NY_E_TRY:
    ny_ast_require(e->as.try_expr.target != NULL, phase, e->tok, "try missing target");
    ny_ast_verify_expr(e->as.try_expr.target, phase);
    return;
  }
}

static void ny_ast_verify_stmt_list(ny_stmt_list *body, const char *phase, token_t tok) {
  if (!body)
    return;
  ny_ast_verify_vec_storage(body->data, body->len, body->cap, phase, tok, "stmt list");
  for (size_t i = 0; i < body->len; ++i) {
    ny_ast_require(body->data[i] != NULL, phase, tok, "null stmt at index %zu", i);
    ny_ast_verify_stmt(body->data[i], phase);
  }
}

static void ny_ast_verify_stmt(stmt_t *s, const char *phase) {
  ny_ast_require(s != NULL, phase, (token_t){0}, "null statement");
  if (!s)
    return;
  ny_ast_require(s->kind >= NY_S_BLOCK && s->kind <= NY_S_IMPL, phase, s->tok,
                 "stmt kind out of range: %d", (int)s->kind);
  ny_ast_verify_vec_storage(s->attributes.data, s->attributes.len, s->attributes.cap,
                            phase, s->tok, "attributes");
  for (size_t i = 0; i < s->attributes.len; ++i) {
    attribute_t *attr = &s->attributes.data[i];
    ny_ast_require(attr->name && *attr->name, phase, attr->tok, "attribute %zu missing name", i);
    ny_ast_verify_expr_list(&attr->args, phase, attr->tok);
  }

  switch (s->kind) {
  case NY_S_BLOCK:
    ny_ast_verify_stmt_list(&s->as.block.body, phase, s->tok);
    return;
  case NY_S_USE:
    ny_ast_require(s->as.use.module && *s->as.use.module, phase, s->tok, "use missing module");
    if (s->as.use.alias)
      ny_ast_require(*s->as.use.alias, phase, s->tok, "use alias is empty");
    if (s->as.use.profile)
      ny_ast_require(*s->as.use.profile, phase, s->tok, "use profile is empty");
    ny_ast_verify_use_items(&s->as.use.imports, phase, s->tok);
    ny_ast_require(!s->as.use.import_all || s->as.use.imports.len == 0, phase, s->tok,
                   "use '*' combined with import list");
    ny_ast_require(!s->as.use.alias || (!s->as.use.import_all && s->as.use.imports.len == 0),
                   phase, s->tok, "module alias combined with import list/star");
    return;
  case NY_S_VAR:
    ny_ast_verify_vec_storage(s->as.var.names.data, s->as.var.names.len,
                              s->as.var.names.cap, phase, s->tok, "var names");
    ny_ast_verify_vec_storage(s->as.var.types.data, s->as.var.types.len,
                              s->as.var.types.cap, phase, s->tok, "var types");
    ny_ast_verify_vec_storage(s->as.var.exprs.data, s->as.var.exprs.len,
                              s->as.var.exprs.cap, phase, s->tok, "var exprs");
    ny_ast_require(s->as.var.names.len > 0, phase, s->tok, "var missing names");
    ny_ast_require(s->as.var.types.len == 0 || s->as.var.types.len == 1 ||
                       s->as.var.types.len == s->as.var.names.len,
                   phase, s->tok, "var type arity mismatch names=%zu types=%zu",
                   s->as.var.names.len, s->as.var.types.len);
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      ny_ast_require(s->as.var.names.data[i] && *s->as.var.names.data[i], phase, s->tok,
                     "var binding %zu missing name", i);
      if (i < s->as.var.types.len)
        ny_ast_require(!s->as.var.types.data[i] || *s->as.var.types.data[i], phase, s->tok,
                       "var binding %zu has empty type", i);
    }
    ny_ast_verify_expr_list((ny_expr_list *)&s->as.var.exprs, phase, s->tok);
    return;
  case NY_S_EXPR:
    ny_ast_require(s->as.expr.expr != NULL, phase, s->tok, "expr stmt missing expr");
    ny_ast_verify_expr(s->as.expr.expr, phase);
    return;
  case NY_S_IF:
    ny_ast_require(s->as.iff.test && s->as.iff.conseq, phase, s->tok, "if missing test/body");
    ny_ast_verify_expr(s->as.iff.test, phase);
    ny_ast_verify_stmt(s->as.iff.conseq, phase);
    if (s->as.iff.alt)
      ny_ast_verify_stmt(s->as.iff.alt, phase);
    if (s->as.iff.init)
      ny_ast_verify_stmt(s->as.iff.init, phase);
    return;
  case NY_S_GUARD:
    ny_ast_require(s->as.guard.type_name && *s->as.guard.type_name, phase, s->tok,
                   "guard missing type");
    ny_ast_require(s->as.guard.name && *s->as.guard.name, phase, s->tok, "guard missing name");
    ny_ast_require(s->as.guard.value && s->as.guard.fallback, phase, s->tok,
                   "guard missing value/fallback");
    ny_ast_verify_expr(s->as.guard.value, phase);
    ny_ast_verify_stmt(s->as.guard.fallback, phase);
    return;
  case NY_S_WHILE:
    ny_ast_require(s->as.whl.test && s->as.whl.body, phase, s->tok, "while missing test/body");
    ny_ast_verify_expr(s->as.whl.test, phase);
    ny_ast_verify_stmt(s->as.whl.body, phase);
    if (s->as.whl.update)
      ny_ast_verify_stmt(s->as.whl.update, phase);
    if (s->as.whl.init)
      ny_ast_verify_stmt(s->as.whl.init, phase);
    return;
  case NY_S_FOR:
    ny_ast_require(s->as.fr.body != NULL, phase, s->tok, "for missing body");
    if (s->as.fr.iterable) {
      ny_ast_require(s->as.fr.iter_var && *s->as.fr.iter_var, phase, s->tok,
                     "iterator for missing iter var");
      ny_ast_require(!s->as.fr.init && !s->as.fr.cond && !s->as.fr.update, phase, s->tok,
                     "iterator for has c-style fields");
      if (s->as.fr.iter_index_var)
        ny_ast_require(*s->as.fr.iter_index_var, phase, s->tok,
                       "iterator for has empty index var");
      ny_ast_verify_expr(s->as.fr.iterable, phase);
    } else {
      ny_ast_require(!s->as.fr.iter_var, phase, s->tok, "header-style for has iter var");
      ny_ast_require(!s->as.fr.iter_index_var, phase, s->tok,
                     "header-style for has index var");
      ny_ast_require(s->as.fr.init && s->as.fr.cond, phase, s->tok,
                     "header-style for missing init/cond");
      ny_ast_verify_stmt(s->as.fr.init, phase);
      ny_ast_verify_expr(s->as.fr.cond, phase);
      if (s->as.fr.update)
        ny_ast_verify_stmt(s->as.fr.update, phase);
    }
    ny_ast_verify_stmt(s->as.fr.body, phase);
    return;
  case NY_S_TRY:
    ny_ast_require(s->as.tr.body && s->as.tr.handler, phase, s->tok, "try missing body/handler");
    if (s->as.tr.err)
      ny_ast_require(*s->as.tr.err, phase, s->tok, "try handler error name is empty");
    ny_ast_verify_stmt(s->as.tr.body, phase);
    ny_ast_verify_stmt(s->as.tr.handler, phase);
    return;
  case NY_S_FUNC:
    ny_ast_require(s->as.fn.name && *s->as.fn.name, phase, s->tok, "fn missing name");
    ny_ast_require(s->as.fn.body != NULL, phase, s->tok, "fn missing body");
    if (s->as.fn.return_type)
      ny_ast_require(*s->as.fn.return_type, phase, s->tok, "fn has empty return type");
    if (s->as.fn.link_name)
      ny_ast_require(*s->as.fn.link_name, phase, s->tok, "fn has empty link name");
    ny_ast_verify_params(&s->as.fn.params, phase, s->tok, "fn");
    ny_ast_verify_stmt(s->as.fn.body, phase);
    return;
  case NY_S_EXTERN:
    ny_ast_require(s->as.ext.name && *s->as.ext.name, phase, s->tok, "extern missing name");
    if (s->as.ext.return_type)
      ny_ast_require(*s->as.ext.return_type, phase, s->tok, "extern has empty return type");
    if (s->as.ext.link_name)
      ny_ast_require(*s->as.ext.link_name, phase, s->tok, "extern has empty link name");
    ny_ast_verify_params(&s->as.ext.params, phase, s->tok, "extern");
    return;
  case NY_S_LINK:
    ny_ast_require(s->as.link.lib && *s->as.link.lib, phase, s->tok, "link missing lib");
    return;
  case NY_S_RETURN:
    if (s->as.ret.value)
      ny_ast_verify_expr(s->as.ret.value, phase);
    return;
  case NY_S_LABEL:
    ny_ast_require(s->as.label.name && *s->as.label.name, phase, s->tok, "label missing name");
    return;
  case NY_S_DEFER:
    ny_ast_require(s->as.de.body != NULL, phase, s->tok, "defer missing body");
    ny_ast_verify_stmt(s->as.de.body, phase);
    return;
  case NY_S_GOTO:
    ny_ast_require(s->as.go.name && *s->as.go.name, phase, s->tok, "goto missing target");
    return;
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return;
  case NY_S_LAYOUT:
    ny_ast_require(s->as.layout.name && *s->as.layout.name, phase, s->tok, "layout missing name");
    ny_ast_verify_layout_fields(&s->as.layout.fields, phase, s->tok);
    ny_ast_verify_stmt_list(&s->as.layout.methods, phase, s->tok);
    return;
  case NY_S_MATCH:
    ny_ast_require(s->as.match.test != NULL, phase, s->tok, "match stmt missing test");
    ny_ast_verify_expr(s->as.match.test, phase);
    ny_ast_verify_vec_storage(s->as.match.arms.data, s->as.match.arms.len,
                              s->as.match.arms.cap, phase, s->tok, "match stmt arms");
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      ny_ast_require(arm->patterns.len > 0 && arm->conseq != NULL, phase, s->tok,
                     "match stmt arm %zu incomplete", i);
      ny_ast_verify_vec_storage(arm->patterns.data, arm->patterns.len, arm->patterns.cap,
                                phase, s->tok, "match stmt patterns");
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        ny_ast_require(arm->patterns.data[j] != NULL, phase, s->tok,
                       "match stmt arm %zu pattern %zu null", i, j);
        ny_ast_verify_expr(arm->patterns.data[j], phase);
      }
      if (arm->guard)
        ny_ast_verify_expr(arm->guard, phase);
      ny_ast_verify_stmt(arm->conseq, phase);
    }
    if (s->as.match.default_conseq)
      ny_ast_verify_stmt(s->as.match.default_conseq, phase);
    return;
  case NY_S_MODULE:
    ny_ast_require(s->as.module.name && *s->as.module.name, phase, s->tok, "module missing name");
    if (s->as.module.path)
      ny_ast_require(*s->as.module.path, phase, s->tok, "module has empty path");
    ny_ast_verify_stmt_list(&s->as.module.body, phase, s->tok);
    return;
  case NY_S_EXPORT:
    ny_ast_verify_export_names(&s->as.exprt, phase, s->tok);
    ny_ast_require(s->as.exprt.names.len > 0, phase, s->tok, "export missing names");
    return;
  case NY_S_STRUCT:
    ny_ast_require(s->as.struc.name && *s->as.struc.name, phase, s->tok, "struct missing name");
    ny_ast_verify_layout_fields(&s->as.struc.fields, phase, s->tok);
    ny_ast_verify_stmt_list(&s->as.struc.methods, phase, s->tok);
    return;
  case NY_S_ENUM:
    ny_ast_require(s->as.enu.name && *s->as.enu.name, phase, s->tok, "enum missing name");
    ny_ast_verify_type_params(&s->as.enu.type_params, phase, s->tok, "enum");
    ny_ast_verify_vec_storage(s->as.enu.items.data, s->as.enu.items.len,
                              s->as.enu.items.cap, phase, s->tok, "enum items");
    for (size_t i = 0; i < s->as.enu.items.len; ++i) {
      ny_ast_require(s->as.enu.items.data[i].name && *s->as.enu.items.data[i].name, phase, s->tok,
                     "enum item %zu missing name", i);
      ny_ast_verify_enum_fields(&s->as.enu.items.data[i].fields, phase, s->tok);
      if (s->as.enu.items.data[i].value)
        ny_ast_verify_expr(s->as.enu.items.data[i].value, phase);
    }
    return;
  case NY_S_MACRO:
    ny_ast_require(s->as.macro.name && *s->as.macro.name && s->as.macro.body, phase, s->tok,
                   "macro missing name/body");
    ny_ast_verify_expr_list(&s->as.macro.args, phase, s->tok);
    ny_ast_verify_stmt(s->as.macro.body, phase);
    return;
  case NY_S_INCLUDE:
    ny_ast_require(s->as.inc.path && *s->as.inc.path, phase, s->tok, "include missing path");
    return;
  case NY_S_DEFINE:
    ny_ast_require(s->as.def.name && *s->as.def.name, phase, s->tok, "define missing name");
    return;
  case NY_S_OPERATOR:
    ny_ast_require(s->as.oper.op && *s->as.oper.op, phase, s->tok, "operator missing op");
    ny_ast_require(s->as.oper.target && *s->as.oper.target, phase, s->tok, "operator missing target");
    if (s->as.oper.left_type)
      ny_ast_require(*s->as.oper.left_type, phase, s->tok, "operator has empty left type");
    if (s->as.oper.right_type)
      ny_ast_require(*s->as.oper.right_type, phase, s->tok, "operator has empty right type");
    if (s->as.oper.return_type)
      ny_ast_require(*s->as.oper.return_type, phase, s->tok, "operator has empty return type");
    return;
  case NY_S_IMPL:
    ny_ast_require(s->as.impl.type_name && *s->as.impl.type_name, phase, s->tok, "impl missing type");
    ny_ast_verify_stmt_list(&s->as.impl.methods, phase, s->tok);
    return;
  }
}

void ny_ast_verify_program(program_t *prog, const char *phase) {
  if (!ny_compiler_asserts_enabled())
    return;
  ny_ast_require(prog != NULL, phase, (token_t){0}, "null program");
  if (!prog)
    return;
  ny_ast_verify_vec_storage(prog->body.data, prog->body.len, prog->body.cap,
                            phase, (token_t){0}, "program body");
  for (size_t i = 0; i < prog->body.len; ++i) {
    ny_ast_require(prog->body.data[i] != NULL, phase, (token_t){0}, "null top-level stmt %zu", i);
    ny_ast_verify_stmt(prog->body.data[i], phase);
  }
}

#include "parse/json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct json_writer_t json_writer_t;
static void dump_expr(expr_t *e, json_writer_t *out);
static void dump_stmt(stmt_t *s, json_writer_t *out);
static size_t ny_src_line_count(const char *start, const char *end, size_t fallback);

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

static void append_json_str(char **buf, size_t *len, size_t *cap, const char *s) {
  append(buf, len, cap, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        append(buf, len, cap, "\\\"");
        break;
      case '\\':
        append(buf, len, cap, "\\\\");
        break;
      case '\n':
        append(buf, len, cap, "\\n");
        break;
      case '\r':
        append(buf, len, cap, "\\r");
        break;
      case '\t':
        append(buf, len, cap, "\\t");
        break;
      default:
        if (*p < 32)
          append(buf, len, cap, "\\u%04x", (unsigned)*p);
        else
          append(buf, len, cap, "%c", *p);
        break;
      }
    }
  }
  append(buf, len, cap, "\"");
}

typedef struct json_writer_t {
  char *buf;
  size_t len;
  size_t cap;
} json_writer_t;

static void json_writer_init(json_writer_t *j, size_t cap) {
  j->len = 0;
  j->cap = cap ? cap : 256;
  j->buf = (char *)(uintptr_t)rt_malloc(j->cap);
  if (!j->buf) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  j->buf[0] = '\0';
}

static char *json_writer_take(json_writer_t *j) { return j->buf; }

static void json_append(json_writer_t *j, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (j->len + (size_t)n + 1 > j->cap) {
    j->cap = (j->len + (size_t)n + 1) * 2;
    j->buf = (char *)(uintptr_t)rt_realloc((int64_t)(uintptr_t)j->buf, j->cap);
    if (!j->buf) {
      fprintf(stderr, "oom\n");
      exit(1);
    }
  }
  va_start(ap, fmt);
  vsnprintf(j->buf + j->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  j->len += (size_t)n;
}

static void json_string(json_writer_t *j, const char *s) {
  json_append(j, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        json_append(j, "\\\"");
        break;
      case '\\':
        json_append(j, "\\\\");
        break;
      case '\n':
        json_append(j, "\\n");
        break;
      case '\r':
        json_append(j, "\\r");
        break;
      case '\t':
        json_append(j, "\\t");
        break;
      default:
        if (*p < 32)
          json_append(j, "\\u%04x", (unsigned)*p);
        else
          json_append(j, "%c", *p);
        break;
      }
    }
  }
  json_append(j, "\"");
}

static void dump_params(ny_param_list *params, json_writer_t *out) {
  json_append(out, "[");
  for (size_t i = 0; params && i < params->len; ++i) {
    param_t *p = &params->data[i];
    json_append(out, "{\"name\":");
    json_string(out, p->name ? p->name : "");
    if (p->type) {
      json_append(out, ",\"type\":");
      json_string(out, p->type);
    }
    if (p->def) {
      json_append(out, ",\"default\":");
      dump_expr(p->def, out);
    }
    json_append(out, "}");
    if (i + 1 < params->len)
      json_append(out, ",");
  }
  json_append(out, "]");
}

static void dump_literal(literal_t *l, json_writer_t *out) {
  const char *hint = NULL;
  switch (l->hint) {
  case NY_LIT_HINT_I8:
    hint = "i8";
    break;
  case NY_LIT_HINT_I16:
    hint = "i16";
    break;
  case NY_LIT_HINT_I32:
    hint = "i32";
    break;
  case NY_LIT_HINT_I64:
    hint = "i64";
    break;
  case NY_LIT_HINT_U8:
    hint = "u8";
    break;
  case NY_LIT_HINT_U16:
    hint = "u16";
    break;
  case NY_LIT_HINT_U32:
    hint = "u32";
    break;
  case NY_LIT_HINT_U64:
    hint = "u64";
    break;
  case NY_LIT_HINT_F32:
    hint = "f32";
    break;
  case NY_LIT_HINT_F64:
    hint = "f64";
    break;
  case NY_LIT_HINT_F128:
    hint = "f128";
    break;
  case NY_LIT_HINT_NONE:
  default:
    hint = NULL;
    break;
  }
  json_append(out, "{\"type\":\"literal\",\"kind\":");
  switch (l->kind) {
  case NY_LIT_INT:
    if (hint)
      json_append(out, "\"int\",\"hint\":\"%s\",\"value\":%ld}", hint, l->as.i);
    else
      json_append(out, "\"int\",\"value\":%ld}", l->as.i);
    break;
  case NY_LIT_FLOAT:
    if (hint)
      json_append(out, "\"float\",\"hint\":\"%s\",\"value\":%f}", hint, l->as.f);
    else
      json_append(out, "\"float\",\"value\":%f}", l->as.f);
    break;
  case NY_LIT_BOOL:
    if (hint)
      json_append(out, "\"bool\",\"hint\":\"%s\",\"value\":%s}", hint,
             l->as.b ? "true" : "false");
    else
      json_append(out, "\"bool\",\"value\":%s}", l->as.b ? "true" : "false");
    break;
  case NY_LIT_STR:
    if (hint)
      json_append(out, "\"string\",\"hint\":\"%s\",\"value\":\"", hint);
    else
      json_append(out, "\"string\",\"value\":\"");
    for (size_t i = 0; i < l->as.s.len; ++i) {
      char c = l->as.s.data[i];
      if (c == '"')
        json_append(out, "\\\"");
      else if (c == '\\')
        json_append(out, "\\\\");
      else if (c == '\n')
        json_append(out, "\\n");
      else
        json_append(out, "%c", c);
    }
    json_append(out, "\"}");
    break;
  }
}

static void dump_expr(expr_t *e, json_writer_t *out) {
  if (!e) {
    json_append(out, "null");
    return;
  }
  switch (e->kind) {
  case NY_E_IDENT:
    json_append(out, "{\"type\":\"ident\",\"name\":\"%s\"}", e->as.ident.name);
    break;
  case NY_E_LITERAL:
    dump_literal(&e->as.literal, out);
    break;
  case NY_E_UNARY:
    json_append(out, "{\"type\":\"unary\",\"op\":\"%s\",\"right\":", e->as.unary.op);
    dump_expr(e->as.unary.right, out);
    json_append(out, "}");
    break;
  case NY_E_BINARY:
    json_append(out, "{\"type\":\"binary\",\"op\":\"%s\",\"left\":", e->as.binary.op);
    dump_expr(e->as.binary.left, out);
    json_append(out, ",\"right\":");
    dump_expr(e->as.binary.right, out);
    json_append(out, "}");
    break;
  case NY_E_LOGICAL:
    json_append(out, "{\"type\":\"logical\",\"op\":\"%s\",\"left\":", e->as.logical.op);
    dump_expr(e->as.logical.left, out);
    json_append(out, ",\"right\":");
    dump_expr(e->as.logical.right, out);
    json_append(out, "}");
    break;
  case NY_E_TERNARY:
    json_append(out, "{\"type\":\"ternary\",\"cond\":");
    dump_expr(e->as.ternary.cond, out);
    json_append(out, ",\"then\":");
    dump_expr(e->as.ternary.true_expr, out);
    json_append(out, ",\"else\":");
    dump_expr(e->as.ternary.false_expr, out);
    json_append(out, "}");
    break;
  case NY_E_CALL:
    json_append(out, "{\"type\":\"call\",\"callee\":");
    dump_expr(e->as.call.callee, out);
    json_append(out, ",\"args\":[");
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        json_append(out, "{\"name\":");
        json_string(out, e->as.call.args.data[i].name);
        json_append(out, ",\"value\":");
      }
      dump_expr(e->as.call.args.data[i].val, out);
      if (e->as.call.args.data[i].name)
        json_append(out, "}");
      if (i < e->as.call.args.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_E_MEMCALL:
    json_append(out, "{\"type\":\"memcall\",\"target\":");
    dump_expr(e->as.memcall.target, out);
    json_append(out, ",\"name\":\"%s\",\"args\":[", e->as.memcall.name);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
      if (e->as.memcall.args.data[i].name) {
        json_append(out, "{\"name\":");
        json_string(out, e->as.memcall.args.data[i].name);
        json_append(out, ",\"value\":");
      }
      dump_expr(e->as.memcall.args.data[i].val, out);
      if (e->as.memcall.args.data[i].name)
        json_append(out, "}");
      if (i < e->as.memcall.args.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_E_INDEX:
    json_append(out, "{\"type\":\"index\",\"target\":");
    dump_expr(e->as.index.target, out);
    json_append(out, ",\"start\":");
    dump_expr(e->as.index.start, out);
    json_append(out, ",\"stop\":");
    dump_expr(e->as.index.stop, out);
    json_append(out, ",\"step\":");
    dump_expr(e->as.index.step, out);
    json_append(out, "}");
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    json_append(out, "{\"type\":\"%s\",\"return\":", e->kind == NY_E_FN ? "fn" : "lambda");
    json_string(out, e->as.lambda.return_type ? e->as.lambda.return_type : "");
    json_append(out, ",\"params\":");
    dump_params(&e->as.lambda.params, out);
    json_append(out, ",\"variadic\":%s,\"body\":",
           e->as.lambda.is_variadic ? "true" : "false");
    dump_stmt(e->as.lambda.body, out);
    json_append(out, "}");
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    json_append(out, "{\"type\":\"%s\",\"elements\":[",
           e->kind == NY_E_LIST ? "list" : (e->kind == NY_E_TUPLE ? "tuple" : "set"));
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      dump_expr(e->as.list_like.data[i], out);
      if (i < e->as.list_like.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_E_DICT:
    json_append(out, "{\"type\":\"dict\",\"pairs\":[");
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      json_append(out, "{\"key\":");
      dump_expr(e->as.dict.pairs.data[i].key, out);
      json_append(out, ",\"value\":");
      dump_expr(e->as.dict.pairs.data[i].value, out);
      json_append(out, "}");
      if (i < e->as.dict.pairs.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_E_ASM:
    json_append(out, "{\"type\":\"asm\",\"code\":\"%s\",\"constraints\":\"%s\",\"args\":[",
           e->as.as_asm.code, e->as.as_asm.constraints);
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
      dump_expr(e->as.as_asm.args.data[i], out);
      if (i < e->as.as_asm.args.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_E_COMPTIME:
    json_append(out, "{\"type\":\"comptime\",\"body\":");
    dump_stmt(e->as.comptime_expr.body, out);
    json_append(out, "}");
    break;
  case NY_E_FSTRING:
    json_append(out, "{\"type\":\"fstring\",\"parts\":[");
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_STR) {
        json_append(out, "{\"kind\":\"str\",\"value\":");
        char *tmp = (char *)(uintptr_t)rt_malloc(part->as.s.len + 1);
        if (tmp) {
          memcpy(tmp, part->as.s.data, part->as.s.len);
          tmp[part->as.s.len] = '\0';
          json_string(out, tmp);
          rt_free((int64_t)(uintptr_t)tmp);
        } else {
          json_string(out, "");
        }
        json_append(out, "}");
      } else {
        json_append(out, "{\"kind\":\"expr\",\"value\":");
        dump_expr(part->as.e, out);
        json_append(out, "}");
      }
      if (i + 1 < e->as.fstring.parts.len)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_E_INFERRED_MEMBER:
    json_append(out, "{\"type\":\"inferred_member\",\"name\":");
    json_string(out, e->as.inferred_member.name ? e->as.inferred_member.name : "");
    json_append(out, "}");
    break;
  case NY_E_EMBED:
    json_append(out, "{\"type\":\"embed\",\"path\":");
    json_string(out, e->as.embed.path ? e->as.embed.path : "");
    json_append(out, "}");
    break;
  case NY_E_MATCH:
    json_append(out, "{\"type\":\"match\",\"test\":");
    dump_expr(e->as.match.test, out);
    json_append(out, ",\"arms\":[");
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      json_append(out, "{\"patterns\":[");
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        dump_expr(arm->patterns.data[j], out);
        if (j + 1 < arm->patterns.len)
          json_append(out, ",");
      }
      json_append(out, "],\"guard\":");
      dump_expr(arm->guard, out);
      json_append(out, ",\"conseq\":");
      dump_stmt(arm->conseq, out);
      json_append(out, "}");
      if (i + 1 < e->as.match.arms.len)
        json_append(out, ",");
    }
    json_append(out, "],\"default\":");
    dump_stmt(e->as.match.default_conseq, out);
    json_append(out, "}");
    break;
  case NY_E_MEMBER:
    json_append(out, "{\"type\":\"member\",\"target\":");
    dump_expr(e->as.member.target, out);
    json_append(out, ",\"name\":");
    json_string(out, e->as.member.name ? e->as.member.name : "");
    json_append(out, "}");
    break;
  case NY_E_PTR_TYPE:
    json_append(out, "{\"type\":\"ptr_type\",\"target\":");
    dump_expr(e->as.ptr_type.target, out);
    json_append(out, "}");
    break;
  case NY_E_DEREF:
    json_append(out, "{\"type\":\"deref\",\"target\":");
    dump_expr(e->as.deref.target, out);
    json_append(out, "}");
    break;
  case NY_E_SIZEOF:
    json_append(out, "{\"type\":\"sizeof\",\"is_type\":%s,\"type_name\":",
           e->as.szof.is_type ? "true" : "false");
    json_string(out, e->as.szof.type_name ? e->as.szof.type_name : "");
    json_append(out, ",\"target\":");
    dump_expr(e->as.szof.target, out);
    json_append(out, "}");
    break;
  case NY_E_TRY:
    json_append(out, "{\"type\":\"try_expr\",\"target\":");
    dump_expr(e->as.try_expr.target, out);
    json_append(out, "}");
    break;
  default:
    json_append(out, "{\"type\":\"unknown\"}");
    break;
  }
}

static void dump_stmt(stmt_t *s, json_writer_t *out) {
  if (!s) {
    json_append(out, "null");
    return;
  }
  switch (s->kind) {
  case NY_S_BLOCK:
    json_append(out, "{\"type\":\"block\",\"body\":[");
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      dump_stmt(s->as.block.body.data[i], out);
      if (i < s->as.block.body.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_S_USE:
    json_append(out, "{\"type\":\"use\",\"module\":\"%s\"", s->as.use.module);
    if (s->as.use.alias) {
      json_append(out, ",\"alias\":\"%s\"", s->as.use.alias);
    }
    if (s->as.use.profile) {
      json_append(out, ",\"profile\":\"%s\"", s->as.use.profile);
    }
    if (s->as.use.is_local) {
      json_append(out, ",\"local\":true");
    }
    json_append(out, "}");
    break;
  case NY_S_VAR:
    json_append(out, "{\"type\":\"var\",\"names\":[");
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      json_append(out, "\"%s\"", s->as.var.names.data[i]);
      if (i < s->as.var.names.len - 1)
        json_append(out, ",");
    }
    json_append(out, "],\"del\":%s,\"exprs\":[", s->as.var.is_del ? "true" : "false");
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      dump_expr(s->as.var.exprs.data[i], out);
      if (i < s->as.var.exprs.len - 1)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_S_EXPR:
    json_append(out, "{\"type\":\"expr_stmt\",\"expr_t\":");
    dump_expr(s->as.expr.expr, out);
    json_append(out, "}");
    break;
  case NY_S_IF:
    json_append(out, "{\"type\":\"if\",\"test\":");
    dump_expr(s->as.iff.test, out);
    json_append(out, ",\"conseq\":");
    dump_stmt(s->as.iff.conseq, out);
    json_append(out, ",\"alt\":");
    dump_stmt(s->as.iff.alt, out);
    json_append(out, "}");
    break;
  case NY_S_GUARD:
    json_append(out, "{\"type\":\"layout_guard\",\"layout\":");
    json_string(out, s->as.guard.type_name);
    json_append(out, ",\"name\":");
    json_string(out, s->as.guard.name);
    json_append(out, ",\"value\":");
    dump_expr(s->as.guard.value, out);
    json_append(out, ",\"fallback\":");
    dump_stmt(s->as.guard.fallback, out);
    json_append(out, "}");
    break;
  case NY_S_WHILE:
    json_append(out, "{\"type\":\"while\",\"test\":");
    dump_expr(s->as.whl.test, out);
    json_append(out, ",\"body\":");
    dump_stmt(s->as.whl.body, out);
    if (s->as.whl.update) {
      json_append(out, ",\"update\":");
      dump_stmt(s->as.whl.update, out);
    }
    if (s->as.whl.init) {
      json_append(out, ",\"init\":");
      dump_stmt(s->as.whl.init, out);
    }
    json_append(out, "}");
    break;
  case NY_S_FOR:
    if (s->as.fr.iter_var) {
      json_append(out, "{\"type\":\"for\",\"var\":\"%s\",\"iterable\":", s->as.fr.iter_var);
      dump_expr(s->as.fr.iterable, out);
      json_append(out, ",\"by_index\":%s", s->as.fr.iter_by_index ? "true" : "false");
      if (s->as.fr.iter_index_var)
        json_append(out, ",\"index_var\":\"%s\"", s->as.fr.iter_index_var);
    } else {
      json_append(out, "{\"type\":\"for_cstyle\",\"init\":");
      dump_stmt(s->as.fr.init, out);
      json_append(out, ",\"cond\":");
      dump_expr(s->as.fr.cond, out);
      json_append(out, ",\"update\":");
      dump_stmt(s->as.fr.update, out);
    }
    json_append(out, ",\"body\":");
    dump_stmt(s->as.fr.body, out);
    json_append(out, "}");
    break;
  case NY_S_TRY:
    json_append(out, "{\"type\":\"try\",\"body\":");
    dump_stmt(s->as.tr.body, out);
    json_append(out, ",\"err\":\"%s\",\"handler\":", s->as.tr.err ? s->as.tr.err : "null");
    dump_stmt(s->as.tr.handler, out);
    json_append(out, "}");
    break;
  case NY_S_FUNC:
    json_append(out, "{\"type\":\"func\",\"name\":");
    json_string(out, s->as.fn.name ? s->as.fn.name : "");
    json_append(out, ",\"return\":");
    json_string(out, s->as.fn.return_type ? s->as.fn.return_type : "");
    json_append(out, ",\"params\":");
    dump_params(&s->as.fn.params, out);
    json_append(out, ",\"variadic\":%s,\"body\":",
           s->as.fn.is_variadic ? "true" : "false");
    dump_stmt(s->as.fn.body, out);
    json_append(out, "}");
    break;
  case NY_S_EXTERN:
    json_append(out, "{\"type\":\"extern\",\"name\":");
    json_string(out, s->as.ext.name ? s->as.ext.name : "");
    json_append(out, ",\"return\":");
    json_string(out, s->as.ext.return_type ? s->as.ext.return_type : "");
    json_append(out, ",\"link_name\":");
    json_string(out, s->as.ext.link_name ? s->as.ext.link_name : "");
    json_append(out, ",\"params\":");
    dump_params(&s->as.ext.params, out);
    json_append(out, ",\"variadic\":%s}", s->as.ext.is_variadic ? "true" : "false");
    break;
  case NY_S_RETURN:
    json_append(out, "{\"type\":\"return\",\"value\":");
    dump_expr(s->as.ret.value, out);
    json_append(out, "}");
    break;
  case NY_S_LINK:
    json_append(out, "{\"type\":\"link\",\"lib\":\"%s\"}", s->as.link.lib);
    break;
  case NY_S_LABEL:
    json_append(out, "{\"type\":\"label\",\"name\":\"%s\"}", s->as.label.name);
    break;
  case NY_S_GOTO:
    json_append(out, "{\"type\":\"goto\",\"name\":\"%s\"}", s->as.go.name);
    break;
  case NY_S_DEFER:
    json_append(out, "{\"type\":\"defer\",\"body\":");
    dump_stmt(s->as.de.body, out);
    json_append(out, "}");
    break;
  case NY_S_BREAK:
    json_append(out, "{\"type\":\"break\"}");
    break;
  case NY_S_CONTINUE:
    json_append(out, "{\"type\":\"continue\"}");
    break;
  case NY_S_LAYOUT:
    json_append(out, "{\"type\":\"layout\",\"name\":");
    json_string(out, s->as.layout.name ? s->as.layout.name : "");
    json_append(out, ",\"pack\":%zu,\"align\":%zu,\"fields\":[", s->as.layout.pack,
           s->as.layout.align_override);
    for (size_t i = 0; i < s->as.layout.fields.len; ++i) {
      layout_field_t *f = &s->as.layout.fields.data[i];
      json_append(out, "{\"name\":");
      json_string(out, f->name ? f->name : "");
      json_append(out, ",\"type\":");
      json_string(out, f->type_name ? f->type_name : "");
      json_append(out, ",\"width\":%d}", f->width);
      if (i + 1 < s->as.layout.fields.len)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_S_STRUCT:
    json_append(out, "{\"type\":\"struct\",\"name\":");
    json_string(out, s->as.struc.name ? s->as.struc.name : "");
    json_append(out, ",\"pack\":%zu,\"align\":%zu,\"fields\":[", s->as.struc.pack,
           s->as.struc.align_override);
    for (size_t i = 0; i < s->as.struc.fields.len; ++i) {
      layout_field_t *f = &s->as.struc.fields.data[i];
      json_append(out, "{\"name\":");
      json_string(out, f->name ? f->name : "");
      json_append(out, ",\"type\":");
      json_string(out, f->type_name ? f->type_name : "");
      json_append(out, ",\"width\":%d}", f->width);
      if (i + 1 < s->as.struc.fields.len)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_S_MATCH:
    json_append(out, "{\"type\":\"match_stmt\",\"test\":");
    dump_expr(s->as.match.test, out);
    json_append(out, ",\"arms\":[");
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      json_append(out, "{\"patterns\":[");
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        dump_expr(arm->patterns.data[j], out);
        if (j + 1 < arm->patterns.len)
          json_append(out, ",");
      }
      json_append(out, "],\"guard\":");
      dump_expr(arm->guard, out);
      json_append(out, ",\"conseq\":");
      dump_stmt(arm->conseq, out);
      json_append(out, "}");
      if (i + 1 < s->as.match.arms.len)
        json_append(out, ",");
    }
    json_append(out, "],\"default\":");
    dump_stmt(s->as.match.default_conseq, out);
    json_append(out, "}");
    break;
  case NY_S_MODULE:
    json_append(out, "{\"type\":\"module\",\"name\":");
    json_string(out, s->as.module.name ? s->as.module.name : "");
    json_append(out, ",\"export_all\":%s,\"body\":[",
           s->as.module.export_all ? "true" : "false");
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      dump_stmt(s->as.module.body.data[i], out);
      if (i + 1 < s->as.module.body.len)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_S_EXPORT:
    json_append(out, "{\"type\":\"export\",\"names\":[");
    for (size_t i = 0; i < s->as.exprt.names.len; ++i) {
      json_string(out, s->as.exprt.names.data[i] ? s->as.exprt.names.data[i] : "");
      if (i + 1 < s->as.exprt.names.len)
        json_append(out, ",");
    }
    json_append(out, "]");
    if (s->as.exprt.profile) {
      json_append(out, ",\"profile\":");
      json_string(out, s->as.exprt.profile);
    }
    if (s->as.exprt.is_internal)
      json_append(out, ",\"internal\":true");
    json_append(out, "}");
    break;
  case NY_S_ENUM:
    json_append(out, "{\"type\":\"enum\",\"name\":");
    json_string(out, s->as.enu.name ? s->as.enu.name : "");
    json_append(out, ",\"items\":[");
    for (size_t i = 0; i < s->as.enu.items.len; ++i) {
      json_append(out, "{\"name\":");
      json_string(out, s->as.enu.items.data[i].name);
      json_append(out, ",\"value\":");
      dump_expr(s->as.enu.items.data[i].value, out);
      json_append(out, "}");
      if (i + 1 < s->as.enu.items.len)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  case NY_S_MACRO:
    json_append(out, "{\"type\":\"macro\",\"name\":\"%s\",\"args\":[", s->as.macro.name);
    for (size_t i = 0; i < s->as.macro.args.len; ++i) {
      dump_expr(s->as.macro.args.data[i], out);
      if (i < s->as.macro.args.len - 1)
        json_append(out, ",");
    }
    json_append(out, "],\"body\":");
    dump_stmt(s->as.macro.body, out);
    json_append(out, "}");
    break;
  case NY_S_INCLUDE:
    json_append(out, "{\"type\":\"include\",\"path\":\"%s\",\"prefix\":\"%s\"}",
           s->as.inc.path, s->as.inc.prefix ? s->as.inc.prefix : "");
    break;
  case NY_S_DEFINE:
    json_append(out, "{\"type\":\"define\",\"name\":");
    json_string(out, s->as.def.name ? s->as.def.name : "");
    json_append(out, ",\"value\":");
    json_string(out, s->as.def.value ? s->as.def.value : "");
    json_append(out, "}");
    break;
  case NY_S_OPERATOR:
    json_append(out,
           "{\"type\":\"operator\",\"op\":\"%s\",\"left\":\"%s\",\"right\":\"%s\","
           "\"return\":\"%s\",\"target\":\"%s\"}",
           s->as.oper.op ? s->as.oper.op : "", s->as.oper.left_type ? s->as.oper.left_type : "",
           s->as.oper.right_type ? s->as.oper.right_type : "",
           s->as.oper.return_type ? s->as.oper.return_type : "",
           s->as.oper.target ? s->as.oper.target : "");
    break;
  case NY_S_IMPL:
    json_append(out, "{\"type\":\"impl\",\"for\":\"%s\",\"methods\":[",
           s->as.impl.type_name ? s->as.impl.type_name : "");
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      dump_stmt(s->as.impl.methods.data[i], out);
      if (i + 1 < s->as.impl.methods.len)
        json_append(out, ",");
    }
    json_append(out, "]}");
    break;
  default:
    json_append(out, "{\"type\":\"unknown_stmt\"}");
    break;
  }
}

char *ny_ast_to_json(program_t *prog) {
  json_writer_t jw;
  json_writer_init(&jw, 1024);
  json_append(&jw, "[");
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    dump_stmt(prog->body.data[i], &jw);
    if (i < prog->body.len - 1)
      json_append(&jw, ",");
  }
  json_append(&jw, "]");
  return json_writer_take(&jw);
}

char *ny_expr_to_json(expr_t *expr) {
  json_writer_t jw;
  json_writer_init(&jw, 512);
  dump_expr(expr, &jw);
  return json_writer_take(&jw);
}

static bool ny_ast_token_in_file(token_t tok, const char *filename) {
  if (!filename || !*filename)
    return true;
  if (!tok.filename)
    return false;
  if (strcmp(tok.filename, filename) == 0)
    return true;
  size_t n = strlen(filename);
  return strncmp(tok.filename, filename, n) == 0 && tok.filename[n] == ':';
}

char *ny_ast_to_json_filtered(program_t *prog, const char *filename) {
  json_writer_t jw;
  json_writer_init(&jw, 1024);
  json_append(&jw, "[");
  bool first = true;
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (!ny_ast_token_in_file(s->tok, filename))
      continue;
    if (!first)
      json_append(&jw, ",");
    dump_stmt(s, &jw);
    first = false;
  }
  json_append(&jw, "]");
  return json_writer_take(&jw);
}

static void ny_ast_append_param_signature(char **buf, size_t *len, size_t *cap,
                                          const param_t *p) {
  if (!p)
    return;
  if (p->type && *p->type)
    append(buf, len, cap, "%s %s", p->type, p->name ? p->name : "");
  else
    append(buf, len, cap, "%s", p->name ? p->name : "");
}

static void ny_ast_append_params_signature(char **buf, size_t *len, size_t *cap,
                                           const ny_param_list *params) {
  append(buf, len, cap, "(");
  for (size_t i = 0; params && i < params->len; ++i) {
    if (i)
      append(buf, len, cap, ", ");
    ny_ast_append_param_signature(buf, len, cap, &params->data[i]);
  }
  append(buf, len, cap, ")");
}

static void ny_ast_append_params_json(char **buf, size_t *len, size_t *cap,
                                      const ny_param_list *params) {
  append(buf, len, cap, "[");
  for (size_t i = 0; params && i < params->len; ++i) {
    const param_t *p = &params->data[i];
    if (i)
      append(buf, len, cap, ",");
    append(buf, len, cap, "{\"name\":");
    append_json_str(buf, len, cap, p->name ? p->name : "");
    append(buf, len, cap, ",\"type\":");
    append_json_str(buf, len, cap, p->type ? p->type : "");
    append(buf, len, cap, "}");
  }
  append(buf, len, cap, "]");
}

static size_t ny_ast_symbol_span_lines(stmt_t *s) {
  if (!s)
    return 1;
  if (s->kind == NY_S_FUNC)
    return ny_src_line_count(s->as.fn.src_start, s->as.fn.src_end, 1);
  if (s->kind == NY_S_MODULE)
    return ny_src_line_count(s->as.module.src_start, s->as.module.src_end, 1);
  if (s->kind == NY_S_LAYOUT)
    return s->as.layout.fields.len + 2;
  if (s->kind == NY_S_STRUCT)
    return s->as.struc.fields.len + 2;
  if (s->kind == NY_S_IMPL)
    return s->as.impl.methods.len + 2;
  return 1;
}

static void ny_ast_append_symbol_common(char **buf, size_t *len, size_t *cap,
                                        const char *kind, const char *name, stmt_t *s,
                                        const char *signature, const char *doc) {
  int line = s ? s->tok.line : 1;
  int col = s ? s->tok.col : 1;
  size_t span = ny_ast_symbol_span_lines(s);
  int end_line = line + (span > 0 ? (int)span - 1 : 0);
  append(buf, len, cap, "{\"kind\":");
  append_json_str(buf, len, cap, kind ? kind : "");
  append(buf, len, cap, ",\"name\":");
  append_json_str(buf, len, cap, name ? name : "");
  append(buf, len, cap, ",\"line\":%d,\"col\":%d,\"end_line\":%d,\"signature\":",
         line > 0 ? line : 1, col > 0 ? col : 1, end_line > 0 ? end_line : 1);
  append_json_str(buf, len, cap, signature ? signature : "");
  if (doc && *doc) {
    append(buf, len, cap, ",\"doc\":");
    append_json_str(buf, len, cap, doc);
  }
}

static void ny_ast_append_symbol_for_stmt(char **buf, size_t *len, size_t *cap,
                                          stmt_t *s, const char *override_name,
                                          bool *first) {
  if (!s)
    return;
  const char *kind = NULL;
  const char *name = override_name;
  size_t sig_len = 0, sig_cap = 256;
  char *sig = (char *)(uintptr_t)rt_malloc(sig_cap);
  if (!sig)
    return;
  sig[0] = '\0';
  switch (s->kind) {
  case NY_S_MODULE:
    kind = "module";
    name = name ? name : s->as.module.name;
    append(&sig, &sig_len, &sig_cap, "module %s", name ? name : "");
    break;
  case NY_S_USE:
    kind = "use";
    name = name ? name : s->as.use.module;
    append(&sig, &sig_len, &sig_cap, "use %s", name ? name : "");
    if (s->as.use.alias)
      append(&sig, &sig_len, &sig_cap, " as %s", s->as.use.alias);
    break;
  case NY_S_VAR:
    kind = s->as.var.is_mut ? "mut" : (s->as.var.is_del ? "del" : "def");
    name = name ? name : (s->as.var.names.len ? s->as.var.names.data[0] : "");
    append(&sig, &sig_len, &sig_cap, "%s %s", kind, name ? name : "");
    break;
  case NY_S_FUNC:
    kind = "fn";
    name = name ? name : s->as.fn.name;
    append(&sig, &sig_len, &sig_cap, "fn %s", name ? name : "");
    ny_ast_append_params_signature(&sig, &sig_len, &sig_cap, &s->as.fn.params);
    if (s->as.fn.return_type && *s->as.fn.return_type)
      append(&sig, &sig_len, &sig_cap, " %s", s->as.fn.return_type);
    break;
  case NY_S_EXTERN:
    kind = "extern";
    name = name ? name : s->as.ext.name;
    append(&sig, &sig_len, &sig_cap, "extern fn %s", name ? name : "");
    ny_ast_append_params_signature(&sig, &sig_len, &sig_cap, &s->as.ext.params);
    if (s->as.ext.return_type && *s->as.ext.return_type)
      append(&sig, &sig_len, &sig_cap, " %s", s->as.ext.return_type);
    break;
  case NY_S_LAYOUT:
    kind = "layout";
    name = name ? name : s->as.layout.name;
    append(&sig, &sig_len, &sig_cap, "layout %s", name ? name : "");
    break;
  case NY_S_STRUCT:
    kind = "struct";
    name = name ? name : s->as.struc.name;
    append(&sig, &sig_len, &sig_cap, "struct %s", name ? name : "");
    break;
  case NY_S_ENUM:
    kind = "enum";
    name = name ? name : s->as.enu.name;
    append(&sig, &sig_len, &sig_cap, "enum %s", name ? name : "");
    break;
  case NY_S_DEFINE:
    kind = "define";
    name = name ? name : s->as.def.name;
    append(&sig, &sig_len, &sig_cap, "#define %s", name ? name : "");
    break;
  case NY_S_OPERATOR:
    kind = "operator";
    name = name ? name : s->as.oper.target;
    append(&sig, &sig_len, &sig_cap, "operator %s: %s = %s",
           s->as.oper.op ? s->as.oper.op : "",
           s->as.oper.return_type ? s->as.oper.return_type : "",
           s->as.oper.target ? s->as.oper.target : "");
    break;
  case NY_S_IMPL:
    kind = "impl";
    name = name ? name : s->as.impl.type_name;
    append(&sig, &sig_len, &sig_cap, "impl %s", name ? name : "");
    break;
  case NY_S_MACRO:
    kind = "macro";
    name = name ? name : s->as.macro.name;
    append(&sig, &sig_len, &sig_cap, "macro %s", name ? name : "");
    break;
  default:
    rt_free((int64_t)(uintptr_t)sig);
    return;
  }
  if (!name || !*name) {
    rt_free((int64_t)(uintptr_t)sig);
    return;
  }
  if (!*first)
    append(buf, len, cap, ",");
  *first = false;
  ny_ast_append_symbol_common(buf, len, cap, kind, name, s, sig,
                              s->kind == NY_S_FUNC ? s->as.fn.doc : NULL);
  if (s->kind == NY_S_FUNC) {
    append(buf, len, cap, ",\"params\":");
    ny_ast_append_params_json(buf, len, cap, &s->as.fn.params);
    append(buf, len, cap, ",\"return\":");
    append_json_str(buf, len, cap, s->as.fn.return_type ? s->as.fn.return_type : "");
  } else if (s->kind == NY_S_EXTERN) {
    append(buf, len, cap, ",\"params\":");
    ny_ast_append_params_json(buf, len, cap, &s->as.ext.params);
    append(buf, len, cap, ",\"return\":");
    append_json_str(buf, len, cap, s->as.ext.return_type ? s->as.ext.return_type : "");
  }
  append(buf, len, cap, "}");
  rt_free((int64_t)(uintptr_t)sig);
}

static void ny_ast_collect_symbols(char **buf, size_t *len, size_t *cap, stmt_t *s,
                                   const char *filename, bool *first) {
  if (!s || !ny_ast_token_in_file(s->tok, filename))
    return;
  if (s->kind == NY_S_VAR && s->as.var.names.len > 1) {
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      ny_ast_append_symbol_for_stmt(buf, len, cap, s, s->as.var.names.data[i], first);
  } else {
    ny_ast_append_symbol_for_stmt(buf, len, cap, s, NULL, first);
  }
  switch (s->kind) {
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_ast_collect_symbols(buf, len, cap, s->as.module.body.data[i], filename, first);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      ny_ast_collect_symbols(buf, len, cap, s->as.impl.methods.data[i], filename, first);
    break;
  default:
    break;
  }
}

char *ny_ast_symbols_to_json_filtered(program_t *prog, const char *filename) {
  size_t len = 0, cap = 1024;
  char *buf = (char *)(uintptr_t)rt_malloc(cap);
  append(&buf, &len, &cap, "[");
  bool first = true;
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    ny_ast_collect_symbols(&buf, &len, &cap, prog->body.data[i], filename, &first);
  append(&buf, &len, &cap, "]");
  return buf;
}

typedef struct ny_expand_stats_t {
  size_t funcs;
  size_t externs;
  size_t layouts;
  size_t structs;
  size_t modules;
  size_t operators;
  size_t macros;
  size_t comptime_exprs;
  size_t calls;
  size_t matched_sites;
} ny_expand_stats_t;

static bool ny_contains(const char *haystack, const char *needle) {
  return haystack && needle && *needle && strstr(haystack, needle) != NULL;
}

static bool ny_expand_filter_match(const char *filter, const char *kind, const char *name,
                                   const char *extra) {
  if (!filter || !*filter)
    return true;
  if (ny_contains(kind, filter) || ny_contains(name, filter) || ny_contains(extra, filter))
    return true;
  if (name && *name) {
    char derive_name[512];
    snprintf(derive_name, sizeof(derive_name), "%s.derive", name);
    if (strcmp(filter, derive_name) == 0 || ny_contains(derive_name, filter))
      return true;
  }
  return false;
}

static bool ny_expand_trace_filter_match(const char *filter, const char *kind, const char *name,
                                         const char *extra) {
  if (!filter || !*filter)
    return false;
  if ((kind && strcmp(filter, kind) == 0) || (name && strcmp(filter, name) == 0) ||
      ny_contains(extra, filter))
    return true;
  return false;
}

static bool ny_ast_stmt_matches_filter(stmt_t *s, const char *filter) {
  if (!filter || !*filter || !s)
    return true;
  switch (s->kind) {
  case NY_S_FUNC:
    return ny_expand_filter_match(filter, "func", s->as.fn.name, s->as.fn.return_type);
  case NY_S_EXTERN:
    return ny_expand_filter_match(filter, "extern", s->as.ext.name, s->as.ext.link_name);
  case NY_S_LAYOUT:
    return ny_expand_filter_match(filter, "layout", s->as.layout.name, "derive");
  case NY_S_STRUCT:
    return ny_expand_filter_match(filter, "struct", s->as.struc.name, "derive");
  case NY_S_MODULE:
    return ny_expand_filter_match(filter, "module", s->as.module.name, NULL);
  case NY_S_MACRO:
    return ny_expand_filter_match(filter, "macro", s->as.macro.name, "template");
  case NY_S_OPERATOR:
    return ny_expand_filter_match(filter, "operator", s->as.oper.target, s->as.oper.op);
  case NY_S_IMPL:
    return ny_expand_filter_match(filter, "impl", s->as.impl.type_name, NULL);
  default:
    return false;
  }
}

static char *ny_ast_to_json_filtered_by_name(program_t *prog, const char *filename,
                                             const char *filter) {
  json_writer_t jw;
  json_writer_init(&jw, 1024);
  json_append(&jw, "[");
  bool first = true;
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (!ny_ast_token_in_file(s->tok, filename) || !ny_ast_stmt_matches_filter(s, filter))
      continue;
    if (!first)
      json_append(&jw, ",");
    dump_stmt(s, &jw);
    first = false;
  }
  json_append(&jw, "]");
  return json_writer_take(&jw);
}

static const char *ny_expr_callee_name(expr_t *e) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_IDENT)
    return e->as.ident.name;
  if (e->kind == NY_E_MEMBER)
    return e->as.member.name;
  if (e->kind == NY_E_MEMCALL)
    return e->as.memcall.name;
  return NULL;
}

static size_t ny_src_line_count(const char *start, const char *end, size_t fallback) {
  if (!start || !end || end <= start)
    return fallback ? fallback : 1;
  size_t lines = 1;
  for (const char *p = start; p < end; ++p) {
    if (*p == '\n')
      lines++;
  }
  return lines;
}

static size_t ny_stmt_input_lines(stmt_t *s) {
  if (!s)
    return 0;
  if (s->kind == NY_S_FUNC)
    return ny_src_line_count(s->as.fn.src_start, s->as.fn.src_end, 3);
  if (s->kind == NY_S_MODULE)
    return ny_src_line_count(s->as.module.src_start, s->as.module.src_end, s->as.module.body.len);
  if (s->kind == NY_S_LAYOUT)
    return s->as.layout.fields.len + 2;
  if (s->kind == NY_S_STRUCT)
    return s->as.struc.fields.len + 2;
  if (s->kind == NY_S_IMPL)
    return s->as.impl.methods.len + 2;
  return 1;
}

static void ny_expand_report_expr(expr_t *e, char **buf, size_t *len, size_t *cap,
                                  const char *filter, bool meta_trace, ny_expand_stats_t *stats);
static void ny_expand_report_stmt(stmt_t *s, char **buf, size_t *len, size_t *cap,
                                  const char *source_name, const char *filter, bool meta_trace,
                                  const char *explain, ny_expand_stats_t *stats);

static void ny_expand_report_rule(char **buf, size_t *len, size_t *cap) {
  append(buf, len, cap, "%s----------------------------------------------------------------%s\n",
         clr(NY_CLR_GRAY), clr(NY_CLR_RESET));
}

static const char *ny_expand_short_file(const char *path) {
  if (!path || !*path)
    return "<unknown>";
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(path, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  return slash ? slash + 1 : path;
}

static void ny_expand_report_kv(char **buf, size_t *len, size_t *cap, const char *key,
                                const char *fmt, ...) {
  append(buf, len, cap, "%s%-8s%s ", clr(NY_CLR_GRAY), key ? key : "", clr(NY_CLR_RESET));
  va_list ap;
  va_start(ap, fmt);
  char tmp[1024];
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  append(buf, len, cap, "%s\n", tmp);
}

static void ny_expand_report_section(char **buf, size_t *len, size_t *cap, const char *name) {
  append(buf, len, cap, "%s%s%s%s\n", clr(NY_CLR_BOLD), clr(NY_CLR_CYAN),
         name ? name : "graph", clr(NY_CLR_RESET));
}

static void ny_expand_append_trimmed(char **buf, size_t *len, size_t *cap, const char *s,
                                     size_t max) {
  if (!s || !*s)
    return;
  size_t n = strlen(s);
  if (n <= max) {
    append(buf, len, cap, "%s", s);
    return;
  }
  if (max <= 3) {
    append(buf, len, cap, "%.*s", (int)max, s);
    return;
  }
  append(buf, len, cap, "%.*s...", (int)(max - 3), s);
}

static void ny_expand_emit_site(char **buf, size_t *len, size_t *cap, ny_expand_stats_t *stats,
                                const char *kind, const char *name, token_t tok,
                                const char *why, const char *symbols, size_t input_lines) {
  size_t idx = stats ? stats->matched_sites + 1 : 0;
  append(buf, len, cap, "  %s|--%s %s%03zu%s %s%-13s%s", clr(NY_CLR_GRAY),
         clr(NY_CLR_RESET), clr(NY_CLR_GRAY), idx, clr(NY_CLR_RESET), clr(NY_CLR_BOLD),
         kind ? kind : "site", clr(NY_CLR_RESET));
  if (name && *name)
    append(buf, len, cap, " %-28s", name);
  else
    append(buf, len, cap, " %-28s", "-");
  append(buf, len, cap, " %s@%s %s:%d:%d", clr(NY_CLR_GRAY), clr(NY_CLR_RESET),
         ny_expand_short_file(tok.filename), tok.line, tok.col);
  append(buf, len, cap, " %s%zuL%s", clr(NY_CLR_GREEN), input_lines ? input_lines : 1,
         clr(NY_CLR_RESET));
  if (symbols && *symbols) {
    append(buf, len, cap, " %s->%s ", clr(NY_CLR_MAGENTA), clr(NY_CLR_RESET));
    ny_expand_append_trimmed(buf, len, cap, symbols, 76);
  } else if (why && *why) {
    append(buf, len, cap, " %s->%s ", clr(NY_CLR_MAGENTA), clr(NY_CLR_RESET));
    ny_expand_append_trimmed(buf, len, cap, why, 76);
  }
  append(buf, len, cap, "\n");
  if (stats)
    stats->matched_sites++;
}

static void ny_expand_count_expr(expr_t *e, ny_expand_stats_t *stats) {
  if (!e || !stats)
    return;
  if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL)
    stats->calls++;
  if (e->kind == NY_E_COMPTIME)
    stats->comptime_exprs++;
  switch (e->kind) {
  case NY_E_UNARY:
    ny_expand_count_expr(e->as.unary.right, stats);
    break;
  case NY_E_BINARY:
    ny_expand_count_expr(e->as.binary.left, stats);
    ny_expand_count_expr(e->as.binary.right, stats);
    break;
  case NY_E_LOGICAL:
    ny_expand_count_expr(e->as.logical.left, stats);
    ny_expand_count_expr(e->as.logical.right, stats);
    break;
  case NY_E_TERNARY:
    ny_expand_count_expr(e->as.ternary.cond, stats);
    ny_expand_count_expr(e->as.ternary.true_expr, stats);
    ny_expand_count_expr(e->as.ternary.false_expr, stats);
    break;
  case NY_E_CALL:
    ny_expand_count_expr(e->as.call.callee, stats);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      ny_expand_count_expr(e->as.call.args.data[i].val, stats);
    break;
  case NY_E_MEMCALL:
    ny_expand_count_expr(e->as.memcall.target, stats);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_expand_count_expr(e->as.memcall.args.data[i].val, stats);
    break;
  case NY_E_INDEX:
    ny_expand_count_expr(e->as.index.target, stats);
    ny_expand_count_expr(e->as.index.start, stats);
    ny_expand_count_expr(e->as.index.stop, stats);
    ny_expand_count_expr(e->as.index.step, stats);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      ny_expand_count_expr(e->as.lambda.params.data[i].def, stats);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      ny_expand_count_expr(e->as.list_like.data[i], stats);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      ny_expand_count_expr(e->as.dict.pairs.data[i].key, stats);
      ny_expand_count_expr(e->as.dict.pairs.data[i].value, stats);
    }
    break;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      ny_expand_count_expr(e->as.as_asm.args.data[i], stats);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        ny_expand_count_expr(e->as.fstring.parts.data[i].as.e, stats);
    }
    break;
  case NY_E_MEMBER:
    ny_expand_count_expr(e->as.member.target, stats);
    break;
  case NY_E_PTR_TYPE:
    ny_expand_count_expr(e->as.ptr_type.target, stats);
    break;
  case NY_E_DEREF:
    ny_expand_count_expr(e->as.deref.target, stats);
    break;
  case NY_E_SIZEOF:
    ny_expand_count_expr(e->as.szof.target, stats);
    break;
  case NY_E_TRY:
    ny_expand_count_expr(e->as.try_expr.target, stats);
    break;
  default:
    break;
  }
}

static void ny_expand_count_stmt(stmt_t *s, ny_expand_stats_t *stats) {
  if (!s || !stats)
    return;
  switch (s->kind) {
  case NY_S_FUNC:
    stats->funcs++;
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      ny_expand_count_expr(s->as.fn.params.data[i].def, stats);
    ny_expand_count_stmt(s->as.fn.body, stats);
    break;
  case NY_S_EXTERN:
    stats->externs++;
    break;
  case NY_S_LAYOUT:
    stats->layouts++;
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      ny_expand_count_stmt(s->as.layout.methods.data[i], stats);
    break;
  case NY_S_STRUCT:
    stats->structs++;
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      ny_expand_count_stmt(s->as.struc.methods.data[i], stats);
    break;
  case NY_S_MODULE:
    stats->modules++;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_expand_count_stmt(s->as.module.body.data[i], stats);
    break;
  case NY_S_OPERATOR:
    stats->operators++;
    break;
  case NY_S_MACRO:
    stats->macros++;
    ny_expand_count_stmt(s->as.macro.body, stats);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_expand_count_stmt(s->as.block.body.data[i], stats);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      ny_expand_count_expr(s->as.var.exprs.data[i], stats);
    break;
  case NY_S_EXPR:
    ny_expand_count_expr(s->as.expr.expr, stats);
    break;
  case NY_S_IF:
    ny_expand_count_stmt(s->as.iff.init, stats);
    ny_expand_count_expr(s->as.iff.test, stats);
    ny_expand_count_stmt(s->as.iff.conseq, stats);
    ny_expand_count_stmt(s->as.iff.alt, stats);
    break;
  case NY_S_GUARD:
    ny_expand_count_expr(s->as.guard.value, stats);
    ny_expand_count_stmt(s->as.guard.fallback, stats);
    break;
  case NY_S_WHILE:
    ny_expand_count_stmt(s->as.whl.init, stats);
    ny_expand_count_expr(s->as.whl.test, stats);
    ny_expand_count_stmt(s->as.whl.update, stats);
    ny_expand_count_stmt(s->as.whl.body, stats);
    break;
  case NY_S_FOR:
    ny_expand_count_stmt(s->as.fr.init, stats);
    ny_expand_count_expr(s->as.fr.cond, stats);
    ny_expand_count_expr(s->as.fr.iterable, stats);
    ny_expand_count_stmt(s->as.fr.update, stats);
    ny_expand_count_stmt(s->as.fr.body, stats);
    break;
  case NY_S_RETURN:
    ny_expand_count_expr(s->as.ret.value, stats);
    break;
  case NY_S_DEFER:
    ny_expand_count_stmt(s->as.de.body, stats);
    break;
  case NY_S_MATCH:
    ny_expand_count_expr(s->as.match.test, stats);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        ny_expand_count_expr(arm->patterns.data[j], stats);
      ny_expand_count_expr(arm->guard, stats);
      ny_expand_count_stmt(arm->conseq, stats);
    }
    ny_expand_count_stmt(s->as.match.default_conseq, stats);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      ny_expand_count_stmt(s->as.impl.methods.data[i], stats);
    break;
  default:
    break;
  }
}

static void ny_expand_report_expr(expr_t *e, char **buf, size_t *len, size_t *cap,
                                  const char *filter, bool meta_trace, ny_expand_stats_t *stats) {
  if (!e)
    return;
  if (e->kind == NY_E_COMPTIME &&
      ny_expand_filter_match(filter, "comptime", "comptime", NULL)) {
    ny_expand_emit_site(buf, len, cap, stats, "comptime-expr", "comptime", e->tok,
                        "compile-time expression body is evaluated before runtime codegen",
                        "ct_eval.block", 1);
  }
  if (e->kind == NY_E_CALL) {
    const char *name = ny_expr_callee_name(e->as.call.callee);
    if (name && meta_trace && ny_expand_filter_match(filter, "call", name, NULL)) {
      ny_expand_emit_site(buf, len, cap, stats, "call", name, e->tok,
                          "meta trace records the parsed call before overload/codegen lowering",
                          "resolve_overload -> gen_call_expr", 1);
    }
    if (name && strcmp(name, "expand") == 0 &&
        ((meta_trace && ny_expand_filter_match(filter, "expand", name, NULL)) ||
         ny_expand_trace_filter_match(filter, "expand", name, NULL))) {
      ny_expand_emit_site(buf, len, cap, stats, "expand-form", name, e->tok,
                          "compiler dumps the argument AST and returns the argument unchanged",
                          "ny_expr_to_json + gen_expr(arg)", 1);
    } else if (name && strcmp(name, "store_layout") == 0 &&
               ((meta_trace && ny_expand_filter_match(filter, "store_layout", name, NULL)) ||
                ny_expand_trace_filter_match(filter, "store_layout", name, NULL))) {
      ny_expand_emit_site(buf, len, cap, stats, "layout-store", name, e->tok,
                          "callgen lowers known layout stores into direct field stores",
                          "emit_store_layout / __layout_offset", 1);
    } else if (name && strcmp(name, "load_layout") == 0 &&
               ((meta_trace && ny_expand_filter_match(filter, "load_layout", name, NULL)) ||
                ny_expand_trace_filter_match(filter, "load_layout", name, NULL))) {
      ny_expand_emit_site(buf, len, cap, stats, "layout-load", name, e->tok,
                          "callgen lowers known layout loads into direct typed field loads",
                          "emit_load_layout / __layout_offset", 1);
    } else if (name && (strcmp(name, "__layout_size") == 0 ||
                        strcmp(name, "__layout_align") == 0 ||
                        strcmp(name, "__layout_offset") == 0) &&
               ((meta_trace && ny_expand_filter_match(filter, "layout-query", name, NULL)) ||
                ny_expand_trace_filter_match(filter, "layout-query", name, NULL))) {
      ny_expand_emit_site(buf, len, cap, stats, "layout-query", name, e->tok,
                          "layout query folds to a compile-time constant when names are literals",
                          "emit_layout_query", 1);
    }
    ny_expand_report_expr(e->as.call.callee, buf, len, cap, filter, meta_trace, stats);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      ny_expand_report_expr(e->as.call.args.data[i].val, buf, len, cap, filter, meta_trace, stats);
    return;
  }
  if (e->kind == NY_E_MEMCALL) {
    if (meta_trace && ny_expand_filter_match(filter, "memcall", e->as.memcall.name, NULL)) {
      ny_expand_emit_site(buf, len, cap, stats, "memcall", e->as.memcall.name, e->tok,
                          "member-call sugar resolves to module/attached method lookup",
                          "lookup_attached_method -> gen_call_expr", 1);
    }
    ny_expand_report_expr(e->as.memcall.target, buf, len, cap, filter, meta_trace, stats);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_expand_report_expr(e->as.memcall.args.data[i].val, buf, len, cap, filter, meta_trace,
                            stats);
    return;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    ny_expand_report_expr(e->as.unary.right, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_BINARY:
    if (meta_trace && ny_expand_filter_match(filter, "operator", e->as.binary.op, NULL)) {
      ny_expand_emit_site(buf, len, cap, stats, "binary-op", e->as.binary.op, e->tok,
                          "typed operator registry can redirect this op during semantic/callgen",
                          "operator table -> gen_binary_expr", 1);
    }
    ny_expand_report_expr(e->as.binary.left, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.binary.right, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_LOGICAL:
    ny_expand_report_expr(e->as.logical.left, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.logical.right, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_TERNARY:
    ny_expand_report_expr(e->as.ternary.cond, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.ternary.true_expr, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.ternary.false_expr, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_INDEX:
    ny_expand_report_expr(e->as.index.target, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.index.start, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.index.stop, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(e->as.index.step, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      ny_expand_report_expr(e->as.lambda.params.data[i].def, buf, len, cap, filter, meta_trace,
                            stats);
    ny_expand_report_stmt(e->as.lambda.body, buf, len, cap, NULL, filter, meta_trace, NULL, stats);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      ny_expand_report_expr(e->as.list_like.data[i], buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      ny_expand_report_expr(e->as.dict.pairs.data[i].key, buf, len, cap, filter, meta_trace, stats);
      ny_expand_report_expr(e->as.dict.pairs.data[i].value, buf, len, cap, filter, meta_trace,
                            stats);
    }
    break;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      ny_expand_report_expr(e->as.as_asm.args.data[i], buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        ny_expand_report_expr(e->as.fstring.parts.data[i].as.e, buf, len, cap, filter, meta_trace,
                              stats);
    }
    break;
  case NY_E_MEMBER:
    ny_expand_report_expr(e->as.member.target, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_PTR_TYPE:
    ny_expand_report_expr(e->as.ptr_type.target, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_DEREF:
    ny_expand_report_expr(e->as.deref.target, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_SIZEOF:
    ny_expand_report_expr(e->as.szof.target, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_E_TRY:
    ny_expand_report_expr(e->as.try_expr.target, buf, len, cap, filter, meta_trace, stats);
    break;
  default:
    break;
  }
}

static void ny_expand_report_stmt(stmt_t *s, char **buf, size_t *len, size_t *cap,
                                  const char *source_name, const char *filter, bool meta_trace,
                                  const char *explain, ny_expand_stats_t *stats) {
  if (!s)
    return;
  if (source_name && *source_name && !ny_ast_token_in_file(s->tok, source_name))
    return;
  switch (s->kind) {
  case NY_S_LAYOUT:
    if (ny_expand_filter_match(filter, "layout", s->as.layout.name, "derive")) {
      char symbols[512];
      snprintf(symbols, sizeof(symbols), "__layout_size __layout_align __layout_offset");
      ny_expand_emit_site(buf, len, cap, stats, "layout", s->as.layout.name, s->tok,
                          "layout metadata and direct field accessor constants are generated",
                          symbols, ny_stmt_input_lines(s));
    }
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      ny_expand_report_stmt(s->as.layout.methods.data[i], buf, len, cap, NULL, filter, meta_trace,
                            explain, stats);
    break;
  case NY_S_STRUCT:
    if (ny_expand_filter_match(filter, "struct", s->as.struc.name, "derive")) {
      char symbols[512];
      snprintf(symbols, sizeof(symbols), "%s.init %s.field_offsets",
               s->as.struc.name ? s->as.struc.name : "", s->as.struc.name ? s->as.struc.name : "");
      ny_expand_emit_site(buf, len, cap, stats, "struct", s->as.struc.name, s->tok,
                          "typed struct shape feeds layout, field access, and constructor lowering",
                          symbols, ny_stmt_input_lines(s));
    }
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      ny_expand_report_stmt(s->as.struc.methods.data[i], buf, len, cap, NULL, filter, meta_trace,
                            explain, stats);
    break;
  case NY_S_OPERATOR:
    if (ny_expand_filter_match(filter, "operator", s->as.oper.target, s->as.oper.op)) {
      char name[512];
      snprintf(name, sizeof(name), "%s %s %s: %s -> %s",
               s->as.oper.left_type ? s->as.oper.left_type : "_", s->as.oper.op ? s->as.oper.op : "",
               s->as.oper.right_type ? s->as.oper.right_type : "_",
               s->as.oper.return_type ? s->as.oper.return_type : "_",
               s->as.oper.target ? s->as.oper.target : "_");
      ny_expand_emit_site(buf, len, cap, stats, "operator", name, s->tok,
                          "typed operator dispatch is resolved before runtime fallback",
                          "operator registry -> direct call target", ny_stmt_input_lines(s));
    }
    break;
  case NY_S_MACRO:
    if (ny_expand_filter_match(filter, "macro", s->as.macro.name, "template")) {
      ny_expand_emit_site(buf, len, cap, stats, "macro", s->as.macro.name, s->tok,
                          "macro/template body is visible to expansion tooling before fallback codegen",
                          "macro.body AST", ny_stmt_input_lines(s));
    }
    ny_expand_report_stmt(s->as.macro.body, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    break;
  case NY_S_FUNC:
    if (explain && *explain && ny_expand_filter_match(explain, "func", s->as.fn.name, NULL)) {
      char symbols[512];
      snprintf(symbols, sizeof(symbols), "llvm=%s argc=%zu return=%s",
               s->as.fn.link_name ? s->as.fn.link_name : (s->as.fn.name ? s->as.fn.name : "<anon>"),
               s->as.fn.params.len, s->as.fn.return_type ? s->as.fn.return_type : "dynamic");
      ny_expand_emit_site(buf, len, cap, stats, "specialization", s->as.fn.name, s->tok,
                          "specialization candidate: typed params/return let callgen skip dynamic ABI work",
                          symbols, ny_stmt_input_lines(s));
    }
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      ny_expand_report_expr(s->as.fn.params.data[i].def, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_stmt(s->as.fn.body, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    break;
  case NY_S_EXTERN:
    if (meta_trace && ny_expand_filter_match(filter, "extern", s->as.ext.name, NULL)) {
      ny_expand_emit_site(buf, len, cap, stats, "extern", s->as.ext.name, s->tok,
                          "extern declaration creates a typed ABI boundary without Ny wrapper code",
                          s->as.ext.link_name ? s->as.ext.link_name : s->as.ext.name,
                          ny_stmt_input_lines(s));
    }
    break;
  case NY_S_MODULE:
    if (meta_trace && ny_expand_filter_match(filter, "module", s->as.module.name, NULL)) {
      ny_expand_emit_site(buf, len, cap, stats, "module", s->as.module.name, s->tok,
                          "module body is scoped, exported, and contributes import aliases",
                          "module scope + alias table", ny_stmt_input_lines(s));
    }
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_expand_report_stmt(s->as.module.body.data[i], buf, len, cap, NULL, filter, meta_trace,
                            explain, stats);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_expand_report_stmt(s->as.block.body.data[i], buf, len, cap, NULL, filter, meta_trace,
                            explain, stats);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      ny_expand_report_expr(s->as.var.exprs.data[i], buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_S_EXPR:
    ny_expand_report_expr(s->as.expr.expr, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_S_IF:
    ny_expand_report_stmt(s->as.iff.init, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    ny_expand_report_expr(s->as.iff.test, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_stmt(s->as.iff.conseq, buf, len, cap, NULL, filter, meta_trace, explain,
                          stats);
    ny_expand_report_stmt(s->as.iff.alt, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    break;
  case NY_S_GUARD:
    ny_expand_report_expr(s->as.guard.value, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_stmt(s->as.guard.fallback, buf, len, cap, NULL, filter, meta_trace, explain,
                          stats);
    break;
  case NY_S_WHILE:
    ny_expand_report_stmt(s->as.whl.init, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    ny_expand_report_expr(s->as.whl.test, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_stmt(s->as.whl.update, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    ny_expand_report_stmt(s->as.whl.body, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    break;
  case NY_S_FOR:
    ny_expand_report_stmt(s->as.fr.init, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    ny_expand_report_expr(s->as.fr.cond, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_expr(s->as.fr.iterable, buf, len, cap, filter, meta_trace, stats);
    ny_expand_report_stmt(s->as.fr.update, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    ny_expand_report_stmt(s->as.fr.body, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    break;
  case NY_S_RETURN:
    ny_expand_report_expr(s->as.ret.value, buf, len, cap, filter, meta_trace, stats);
    break;
  case NY_S_DEFER:
    ny_expand_report_stmt(s->as.de.body, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    break;
  case NY_S_TRY:
    ny_expand_report_stmt(s->as.tr.body, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    ny_expand_report_stmt(s->as.tr.handler, buf, len, cap, NULL, filter, meta_trace, explain,
                          stats);
    break;
  case NY_S_MATCH:
    ny_expand_report_expr(s->as.match.test, buf, len, cap, filter, meta_trace, stats);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        ny_expand_report_expr(arm->patterns.data[j], buf, len, cap, filter, meta_trace, stats);
      ny_expand_report_expr(arm->guard, buf, len, cap, filter, meta_trace, stats);
      ny_expand_report_stmt(arm->conseq, buf, len, cap, NULL, filter, meta_trace, explain, stats);
    }
    ny_expand_report_stmt(s->as.match.default_conseq, buf, len, cap, NULL, filter, meta_trace,
                          explain, stats);
    break;
  case NY_S_IMPL:
    if (meta_trace && ny_expand_filter_match(filter, "impl", s->as.impl.type_name, NULL)) {
      ny_expand_emit_site(buf, len, cap, stats, "impl", s->as.impl.type_name, s->tok,
                          "impl block attaches method names to a concrete owner type",
                          "attached method table", ny_stmt_input_lines(s));
    }
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      ny_expand_report_stmt(s->as.impl.methods.data[i], buf, len, cap, NULL, filter, meta_trace,
                            explain, stats);
    break;
  default:
    break;
  }
}

char *ny_ast_expand_report(program_t *prog, const char *source_name, const char *filter,
                           const char *explain_specialization, bool meta_trace, bool include_json) {
  size_t len = 0, cap = 8192;
  char *buf = (char *)(uintptr_t)rt_malloc(cap);
  const char *tree_filter =
      (filter && *filter) ? filter : ((explain_specialization && *explain_specialization)
                                          ? explain_specialization
                                          : NULL);
  ny_expand_stats_t stats = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (ny_ast_token_in_file(s->tok, source_name))
      ny_expand_count_stmt(s, &stats);
  }

  append(&buf, &len, &cap, "%s%sEXPAND%s %s\n", clr(NY_CLR_BOLD), clr(NY_CLR_CYAN),
         clr(NY_CLR_RESET), source_name ? source_name : "<all>");
  ny_expand_report_rule(&buf, &len, &cap);
  ny_expand_report_kv(&buf, &len, &cap, "stats",
                      "fn=%zu ext=%zu layout=%zu struct=%zu module=%zu op=%zu macro=%zu ct=%zu call=%zu",
                      stats.funcs, stats.externs, stats.layouts, stats.structs, stats.modules,
                      stats.operators, stats.macros, stats.comptime_exprs, stats.calls);
  if (filter && *filter)
    ny_expand_report_kv(&buf, &len, &cap, "filter", "%s", filter);
  if (explain_specialization && *explain_specialization)
    ny_expand_report_kv(&buf, &len, &cap, "spec", "%s", explain_specialization);
  if (meta_trace)
    ny_expand_report_kv(&buf, &len, &cap, "trace", "calls/operators/extern/module/impl included");

  append(&buf, &len, &cap, "\n");
  ny_expand_report_section(&buf, &len, &cap, "graph");
  size_t before_sites = stats.matched_sites;
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    ny_expand_report_stmt(prog->body.data[i], &buf, &len, &cap, source_name, tree_filter, meta_trace,
                          explain_specialization, &stats);
  if (stats.matched_sites == before_sites)
    append(&buf, &len, &cap, "  %s(no expansion sites matched)%s\n", clr(NY_CLR_GRAY),
           clr(NY_CLR_RESET));
  ny_expand_report_kv(&buf, &len, &cap, "matched", "%zu site(s)",
                      stats.matched_sites - before_sites);
  ny_expand_report_kv(&buf, &len, &cap, "hint", "use --meta-trace for call/op graph; --expand-json for AST");

  if (include_json) {
    append(&buf, &len, &cap, "\n");
    ny_expand_report_section(&buf, &len, &cap, "ast-json");
    char *json =
        tree_filter && *tree_filter ? ny_ast_to_json_filtered_by_name(prog, source_name, tree_filter)
                                    : ny_ast_to_json_filtered(prog, source_name);
    append(&buf, &len, &cap, "%s\n", json ? json : "[]");
    if (json)
      rt_free((int64_t)(uintptr_t)json);
  }
  return buf;
}
