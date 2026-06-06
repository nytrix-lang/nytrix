#include "base/util.h"
#include "priv.h"
#include "systems.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LLVMValueRef ny_small_int_range_ok(codegen_t *cg, LLVMValueRef raw) {
  /* Nytrix small-ints are 62-bit signed (leaving 2 bits for tagging/safety).
   * A value fits if sign-extending from bit 61 to 63 yields the same value. */
  LLVMValueRef shift = LLVMConstInt(cg->type_i64, 2, false);
  LLVMValueRef shl = LLVMBuildShl(cg->builder, raw, shift, "small_shl");
  LLVMValueRef ashr = LLVMBuildAShr(cg->builder, shl, shift, "small_ashr");
  return ny_eq(cg, raw, ashr, "small_range_ok");
}

static bool ny_const_tagged_int(LLVMValueRef v, int64_t *out_raw) {
  if (!v || !LLVMIsAConstantInt(v))
    return false;
  int64_t tagged = LLVMConstIntGetSExtValue(v);
  if ((tagged & 1) == 0)
    return false;
  if (out_raw)
    *out_raw = tagged >> 1;
  return true;
}

static LLVMValueRef ny_const_tagged_int_value(codegen_t *cg, int64_t raw) {
  uint64_t tagged = (((uint64_t)raw) << 1) | 1u;
  return LLVMConstInt(cg->type_i64, tagged, false);
}

static LLVMValueRef ny_const_tagged_bool_value(codegen_t *cg, bool v) {
  return v ? ny_ctrue(cg) : ny_cfalse(cg);
}

static LLVMValueRef ny_tag_bool(codegen_t *cg, LLVMValueRef cond) {
  return ny_select(cg, cond, ny_ctrue(cg), ny_cfalse(cg), "tag_bool");
}

static LLVMValueRef ny_get_overflow_intrinsic_i64(codegen_t *cg, const char *name,
                                                  LLVMTypeRef *out_ft);

typedef struct {
  LLVMValueRef raw;
  LLVMValueRef ok;
} ny_raw_int_expr_t;

typedef struct {
  bool known;
  int64_t min_raw;
  int64_t max_raw;
} ny_int_range_t;

static bool ny_static_indexable_int_bounds(codegen_t *cg, scope *scopes, size_t depth, expr_t *target,
                                           int64_t *out_min, int64_t *out_max);
static bool ny_expr_is_ptr_like_for_arith(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                                          int recursion);

typedef enum {
  NY_CONST_NUM_NONE = 0,
  NY_CONST_NUM_INT,
  NY_CONST_NUM_FLOAT,
} ny_const_num_kind_t;

typedef struct {
  ny_const_num_kind_t kind;
  int64_t i;
  double f;
} ny_const_num_t;

static binding *ny_binary_lookup_binding(codegen_t *cg, scope *scopes, size_t depth,
                                         const char *name, size_t name_len, uint64_t hash) {
  return lookup_binding_hash(cg, scopes, depth, name, name_len, hash);
}

static bool ny_const_num_eval(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                              ny_const_num_t *out, int recursion) {
  if (!e || !out || recursion > 32)
    return false;
  memset(out, 0, sizeof(*out));

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      out->kind = NY_CONST_NUM_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      out->kind = NY_CONST_NUM_FLOAT;
      out->f = e->as.literal.as.f;
      return true;
    }
    return false;
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
    if (!b || b->is_mut)
      return false;
    if (b->has_int_range && b->int_min_raw == 0 && b->int_max_raw == 0) {
      out->kind = NY_CONST_NUM_INT;
      out->i = 0;
      return true;
    }
    return ny_const_num_eval(cg, scopes, depth, ny_binding_var_init_expr(b, e->as.ident.name),
                             out, recursion + 1);
  }
  case NY_E_UNARY: {
    ny_const_num_t r = {0};
    if (!ny_const_num_eval(cg, scopes, depth, e->as.unary.right, &r, recursion + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *out = r;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (r.kind == NY_CONST_NUM_INT) {
        if (r.i == INT64_MIN)
          return false;
        out->kind = NY_CONST_NUM_INT;
        out->i = -r.i;
        return true;
      }
      if (r.kind == NY_CONST_NUM_FLOAT) {
        out->kind = NY_CONST_NUM_FLOAT;
        out->f = -r.f;
        return true;
      }
    }
    return false;
  }
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op)
      return false;
    ny_const_num_t l = {0};
    ny_const_num_t r = {0};
    if (!ny_const_num_eval(cg, scopes, depth, e->as.binary.left, &l, recursion + 1) ||
        !ny_const_num_eval(cg, scopes, depth, e->as.binary.right, &r, recursion + 1))
      return false;

    if (l.kind == NY_CONST_NUM_INT && r.kind == NY_CONST_NUM_INT) {
      int64_t v = 0;
      if (strcmp(op, "+") == 0) {
        if (__builtin_add_overflow(l.i, r.i, &v))
          return false;
      } else if (strcmp(op, "-") == 0) {
        if (__builtin_sub_overflow(l.i, r.i, &v))
          return false;
      } else if (strcmp(op, "*") == 0) {
        if (__builtin_mul_overflow(l.i, r.i, &v))
          return false;
      } else if (strcmp(op, "/") == 0) {
        if (r.i == 0 || (l.i == INT64_MIN && r.i == -1))
          return false;
        v = l.i / r.i;
      } else if (strcmp(op, "%") == 0) {
        if (r.i == 0 || (l.i == INT64_MIN && r.i == -1))
          return false;
        v = l.i % r.i;
      } else {
        return false;
      }
      out->kind = NY_CONST_NUM_INT;
      out->i = v;
      return true;
    }

    if ((l.kind == NY_CONST_NUM_INT || l.kind == NY_CONST_NUM_FLOAT) &&
        (r.kind == NY_CONST_NUM_INT || r.kind == NY_CONST_NUM_FLOAT)) {
      double lf = l.kind == NY_CONST_NUM_INT ? (double)l.i : l.f;
      double rf = r.kind == NY_CONST_NUM_INT ? (double)r.i : r.f;
      if (strcmp(op, "+") == 0)
        out->f = lf + rf;
      else if (strcmp(op, "-") == 0)
        out->f = lf - rf;
      else if (strcmp(op, "*") == 0)
        out->f = lf * rf;
      else if (strcmp(op, "/") == 0) {
        if (rf == 0.0)
          return false;
        out->f = lf / rf;
      } else {
        return false;
      }
      out->kind = NY_CONST_NUM_FLOAT;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ny_expr_is_compile_time_zero(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  ny_const_num_t v = {0};
  if (!ny_const_num_eval(cg, scopes, depth, e, &v, 0))
    return false;
  if (v.kind == NY_CONST_NUM_INT)
    return v.i == 0;
  if (v.kind == NY_CONST_NUM_FLOAT)
    return v.f == 0.0;
  return false;
}

static void ny_warn_compile_time_zero_divisor(codegen_t *cg, scope *scopes, size_t depth,
                                              const char *op, expr_t *rhs) {
  if (!rhs || !op)
    return;
  bool is_div = strcmp(op, "/") == 0;
  bool is_mod = strcmp(op, "%") == 0;
  if (!is_div && !is_mod)
    return;
  if (!ny_expr_is_compile_time_zero(cg, scopes, depth, rhs))
    return;
  ny_diag_warning_code(rhs->tok, 2005, "%s by zero", is_mod ? "modulo" : "division");
  ny_diag_hint("right-hand side of '%s' is a compile-time zero; runtime will panic if executed",
               op);
}

static bool ny_get_call_is_unproven(codegen_t *cg, scope *scopes, size_t depth, expr_t *call) {
  if (!cg || !scopes || !call || call->kind != NY_E_CALL || !call->as.call.callee ||
      call->as.call.callee->kind != NY_E_IDENT || call->as.call.args.len < 2)
    return false;
  size_t n_len = 0;
  uint64_t n_hash = 0;
  const char *n = ny_builtin_surface_name_for_callee(
      call->as.call.callee, &n_len, &n_hash);
  bool builtin_shadowed = ny_builtin_name_shadowed_by_user_symbol(
      cg, scopes, depth, n, n_len, n_hash);
  bool want_builtin_get =
      !builtin_shadowed && n &&
      (strcmp(n, "get") == 0 || strcmp(n, "std.core.get") == 0 ||
       strcmp(n, "std.core.reflect.get") == 0);
  if (!want_builtin_get)
    return false;
  expr_t *target = call->as.call.args.data[0].val;
  if (!target)
    return true;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    size_t tlen = (size_t)target->tok.len;
    if (tlen == 0)
      tlen = strlen(target->as.ident.name);
    binding *tb =
        ny_binary_lookup_binding(cg, scopes, depth, target->as.ident.name, tlen,
                                 target->as.ident.hash);
    if (tb && (tb->is_int_list_storage || tb->is_int_dict_storage))
      return false;
  }
  int64_t min_v = 0, max_v = 0;
  return !ny_static_indexable_int_bounds(cg, scopes, depth, target, &min_v, &max_v);
}

static bool ny_expr_backed_by_unproven_get(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!cg || !scopes || !e)
    return false;
  if (e->kind == NY_E_CALL)
    return ny_get_call_is_unproven(cg, scopes, depth, e);
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b =
      ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
  expr_t *init = ny_binding_var_init_expr(b, e->as.ident.name);
  return ny_get_call_is_unproven(cg, scopes, depth, init);
}

static bool ny_static_indexable_int_bounds(codegen_t *cg, scope *scopes, size_t depth, expr_t *target,
                                           int64_t *out_min, int64_t *out_max) {
  expr_t *init = NULL;
  if (target && (target->kind == NY_E_LIST || target->kind == NY_E_TUPLE)) {
    init = target;
  } else if (target && target->kind == NY_E_IDENT && target->as.ident.name) {
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = ny_binary_lookup_binding(cg, scopes, depth, target->as.ident.name, name_len,
                                          target->as.ident.hash);
    init = ny_binding_var_init_expr(b, target->as.ident.name);
  }
  if (!init || (init->kind != NY_E_LIST && init->kind != NY_E_TUPLE) || init->as.list_like.len == 0)
    return false;

  int64_t min_v = 0;
  int64_t max_v = 0;
  for (size_t i = 0; i < init->as.list_like.len; ++i) {
    expr_t *item = init->as.list_like.data[i];
    if (!item || item->kind != NY_E_LITERAL || item->as.literal.kind != NY_LIT_INT)
      return false;
    int64_t v = item->as.literal.as.i;
    if (i == 0) {
      min_v = v;
      max_v = v;
    } else {
      if (v < min_v)
        min_v = v;
      if (v > max_v)
        max_v = v;
    }
  }
  if (out_min)
    *out_min = min_v;
  if (out_max)
    *out_max = max_v;
  return true;
}

static bool ny_expr_static_len(codegen_t *cg, scope *scopes, size_t depth, expr_t *target,
                               int64_t *out_len) {
  if (out_len)
    *out_len = 0;
  if (!target)
    return false;

  expr_t *init = NULL;
  if (target->kind == NY_E_LITERAL || target->kind == NY_E_LIST || target->kind == NY_E_TUPLE) {
    init = target;
  } else if (target->kind == NY_E_IDENT && target->as.ident.name) {
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = ny_binary_lookup_binding(cg, scopes, depth, target->as.ident.name, name_len,
                                          target->as.ident.hash);
    init = ny_binding_var_init_expr(b, target->as.ident.name);
  }

  if (!init)
    return false;
  int64_t len = -1;
  if (init->kind == NY_E_LITERAL && init->as.literal.kind == NY_LIT_STR)
    len = (int64_t)init->as.literal.as.s.len;
  else if (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE)
    len = (int64_t)init->as.list_like.len;
  if (len < 0)
    return false;
  if (out_len)
    *out_len = len;
  return true;
}

static ny_int_range_t ny_expr_proven_small_int_range(codegen_t *cg, scope *scopes, size_t depth,
                                                     expr_t *e);

typedef struct {
  const char *name;
  ny_int_range_t range;
} ny_param_int_range_t;

typedef struct {
  const char *name;
  LLVMValueRef raw;
  LLVMValueRef ok;
  ny_int_range_t range;
} ny_raw_int_param_t;

enum { NY_RAW_INT_CALL_STACK_MAX = 64 };

static _Thread_local const fun_sig
    *g_raw_int_call_stack[NY_RAW_INT_CALL_STACK_MAX];
static _Thread_local size_t g_raw_int_call_stack_len;

static bool ny_raw_int_call_active(const fun_sig *sig) {
  if (!sig)
    return false;
  for (size_t i = 0; i < g_raw_int_call_stack_len; ++i)
    if (g_raw_int_call_stack[i] == sig)
      return true;
  return false;
}

static bool ny_raw_int_call_push(const fun_sig *sig) {
  if (!sig || ny_raw_int_call_active(sig) ||
      g_raw_int_call_stack_len >= NY_RAW_INT_CALL_STACK_MAX)
    return false;
  g_raw_int_call_stack[g_raw_int_call_stack_len++] = sig;
  return true;
}

static void ny_raw_int_call_pop(const fun_sig *sig) {
  if (g_raw_int_call_stack_len == 0)
    return;
  if (g_raw_int_call_stack[g_raw_int_call_stack_len - 1] == sig) {
    g_raw_int_call_stack_len--;
    return;
  }
  for (size_t i = g_raw_int_call_stack_len; i > 0; --i) {
    if (g_raw_int_call_stack[i - 1] == sig) {
      memmove(&g_raw_int_call_stack[i - 1], &g_raw_int_call_stack[i],
              (g_raw_int_call_stack_len - i) *
                  sizeof(g_raw_int_call_stack[0]));
      g_raw_int_call_stack_len--;
      return;
    }
  }
}

static expr_t *ny_single_return_expr(stmt_t *s) {
  if (!s)
    return NULL;
  if (s->kind == NY_S_RETURN)
    return s->as.ret.value;
  if (s->kind == NY_S_BLOCK && s->as.block.body.len == 1)
    return ny_single_return_expr(s->as.block.body.data[0]);
  return NULL;
}

static ny_int_range_t ny_checked_int_range(int64_t lo, int64_t hi) {
  ny_int_range_t fail = {false, 0, 0};
  if (!ny_small_int_fits_i64(lo) || !ny_small_int_fits_i64(hi))
    return fail;
  return (ny_int_range_t){true, lo, hi};
}

static ny_int_range_t ny_mask_literal_int_range(expr_t *rhs) {
  ny_int_range_t fail = {false, 0, 0};
  if (!rhs || rhs->kind != NY_E_LITERAL || rhs->as.literal.kind != NY_LIT_INT ||
      rhs->as.literal.as.i < 0 || !ny_small_int_fits_i64(rhs->as.literal.as.i))
    return fail;
  return (ny_int_range_t){true, 0, rhs->as.literal.as.i};
}

static ny_int_range_t ny_binary_small_int_range(const char *op, ny_int_range_t l,
                                                ny_int_range_t r) {
  ny_int_range_t fail = {false, 0, 0};
  if (!op || !l.known || !r.known)
    return fail;

  if (strcmp(op, "+") == 0) {
    int64_t lo = 0, hi = 0;
    if (!ny_add_range_ok(l.min_raw, r.min_raw, &lo) ||
        !ny_add_range_ok(l.max_raw, r.max_raw, &hi))
      return fail;
    return ny_checked_int_range(lo, hi);
  }
  if (strcmp(op, "-") == 0) {
    int64_t lo = 0, hi = 0;
    if (!ny_sub_range_ok(l.min_raw, r.max_raw, &lo) ||
        !ny_sub_range_ok(l.max_raw, r.min_raw, &hi))
      return fail;
    return ny_checked_int_range(lo, hi);
  }
  if (strcmp(op, "*") == 0) {
    int64_t c[4];
    if (!ny_mul_range_ok(l.min_raw, r.min_raw, &c[0]) ||
        !ny_mul_range_ok(l.min_raw, r.max_raw, &c[1]) ||
        !ny_mul_range_ok(l.max_raw, r.min_raw, &c[2]) ||
        !ny_mul_range_ok(l.max_raw, r.max_raw, &c[3]))
      return fail;
    int64_t lo = c[0], hi = c[0];
    for (int i = 1; i < 4; ++i) {
      if (c[i] < lo)
        lo = c[i];
      if (c[i] > hi)
        hi = c[i];
    }
    return ny_checked_int_range(lo, hi);
  }
  if (strcmp(op, "/") == 0 && r.min_raw == r.max_raw && r.max_raw > 0) {
    int64_t lo = l.min_raw / r.max_raw;
    int64_t hi = l.max_raw / r.max_raw;
    if (lo > hi) {
      int64_t tmp = lo;
      lo = hi;
      hi = tmp;
    }
    return ny_checked_int_range(lo, hi);
  }
  if (strcmp(op, "%") == 0 && r.min_raw == r.max_raw && r.max_raw > 0) {
    int64_t hi = r.max_raw - 1;
    int64_t lo = l.min_raw >= 0 ? 0 : -hi;
    return ny_checked_int_range(lo, hi);
  }
  if (strcmp(op, "&") == 0 && r.min_raw == r.max_raw && r.max_raw >= 0 &&
      l.min_raw >= 0)
    return ny_checked_int_range(0, r.max_raw);
  if (strcmp(op, ">>") == 0 && r.min_raw == r.max_raw &&
      r.min_raw >= 0 && r.min_raw < 64 && l.min_raw >= 0) {
    unsigned shift = (unsigned)r.min_raw;
    return ny_checked_int_range((int64_t)((uint64_t)l.min_raw >> shift),
                                (int64_t)((uint64_t)l.max_raw >> shift));
  }
  return fail;
}

static ny_int_range_t ny_expr_range_with_params(codegen_t *cg, scope *scopes, size_t depth,
                                                expr_t *e, const ny_param_int_range_t *params,
                                                size_t param_count, int recursion) {
  ny_int_range_t fail = {false, 0, 0};
  if (!e || recursion > 24)
    return fail;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT && ny_small_int_fits_i64(e->as.literal.as.i))
      return (ny_int_range_t){true, e->as.literal.as.i, e->as.literal.as.i};
    return fail;
  case NY_E_IDENT:
    if (e->as.ident.name) {
      for (size_t i = 0; i < param_count; ++i) {
        if (params[i].name && strcmp(params[i].name, e->as.ident.name) == 0)
          return params[i].range;
      }
    }
    return ny_expr_proven_small_int_range(cg, scopes, depth, e);
  case NY_E_UNARY:
    if (e->as.unary.op && strcmp(e->as.unary.op, "+") == 0)
      return ny_expr_range_with_params(cg, scopes, depth, e->as.unary.right, params,
                                       param_count, recursion + 1);
    if (e->as.unary.op && strcmp(e->as.unary.op, "-") == 0) {
      ny_int_range_t r = ny_expr_range_with_params(cg, scopes, depth, e->as.unary.right,
                                                   params, param_count, recursion + 1);
      if (!r.known || r.min_raw == INT64_MIN || r.max_raw == INT64_MIN)
        return fail;
      int64_t lo = -r.max_raw;
      int64_t hi = -r.min_raw;
      if (!ny_small_int_fits_i64(lo) || !ny_small_int_fits_i64(hi))
        return fail;
      return (ny_int_range_t){true, lo, hi};
    }
    return fail;
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op)
      return fail;
    if (strcmp(op, "&") == 0) {
      ny_int_range_t mask = ny_mask_literal_int_range(e->as.binary.right);
      if (mask.known)
        return mask;
    }
    ny_int_range_t l = ny_expr_range_with_params(cg, scopes, depth, e->as.binary.left,
                                                 params, param_count, recursion + 1);
    ny_int_range_t r = ny_expr_range_with_params(cg, scopes, depth, e->as.binary.right,
                                                 params, param_count, recursion + 1);
    return ny_binary_small_int_range(op, l, r);
  }
  default:
    return fail;
  }
}

static ny_int_range_t ny_call_return_small_int_range(codegen_t *cg, scope *scopes, size_t depth,
                                                     expr_t *call) {
  ny_int_range_t fail = {false, 0, 0};
  if (!call || call->kind != NY_E_CALL || !call->as.call.callee ||
      call->as.call.callee->kind != NY_E_IDENT)
    return fail;
  const char *name = call->as.call.callee->as.ident.name;
  if (!name || call->as.call.args.len > 16)
    return fail;
  fun_sig *sig =
      resolve_overload(cg, name, call->as.call.args.len, call->as.call.callee->as.ident.hash);
  if (!sig || sig->is_extern || sig->is_variadic || sig->is_recursive ||
      !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return fail;
  if (ny_name_tail_is(name, "now_ms") && ny_is_stdlib_tok(sig->stmt_t->tok))
    return (ny_int_range_t){true, 0, NY_SMALL_INT_MAX};
  stmt_t *body = sig->stmt_t->as.fn.body;
  expr_t *ret = ny_single_return_expr(body);
  if (!ret || sig->stmt_t->as.fn.params.len < call->as.call.args.len)
    return fail;
  if (ret->kind == NY_E_CALL && ret->as.call.callee &&
      ret->as.call.callee->kind == NY_E_IDENT &&
      ret->as.call.callee->as.ident.name &&
      ny_name_tail_is(ret->as.call.callee->as.ident.name,
                      "__time_milliseconds") &&
      ret->as.call.args.len == 0)
    return (ny_int_range_t){true, 0, NY_SMALL_INT_MAX};
  ny_param_int_range_t params[16] = {0};
  for (size_t i = 0; i < call->as.call.args.len; ++i) {
    params[i].name = sig->stmt_t->as.fn.params.data[i].name;
    params[i].range =
        ny_expr_proven_small_int_range(cg, scopes, depth, call->as.call.args.data[i].val);
    if (!params[i].name || !params[i].range.known)
      return fail;
  }
  return ny_expr_range_with_params(cg, scopes, depth, ret, params, call->as.call.args.len, 0);
}

static ny_int_range_t ny_expr_proven_small_int_range(codegen_t *cg, scope *scopes, size_t depth,
                                                      expr_t *e) {
  ny_int_range_t fail = {false, 0, 0};
  if (!e)
    return fail;

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT && ny_small_int_fits_i64(e->as.literal.as.i))
      return (ny_int_range_t){true, e->as.literal.as.i, e->as.literal.as.i};
    return fail;
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return fail;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
    if (b && b->has_int_range)
      return (ny_int_range_t){true, b->int_min_raw, b->int_max_raw};
    expr_t *init = b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
    if (init)
      return ny_expr_proven_small_int_range(cg, scopes, depth, init);
    return fail;
  }
  case NY_E_UNARY:
    if (!e->as.unary.op || !e->as.unary.right)
      return fail;
    if (strcmp(e->as.unary.op, "+") == 0)
      return ny_expr_proven_small_int_range(cg, scopes, depth, e->as.unary.right);
    if (strcmp(e->as.unary.op, "-") == 0) {
      ny_int_range_t r = ny_expr_proven_small_int_range(cg, scopes, depth, e->as.unary.right);
      if (!r.known || r.min_raw == INT64_MIN || r.max_raw == INT64_MIN)
        return fail;
      int64_t lo = -r.max_raw;
      int64_t hi = -r.min_raw;
      if (!ny_small_int_fits_i64(lo) || !ny_small_int_fits_i64(hi))
        return fail;
      return (ny_int_range_t){true, lo, hi};
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      ny_int_range_t r = ny_expr_proven_small_int_range(cg, scopes, depth, e->as.unary.right);
      if (!r.known || r.min_raw == INT64_MIN || r.max_raw == INT64_MIN)
        return fail;
      int64_t lo = ~r.max_raw;
      int64_t hi = ~r.min_raw;
      if (!ny_small_int_fits_i64(lo) || !ny_small_int_fits_i64(hi))
        return fail;
      return (ny_int_range_t){true, lo, hi};
    }
    return fail;
  case NY_E_CALL: {
    ny_int_range_t call_range = ny_call_return_small_int_range(cg, scopes, depth, e);
    if (call_range.known)
      return call_range;
    if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT)
      return fail;
    size_t n_len = 0;
    uint64_t n_hash = 0;
    const char *n = ny_builtin_surface_name_for_callee(
        e->as.call.callee, &n_len, &n_hash);
    bool builtin_shadowed =
        ny_builtin_name_shadowed_by_user_symbol(cg, scopes, depth, n, n_len,
                                                n_hash);
    if (!builtin_shadowed && n && ny_name_tail_is(n, "__time_milliseconds") &&
        e->as.call.args.len == 0)
      return (ny_int_range_t){true, 0, NY_SMALL_INT_MAX};
    if (!builtin_shadowed && n && ny_name_tail_is(n, "len") &&
        e->as.call.args.len == 1) {
      int64_t len = 0;
      if (ny_expr_static_len(cg, scopes, depth, e->as.call.args.data[0].val, &len) &&
          ny_small_int_fits_i64(len))
        return (ny_int_range_t){true, len, len};
      return fail;
    }
    if (!builtin_shadowed && n && e->as.call.args.len == 2 &&
        (ny_name_tail_is(n, "load8") || strcmp(n, "__load8_idx") == 0))
      return (ny_int_range_t){true, 0, 255};
    if (!builtin_shadowed && n && e->as.call.args.len == 2 &&
        (ny_name_tail_is(n, "load16") || strcmp(n, "__load16_idx") == 0))
      return (ny_int_range_t){true, 0, 65535};
    if (!builtin_shadowed && n && e->as.call.args.len == 2 &&
        (ny_name_tail_is(n, "load32") || strcmp(n, "__load32_idx") == 0 ||
         ny_name_tail_is(n, "load32_h")))
      return (ny_int_range_t){true, 0, UINT32_MAX};
    if (!builtin_shadowed && n && ny_name_tail_is(n, "band") &&
        e->as.call.args.len == 2) {
      expr_t *rhs = e->as.call.args.data[1].val;
      if (rhs && rhs->kind == NY_E_LITERAL && rhs->as.literal.kind == NY_LIT_INT &&
          rhs->as.literal.as.i >= 0 &&
          ny_is_proven_int(cg, scopes, depth, e->as.call.args.data[0].val, NULL) &&
          ny_small_int_fits_i64(rhs->as.literal.as.i))
        return (ny_int_range_t){true, 0, rhs->as.literal.as.i};
    }
    if (e->as.call.args.len < 2)
      return fail;
    bool want_builtin_get =
        !builtin_shadowed && n &&
        (strcmp(n, "get") == 0 || strcmp(n, "std.core.get") == 0 ||
              strcmp(n, "std.core.reflect.get") == 0 || ny_name_tail_is(n, "get"));
    if (!want_builtin_get)
      return fail;
    expr_t *target = e->as.call.args.data[0].val;
    if (target && target->kind == NY_E_IDENT && target->as.ident.name) {
      size_t name_len = (size_t)target->tok.len;
      if (name_len == 0)
        name_len = strlen(target->as.ident.name);
      binding *b = ny_binary_lookup_binding(cg, scopes, depth, target->as.ident.name, name_len,
                                            target->as.ident.hash);
      if (b && b->is_int_list_storage && b->has_list_int_range)
        return (ny_int_range_t){true, b->list_int_min_raw, b->list_int_max_raw};
      if (b && b->is_int_dict_storage && b->has_dict_int_range)
        return (ny_int_range_t){true, b->dict_int_min_raw, b->dict_int_max_raw};
    }
    int64_t min_v = 0, max_v = 0;
    if (ny_static_indexable_int_bounds(cg, scopes, depth, target, &min_v, &max_v))
      return (ny_int_range_t){true, min_v, max_v};
    return fail;
  }
  case NY_E_MEMCALL: {
    if (!e->as.memcall.name || !ny_name_tail_is(e->as.memcall.name, "get") ||
        !e->as.memcall.target)
      return fail;
    expr_t *target = e->as.memcall.target;
    if (target->kind == NY_E_IDENT && target->as.ident.name) {
      size_t name_len = (size_t)target->tok.len;
      if (name_len == 0)
        name_len = strlen(target->as.ident.name);
      binding *b = ny_binary_lookup_binding(cg, scopes, depth,
                                            target->as.ident.name, name_len,
                                            target->as.ident.hash);
      if (b && b->is_int_list_storage && b->has_list_int_range)
        return (ny_int_range_t){true, b->list_int_min_raw, b->list_int_max_raw};
      if (b && b->is_int_dict_storage && b->has_dict_int_range)
        return (ny_int_range_t){true, b->dict_int_min_raw, b->dict_int_max_raw};
    }
    int64_t min_v = 0, max_v = 0;
    if (ny_static_indexable_int_bounds(cg, scopes, depth, target, &min_v,
                                       &max_v))
      return (ny_int_range_t){true, min_v, max_v};
    return fail;
  }
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (op && strcmp(op, "&") == 0) {
      ny_int_range_t mask = ny_mask_literal_int_range(e->as.binary.right);
      if (mask.known && ny_is_proven_int(cg, scopes, depth, e->as.binary.left, NULL))
        return mask;
    }
    ny_int_range_t l = ny_expr_proven_small_int_range(cg, scopes, depth, e->as.binary.left);
    ny_int_range_t r = ny_expr_proven_small_int_range(cg, scopes, depth, e->as.binary.right);
    return ny_binary_small_int_range(op, l, r);
  }
  default:
    return fail;
  }
}

static bool ny_expr_is_int_typed(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!e)
    return false;
  return ny_is_proven_int(cg, scopes, depth, e, NULL);
}

static bool ny_can_lower_raw_int_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!e || !ny_expr_is_int_typed(cg, scopes, depth, e))
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_INT;
  case NY_E_IDENT:
    return true;
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0 &&
        strcmp(op, "%") != 0 && strcmp(op, "/") != 0 && strcmp(op, "&") != 0 &&
        strcmp(op, ">>") != 0)
      return false;
    if (strcmp(op, "%") == 0 || strcmp(op, "/") == 0) {
      int64_t rhs_lit = 0;
      if (!ny_expr_literal_i64(e->as.binary.right, &rhs_lit) || rhs_lit <= 0)
        return false;
      return ny_can_lower_raw_int_expr(cg, scopes, depth, e->as.binary.left);
    }
    if (strcmp(op, ">>") == 0) {
      int64_t rhs_lit = 0;
      ny_int_range_t lhs_range =
          ny_expr_proven_small_int_range(cg, scopes, depth, e->as.binary.left);
      if (!ny_expr_literal_i64(e->as.binary.right, &rhs_lit) ||
          rhs_lit < 0 || rhs_lit >= 64 || !lhs_range.known ||
          lhs_range.min_raw < 0)
        return false;
      return ny_can_lower_raw_int_expr(cg, scopes, depth, e->as.binary.left);
    }
    return ny_can_lower_raw_int_expr(cg, scopes, depth, e->as.binary.left) &&
           ny_can_lower_raw_int_expr(cg, scopes, depth, e->as.binary.right);
  }
  default:
    return false;
  }
}

static ny_int_range_t ny_raw_int_expr_range_with_params(codegen_t *cg, scope *scopes,
                                                        size_t depth, expr_t *e,
                                                        const ny_raw_int_param_t *params,
                                                        size_t param_count) {
  if (param_count == 0)
    return ny_expr_proven_small_int_range(cg, scopes, depth, e);
  ny_param_int_range_t ranges[16] = {0};
  if (param_count > 16)
    return (ny_int_range_t){false, 0, 0};
  for (size_t i = 0; i < param_count; ++i) {
    ranges[i].name = params[i].name;
    ranges[i].range = params[i].range;
  }
  return ny_expr_range_with_params(cg, scopes, depth, e, ranges, param_count, 0);
}

static bool ny_mask_literal_i64(expr_t *e, int64_t *out);

static ny_raw_int_expr_t ny_lower_raw_int_expr_with_params(codegen_t *cg, scope *scopes,
                                                           size_t depth, expr_t *e,
                                                           const ny_raw_int_param_t *params,
                                                           size_t param_count,
                                                           int recursion) {
  ny_raw_int_expr_t fail = {NULL, LLVMConstInt(cg->type_i1, 0, false)};
  if (!e || recursion > 24)
    return fail;

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind != NY_LIT_INT)
      return fail;
    return (ny_raw_int_expr_t){LLVMConstInt(cg->type_i64, (uint64_t)e->as.literal.as.i, true),
                               LLVMConstInt(cg->type_i1, 1, false)};
  case NY_E_IDENT: {
    if (e->as.ident.name) {
      for (size_t i = 0; i < param_count; ++i) {
        if (params[i].name && strcmp(params[i].name, e->as.ident.name) == 0)
          return (ny_raw_int_expr_t){params[i].raw, params[i].ok};
      }
    }
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
    if (b && b->raw_int_value && b->is_int_direct) {
      LLVMValueRef raw = b->raw_int_value;
      return (ny_raw_int_expr_t){raw, LLVMConstInt(cg->type_i1, 1, false)};
    }
    if (ny_env_enabled("NYTRIX_RAW_INT_SLOT_EXPR_FAST") && b &&
        b->raw_int_value && b->is_int_slot && !ny_binding_is_valid(cg, b)) {
      LLVMValueRef raw = ny_load(cg, b->raw_int_value, NY_LLVM_NAME(cg, "rawi.slot"));
      return (ny_raw_int_expr_t){raw, LLVMConstInt(cg->type_i1, 1, false)};
    }
    expr_t *init = b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
    if (init) {
      ny_raw_int_expr_t lowered =
          ny_lower_raw_int_expr_with_params(cg, scopes, depth, init, params, param_count,
                                            recursion + 1);
      if (lowered.raw)
        return lowered;
    }
    LLVMValueRef tagged = gen_expr(cg, scopes, depth, e);
    LLVMValueRef ok = b && (b->is_int_slot || b->is_int_direct)
                          ? LLVMConstInt(cg->type_i1, 1, false)
                          : ny_is_tagged_int(cg, tagged);
    return (ny_raw_int_expr_t){ny_untag_int(cg, tagged), ok};
  }
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    int64_t mask = 0;
    if (op && strcmp(op, "&") == 0 && ny_mask_literal_i64(e->as.binary.right, &mask)) {
      ny_int_range_t lhs_range =
          ny_raw_int_expr_range_with_params(cg, scopes, depth, e->as.binary.left, params,
                                            param_count);
      if (!lhs_range.known)
        return fail;
      ny_raw_int_expr_t lhs = ny_lower_raw_int_expr_with_params(
          cg, scopes, depth, e->as.binary.left, params, param_count, recursion + 1);
      if (!lhs.raw)
        return fail;
      LLVMValueRef raw = LLVMBuildAnd(cg->builder, lhs.raw,
                                      LLVMConstInt(cg->type_i64, (uint64_t)mask, true),
                                      NY_LLVM_NAME(cg, "rawi_masked"));
      if (cg)
        cg->mono_masked_range_uses++;
      return (ny_raw_int_expr_t){raw, lhs.ok};
    }
    if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0 &&
        strcmp(op, "%") != 0 && strcmp(op, "/") != 0 && strcmp(op, "&") != 0 &&
        strcmp(op, ">>") != 0)
      return fail;

    if (strcmp(op, "%") == 0 || strcmp(op, "/") == 0) {
      int64_t rhs_lit = 0;
      if (!ny_expr_literal_i64(e->as.binary.right, &rhs_lit) || rhs_lit <= 0)
        return fail;
      ny_int_range_t lhs_range =
          ny_raw_int_expr_range_with_params(cg, scopes, depth, e->as.binary.left, params,
                                            param_count);
      ny_raw_int_expr_t lhs =
          ny_lower_raw_int_expr_with_params(cg, scopes, depth, e->as.binary.left, params,
                                            param_count, recursion + 1);
      if (!lhs.raw)
        return fail;
      bool lhs_nonnegative = lhs_range.known && lhs_range.min_raw >= 0;
      LLVMValueRef rhs = LLVMConstInt(cg->type_i64, (uint64_t)rhs_lit,
                                      !lhs_nonnegative);
      LLVMValueRef raw =
          strcmp(op, "%") == 0
              ? (lhs_nonnegative
                     ? LLVMBuildURem(cg->builder, lhs.raw, rhs,
                                     NY_LLVM_NAME(cg, "rawi_urem"))
                     : LLVMBuildSRem(cg->builder, lhs.raw, rhs,
                                     NY_LLVM_NAME(cg, "rawi_rem")))
              : (lhs_nonnegative
                     ? LLVMBuildUDiv(cg->builder, lhs.raw, rhs,
                                     NY_LLVM_NAME(cg, "rawi_udiv"))
                     : LLVMBuildSDiv(cg->builder, lhs.raw, rhs,
                                     NY_LLVM_NAME(cg, "rawi_div")));
      return (ny_raw_int_expr_t){raw, lhs.ok};
    }

    if (strcmp(op, ">>") == 0) {
      int64_t rhs_lit = 0;
      if (!ny_expr_literal_i64(e->as.binary.right, &rhs_lit) ||
          rhs_lit < 0 || rhs_lit >= 64)
        return fail;
      ny_int_range_t lhs_range =
          ny_raw_int_expr_range_with_params(cg, scopes, depth, e->as.binary.left, params,
                                            param_count);
      if (!lhs_range.known || lhs_range.min_raw < 0)
        return fail;
      ny_raw_int_expr_t lhs =
          ny_lower_raw_int_expr_with_params(cg, scopes, depth, e->as.binary.left, params,
                                            param_count, recursion + 1);
      if (!lhs.raw)
        return fail;
      LLVMValueRef raw =
          LLVMBuildLShr(cg->builder, lhs.raw,
                        LLVMConstInt(cg->type_i64, (uint64_t)rhs_lit, false),
                        NY_LLVM_NAME(cg, "rawi_shr"));
      return (ny_raw_int_expr_t){raw, lhs.ok};
    }

    ny_raw_int_expr_t lhs =
        ny_lower_raw_int_expr_with_params(cg, scopes, depth, e->as.binary.left, params,
                                          param_count, recursion + 1);
    ny_raw_int_expr_t rhs =
        ny_lower_raw_int_expr_with_params(cg, scopes, depth, e->as.binary.right, params,
                                          param_count, recursion + 1);
    if (!lhs.raw || !rhs.raw)
      return fail;

    if (strcmp(op, "&") == 0) {
      LLVMValueRef raw =
          LLVMBuildAnd(cg->builder, lhs.raw, rhs.raw, NY_LLVM_NAME(cg, "rawi_and"));
      LLVMValueRef ok = ny_and(cg, lhs.ok, rhs.ok, "rawi_ok_and");
      return (ny_raw_int_expr_t){raw, ok};
    }

    ny_int_range_t range =
        ny_raw_int_expr_range_with_params(cg, scopes, depth, e, params, param_count);
    if (range.known) {
      LLVMValueRef raw = NULL;
      if (strcmp(op, "+") == 0)
        raw = LLVMBuildAdd(cg->builder, lhs.raw, rhs.raw, NY_LLVM_NAME(cg, "rawi_add"));
      else if (strcmp(op, "-") == 0)
        raw = LLVMBuildSub(cg->builder, lhs.raw, rhs.raw, NY_LLVM_NAME(cg, "rawi_sub"));
      else
        raw = LLVMBuildMul(cg->builder, lhs.raw, rhs.raw, NY_LLVM_NAME(cg, "rawi_mul"));
      LLVMValueRef ok = ny_and(cg, lhs.ok, rhs.ok, "rawi_ok_range");
      return (ny_raw_int_expr_t){raw, ok};
    }

    const char *intr_name = strcmp(op, "+") == 0   ? "llvm.sadd.with.overflow.i64"
                            : strcmp(op, "-") == 0 ? "llvm.ssub.with.overflow.i64"
                                                    : "llvm.smul.with.overflow.i64";
    LLVMTypeRef intr_ty = NULL;
    LLVMValueRef intr = ny_get_overflow_intrinsic_i64(cg, intr_name, &intr_ty);
    LLVMValueRef packed = LLVMBuildCall2(cg->builder, intr_ty, intr,
                                         (LLVMValueRef[]){lhs.raw, rhs.raw}, 2, "rawi_packed");
    LLVMValueRef raw = LLVMBuildExtractValue(cg->builder, packed, 0, NY_LLVM_NAME(cg, "rawi"));
    LLVMValueRef ov =
        LLVMBuildExtractValue(cg->builder, packed, 1, NY_LLVM_NAME(cg, "rawi_ov"));
    LLVMValueRef ok = ny_and(cg, lhs.ok, rhs.ok, "rawi_ok_lr");
    ok = ny_and(cg, ok, LLVMBuildNot(cg->builder, ov, "rawi_no_ov"), "rawi_ok");
    LLVMValueRef small_shl = LLVMBuildShl(cg->builder, raw, LLVMConstInt(cg->type_i64, 2, false),
                                          NY_LLVM_NAME(cg, "rawi_small_shl"));
    LLVMValueRef small_ashr =
        LLVMBuildAShr(cg->builder, small_shl, LLVMConstInt(cg->type_i64, 2, false),
                      NY_LLVM_NAME(cg, "rawi_small_ashr"));
    LLVMValueRef small_ok = ny_eq(cg, raw, small_ashr, NY_LLVM_NAME(cg, "rawi_small_ok"));
    ok = ny_and(cg, ok, small_ok, "rawi_ok_small");
    return (ny_raw_int_expr_t){raw, ok};
  }
  case NY_E_CALL: {
    if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT ||
        e->as.call.args.len > 16)
      return fail;
    const char *name = e->as.call.callee->as.ident.name;
    if (!name)
      return fail;
    fun_sig *sig =
        resolve_overload(cg, name, e->as.call.args.len, e->as.call.callee->as.ident.hash);
    if (!sig || sig->is_extern || sig->is_variadic || sig->is_recursive ||
        !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
        sig->stmt_t->as.fn.params.len < e->as.call.args.len)
      return fail;
    expr_t *ret = ny_single_return_expr(sig->stmt_t->as.fn.body);
    if (!ret)
      return fail;

    ny_raw_int_param_t call_params[16] = {0};
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      expr_t *arg = e->as.call.args.data[i].val;
      ny_raw_int_expr_t raw_arg =
          ny_lower_raw_int_expr_with_params(cg, scopes, depth, arg, params, param_count,
                                            recursion + 1);
      if (!raw_arg.raw || !raw_arg.ok)
        return fail;
      ny_int_range_t arg_range =
          ny_raw_int_expr_range_with_params(cg, scopes, depth, arg, params, param_count);
      if (!arg_range.known)
        return fail;
      call_params[i] = (ny_raw_int_param_t){
          .name = sig->stmt_t->as.fn.params.data[i].name,
          .raw = raw_arg.raw,
          .ok = raw_arg.ok,
          .range = arg_range,
      };
      if (!call_params[i].name)
        return fail;
    }
    if (!ny_raw_int_call_push(sig))
      return fail;
    ny_raw_int_expr_t lowered =
        ny_lower_raw_int_expr_with_params(cg, scopes, depth, ret, call_params,
                                          e->as.call.args.len, recursion + 1);
    ny_raw_int_call_pop(sig);
    return lowered;
  }
  default:
    return fail;
  }
}

static bool ny_mask_literal_i64(expr_t *e, int64_t *out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_INT ||
      e->as.literal.as.i < 0 || !ny_small_int_fits_i64(e->as.literal.as.i))
    return false;
  if (out)
    *out = e->as.literal.as.i;
  return true;
}

static ny_raw_int_expr_t ny_lower_raw_int_expr(codegen_t *cg, scope *scopes, size_t depth,
                                               expr_t *e) {
  return ny_lower_raw_int_expr_with_params(cg, scopes, depth, e, NULL, 0, 0);
}

bool ny_build_mono_raw_int_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                                LLVMValueRef *raw_out, LLVMValueRef *ok_out) {
  if (!cg || !raw_out || !ok_out || !e)
    return false;
  ny_raw_int_expr_t lowered = ny_lower_raw_int_expr(cg, scopes, depth, e);
  if (!lowered.raw || !lowered.ok)
    return false;
  *raw_out = lowered.raw;
  *ok_out = lowered.ok;
  return true;
}

static LLVMValueRef ny_try_emit_proven_int_modexpr_fast(codegen_t *cg, scope *scopes,
                                                        size_t depth, LLVMValueRef fn,
                                                        LLVMValueRef l, LLVMValueRef r, expr_t *le,
                                                        expr_t *re, fun_sig *fallback) {
  (void)re;
  if (!fallback || !le || !ny_expr_is_int_typed(cg, scopes, depth, le))
    return NULL;

  int64_t mod_raw = 0;
  if (!ny_const_tagged_int(r, &mod_raw) || mod_raw <= 0)
    return NULL;
  if (!ny_can_lower_raw_int_expr(cg, scopes, depth, le))
    return NULL;

  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "bin.modexpr.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "bin.modexpr.slow");
  LLVMBasicBlockRef merge_bb = ny_bb_fn(fn, "bin.modexpr.merge");

  ny_br(cg, fast_bb);
  ny_pos(cg, fast_bb);

  ny_raw_int_expr_t lhs = ny_lower_raw_int_expr(cg, scopes, depth, le);
  LLVMBasicBlockRef fast_done_bb = NULL;
  LLVMValueRef fast_value = NULL;

  LLVMValueRef lhs_ok = lhs.ok ? lhs.ok : LLVMConstInt(cg->type_i1, 0, false);
  LLVMBasicBlockRef fast_ok_bb = ny_bb_fn(fn, "bin.modexpr.fast.ok");
  ny_cond_br(cg, lhs_ok, fast_ok_bb, slow_bb);

  ny_pos(cg, fast_ok_bb);
  LLVMValueRef divisor = LLVMConstInt(cg->type_i64, (uint64_t)mod_raw, true);
  ny_int_range_t lhs_range = ny_expr_proven_small_int_range(cg, scopes, depth, le);
  bool lhs_nonnegative = lhs_range.known && lhs_range.min_raw >= 0;
  LLVMValueRef raw =
      lhs_nonnegative
          ? LLVMBuildURem(cg->builder, lhs.raw,
                          LLVMConstInt(cg->type_i64, (uint64_t)mod_raw, false),
                          NY_LLVM_NAME(cg, "modexpr_urem"))
          : LLVMBuildSRem(cg->builder, lhs.raw, divisor,
                          NY_LLVM_NAME(cg, "modexpr"));
  fast_value = ny_tag_int(cg, raw);
  fast_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, fallback->type, fallback->value,
                                           (LLVMValueRef[]){l, r}, 2, "bin.modexpr.slow");
  LLVMBasicBlockRef slow_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, merge_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "modexpr_res"));
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_value, slow_value},
                  (LLVMBasicBlockRef[]){fast_done_bb, slow_done_bb}, 2);
  return phi;
}

static LLVMValueRef ny_try_emit_proven_int_mod_fast(codegen_t *cg, scope *scopes, size_t depth,
                                                    LLVMValueRef l, LLVMValueRef r, expr_t *le,
                                                    expr_t *re, ny_int_range_t range_r) {
  if (!ny_fast_path_enabled(cg, "NYTRIX_PROVEN_INT_MOD_FAST"))
    return NULL;
  if (!ny_is_proven_int(cg, scopes, depth, le, l) || !ny_is_proven_int(cg, scopes, depth, re, r))
    return NULL;

  int64_t divisor = 0;
  bool divisor_positive = range_r.known && range_r.min_raw > 0;
  if (!divisor_positive && ny_const_tagged_int(r, &divisor))
    divisor_positive = divisor > 0;
  if (!divisor_positive)
    return NULL;

  LLVMValueRef li = ny_untag_int(cg, l);
  LLVMValueRef ri = ny_untag_int(cg, r);
  ny_int_range_t range_l = ny_expr_proven_small_int_range(cg, scopes, depth, le);
  bool lhs_nonnegative = range_l.known && range_l.min_raw >= 0;
  LLVMValueRef raw =
      lhs_nonnegative
          ? LLVMBuildURem(cg->builder, li, ri,
                          NY_LLVM_NAME(cg, "proven_int_umod"))
          : LLVMBuildSRem(cg->builder, li, ri,
                          NY_LLVM_NAME(cg, "proven_int_mod"));
  return ny_tag_int(cg, raw);
}

static LLVMValueRef ny_get_overflow_intrinsic_i64(codegen_t *cg, const char *name,
                                                  LLVMTypeRef *out_ft) {
  LLVMTypeRef ret_parts[2] = {cg->type_i64, cg->type_i1};
  LLVMTypeRef ret_ty = LLVMStructTypeInContext(cg->ctx, ret_parts, 2, false);
  LLVMTypeRef args[2] = {cg->type_i64, cg->type_i64};
  LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, args, 2, false);
  LLVMValueRef fn = ny_get_named_fn(cg, name);
  if (!fn)
    fn = LLVMAddFunction(cg->module, name, fn_ty);
  if (out_ft)
    *out_ft = fn_ty;
  return fn;
}

static bool ny_should_prefer_builtin_ops(const codegen_t *cg) {
  if (!cg || !cg->current_module_name || !*cg->current_module_name)
    return false;
  const char *mod = cg->current_module_name;
  bool is_std_mod = (strncmp(mod, "std.", 4) == 0 || strncmp(mod, "lib.", 4) == 0);
  if (!is_std_mod)
    return false;
  if (strncmp(mod, "std.core.reflect", 16) == 0)
    return false;
  return ny_env_enabled_default_on("NYTRIX_STD_BUILTIN_OPS");
}

static fun_sig *ny_helper_eq(codegen_t *cg) {
  fun_sig *s = lookup_fun(cg, "std.core.reflect.eq", 0);
  if (!s)
    s = lookup_fun(cg, "eq", 0);
  if (!s)
    s = lookup_fun(cg, "__eq", 0);
  if (s)
    cg->cached_fn_eq = s;
  return s;
}

static fun_sig *ny_helper_contains(codegen_t *cg) {
  fun_sig *s = lookup_fun(cg, "contains", 0);
  if (s)
    cg->cached_fn_contains = s;
  return s;
}

static LLVMValueRef ny_try_emit_list_tuple_builtin_guard(codegen_t *cg, LLVMValueRef fn,
                                                         LLVMValueRef l, LLVMValueRef r,
                                                         fun_sig *builtin, fun_sig *fallback,
                                                         const char *name,
                                                         LLVMBasicBlockRef *out_done_bb) {
  if (out_done_bb)
    *out_done_bb = NULL;
  if (!cg || !fn || !l || !r || !builtin || !fallback || builtin == fallback)
    return NULL;

  fun_sig *tagof = lookup_fun(cg, "__tagof", 0);
  if (!tagof)
    return NULL;

  LLVMValueRef lt = LLVMBuildCall2(cg->builder, tagof->type, tagof->value,
                                   (LLVMValueRef[]){l}, 1, "lt_tagof");
  LLVMValueRef rt = LLVMBuildCall2(cg->builder, tagof->type, tagof->value,
                                   (LLVMValueRef[]){r}, 1, "rt_tagof");
  LLVMValueRef list_tag = ny_const_tagged_int_value(cg, 100);
  LLVMValueRef tuple_tag = ny_const_tagged_int_value(cg, 103);
  LLVMValueRef str_tag = ny_const_tagged_int_value(cg, 120);
  LLVMValueRef str_const_tag = ny_const_tagged_int_value(cg, 121);
  LLVMValueRef l_list = ny_eq(cg, lt, list_tag, "l_is_list");
  LLVMValueRef l_tuple = ny_eq(cg, lt, tuple_tag, "l_is_tuple");
  LLVMValueRef l_str = ny_eq(cg, lt, str_tag, "l_is_str");
  LLVMValueRef l_str_const = ny_eq(cg, lt, str_const_tag, "l_is_str_const");
  LLVMValueRef r_list = ny_eq(cg, rt, list_tag, "r_is_list");
  LLVMValueRef r_tuple = ny_eq(cg, rt, tuple_tag, "r_is_tuple");
  LLVMValueRef r_str = ny_eq(cg, rt, str_tag, "r_is_str");
  LLVMValueRef r_str_const = ny_eq(cg, rt, str_const_tag, "r_is_str_const");
  LLVMValueRef l_seq = ny_or(cg, l_list, l_tuple, "l_is_seq");
  LLVMValueRef r_seq = ny_or(cg, r_list, r_tuple, "r_is_seq");
  LLVMValueRef l_string = ny_or(cg, l_str, l_str_const, "l_is_string");
  LLVMValueRef r_string = ny_or(cg, r_str, r_str_const, "r_is_string");
  LLVMValueRef both_seq = ny_and(cg, l_seq, r_seq, "both_list_tuple");
  LLVMValueRef both_string = ny_and(cg, l_string, r_string, "both_string");
  LLVMValueRef use_builtin = ny_or(cg, both_seq, both_string, "builtin_pair");

  char fast_name[64];
  char slow_name[64];
  char join_name[64];
  snprintf(fast_name, sizeof(fast_name), "%s.list_tuple", name ? name : "bin");
  snprintf(slow_name, sizeof(slow_name), "%s.generic", name ? name : "bin");
  snprintf(join_name, sizeof(join_name), "%s.join", name ? name : "bin");

  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, fast_name);
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, slow_name);
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, join_name);
  ny_cond_br(cg, use_builtin, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef fast_value = LLVMBuildCall2(cg->builder, builtin->type, builtin->value,
                                           (LLVMValueRef[]){l, r}, 2, "bin.list_tuple");
  LLVMBasicBlockRef fast_done = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, fallback->type, fallback->value,
                                           (LLVMValueRef[]){l, r}, 2, "bin.generic");
  LLVMBasicBlockRef slow_done = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "bin_guarded"));
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_value, slow_value},
                  (LLVMBasicBlockRef[]){fast_done, slow_done}, 2);
  if (out_done_bb)
    *out_done_bb = join_bb;
  return phi;
}

typedef enum {
  NY_BINOP_ADD,
  NY_BINOP_SUB,
  NY_BINOP_MUL,
  NY_BINOP_DIV,
  NY_BINOP_MOD,
  NY_BINOP_POW,
  NY_BINOP_AND,
  NY_BINOP_OR,
  NY_BINOP_XOR,
  NY_BINOP_SHL,
  NY_BINOP_SHR,
  NY_BINOP_EQ,
  NY_BINOP_NE,
  NY_BINOP_LT,
  NY_BINOP_LE,
  NY_BINOP_GT,
  NY_BINOP_GE,
  NY_BINOP_IN,
  NY_BINOP_UNKNOWN
} ny_binop_kind_t;

typedef struct {
  const char *op;
  const char *generic;
  const char *builtin;
  ny_binop_kind_t kind;
  bool fast_int_supported;
  const char *overflow_intr;
} op_map_t;

static const op_map_t op_map[] = {
    {"+", "add", "__add", NY_BINOP_ADD, true, "llvm.sadd.with.overflow.i64"},
    {"-", "sub", "__sub", NY_BINOP_SUB, true, "llvm.ssub.with.overflow.i64"},
    {"*", "mul", "__mul", NY_BINOP_MUL, true, "llvm.smul.with.overflow.i64"},
    {"/", "div", "__div", NY_BINOP_DIV, true, NULL},
    {"%", "mod", "__mod", NY_BINOP_MOD, true, NULL},
    {"^", "pow", NULL, NY_BINOP_POW, false, NULL},
    {"|", "bor", "__or", NY_BINOP_OR, true, NULL},
    {"&", "band", "__and", NY_BINOP_AND, true, NULL},
    {"^^", "bxor", "__xor", NY_BINOP_XOR, true, NULL},
    {"<", "lt", "__lt", NY_BINOP_LT, true, NULL},
    {"<=", "le", "__le", NY_BINOP_LE, true, NULL},
    {">", "gt", "__gt", NY_BINOP_GT, true, NULL},
    {">=", "ge", "__ge", NY_BINOP_GE, true, NULL},
    {"<<", "bshl", "__shl", NY_BINOP_SHL, true, NULL},
    {">>", "bshr", "__shr", NY_BINOP_SHR, true, NULL},
    {"==", NULL, NULL, NY_BINOP_EQ, true, NULL},
    {"!=", NULL, NULL, NY_BINOP_NE, false, NULL},
    {"in", NULL, NULL, NY_BINOP_IN, false, NULL},
    {NULL, NULL, NULL, NY_BINOP_UNKNOWN, false, NULL}};

static bool ny_const_int_pow_exact(int64_t base, int64_t exp, int64_t *out) {
  if (!out || exp < 0)
    return false;
  int64_t result = 1;
  int64_t b = base;
  uint64_t e = (uint64_t)exp;
  while (e) {
    if (e & 1) {
      if (__builtin_mul_overflow(result, b, &result) || !ny_small_int_fits_i64(result))
        return false;
    }
    e >>= 1;
    if (e) {
      if (__builtin_mul_overflow(b, b, &b) || !ny_small_int_fits_i64(b))
        return false;
    }
  }
  *out = result;
  return true;
}

static __attribute__((unused)) LLVMValueRef ny_emit_raw_int_binary(codegen_t *cg,
                                                                   const op_map_t *entry,
                                                                   LLVMValueRef l, LLVMValueRef r) {
  /* Complete tag elimination - pure i64 operations, no tagging */
  if (!entry)
    return NULL;

  ny_binop_kind_t kind = entry->kind;
  LLVMValueRef result = NULL;

  switch (kind) {
  case NY_BINOP_ADD:
    result = ny_add(cg, l, r, "raw_add");
    break;
  case NY_BINOP_SUB:
    result = ny_sub(cg, l, r, "raw_sub");
    break;
  case NY_BINOP_MUL:
    result = ny_mul(cg, l, r, "raw_mul");
    break;
  case NY_BINOP_DIV:
    result = LLVMBuildSDiv(cg->builder, l, r, "raw_div");
    break;
  case NY_BINOP_MOD:
    result = LLVMBuildSRem(cg->builder, l, r, "raw_mod");
    break;
  case NY_BINOP_AND:
    result = ny_and(cg, l, r, "raw_and");
    break;
  case NY_BINOP_OR:
    result = ny_or(cg, l, r, "raw_or");
    break;
  case NY_BINOP_XOR:
    result = ny_xor(cg, l, r, "raw_xor");
    break;
  case NY_BINOP_SHL:
    result = ny_shl(cg, l, r, "raw_shl");
    break;
  case NY_BINOP_SHR:
    result = ny_ashr(cg, l, r, "raw_shr");
    break;
  case NY_BINOP_LT:
    result = ny_slt(cg, l, r, "raw_lt");
    break;
  case NY_BINOP_LE:
    result = ny_sle(cg, l, r, "raw_le");
    break;
  case NY_BINOP_GT:
    result = ny_sgt(cg, l, r, "raw_gt");
    break;
  case NY_BINOP_GE:
    result = ny_sge(cg, l, r, "raw_ge");
    break;
  case NY_BINOP_EQ:
    result = ny_eq(cg, l, r, "raw_eq");
    break;
  case NY_BINOP_NE:
    result = ny_ne(cg, l, r, "raw_ne");
    break;
  default:
    return NULL;
  }

  return result;
}

static LLVMValueRef ny_emit_tagged_int_fast_no_slow(codegen_t *cg, const op_map_t *entry,
                                                    LLVMValueRef l, LLVMValueRef r) {
  if (!entry)
    return NULL;
  ny_binop_kind_t kind = entry->kind;
  if (kind == NY_BINOP_SHL || kind == NY_BINOP_SHR)
    return NULL;

  if (kind == NY_BINOP_ADD) {
    LLVMValueRef sum = ny_add(cg, l, r, "");
    return ny_sub(cg, sum, ny_c1(cg), "tag_add");
  }
  if (kind == NY_BINOP_SUB) {
    LLVMValueRef diff = ny_sub(cg, l, r, "");
    return ny_add(cg, diff, ny_c1(cg), "tag_sub");
  }
  if (kind == NY_BINOP_MUL) {
    LLVMValueRef li = ny_untag_int(cg, l);
    LLVMValueRef ri = ny_untag_int(cg, r);
    LLVMValueRef raw = LLVMBuildNSWMul(cg->builder, li, ri, "mul_nsw");
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_BINOP_DIV || kind == NY_BINOP_MOD) {
    int64_t rv = 0;
    if (ny_const_tagged_int(r, &rv) && rv != 0) {
      // Power-of-2 constant: use shift/mask
      LLVMValueRef li = ny_untag_int(cg, l);
      if (rv > 0 && (rv & (rv - 1)) == 0 && kind == NY_BINOP_DIV) {
        int shift = __builtin_ctzll((uint64_t)rv);
        LLVMValueRef raw = ny_ashr(cg, li, LLVMConstInt(cg->type_i64, shift, false), "div_shr");
        return ny_tag_int(cg, raw);
      }
      if (rv > 0 && (rv & (rv - 1)) == 0 && kind == NY_BINOP_MOD) {
        LLVMValueRef mask = LLVMConstInt(cg->type_i64, rv - 1, false);
        LLVMValueRef raw = ny_and(cg, li, mask, "mod_and");
        return ny_tag_int(cg, raw);
      }
      LLVMValueRef ri = LLVMConstInt(cg->type_i64, (uint64_t)rv, true);
      LLVMValueRef raw = (kind == NY_BINOP_DIV)
                             ? LLVMBuildSDiv(cg->builder, li, ri, "div_const")
                             : LLVMBuildSRem(cg->builder, li, ri, "mod_const");
      return ny_tag_int(cg, raw);
    }
    return NULL;
  }
  if (kind == NY_BINOP_AND || kind == NY_BINOP_OR || kind == NY_BINOP_XOR) {
    LLVMValueRef li = ny_untag_int(cg, l);
    LLVMValueRef ri = ny_untag_int(cg, r);
    LLVMValueRef raw = NULL;
    if (kind == NY_BINOP_AND)
      raw = ny_and(cg, li, ri, NY_LLVM_NAME(cg, "and_fast"));
    else if (kind == NY_BINOP_OR)
      raw = ny_or(cg, li, ri, NY_LLVM_NAME(cg, "or_fast"));
    else
      raw = ny_xor(cg, li, ri, NY_LLVM_NAME(cg, "xor_fast"));
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_BINOP_LT || kind == NY_BINOP_LE || kind == NY_BINOP_GT || kind == NY_BINOP_GE ||
      kind == NY_BINOP_EQ || kind == NY_BINOP_NE) {
    LLVMIntPredicate pred = LLVMIntEQ;
    if (kind == NY_BINOP_LT)
      pred = LLVMIntSLT;
    else if (kind == NY_BINOP_LE)
      pred = LLVMIntSLE;
    else if (kind == NY_BINOP_GT)
      pred = LLVMIntSGT;
    else if (kind == NY_BINOP_GE)
      pred = LLVMIntSGE;
    else if (kind == NY_BINOP_NE)
      pred = LLVMIntNE;
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, l, r, NY_LLVM_NAME(cg, "icmp_fast"));
    return ny_tag_bool(cg, cmp);
  }
  return NULL;
}

static void ny_note_raw_int_expr_fast(codegen_t *cg, ny_binop_kind_t kind) {
  if (!cg || !cg->module)
    return;
  const char *name = "raw_int_expr_fast";
  if (kind == NY_BINOP_ADD)
    name = "raw_int_expr_fast_add";
  else if (kind == NY_BINOP_SUB)
    name = "raw_int_expr_fast_sub";
  else if (kind == NY_BINOP_MUL)
    name = "raw_int_expr_fast_mul";
  else if (kind == NY_BINOP_DIV)
    name = "raw_int_expr_fast_div";
  else if (kind == NY_BINOP_MOD)
    name = "raw_int_expr_fast_mod";
  LLVMMetadataRef s = LLVMMDStringInContext2(cg->ctx, name, strlen(name));
  LLVMMetadataRef md = LLVMMDNodeInContext2(cg->ctx, &s, 1);
  LLVMAddNamedMetadataOperand(cg->module, "nytrix.raw_int_expr_fast",
                              LLVMMetadataAsValue(cg->ctx, md));
}

static bool ny_bin_expr_is_typed_fixnum(codegen_t *cg, scope *scopes, size_t depth,
                                        expr_t *e);

static bool ny_raw_int_expr_mul_default_enabled(codegen_t *cg) {
  if (cg && cg->env_cache.raw_int_expr_mul_fast != 0)
    return cg->env_cache.raw_int_expr_mul_fast == 1;
  bool enabled = ny_env_enabled_default_on("NYTRIX_RAW_INT_EXPR_MUL_FAST");
  if (cg)
    cg->env_cache.raw_int_expr_mul_fast = enabled ? 1 : -1;
  return enabled;
}

static bool ny_raw_int_expr_addsub_default_enabled(codegen_t *cg) {
  if (cg && cg->env_cache.raw_int_expr_addsub_fast != 0)
    return cg->env_cache.raw_int_expr_addsub_fast == 1;
  bool enabled = ny_env_enabled_default_on("NYTRIX_RAW_INT_EXPR_ADDSUB_FAST");
  if (cg)
    cg->env_cache.raw_int_expr_addsub_fast = enabled ? 1 : -1;
  return enabled;
}

static bool ny_raw_int_expr_fast_op_enabled(ny_binop_kind_t kind) {
  const char *ops = getenv("NYTRIX_RAW_INT_EXPR_FAST_OPS");
  if (!ops || !*ops)
    return true;
  const char *needle = NULL;
  if (kind == NY_BINOP_ADD)
    needle = "add";
  else if (kind == NY_BINOP_SUB)
    needle = "sub";
  else if (kind == NY_BINOP_MUL)
    needle = "mul";
  else if (kind == NY_BINOP_DIV)
    needle = "div";
  else if (kind == NY_BINOP_MOD)
    needle = "mod";
  if (!needle)
    return false;
  const size_t nlen = strlen(needle);
  const char *p = ops;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t')
      p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ' ' && *p != '\t')
      p++;
    if ((size_t)(p - start) == nlen && strncmp(start, needle, nlen) == 0)
      return true;
  }
  return false;
}

static LLVMValueRef ny_try_emit_raw_int_expr_fast_binary(codegen_t *cg, scope *scopes,
                                                         size_t depth, const op_map_t *entry,
                                                         LLVMValueRef l, LLVMValueRef r,
                                                         expr_t *le, expr_t *re,
                                                         ny_int_range_t range_l,
                                                         ny_int_range_t range_r) {
  if (!entry || !l || !r || !le || !re)
    return NULL;
  (void)scopes;
  (void)depth;
  ny_binop_kind_t kind = entry->kind;
  bool full_raw_expr = ny_fast_path_enabled(cg, "NYTRIX_RAW_INT_EXPR_FAST");
  bool default_addsub_expr =
      (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB) &&
      ny_raw_int_expr_addsub_default_enabled(cg);
  bool default_mul_expr = kind == NY_BINOP_MUL && ny_raw_int_expr_mul_default_enabled(cg);
  if (!full_raw_expr && !default_addsub_expr && !default_mul_expr)
    return NULL;
  if (full_raw_expr && !ny_raw_int_expr_fast_op_enabled(kind) && !default_addsub_expr &&
      !default_mul_expr)
    return NULL;
  bool ok = false;
  if (kind == NY_BINOP_ADD) {
    int64_t lo = 0, hi = 0;
    ok = range_l.known && range_r.known &&
         ny_add_range_ok(range_l.min_raw, range_r.min_raw, &lo) &&
         ny_add_range_ok(range_l.max_raw, range_r.max_raw, &hi) &&
         ny_small_int_fits_i64(lo) && ny_small_int_fits_i64(hi);
  } else if (kind == NY_BINOP_SUB) {
    int64_t lo = 0, hi = 0;
    ok = range_l.known && range_r.known &&
         ny_sub_range_ok(range_l.min_raw, range_r.max_raw, &lo) &&
         ny_sub_range_ok(range_l.max_raw, range_r.min_raw, &hi) &&
         ny_small_int_fits_i64(lo) && ny_small_int_fits_i64(hi);
  } else if (kind == NY_BINOP_MUL) {
    int64_t candidates[4];
    ok = range_l.known && range_r.known &&
         ny_mul_range_ok(range_l.min_raw, range_r.min_raw, &candidates[0]) &&
         ny_mul_range_ok(range_l.min_raw, range_r.max_raw, &candidates[1]) &&
         ny_mul_range_ok(range_l.max_raw, range_r.min_raw, &candidates[2]) &&
         ny_mul_range_ok(range_l.max_raw, range_r.max_raw, &candidates[3]);
    if (ok) {
      int64_t lo = candidates[0];
      int64_t hi = candidates[0];
      for (int i = 1; i < 4; ++i) {
        if (candidates[i] < lo)
          lo = candidates[i];
        if (candidates[i] > hi)
          hi = candidates[i];
      }
      ok = ny_small_int_fits_i64(lo) && ny_small_int_fits_i64(hi);
    }
  } else if (kind == NY_BINOP_DIV) {
    ok = range_r.known && range_r.min_raw > 0;
  } else if (kind == NY_BINOP_MOD) {
    ok = range_r.known && range_r.min_raw > 0;
    /* Constant divisors already have a tighter tagged-int path below, including
     * power-of-two masks.  The raw-expression path is only useful for proven
     * positive variable divisors. */
    int64_t const_rhs = 0;
    if (ok && ny_const_tagged_int(r, &const_rhs))
      return NULL;
  }
  if (!ok)
    return NULL;

  if (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB) {
    LLVMValueRef tagged = ny_emit_tagged_int_fast_no_slow(cg, entry, l, r);
    if (tagged) {
      ny_note_raw_int_expr_fast(cg, kind);
      return tagged;
    }
  }

  /* Use the already-emitted operands for this binary node.  Re-walking the AST here can
   * regenerate mutable direct locals outside their loop-carried SSA value. */
  LLVMValueRef lhs = ny_untag_int(cg, l);
  LLVMValueRef rhs = ny_untag_int(cg, r);

  LLVMValueRef raw = NULL;
  if (kind == NY_BINOP_ADD)
    raw = ny_add(cg, lhs, rhs, NY_LLVM_NAME(cg, "raw_int_expr_fast_add"));
  else if (kind == NY_BINOP_SUB)
    raw = ny_sub(cg, lhs, rhs, NY_LLVM_NAME(cg, "raw_int_expr_fast_sub"));
  else if (kind == NY_BINOP_MUL)
    raw = ny_mul(cg, lhs, rhs, NY_LLVM_NAME(cg, "raw_int_expr_fast_mul"));
  else if (kind == NY_BINOP_DIV)
    raw = LLVMBuildSDiv(cg->builder, lhs, rhs, NY_LLVM_NAME(cg, "raw_int_expr_fast_div"));
  else if (kind == NY_BINOP_MOD)
    raw = LLVMBuildSRem(cg->builder, lhs, rhs, NY_LLVM_NAME(cg, "raw_int_expr_fast_mod"));
  if (raw)
    ny_note_raw_int_expr_fast(cg, kind);
  return raw ? ny_tag_int(cg, raw) : NULL;
}

static LLVMValueRef ny_try_emit_tagged_int_fast_binary(codegen_t *cg, scope *scopes, size_t depth,
                                                       const op_map_t *entry, LLVMValueRef l,
                                                       LLVMValueRef r, expr_t *le, expr_t *re,
                                                       fun_sig *fallback) {

  if (!fallback || !entry)
    return NULL;
  /* Fast int binops now enabled by default - provides major speedup */
  if (!ny_env_enabled_default_on("NYTRIX_FAST_INT_BINOPS") &&
      !ny_env_enabled("NYTRIX_ENABLE_TYPEINFER") && !ny_env_enabled("NYTRIX_ENABLE_OPTIMIZE"))
    return NULL;

  if (!entry->fast_int_supported)
    return NULL;

  ny_binop_kind_t kind = entry->kind;

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  bool proven_l = ny_is_proven_int(cg, scopes, depth, le, l);
  bool proven_r = ny_is_proven_int(cg, scopes, depth, re, r);
  if (ny_expr_backed_by_unproven_get(cg, scopes, depth, le) ||
      ny_expr_backed_by_unproven_get(cg, scopes, depth, re))
    return NULL;
  if (cg->mono_emitting && cg->mono_raw_expr_disabled) {
    proven_l = false;
    proven_r = false;
  }
  bool proven_both = proven_l && proven_r;
  bool dynamic_tagged_int_ok =
      !proven_both && !ny_expr_is_ptr_like_for_arith(cg, scopes, depth, le, 3) &&
      !ny_expr_is_ptr_like_for_arith(cg, scopes, depth, re, 3);
  ny_int_range_t range_l = ny_expr_proven_small_int_range(cg, scopes, depth, le);
  ny_int_range_t range_r = ny_expr_proven_small_int_range(cg, scopes, depth, re);
  if (!proven_l && range_l.known)
    proven_l = true;
  if (!proven_r && range_r.known)
    proven_r = true;
  if (cg->opt_unsafe_arith) {
    if (!proven_l && !ny_expr_is_ptr_like_for_arith(cg, scopes, depth, le, 3) &&
        ny_bin_expr_is_typed_fixnum(cg, scopes, depth, le))
      proven_l = true;
    if (!proven_r && !ny_expr_is_ptr_like_for_arith(cg, scopes, depth, re, 3) &&
        ny_bin_expr_is_typed_fixnum(cg, scopes, depth, re))
      proven_r = true;
  }
  proven_both = proven_l && proven_r;
  dynamic_tagged_int_ok =
      !proven_both && !ny_expr_is_ptr_like_for_arith(cg, scopes, depth, le, 3) &&
      !ny_expr_is_ptr_like_for_arith(cg, scopes, depth, re, 3);

  if (kind == NY_BINOP_MOD) {
    LLVMValueRef proven_fast =
        ny_try_emit_proven_int_mod_fast(cg, scopes, depth, l, r, le, re, range_r);
    if (proven_fast)
      return proven_fast;
  }

  if (proven_l && proven_r) {
    LLVMValueRef raw_expr_fast = ny_try_emit_raw_int_expr_fast_binary(
        cg, scopes, depth, entry, l, r, le, re, range_l, range_r);
    if (raw_expr_fast)
      return raw_expr_fast;
  }

  if (kind == NY_BINOP_MOD && (!proven_l || !proven_r)) {
    LLVMValueRef modexpr_fast =
        ny_try_emit_proven_int_modexpr_fast(cg, scopes, depth, fn, l, r, le, re, fallback);
    if (modexpr_fast)
      return modexpr_fast;
  }

  bool takes_overflow_path = (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL);

  if (cg->opt_unsafe_arith && proven_l && proven_r && takes_overflow_path) {
    LLVMValueRef fast = ny_emit_tagged_int_fast_no_slow(cg, entry, l, r);
    if (fast)
      return fast;
  }

  if (proven_l && proven_r && takes_overflow_path && range_l.known && range_r.known) {
    bool in_small_range = false;
    if (kind == NY_BINOP_ADD) {
      int64_t lo = 0, hi = 0;
      in_small_range = ny_add_range_ok(range_l.min_raw, range_r.min_raw, &lo) &&
                       ny_add_range_ok(range_l.max_raw, range_r.max_raw, &hi) &&
                       ny_small_int_fits_i64(lo) && ny_small_int_fits_i64(hi);
    } else if (kind == NY_BINOP_SUB) {
      int64_t lo = 0, hi = 0;
      in_small_range = ny_sub_range_ok(range_l.min_raw, range_r.max_raw, &lo) &&
                       ny_sub_range_ok(range_l.max_raw, range_r.min_raw, &hi) &&
                       ny_small_int_fits_i64(lo) && ny_small_int_fits_i64(hi);
    } else if (kind == NY_BINOP_MUL) {
      int64_t candidates[4];
      in_small_range =
          ny_mul_range_ok(range_l.min_raw, range_r.min_raw, &candidates[0]) &&
          ny_mul_range_ok(range_l.min_raw, range_r.max_raw, &candidates[1]) &&
          ny_mul_range_ok(range_l.max_raw, range_r.min_raw, &candidates[2]) &&
          ny_mul_range_ok(range_l.max_raw, range_r.max_raw, &candidates[3]);
      if (in_small_range) {
        int64_t lo = candidates[0];
        int64_t hi = candidates[0];
        for (int i = 1; i < 4; ++i) {
          if (candidates[i] < lo)
            lo = candidates[i];
          if (candidates[i] > hi)
            hi = candidates[i];
        }
        in_small_range = ny_small_int_fits_i64(lo) && ny_small_int_fits_i64(hi);
      }
    }
    if (in_small_range) {
      LLVMValueRef fast = ny_emit_tagged_int_fast_no_slow(cg, entry, l, r);
      if (fast)
        return fast;
    }
  }

  if (proven_l && proven_r && !takes_overflow_path) {
    LLVMValueRef fast = ny_emit_tagged_int_fast_no_slow(cg, entry, l, r);
    if (fast)
      return fast;
  }

  /* For dynamic values, keep the normal generic helper as the slow path and
   * take the inline path only when runtime tags prove both operands are small
   * ints. Pointer-like expressions are excluded because raw handles can have
   * arbitrary low bits after address arithmetic. */
  if (!proven_both && !dynamic_tagged_int_ok)
    return NULL;

  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "bin.int.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "bin.runtime.slow");
  LLVMBasicBlockRef merge_bb = ny_bb_fn(fn, "bin.merge");

  LLVMValueRef both_int;
  if (proven_l && proven_r)
    both_int = LLVMConstInt(cg->type_i1, 1, false);
  else if (proven_l)
    both_int = ny_is_tagged_int(cg, r);
  else if (proven_r)
    both_int = ny_is_tagged_int(cg, l);
  else
    both_int = ny_and(cg, ny_is_tagged_int(cg, l), ny_is_tagged_int(cg, r), "bin.both_int");

  if (LLVMIsAConstantInt(both_int) && LLVMConstIntGetZExtValue(both_int)) {
    ny_br(cg, fast_bb);
  } else {
    ny_cond_br(cg, both_int, fast_bb, slow_bb);
  }
  ny_pos(cg, fast_bb);

  LLVMValueRef fast_value = NULL;
  LLVMBasicBlockRef fast_done_bb = NULL;
  if (kind == NY_BINOP_SUB && l == r) {
    fast_value = ny_const_tagged_int_value(cg, 0);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if (kind == NY_BINOP_XOR && l == r) {
    fast_value = ny_const_tagged_int_value(cg, 0);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if ((kind == NY_BINOP_AND || kind == NY_BINOP_OR) && l == r) {
    fast_value = l;
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if ((kind == NY_BINOP_EQ || kind == NY_BINOP_LE || kind == NY_BINOP_GE) && l == r) {
    fast_value = ny_const_tagged_bool_value(cg, true);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if ((kind == NY_BINOP_LT || kind == NY_BINOP_GT) && l == r) {
    fast_value = ny_const_tagged_bool_value(cg, false);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL) {
    const char *intr_name = entry->overflow_intr;
    LLVMTypeRef intr_ty = NULL;
    LLVMValueRef intr = ny_get_overflow_intrinsic_i64(cg, intr_name, &intr_ty);
    LLVMValueRef li = ny_untag_int(cg, l);
    LLVMValueRef ri = ny_untag_int(cg, r);
    LLVMValueRef packed =
        LLVMBuildCall2(cg->builder, intr_ty, intr, (LLVMValueRef[]){li, ri}, 2, "arith_packed");
    LLVMValueRef raw = LLVMBuildExtractValue(cg->builder, packed, 0, NY_LLVM_NAME(cg, "arith"));
    LLVMValueRef ov = LLVMBuildExtractValue(cg->builder, packed, 1, NY_LLVM_NAME(cg, "arith_ov"));
    LLVMValueRef in_range = ny_small_int_range_ok(cg, raw);
    LLVMValueRef fast_ok =
        ny_and(cg, LLVMBuildNot(cg->builder, ov, "arith_no_ov"), in_range, "arith_ok");
    LLVMBasicBlockRef fast_ok_bb = ny_bb_fn(fn, "bin.int.fast.ok");
    ny_cond_br(cg, fast_ok, fast_ok_bb, slow_bb);

    ny_pos(cg, fast_ok_bb);

    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if (kind == NY_BINOP_DIV || kind == NY_BINOP_MOD) {
    int64_t rv = 0;
    if (ny_const_tagged_int(r, &rv) && rv > 0 && (rv & (rv - 1)) == 0) {
      LLVMValueRef li = ny_untag_int(cg, l);
      if (kind == NY_BINOP_DIV) {
        int shift = __builtin_ctzll((uint64_t)rv);
        fast_value =
            ny_tag_int(cg, ny_ashr(cg, li, LLVMConstInt(cg->type_i64, shift, false), "div_shr"));
      } else {
        fast_value =
            ny_tag_int(cg, ny_and(cg, li, LLVMConstInt(cg->type_i64, rv - 1, false), "mod_and"));
      }
      fast_done_bb = ny_cur_block(cg);
      ny_br(cg, merge_bb);
    } else {
      LLVMValueRef li = ny_untag_int(cg, l);
      LLVMValueRef ri = ny_untag_int(cg, r);
      LLVMValueRef is_zero = ny_eq(cg, ri, ny_c0(cg), "divmod_zero");
      LLVMBasicBlockRef div_ok_bb = ny_bb_fn(fn, "bin.int.fast.div.ok");
      ny_cond_br(cg, is_zero, slow_bb, div_ok_bb);

      ny_pos(cg, div_ok_bb);
      LLVMValueRef raw =
          (kind == NY_BINOP_DIV)
              ? LLVMBuildSDiv(cg->builder, li, ri, NY_LLVM_NAME(cg, "sdiv_fast"))
              : LLVMBuildSRem(cg->builder, li, ri, NY_LLVM_NAME(cg, "srem_fast"));
      fast_value = ny_tag_int(cg, raw);
      fast_done_bb = ny_cur_block(cg);

      ny_br(cg, merge_bb);
    }
  } else if (kind == NY_BINOP_AND || kind == NY_BINOP_OR || kind == NY_BINOP_XOR) {
    LLVMValueRef li = ny_untag_int(cg, l);
    LLVMValueRef ri = ny_untag_int(cg, r);
    LLVMValueRef raw = NULL;
    if (kind == NY_BINOP_AND)
      raw = ny_and(cg, li, ri, NY_LLVM_NAME(cg, "and_fast"));
    else if (kind == NY_BINOP_OR)
      raw = ny_or(cg, li, ri, NY_LLVM_NAME(cg, "or_fast"));
    else
      raw = ny_xor(cg, li, ri, NY_LLVM_NAME(cg, "xor_fast"));
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if (kind == NY_BINOP_SHL || kind == NY_BINOP_SHR) {
    LLVMValueRef li = ny_untag_int(cg, l);
    LLVMValueRef ri = ny_untag_int(cg, r);
    LLVMValueRef zero = ny_c0(cg);
    LLVMValueRef sixty_four = LLVMConstInt(cg->type_i64, 64, false);
    LLVMValueRef ge_zero = ny_sge(cg, ri, zero, NY_LLVM_NAME(cg, "sh_nonneg"));
    LLVMValueRef lt_sixty_four = ny_slt(cg, ri, sixty_four, NY_LLVM_NAME(cg, "sh_lt64"));
    LLVMValueRef in_range = ny_and(cg, ge_zero, lt_sixty_four, NY_LLVM_NAME(cg, "sh_range"));
    LLVMBasicBlockRef fast_shift_bb = ny_bb_fn(fn, "bin.int.fast.shift");
    ny_cond_br(cg, in_range, fast_shift_bb, slow_bb);

    ny_pos(cg, fast_shift_bb);

    LLVMValueRef raw = (kind == NY_BINOP_SHL)
                           ? ny_shl(cg, li, ri, NY_LLVM_NAME(cg, "shl_fast"))
                           : LLVMBuildLShr(cg->builder, li, ri, NY_LLVM_NAME(cg, "shr_fast"));
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else if (kind == NY_BINOP_LT || kind == NY_BINOP_LE || kind == NY_BINOP_GT ||
             kind == NY_BINOP_GE || kind == NY_BINOP_EQ || kind == NY_BINOP_NE) {
    LLVMIntPredicate pred = LLVMIntEQ;
    if (kind == NY_BINOP_NE)
      pred = LLVMIntNE;
    else if (kind == NY_BINOP_LT)
      pred = LLVMIntSLT;
    else if (kind == NY_BINOP_LE)
      pred = LLVMIntSLE;
    else if (kind == NY_BINOP_GT)
      pred = LLVMIntSGT;
    else if (kind == NY_BINOP_GE)
      pred = LLVMIntSGE;
    // For tagged integers, relative order is preserved:
    // (a << 1 | 1) < (b << 1 | 1) <=> a < b
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, l, r, NY_LLVM_NAME(cg, "icmp_fast"));
    fast_value = ny_tag_bool(cg, cmp);
    fast_done_bb = ny_cur_block(cg);

    ny_br(cg, merge_bb);
  } else {
    // Fallback if kind is not handled in fast path (should not happen given
    // op_map)
    fast_value = ny_c0(cg);
    fast_done_bb = ny_cur_block(cg);
    ny_br(cg, merge_bb);
  }
  ny_pos(cg, slow_bb);

  LLVMValueRef slow_value = NULL;
  LLVMBasicBlockRef slow_done_bb = NULL;
  fun_sig *slow_fallback = fallback;
  if (proven_both && (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL) &&
      entry->builtin) {
    fun_sig *builtin_fallback = lookup_fun(cg, entry->builtin, 0);
    if (builtin_fallback)
      slow_fallback = builtin_fallback;
  }
  if (kind == NY_BINOP_ADD || kind == NY_BINOP_EQ) {
    fun_sig *builtin_fallback = NULL;
    if (kind == NY_BINOP_ADD && entry->builtin)
      builtin_fallback = lookup_fun(cg, entry->builtin, 0);
    else if (kind == NY_BINOP_EQ)
      builtin_fallback = lookup_fun(cg, "__eq", 0);
    slow_value = ny_try_emit_list_tuple_builtin_guard(cg, fn, l, r, builtin_fallback, slow_fallback,
                                                      kind == NY_BINOP_EQ ? "bin.eq" : "bin.add",
                                                      &slow_done_bb);
  }
  if (!slow_value) {
    slow_value = LLVMBuildCall2(cg->builder, slow_fallback->type, slow_fallback->value,
                                (LLVMValueRef[]){l, r}, 2, "bin.slow");
    slow_done_bb = ny_cur_block(cg);
  }

  ny_br(cg, merge_bb);

  ny_pos(cg, merge_bb);

  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "bin_result"));
  LLVMValueRef incoming_vals[2] = {fast_value, slow_value};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef ny_direct_unbox_float(codegen_t *cg, LLVMValueRef v) {
  if (ny_module_target_is_apple_arm64(cg ? cg->module : NULL))
    return ny_unbox_float(cg, v);
  LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, v, LLVMPointerType(cg->type_f64, 0), "");
  return LLVMBuildLoad2(cg->builder, cg->type_f64, ptr, "flt_load");
}

static LLVMValueRef ny_direct_box_float(codegen_t *cg, LLVMValueRef fval) {
  fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
  if (!box_sig)
    return ny_c0(cg);
  return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                        (LLVMValueRef[]){ny_bitcast(cg, fval, cg->type_i64, "")}, 1, "box");
}

static bool ny_bin_type_is_ptr_like(const char *type_name) {
  const char *tail = ny_type_leaf(type_name);
  return (tail && (strcmp(tail, "ptr") == 0 || strcmp(tail, "handle") == 0)) ||
         (type_name && type_name[0] == '*');
}

static bool ny_bin_type_alias_eq(const char *a, const char *b) {
  const char *at = ny_type_leaf(a);
  const char *bt = ny_type_leaf(b);
  if (!at || !bt)
    return false;
  if (strcmp(at, bt) == 0)
    return true;
  if ((strcmp(at, "Vector2") == 0 && strcmp(bt, "vec2") == 0) ||
      (strcmp(at, "vec2") == 0 && strcmp(bt, "Vector2") == 0))
    return true;
  if ((strcmp(at, "Vector3") == 0 && strcmp(bt, "vec3") == 0) ||
      (strcmp(at, "vec3") == 0 && strcmp(bt, "Vector3") == 0))
    return true;
  if ((strcmp(at, "Vector4") == 0 && strcmp(bt, "vec4") == 0) ||
      (strcmp(at, "vec4") == 0 && strcmp(bt, "Vector4") == 0))
    return true;
  return false;
}

static bool ny_bin_type_is_core_scalar(const char *type_name) {
  const char *t = ny_type_leaf(type_name);
  if (!t)
    return false;
  return strcmp(t, "int") == 0 || strcmp(t, "i8") == 0 || strcmp(t, "i16") == 0 ||
         strcmp(t, "i32") == 0 || strcmp(t, "i64") == 0 || strcmp(t, "i128") == 0 ||
         strcmp(t, "u8") == 0 || strcmp(t, "u16") == 0 || strcmp(t, "u32") == 0 ||
         strcmp(t, "u64") == 0 || strcmp(t, "u128") == 0 || strcmp(t, "f32") == 0 ||
         strcmp(t, "f64") == 0 || strcmp(t, "f128") == 0 || strcmp(t, "bool") == 0 ||
         strcmp(t, "str") == 0 || strcmp(t, "char") == 0 || strcmp(t, "ptr") == 0 ||
         strcmp(t, "handle") == 0 || strcmp(t, "nil") == 0 || strcmp(t, "none") == 0;
}

static bool ny_bin_type_is_str(const char *type_name) {
  const char *t = ny_type_leaf(type_name);
  return t && strcmp(t, "str") == 0;
}

static bool ny_bin_expr_is_stringish(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_STR)
    return true;
  if (ny_bin_type_is_str(infer_expr_type(cg, scopes, depth, e)))
    return true;
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b =
      ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                               e->as.ident.hash);
  return b &&
         (ny_bin_type_is_str(b->type_name) || ny_bin_type_is_str(b->decl_type_name));
}

static LLVMValueRef ny_try_emit_direct_str_concat(codegen_t *cg, scope *scopes,
                                                  size_t depth, ny_binop_kind_t kind,
                                                  LLVMValueRef l, LLVMValueRef r,
                                                  expr_t *le, expr_t *re) {
  if (kind != NY_BINOP_ADD)
    return NULL;
  if (!ny_bin_expr_is_stringish(cg, scopes, depth, le) ||
      !ny_bin_expr_is_stringish(cg, scopes, depth, re))
    return NULL;
  fun_sig *s = lookup_fun(cg, "__str_concat", 0);
  if (!s || !s->type || !s->value)
    return NULL;
  return LLVMBuildCall2(cg->builder, s->type, s->value,
                        (LLVMValueRef[]){l, r}, 2,
                        NY_LLVM_NAME(cg, "str_concat_direct"));
}

static bool ny_bin_type_is_fixnum_like(const char *type_name) {
  const char *t = ny_type_leaf(type_name);
  if (!t)
    return false;
  return strcmp(t, "int") == 0 || strcmp(t, "i8") == 0 || strcmp(t, "i16") == 0 ||
         strcmp(t, "i32") == 0 || strcmp(t, "i64") == 0 || strcmp(t, "u8") == 0 ||
         strcmp(t, "u16") == 0 || strcmp(t, "u32") == 0 || strcmp(t, "u64") == 0;
}

static bool ny_bin_expr_is_typed_fixnum(codegen_t *cg, scope *scopes, size_t depth,
                                        expr_t *e) {
  if (!e)
    return false;
  const char *t = infer_expr_type(cg, scopes, depth, e);
  if (ny_bin_type_is_fixnum_like(t))
    return true;
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b =
      ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
  return b && (ny_bin_type_is_fixnum_like(b->type_name) ||
               ny_bin_type_is_fixnum_like(b->decl_type_name));
}

static bool ny_bin_eq_type_prefers_builtin(const char *lt, const char *rt) {
  const char *l = ny_type_leaf(lt);
  const char *r = ny_type_leaf(rt);
  if (!l || !r)
    return false;
  if (ny_bin_type_is_core_scalar(l) && ny_bin_type_is_core_scalar(r))
    return true;
  bool l_str = strcmp(l, "str") == 0;
  bool r_str = strcmp(r, "str") == 0;
  if (l_str && r_str)
    return true;
  bool l_seq = strcmp(l, "list") == 0 || strcmp(l, "tuple") == 0;
  bool r_seq = strcmp(r, "list") == 0 || strcmp(r, "tuple") == 0;
  if (l_seq && r_seq)
    return true;
  bool l_big = strcmp(l, "bigint") == 0;
  bool r_big = strcmp(r, "bigint") == 0;
  return (l_big && (r_big || strcmp(r, "int") == 0)) ||
         (r_big && (l_big || strcmp(l, "int") == 0));
}

static bool ny_bin_expr_is_builtin_eq_shape(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT || e->as.literal.kind == NY_LIT_BOOL ||
           e->as.literal.kind == NY_LIT_FLOAT || e->as.literal.kind == NY_LIT_STR;
  if (e->kind == NY_E_UNARY && e->as.unary.right &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
       strcmp(e->as.unary.op, "~") == 0) &&
      e->as.unary.right->kind == NY_E_LITERAL &&
      e->as.unary.right->as.literal.kind == NY_LIT_INT)
    return true;
  if (e->kind == NY_E_LIST || e->kind == NY_E_TUPLE)
    return true;
  const char *t = infer_expr_type(cg, scopes, depth, e);
  const char *leaf = ny_type_leaf(t);
  return leaf && (strcmp(leaf, "str") == 0 || strcmp(leaf, "list") == 0 ||
                  strcmp(leaf, "tuple") == 0 || strcmp(leaf, "bigint") == 0);
}

static bool ny_bin_expr_is_static_bigintish(codegen_t *cg, scope *scopes, size_t depth,
                                            expr_t *e, unsigned recursion) {
  if (!e || recursion > 32)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL &&
           !ny_small_int_fits_i64(e->as.literal.as.i);
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
    expr_t *init = b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
    return init && init != e &&
           ny_bin_expr_is_static_bigintish(cg, scopes, depth, init, recursion + 1);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right) {
    if (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
        strcmp(e->as.unary.op, "~") == 0)
      return ny_bin_expr_is_static_bigintish(cg, scopes, depth, e->as.unary.right,
                                             recursion + 1);
    return false;
  }
  if (e->kind != NY_E_BINARY || !e->as.binary.op)
    return false;
  const char *op = e->as.binary.op;
  if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0)
    return false;
  if (ny_bin_expr_is_static_bigintish(cg, scopes, depth, e->as.binary.left, recursion + 1) ||
      ny_bin_expr_is_static_bigintish(cg, scopes, depth, e->as.binary.right, recursion + 1))
    return true;
  ny_const_num_t cv = {0};
  return ny_const_num_eval(cg, scopes, depth, e, &cv, 0) && cv.kind == NY_CONST_NUM_INT &&
         !ny_small_int_fits_i64(cv.i);
}

static int ny_bin_type_vector_dim(const char *type_name) {
  const char *t = ny_type_leaf(type_name);
  if (!t)
    return 0;
  if (strcmp(t, "vec2") == 0 || strcmp(t, "Vector2") == 0)
    return 2;
  if (strcmp(t, "vec3") == 0 || strcmp(t, "Vector3") == 0)
    return 3;
  if (strcmp(t, "vec4") == 0 || strcmp(t, "Vector4") == 0)
    return 4;
  return 0;
}

static bool ny_binop_needs_vector_operator(ny_binop_kind_t kind) {
  return kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL ||
         kind == NY_BINOP_DIV || kind == NY_BINOP_MOD;
}

static LLVMValueRef ny_reject_missing_vector_operator(codegen_t *cg, scope *scopes, size_t depth,
                                                      ny_binop_kind_t kind, const char *op,
                                                      expr_t *le, expr_t *re) {
  if (!ny_binop_needs_vector_operator(kind) || !le || !re)
    return NULL;
  const char *lt = infer_expr_type(cg, scopes, depth, le);
  const char *rt = infer_expr_type(cg, scopes, depth, re);
  if (!lt || !rt)
    return NULL;
  if (!ny_bin_type_vector_dim(lt) && !ny_bin_type_vector_dim(rt))
    return NULL;
  token_t tok = le ? le->tok : (re ? re->tok : (token_t){0});
  return expr_fail(cg, tok, "undefined operator '%s' for %s and %s", op, lt ? lt : "unknown",
                   rt ? rt : "unknown");
}

static bool ny_operator_module_active(codegen_t *cg, const ny_operator_def_t *def) {
  if (!def || !def->module_name || !*def->module_name)
    return true;
  if (cg->current_module_name && strcmp(cg->current_module_name, def->module_name) == 0)
    return true;
  return ny_is_module_active(cg, def->module_name);
}

static LLVMValueRef ny_try_emit_scoped_operator(codegen_t *cg, scope *scopes, size_t depth,
                                                const char *op, LLVMValueRef l, LLVMValueRef r,
                                                expr_t *le, expr_t *re) {
  if (!cg || !op || cg->operators.len == 0 || !le || !re)
    return NULL;
  const char *lt = infer_expr_type(cg, scopes, depth, le);
  const char *rt = infer_expr_type(cg, scopes, depth, re);
  if (!lt || !rt)
    return NULL;
  if (ny_bin_type_is_core_scalar(lt) && ny_bin_type_is_core_scalar(rt))
    return NULL;

  for (size_t i = cg->operators.len; i > 0; --i) {
    ny_operator_def_t *def = &cg->operators.data[i - 1];
    if (!def->op || strcmp(def->op, op) != 0)
      continue;
    if (!ny_operator_module_active(cg, def))
      continue;
    if (!ny_bin_type_alias_eq(def->left_type, lt) || !ny_bin_type_alias_eq(def->right_type, rt))
      continue;
    fun_sig *target = lookup_fun(cg, def->target_name, 0);
    if (!target) {
      token_t tok = def->stmt ? def->stmt->tok : (le ? le->tok : (token_t){0});
      return expr_fail(cg, tok, "operator target '%s' is not defined", def->target_name);
    }
    if (!target->is_variadic && target->arity != 2) {
      token_t tok = def->stmt ? def->stmt->tok : (le ? le->tok : (token_t){0});
      return expr_fail(cg, tok, "operator target '%s' must take exactly two arguments",
                       def->target_name);
    }
    const char *lp = ny_sig_param_type(target, 0);
    const char *rp = ny_sig_param_type(target, 1);
    LLVMValueRef args[2] = {
        (target->is_native_abi && lp) ? ny_coerce_to_abi(cg, l, lp) : l,
        (target->is_native_abi && rp) ? ny_coerce_to_abi(cg, r, rp) : r,
    };
    LLVMValueRef raw =
        LLVMBuildCall2(cg->builder, target->type, target->value, args, 2, "op.scoped");
    const char *target_ret =
        target->abi_return_type ? target->abi_return_type : target->return_type;
    return (target->is_native_abi && target_ret)
               ? ny_box_abi_result(cg, raw, target_ret)
               : raw;
  }
  return NULL;
}

static bool ny_binding_is_ptr_like_for_arith(codegen_t *cg, scope *scopes, size_t depth,
                                             binding *b, const char *name, int recursion) {
  if (!b)
    return false;
  if (ny_bin_type_is_ptr_like(b->type_name))
    return true;
  if (recursion <= 0 || !name)
    return false;
  expr_t *init = ny_binding_var_init_expr(b, name);
  return ny_expr_is_ptr_like_for_arith(cg, scopes, depth, init, recursion - 1);
}

static bool ny_expr_is_ptr_like_for_arith(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                                          int recursion) {
  if (!e)
    return false;
  const char *type_name = infer_expr_type(cg, scopes, depth, e);
  if (ny_bin_type_is_ptr_like(type_name))
    return true;
  if (recursion <= 0)
    return false;
  switch (e->kind) {
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len, e->as.ident.hash);
    if (ny_binding_is_ptr_like_for_arith(cg, scopes, depth, b, e->as.ident.name, recursion - 1))
      return true;
    return false;
  }
  case NY_E_CALL: {
    if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT)
      return false;
    const char *name = e->as.call.callee->as.ident.name;
    if (ny_name_tail_is(name, "malloc") || ny_name_tail_is(name, "zalloc") ||
        ny_name_tail_is(name, "realloc") || ny_name_tail_is(name, "ptr_add") ||
        ny_name_tail_is(name, "ptr_sub") || ny_name_tail_is(name, "_raw_ptr") ||
        ny_name_tail_is(name, "load64_h") || strcmp(name, "__load64_h") == 0)
      return true;
    if (ny_name_tail_is(name, "to_int") && e->as.call.args.len == 1)
      return ny_expr_is_ptr_like_for_arith(cg, scopes, depth, e->as.call.args.data[0].val,
                                           recursion - 1);
    return false;
  }
  case NY_E_BINARY:
    if (!e->as.binary.op)
      return false;
    if (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0)
      return ny_expr_is_ptr_like_for_arith(cg, scopes, depth, e->as.binary.left, recursion - 1) ||
             ny_expr_is_ptr_like_for_arith(cg, scopes, depth, e->as.binary.right, recursion - 1);
    return false;
  default:
    return false;
  }
}

static LLVMValueRef ny_unbox_tagged_or_native_i64(codegen_t *cg, LLVMValueRef v, const char *name) {
  if (LLVMTypeOf(v) != cg->type_i64)
    v = ny_ptr2i64(cg, v, name ? name : "rawish_arg");
  LLVMValueRef low1 = ny_and(cg, v, ny_c1(cg), "rawish_low1");
  LLVMValueRef is_int = ny_eq(cg, low1, ny_c1(cg), "rawish_is_int");
  LLVMValueRef int_raw = ny_ashr(cg, v, ny_c1(cg), "rawish_int");
  LLVMValueRef low3 =
      ny_and(cg, v, LLVMConstInt(cg->type_i64, NY_NATIVE_TAG_MASK, false),
             "rawish_low3");
  LLVMValueRef is_native =
      ny_eq(cg, low3, LLVMConstInt(cg->type_i64, NY_NATIVE_TAG, false),
            "rawish_is_native");
  LLVMValueRef native_raw =
      LLVMBuildLShr(cg->builder, v,
                    LLVMConstInt(cg->type_i64, NY_NATIVE_SHIFT, false),
                    "rawish_native");
  LLVMValueRef not_int_raw = ny_select(cg, is_native, native_raw, v, "rawish_not_int");
  return ny_select(cg, is_int, int_raw, not_int_raw, name ? name : "rawish");
}

static LLVMValueRef ny_try_emit_ptr_arith_binary(codegen_t *cg, scope *scopes, size_t depth,
                                                 ny_binop_kind_t kind, LLVMValueRef l,
                                                 LLVMValueRef r, expr_t *le, expr_t *re) {
  if (kind != NY_BINOP_ADD && kind != NY_BINOP_SUB)
    return NULL;
  bool l_ptr = ny_expr_is_ptr_like_for_arith(cg, scopes, depth, le, 4);
  bool r_ptr = ny_expr_is_ptr_like_for_arith(cg, scopes, depth, re, 4);
  bool l_int = ny_is_proven_int(cg, scopes, depth, le, l);
  bool r_int = ny_is_proven_int(cg, scopes, depth, re, r);

  if (kind == NY_BINOP_ADD && l_ptr && r_int) {
    LLVMValueRef base = ny_unbox_tagged_or_native_i64(cg, l, "ptradd_base");
    LLVMValueRef off = ny_unbox_tagged_or_native_i64(cg, r, "ptradd_off");
    return ny_add(cg, base, off, "ptr_add_raw");
  }
  if (kind == NY_BINOP_ADD && r_ptr && l_int) {
    LLVMValueRef base = ny_unbox_tagged_or_native_i64(cg, r, "ptradd_base");
    LLVMValueRef off = ny_unbox_tagged_or_native_i64(cg, l, "ptradd_off");
    return ny_add(cg, base, off, "ptr_add_raw");
  }
  if (kind == NY_BINOP_SUB && l_ptr && r_int) {
    LLVMValueRef base = ny_unbox_tagged_or_native_i64(cg, l, "ptrsub_base");
    LLVMValueRef off = ny_unbox_tagged_or_native_i64(cg, r, "ptrsub_off");
    return ny_sub(cg, base, off, "ptr_sub_raw");
  }
  return NULL;
}

static LLVMValueRef ny_try_emit_float_fast_binary(codegen_t *cg, const op_map_t *entry,
                                                  LLVMValueRef l, LLVMValueRef r, fun_sig *fallback,
                                                  scope *scopes, size_t depth, expr_t *le,
                                                  expr_t *re) {
  if (!fallback || !entry)
    return NULL;
  if (!ny_env_enabled_default_on("NYTRIX_FAST_FLOAT_BINOPS"))
    return NULL;

  ny_binop_kind_t kind = entry->kind;
  if (kind == NY_BINOP_AND || kind == NY_BINOP_OR || kind == NY_BINOP_XOR || kind == NY_BINOP_SHL ||
      kind == NY_BINOP_SHR || kind == NY_BINOP_MOD)
    return NULL;

  // Check if both operands are proven floats — skip all branching
  const char *lt = le ? infer_expr_type(cg, scopes, depth, le) : NULL;
  const char *rt = re ? infer_expr_type(cg, scopes, depth, re) : NULL;
  bool proven_l = lt && (strcmp(lt, "f64") == 0 || strcmp(lt, "f32") == 0);
  bool proven_r = rt && (strcmp(rt, "f64") == 0 || strcmp(rt, "f32") == 0);

  /* Also check binding flags directly for cases where infer_expr_type fails */
  if (!proven_l && le && le->kind == NY_E_IDENT) {
    size_t name_len = (size_t)le->tok.len;
    if (name_len == 0)
      name_len = strlen(le->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, le->as.ident.name, name_len, le->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct))
      proven_l = true;
  }
  if (!proven_r && re && re->kind == NY_E_IDENT) {
    size_t name_len = (size_t)re->tok.len;
    if (name_len == 0)
      name_len = strlen(re->as.ident.name);
    binding *b =
        ny_binary_lookup_binding(cg, scopes, depth, re->as.ident.name, name_len, re->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct))
      proven_r = true;
  }

  if (proven_l && proven_r) {
    LLVMValueRef lf = ny_direct_unbox_float(cg, l);
    LLVMValueRef rf = ny_direct_unbox_float(cg, r);
    LLVMValueRef res_f = NULL;
    if (kind == NY_BINOP_ADD)
      res_f = LLVMBuildFAdd(cg->builder, lf, rf, "fadd");
    else if (kind == NY_BINOP_SUB)
      res_f = LLVMBuildFSub(cg->builder, lf, rf, "fsub");
    else if (kind == NY_BINOP_MUL)
      res_f = LLVMBuildFMul(cg->builder, lf, rf, "fmul");
    else if (kind == NY_BINOP_DIV)
      res_f = LLVMBuildFDiv(cg->builder, lf, rf, "fdiv");
    else {
      LLVMRealPredicate pred = LLVMRealOEQ;
      if (kind == NY_BINOP_LT)
        pred = LLVMRealOLT;
      else if (kind == NY_BINOP_LE)
        pred = LLVMRealOLE;
      else if (kind == NY_BINOP_GT)
        pred = LLVMRealOGT;
      else if (kind == NY_BINOP_GE)
        pred = LLVMRealOGE;
      LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, lf, rf, "fcmp");
      return ny_tag_bool(cg, cmp);
    }
    return ny_direct_box_float(cg, res_f);
  }

  // If neither operand is proven float, bail out to let int fast path try
  if (!proven_l && !proven_r)
    return NULL;

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);

  LLVMValueRef is_l_flt = proven_l ? LLVMConstInt(cg->type_i1, 1, false) : ny_is_float(cg, l);
  LLVMValueRef is_r_flt = proven_r ? LLVMConstInt(cg->type_i1, 1, false) : ny_is_float(cg, r);
  LLVMValueRef either_flt = ny_or(cg, is_l_flt, is_r_flt, NY_LLVM_NAME(cg, "bin.either_flt"));

  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "bin.flt.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "bin.flt.slow");
  LLVMBasicBlockRef merge_bb = ny_bb_fn(fn, "bin.flt.merge");

  ny_cond_br(cg, either_flt, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef lf = (proven_l) ? ny_direct_unbox_float(cg, l) : ny_unbox_float(cg, l);
  LLVMValueRef rf = (proven_r) ? ny_direct_unbox_float(cg, r) : ny_unbox_float(cg, r);
  LLVMValueRef res_f = NULL;

  if (kind == NY_BINOP_ADD)
    res_f = LLVMBuildFAdd(cg->builder, lf, rf, NY_LLVM_NAME(cg, "fadd"));
  else if (kind == NY_BINOP_SUB)
    res_f = LLVMBuildFSub(cg->builder, lf, rf, NY_LLVM_NAME(cg, "fsub"));
  else if (kind == NY_BINOP_MUL)
    res_f = LLVMBuildFMul(cg->builder, lf, rf, NY_LLVM_NAME(cg, "fmul"));
  else if (kind == NY_BINOP_DIV)
    res_f = LLVMBuildFDiv(cg->builder, lf, rf, NY_LLVM_NAME(cg, "fdiv"));
  else {
    LLVMRealPredicate pred = LLVMRealOEQ;
    if (kind == NY_BINOP_LT)
      pred = LLVMRealOLT;
    else if (kind == NY_BINOP_LE)
      pred = LLVMRealOLE;
    else if (kind == NY_BINOP_GT)
      pred = LLVMRealOGT;
    else if (kind == NY_BINOP_GE)
      pred = LLVMRealOGE;
    LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, lf, rf, NY_LLVM_NAME(cg, "fcmp"));
    LLVMValueRef fast_bool = ny_tag_bool(cg, cmp);
    LLVMBasicBlockRef fast_done_bb = ny_cur_block(cg);
    ny_br(cg, merge_bb);

    ny_pos(cg, slow_bb);
    LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, fallback->type, fallback->value,
                                             (LLVMValueRef[]){l, r}, 2, "bin.slow");
    LLVMBasicBlockRef slow_done_bb = ny_cur_block(cg);
    ny_br(cg, merge_bb);

    ny_pos(cg, merge_bb);
    LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "bin_res_bool"));
    LLVMAddIncoming(phi, (LLVMValueRef[]){fast_bool, slow_value},
                    (LLVMBasicBlockRef[]){fast_done_bb, slow_done_bb}, 2);
    return phi;
  }

  LLVMValueRef fast_val = ny_direct_box_float(cg, res_f);
  LLVMBasicBlockRef fast_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, fallback->type, fallback->value,
                                           (LLVMValueRef[]){l, r}, 2, "bin.slow");
  LLVMBasicBlockRef slow_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, merge_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "bin_res_num"));
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_val, slow_value},
                  (LLVMBasicBlockRef[]){fast_done_bb, slow_done_bb}, 2);
  return phi;
}

LLVMValueRef gen_binary(codegen_t *cg, scope *scopes, size_t depth, const char *op, LLVMValueRef l,
                        LLVMValueRef r, expr_t *le, expr_t *re) {

  if (!l || !r)
    return ny_c0(cg);

  if (cg->opt_sys_mode) {
    LLVMValueRef raw = gen_raw_binary(cg, op, l, r);
    if (raw) {
      LLVMTypeRef raw_type = LLVMTypeOf(raw);
      LLVMTypeKind kind = LLVMGetTypeKind(raw_type);
      if (kind == LLVMIntegerTypeKind) {
        unsigned width = LLVMGetIntTypeWidth(raw_type);
        if (width == 1) {
          return LLVMBuildZExt(cg->builder, raw, cg->type_i64, "bool_i64");
        }
        return raw;
      }
    }
  }

  bool prefer_builtin_ops = ny_should_prefer_builtin_ops(cg);

  const op_map_t *entry = NULL;
  switch (op[0]) {
  case '+':
    entry = &op_map[0];
    break; // "+"
  case '-':
    entry = &op_map[1];
    break; // "-"
  case '*':
    entry = &op_map[2];
    break; // "*"
  case '/':
    entry = &op_map[3];
    break; // "/"
  case '%':
    entry = &op_map[4];
    break; // "%"
  case '^':
    entry = (op[1] == '^') ? &op_map[8] : &op_map[5];
    break; // "^" or "^^"
  case '|':
    entry = &op_map[6];
    break; // "|"
  case '&':
    entry = &op_map[7];
    break; // "&"
  case '<':
    entry = (op[1] == '=') ? &op_map[10] : (op[1] == '<') ? &op_map[13] : &op_map[9];
    break;
  case '>':
    entry = (op[1] == '=') ? &op_map[12] : (op[1] == '>') ? &op_map[14] : &op_map[11];
    break;
  case '=':
    if (op[1] == '=')
      entry = &op_map[15];
    break; // "=="
  case '!':
    if (op[1] == '=')
      entry = &op_map[16];
    break; // "!="
  case 'i':
    if (op[1] == 'n' && !op[2])
      entry = &op_map[17];
    break; // "in"
  }

  if (!entry) {
    token_t tok = le ? le->tok : (re ? re->tok : (token_t){0});
    return expr_fail(cg, tok, "undefined operator '%s'", op);
  }

  const char *generic_name = entry->generic;
  const char *builtin_name = entry->builtin;
  ny_binop_kind_t kind = entry->kind;
  ny_warn_compile_time_zero_divisor(cg, scopes, depth, op, re);
  bool proven_l = ny_is_proven_int(cg, scopes, depth, le, l);
  bool proven_r = ny_is_proven_int(cg, scopes, depth, re, r);
  bool allow_fast_int_numeric = proven_l && proven_r;
  bool allow_dynamic_int_numeric =
      !allow_fast_int_numeric && entry->fast_int_supported &&
      ny_env_enabled_default_on("NYTRIX_DYNAMIC_INT_BINOPS");

  if (kind == NY_BINOP_IN) {
    fun_sig *s = ny_helper_contains(cg);
    if (!s) {
      token_t tok = le ? le->tok : (token_t){0};
      return expr_fail(cg, tok, "'in' requires 'contains' (usually in std.core)");
    }
    return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){r, l}, 2, "");
  }

  LLVMValueRef ptr_arith = ny_try_emit_ptr_arith_binary(cg, scopes, depth, kind, l, r, le, re);
  if (ptr_arith)
    return ptr_arith;

  LLVMValueRef scoped_operator = ny_try_emit_scoped_operator(cg, scopes, depth, op, l, r, le, re);
  if (scoped_operator)
    return scoped_operator;

  LLVMValueRef direct_str_concat =
      ny_try_emit_direct_str_concat(cg, scopes, depth, kind, l, r, le, re);
  if (direct_str_concat)
    return direct_str_concat;

  if (kind == NY_BINOP_POW) {
    int64_t li = 0, ri = 0, out = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri) &&
        ny_const_int_pow_exact(li, ri, &out))
      return ny_const_tagged_int_value(cg, out);
  }

  if ((kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL) &&
      (ny_bin_expr_is_static_bigintish(cg, scopes, depth, le, 0) ||
       ny_bin_expr_is_static_bigintish(cg, scopes, depth, re, 0))) {
    fun_sig *builtin = builtin_name ? lookup_fun(cg, builtin_name, 0) : NULL;
    if (builtin)
      return LLVMBuildCall2(cg->builder, builtin->type, builtin->value,
                            (LLVMValueRef[]){l, r}, 2, "");
  }

  if (kind == NY_BINOP_NE) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri))
      return ny_const_tagged_bool_value(cg, li != ri);
    if (allow_fast_int_numeric) {
      LLVMValueRef cmp = ny_ne(cg, l, r, "ne_fast");
      return ny_tag_bool(cg, cmp);
    }
    LLVMValueRef eq_res = gen_binary(cg, scopes, depth, "==", l, r, le, re);
    return ny_select(cg, to_bool(cg, eq_res), ny_cfalse(cg), ny_ctrue(cg), "ne_from_eq");
  }

  if (kind == NY_BINOP_EQ) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri))
      return ny_const_tagged_bool_value(cg, li == ri);
    if (allow_fast_int_numeric) {
      LLVMValueRef cmp = ny_eq(cg, l, r, "eq_fast");
      return ny_tag_bool(cg, cmp);
    }
    /* Direct equality check against boolean immediates.
       These are not tagged ints, so the tagged-int fast path would waste
       cycles checking LSB and then constant-fold to false. Just compare. */
    int64_t l_const = 0, r_const = 0;
    bool l_is_const = LLVMIsConstant(l) && ny_const_tagged_int(l, &l_const);
    bool r_is_const = LLVMIsConstant(r) && ny_const_tagged_int(r, &r_const);
    if ((l_is_const && (l_const == NY_IMM_TRUE || l_const == NY_IMM_FALSE)) ||
        (r_is_const && (r_const == NY_IMM_TRUE || r_const == NY_IMM_FALSE))) {
      LLVMValueRef cmp = ny_eq(cg, l, r, "bool_eq");
      return ny_tag_bool(cg, cmp);
    }
       fun_sig *s = prefer_builtin_ops ? lookup_fun(cg, "__eq", 0) : ny_helper_eq(cg);
       if (!s)
         return expr_fail(cg, (token_t){0}, "'==' requires 'eq' (or __eq)");
       const char *lt = infer_expr_type(cg, scopes, depth, le);
       const char *rt = infer_expr_type(cg, scopes, depth, re);
       if (ny_bin_eq_type_prefers_builtin(lt, rt) ||
           ny_bin_expr_is_builtin_eq_shape(cg, scopes, depth, le) ||
           ny_bin_expr_is_builtin_eq_shape(cg, scopes, depth, re)) {
         fun_sig *builtin_eq = lookup_fun(cg, "__eq", 0);
         if (builtin_eq)
           return LLVMBuildCall2(cg->builder, builtin_eq->type, builtin_eq->value,
                                 (LLVMValueRef[]){l, r}, 2, "");
       }
       LLVMValueRef fast = ny_try_emit_float_fast_binary(cg, entry, l, r, s, scopes, depth, le, re);
    if (fast)
      return fast;
    return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){l, r}, 2, "");
  }

  if (builtin_name) {
    if (!allow_fast_int_numeric)
      goto skip_fast_builtin_numeric;
    /* Note: Complete tag elimination disabled */
    /* LLVMValueRef raw =
        ny_try_emit_raw_int_binary(cg, scopes, depth, entry, l, r, le, re);
    if (raw)
      return raw; */

    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri)) {
      if (kind == NY_BINOP_ADD) {
        int64_t out = 0;
        if (!__builtin_add_overflow(li, ri, &out) && ny_small_int_fits_i64(out))
          return ny_const_tagged_int_value(cg, out);
      }
      if (kind == NY_BINOP_SUB) {
        int64_t out = 0;
        if (!__builtin_sub_overflow(li, ri, &out) && ny_small_int_fits_i64(out))
          return ny_const_tagged_int_value(cg, out);
      }
      if (kind == NY_BINOP_MUL) {
        int64_t out = 0;
        if (!__builtin_mul_overflow(li, ri, &out) && ny_small_int_fits_i64(out))
          return ny_const_tagged_int_value(cg, out);
      }
      if (kind == NY_BINOP_DIV) {
        if (ri == 0)
          goto skip_fast_builtin_numeric;
        if (li == INT64_MIN && ri == -1)
          goto skip_const_div_fold;
        if (ri == 1)
          return ny_const_tagged_int_value(cg, li);
        return ny_const_tagged_int_value(cg, li / ri);
      }
    skip_const_div_fold:
      if (kind == NY_BINOP_MOD) {
        if (ri == 0)
          goto skip_fast_builtin_numeric;
        if (ri == 1 || ri == -1)
          return ny_const_tagged_int_value(cg, 0);
        return ny_const_tagged_int_value(cg, li % ri);
      }
      if (kind == NY_BINOP_AND)
        return ny_const_tagged_int_value(cg, li & ri);
      if (kind == NY_BINOP_OR)
        return ny_const_tagged_int_value(cg, li | ri);
      if (kind == NY_BINOP_XOR)
        return ny_const_tagged_int_value(cg, li ^ ri);
      if (kind == NY_BINOP_LT)
        return ny_const_tagged_bool_value(cg, li < ri);
      if (kind == NY_BINOP_LE)
        return ny_const_tagged_bool_value(cg, li <= ri);
      if (kind == NY_BINOP_GT)
        return ny_const_tagged_bool_value(cg, li > ri);
      if (kind == NY_BINOP_GE)
        return ny_const_tagged_bool_value(cg, li >= ri);
      if (kind == NY_BINOP_SHL && ri >= 0 && ri < 64)
        return ny_const_tagged_int_value(cg, (int64_t)(((uint64_t)li) << ri));
      if (kind == NY_BINOP_SHR && ri >= 0 && ri < 64)
        return ny_const_tagged_int_value(cg, (int64_t)(((uint64_t)li) >> ri));
    }
  }
skip_fast_builtin_numeric:

  if (generic_name && !prefer_builtin_ops) {
    char full_generic[128];
    snprintf(full_generic, sizeof(full_generic), "std.core.reflect.%s", generic_name);
    fun_sig *s = lookup_fun(cg, full_generic, 0);
    if (!s) {
      char core_generic[128];
      snprintf(core_generic, sizeof(core_generic), "std.core.%s", generic_name);
      s = lookup_fun(cg, core_generic, 0);
    }
    if (!s)
      s = lookup_fun(cg, generic_name, 0);
    if (s && (!builtin_name || strcmp(s->name, builtin_name) != 0)) {
      if (s->stmt_t && !ny_is_stdlib_tok(s->stmt_t->tok))
        s = NULL;
    }
    if (s && (!builtin_name || strcmp(s->name, builtin_name) != 0)) {
      if (builtin_name) {
        fun_sig *builtin_s = lookup_fun(cg, builtin_name, 0);
        fun_sig *fast_fallback =
            (builtin_s && (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL))
                ? builtin_s
                : s;
        LLVMValueRef fast = allow_fast_int_numeric
                                ? ny_try_emit_float_fast_binary(cg, entry, l, r, fast_fallback,
                                                                scopes, depth, le, re)
                                : NULL;
        if (fast)
          return fast;
        fast = (allow_fast_int_numeric || allow_dynamic_int_numeric)
                   ? ny_try_emit_tagged_int_fast_binary(cg, scopes, depth, entry, l, r, le, re,
                                                        fast_fallback)
                   : NULL;
        if (fast)
          return fast;
        if (fast_fallback != s)
          return LLVMBuildCall2(cg->builder, fast_fallback->type, fast_fallback->value,
                                (LLVMValueRef[]){l, r}, 2, "");
      }
      return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){l, r}, 2, "");
    }
  }

  /* When prefer_builtin_ops is true, still try std.core.reflect generic as
   * fallback for non-numeric types (lists, strings, dicts). The builtin
   * __add/__mul only handle integers. If the generic exists, we bypass fast int
   * paths for non-int operands by letting the generic function handle type
   * dispatch. */
  if (generic_name && prefer_builtin_ops) {
    char full_generic[128];
    snprintf(full_generic, sizeof(full_generic), "std.core.reflect.%s", generic_name);
    fun_sig *gs = lookup_fun(cg, full_generic, 0);
    if (!gs) {
      char core_generic[128];
      snprintf(core_generic, sizeof(core_generic), "std.core.%s", generic_name);
      gs = lookup_fun(cg, core_generic, 0);
    }
    if (!gs)
      gs = lookup_fun(cg, generic_name, 0);
    if (gs && gs->stmt_t && ny_is_stdlib_tok(gs->stmt_t->tok)) {
      /* Generic exists in stdlib - try fast paths first, then fall through to
       * generic */
      if (builtin_name) {
        fun_sig *s = lookup_fun(cg, builtin_name, 0);
        LLVMValueRef fast = (s && allow_fast_int_numeric)
                                ? ny_try_emit_float_fast_binary(cg, entry, l, r, s, scopes, depth,
                                                                le, re)
                                : NULL;
        if (fast)
          return fast;
        if (allow_fast_int_numeric || allow_dynamic_int_numeric) {
          fast = s ? ny_try_emit_tagged_int_fast_binary(cg, scopes, depth, entry, l, r, le, re, s)
                   : NULL;
          if (fast)
            return fast;
        }
      }
      /* Not proven integers - let std.core.reflect dispatch by type */
      return LLVMBuildCall2(cg->builder, gs->type, gs->value, (LLVMValueRef[]){l, r}, 2, "");
    }
  }

  LLVMValueRef missing_vector_operator =
      ny_reject_missing_vector_operator(cg, scopes, depth, kind, op, le, re);
  if (missing_vector_operator)
    return missing_vector_operator;

  if (builtin_name) {
    fun_sig *s = lookup_fun(cg, builtin_name, 0);
    if (!s)
      return expr_fail(cg, (token_t){0}, "builtin %s missing", builtin_name);
    LLVMValueRef fast = allow_fast_int_numeric
                            ? ny_try_emit_float_fast_binary(cg, entry, l, r, s, scopes, depth, le,
                                                            re)
                            : NULL;
    if (fast)
      return fast;
    fast = (allow_fast_int_numeric || allow_dynamic_int_numeric)
               ? ny_try_emit_tagged_int_fast_binary(cg, scopes, depth, entry, l, r, le, re, s)
               : NULL;
    if (fast)
      return fast;
    return LLVMBuildCall2(cg->builder, s->type, s->value, (LLVMValueRef[]){l, r}, 2, "");
  }

  return expr_fail(cg, (token_t){0}, "undefined operator '%s'", op);
}
