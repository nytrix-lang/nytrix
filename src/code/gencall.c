#include "base/util.h"
#include "parse/json.h"
#include "priv.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <ctype.h>
#include <limits.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

static int parse_runtime_call_arity(const char *name) {
  if (!name || strncmp(name, "__call", 6) != 0)
    return -1;
  const char *num = name + 6;
  if (!*num)
    return -1;
  int arity = 0;
  for (; *num; ++num) {
    if (*num < '0' || *num > '9')
      return -1;
    arity = arity * 10 + (*num - '0');
  }
  return arity;
}

static LLVMValueRef ny_cast_to_i64(codegen_t *cg, LLVMValueRef v,
                                   const char *name) {
  if (!cg || !v)
    return v;
  if (LLVMTypeOf(v) == cg->type_i64)
    return v;
  return ny_ptr2i64(cg, v, ny_llvm_name(cg, name));
}

static LLVMValueRef ny_build_is_ptr_pred(codegen_t *cg, LLVMValueRef v,
                                         const char *name) {
  LLVMValueRef nonzero = LLVMBuildICmp(cg->builder, LLVMIntNE, v, ny_c0(cg),
                                       name ? name : "ptr_nz");
  LLVMValueRef low_bits =
      ny_and(cg, v, LLVMConstInt(cg->type_i64, NY_VALUE_PTR_TAG_MASK, false),
             "ptr_low_bits");
  LLVMValueRef aligned = ny_eq(cg, low_bits, ny_c0(cg), "ptr_aligned");
  LLVMValueRef gt_min = ny_ugt(
      cg, v, LLVMConstInt(cg->type_i64, (uint64_t)NY_VALUE_PTR_MIN_ADDR, false),
      "ptr_gt_min");
  return ny_and(cg, nonzero,
                ny_and(cg, aligned, gt_min, NY_LLVM_NAME(cg, "ptr_and")),
                "ptr_pred");
}

static LLVMValueRef ny_build_untagged_or_raw_i64(codegen_t *cg, LLVMValueRef v,
                                                 const char *name) {
  LLVMValueRef lsb = ny_and(cg, v, ny_c1(cg), "idx_lsb");
  LLVMValueRef is_tagged = ny_eq(cg, lsb, ny_c1(cg), "idx_is_tagged");
  LLVMValueRef untagged = ny_ashr(cg, v, ny_c1(cg), "idx_untag");
  return ny_select(cg, is_tagged, untagged, v, name ? name : "idx_raw");
}

static bool ny_gencall_str_in(const char *s, const char *const *vals,
                              size_t n) {
  if (!s)
    return false;
  for (size_t i = 0; i < n; i++)
    if (strcmp(s, vals[i]) == 0)
      return true;
  return false;
}

static binding *ny_gencall_lookup_binding(codegen_t *cg, scope *scopes,
                                          size_t depth, const char *name,
                                          size_t name_len, uint64_t hash);
static fun_sig *ny_gencall_lookup_source_file_fun(codegen_t *cg,
                                                  const char *tail_name,
                                                  token_t tok, size_t argc);
static LLVMValueRef ny_gencall_const_bool(codegen_t *cg, bool value,
                                          const char *name);
static const char *ny_static_assert_message(expr_t *msg, char *buf, size_t cap);

static LLVMValueRef ny_gencall_index_raw_i64(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *idx_expr,
                                             LLVMValueRef idx_v,
                                             const char *name) {
  idx_v = ny_cast_to_i64(cg, idx_v, name ? name : "idx");
  int64_t lit = 0;
  if (ny_expr_literal_i64(idx_expr, &lit))
    return LLVMConstInt(cg->type_i64, (uint64_t)lit, true);
  if (idx_expr && idx_expr->kind == NY_E_IDENT && idx_expr->as.ident.name) {
    size_t name_len = (size_t)idx_expr->tok.len;
    if (name_len == 0)
      name_len = strlen(idx_expr->as.ident.name);
    binding *b =
        ny_gencall_lookup_binding(cg, scopes, depth, idx_expr->as.ident.name,
                                  name_len, idx_expr->as.ident.hash);
    if (b && b->raw_int_value && b->is_int_direct)
      return b->raw_int_value;
  }
  if (ny_is_proven_int(cg, scopes, depth, idx_expr, idx_v))
    return ny_untag_int(cg, idx_v);
  return ny_build_untagged_or_raw_i64(cg, idx_v, name ? name : "idx_raw");
}

static LLVMValueRef ny_build_rt_untag_i64(codegen_t *cg, LLVMValueRef v,
                                          const char *name) {
  LLVMValueRef low1 = ny_and(cg, v, ny_c1(cg), "rt_untag_low1");
  LLVMValueRef is_int = ny_eq(cg, low1, ny_c1(cg), "rt_untag_is_int");
  LLVMValueRef int_raw = ny_ashr(cg, v, ny_c1(cg), "rt_untag_int");
  LLVMValueRef low3 =
      ny_and(cg, v, LLVMConstInt(cg->type_i64, NY_NATIVE_TAG_MASK, false),
             "rt_untag_low3");
  LLVMValueRef is_native =
      ny_eq(cg, low3, LLVMConstInt(cg->type_i64, NY_NATIVE_TAG, false),
            "rt_untag_is_native");
  LLVMValueRef native_raw =
      ny_ashr(cg, v, LLVMConstInt(cg->type_i64, NY_NATIVE_SHIFT, false),
              "rt_untag_native");
  LLVMValueRef not_int_raw =
      ny_select(cg, is_native, native_raw, v, "rt_untag_not_int");
  return ny_select(cg, is_int, int_raw, not_int_raw, name ? name : "rt_untag");
}

static fun_sig *ny_gencall_flt_box(codegen_t *cg);
static LLVMValueRef abi_untag_proven_int_fast(codegen_t *cg, LLVMValueRef v);

static bool ny_proven_int_cast_fast_allowed(expr_t *arg) {
  (void)arg;
  return true;
}

static bool ny_proven_int_cast_fast_enabled(codegen_t *cg) {
  if (cg && cg->env_cache.proven_int_cast_fast != 0)
    return cg->env_cache.proven_int_cast_fast == 1;
  bool enabled = ny_env_enabled_default_on("NYTRIX_PROVEN_INT_CAST_FAST");
  if (cg)
    cg->env_cache.proven_int_cast_fast = enabled ? 1 : -1;
  return enabled;
}

static bool ny_is_unshadowed_builtin_callee(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *callee,
                                            const char **out_name) {
  size_t name_len = 0;
  uint64_t hash = 0;
  const char *name =
      ny_builtin_surface_name_for_callee(callee, &name_len, &hash);
  if (out_name)
    *out_name = name;
  if (!name || !*name)
    return false;
  return !ny_builtin_name_shadowed_by_user_symbol(cg, scopes, depth, name,
                                                  name_len, hash);
}

static binding *ny_gencall_lookup_binding(codegen_t *cg, scope *scopes,
                                          size_t depth, const char *name,
                                          size_t name_len, uint64_t hash) {
  return lookup_binding_hash(cg, scopes, depth, name, name_len, hash);
}

static bool ny_gencall_small_int_fits_i64(int64_t raw) {
  return raw >= INT64_C(-4611686018427387904) &&
         raw <= INT64_C(4611686018427387903);
}

static bool ny_gencall_const_small_int_value(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             int64_t *out, unsigned recursion) {
  if (!e || !out || recursion > 32)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind != NY_LIT_INT || e->tok.kind == NY_T_NIL ||
        !ny_gencall_small_int_fits_i64(e->as.literal.as.i))
      return false;
    *out = e->as.literal.as.i;
    return true;
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                           name_len, e->as.ident.hash);
    expr_t *init =
        b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
    if (!init || init == e)
      return false;
    return ny_gencall_const_small_int_value(cg, scopes, depth, init, out,
                                            recursion + 1);
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right)
      return false;
    int64_t r = 0, v = 0;
    if (!ny_gencall_const_small_int_value(cg, scopes, depth, e->as.unary.right,
                                          &r, recursion + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *out = r;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (__builtin_sub_overflow((int64_t)0, r, &v) ||
          !ny_gencall_small_int_fits_i64(v))
        return false;
      *out = v;
      return true;
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      v = ~r;
      if (!ny_gencall_small_int_fits_i64(v))
        return false;
      *out = v;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    if (!e->as.binary.op)
      return false;
    int64_t l = 0, r = 0, v = 0;
    if (!ny_gencall_const_small_int_value(cg, scopes, depth, e->as.binary.left,
                                          &l, recursion + 1) ||
        !ny_gencall_const_small_int_value(cg, scopes, depth, e->as.binary.right,
                                          &r, recursion + 1))
      return false;
    if (strcmp(e->as.binary.op, "+") == 0) {
      if (__builtin_add_overflow(l, r, &v))
        return false;
    } else if (strcmp(e->as.binary.op, "-") == 0) {
      if (__builtin_sub_overflow(l, r, &v))
        return false;
    } else if (strcmp(e->as.binary.op, "*") == 0) {
      if (__builtin_mul_overflow(l, r, &v))
        return false;
    } else if (strcmp(e->as.binary.op, "/") == 0) {
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      v = l / r;
    } else if (strcmp(e->as.binary.op, "%") == 0) {
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      v = l % r;
    } else {
      return false;
    }
    if (!ny_gencall_small_int_fits_i64(v))
      return false;
    *out = v;
    return true;
  }
  default:
    return false;
  }
}

static bool ny_gencall_type_is_fixed_int_leaf(const char *leaf) {
  static const char *const ints[] = {"i8",   "i16",  "i32", "i64",
                                     "i128", "u8",   "u16", "u32",
                                     "u64",  "u128", "char"};
  return ny_gencall_str_in(leaf, ints, sizeof(ints) / sizeof(ints[0]));
}

static const char *ny_gencall_static_surface_type(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e,
                                                  unsigned recursion) {
  if (!e || recursion > 32)
    return NULL;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      if (e->tok.kind == NY_T_NIL)
        return "none";
      return ny_gencall_small_int_fits_i64(e->as.literal.as.i) ? "int"
                                                               : "bigint";
    }
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return "float";
    if (e->as.literal.kind == NY_LIT_BOOL)
      return "bool";
    if (e->as.literal.kind == NY_LIT_STR)
      return "str";
    return NULL;
  case NY_E_LIST:
    return "list";
  case NY_E_TUPLE:
    return "tuple";
  case NY_E_DICT:
    return "dict";
  case NY_E_SET:
    return "set";
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return NULL;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                           name_len, e->as.ident.hash);
    expr_t *init =
        b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
    if (init && init != e) {
      const char *it = ny_gencall_static_surface_type(cg, scopes, depth, init,
                                                      recursion + 1);
      if (it)
        return it;
    }
    const char *leaf = ny_type_leaf(infer_expr_type(cg, scopes, depth, e));
    if (ny_gencall_type_is_fixed_int_leaf(leaf))
      return "int";
    static const char *const bigint_names[] = {"bigint", "BigInt"};
    static const char *const float_names[] = {"f32", "f64", "f128", "float"};
    static const char *const direct_names[] = {"bool", "str", "ptr",
                                               "handle"};
    if (ny_gencall_str_in(leaf, bigint_names,
                          sizeof(bigint_names) / sizeof(bigint_names[0])))
      return "bigint";
    if (ny_gencall_str_in(leaf, float_names,
                          sizeof(float_names) / sizeof(float_names[0])))
      return "float";
    if (ny_gencall_str_in(leaf, direct_names,
                          sizeof(direct_names) / sizeof(direct_names[0])))
      return leaf;
    return NULL;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right)
      return NULL;
    const char *rt = ny_gencall_static_surface_type(
        cg, scopes, depth, e->as.unary.right, recursion + 1);
    if (rt && strcmp(rt, "bigint") == 0)
      return "bigint";
    int64_t raw = 0;
    if (ny_gencall_const_small_int_value(cg, scopes, depth, e, &raw, 0))
      return "int";
    return NULL;
  }
  case NY_E_BINARY: {
    if (!e->as.binary.op)
      return NULL;
    const char *op = e->as.binary.op;
    bool arith =
        strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0;
    if (!arith)
      return NULL;
    const char *lt = ny_gencall_static_surface_type(
        cg, scopes, depth, e->as.binary.left, recursion + 1);
    const char *rt = ny_gencall_static_surface_type(
        cg, scopes, depth, e->as.binary.right, recursion + 1);
    if ((lt && strcmp(lt, "bigint") == 0) || (rt && strcmp(rt, "bigint") == 0))
      return "bigint";
    int64_t raw = 0;
    if (ny_gencall_const_small_int_value(cg, scopes, depth, e, &raw, 0))
      return "int";
    if (lt && rt && strcmp(lt, "int") == 0 && strcmp(rt, "int") == 0)
      return "bigint";
    return NULL;
  }
  default:
    return NULL;
  }
}

static bool ny_gencall_type_is_raw_to_str_scalar(const char *type_name) {
  const char *leaf = ny_type_leaf(type_name);
  static const char *const scalar_names[] = {
      "int",    "bigint", "BigInt", "float", "f32",  "f64", "f128",
      "bool",   "str",    "nil",    "none",  "ptr",  "handle"};
  return ny_gencall_type_is_fixed_int_leaf(leaf) ||
         ny_gencall_str_in(leaf, scalar_names,
                           sizeof(scalar_names) / sizeof(scalar_names[0]));
}

static bool ny_gencall_name_matches_prefix_tail(const char *name,
                                                const char *prefix,
                                                const char *tail) {
  size_t prefix_len = strlen(prefix);
  size_t tail_len = strlen(tail);
  return strncmp(name, prefix, prefix_len) == 0 &&
         strlen(name + prefix_len) == tail_len &&
         memcmp(name + prefix_len, tail, tail_len) == 0;
}

static bool ny_gencall_builtin_name_is(const char *name, const char *tail,
                                       bool shadowed) {
  if (!name || !tail)
    return false;
  if (strcmp(name, tail) == 0)
    return !shadowed;
  const char *leaf = strrchr(name, '.');
  if (leaf && leaf[1] && strcmp(leaf + 1, tail) == 0 &&
      strncmp(name, "std.", 4) == 0)
    return true;
  if (ny_gencall_name_matches_prefix_tail(name, "std.core.", tail))
    return true;
  if (ny_gencall_name_matches_prefix_tail(name, "std.core.reflect.", tail))
    return true;
  if (ny_gencall_name_matches_prefix_tail(name, "std.core.primitives.", tail))
    return true;
  if (ny_gencall_name_matches_prefix_tail(name, "std.core.syntax.type.", tail))
    return true;
  return false;
}

static bool ny_gencall_const_str_bytes(expr_t *e, const char **out,
                                       size_t *out_len) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  if (out)
    *out = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
  if (out_len)
    *out_len = e->as.literal.as.s.len;
  return true;
}

static LLVMValueRef ny_try_const_runtime_tag_builtin(
    codegen_t *cg, const char *name, bool shadowed, expr_call_t *c) {
  if (!cg || !c || shadowed || c->args.len != 1 || !name)
    return NULL;
  if (strcmp(name, "__runtime_tag") != 0 &&
      !ny_gencall_builtin_name_is(name, "runtime_tag_raw", shadowed))
    return NULL;
  const char *s = NULL;
  size_t n = 0;
  if (!ny_gencall_const_str_bytes(c->args.data[0].val, &s, &n))
    return NULL;
  int64_t raw = rt_runtime_tag_raw_name(s, n);
  return ny_ci(cg, (((uint64_t)raw) << 1) | 1u);
}

static bool ny_gencall_expr_is_int_index(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *e) {
  if (ny_expr_literal_i64(e, NULL))
    return true;
  const char *t = infer_expr_type(cg, scopes, depth, e);
  return t && (ny_gencall_type_is(t, "int") || ny_gencall_type_is(t, "i8") ||
               ny_gencall_type_is(t, "i16") || ny_gencall_type_is(t, "i32") ||
               ny_gencall_type_is(t, "i64") || ny_gencall_type_is(t, "u8") ||
               ny_gencall_type_is(t, "u16") || ny_gencall_type_is(t, "u32") ||
               ny_gencall_type_is(t, "u64"));
}

static bool ny_gencall_expr_int_range(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *e, int64_t *out_min,
                                      int64_t *out_max);
static bool ny_gencall_list_len_min(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *target, int64_t *out_min_len);

static bool ny_gencall_expr_is_safe_fast_set_index(codegen_t *cg, scope *scopes,
                                                   size_t depth, expr_t *e) {
  int64_t lit = 0;
  if (ny_expr_literal_i64(e, &lit))
    return lit >= 0;
  int64_t idx_min = 0, idx_max = 0;
  if (ny_gencall_expr_int_range(cg, scopes, depth, e, &idx_min, &idx_max))
    return idx_min >= 0;
  if (e && e->kind == NY_E_IDENT && e->as.ident.name) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                           name_len, e->as.ident.hash);
    if (b && b->has_int_range)
      return b->int_min_raw >= 0;
  }
  const char *t = infer_expr_type(cg, scopes, depth, e);
  return t && (ny_gencall_type_is(t, "u8") || ny_gencall_type_is(t, "u16") ||
               ny_gencall_type_is(t, "u32") || ny_gencall_type_is(t, "u64"));
}

static bool ny_gencall_expr_int_range(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *e, int64_t *out_min,
                                      int64_t *out_max) {
  int64_t lit = 0;
  if (ny_expr_literal_i64(e, &lit)) {
    if (out_min)
      *out_min = lit;
    if (out_max)
      *out_max = lit;
    return true;
  }
  if (e && e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name && e->as.call.args.len == 1) {
    size_t name_len = 0;
    uint64_t name_hash = 0;
    const char *name = ny_builtin_surface_name_for_callee(
        e->as.call.callee, &name_len, &name_hash);
    bool shadowed = ny_builtin_name_shadowed_by_user_symbol(
        cg, scopes, depth, name, name_len, name_hash);
    if (!name || shadowed || !ny_name_tail_is(name, "len"))
      return false;
    int64_t len = 0;
    if (ny_gencall_list_len_min(cg, scopes, depth, e->as.call.args.data[0].val,
                                &len)) {
      if (out_min)
        *out_min = len;
      if (out_max)
        *out_max = len;
      return true;
    }
  }
  if (e && e->kind == NY_E_BINARY && e->as.binary.op) {
    int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
    if (!ny_gencall_expr_int_range(cg, scopes, depth, e->as.binary.left, &lmin,
                                   &lmax) ||
        !ny_gencall_expr_int_range(cg, scopes, depth, e->as.binary.right, &rmin,
                                   &rmax))
      return false;
    const char *op = e->as.binary.op;
    int64_t lo = 0, hi = 0;
    if (strcmp(op, "+") == 0) {
      if (!ny_add_range_ok(lmin, rmin, &lo) ||
          !ny_add_range_ok(lmax, rmax, &hi))
        return false;
    } else if (strcmp(op, "-") == 0) {
      if (!ny_sub_range_ok(lmin, rmax, &lo) ||
          !ny_sub_range_ok(lmax, rmin, &hi))
        return false;
    } else if (strcmp(op, "*") == 0) {
      int64_t c[4];
      if (!ny_mul_range_ok(lmin, rmin, &c[0]) ||
          !ny_mul_range_ok(lmin, rmax, &c[1]) ||
          !ny_mul_range_ok(lmax, rmin, &c[2]) ||
          !ny_mul_range_ok(lmax, rmax, &c[3]))
        return false;
      lo = c[0];
      hi = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < lo)
          lo = c[i];
        if (c[i] > hi)
          hi = c[i];
      }
    } else if (strcmp(op, "%") == 0) {
      if (rmin != rmax || rmax <= 0 || lmin < 0)
        return false;
      lo = 0;
      hi = lmax < rmax ? lmax : rmax - 1;
    } else if (strcmp(op, "&") == 0) {
      if (rmin != rmax || rmax < 0 || lmin < 0)
        return false;
      lo = 0;
      hi = rmax;
    } else {
      return false;
    }
    if (lo > hi)
      return false;
    if (out_min)
      *out_min = lo;
    if (out_max)
      *out_max = hi;
    return true;
  }
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b = ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                         name_len, e->as.ident.hash);
  if (b && b->has_int_range) {
    if (out_min)
      *out_min = b->int_min_raw;
    if (out_max)
      *out_max = b->int_max_raw;
    return true;
  }
  expr_t *init =
      b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
  if (ny_expr_literal_i64(init, &lit)) {
    if (out_min)
      *out_min = lit;
    if (out_max)
      *out_max = lit;
    return true;
  }
  if (init && init != e)
    return ny_gencall_expr_int_range(cg, scopes, depth, init, out_min, out_max);
  return false;
}

static bool ny_gencall_list_len_min(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *target, int64_t *out_min_len) {
  if (!target)
    return false;
  if (ny_expr_is_list_or_tuple_lit(target)) {
    if (out_min_len)
      *out_min_len = (int64_t)target->as.list_like.len;
    return true;
  }
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b =
      ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                name_len, target->as.ident.hash);
  if (!b || !b->has_list_len_min)
    return false;
  if (out_min_len)
    *out_min_len = b->list_len_min_raw;
  return true;
}

static bool ny_gencall_expr_in_list_len_min(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *target,
                                            expr_t *key) {
  int64_t idx_min = 0, idx_max = 0, len_min = 0;
  if (!ny_gencall_expr_int_range(cg, scopes, depth, key, &idx_min, &idx_max))
    return false;
  if (idx_min < 0)
    return false;
  if (!ny_gencall_list_len_min(cg, scopes, depth, target, &len_min))
    return false;
  return len_min > 0 && idx_max < len_min;
}

static bool ny_compile_range_builtin_name_is(const char *name, bool shadowed,
                                             const char *leaf) {
  if (!name || !leaf)
    return false;
  if (strcmp(name, leaf) == 0)
    return !shadowed;
  char qname[128];
  snprintf(qname, sizeof(qname), "std.core.%s", leaf);
  return strcmp(name, qname) == 0;
}

static bool ny_gencall_exact_int_value(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e, int64_t *out) {
  int64_t lo = 0, hi = 0;
  if (!ny_gencall_expr_int_range(cg, scopes, depth, e, &lo, &hi))
    return false;
  if (lo != hi)
    return false;
  if (out)
    *out = lo;
  return true;
}

static bool ny_gencall_range_is_proven(
    codegen_t *cg, scope *scopes, size_t depth, expr_t *value, expr_t *lo_expr,
    expr_t *hi_expr, int64_t *out_vlo, int64_t *out_vhi, int64_t *out_lo,
    int64_t *out_hi, bool *out_has_value, bool *out_has_bounds) {
  int64_t vlo = 0, vhi = 0, lo = 0, hi = 0;
  bool has_value =
      ny_gencall_expr_int_range(cg, scopes, depth, value, &vlo, &vhi);
  bool has_bounds =
      ny_gencall_exact_int_value(cg, scopes, depth, lo_expr, &lo) &&
      ny_gencall_exact_int_value(cg, scopes, depth, hi_expr, &hi);
  if (out_vlo)
    *out_vlo = vlo;
  if (out_vhi)
    *out_vhi = vhi;
  if (out_lo)
    *out_lo = lo;
  if (out_hi)
    *out_hi = hi;
  if (out_has_value)
    *out_has_value = has_value;
  if (out_has_bounds)
    *out_has_bounds = has_bounds;
  return has_value && has_bounds && lo <= hi && vlo >= lo && vhi <= hi;
}

static LLVMValueRef ny_try_compile_range_builtin(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 const char *name,
                                                 bool shadowed,
                                                 expr_call_t *c) {
  if (!cg || !e || !name || !c)
    return NULL;

  bool want_range_proven =
      ny_compile_range_builtin_name_is(name, shadowed, "range_proven");
  bool want_index_proven =
      ny_compile_range_builtin_name_is(name, shadowed, "index_proven");
  bool want_assert_range =
      ny_compile_range_builtin_name_is(name, shadowed, "assert_compile_range");
  bool want_assert_index =
      ny_compile_range_builtin_name_is(name, shadowed, "assert_compile_index");
  if (!want_range_proven && !want_index_proven && !want_assert_range &&
      !want_assert_index)
    return NULL;

  if (want_range_proven || want_assert_range) {
    bool want_assert = want_assert_range;
    size_t min_args = 3, max_args = want_assert ? 4 : 3;
    if (c->args.len < min_args || c->args.len > max_args) {
      ny_diag_error(e->tok, "%s expects value, min, max%s", name,
                    want_assert ? ", and optional message" : "");
      cg->had_error = 1;
      return ny_gencall_const_bool(cg, want_assert, "compile_range_bad_arity");
    }

    char msg_buf[512];
    const char *msg = "compile-time range assertion failed";
    if (want_assert && c->args.len == 4) {
      msg = ny_static_assert_message(c->args.data[3].val, msg_buf,
                                     sizeof(msg_buf));
      if (!msg) {
        ny_diag_error(c->args.data[3].val ? c->args.data[3].val->tok : e->tok,
                      "%s message must be a string literal", name);
        cg->had_error = 1;
        msg = "compile-time range assertion failed";
      }
    }

    int64_t vlo = 0, vhi = 0, lo = 0, hi = 0;
    bool has_value = false, has_bounds = false;
    bool proven = ny_gencall_range_is_proven(
        cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
        c->args.data[2].val, &vlo, &vhi, &lo, &hi, &has_value, &has_bounds);
    if (!want_assert)
      return ny_gencall_const_bool(cg, proven, "range_proven");

    if (!has_bounds || lo > hi) {
      ny_diag_error(e->tok, "%s bounds must be exact compile-time integers",
                    name);
      ny_diag_hint("use literal bounds or values whose integer range has a "
                   "single value");
      cg->had_error = 1;
      return ny_gencall_const_bool(cg, true, "assert_compile_range_bad_bounds");
    }
    if (!has_value) {
      ny_diag_error(c->args.data[0].val ? c->args.data[0].val->tok : e->tok,
                    "%s could not prove an integer range for the value", name);
      ny_diag_hint("range proofs currently understand int bindings, literals, "
                   "len(...), +, -, *, %, &, and simple loop guards");
      cg->had_error = 1;
      return ny_gencall_const_bool(cg, true, "assert_compile_range_unknown");
    }
    if (!proven) {
      ny_diag_error(c->args.data[0].val ? c->args.data[0].val->tok : e->tok,
                    "%s", msg);
      ny_diag_hint("proved range is [%lld, %lld], required [%lld, %lld]",
                   (long long)vlo, (long long)vhi, (long long)lo,
                   (long long)hi);
      cg->had_error = 1;
    }
    return ny_gencall_const_bool(cg, true, "assert_compile_range_ok");
  }

  bool want_assert = want_assert_index;
  size_t min_args = 2, max_args = want_assert ? 3 : 2;
  if (c->args.len < min_args || c->args.len > max_args) {
    ny_diag_error(e->tok, "%s expects container, index%s", name,
                  want_assert ? ", and optional message" : "");
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, want_assert, "compile_index_bad_arity");
  }

  int64_t idx_min = 0, idx_max = 0, len_min = 0;
  bool has_idx = ny_gencall_expr_int_range(
      cg, scopes, depth, c->args.data[1].val, &idx_min, &idx_max);
  bool has_len =
      ny_gencall_list_len_min(cg, scopes, depth, c->args.data[0].val, &len_min);
  bool proven =
      has_idx && has_len && len_min > 0 && idx_min >= 0 && idx_max < len_min;
  if (!want_assert)
    return ny_gencall_const_bool(cg, proven, "index_proven");

  char msg_buf[512];
  const char *msg = "compile-time index assertion failed";
  if (c->args.len == 3) {
    msg =
        ny_static_assert_message(c->args.data[2].val, msg_buf, sizeof(msg_buf));
    if (!msg) {
      ny_diag_error(c->args.data[2].val ? c->args.data[2].val->tok : e->tok,
                    "%s message must be a string literal", name);
      cg->had_error = 1;
      msg = "compile-time index assertion failed";
    }
  }
  if (!has_idx) {
    ny_diag_error(c->args.data[1].val ? c->args.data[1].val->tok : e->tok,
                  "%s could not prove an integer range for the index", name);
    ny_diag_hint("range proofs currently understand int bindings, literals, "
                 "len(...), +, -, *, %, &, and simple loop guards");
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, true,
                                 "assert_compile_index_unknown_index");
  }
  if (!has_len) {
    ny_diag_error(c->args.data[0].val ? c->args.data[0].val->tok : e->tok,
                  "%s could not prove a minimum container length", name);
    ny_diag_hint("use a literal list/tuple or a list binding whose minimum "
                 "length is known");
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, true, "assert_compile_index_unknown_len");
  }
  if (!proven) {
    ny_diag_error(c->args.data[1].val ? c->args.data[1].val->tok : e->tok, "%s",
                  msg);
    ny_diag_hint(
        "proved index range is [%lld, %lld], known minimum length is %lld",
        (long long)idx_min, (long long)idx_max, (long long)len_min);
    cg->had_error = 1;
  }
  return ny_gencall_const_bool(cg, true, "assert_compile_index_ok");
}

static bool ny_gencall_literal_collection_len(expr_t *e, uint64_t *out_len) {
  if (!e || (e->kind != NY_E_LIST && e->kind != NY_E_TUPLE))
    return false;
  for (size_t i = 0; i < e->as.list_like.len; ++i) {
    expr_t *item = e->as.list_like.data[i];
    if (!item || item->kind != NY_E_LITERAL)
      return false;
  }
  if (out_len)
    *out_len = (uint64_t)e->as.list_like.len;
  return true;
}

static expr_t *ny_gencall_binding_init_expr(binding *b, const char *name) {
  if (!b || b->is_mut || !name || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  if (var->is_mut)
    return NULL;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *n = var->names.data[i];
    if (n && strcmp(n, name) == 0)
      return var->exprs.data[i];
  }
  return NULL;
}

static expr_t *ny_gencall_static_init_expr(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return NULL;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b = ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                         name_len, e->as.ident.hash);
  return ny_gencall_binding_init_expr(b, e->as.ident.name);
}

static expr_t *ny_gencall_diag_type_source(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *e) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_LITERAL || e->kind == NY_E_LIST ||
      e->kind == NY_E_TUPLE || e->kind == NY_E_DICT || e->kind == NY_E_SET)
    return e;
  expr_t *init = ny_gencall_static_init_expr(cg, scopes, depth, e);
  if (init && (init->kind == NY_E_LITERAL || init->kind == NY_E_LIST ||
               init->kind == NY_E_TUPLE || init->kind == NY_E_DICT ||
               init->kind == NY_E_SET))
    return init;
  return NULL;
}

static LLVMValueRef ny_gencall_const_bool(codegen_t *cg, bool value,
                                          const char *name) {
  (void)name;
  return LLVMConstInt(cg->type_i64, value ? NY_IMM_TRUE : NY_IMM_FALSE, false);
}

static bool ny_gencall_const_truthy(LLVMValueRef v, bool *out) {
  if (!v || !LLVMIsAConstantInt(v))
    return false;
  LLVMTypeRef ty = LLVMTypeOf(v);
  uint64_t raw = LLVMConstIntGetZExtValue(v);
  if (ty && LLVMGetTypeKind(ty) == LLVMIntegerTypeKind &&
      LLVMGetIntTypeWidth(ty) == 1) {
    if (out)
      *out = raw != 0;
    return true;
  }
  if (out)
    *out = (raw != NY_IMM_NIL && raw != NY_IMM_FALSE && raw != 1);
  return true;
}

static const char *ny_static_assert_message(expr_t *msg, char *buf,
                                            size_t cap) {
  if (!msg)
    return "static assertion failed";
  if (msg->kind != NY_E_LITERAL || msg->as.literal.kind != NY_LIT_STR)
    return NULL;
  size_t len = msg->as.literal.as.s.len;
  if (len >= cap)
    len = cap - 1;
  memcpy(buf, msg->as.literal.as.s.data ? msg->as.literal.as.s.data : "", len);
  buf[len] = '\0';
  return buf;
}

static bool ny_compile_assert_name_is(const char *name) {
  return name && (strcmp(name, "static_assert") == 0 ||
                  strcmp(name, "assert_compile") == 0);
}

static LLVMValueRef ny_try_static_assert_builtin(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 const char *name,
                                                 bool shadowed,
                                                 expr_call_t *c) {
  if (!cg || !e || !name || shadowed || !ny_compile_assert_name_is(name))
    return NULL;
  if (!c || c->args.len < 1 || c->args.len > 2) {
    ny_diag_error(e->tok, "%s expects condition and optional message", name);
    ny_diag_hint(
        "use %s(comptime{ return cond }, \"message\") for computed checks",
        name);
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, true, "static_assert_bad_arity");
  }

  char msg_buf[512];
  const char *msg = "static assertion failed";
  if (c->args.len == 2) {
    msg =
        ny_static_assert_message(c->args.data[1].val, msg_buf, sizeof(msg_buf));
    if (!msg) {
      ny_diag_error(c->args.data[1].val ? c->args.data[1].val->tok : e->tok,
                    "%s message must be a string literal", name);
      cg->had_error = 1;
      msg = "static assertion failed";
    }
  }

  expr_t *cond = c->args.data[0].val;
  LLVMValueRef v = gen_expr(cg, scopes, depth, cond);
  bool truthy = false;
  if (!ny_gencall_const_truthy(v, &truthy)) {
    ny_diag_error(cond ? cond->tok : e->tok,
                  "%s condition must be known at compile time", name);
    ny_diag_hint("wrap computed checks in comptime{ return ... } or pass a "
                 "constant expression");
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, true, "static_assert_dynamic");
  }
  if (!truthy) {
    ny_diag_error(cond ? cond->tok : e->tok, "%s", msg);
    cg->had_error = 1;
  }
  return ny_gencall_const_bool(cg, true, "static_assert_ok");
}

static bool ny_gencall_type_is_known_float(const char *type_name) {
  return ny_gencall_type_is(type_name, "f32") ||
         ny_gencall_type_is(type_name, "f64") ||
         ny_gencall_type_is(type_name, "f128");
}

static bool ny_gencall_type_is_known_value(const char *type_name) {
  if (!type_name || ny_gencall_type_is_nullable(type_name))
    return false;
  return ny_gencall_type_is_known_non_obj(type_name) ||
         ny_gencall_type_is_known_obj(type_name) ||
         ny_gencall_type_is_known_float(type_name) ||
         ny_gencall_type_is(type_name, "range");
}

static bool ny_gencall_type_supports_len(const char *type_name) {
  return ny_gencall_type_is(type_name, "str") ||
         ny_gencall_type_is(type_name, "bytes") ||
         ny_gencall_type_is(type_name, "list") ||
         ny_gencall_type_is(type_name, "tuple") ||
         ny_gencall_type_is(type_name, "dict") ||
         ny_gencall_type_is(type_name, "set") ||
         ny_gencall_type_is(type_name, "range");
}

static bool ny_gencall_type_supports_get(const char *type_name) {
  return ny_gencall_type_is(type_name, "str") ||
         ny_gencall_type_is(type_name, "list") ||
         ny_gencall_type_is(type_name, "tuple") ||
         ny_gencall_type_is(type_name, "dict") ||
         ny_gencall_type_is(type_name, "range") ||
         ny_gencall_type_is_vec(type_name);
}

static bool ny_gencall_expr_declared_any(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  binding *b = ny_gencall_lookup_binding(
      cg, scopes, depth, e->as.ident.name, name_len,
      e->as.ident.hash ? e->as.ident.hash
                       : ny_hash_name(e->as.ident.name, name_len));
  if (!b)
    return false;
  const char *decl = b->decl_type_name ? b->decl_type_name : b->type_name;
  const char *leaf = ny_type_leaf(decl);
  return leaf && strcmp(leaf, "any") == 0;
}

static bool ny_try_bad_std_call_type_diag(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e,
                                          const char *name, bool shadowed,
                                          expr_call_t *c) {
  if (!cg || !e || !name || !c)
    return false;
  if (ny_is_stdlib_tok(e->tok))
    return false;

  bool want_len = ny_gencall_builtin_name_is(name, "len", shadowed);
  bool want_get = ny_gencall_builtin_name_is(name, "get", shadowed);

  if (want_len && c->args.len == 1) {
    expr_t *arg = c->args.data[0].val;
    expr_t *type_source = ny_gencall_diag_type_source(cg, scopes, depth, arg);
    if (!type_source)
      return false;
    const char *got = infer_expr_type(cg, scopes, depth, type_source);
    if (!got || !ny_gencall_type_is_known_value(got))
      return false;
    if (ny_gencall_type_supports_len(got))
      return false;
    ny_diag_error(e->tok, "len expects a compatible collection, got '%s'",
                  ny_type_leaf(got));
    ny_diag_hint("len supports str, bytes, list, tuple, dict, set, and range");
    cg->had_error = 1;
    return true;
  }

  if (want_get && (c->args.len == 2 || c->args.len == 3)) {
    expr_t *arg = c->args.data[0].val;
    expr_t *type_source = ny_gencall_diag_type_source(cg, scopes, depth, arg);
    if (!type_source)
      return false;
    const char *got = infer_expr_type(cg, scopes, depth, type_source);
    if (!got || !ny_gencall_type_is_known_value(got) ||
        ny_gencall_type_supports_get(got))
      return false;
    ny_diag_error(e->tok,
                  "get expects a string, list, tuple, dict, or range, got '%s'",
                  ny_type_leaf(got));
    ny_diag_hint("raw pointers and scalar values are not indexable through "
                 "std.core.get");
    cg->had_error = 1;
    return true;
  }

  return false;
}

static LLVMValueRef
ny_try_compile_time_type_builtin(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e, const char *name, bool shadowed,
                                 expr_call_t *c) {
  if (!cg || !e || !name || !c || c->args.len == 0)
    return 0;

  bool want_is_ny_obj = (strcmp(name, "__is_ny_obj") == 0);
  bool want_is_str_obj = (strcmp(name, "__is_str_obj") == 0);
  bool want_is_float_obj = (strcmp(name, "__is_float_obj") == 0);
  bool want_is_ptr = (strcmp(name, "__is_ptr") == 0);
  bool want_is_int = (strcmp(name, "__is_int") == 0) ||
                     ny_gencall_builtin_name_is(name, "is_int", shadowed);
  bool want_is_bool = ny_gencall_builtin_name_is(name, "is_bool", shadowed);
  bool want_is_nil = ny_gencall_builtin_name_is(name, "is_nil", shadowed) ||
                     ny_gencall_builtin_name_is(name, "is_none", shadowed);
  bool want_has_tag = (strcmp(name, "__has_tag") == 0);
  bool want_tagof = (strcmp(name, "__tagof") == 0);
  if (ny_gencall_builtin_name_is(name, "is_nytrix_obj", shadowed))
    want_is_ny_obj = true;
  if (ny_gencall_builtin_name_is(name, "is_float", shadowed))
    want_is_float_obj = true;
  if (ny_gencall_builtin_name_is(name, "is_ptr", shadowed))
    want_is_ptr = true;
  const char *want_named_tag = NULL;
  if (ny_gencall_builtin_name_is(name, "is_list", shadowed))
    want_named_tag = "list";
  else if (ny_gencall_builtin_name_is(name, "is_dict", shadowed))
    want_named_tag = "dict";
  else if (ny_gencall_builtin_name_is(name, "is_set", shadowed))
    want_named_tag = "set";
  else if (ny_gencall_builtin_name_is(name, "is_tuple", shadowed))
    want_named_tag = "tuple";
  else if (ny_gencall_builtin_name_is(name, "is_str", shadowed))
    want_named_tag = "str";
  else if (ny_gencall_builtin_name_is(name, "is_bytes", shadowed))
    want_named_tag = "bytes";

  if (!want_is_ny_obj && !want_is_str_obj && !want_is_float_obj &&
      !want_is_ptr && !want_is_int && !want_is_bool && !want_is_nil &&
      !want_has_tag && !want_tagof && !want_named_tag)
    return 0;
  if ((want_has_tag && c->args.len != 2) || (!want_has_tag && c->args.len != 1))
    return 0;

  expr_t *arg = c->args.data[0].val;
  const char *type_name = infer_expr_type(cg, scopes, depth, arg);
  if (!type_name)
    return 0;
  if (ny_gencall_expr_declared_any(cg, scopes, depth, arg))
    return 0;
  bool type_is_integer_number = ny_gencall_type_is_integer_number(type_name);

  if (want_is_str_obj ||
      (want_named_tag && strcmp(want_named_tag, "str") == 0)) {
    if (ny_gencall_type_is(type_name, "str"))
      return ny_gencall_const_bool(cg, true, "ct_is_str");
    if (ny_gencall_type_is_known_non_obj(type_name))
      return ny_gencall_const_bool(cg, false, "ct_is_str");
    int known_tag = ny_gencall_known_obj_tag(type_name);
    if (known_tag >= 0 && known_tag != 120 && known_tag != 121)
      return ny_gencall_const_bool(cg, false, "ct_is_str");
    return 0;
  }

  if (want_is_float_obj) {
    if (ny_gencall_type_is(type_name, "f32") ||
        ny_gencall_type_is(type_name, "f64") ||
        ny_gencall_type_is(type_name, "f128"))
      return ny_gencall_const_bool(cg, true, "ct_is_float");
    if (ny_gencall_type_is_known_non_obj(type_name) ||
        ny_gencall_type_is_known_obj(type_name))
      return ny_gencall_const_bool(cg, false, "ct_is_float");
    return 0;
  }

  if (want_is_ptr) {
    if (ny_gencall_type_is_nullable(type_name))
      return 0;
    if (ny_gencall_type_is(type_name, "ptr") ||
        ny_gencall_type_is(type_name, "f32") ||
        ny_gencall_type_is(type_name, "f64") ||
        ny_gencall_type_is(type_name, "f128") ||
        ny_gencall_type_is_known_obj(type_name))
      return ny_gencall_const_bool(cg, true, "ct_is_ptr");
    if (!type_is_integer_number && ny_gencall_type_is_known_non_obj(type_name))
      return ny_gencall_const_bool(cg, false, "ct_is_ptr");
    return 0;
  }

  if (want_is_ny_obj) {
    if (ny_gencall_type_is_known_obj(type_name))
      return ny_gencall_const_bool(cg, true, "ct_is_obj");
    if (!type_is_integer_number && ny_gencall_type_is_known_non_obj(type_name))
      return ny_gencall_const_bool(cg, false, "ct_is_obj");
    return 0;
  }

  if (want_is_int) {
    if (ny_expr_literal_i64(arg, NULL))
      return ny_gencall_const_bool(cg, true, "ct_is_int");
    if ((!type_is_integer_number &&
         ny_gencall_type_is_known_non_obj(type_name)) ||
        ny_gencall_type_is_known_obj(type_name))
      return ny_gencall_const_bool(cg, false, "ct_is_int");
    return 0;
  }

  if (want_is_bool) {
    if (ny_gencall_type_is(type_name, "bool"))
      return ny_gencall_const_bool(cg, true, "ct_is_bool");
    if (!ny_gencall_type_is_nullable(type_name) &&
        (ny_gencall_type_is_known_non_obj(type_name) ||
         ny_gencall_type_is_known_obj(type_name)))
      return ny_gencall_const_bool(cg, false, "ct_is_bool");
    return 0;
  }

  if (want_is_nil) {
    if (ny_gencall_type_is(type_name, "nil") ||
        ny_gencall_type_is(type_name, "none"))
      return ny_gencall_const_bool(cg, true, "ct_is_nil");
    if (!ny_gencall_type_is_nullable(type_name) &&
        (ny_gencall_type_is_known_non_obj(type_name) ||
         ny_gencall_type_is_known_obj(type_name)))
      return ny_gencall_const_bool(cg, false, "ct_is_nil");
    return 0;
  }

  if (want_named_tag) {
    int got_tag = ny_gencall_known_obj_tag(type_name);
    int want_tag = ny_gencall_known_obj_tag(want_named_tag);
    if (got_tag >= 0 && want_tag >= 0)
      return ny_gencall_const_bool(cg, got_tag == want_tag, "ct_is_tag");
    bool integer_may_be_bigint = type_is_integer_number && want_tag == 130;
    if ((!integer_may_be_bigint &&
         ny_gencall_type_is_known_non_obj(type_name)) ||
        ny_gencall_type_is(type_name, "str"))
      return ny_gencall_const_bool(cg, false, "ct_is_tag");
    return 0;
  }

  if (want_has_tag) {
    int64_t want_tag = 0;
    if (!ny_expr_literal_i64(c->args.data[1].val, &want_tag))
      return 0;
    if (ny_gencall_type_is(type_name, "f32") ||
        ny_gencall_type_is(type_name, "f64") ||
        ny_gencall_type_is(type_name, "f128"))
      return ny_gencall_const_bool(cg, want_tag == 110, "ct_has_tag_float");
    int got_tag = ny_gencall_known_obj_tag(type_name);
    if (got_tag >= 0)
      return ny_gencall_const_bool(cg, got_tag == want_tag, "ct_has_tag");
    if (!(type_is_integer_number && want_tag == 130) &&
        ny_gencall_type_is_known_non_obj(type_name))
      return ny_gencall_const_bool(cg, false, "ct_has_tag");
    if (ny_gencall_type_is(type_name, "str") && want_tag != 120 &&
        want_tag != 121)
      return ny_gencall_const_bool(cg, false, "ct_has_tag");
  }

  if (want_tagof) {
    int got_tag = ny_gencall_known_tagof(type_name);
    if (got_tag >= 0)
      return LLVMConstInt(cg->type_i64, ((uint64_t)got_tag << 1) | 1u, false);
  }

  return 0;
}

static LLVMValueRef ny_try_fast_numeric_builtin(codegen_t *cg, scope *scopes,
                                                size_t depth, expr_t *call_expr,
                                                expr_t *callee,
                                                ny_call_arg_list *args) {
  if (!cg || !call_expr || !callee || !args || callee->kind != NY_E_IDENT ||
      args->len != 1)
    return 0;
  const char *name = callee->as.ident.name;
  if (!ny_is_unshadowed_builtin_callee(cg, scopes, depth, callee, &name))
    return 0;
  if (!name)
    return 0;

  expr_t *arg_expr = args->data[0].val;
  if (!arg_expr)
    return 0;

  if (strcmp(name, "float") == 0 || strcmp(name, "to_float") == 0 ||
      strcmp(name, "f64") == 0) {
    fun_sig *box = ny_gencall_flt_box(cg);
    if (!box)
      return 0;

    LLVMValueRef dbl = 0;
    if (ny_is_proven_int(cg, scopes, depth, arg_expr, 0)) {
      LLVMValueRef tagged = gen_expr(cg, scopes, depth, arg_expr);
      if (!tagged)
        return 0;
      LLVMValueRef raw = ny_untag_int(cg, tagged);
      dbl = LLVMBuildSIToFP(cg->builder, raw, cg->type_f64,
                            NY_LLVM_NAME(cg, "fast_i2f"));
    } else if (arg_expr->kind == NY_E_LITERAL &&
               arg_expr->as.literal.kind == NY_LIT_FLOAT) {
      dbl = LLVMConstReal(cg->type_f64, arg_expr->as.literal.as.f);
    } else {
      bool direct_float = false;
      if (arg_expr->kind == NY_E_IDENT) {
        size_t name_len = (size_t)arg_expr->tok.len;
        if (name_len == 0)
          name_len = strlen(arg_expr->as.ident.name);
        binding *b = ny_gencall_lookup_binding(
            cg, scopes, depth, arg_expr->as.ident.name, name_len,
            arg_expr->as.ident.hash);
        direct_float = b && (b->is_f64_slot || b->is_f64_direct ||
                             b->is_f32_slot || b->is_f32_direct);
      }
      if (direct_float) {
        dbl = gen_expr_as_f64(cg, scopes, depth, arg_expr);
      } else {
        LLVMValueRef v = gen_expr(cg, scopes, depth, arg_expr);
        if (!v)
          return 0;
        if (LLVMTypeOf(v) != cg->type_i64)
          v = ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, "fast_float_arg"));
        dbl = ny_unbox_float(cg, v);
      }
    }

    LLVMValueRef bits =
        ny_bitcast(cg, dbl, cg->type_i64, NY_LLVM_NAME(cg, "fast_fbits"));
    ny_dbg_loc(cg, call_expr->tok);
    return LLVMBuildCall2(cg->builder, box->type, box->value, &bits, 1,
                          NY_LLVM_NAME(cg, "fast_float"));
  }

  if (strcmp(name, "int") == 0 || strcmp(name, "to_int") == 0) {
    bool want_raw = (strcmp(name, "to_int") == 0);
    if (ny_is_proven_int(cg, scopes, depth, arg_expr, 0)) {
      LLVMValueRef iv = gen_expr(cg, scopes, depth, arg_expr);
      if (ny_proven_int_cast_fast_enabled(cg)) {
        LLVMValueRef raw = abi_untag_proven_int_fast(cg, iv);
        return want_raw ? raw : ny_tag_int(cg, raw);
      }
      return want_raw ? ny_untag_int(cg, iv) : iv;
    }

    bool can_from_float = false;
    if (arg_expr->kind == NY_E_LITERAL &&
        arg_expr->as.literal.kind == NY_LIT_FLOAT) {
      can_from_float = true;
    } else if (arg_expr->kind == NY_E_IDENT) {
      size_t name_len = (size_t)arg_expr->tok.len;
      if (name_len == 0)
        name_len = strlen(arg_expr->as.ident.name);
      binding *b =
          ny_gencall_lookup_binding(cg, scopes, depth, arg_expr->as.ident.name,
                                    name_len, arg_expr->as.ident.hash);
      can_from_float = (b && (b->is_f64_slot || b->is_f64_direct ||
                              b->is_f32_slot || b->is_f32_direct));
    }
    if (!can_from_float) {
      if (want_raw)
        return 0;

      LLVMValueRef v = gen_expr(cg, scopes, depth, arg_expr);
      fun_sig *f2i = lookup_fun(cg, "__flt_to_int", 0);
      if (!v || !f2i)
        return 0;
      if (LLVMTypeOf(v) != cg->type_i64)
        v = ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, "fast_int_arg"));

      LLVMValueRef is_int = ny_is_tagged_int(cg, v);
      fun_sig *b2i = lookup_fun(cg, "__bigint_to_int", 0);
      fun_sig *has_tag = lookup_fun(cg, "__has_tag", 0);
      LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
      LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
      LLVMBasicBlockRef int_bb = ny_bb_fn(fn, "fast_int.int");
      LLVMBasicBlockRef float_chk_bb = ny_bb_fn(fn, "fast_int.float_chk");
      LLVMBasicBlockRef float_bb = ny_bb_fn(fn, "fast_int.float");
      LLVMBasicBlockRef bigint_chk_bb = ny_bb_fn(fn, "fast_int.bigint_chk");
      LLVMBasicBlockRef bigint_bb = ny_bb_fn(fn, "fast_int.bigint");
      LLVMBasicBlockRef zero_bb = ny_bb_fn(fn, "fast_int.zero");
      LLVMBasicBlockRef done_bb = ny_bb_fn(fn, "fast_int.done");

      ny_cond_br(cg, is_int, int_bb, float_chk_bb);

      ny_pos(cg, int_bb);
      LLVMValueRef int_res = v;
      LLVMBasicBlockRef int_end_bb = ny_cur_block(cg);
      ny_br(cg, done_bb);

      ny_pos(cg, float_chk_bb);
      ny_cond_br(cg, ny_is_float(cg, v), float_bb, bigint_chk_bb);

      ny_pos(cg, float_bb);
      ny_dbg_loc(cg, call_expr->tok);
      LLVMValueRef float_res =
          LLVMBuildCall2(cg->builder, f2i->type, f2i->value, &v, 1,
                         NY_LLVM_NAME(cg, "fast_int"));
      LLVMBasicBlockRef float_end_bb = ny_cur_block(cg);
      ny_br(cg, done_bb);

      ny_pos(cg, bigint_chk_bb);
      LLVMValueRef is_bigint = LLVMConstInt(cg->type_i1, 0, false);
      if (b2i && has_tag) {
        LLVMValueRef bigint_tag =
            ny_tag_int(cg, LLVMConstInt(cg->type_i64, 130, false));
        LLVMValueRef has_args[2] = {v, bigint_tag};
        LLVMValueRef tagged_has_bigint =
            LLVMBuildCall2(cg->builder, has_tag->type, has_tag->value, has_args,
                           2, NY_LLVM_NAME(cg, "fast_int_is_bigint"));
        is_bigint = ny_eq(cg, tagged_has_bigint, ny_ctrue(cg),
                          NY_LLVM_NAME(cg, "fast_int_bigint_pred"));
      }
      ny_cond_br(cg, is_bigint, bigint_bb, zero_bb);

      ny_pos(cg, bigint_bb);
      ny_dbg_loc(cg, call_expr->tok);
      LLVMValueRef bigint_res =
          LLVMBuildCall2(cg->builder, b2i->type, b2i->value, &v, 1,
                         NY_LLVM_NAME(cg, "fast_bigint_to_int"));
      LLVMBasicBlockRef bigint_end_bb = ny_cur_block(cg);
      ny_br(cg, done_bb);

      ny_pos(cg, zero_bb);
      LLVMValueRef zero_res = ny_tag_int(cg, ny_c0(cg));
      LLVMBasicBlockRef zero_end_bb = ny_cur_block(cg);
      ny_br(cg, done_bb);

      ny_pos(cg, done_bb);
      LLVMValueRef phi =
          ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_int_phi"));
      LLVMValueRef incoming_vals[4] = {int_res, float_res, bigint_res,
                                       zero_res};
      LLVMBasicBlockRef incoming_bbs[4] = {int_end_bb, float_end_bb,
                                           bigint_end_bb, zero_end_bb};
      LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 4);
      return phi;
    }

    LLVMValueRef dbl = gen_expr_as_f64(cg, scopes, depth, arg_expr);
    LLVMValueRef as_i64 = LLVMBuildFPToSI(cg->builder, dbl, cg->type_i64,
                                          NY_LLVM_NAME(cg, "fast_f2i"));
    return want_raw ? as_i64 : ny_tag_int(cg, as_i64);
  }

  return 0;
}

typedef enum {
  NY_GENCALL_INT_BIN_NONE = 0,
  NY_GENCALL_INT_BIN_ADD,
  NY_GENCALL_INT_BIN_SUB,
  NY_GENCALL_INT_BIN_MUL,
  NY_GENCALL_INT_BIN_AND,
  NY_GENCALL_INT_BIN_OR,
  NY_GENCALL_INT_BIN_XOR,
  NY_GENCALL_INT_BIN_SHL,
  NY_GENCALL_INT_BIN_SHR,
  NY_GENCALL_INT_BIN_EQ,
  NY_GENCALL_INT_BIN_NE,
  NY_GENCALL_INT_BIN_LT,
  NY_GENCALL_INT_BIN_LE,
  NY_GENCALL_INT_BIN_GT,
  NY_GENCALL_INT_BIN_GE,
} ny_gencall_int_bin_kind_t;

static ny_gencall_int_bin_kind_t ny_gencall_int_bin_kind(const char *name,
                                                         bool shadowed) {
  if (!name)
    return NY_GENCALL_INT_BIN_NONE;
  if (strcmp(name, "__add") == 0 ||
      ny_gencall_builtin_name_is(name, "add", shadowed))
    return NY_GENCALL_INT_BIN_ADD;
  if (strcmp(name, "__sub") == 0 ||
      ny_gencall_builtin_name_is(name, "sub", shadowed))
    return NY_GENCALL_INT_BIN_SUB;
  if (strcmp(name, "__mul") == 0 ||
      ny_gencall_builtin_name_is(name, "mul", shadowed))
    return NY_GENCALL_INT_BIN_MUL;
  if (ny_gencall_builtin_name_is(name, "band", shadowed))
    return NY_GENCALL_INT_BIN_AND;
  if (ny_gencall_builtin_name_is(name, "bor", shadowed))
    return NY_GENCALL_INT_BIN_OR;
  if (ny_gencall_builtin_name_is(name, "bxor", shadowed))
    return NY_GENCALL_INT_BIN_XOR;
  if (ny_gencall_builtin_name_is(name, "bshl", shadowed))
    return NY_GENCALL_INT_BIN_SHL;
  if (ny_gencall_builtin_name_is(name, "bshr", shadowed))
    return NY_GENCALL_INT_BIN_SHR;
  if (strcmp(name, "__eq") == 0 ||
      ny_gencall_builtin_name_is(name, "eq", shadowed))
    return NY_GENCALL_INT_BIN_EQ;
  if (strcmp(name, "__ne") == 0 ||
      ny_gencall_builtin_name_is(name, "ne", shadowed))
    return NY_GENCALL_INT_BIN_NE;
  if (strcmp(name, "__lt") == 0 ||
      ny_gencall_builtin_name_is(name, "lt", shadowed))
    return NY_GENCALL_INT_BIN_LT;
  if (strcmp(name, "__le") == 0 ||
      ny_gencall_builtin_name_is(name, "le", shadowed))
    return NY_GENCALL_INT_BIN_LE;
  if (strcmp(name, "__gt") == 0 ||
      ny_gencall_builtin_name_is(name, "gt", shadowed))
    return NY_GENCALL_INT_BIN_GT;
  if (strcmp(name, "__ge") == 0 ||
      ny_gencall_builtin_name_is(name, "ge", shadowed))
    return NY_GENCALL_INT_BIN_GE;
  return NY_GENCALL_INT_BIN_NONE;
}

static LLVMValueRef ny_gencall_tag_bool(codegen_t *cg, LLVMValueRef pred,
                                        const char *name) {
  return ny_select(cg, pred, ny_ctrue(cg), ny_cfalse(cg),
                   name ? name : "fast_bool");
}

static LLVMValueRef
ny_try_fast_int_binary_builtin(codegen_t *cg, scope *scopes, size_t depth,
                               const char *name, bool shadowed, expr_call_t *c,
                               token_t tok) {
  if (!cg || !name || !c || c->args.len != 2)
    return 0;

  ny_gencall_int_bin_kind_t kind = ny_gencall_int_bin_kind(name, shadowed);
  if (kind == NY_GENCALL_INT_BIN_NONE)
    return 0;

  expr_t *left = c->args.data[0].val;
  expr_t *right = c->args.data[1].val;
  if (!ny_is_proven_int(cg, scopes, depth, left, 0) ||
      !ny_is_proven_int(cg, scopes, depth, right, 0))
    return 0;

  LLVMValueRef l = gen_expr(cg, scopes, depth, left);
  LLVMValueRef r = gen_expr(cg, scopes, depth, right);
  if (!l || !r) {
    ny_diag_error(tok, "failed to evaluate fast integer builtin operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  l = ny_cast_to_i64(cg, l, "fast_int_bin_l");
  r = ny_cast_to_i64(cg, r, "fast_int_bin_r");
  ny_dbg_loc(cg, tok);

  if (kind == NY_GENCALL_INT_BIN_ADD)
    return ny_sub(cg, ny_add(cg, l, r, "fast_call_add_sum"), ny_c1(cg),
                  "fast_call_add");
  if (kind == NY_GENCALL_INT_BIN_SUB)
    return ny_add(cg, ny_sub(cg, l, r, "fast_call_sub_diff"), ny_c1(cg),
                  "fast_call_sub");
  if (kind == NY_GENCALL_INT_BIN_MUL) {
    LLVMValueRef raw = LLVMBuildMul(cg->builder, ny_untag_int(cg, l),
                                    ny_untag_int(cg, r), "fast_call_mul_raw");
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_GENCALL_INT_BIN_AND) {
    LLVMValueRef raw = LLVMBuildAnd(cg->builder, ny_untag_int(cg, l),
                                    ny_untag_int(cg, r), "fast_call_and_raw");
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_GENCALL_INT_BIN_OR) {
    LLVMValueRef raw = LLVMBuildOr(cg->builder, ny_untag_int(cg, l),
                                   ny_untag_int(cg, r), "fast_call_or_raw");
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_GENCALL_INT_BIN_XOR) {
    LLVMValueRef raw = LLVMBuildXor(cg->builder, ny_untag_int(cg, l),
                                    ny_untag_int(cg, r), "fast_call_xor_raw");
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_GENCALL_INT_BIN_SHL) {
    LLVMValueRef raw = LLVMBuildShl(cg->builder, ny_untag_int(cg, l),
                                    ny_untag_int(cg, r), "fast_call_shl_raw");
    return ny_tag_int(cg, raw);
  }
  if (kind == NY_GENCALL_INT_BIN_SHR) {
    LLVMValueRef raw = LLVMBuildLShr(cg->builder, ny_untag_int(cg, l),
                                     ny_untag_int(cg, r), "fast_call_shr_raw");
    return ny_tag_int(cg, raw);
  }

  LLVMValueRef pred = 0;
  if (kind == NY_GENCALL_INT_BIN_EQ)
    pred = ny_eq(cg, l, r, "fast_call_eq");
  else if (kind == NY_GENCALL_INT_BIN_NE)
    pred = ny_ne(cg, l, r, "fast_call_ne");
  else {
    LLVMValueRef li = ny_untag_int(cg, l);
    LLVMValueRef ri = ny_untag_int(cg, r);
    if (kind == NY_GENCALL_INT_BIN_LT)
      pred = ny_slt(cg, li, ri, "fast_call_lt");
    else if (kind == NY_GENCALL_INT_BIN_LE)
      pred = ny_sle(cg, li, ri, "fast_call_le");
    else if (kind == NY_GENCALL_INT_BIN_GT)
      pred = ny_sgt(cg, li, ri, "fast_call_gt");
    else if (kind == NY_GENCALL_INT_BIN_GE)
      pred = ny_sge(cg, li, ri, "fast_call_ge");
  }

  return pred ? ny_gencall_tag_bool(cg, pred, "fast_call_cmp") : 0;
}

static bool ny_gencall_expr_is_raw_ptr_typed(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e);
static int64_t ny_gencall_raw_elem_bytes(LLVMTypeRef elem_ty);
static bool ny_gencall_check_safe_raw_access(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *ptr_expr,
                                             expr_t *idx_expr,
                                             int64_t elem_bytes, token_t tok,
                                             const char *diag_name);

static LLVMValueRef
ny_gencall_load_idx_intrinsic(codegen_t *cg, expr_t *e, scope *scopes,
                              size_t depth, expr_call_t *c,
                              const char *diag_name, LLVMTypeRef elem_ty,
                              unsigned align, bool tag_result) {
  expr_t *idx_expr = (c->args.len >= 2) ? c->args.data[1].val : NULL;
  if (!ny_gencall_check_safe_raw_access(
          cg, scopes, depth, c->args.data[0].val, idx_expr,
          ny_gencall_raw_elem_bytes(elem_ty), e->tok, diag_name))
    return ny_c0(cg);
  LLVMValueRef addr_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  LLVMValueRef idx_v =
      idx_expr ? gen_expr(cg, scopes, depth, idx_expr) : ny_c0(cg);
  if (!addr_v || !idx_v) {
    ny_diag_error(e->tok, "failed to evaluate arguments for %s", diag_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  addr_v = ny_cast_to_i64(cg, addr_v, "ldx_addr");
  if (!ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, c->args.data[0].val))
    addr_v = ny_build_rt_untag_i64(cg, addr_v, "ldx_addr_raw");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef idx_raw = ny_gencall_index_raw_i64(cg, scopes, depth, idx_expr,
                                                  idx_v, "ldx_idx_raw");
  LLVMValueRef base_ptr =
      LLVMBuildIntToPtr(cg->builder, addr_v, cg->type_i8ptr, "ldx_base_p");
  LLVMValueRef byte_ptr = LLVMBuildGEP2(cg->builder, cg->type_i8, base_ptr,
                                        &idx_raw, 1, "ldx_byte_p");
  LLVMValueRef ptr = LLVMBuildPointerCast(cg->builder, byte_ptr,
                                          LLVMPointerType(elem_ty, 0), "ldx_p");
  LLVMValueRef load = LLVMBuildLoad2(cg->builder, elem_ty, ptr, "ldx_val");
  LLVMSetAlignment(load, align);
  if (!tag_result) {
    if (LLVMTypeOf(load) == cg->type_i64)
      return load;
    return LLVMBuildSExt(cg->builder, load, cg->type_i64, "ldx_sext");
  }
  LLVMValueRef ext = load;
  if (LLVMTypeOf(ext) != cg->type_i64)
    ext = LLVMBuildZExt(cg->builder, load, cg->type_i64, "ldx_ext");
  LLVMValueRef shl = LLVMBuildShl(cg->builder, ext, ny_c1(cg), "ldx_shl");
  return LLVMBuildOr(cg->builder, shl, ny_c1(cg), "ldx_tag");
}

static bool ny_gencall_is_std_tbuf_call_name(const char *name, bool shadowed,
                                             const char *leaf) {
  if (!name || !leaf)
    return false;
  if (!shadowed && strcmp(name, leaf) == 0)
    return true;
  const char *prefix = "std.core.tbuf.";
  size_t prefix_len = strlen(prefix);
  return strncmp(name, prefix, prefix_len) == 0 &&
         strcmp(name + prefix_len, leaf) == 0;
}

static bool ny_gencall_sig_is_std_tbuf_leaf(fun_sig *sig, const char *leaf) {
  if (!sig || !sig->name || !leaf)
    return false;
  const char *prefix = "std.core.tbuf.";
  size_t prefix_len = strlen(prefix);
  return strncmp(sig->name, prefix, prefix_len) == 0 &&
         strcmp(sig->name + prefix_len, leaf) == 0;
}

static bool ny_gencall_expr_is_raw_ptr_typed(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e) {
  if (!cg)
    return false;
  const char *type_name = infer_expr_type(cg, scopes, depth, e);
  if (!type_name || ny_gencall_type_is_nullable(type_name))
    return false;
  return ny_gencall_type_is(type_name, "ptr") || type_name[0] == '*';
}

static int64_t ny_gencall_raw_elem_bytes(LLVMTypeRef elem_ty) {
  if (!elem_ty)
    return 1;
  LLVMTypeKind kind = LLVMGetTypeKind(elem_ty);
  if (kind == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(elem_ty);
    return bits <= 8 ? 1 : (int64_t)((bits + 7) / 8);
  }
  if (kind == LLVMFloatTypeKind)
    return 4;
  if (kind == LLVMDoubleTypeKind)
    return 8;
  return 1;
}

static binding *ny_gencall_ident_binding(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return NULL;
  size_t name_len = (size_t)e->tok.len;
  if (name_len == 0)
    name_len = strlen(e->as.ident.name);
  return ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                   name_len, e->as.ident.hash);
}

static bool ny_gencall_raw_alloc_size_from_init(codegen_t *cg, scope *scopes,
                                                size_t depth, binding *b,
                                                expr_t *ptr_expr,
                                                int64_t *out_size) {
  if (!b || !out_size)
    return false;
  const char *name = b->name;
  if (ptr_expr && ptr_expr->kind == NY_E_IDENT && ptr_expr->as.ident.name)
    name = ptr_expr->as.ident.name;
  if (!name || !*name)
    return false;

  expr_t *init = ny_binding_var_init_expr(b, name);
  if (!init) {
    const char *tail = strrchr(name, '.');
    if (tail && tail[1])
      init = ny_binding_var_init_expr(b, tail + 1);
  }
  if (!init || init->kind != NY_E_CALL || !init->as.call.callee ||
      init->as.call.callee->kind != NY_E_IDENT ||
      !init->as.call.callee->as.ident.name)
    return false;

  const char *callee = init->as.call.callee->as.ident.name;
  const char *leaf = strrchr(callee, '.');
  leaf = leaf ? leaf + 1 : callee;
  size_t arg_idx = SIZE_MAX;
  if ((strcmp(leaf, "malloc") == 0 || strcmp(leaf, "zalloc") == 0) &&
      init->as.call.args.len >= 1) {
    arg_idx = 0;
  } else if (strcmp(leaf, "realloc") == 0 && init->as.call.args.len >= 2) {
    arg_idx = 1;
  }
  if (arg_idx == SIZE_MAX)
    return false;

  int64_t lo = 0, hi = 0;
  if (!ny_gencall_expr_int_range(cg, scopes, depth,
                                 init->as.call.args.data[arg_idx].val, &lo,
                                 &hi) ||
      lo != hi || lo < 0)
    return false;
  *out_size = lo;
  return true;
}

static bool ny_gencall_safe_raw_checks_enabled(codegen_t *cg) {
  return cg && (cg->strict_diagnostics || cg->ownership_strict);
}

static bool ny_gencall_check_safe_raw_access(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *ptr_expr,
                                             expr_t *idx_expr,
                                             int64_t elem_bytes, token_t tok,
                                             const char *diag_name) {
  if (!ny_gencall_safe_raw_checks_enabled(cg) || ny_is_stdlib_tok(tok))
    return true;
  binding *b = ny_gencall_ident_binding(cg, scopes, depth, ptr_expr);
  int64_t alloc_size = 0;
  bool has_alloc_size =
      b && b->ownership_tracked && b->ownership_raw_ptr &&
      b->ownership_alloc_size_known;
  if (has_alloc_size)
    alloc_size = b->ownership_alloc_size_raw;
  else if (b && ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, ptr_expr))
    has_alloc_size =
        ny_gencall_raw_alloc_size_from_init(cg, scopes, depth, b, ptr_expr,
                                            &alloc_size);
  else if (!b || !ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, ptr_expr))
    return true;

  if (!has_alloc_size) {
    ny_diag_error(tok,
                  "safe-mode raw memory access requires a known allocation size");
    ny_diag_hint("allocate with a compile-time-proven size, pass the buffer "
                 "through a typed wrapper, or leave safe mode for unchecked "
                 "raw memory work");
    cg->had_error = 1;
    return false;
  }
  int64_t idx_min = 0, idx_max = 0;
  bool has_idx = true;
  if (idx_expr)
    has_idx =
        ny_gencall_expr_int_range(cg, scopes, depth, idx_expr, &idx_min, &idx_max);
  if (!has_idx) {
    ny_diag_error(idx_expr ? idx_expr->tok : tok,
                  "safe-mode raw memory access requires a proven byte range for index");
    ny_diag_hint("use a literal, a range-proven binding, or "
                 "assert_compile_range(index, 0, size - width, \"...\") before "
                 "the raw memory access");
    cg->had_error = 1;
    return false;
  }
  int64_t max_start = alloc_size - elem_bytes;
  if (idx_min < 0 || idx_max > max_start) {
    ny_diag_error(idx_expr ? idx_expr->tok : tok,
                  "safe-mode raw memory access out of bounds");
    ny_diag_hint("%s proved index range [%lld, %lld], allocation size %lld "
                 "bytes, access width %lld bytes",
                 diag_name ? diag_name : "raw access", (long long)idx_min,
                 (long long)idx_max, (long long)alloc_size, (long long)elem_bytes);
    cg->had_error = 1;
    return false;
  }
  return true;
}

static bool ny_gencall_precheck_safe_raw_memory_api(
    codegen_t *cg, expr_t *e, scope *scopes, size_t depth, const char *name,
    bool shadowed, expr_call_t *c) {
  if (!ny_gencall_safe_raw_checks_enabled(cg) || !e || !c || !name ||
      ny_is_stdlib_tok(e->tok) || c->args.len == 0)
    return true;

  struct raw_call_shape {
    const char *api;
    const char *intrinsic;
    int64_t width;
    bool store;
  };
  static const struct raw_call_shape shapes[] = {
      {"load8", "__load8_idx", 1, false},
      {"load16", "__load16_idx", 2, false},
      {"load32", "__load32_idx", 4, false},
      {"load64", "__load64_idx", 8, false},
      {"store8", "__store8_idx", 1, true},
      {"store16", "__store16_idx", 2, true},
      {"store32", "__store32_idx", 4, true},
      {"store64", "__store64_idx", 8, true},
  };

  for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
    const struct raw_call_shape *s = &shapes[i];
    bool public_api = ny_gencall_builtin_name_is(name, s->api, shadowed);
    bool intrinsic = strcmp(name, s->intrinsic) == 0;
    if (!public_api && !intrinsic)
      continue;

    expr_t *idx_expr = NULL;
    bool arity_ok = false;
    if (s->store) {
      if (public_api && (c->args.len == 2 || c->args.len == 3)) {
        idx_expr = c->args.len == 3 ? c->args.data[2].val : NULL;
        arity_ok = true;
      } else if (intrinsic && c->args.len == 3) {
        idx_expr = c->args.data[1].val;
        arity_ok = true;
      }
    } else {
      if ((public_api || intrinsic) && (c->args.len == 1 || c->args.len == 2)) {
        idx_expr = c->args.len == 2 ? c->args.data[1].val : NULL;
        arity_ok = true;
      }
    }
    if (!arity_ok)
      return true;

    const char *diag = s->intrinsic + 2;
    return ny_gencall_check_safe_raw_access(
        cg, scopes, depth, c->args.data[0].val, idx_expr, s->width, e->tok,
        diag);
  }
  return true;
}

static LLVMValueRef ny_emit_f64buf_load_raw(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_call_t *c,
                                            token_t tok) {
  if (!cg || !c || c->args.len != 2)
    return NULL;
  LLVMValueRef buf_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  LLVMValueRef idx_v = gen_expr(cg, scopes, depth, c->args.data[1].val);
  if (!buf_v || !idx_v) {
    ny_diag_error(tok, "failed to evaluate f64buf_load(...) arguments");
    cg->had_error = 1;
    return LLVMConstReal(cg->type_f64, 0.0);
  }
  buf_v = ny_cast_to_i64(cg, buf_v, "f64buf_addr");
  if (!ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, c->args.data[0].val))
    buf_v = ny_build_rt_untag_i64(cg, buf_v, "f64buf_addr_raw");
  idx_v = ny_cast_to_i64(cg, idx_v, "f64buf_idx");
  LLVMValueRef idx_raw = ny_gencall_index_raw_i64(
      cg, scopes, depth, c->args.data[1].val, idx_v, "f64buf_idx_raw");
  LLVMValueRef byte_off =
      LLVMBuildShl(cg->builder, idx_raw, LLVMConstInt(cg->type_i64, 3, false),
                   "f64buf_byte_off");
  LLVMValueRef base_ptr =
      LLVMBuildIntToPtr(cg->builder, buf_v, cg->type_i8ptr, "f64buf_base_p");
  LLVMValueRef byte_ptr = LLVMBuildGEP2(cg->builder, cg->type_i8, base_ptr,
                                        &byte_off, 1, "f64buf_byte_p");
  LLVMValueRef ptr = LLVMBuildPointerCast(
      cg->builder, byte_ptr, LLVMPointerType(cg->type_f64, 0), "f64buf_p");
  LLVMValueRef load =
      LLVMBuildLoad2(cg->builder, cg->type_f64, ptr, "f64buf_load_raw");
  LLVMSetAlignment(load, 8);
  return load;
}

LLVMValueRef ny_try_fast_f64buf_load_as_f64(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return NULL;
  const char *fn_name = e->as.call.callee->as.ident.name;
  if (!fn_name || e->as.call.args.len != 2)
    return NULL;
  fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
  if (!ny_gencall_sig_is_std_tbuf_leaf(sig, "f64buf_load"))
    return NULL;
  return ny_emit_f64buf_load_raw(cg, scopes, depth, &e->as.call, e->tok);
}

static LLVMValueRef ny_gencall_store_idx_intrinsic(
    codegen_t *cg, expr_t *e, scope *scopes, size_t depth, expr_call_t *c,
    const char *diag_name, LLVMTypeRef elem_ty, unsigned align,
    bool untag_before_store, bool value_before_index) {
  size_t val_arg = value_before_index ? 1u : 2u;
  size_t idx_arg = value_before_index ? 2u : 1u;
  bool has_idx_arg = value_before_index ? c->args.len >= 3 : c->args.len >= 2;
  expr_t *idx_expr = has_idx_arg ? c->args.data[idx_arg].val : NULL;
  if (!ny_gencall_check_safe_raw_access(
          cg, scopes, depth, c->args.data[0].val, idx_expr,
          ny_gencall_raw_elem_bytes(elem_ty), e->tok, diag_name))
    return ny_c0(cg);
  LLVMValueRef addr_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  LLVMValueRef idx_v =
      idx_expr ? gen_expr(cg, scopes, depth, idx_expr) : ny_c0(cg);
  LLVMValueRef val_v = gen_expr(cg, scopes, depth, c->args.data[val_arg].val);
  if (!addr_v || !idx_v || !val_v) {
    ny_diag_error(e->tok, "failed to evaluate arguments for %s", diag_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  addr_v = ny_cast_to_i64(cg, addr_v, "stx_addr");
  if (!ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, c->args.data[0].val))
    addr_v = ny_build_rt_untag_i64(cg, addr_v, "stx_addr_raw");
  val_v = ny_cast_to_i64(cg, val_v, "stx_val");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef idx_raw = ny_gencall_index_raw_i64(cg, scopes, depth, idx_expr,
                                                  idx_v, "stx_idx_raw");
  LLVMValueRef raw_v =
      untag_before_store
          ? ny_build_untagged_or_raw_i64(cg, val_v, "stx_val_raw")
          : val_v;
  LLVMValueRef base_ptr =
      LLVMBuildIntToPtr(cg->builder, addr_v, cg->type_i8ptr, "stx_base_p");
  LLVMValueRef byte_ptr = LLVMBuildGEP2(cg->builder, cg->type_i8, base_ptr,
                                        &idx_raw, 1, "stx_byte_p");
  LLVMValueRef ptr = LLVMBuildPointerCast(cg->builder, byte_ptr,
                                          LLVMPointerType(elem_ty, 0), "stx_p");
  LLVMValueRef cast_v = raw_v;
  if (LLVMTypeOf(cast_v) != elem_ty)
    cast_v = LLVMBuildTrunc(cg->builder, cast_v, elem_ty, "stx_trunc");
  LLVMValueRef st = LLVMBuildStore(cg->builder, cast_v, ptr);
  LLVMSetAlignment(st, align);
  return val_v;
}

static LLVMValueRef ny_try_fast_tbuf_builtin(codegen_t *cg, expr_t *e,
                                             scope *scopes, size_t depth,
                                             const char *name, bool shadowed,
                                             expr_call_t *c) {
  if (!cg || !e || !name || !c)
    return NULL;
  if (ny_gencall_is_std_tbuf_call_name(name, shadowed, "f64buf_load") &&
      c->args.len == 2) {
    LLVMValueRef raw = ny_emit_f64buf_load_raw(cg, scopes, depth, c, e->tok);
    if (!raw)
      return NULL;
    fun_sig *box = ny_gencall_flt_box(cg);
    if (!box)
      return NULL;
    LLVMValueRef bits = ny_bitcast(cg, raw, cg->type_i64, "f64buf_bits");
    return LLVMBuildCall2(cg->builder, box->type, box->value, &bits, 1,
                          "f64buf_box");
  }
  if (ny_gencall_is_std_tbuf_call_name(name, shadowed, "f64buf_store") &&
      c->args.len == 3) {
    LLVMValueRef buf_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
    LLVMValueRef idx_v = gen_expr(cg, scopes, depth, c->args.data[1].val);
    LLVMValueRef val_v =
        gen_expr_as_f64(cg, scopes, depth, c->args.data[2].val);
    if (!buf_v || !idx_v || !val_v) {
      ny_diag_error(e->tok, "failed to evaluate f64buf_store(...) arguments");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    buf_v = ny_cast_to_i64(cg, buf_v, "f64buf_store_addr");
    if (!ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth,
                                          c->args.data[0].val))
      buf_v = ny_build_rt_untag_i64(cg, buf_v, "f64buf_store_addr_raw");
    idx_v = ny_cast_to_i64(cg, idx_v, "f64buf_store_idx");
    LLVMValueRef idx_raw = ny_gencall_index_raw_i64(
        cg, scopes, depth, c->args.data[1].val, idx_v, "f64buf_store_idx_raw");
    LLVMValueRef byte_off =
        LLVMBuildShl(cg->builder, idx_raw, LLVMConstInt(cg->type_i64, 3, false),
                     "f64buf_store_byte_off");
    LLVMValueRef base_ptr = LLVMBuildIntToPtr(
        cg->builder, buf_v, cg->type_i8ptr, "f64buf_store_base_p");
    LLVMValueRef byte_ptr = LLVMBuildGEP2(cg->builder, cg->type_i8, base_ptr,
                                          &byte_off, 1, "f64buf_store_byte_p");
    LLVMValueRef ptr = LLVMBuildPointerCast(cg->builder, byte_ptr,
                                            LLVMPointerType(cg->type_f64, 0),
                                            "f64buf_store_p");
    LLVMValueRef st = LLVMBuildStore(cg->builder, val_v, ptr);
    LLVMSetAlignment(st, 8);
    return ny_c0(cg);
  }
  return NULL;
}

static LLVMValueRef ny_get_raw_i64_runtime_fn(codegen_t *cg, const char *name,
                                              unsigned argc);

static LLVMValueRef
ny_try_fast_handle_memory_callsite(codegen_t *cg, expr_t *e, scope *scopes,
                                   size_t depth, const char *name,
                                   bool shadowed, expr_call_t *c) {
  if (!cg || !e || !name || !c)
    return 0;

  bool exact_load32 = strcmp(name, "load32_h") == 0;
  bool exact_load64 =
      strcmp(name, "load64_h") == 0 || strcmp(name, "load64_i") == 0;
  bool exact_load32_builtin = strcmp(name, "__load32_h") == 0;
  bool exact_load64_builtin = strcmp(name, "__load64_h") == 0;
  bool exact_store32 = strcmp(name, "store32_h") == 0;
  bool exact_store64 =
      strcmp(name, "store64_h") == 0 || strcmp(name, "store64_i") == 0;
  bool want_load32 = exact_load32_builtin || (exact_load32 && !shadowed) ||
                     strcmp(name, "std.core.load32_h") == 0;
  bool want_load64 = exact_load64_builtin || (exact_load64 && !shadowed) ||
                     strcmp(name, "std.core.load64_h") == 0 ||
                     strcmp(name, "std.core.load64_i") == 0;
  bool want_store32 =
      (exact_store32 && !shadowed) || strcmp(name, "std.core.store32_h") == 0;
  bool want_store64 = (exact_store64 && !shadowed) ||
                      strcmp(name, "std.core.store64_h") == 0 ||
                      strcmp(name, "std.core.store64_i") == 0;
  bool want_load = want_load32 || want_load64;
  bool want_store = want_store32 || want_store64;
  if (!want_load && !want_store)
    return 0;
  bool is32 = want_load32 || want_store32;

  if ((want_load && !(c->args.len == 1 || c->args.len == 2)) ||
      (want_store && !(c->args.len == 2 || c->args.len == 3)))
    return 0;

  LLVMValueRef addr_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  LLVMValueRef idx_v = NULL;
  LLVMValueRef val_v = NULL;
  if (want_load) {
    idx_v = (c->args.len == 2)
                ? gen_expr(cg, scopes, depth, c->args.data[1].val)
                : ny_c0(cg);
  } else {
    val_v = gen_expr(cg, scopes, depth, c->args.data[1].val);
    idx_v = (c->args.len == 3)
                ? gen_expr(cg, scopes, depth, c->args.data[2].val)
                : ny_c0(cg);
  }
  if (!addr_v || !idx_v || (want_store && !val_v)) {
    ny_diag_error(e->tok, "failed to evaluate arguments for fast %s", name);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  addr_v = ny_cast_to_i64(cg, addr_v, "fast_h_addr");
  if (!ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, c->args.data[0].val))
    addr_v = ny_build_rt_untag_i64(cg, addr_v, "fast_h_addr_raw");
  expr_t *idx_expr = want_load
                         ? (c->args.len == 2 ? c->args.data[1].val : NULL)
                         : (c->args.len == 3 ? c->args.data[2].val : NULL);
  LLVMValueRef idx_raw = ny_gencall_index_raw_i64(cg, scopes, depth, idx_expr,
                                                  idx_v, "fast_h_idx_raw");
  LLVMValueRef ptr_i64 = ny_add(cg, addr_v, idx_raw, "fast_h_ptr");
  LLVMValueRef ptr_ok =
      ny_ugt(cg, ptr_i64,
             LLVMConstInt(cg->type_i64, (uint64_t)NY_VALUE_PTR_MIN_ADDR, false),
             "fast_h_ok");

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef do_bb =
      ny_bb_fn(fn, want_load ? (is32 ? "load32_h.fast" : "load64_h.fast")
                             : (is32 ? "store32_h.fast" : "store64_h.fast"));
  LLVMBasicBlockRef slow_bb =
      ny_bb_fn(fn, want_load ? (is32 ? "load32_h.slow" : "load64_h.slow")
                             : (is32 ? "store32_h.slow" : "store64_h.slow"));
  LLVMBasicBlockRef done_bb =
      ny_bb_fn(fn, want_load ? (is32 ? "load32_h.done" : "load64_h.done")
                             : (is32 ? "store32_h.done" : "store64_h.done"));
  ny_cond_br(cg, ptr_ok, do_bb, slow_bb);

  ny_pos(cg, do_bb);
  ny_dbg_loc(cg, e->tok);
  LLVMTypeRef elem_ty = is32 ? cg->type_i32 : cg->type_i64;
  LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, ptr_i64,
                                       LLVMPointerType(elem_ty, 0), "fast_h_p");
  LLVMValueRef fast_res = NULL;
  if (want_load) {
    LLVMValueRef raw =
        LLVMBuildLoad2(cg->builder, elem_ty, ptr,
                       is32 ? "fast_load32_h_raw" : "fast_load64_h_raw");
    LLVMSetAlignment(raw, 1);
    if (is32)
      raw = LLVMBuildZExt(cg->builder, raw, cg->type_i64, "fast_load32_h_ext");
    fast_res = ny_or(cg,
                     ny_shl(cg, raw, ny_c1(cg),
                            is32 ? "fast_load32_h_shl" : "fast_load64_h_shl"),
                     ny_c1(cg), is32 ? "fast_load32_h" : "fast_load64_h");
  } else {
    val_v = ny_cast_to_i64(cg, val_v, "fast_h_val");
    LLVMValueRef raw = ny_build_rt_untag_i64(cg, val_v, "fast_store64_h_raw");
    if (is32)
      raw = LLVMBuildTrunc(cg->builder, raw, cg->type_i32,
                           "fast_store32_h_trunc");
    LLVMValueRef st = LLVMBuildStore(cg->builder, raw, ptr);
    LLVMSetAlignment(st, 1);
    fast_res = val_v;
  }
  LLVMBasicBlockRef fast_end_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, slow_bb);
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef slow_res = NULL;
  if (want_load) {
    LLVMValueRef rt =
        ny_get_raw_i64_runtime_fn(cg, is32 ? "rt_load32_h" : "rt_load64_h", 2);
    if (!rt)
      return 0;
    LLVMTypeRef ft = LLVMGlobalGetValueType(rt);
    LLVMValueRef args[2] = {addr_v, idx_raw};
    slow_res = LLVMBuildCall2(cg->builder, ft, rt, args, 2,
                              is32 ? "slow_load32_h" : "slow_load64_h");
  } else if (is32) {
    LLVMValueRef rt = ny_get_raw_i64_runtime_fn(cg, "rt_store32_idx", 3);
    if (!rt)
      return 0;
    LLVMTypeRef ft = LLVMGlobalGetValueType(rt);
    LLVMValueRef raw_val =
        LLVMBuildTrunc(cg->builder,
                       ny_build_rt_untag_i64(
                           cg, ny_cast_to_i64(cg, val_v, "slow_store32_h_val"),
                           "slow_store32_h_raw"),
                       cg->type_i32, "slow_store32_h_trunc");
    LLVMValueRef raw_val64 =
        LLVMBuildZExt(cg->builder, raw_val, cg->type_i64, "slow_store32_h_ext");
    LLVMValueRef args[3] = {addr_v, idx_raw, raw_val64};
    (void)LLVMBuildCall2(cg->builder, ft, rt, args, 3, "slow_store32_h");
    slow_res = val_v;
  } else {
    LLVMValueRef rt = ny_get_raw_i64_runtime_fn(cg, "rt_store64_h", 3);
    if (!rt)
      return 0;
    LLVMTypeRef ft = LLVMGlobalGetValueType(rt);
    LLVMValueRef args[3] = {addr_v, idx_raw, val_v};
    slow_res = LLVMBuildCall2(cg->builder, ft, rt, args, 3, "slow_store64_h");
  }
  LLVMBasicBlockRef slow_end_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64,
             want_load ? "fast_load64_h_result" : "fast_store64_h_result");
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_res, slow_res},
                  (LLVMBasicBlockRef[]){fast_end_bb, slow_end_bb}, 2);
  return phi;
}

static LLVMValueRef ny_try_fast_scalar_rt_builtin(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e,
                                                  const char *name,
                                                  bool shadowed,
                                                  expr_call_t *c) {
  if (!cg || !e || !name || !c || c->args.len != 1)
    return 0;

  bool want_tag = (strcmp(name, "__tag") == 0) ||
                  ny_gencall_builtin_name_is(name, "from_int", shadowed);
  bool want_untag = (strcmp(name, "__untag") == 0);
  bool want_is_int = (strcmp(name, "__is_int") == 0) ||
                     ny_gencall_builtin_name_is(name, "is_int", shadowed);
  bool want_rt_is_ptr = (strcmp(name, "__is_ptr") == 0);
  bool want_std_is_ptr =
      !want_rt_is_ptr && ny_gencall_builtin_name_is(name, "is_ptr", shadowed);
  bool want_is_bool = ny_gencall_builtin_name_is(name, "is_bool", shadowed);
  bool want_is_nil = ny_gencall_builtin_name_is(name, "is_nil", shadowed) ||
                     ny_gencall_builtin_name_is(name, "is_none", shadowed);
  if (!want_tag && !want_untag && !want_is_int && !want_rt_is_ptr &&
      !want_std_is_ptr && !want_is_bool && !want_is_nil)
    return 0;

  LLVMValueRef arg_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  if (!arg_v) {
    ny_diag_error(e->tok, "failed to evaluate argument for %s", name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  arg_v = ny_cast_to_i64(cg, arg_v, "fast_scalar_arg");
  ny_dbg_loc(cg, e->tok);

  if (want_tag)
    return ny_or(cg, ny_shl(cg, arg_v, ny_c1(cg), "fast_tag_shl"), ny_c1(cg),
                 "fast_tag");
  if (want_untag)
    return ny_build_rt_untag_i64(cg, arg_v, "fast_untag");

  if (want_rt_is_ptr || want_std_is_ptr) {
    LLVMValueRef low_mask =
        want_std_is_ptr
            ? LLVMConstInt(cg->type_i64, NY_VALUE_PTR_TAG_MASK, false)
            : ny_c1(cg);
    LLVMValueRef low = ny_and(cg, arg_v, low_mask, "fast_is_ptr_low");
    LLVMValueRef low_ok = ny_eq(cg, low, ny_c0(cg), "fast_is_ptr_low_ok");
    LLVMValueRef gt_min = ny_ugt(
        cg, arg_v,
        LLVMConstInt(cg->type_i64, (uint64_t)NY_VALUE_PTR_MIN_ADDR, false),
        "fast_is_ptr_gt_min");
    return ny_gencall_tag_bool(
        cg, ny_and(cg, low_ok, gt_min, "fast_is_ptr_pred"), "fast_is_ptr");
  }

  if (want_is_bool) {
    LLVMValueRef is_true = ny_eq(cg, arg_v, ny_ctrue(cg), "fast_is_bool_true");
    LLVMValueRef is_false =
        ny_eq(cg, arg_v, ny_cfalse(cg), "fast_is_bool_false");
    return ny_gencall_tag_bool(
        cg, ny_or(cg, is_true, is_false, "fast_is_bool_pred"), "fast_is_bool");
  }

  if (want_is_nil) {
    LLVMValueRef is_nil = ny_eq(cg, arg_v, ny_c0(cg), "fast_is_nil_pred");
    return ny_gencall_tag_bool(cg, is_nil, "fast_is_nil");
  }

  LLVMValueRef lsb = ny_and(cg, arg_v, ny_c1(cg), "fast_is_int_lsb");
  LLVMValueRef pred = ny_eq(cg, lsb, ny_c1(cg), "fast_is_int_pred");
  return ny_gencall_tag_bool(cg, pred, "fast_is_int");
}

static LLVMValueRef
ny_gencall_copy_or_set_intrinsic(codegen_t *cg, expr_t *e, scope *scopes,
                                 size_t depth, expr_call_t *c, bool is_memset) {
  LLVMValueRef dst_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  LLVMValueRef mid_v = gen_expr(cg, scopes, depth, c->args.data[1].val);
  LLVMValueRef n_v = gen_expr(cg, scopes, depth, c->args.data[2].val);
  if (!dst_v || !mid_v || !n_v) {
    ny_diag_error(e->tok, "failed to evaluate arguments for %s",
                  is_memset ? "__memset" : "__copy_mem");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  dst_v = ny_cast_to_i64(cg, dst_v, "mem_dst");
  if (!ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, c->args.data[0].val))
    dst_v = ny_build_rt_untag_i64(cg, dst_v, "mem_dst_raw");
  mid_v = ny_cast_to_i64(cg, mid_v, is_memset ? "mem_val" : "mem_src");
  if (!is_memset &&
      !ny_gencall_expr_is_raw_ptr_typed(cg, scopes, depth, c->args.data[1].val))
    mid_v = ny_build_rt_untag_i64(cg, mid_v, "mem_src_raw");
  n_v = ny_cast_to_i64(cg, n_v, "mem_len");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef n_raw = ny_gencall_index_raw_i64(
      cg, scopes, depth, c->args.data[2].val, n_v, "mem_len_raw");
  LLVMValueRef has_bytes =
      LLVMBuildICmp(cg->builder, LLVMIntSGT, n_raw, ny_c0(cg), "mem_has_bytes");

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef do_bb = ny_bb_fn(fn, is_memset ? "memset.do" : "memcpy.do");
  LLVMBasicBlockRef done_bb =
      ny_bb_fn(fn, is_memset ? "memset.done" : "memcpy.done");
  ny_cond_br(cg, has_bytes, do_bb, done_bb);

  ny_pos(cg, do_bb);
  LLVMValueRef dst_p =
      LLVMBuildIntToPtr(cg->builder, dst_v, cg->type_i8ptr, "mem_dst_p");
  if (is_memset) {
    LLVMValueRef val_raw =
        ny_build_untagged_or_raw_i64(cg, mid_v, "memset_val_raw");
    LLVMValueRef val8 =
        LLVMBuildTrunc(cg->builder, val_raw, cg->type_i8, "memset_val8");
    LLVMBuildMemSet(cg->builder, dst_p, val8, n_raw, 1);
  } else {
    LLVMValueRef src_p =
        LLVMBuildIntToPtr(cg->builder, mid_v, cg->type_i8ptr, "mem_src_p");
    LLVMBuildMemCpy(cg->builder, dst_p, 1, src_p, 1, n_raw);
  }
  ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  return dst_v;
}

static bool ny_gencall_expr_is_managed_memory(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *target) {
  const char *type_name = infer_expr_type(cg, scopes, depth, target);
  if (!type_name || ny_gencall_type_is_nullable(type_name))
    return false;
  return ny_gencall_type_is(type_name, "ptr") ||
         ny_gencall_type_is(type_name, "str") ||
         ny_gencall_type_is(type_name, "bytes") ||
         ny_gencall_type_is(type_name, "list") ||
         ny_gencall_type_is(type_name, "tuple") ||
         ny_gencall_type_is(type_name, "dict") ||
         ny_gencall_type_is(type_name, "set");
}

static bool ny_gencall_raw_memory_intrinsic_safe(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_call_t *c,
                                                 bool is_copy_or_set) {
  if (ny_env_enabled("NYTRIX_UNSAFE_FAST_MEM"))
    return true;
  if (!c || c->args.len == 0)
    return false;
  if (is_copy_or_set)
    return false;
  if (!ny_gencall_expr_is_managed_memory(cg, scopes, depth,
                                         c->args.data[0].val))
    return false;
  if (!is_copy_or_set)
    return true;
  return false;
}

static LLVMValueRef ny_try_fast_raw_memory_builtin(codegen_t *cg, expr_t *e,
                                                   scope *scopes, size_t depth,
                                                   const char *name,
                                                   bool shadowed,
                                                   expr_call_t *c) {
  if (!name || !c)
    return 0;
  bool is_copy_or_set =
      strcmp(name, "__copy_mem") == 0 || strcmp(name, "__memset") == 0;
  if (!ny_gencall_raw_memory_intrinsic_safe(cg, scopes, depth, c,
                                            is_copy_or_set))
    return 0;
  bool want_load8_idx = strcmp(name, "__load8_idx") == 0;
  bool want_load16_idx = strcmp(name, "__load16_idx") == 0;
  bool want_load32_idx = strcmp(name, "__load32_idx") == 0;
  bool want_load64_idx = strcmp(name, "__load64_idx") == 0;
  bool want_load8_api = ny_gencall_builtin_name_is(name, "load8", shadowed);
  bool want_load16_api = ny_gencall_builtin_name_is(name, "load16", shadowed);
  bool want_load32_api = ny_gencall_builtin_name_is(name, "load32", shadowed);
  bool want_load64_api = ny_gencall_builtin_name_is(name, "load64", shadowed);
  bool want_store8_idx = strcmp(name, "__store8_idx") == 0;
  bool want_store16_idx = strcmp(name, "__store16_idx") == 0;
  bool want_store32_idx = strcmp(name, "__store32_idx") == 0;
  bool want_store64_idx = strcmp(name, "__store64_idx") == 0;
  bool want_store8_api = ny_gencall_builtin_name_is(name, "store8", shadowed);
  bool want_store16_api = ny_gencall_builtin_name_is(name, "store16", shadowed);
  bool want_store32_api = ny_gencall_builtin_name_is(name, "store32", shadowed);
  bool want_store64_api = ny_gencall_builtin_name_is(name, "store64", shadowed);
  if (((want_load8_idx && c->args.len == 2) ||
       (want_load8_api && (c->args.len == 1 || c->args.len == 2))))
    return ny_gencall_load_idx_intrinsic(cg, e, scopes, depth, c,
                                         "rt_load8_idx", cg->type_i8, 1, true);
  if (((want_load16_idx && c->args.len == 2) ||
       (want_load16_api && (c->args.len == 1 || c->args.len == 2))))
    return ny_gencall_load_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_load16_idx", cg->type_i16, 1, true);
  if (((want_load32_idx && c->args.len == 2) ||
       (want_load32_api && (c->args.len == 1 || c->args.len == 2))))
    return ny_gencall_load_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_load32_idx", cg->type_i32, 1, true);
  if (((want_load64_idx && c->args.len == 2) ||
       (want_load64_api && (c->args.len == 1 || c->args.len == 2))))
    return ny_gencall_load_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_load64_idx", cg->type_i64, 1, false);
  if (want_store8_idx && c->args.len == 3)
    return ny_gencall_store_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_store8_idx", cg->type_i8, 1, true, false);
  if (want_store8_api && (c->args.len == 2 || c->args.len == 3))
    return ny_gencall_store_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_store8_idx", cg->type_i8, 1, true, true);
  if (want_store16_idx && c->args.len == 3)
    return ny_gencall_store_idx_intrinsic(cg, e, scopes, depth, c,
                                          "rt_store16_idx", cg->type_i16, 1,
                                          true, false);
  if (want_store16_api && (c->args.len == 2 || c->args.len == 3))
    return ny_gencall_store_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_store16_idx", cg->type_i16, 1, true, true);
  if (want_store32_idx && c->args.len == 3)
    return ny_gencall_store_idx_intrinsic(cg, e, scopes, depth, c,
                                          "rt_store32_idx", cg->type_i32, 1,
                                          true, false);
  if (want_store32_api && (c->args.len == 2 || c->args.len == 3))
    return ny_gencall_store_idx_intrinsic(
        cg, e, scopes, depth, c, "rt_store32_idx", cg->type_i32, 1, true, true);
  if (want_store64_idx && c->args.len == 3)
    return ny_gencall_store_idx_intrinsic(cg, e, scopes, depth, c,
                                          "rt_store64_idx", cg->type_i64, 1,
                                          false, false);
  if (want_store64_api && (c->args.len == 2 || c->args.len == 3))
    return ny_gencall_store_idx_intrinsic(cg, e, scopes, depth, c,
                                          "rt_store64_idx", cg->type_i64, 1,
                                          false, true);
  if (strcmp(name, "__copy_mem") == 0 && c->args.len == 3)
    return ny_gencall_copy_or_set_intrinsic(cg, e, scopes, depth, c, false);
  if (strcmp(name, "__memset") == 0 && c->args.len == 3)
    return ny_gencall_copy_or_set_intrinsic(cg, e, scopes, depth, c, true);
  return 0;
}

static LLVMValueRef ny_get_raw_i64_runtime_fn(codegen_t *cg, const char *name,
                                              unsigned argc) {
  if (!cg || !name || argc > 16)
    return NULL;
  LLVMTypeRef args[16];
  for (unsigned i = 0; i < argc; i++)
    args[i] = cg->type_i64;
  LLVMTypeRef ft = LLVMFunctionType(cg->type_i64, args, argc, 0);
  LLVMValueRef fn = ny_get_named_fn(cg, name);
  if (!fn) {
    fn = LLVMAddFunction(cg->module, name, ft);
    ny_apply_rt_fn_attrs(cg, fn);
  }
  return fn;
}

static bool ny_gencall_expr_is_ptr_add_of_managed(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name || e->as.call.args.len < 2)
    return false;
  const char *name = e->as.call.callee->as.ident.name;
  if (!ny_name_tail_is(name, "ptr_add"))
    return false;
  expr_t *off = e->as.call.args.data[1].val;
  if (!ny_expr_literal_i64(off, NULL) &&
      !ny_is_proven_int(cg, scopes, depth, off, NULL) &&
      !ny_gencall_expr_is_int_index(cg, scopes, depth, off))
    return false;
  if (ny_gencall_expr_is_managed_memory(cg, scopes, depth,
                                        e->as.call.args.data[0].val))
    return true;

  return true;
}

static bool ny_gencall_expr_is_simmd_ptr_arg(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e) {
  if (ny_gencall_expr_is_managed_memory(cg, scopes, depth, e))
    return true;
  return ny_gencall_expr_is_ptr_add_of_managed(cg, scopes, depth, e);
}

static bool ny_gencall_expr_is_simmd_int_arg(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e) {
  if (ny_expr_literal_i64(e, NULL))
    return true;
  if (ny_is_proven_int(cg, scopes, depth, e, NULL))
    return true;
  return ny_gencall_expr_is_int_index(cg, scopes, depth, e);
}

static LLVMValueRef ny_gencall_simmd_raw_ptr_arg(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 const char *name) {
  if (e && e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name &&
      ny_name_tail_is(e->as.call.callee->as.ident.name, "ptr_add") &&
      e->as.call.args.len >= 2) {
    LLVMValueRef base =
        gen_expr(cg, scopes, depth, e->as.call.args.data[0].val);
    LLVMValueRef off = gen_expr(cg, scopes, depth, e->as.call.args.data[1].val);
    if (!base || !off)
      return NULL;
    base = ny_build_rt_untag_i64(cg,
                                 ny_cast_to_i64(cg, base, "simmd_raw_ptr_base"),
                                 "simmd_raw_ptr_base");
    off = ny_gencall_index_raw_i64(
        cg, scopes, depth, e->as.call.args.data[1].val,
        ny_cast_to_i64(cg, off, "simmd_raw_ptr_off"), "simmd_raw_ptr_off");
    return LLVMBuildAdd(cg->builder, base, off, name ? name : "simmd_raw_ptr");
  }
  LLVMValueRef v = gen_expr(cg, scopes, depth, e);
  if (!v)
    return NULL;
  v = ny_cast_to_i64(cg, v, name);
  return ny_build_rt_untag_i64(cg, v, name);
}

static bool ny_gencall_const_raw_i64_expr(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e, int64_t *out,
                                          unsigned recursion) {
  if (!e || !out || recursion > 32)
    return false;
  if (ny_expr_literal_i64(e, out))
    return true;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = ny_gencall_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                           name_len, e->as.ident.hash);
    if (!b || b->is_mut)
      return false;
    expr_t *init = ny_binding_var_init_expr(b, e->as.ident.name);
    if (!init) {
      const char *tail = strrchr(e->as.ident.name, '.');
      if (tail && tail[1])
        init = ny_binding_var_init_expr(b, tail + 1);
    }
    if (!init || init == e)
      return false;
    return ny_gencall_const_raw_i64_expr(cg, scopes, depth, init, out,
                                         recursion + 1);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op) {
    int64_t r = 0;
    if (!ny_gencall_const_raw_i64_expr(cg, scopes, depth, e->as.unary.right, &r,
                                       recursion + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *out = r;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      int64_t v = 0;
      if (__builtin_sub_overflow((int64_t)0, r, &v))
        return false;
      *out = v;
      return true;
    }
    return false;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    int64_t l = 0, r = 0, v = 0;
    if (!ny_gencall_const_raw_i64_expr(cg, scopes, depth, e->as.binary.left, &l,
                                       recursion + 1) ||
        !ny_gencall_const_raw_i64_expr(cg, scopes, depth, e->as.binary.right,
                                       &r, recursion + 1))
      return false;
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") == 0) {
      if (__builtin_add_overflow(l, r, &v))
        return false;
    } else if (strcmp(op, "-") == 0) {
      if (__builtin_sub_overflow(l, r, &v))
        return false;
    } else if (strcmp(op, "*") == 0) {
      if (__builtin_mul_overflow(l, r, &v))
        return false;
    } else if (strcmp(op, "/") == 0) {
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      v = l / r;
    } else if (strcmp(op, "%") == 0) {
      if (r == 0 || (l == INT64_MIN && r == -1))
        return false;
      v = l % r;
    } else {
      return false;
    }
    *out = v;
    return true;
  }
  return false;
}

static LLVMValueRef ny_gencall_simmd_raw_int_arg(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 const char *name) {
  int64_t raw = 0;
  if (ny_gencall_const_raw_i64_expr(cg, scopes, depth, e, &raw, 0))
    return LLVMConstInt(cg->type_i64, (uint64_t)raw, true);
  LLVMValueRef v = gen_expr(cg, scopes, depth, e);
  if (!v)
    return NULL;
  v = ny_cast_to_i64(cg, v, name);
  return ny_gencall_index_raw_i64(cg, scopes, depth, e, v, name);
}

static LLVMValueRef ny_try_fast_simmd_raw_builtin(codegen_t *cg, expr_t *e,
                                                  scope *scopes, size_t depth,
                                                  const char *name,
                                                  bool shadowed,
                                                  expr_call_t *c) {
  (void)shadowed;
  if (!cg || !e || !name || !c)
    return 0;

  bool want_byte_class = strcmp(name, "__simmd_byte_class_reduce") == 0;
  bool want_sqlscan = strcmp(name, "__simmd_i32_sqlscan_sum_ptr") == 0;
  if (!want_byte_class && !want_sqlscan)
    return 0;

  unsigned ptr_args = want_byte_class ? 1u : 4u;
  unsigned argc = want_byte_class ? 7u : 6u;
  if (c->args.len != argc)
    return 0;

  for (unsigned i = 0; i < ptr_args; i++) {
    if (!ny_gencall_expr_is_simmd_ptr_arg(cg, scopes, depth,
                                          c->args.data[i].val))
      return 0;
  }
  if (want_sqlscan) {
    for (unsigned i = ptr_args; i < argc; i++) {
      if (!ny_gencall_expr_is_simmd_int_arg(cg, scopes, depth,
                                            c->args.data[i].val))
        return 0;
    }
  }

  LLVMValueRef args[7];
  for (unsigned i = 0; i < argc; i++) {
    char arg_name[48];
    snprintf(arg_name, sizeof(arg_name), "simmd_raw_arg_%u", i);
    args[i] = (i < ptr_args)
                  ? ny_gencall_simmd_raw_ptr_arg(cg, scopes, depth,
                                                 c->args.data[i].val, arg_name)
                  : ny_gencall_simmd_raw_int_arg(cg, scopes, depth,
                                                 c->args.data[i].val, arg_name);
    if (!args[i]) {
      ny_diag_error(e->tok,
                    "failed to evaluate arguments for raw SIMD fast path");
      cg->had_error = 1;
      return ny_c0(cg);
    }
  }

  const char *raw_name = want_byte_class ? "rt_simmd_byte_class_reduce_raw"
                                         : "rt_simmd_i32_sqlscan_sum_raw";
  LLVMValueRef fn = ny_get_raw_i64_runtime_fn(cg, raw_name, argc);
  if (!fn)
    return 0;
  LLVMTypeRef ft = LLVMGlobalGetValueType(fn);
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef raw = LLVMBuildCall2(cg->builder, ft, fn, args, argc,
                                    want_byte_class ? "simmd_byte_class_raw"
                                                    : "simmd_sqlscan_raw");
  return ny_tag_int(cg, raw);
}

static LLVMValueRef
ny_build_memoized_direct_call(codegen_t *cg, token_t tok, LLVMTypeRef ft,
                              LLVMValueRef callee, LLVMValueRef *args,
                              unsigned argc, bool memo_enabled, bool int_only) {
  if (!cg || !cg->builder)
    return 0;
#define NY_MEMO_FALLBACK_CALL()                                                \
  do {                                                                         \
    ny_dbg_loc(cg, tok);                                                       \
    return LLVMBuildCall2(cg->builder, ft, callee, args, argc, "");            \
  } while (0)
  if (!memo_enabled)
    NY_MEMO_FALLBACK_CALL();
  if (LLVMGetReturnType(ft) != cg->type_i64)
    NY_MEMO_FALLBACK_CALL();
  if (argc > 6)
    NY_MEMO_FALLBACK_CALL();
  for (unsigned i = 0; i < argc; i++) {
    if (LLVMTypeOf(args[i]) != cg->type_i64)
      NY_MEMO_FALLBACK_CALL();
  }

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  if (!cur_bb)
    NY_MEMO_FALLBACK_CALL();
  LLVMValueRef parent_fn = LLVMGetBasicBlockParent(cur_bb);
  if (!parent_fn)
    NY_MEMO_FALLBACK_CALL();

  uint64_t site_id = ++cg->auto_memo_site_seq;
  char valid_name[96];
  char res_name[96];
  char args_name[96];
  char depth_name[96];
  snprintf(valid_name, sizeof(valid_name), ".__ny_memo.valid.%llu",
           (unsigned long long)site_id);
  snprintf(res_name, sizeof(res_name), ".__ny_memo.res.%llu",
           (unsigned long long)site_id);
  snprintf(args_name, sizeof(args_name), ".__ny_memo.args.%llu",
           (unsigned long long)site_id);
  snprintf(depth_name, sizeof(depth_name), ".__ny_memo.depth.%llu",
           (unsigned long long)site_id);

  LLVMValueRef valid_g = ny_get_global(cg, valid_name);
  if (!valid_g) {
    valid_g = LLVMAddGlobal(cg->module, cg->type_i1, valid_name);
    LLVMSetInitializer(valid_g, LLVMConstInt(cg->type_i1, 0, false));
    LLVMSetLinkage(valid_g, LLVMInternalLinkage);
    LLVMSetThreadLocal(valid_g, 1);
  }

  LLVMValueRef res_g = ny_get_global(cg, res_name);
  if (!res_g) {
    res_g = LLVMAddGlobal(cg->module, cg->type_i64, res_name);
    LLVMSetInitializer(res_g, ny_c0(cg));
    LLVMSetLinkage(res_g, LLVMInternalLinkage);
    LLVMSetThreadLocal(res_g, 1);
  }

  LLVMTypeRef arg_arr_ty = NULL;
  LLVMValueRef args_g = NULL;
  if (argc > 0) {
    arg_arr_ty = LLVMArrayType(cg->type_i64, argc);
    args_g = ny_get_global(cg, args_name);
    if (!args_g) {
      args_g = LLVMAddGlobal(cg->module, arg_arr_ty, args_name);
      LLVMSetInitializer(args_g, LLVMConstNull(arg_arr_ty));
      LLVMSetLinkage(args_g, LLVMInternalLinkage);
      LLVMSetThreadLocal(args_g, 1);
    }
  }

  LLVMValueRef depth_g = ny_get_global(cg, depth_name);
  if (!depth_g) {
    depth_g = LLVMAddGlobal(cg->module, cg->type_i64, depth_name);
    LLVMSetInitializer(depth_g, ny_c0(cg));
    LLVMSetLinkage(depth_g, LLVMInternalLinkage);
    LLVMSetThreadLocal(depth_g, 1);
  }

  LLVMBasicBlockRef hit_bb = ny_bb_fn(parent_fn, "memo.hit");
  LLVMBasicBlockRef miss_bb = ny_bb_fn(parent_fn, "memo.miss");
  LLVMBasicBlockRef join_bb = ny_bb_fn(parent_fn, "memo.join");

  ny_dbg_loc(cg, tok);
  LLVMValueRef valid = LLVMBuildLoad2(cg->builder, cg->type_i1, valid_g, "");
  LLVMValueRef hit_cond = valid;
  LLVMValueRef memo_args_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (int_only && argc > 0) {
    for (unsigned i = 0; i < argc; i++) {
      LLVMValueRef bit = ny_and(cg, args[i], ny_c1(cg), "");
      LLVMValueRef is_int = ny_eq(cg, bit, ny_c1(cg), "");
      memo_args_ok = ny_and(cg, memo_args_ok, is_int, "");
    }
  }
  LLVMValueRef depth_now = ny_load(cg, depth_g, NY_LLVM_NAME(cg, "memo_depth"));
  LLVMValueRef memo_depth_ok = ny_eq(cg, depth_now, ny_c0(cg), "");
  LLVMValueRef memo_lookup_ok = ny_and(cg, memo_depth_ok, memo_args_ok, "");
  if (argc > 0 && args_g) {
    LLVMValueRef all_eq = LLVMConstInt(cg->type_i1, 1, false);
    for (unsigned i = 0; i < argc; i++) {
      LLVMValueRef idxs[2] = {ny_c0(cg), LLVMConstInt(cg->type_i64, i, false)};
      LLVMValueRef slot =
          LLVMBuildInBoundsGEP2(cg->builder, arg_arr_ty, args_g, idxs, 2, "");
      LLVMValueRef cached = ny_load(cg, slot, "");
      LLVMValueRef eq = ny_eq(cg, cached, args[i], "");
      all_eq = ny_and(cg, all_eq, eq, "");
    }
    hit_cond = ny_and(cg, valid, all_eq, "");
  }
  hit_cond = ny_and(cg, hit_cond, memo_lookup_ok, "");
  ny_cond_br(cg, hit_cond, hit_bb, miss_bb);

  ny_pos(cg, hit_bb);

  ny_dbg_loc(cg, tok);
  LLVMValueRef hit_res = ny_load(cg, res_g, "");
  ny_br(cg, join_bb);
  LLVMBasicBlockRef hit_end_bb = ny_cur_block(cg);

  ny_pos(cg, miss_bb);

  ny_dbg_loc(cg, tok);
  LLVMValueRef depth_inc = ny_add(cg, depth_now, ny_c1(cg), "");
  ny_store(cg, depth_g, depth_inc);
  LLVMValueRef miss_res =
      LLVMBuildCall2(cg->builder, ft, callee, args, argc, "");
  LLVMValueRef depth_after = ny_load(cg, depth_g, "");
  LLVMValueRef depth_dec = ny_sub(cg, depth_after, ny_c1(cg), "");
  ny_store(cg, depth_g, depth_dec);

  LLVMValueRef result_cacheable = LLVMConstInt(cg->type_i1, 1, false);
  if (int_only) {
    LLVMValueRef res_lsb = ny_and(cg, miss_res, ny_c1(cg), "");
    LLVMValueRef res_is_int = ny_eq(cg, res_lsb, ny_c1(cg), "");
    LLVMValueRef res_is_none = ny_eq(cg, miss_res, ny_c0(cg), "");
    LLVMValueRef res_is_true = ny_eq(cg, miss_res, ny_ctrue(cg), "");
    LLVMValueRef res_is_false = ny_eq(cg, miss_res, ny_cfalse(cg), "");
    LLVMValueRef res_small = LLVMBuildOr(
        cg->builder, res_is_none, ny_or(cg, res_is_true, res_is_false, ""), "");
    result_cacheable = ny_or(cg, res_is_int, res_small, "");
  }
  LLVMValueRef can_store = ny_and(cg, memo_lookup_ok, result_cacheable, "");

  LLVMValueRef old_res = ny_load(cg, res_g, "");
  LLVMValueRef store_res = ny_select(cg, can_store, miss_res, old_res, "");
  ny_store(cg, res_g, store_res);

  if (argc > 0 && args_g) {
    for (unsigned i = 0; i < argc; i++) {
      LLVMValueRef idxs[2] = {ny_c0(cg), LLVMConstInt(cg->type_i64, i, false)};
      LLVMValueRef slot =
          LLVMBuildInBoundsGEP2(cg->builder, arg_arr_ty, args_g, idxs, 2, "");
      LLVMValueRef old_arg = ny_load(cg, slot, "");
      LLVMValueRef store_arg = ny_select(cg, can_store, args[i], old_arg, "");
      ny_store(cg, slot, store_arg);
    }
  }
  LLVMValueRef old_valid =
      LLVMBuildLoad2(cg->builder, cg->type_i1, valid_g, "");
  LLVMValueRef new_valid = ny_select(
      cg, can_store, LLVMConstInt(cg->type_i1, 1, false), old_valid, "");
  if (int_only) {
    LLVMValueRef should_clear =
        ny_and(cg, memo_lookup_ok,
               LLVMBuildNot(cg->builder, result_cacheable, ""), "");
    new_valid = ny_select(cg, should_clear, LLVMConstInt(cg->type_i1, 0, false),
                          new_valid, "");
  }
  ny_store(cg, valid_g, new_valid);
  ny_br(cg, join_bb);
  LLVMBasicBlockRef miss_end_bb = ny_cur_block(cg);

  ny_pos(cg, join_bb);

  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "memo_res"));
  LLVMAddIncoming(phi, (LLVMValueRef[]){hit_res, miss_res},
                  (LLVMBasicBlockRef[]){hit_end_bb, miss_end_bb}, 2);
  return phi;
#undef NY_MEMO_FALLBACK_CALL
}

static fun_sig *ny_gencall_lookup_helper(codegen_t *cg, fun_sig **cache_slot,
                                         const char *const *names,
                                         size_t names_len) {
  if (cache_slot && *cache_slot) {
    if (ny_sig_in_current_sigs(cg, *cache_slot))
      return *cache_slot;
    *cache_slot = NULL;
  }
  for (size_t i = 0; i < names_len; ++i) {
    const char *name = names[i];
    if (!name || !*name)
      continue;
    fun_sig *sig = lookup_fun(cg, name, 0);
    if (sig) {
      if (cache_slot)
        *cache_slot = sig;
      return sig;
    }
  }
  return NULL;
}

#define NY_DEFINE_GENCALL_LOOKUP_WRAPPER(fn_name, cache_field, ...)            \
  static fun_sig *fn_name(codegen_t *cg) {                                     \
    static const char *const k_names[] = {__VA_ARGS__};                        \
    return ny_gencall_lookup_helper(cg, &cg->cache_field, k_names,             \
                                    sizeof(k_names) / sizeof(k_names[0]));     \
  }

NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_flt_unbox, cached_fn_flt_unbox,
                                 "__flt_unbox_val")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_flt_box, cached_fn_flt_box,
                                 "__flt_box_val")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_getter, cached_fn_get,
                                 "std.core.get", "std.core.reflect.get", "get")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_globals, cached_fn_globals,
                                 "__globals")
NY_DEFINE_GENCALL_LOOKUP_WRAPPER(ny_gencall_kwarg, cached_fn_kwarg, "__kwarg",
                                 "std.core.__kwarg")

#undef NY_DEFINE_GENCALL_LOOKUP_WRAPPER

static bool ny_gencall_is_thread_attr(fun_sig *sig) {
  if (!sig || sig->is_extern || !sig->stmt_t)
    return false;
  if (sig->stmt_t->kind != NY_S_FUNC)
    return false;
  return sig->stmt_t->as.fn.attr_thread;
}

static bool ny_gencall_has_cache_attr(fun_sig *sig) {
  if (!sig || sig->is_extern || !sig->stmt_t)
    return false;
  if (sig->stmt_t->kind != NY_S_FUNC)
    return false;
  return sig->stmt_t->as.fn.attr_cache;
}

static const char *abi_skip_nullable(const char *n) {
  if (n)
    while (*n == '?')
      n++;
  return n;
}

static bool abi_type_is_tagged(const char *type_name) {
  if (!type_name || !*type_name)
    return true;
  type_name = abi_skip_nullable(type_name);
  if (ny_gencall_type_is_vec(type_name))
    return true;
  static const char *const tagged[] = {"any",   "str",   "bool", "Result",
                                       "list",  "dict",  "tuple", "set",
                                       "bytes", "range", "bigint"};
  return ny_gencall_str_in(type_name, tagged,
                           sizeof(tagged) / sizeof(tagged[0]));
}

static bool ny_memo_impure_return_allowed(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  static const char *const allowed[] = {"int", "bool", "none"};
  return ny_gencall_str_in(type_name, allowed,
                           sizeof(allowed) / sizeof(allowed[0]));
}

static bool abi_type_is_ptr(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  if (type_name[0] == '*')
    return true;
  return strcmp(type_name, "ptr") == 0 || strcmp(type_name, "cstr") == 0;
}

static bool abi_type_is_fnptr(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  return strcmp(type_name, "fnptr") == 0;
}

static LLVMValueRef abi_decode_native_fnptr(codegen_t *cg, LLVMValueRef v) {
#if UINTPTR_MAX == 0xffffffff
  LLVMValueRef native_mark = LLVMConstInt(cg->type_i64, NY_NATIVE_MARK, false);
  LLVMValueRef native_tag = LLVMConstInt(cg->type_i64, NY_NATIVE_TAG, false);
  LLVMValueRef low_mask = LLVMConstInt(cg->type_i64, NY_NATIVE_TAG_MASK, false);
  LLVMValueRef has_mark =
      ny_ne(cg, ny_and(cg, v, native_mark, NY_LLVM_NAME(cg, "fnptr_mark")),
            ny_c0(cg), NY_LLVM_NAME(cg, "fnptr_has_mark"));
  LLVMValueRef low_bits =
      ny_and(cg, v, low_mask, NY_LLVM_NAME(cg, "fnptr_low"));
  LLVMValueRef has_tag =
      ny_eq(cg, low_bits, native_tag, NY_LLVM_NAME(cg, "fnptr_has_tag"));
  LLVMValueRef is_native =
      ny_and(cg, has_mark, has_tag, NY_LLVM_NAME(cg, "fnptr_is_native"));
  LLVMValueRef cleared =
      ny_and(cg, v, LLVMConstInt(cg->type_i64, ~NY_NATIVE_MARK, false),
             NY_LLVM_NAME(cg, "fnptr_cleared"));
  LLVMValueRef decoded = LLVMBuildLShr(
      cg->builder, cleared, LLVMConstInt(cg->type_i64, NY_NATIVE_SHIFT, false),
      NY_LLVM_NAME(cg, "fnptr_decoded"));
#else
  LLVMValueRef low_mask = LLVMConstInt(cg->type_i64, NY_NATIVE_TAG_MASK, false);
  LLVMValueRef native_tag = LLVMConstInt(cg->type_i64, NY_NATIVE_TAG, false);
  LLVMValueRef low_bits =
      ny_and(cg, v, low_mask, NY_LLVM_NAME(cg, "fnptr_low"));
  LLVMValueRef is_native =
      ny_eq(cg, low_bits, native_tag, NY_LLVM_NAME(cg, "fnptr_is_native"));
  LLVMValueRef decoded = LLVMBuildLShr(
      cg->builder, v, LLVMConstInt(cg->type_i64, NY_NATIVE_SHIFT, false),
      NY_LLVM_NAME(cg, "fnptr_decoded"));
#endif
  return ny_select(cg, is_native, decoded, v, NY_LLVM_NAME(cg, "fnptr_raw"));
}

static LLVMValueRef abi_raw_value_to_i64(codegen_t *cg, LLVMValueRef v,
                                         const char *name) {
  if (!v)
    return v;
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (ty == cg->type_i64)
    return v;
  if (LLVMGetTypeKind(ty) == LLVMPointerTypeKind)
    return ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, name));
  if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(ty);
    if (bits < 64)
      return LLVMBuildZExt(cg->builder, v, cg->type_i64,
                           NY_LLVM_NAME(cg, name));
    if (bits > 64)
      return LLVMBuildTrunc(cg->builder, v, cg->type_i64,
                            NY_LLVM_NAME(cg, name));
    return v;
  }
  return ny_cast_to_i64(cg, v, name);
}

static LLVMValueRef abi_encode_native_i64(codegen_t *cg, LLVMValueRef raw,
                                          const char *name) {
  raw = abi_raw_value_to_i64(cg, raw, "native_raw");
  LLVMValueRef is_null =
      ny_eq(cg, raw, ny_c0(cg), NY_LLVM_NAME(cg, "native_null"));
  LLVMValueRef shifted = LLVMBuildShl(
      cg->builder, raw, LLVMConstInt(cg->type_i64, NY_NATIVE_SHIFT, false),
      NY_LLVM_NAME(cg, "native_shift"));
  LLVMValueRef tagged =
      ny_or(cg, shifted, LLVMConstInt(cg->type_i64, NY_NATIVE_TAG, false),
            NY_LLVM_NAME(cg, "native_tagged"));
#if UINTPTR_MAX == 0xffffffff
  tagged =
      ny_or(cg, tagged, LLVMConstInt(cg->type_i64, NY_NATIVE_MARK, false),
            NY_LLVM_NAME(cg, "native_marked"));
#endif
  return ny_select(cg, is_null, ny_c0(cg), tagged,
                   NY_LLVM_NAME(cg, name ? name : "native"));
}

static bool abi_type_is_float(const char *type_name) {
  if (!type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  static const char *const floats[] = {"f32", "f64", "f128"};
  return ny_gencall_str_in(type_name, floats,
                           sizeof(floats) / sizeof(floats[0]));
}

static bool abi_type_is_complex(const char *type_name) {
  if (!type_name)
    return false;
  type_name = ny_type_leaf(abi_skip_nullable(type_name));
  static const char *const complex[] = {"complex", "c128", "c64"};
  return ny_gencall_str_in(type_name, complex,
                           sizeof(complex) / sizeof(complex[0]));
}

static LLVMTypeRef abi_complex_type(codegen_t *cg, const char *type_name) {
  const char *leaf = ny_type_leaf(abi_skip_nullable(type_name));
  LLVMTypeRef elem =
      (leaf && strcmp(leaf, "c64") == 0) ? cg->type_f32 : cg->type_f64;
  LLVMTypeRef elems[2] = {elem, elem};
  return LLVMStructTypeInContext(cg->ctx, elems, 2, false);
}

static bool abi_type_is_signed_int(const char *type_name) {
  if (!type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  static const char *const ints[] = {"i8",  "i16",  "i32", "i64",
                                     "i128", "char", "int"};
  return ny_gencall_str_in(type_name, ints, sizeof(ints) / sizeof(ints[0]));
}

static bool abi_type_is_unsigned_int(const char *type_name) {
  if (!type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  static const char *const ints[] = {"u8", "u16", "u32", "u64", "u128",
                                     "handle"};
  return ny_gencall_str_in(type_name, ints, sizeof(ints) / sizeof(ints[0]));
}

static layout_def_t *abi_layout_from_name(codegen_t *cg,
                                          const char *type_name) {
  if (!cg || !type_name || !*type_name)
    return NULL;
  type_name = abi_skip_nullable(type_name);
  if (!*type_name || *type_name == '*')
    return NULL;
  layout_def_t *layout = lookup_layout(cg, type_name);
  if (!layout || !layout->llvm_type)
    return NULL;
  return layout;
}

static bool abi_type_needs_native_coerce(codegen_t *cg, const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  if (abi_type_is_fnptr(type_name))
    return true;
  if (abi_layout_from_name(cg, type_name))
    return true;
  return ny_is_native_abi_type_name(type_name) && !ny_type_is_tagged(type_name);
}

static bool abi_sig_type_needs_native_coerce(codegen_t *cg, fun_sig *sig,
                                             const char *type_name) {
  if (!sig || !sig->is_native_abi || !type_name || !*type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  if (abi_type_is_fnptr(type_name) && !sig->is_extern)
    return false;
  return abi_type_needs_native_coerce(cg, type_name);
}

static bool abi_sig_param_needs_native_coerce(fun_sig *sig,
                                              const char *type_name) {
  if (!sig || !sig->is_native_abi || !type_name || !*type_name)
    return false;
  type_name = abi_skip_nullable(type_name);
  if (abi_type_is_fnptr(type_name) && !sig->is_extern)
    return false;
  return !abi_type_is_tagged(type_name);
}

static LLVMValueRef abi_layout_ptr_from_value(codegen_t *cg, LLVMValueRef v,
                                              LLVMTypeRef pointee_type,
                                              const char *name) {
  LLVMTypeRef ptr_ty = LLVMPointerType(pointee_type, 0);
  if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind)
    return LLVMBuildBitCast(cg->builder, v, ptr_ty, NY_LLVM_NAME(cg, name));
  v = ny_cast_to_i64(cg, v, name ? name : "layout_ptr_i64");
  LLVMValueRef raw =
      ny_build_rt_untag_i64(cg, v, NY_LLVM_NAME(cg, "layout_ptr_raw"));
  return LLVMBuildIntToPtr(cg->builder, raw, ptr_ty, NY_LLVM_NAME(cg, name));
}

static void abi_add_call_type_attr(codegen_t *cg, LLVMValueRef call,
                                   LLVMAttributeIndex idx, const char *name,
                                   LLVMTypeRef type) {
  if (!cg || !call || !name || !*name || !type)
    return;
  unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
  if (!kind)
    return;
  LLVMAttributeRef attr = LLVMCreateTypeAttribute(cg->ctx, kind, type);
  LLVMAddCallSiteAttribute(call, idx, attr);
}

static void abi_add_call_align_attr(codegen_t *cg, LLVMValueRef call,
                                    LLVMAttributeIndex idx, size_t align) {
  if (!cg || !call || align == 0)
    return;
  unsigned kind = LLVMGetEnumAttributeKindForName("align", 5);
  if (!kind)
    return;
  LLVMAttributeRef attr =
      LLVMCreateEnumAttribute(cg->ctx, kind, (uint64_t)align);
  LLVMAddCallSiteAttribute(call, idx, attr);
}

static void abi_apply_native_layout_call_attrs(codegen_t *cg, LLVMValueRef call,
                                               fun_sig *sig_meta) {
  if (!cg || !call || !sig_meta || !sig_meta->is_native_abi)
    return;
  bool has_sret = false;
  layout_def_t *ret_layout = NULL;
  if (sig_meta->native_sret_return && sig_meta->return_type) {
    ret_layout = abi_layout_from_name(cg, sig_meta->return_type);
    if (ret_layout && ret_layout->llvm_type) {
      has_sret = true;
      abi_add_call_type_attr(cg, call, 1, "sret", ret_layout->llvm_type);
      abi_add_call_align_attr(cg, call, 1, ret_layout->align);
    }
  }
  for (size_t i = 0; i < sig_meta->param_types.len; i++) {
    const char *tname = sig_meta->param_types.data[i];
    layout_def_t *layout = abi_layout_from_name(cg, tname);
    if (!layout || !layout->llvm_type || layout->size <= 16)
      continue;
    LLVMAttributeIndex idx = (LLVMAttributeIndex)(i + 1 + (has_sret ? 1 : 0));
    abi_add_call_type_attr(cg, call, idx, "byval", layout->llvm_type);
    abi_add_call_align_attr(cg, call, idx, layout->align);
  }
}

static bool ny_expr_is_list_ctor(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name)
    return false;
  const char *n = e->as.call.callee->as.ident.name;
  return strcmp(n, "list") == 0;
}

static bool ny_expr_is_dict_ctor(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name)
    return false;
  const char *n = e->as.call.callee->as.ident.name;
  return strcmp(n, "dict") == 0;
}

static expr_t *ny_binding_list_storage_init(binding *b) {
  if (!b || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *name = var->names.data[i];
    expr_t *init = var->exprs.data[i];
    if (!name || !b->name || strcmp(name, b->name) != 0 || !init)
      continue;
    if (ny_expr_is_list_or_tuple_lit(init) || ny_expr_is_list_ctor(init))
      return init;
  }
  return NULL;
}

static expr_t *ny_binding_dict_storage_init(binding *b) {
  if (!b || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *name = var->names.data[i];
    expr_t *init = var->exprs.data[i];
    if (!name || !b->name || strcmp(name, b->name) != 0 || !init)
      continue;
    if (ny_expr_is_dict_ctor(init))
      return init;
  }
  return NULL;
}

static bool ny_expr_is_direct_list_storage(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *target) {
  if (ny_expr_is_list_or_tuple_lit(target) || ny_expr_is_list_ctor(target))
    return true;
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b =
      ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                name_len, target->as.ident.hash);
  if (b && b->raw_int_list_mutation && b->raw_int_list_ptr)
    return false;
  return ny_binding_list_storage_init(b) != NULL;
}

static bool ny_expr_has_known_list_like_type(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *target) {
  const char *type_name = infer_expr_type(cg, scopes, depth, target);
  return ny_gencall_type_is(type_name, "list") ||
         ny_gencall_type_is(type_name, "tuple");
}

static bool ny_expr_has_known_list_type(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *target) {
  return ny_gencall_type_is(infer_expr_type(cg, scopes, depth, target), "list");
}

static LLVMValueRef ny_try_emit_direct_list_append(codegen_t *cg, scope *scopes,
                                                   size_t depth, expr_t *target,
                                                   expr_t *value, token_t tok) {
  if (!cg || !target || !value)
    return NULL;
  bool target_is_direct = ny_expr_is_direct_list_storage(cg, scopes, depth, target);
  bool target_is_known_list =
      target_is_direct || ny_expr_has_known_list_type(cg, scopes, depth, target);
  if (!target_is_known_list)
    return NULL;
  fun_sig *append_sig = lookup_fun(cg, "__append", 0);
  if (!append_sig || !append_sig->type || !append_sig->value)
    return NULL;
  LLVMValueRef args[2];
  args[0] = gen_expr(cg, scopes, depth, target);
  args[1] = gen_expr(cg, scopes, depth, value);
  if (!args[0] || !args[1]) {
    ny_diag_error(tok, "failed to evaluate append(...) argument");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  args[0] = ny_cast_to_i64(cg, args[0], "append_list");
  args[1] = ny_cast_to_i64(cg, args[1], "append_value");
  ny_dbg_loc(cg, tok);
  if (!ny_env_enabled_default_on("NYTRIX_FAST_LIST_APPEND"))
    return LLVMBuildCall2(cg->builder, append_sig->type, append_sig->value,
                          args, 2, NY_LLVM_NAME(cg, "append_direct"));

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef bounds_bb = NULL;
  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "append.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "append.slow");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "append.join");

  if (target_is_direct) {
    bounds_bb = cur_bb;
  } else {
    LLVMBasicBlockRef tag_bb = ny_bb_fn(fn, "append.tag");
    bounds_bb = ny_bb_fn(fn, "append.bounds");
    LLVMValueRef is_ptr = ny_build_is_ptr_pred(cg, args[0], "append_is_ptr");
    ny_cond_br(cg, is_ptr, tag_bb, slow_bb);

    ny_pos(cg, tag_bb);
    LLVMValueRef tag_addr =
        ny_sub(cg, args[0], LLVMConstInt(cg->type_i64, 8, false),
               "append_tag_addr");
    LLVMValueRef tag_ptr =
        LLVMBuildIntToPtr(cg->builder, tag_addr, ny_ptr_i64_ty(cg),
                          "append_tag_ptr");
    LLVMValueRef tag_v = ny_load(cg, tag_ptr, NY_LLVM_NAME(cg, "append_tag"));
    LLVMValueRef is_list =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, tag_v,
                      LLVMConstInt(cg->type_i64, 100, false),
                      NY_LLVM_NAME(cg, "append_is_list"));
    ny_cond_br(cg, is_list, bounds_bb, slow_bb);
    ny_pos(cg, bounds_bb);
  }

  LLVMValueRef target_ptr =
      LLVMBuildIntToPtr(cg->builder, args[0], ny_ptr_i64_ty(cg),
                        "append_ptr_i64");
  LLVMValueRef len_tagged =
      ny_load(cg, target_ptr, NY_LLVM_NAME(cg, "append_len"));
  LLVMValueRef len_raw =
      ny_build_untagged_or_raw_i64(cg, len_tagged, "append_len_raw");
  LLVMValueRef cap_addr =
      ny_add(cg, args[0], LLVMConstInt(cg->type_i64, 8, false),
             "append_cap_addr");
  LLVMValueRef cap_ptr =
      LLVMBuildIntToPtr(cg->builder, cap_addr, ny_ptr_i64_ty(cg),
                        "append_cap_ptr_i64");
  LLVMValueRef cap_tagged =
      ny_load(cg, cap_ptr, NY_LLVM_NAME(cg, "append_cap"));
  LLVMValueRef cap_raw =
      ny_build_untagged_or_raw_i64(cg, cap_tagged, "append_cap_raw");
  LLVMValueRef has_cap =
      LLVMBuildICmp(cg->builder, LLVMIntSLT, len_raw, cap_raw,
                    NY_LLVM_NAME(cg, "append_has_cap"));
  ny_cond_br(cg, has_cap, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, len_raw, LLVMConstInt(cg->type_i64, 3, false),
                   "append_scaled");
  LLVMValueRef byte_off =
      LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                   "append_off");
  LLVMValueRef elem_addr =
      ny_add(cg, args[0], byte_off, NY_LLVM_NAME(cg, "append_addr"));
  LLVMValueRef elem_ptr =
      LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                        "append_elem_ptr_i64");
  ny_store(cg, elem_ptr, args[1]);
  LLVMValueRef next_len =
      ny_add(cg, len_raw, LLVMConstInt(cg->type_i64, 1, false),
             NY_LLVM_NAME(cg, "append_next_len"));
  LLVMValueRef next_len_tagged =
      ny_or(cg,
            ny_shl(cg, next_len, ny_c1(cg),
                   NY_LLVM_NAME(cg, "append_next_len_shl")),
            ny_c1(cg), NY_LLVM_NAME(cg, "append_next_len_tagged"));
  ny_store(cg, target_ptr, next_len_tagged);
  LLVMBasicBlockRef fast_end = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_res =
      LLVMBuildCall2(cg->builder, append_sig->type, append_sig->value, args, 2,
                     NY_LLVM_NAME(cg, "append_fallback"));
  LLVMBasicBlockRef slow_end = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "append_result"));
  LLVMValueRef incoming_vals[2] = {args[0], slow_res};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_end, slow_end};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef ny_try_emit_direct_list_ctor(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 expr_call_t *c,
                                                 const char *builtin_name,
                                                 bool builtin_shadowed) {
  if (!cg || !e || !c || !builtin_name)
    return NULL;
  if (!ny_gencall_builtin_name_is(builtin_name, "list", builtin_shadowed))
    return NULL;
  if (c->args.len > 1)
    return NULL;
  fun_sig *list_new = lookup_fun(cg, "__list_new", 0);
  if (!list_new || !list_new->type || !list_new->value)
    return NULL;
  LLVMValueRef cap = NULL;
  if (c->args.len == 0) {
    cap = LLVMConstInt(cg->type_i64, (((uint64_t)8) << 1) | 1u, false);
  } else {
    cap = gen_expr(cg, scopes, depth, c->args.data[0].val);
  }
  if (!cap) {
    ny_diag_error(e->tok, "failed to evaluate list(...) capacity");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  cap = ny_cast_to_i64(cg, cap, "list_cap");
  ny_dbg_loc(cg, e->tok);
  return LLVMBuildCall2(cg->builder, list_new->type, list_new->value, &cap, 1,
                        NY_LLVM_NAME(cg, "list_direct"));
}

static bool ny_expr_is_literal_int_list(expr_t *e) {
  if (!ny_expr_is_list_or_tuple_lit(e))
    return false;
  for (size_t i = 0; i < e->as.list_like.len; ++i) {
    int64_t ignored = 0;
    if (!ny_expr_literal_i64(e->as.list_like.data[i], &ignored))
      return false;
  }
  return true;
}

static expr_t *ny_binding_flow_static_int_list_init(binding *b) {
  if (!b || b->static_indexable_invalid || !b->is_int_list_storage)
    return NULL;
  if (b->escapes && !b->static_indexable_object_elided)
    return NULL;
  expr_t *init = ny_binding_var_init_expr(b, b->name);
  return ny_expr_is_literal_int_list(init) ? init : NULL;
}

static binding *ny_static_int_list_target_binding(codegen_t *cg, scope *scopes,
                                                  size_t depth,
                                                  expr_t *target) {
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return NULL;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  return ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                   name_len, target->as.ident.hash);
}

static bool ny_static_int_list_target_object_elided(codegen_t *cg,
                                                    scope *scopes, size_t depth,
                                                    expr_t *target) {
  binding *b = ny_static_int_list_target_binding(cg, scopes, depth, target);
  return b && b->static_indexable_object_elided;
}

static bool ny_static_int_list_in_std_origin(codegen_t *cg) {
  const char *mod = cg ? cg->current_module_name : NULL;
  return mod && (strncmp(mod, "std.", 4) == 0 || strncmp(mod, "lib.", 4) == 0);
}

static LLVMValueRef ny_static_int_list_global(codegen_t *cg, binding *b,
                                              expr_t *init,
                                              LLVMTypeRef *out_array_ty) {
  if (!cg || !b || !init || !ny_expr_is_literal_int_list(init))
    return NULL;
  size_t len = init->as.list_like.len;
  LLVMTypeRef array_ty = LLVMArrayType(cg->type_i64, (unsigned)(len ? len : 1));
  if (out_array_ty)
    *out_array_ty = array_ty;
  if (b->static_int_list_global && b->static_int_list_len == len)
    return b->static_int_list_global;

  bool untagged =
      ny_fast_path_enabled(cg, "NYTRIX_UNTAGGED_INT_LIST_STORAGE");
  LLVMValueRef *vals = len ? (LLVMValueRef *)alloca(sizeof(LLVMValueRef) * len)
                           : (LLVMValueRef *)alloca(sizeof(LLVMValueRef));
  for (size_t i = 0; i < (len ? len : 1); ++i) {
    int64_t raw = 0;
    if (i < len)
      (void)ny_expr_literal_i64(init->as.list_like.data[i], &raw);
    uint64_t v = untagged ? (uint64_t)raw : (((uint64_t)raw) << 1) | 1u;
    vals[i] = LLVMConstInt(cg->type_i64, v, false);
  }

  char name[96];
  snprintf(name, sizeof(name), "__ny_static_int_list_%d",
           cg->static_int_list_count++);
  LLVMValueRef g = LLVMAddGlobal(cg->module, array_ty, name);
  LLVMSetInitializer(
      g, LLVMConstArray(cg->type_i64, vals, (unsigned)(len ? len : 1)));
  LLVMSetGlobalConstant(g, true);
  LLVMSetLinkage(g, LLVMPrivateLinkage);
  LLVMSetUnnamedAddr(g, true);
  if (ny_static_int_list_in_std_origin(cg))
#ifdef __APPLE__
    LLVMSetSection(g, "__DATA,ny_std");
#else
    LLVMSetSection(g, "ny.std");
#endif
  b->static_int_list_global = g;
  b->static_int_list_len = len;
  b->static_int_list_untagged = untagged;
  if (untagged && cg->module) {
    LLVMMetadataRef s =
        LLVMMDStringInContext2(cg->ctx, "static_int_list_untagged", 24);
    LLVMMetadataRef md = LLVMMDNodeInContext2(cg->ctx, &s, 1);
    LLVMAddNamedMetadataOperand(cg->module, "nytrix.untagged_list",
                                LLVMMetadataAsValue(cg->ctx, md));
  }
  return g;
}

static LLVMValueRef ny_try_emit_static_int_list_get(
    codegen_t *cg, scope *scopes, size_t depth, expr_t *target, expr_t *key,
    expr_t *default_expr, token_t tok, bool assume_nonnegative,
    bool assume_in_bounds) {
  binding *b = ny_static_int_list_target_binding(cg, scopes, depth, target);
  if (b && b->is_mut && !ny_env_enabled("NYTRIX_STATIC_INT_LIST_GET"))
    return NULL;
  expr_t *init = ny_binding_flow_static_int_list_init(b);
  if (!init)
    return NULL;
  LLVMTypeRef array_ty = NULL;
  LLVMValueRef g = ny_static_int_list_global(cg, b, init, &array_ty);
  if (!g || !array_ty)
    return NULL;

  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef default_v = NULL;
  if (!assume_in_bounds)
    default_v =
        default_expr ? gen_expr(cg, scopes, depth, default_expr) : ny_c1(cg);
  if (!key_v || (!assume_in_bounds && !default_v)) {
    ny_diag_error(tok, "failed to evaluate static int-list get(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  key_v = ny_cast_to_i64(cg, key_v, "static_get_key");
  LLVMValueRef key_raw = ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                                                  "static_get_key_raw");

  ny_dbg_loc(cg, tok);
  size_t len = init->as.list_like.len;
  if (assume_in_bounds) {
    LLVMValueRef idxs[2] = {ny_c0(cg), key_raw};
    LLVMValueRef elem_ptr = LLVMBuildInBoundsGEP2(
        cg->builder, array_ty, g, idxs, 2, "static_get_elem_ptr");
    LLVMValueRef val =
        ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "static_get_elem"));
    return b->static_int_list_untagged ? ny_tag_int(cg, val) : val;
  }

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "static_get.load");
  LLVMBasicBlockRef default_bb = ny_bb_fn(fn, "static_get.default");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "static_get.join");

  LLVMValueRef len_raw = LLVMConstInt(cg->type_i64, (uint64_t)len, false);
  LLVMValueRef adj_idx = key_raw;
  LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (!assume_nonnegative) {
    LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                        ny_c0(cg), "static_get_is_neg");
    LLVMValueRef wrapped =
        ny_add(cg, key_raw, len_raw, "static_get_wrapped_idx");
    adj_idx = ny_select(cg, is_neg, wrapped, key_raw, "static_get_adj_idx");
    low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx, ny_c0(cg),
                           "static_get_low_ok");
  }
  LLVMValueRef high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx,
                                       len_raw, "static_get_hi_ok");
  LLVMValueRef in_bounds = ny_and(cg, low_ok, high_ok, "static_get_in_bounds");
  ny_cond_br(cg, in_bounds, load_bb, default_bb);

  ny_pos(cg, load_bb);
  LLVMValueRef idx_for_load[2] = {ny_c0(cg), adj_idx};
  LLVMValueRef elem_ptr = LLVMBuildInBoundsGEP2(
      cg->builder, array_ty, g, idx_for_load, 2, "static_get_elem_ptr");
  LLVMValueRef elem_val =
      ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "static_get_elem"));
  if (b->static_int_list_untagged)
    elem_val = ny_tag_int(cg, elem_val);
  LLVMBasicBlockRef load_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, default_bb);
  LLVMBasicBlockRef default_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "static_get_result"));
  LLVMValueRef incoming_vals[2] = {elem_val, default_v};
  LLVMBasicBlockRef incoming_bbs[2] = {load_end_bb, default_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static binding *ny_raw_int_list_target_binding(codegen_t *cg, scope *scopes,
                                               size_t depth, expr_t *target) {
  binding *b = ny_static_int_list_target_binding(cg, scopes, depth, target);
  return (b && b->raw_int_list_mutation && b->raw_int_list_ptr &&
          b->raw_int_list_len > 0)
             ? b
             : NULL;
}

static binding *ny_f64_list_target_binding(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *target) {
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return NULL;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b =
      ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                name_len, target->as.ident.hash);
  return (b && b->is_f64_list_storage) ? b : NULL;
}

static LLVMValueRef
ny_try_emit_raw_int_list_get(codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *target, expr_t *key, expr_t *default_expr,
                             token_t tok, bool assume_nonnegative,
                             bool assume_in_bounds) {
  binding *b = ny_raw_int_list_target_binding(cg, scopes, depth, target);
  if (!b)
    return NULL;
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef default_v = NULL;
  if (!assume_in_bounds)
    default_v =
        default_expr ? gen_expr(cg, scopes, depth, default_expr) : ny_c1(cg);
  if (!key_v || (!assume_in_bounds && !default_v)) {
    ny_diag_error(tok, "failed to evaluate raw int-list get(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  key_v = ny_cast_to_i64(cg, key_v, "raw_list_get_key");
  LLVMValueRef key_raw = ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                                                  "raw_list_get_key_raw");
  if (assume_in_bounds) {
    LLVMValueRef elem_ptr =
        LLVMBuildInBoundsGEP2(cg->builder, cg->type_i64, b->raw_int_list_ptr,
                              &key_raw, 1, "raw_int_list_get_inbounds_ptr");
    LLVMValueRef val =
        ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "raw_int_list_get_inbounds"));
    return b->raw_int_list_untagged ? ny_tag_int(cg, val) : val;
  }

  LLVMValueRef nonneg = assume_nonnegative
                            ? LLVMConstInt(cg->type_i1, 1, false)
                            : LLVMBuildICmp(cg->builder, LLVMIntSGE, key_raw,
                                            ny_c0(cg), "raw_list_get_nonneg");
  LLVMValueRef in_hi = LLVMBuildICmp(
      cg->builder, LLVMIntULT, key_raw,
      LLVMConstInt(cg->type_i64, (uint64_t)b->raw_int_list_len, false),
      "raw_list_get_hi");
  LLVMValueRef in_bounds =
      LLVMBuildAnd(cg->builder, nonneg, in_hi, "raw_list_get_inbounds");
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef ok_bb = ny_bb_fn(fn, "raw_list_get.ok");
  LLVMBasicBlockRef def_bb = ny_bb_fn(fn, "raw_list_get.default");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "raw_list_get.join");
  ny_cond_br(cg, in_bounds, ok_bb, def_bb);

  ny_pos(cg, ok_bb);
  LLVMValueRef elem_ptr =
      LLVMBuildInBoundsGEP2(cg->builder, cg->type_i64, b->raw_int_list_ptr,
                            &key_raw, 1, "raw_int_list_get_ptr");
  LLVMValueRef loaded =
      ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "raw_int_list_get"));
  if (b->raw_int_list_untagged)
    loaded = ny_tag_int(cg, loaded);
  LLVMBasicBlockRef ok_end = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, def_bb);
  LLVMBasicBlockRef def_end = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "raw_int_list_get_result"));
  LLVMValueRef vals[2] = {loaded, default_v};
  LLVMBasicBlockRef bbs[2] = {ok_end, def_end};
  LLVMAddIncoming(phi, vals, bbs, 2);
  return phi;
}

static LLVMValueRef ny_try_emit_raw_int_list_set(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *target,
                                                 expr_t *key, expr_t *value,
                                                 token_t tok,
                                                 bool assume_nonnegative,
                                                 bool assume_in_bounds) {
  binding *b = ny_raw_int_list_target_binding(cg, scopes, depth, target);
  if (!b)
    return NULL;
  if (b->raw_int_list_untagged &&
      !ny_is_proven_int(cg, scopes, depth, value, NULL))
    return NULL;
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef val_v = gen_expr(cg, scopes, depth, value);
  if (!key_v || !val_v) {
    ny_diag_error(tok, "failed to evaluate raw int-list set_idx(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  key_v = ny_cast_to_i64(cg, key_v, "raw_list_set_key");
  val_v = ny_cast_to_i64(cg, val_v, "raw_list_set_value");
  LLVMValueRef key_raw = ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                                                  "raw_list_set_key_raw");
  LLVMValueRef ok = NULL;
  if (!assume_in_bounds) {
    LLVMValueRef nonneg = assume_nonnegative
                              ? LLVMConstInt(cg->type_i1, 1, false)
                              : LLVMBuildICmp(cg->builder, LLVMIntSGE, key_raw,
                                              ny_c0(cg), "raw_list_set_nonneg");
    LLVMValueRef in_hi = LLVMBuildICmp(
        cg->builder, LLVMIntULT, key_raw,
        LLVMConstInt(cg->type_i64, (uint64_t)b->raw_int_list_len, false),
        "raw_list_set_hi");
    ok = LLVMBuildAnd(cg->builder, nonneg, in_hi, "raw_list_set_inbounds");
  }
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef store_bb = NULL;
  LLVMBasicBlockRef panic_bb = NULL;
  LLVMBasicBlockRef join_bb = NULL;
  if (!assume_in_bounds) {
    store_bb = ny_bb_fn(fn, "raw_list_set.store");
    panic_bb = ny_bb_fn(fn, "raw_list_set.panic");
    join_bb = ny_bb_fn(fn, "raw_list_set.join");
    ny_cond_br(cg, ok, store_bb, panic_bb);
    ny_pos(cg, store_bb);
  }
  LLVMValueRef elem_ptr = LLVMBuildInBoundsGEP2(
      cg->builder, cg->type_i64, b->raw_int_list_ptr, &key_raw, 1,
      assume_in_bounds ? "raw_int_list_set_inbounds_ptr"
                       : "raw_int_list_set_ptr");
  LLVMValueRef val_to_store =
      b->raw_int_list_untagged ? ny_untag_int(cg, val_v) : val_v;
  ny_store(cg, elem_ptr, val_to_store);

  if (!assume_in_bounds) {
    ny_br(cg, join_bb);
    ny_pos(cg, panic_bb);
    fun_sig *panic_sig = lookup_fun(cg, "__panic", 0);
    if (panic_sig) {
      const char *msg = "set index out of range";
      LLVMValueRef msg_global = const_string_ptr(cg, msg, strlen(msg));
      LLVMValueRef msg_ptr = ny_load(cg, msg_global, "raw_list_set_panic_msg");
      LLVMBuildCall2(cg->builder, panic_sig->type, panic_sig->value,
                     (LLVMValueRef[]){msg_ptr}, 1, "");
    }
    LLVMBuildUnreachable(cg->builder);
    ny_pos(cg, join_bb);
  }
  return ny_c0(cg);
}

static bool ny_expr_has_known_dict_type(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *target) {
  if (ny_gencall_type_is(infer_expr_type(cg, scopes, depth, target), "dict"))
    return true;
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b =
      ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                name_len, target->as.ident.hash);
  return ny_binding_dict_storage_init(b) != NULL;
}

static bool ny_expr_is_direct_dict_storage(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *target) {
  if (ny_expr_is_dict_ctor(target))
    return true;
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  size_t name_len = (size_t)target->tok.len;
  if (name_len == 0)
    name_len = strlen(target->as.ident.name);
  binding *b =
      ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                name_len, target->as.ident.hash);
  return ny_binding_dict_storage_init(b) != NULL;
}

static LLVMValueRef ny_emit_trusted_fast_indexable_get(
    codegen_t *cg, scope *scopes, size_t depth, expr_t *target, expr_t *key,
    expr_t *default_expr, token_t tok, bool assume_nonnegative,
    bool assume_in_bounds) {
  LLVMValueRef target_v = gen_expr(cg, scopes, depth, target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef default_v =
      default_expr ? gen_expr(cg, scopes, depth, default_expr) : ny_c1(cg);
  if (!target_v || !key_v || !default_v) {
    ny_diag_error(tok, "failed to evaluate trusted fast get(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  target_v = ny_cast_to_i64(cg, target_v, "trusted_get_target");
  key_v = ny_cast_to_i64(cg, key_v, "trusted_get_key");
  default_v = ny_cast_to_i64(cg, default_v, "trusted_get_default");

  ny_dbg_loc(cg, tok);
  if (assume_in_bounds) {
    LLVMValueRef key_raw =
        ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                                 "trusted_get_key_raw");
    LLVMValueRef scaled =
        LLVMBuildShl(cg->builder, key_raw, LLVMConstInt(cg->type_i64, 3, false),
                     "trusted_get_inbounds_scaled");
    LLVMValueRef byte_off =
        LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                     "trusted_get_inbounds_off");
    LLVMValueRef elem_addr = ny_add(
        cg, target_v, byte_off, NY_LLVM_NAME(cg, "trusted_get_inbounds_addr"));
    LLVMValueRef elem_ptr =
        LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                          "trusted_get_inbounds_elem_ptr_i64");
    return ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "trusted_get_inbounds_elem"));
  }

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef load_bb = ny_bb_fn(fn, "trusted_get.load");
  LLVMBasicBlockRef default_bb = ny_bb_fn(fn, "trusted_get.default");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "trusted_get.join");

  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "trusted_get_ptr_i64");
  LLVMValueRef len_tagged =
      ny_load(cg, target_ptr, NY_LLVM_NAME(cg, "trusted_get_len"));
  LLVMValueRef len_raw = ny_untag_int(cg, len_tagged);
  LLVMValueRef key_raw =
      ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                               "trusted_get_key_raw");

  LLVMValueRef adj_idx = key_raw;
  LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (!assume_nonnegative) {
    LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                        LLVMConstInt(cg->type_i64, 0, false),
                                        NY_LLVM_NAME(cg, "trusted_get_is_neg"));
    LLVMValueRef wrapped = ny_add(cg, key_raw, len_raw,
                                  NY_LLVM_NAME(cg, "trusted_get_wrapped_idx"));
    adj_idx = ny_select(cg, is_neg, wrapped, key_raw,
                        NY_LLVM_NAME(cg, "trusted_get_adj_idx"));
    low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx,
                           LLVMConstInt(cg->type_i64, 0, false),
                           NY_LLVM_NAME(cg, "trusted_get_low_ok"));
  }
  LLVMValueRef high_ok = NULL;
  if (assume_nonnegative && ny_is_proven_int(cg, scopes, depth, key, key_v)) {
    high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_v, len_tagged,
                            NY_LLVM_NAME(cg, "trusted_get_hi_ok_tagged"));
  } else {
    high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx, len_raw,
                            NY_LLVM_NAME(cg, "trusted_get_hi_ok"));
  }
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, NY_LLVM_NAME(cg, "trusted_get_in_bounds"));
  ny_cond_br(cg, in_bounds, load_bb, default_bb);

  ny_pos(cg, load_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_idx, LLVMConstInt(cg->type_i64, 3, false),
                   "trusted_get_scaled");
  LLVMValueRef byte_off =
      LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                   "trusted_get_off");
  LLVMValueRef elem_addr =
      ny_add(cg, target_v, byte_off, NY_LLVM_NAME(cg, "trusted_get_addr"));
  LLVMValueRef elem_ptr = LLVMBuildIntToPtr(
      cg->builder, elem_addr, ny_ptr_i64_ty(cg), "trusted_get_elem_ptr_i64");
  LLVMValueRef elem_val =
      ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "trusted_get_elem"));
  LLVMBasicBlockRef load_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, default_bb);
  LLVMBasicBlockRef default_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "trusted_get_result"));
  LLVMValueRef incoming_vals[2] = {elem_val, default_v};
  LLVMBasicBlockRef incoming_bbs[2] = {load_end_bb, default_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef ny_emit_fast_list_set(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *target,
                                          expr_t *key, expr_t *value_expr,
                                          token_t tok, fun_sig *fallback_sig,
                                          bool assume_nonnegative) {
  LLVMValueRef target_v = gen_expr(cg, scopes, depth, target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef value_v = gen_expr(cg, scopes, depth, value_expr);
  if (!target_v || !key_v || !value_v) {
    ny_diag_error(tok, "failed to evaluate fast set_idx(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  target_v = ny_cast_to_i64(cg, target_v, "fast_set_target");
  key_v = ny_cast_to_i64(cg, key_v, "fast_set_key");
  value_v = ny_cast_to_i64(cg, value_v, "fast_set_value");

  ny_dbg_loc(cg, tok);
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef tag_bb = ny_bb_fn(fn, "fast_set.tag");
  LLVMBasicBlockRef bounds_bb = ny_bb_fn(fn, "fast_set.bounds");
  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "fast_set.ok");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "fast_set.slow");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "fast_set.join");

  LLVMValueRef is_ptr_pred = ny_build_is_ptr_pred(cg, target_v, "fast_set_ptr");
  ny_cond_br(cg, is_ptr_pred, tag_bb, slow_bb);

  ny_pos(cg, tag_bb);
  LLVMValueRef tag_addr = ny_sub(
      cg, target_v, LLVMConstInt(cg->type_i64, 8, false), "fast_set_tag_addr");
  LLVMValueRef tag_ptr = LLVMBuildIntToPtr(
      cg->builder, tag_addr, ny_ptr_i64_ty(cg), "fast_set_tag_ptr");
  LLVMValueRef tag_v = ny_load(cg, tag_ptr, NY_LLVM_NAME(cg, "fast_set_tag"));
  LLVMValueRef is_list = LLVMBuildICmp(cg->builder, LLVMIntEQ, tag_v,
                                       LLVMConstInt(cg->type_i64, 100, false),
                                       NY_LLVM_NAME(cg, "fast_set_is_list"));
  ny_cond_br(cg, is_list, bounds_bb, slow_bb);

  ny_pos(cg, bounds_bb);
  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "fast_set_ptr_i64");
  LLVMValueRef len_tagged =
      ny_load(cg, target_ptr, NY_LLVM_NAME(cg, "fast_set_len"));
  LLVMValueRef len_raw = ny_untag_int(cg, len_tagged);
  LLVMValueRef cap_addr = ny_add(
      cg, target_v, LLVMConstInt(cg->type_i64, 8, false), "fast_set_cap_addr");
  LLVMValueRef cap_ptr = LLVMBuildIntToPtr(
      cg->builder, cap_addr, ny_ptr_i64_ty(cg), "fast_set_cap_ptr_i64");
  LLVMValueRef cap_tagged =
      ny_load(cg, cap_ptr, NY_LLVM_NAME(cg, "fast_set_cap"));
  LLVMValueRef cap_raw =
      ny_build_untagged_or_raw_i64(cg, cap_tagged, "fast_set_cap_raw");
  LLVMValueRef key_raw =
      ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                               "fast_set_key_raw");

  LLVMValueRef adj_idx = key_raw;
  LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (!assume_nonnegative) {
    LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                        LLVMConstInt(cg->type_i64, 0, false),
                                        NY_LLVM_NAME(cg, "fast_set_is_neg"));
    LLVMValueRef wrapped =
        ny_add(cg, key_raw, len_raw, NY_LLVM_NAME(cg, "fast_set_wrapped_idx"));
    adj_idx = ny_select(cg, is_neg, wrapped, key_raw,
                        NY_LLVM_NAME(cg, "fast_set_adj_idx"));
    low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx,
                           LLVMConstInt(cg->type_i64, 0, false),
                           NY_LLVM_NAME(cg, "fast_set_low_ok"));
  }
  LLVMValueRef high_ok =
      LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx, cap_raw,
                    NY_LLVM_NAME(cg, "fast_set_hi_ok"));
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, NY_LLVM_NAME(cg, "fast_set_in_bounds"));

  ny_cond_br(cg, in_bounds, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_idx, LLVMConstInt(cg->type_i64, 3, false),
                   "fast_set_scaled");
  LLVMValueRef byte_off =
      LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                   "fast_set_off");
  LLVMValueRef elem_addr =
      ny_add(cg, target_v, byte_off, NY_LLVM_NAME(cg, "fast_set_addr"));
  LLVMValueRef elem_ptr = LLVMBuildIntToPtr(
      cg->builder, elem_addr, ny_ptr_i64_ty(cg), "fast_set_elem_ptr_i64");
  ny_store(cg, elem_ptr, value_v);
  LLVMValueRef need_grow =
      LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx, len_raw,
                    NY_LLVM_NAME(cg, "fast_set_need_grow"));
  LLVMValueRef grown_len =
      ny_add(cg, adj_idx, LLVMConstInt(cg->type_i64, 1, false),
             NY_LLVM_NAME(cg, "fast_set_grown_len"));
  LLVMValueRef next_len = ny_select(cg, need_grow, grown_len, len_raw,
                                    NY_LLVM_NAME(cg, "fast_set_next_len"));
  LLVMValueRef next_len_tagged =
      ny_or(cg,
            ny_shl(cg, next_len, ny_c1(cg),
                   NY_LLVM_NAME(cg, "fast_set_next_len_shl")),
            ny_c1(cg), NY_LLVM_NAME(cg, "fast_set_next_len_tagged"));
  ny_store(cg, target_ptr, next_len_tagged);
  LLVMBasicBlockRef fast_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_res = ny_c0(cg);
  if (fallback_sig) {
    LLVMValueRef args[3] = {target_v, key_v, value_v};
    ny_dbg_loc(cg, tok);
    slow_res =
        LLVMBuildCall2(cg->builder, fallback_sig->type, fallback_sig->value,
                       args, 3, NY_LLVM_NAME(cg, "fast_set_fallback"));
  }
  LLVMBasicBlockRef slow_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_set_result"));
  LLVMValueRef incoming_vals[2] = {target_v, slow_res};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_end_bb, slow_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef ny_emit_trusted_fast_list_set(
    codegen_t *cg, scope *scopes, size_t depth, expr_t *target, expr_t *key,
    expr_t *value_expr, token_t tok, fun_sig *fallback_sig,
    bool assume_nonnegative, bool assume_existing_index) {
  LLVMValueRef target_v = gen_expr(cg, scopes, depth, target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef value_v = gen_expr(cg, scopes, depth, value_expr);
  if (!target_v || !key_v || !value_v) {
    ny_diag_error(tok, "failed to evaluate trusted fast set_idx(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  target_v = ny_cast_to_i64(cg, target_v, "trusted_set_target");
  key_v = ny_cast_to_i64(cg, key_v, "trusted_set_key");
  value_v = ny_cast_to_i64(cg, value_v, "trusted_set_value");

  ny_dbg_loc(cg, tok);
  if (assume_existing_index) {
    LLVMValueRef key_raw =
        ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                                 "trusted_set_key_raw");
    LLVMValueRef scaled =
        LLVMBuildShl(cg->builder, key_raw, LLVMConstInt(cg->type_i64, 3, false),
                     "trusted_set_inbounds_scaled");
    LLVMValueRef byte_off =
        LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                     "trusted_set_inbounds_off");
    LLVMValueRef elem_addr = ny_add(
        cg, target_v, byte_off, NY_LLVM_NAME(cg, "trusted_set_inbounds_addr"));
    LLVMValueRef elem_ptr =
        LLVMBuildIntToPtr(cg->builder, elem_addr, ny_ptr_i64_ty(cg),
                          "trusted_set_inbounds_elem_ptr_i64");
    ny_store(cg, elem_ptr, value_v);
    return target_v;
  }

  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "trusted_set.ok");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "trusted_set.slow");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "trusted_set.join");

  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "trusted_set_ptr_i64");
  LLVMValueRef len_tagged =
      ny_load(cg, target_ptr, NY_LLVM_NAME(cg, "trusted_set_len"));
  LLVMValueRef len_raw =
      ny_build_untagged_or_raw_i64(cg, len_tagged, "trusted_set_len_raw");
  LLVMValueRef cap_addr =
      ny_add(cg, target_v, LLVMConstInt(cg->type_i64, 8, false),
             "trusted_set_cap_addr");
  LLVMValueRef cap_ptr = LLVMBuildIntToPtr(
      cg->builder, cap_addr, ny_ptr_i64_ty(cg), "trusted_set_cap_ptr_i64");
  LLVMValueRef cap_tagged =
      ny_load(cg, cap_ptr, NY_LLVM_NAME(cg, "trusted_set_cap"));
  LLVMValueRef cap_raw =
      ny_build_untagged_or_raw_i64(cg, cap_tagged, "trusted_set_cap_raw");
  LLVMValueRef key_raw =
      ny_gencall_index_raw_i64(cg, scopes, depth, key, key_v,
                               "trusted_set_key_raw");

  LLVMValueRef adj_idx = key_raw;
  LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (!assume_nonnegative) {
    LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                        LLVMConstInt(cg->type_i64, 0, false),
                                        NY_LLVM_NAME(cg, "trusted_set_is_neg"));
    LLVMValueRef wrapped = ny_add(cg, key_raw, len_raw,
                                  NY_LLVM_NAME(cg, "trusted_set_wrapped_idx"));
    adj_idx = ny_select(cg, is_neg, wrapped, key_raw,
                        NY_LLVM_NAME(cg, "trusted_set_adj_idx"));
    low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx,
                           LLVMConstInt(cg->type_i64, 0, false),
                           NY_LLVM_NAME(cg, "trusted_set_low_ok"));
  }
  LLVMValueRef high_ok =
      LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx, cap_raw,
                    NY_LLVM_NAME(cg, "trusted_set_hi_ok"));
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, NY_LLVM_NAME(cg, "trusted_set_in_bounds"));
  ny_cond_br(cg, in_bounds, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_idx, LLVMConstInt(cg->type_i64, 3, false),
                   "trusted_set_scaled");
  LLVMValueRef byte_off =
      LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                   "trusted_set_off");
  LLVMValueRef elem_addr =
      ny_add(cg, target_v, byte_off, NY_LLVM_NAME(cg, "trusted_set_addr"));
  LLVMValueRef elem_ptr = LLVMBuildIntToPtr(
      cg->builder, elem_addr, ny_ptr_i64_ty(cg), "trusted_set_elem_ptr_i64");
  ny_store(cg, elem_ptr, value_v);
  LLVMValueRef need_grow =
      LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx, len_raw,
                    NY_LLVM_NAME(cg, "trusted_set_need_grow"));
  LLVMValueRef grown_len =
      ny_add(cg, adj_idx, LLVMConstInt(cg->type_i64, 1, false),
             NY_LLVM_NAME(cg, "trusted_set_grown_len"));
  LLVMValueRef next_len = ny_select(cg, need_grow, grown_len, len_raw,
                                    NY_LLVM_NAME(cg, "trusted_set_next_len"));
  LLVMValueRef next_len_tagged =
      ny_or(cg,
            ny_shl(cg, next_len, ny_c1(cg),
                   NY_LLVM_NAME(cg, "trusted_set_next_len_shl")),
            ny_c1(cg), NY_LLVM_NAME(cg, "trusted_set_next_len_tagged"));
  ny_store(cg, target_ptr, next_len_tagged);
  LLVMBasicBlockRef fast_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_res = target_v;
  if (fallback_sig) {
    LLVMValueRef args[3] = {target_v, key_v, value_v};
    ny_dbg_loc(cg, tok);
    slow_res =
        LLVMBuildCall2(cg->builder, fallback_sig->type, fallback_sig->value,
                       args, 3, NY_LLVM_NAME(cg, "trusted_set_fallback"));
  }
  LLVMBasicBlockRef slow_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "trusted_set_result"));
  LLVMValueRef incoming_vals[2] = {target_v, slow_res};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_end_bb, slow_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef
ny_emit_fast_indexable_get(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *target, expr_t *key, expr_t *default_expr,
                           token_t tok, bool assume_nonnegative);
static LLVMValueRef ny_emit_fast_int_dict_get(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *target,
                                              expr_t *key,
                                              expr_t *default_expr,
                                              token_t tok,
                                              fun_sig *fallback_sig);

static LLVMValueRef ny_try_emit_fast_receiver_get(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e,
                                                  expr_t *target,
                                                  ny_call_arg_list *args) {
  if (!cg || !e || !target || !args || (args->len != 1 && args->len != 2))
    return NULL;
  if (ny_env_enabled("NYTRIX_DISABLE_FAST_RECEIVER_GET"))
    return NULL;
  expr_t *key = args->data[0].val;
  expr_t *default_expr = (args->len == 2) ? args->data[1].val : NULL;

  bool target_is_direct_dict =
      ny_expr_is_direct_dict_storage(cg, scopes, depth, target);
  bool target_is_known_dict =
      target_is_direct_dict ||
      ny_expr_has_known_dict_type(cg, scopes, depth, target);
  if (target_is_known_dict) {
    fun_sig *dict_get_sig = lookup_fun(cg, "std.core.dict_mod.dict_read", 0);
    if (!dict_get_sig)
      return NULL;
    if (ny_gencall_expr_is_int_index(cg, scopes, depth, key) &&
        !ny_env_enabled("NYTRIX_GUARDED_FAST_DICT_GET") &&
        ny_env_enabled_default_on("NYTRIX_FAST_INT_DICT_GET")) {
      return ny_emit_fast_int_dict_get(cg, scopes, depth, target, key,
                                       default_expr, e->tok, dict_get_sig);
    }
    LLVMValueRef call_args[3];
    call_args[0] =
        ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, target), "dict_get_target");
    call_args[1] =
        ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, key), "dict_get_key");
    call_args[2] =
        default_expr
            ? ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, default_expr),
                             "dict_get_default")
            : ny_c1(cg);
    if (!call_args[0] || !call_args[1] || !call_args[2]) {
      ny_diag_error(e->tok, "failed to evaluate dict get(...) arguments");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, dict_get_sig->type,
                          dict_get_sig->value, call_args, 3,
                          NY_LLVM_NAME(cg, "dict_get_receiver_direct"));
  }

  if (!ny_gencall_expr_is_int_index(cg, scopes, depth, key))
    return NULL;
  bool target_is_direct_list =
      ny_expr_is_direct_list_storage(cg, scopes, depth, target);
  bool target_is_raw_int_list =
      ny_raw_int_list_target_binding(cg, scopes, depth, target) != NULL;
  bool target_is_known_list_like =
      ny_expr_has_known_list_like_type(cg, scopes, depth, target);
  if (!target_is_raw_int_list && !target_is_direct_list &&
      !target_is_known_list_like)
    return NULL;

  bool assume_nonnegative =
      ny_gencall_expr_is_safe_fast_set_index(cg, scopes, depth, key);
  bool assume_in_bounds =
      assume_nonnegative &&
      ny_gencall_expr_in_list_len_min(cg, scopes, depth, target, key);
  LLVMValueRef raw_int_list_get = ny_try_emit_raw_int_list_get(
      cg, scopes, depth, target, key, default_expr, e->tok, assume_nonnegative,
      assume_in_bounds);
  if (raw_int_list_get)
    return raw_int_list_get;
  LLVMValueRef static_int_list_get = ny_try_emit_static_int_list_get(
      cg, scopes, depth, target, key, default_expr, e->tok, assume_nonnegative,
      assume_in_bounds);
  if (static_int_list_get)
    return static_int_list_get;
  if (target_is_direct_list && !ny_env_enabled("NYTRIX_INDEX_READ_PARITY") &&
      !ny_env_enabled("NYTRIX_GUARDED_FAST_GET") &&
      ny_env_enabled_default_on("NYTRIX_TRUSTED_FAST_GET")) {
    return ny_emit_trusted_fast_indexable_get(cg, scopes, depth, target, key,
                                              default_expr, e->tok,
                                              assume_nonnegative,
                                              assume_in_bounds);
  }
  return ny_emit_fast_indexable_get(cg, scopes, depth, target, key,
                                    default_expr, e->tok, assume_nonnegative);
}

static LLVMValueRef ny_try_emit_fast_receiver_set(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e,
                                                  expr_t *target,
                                                  ny_call_arg_list *args) {
  if (!cg || !e || !target || !args || args->len != 2)
    return NULL;
  if (ny_env_enabled("NYTRIX_DISABLE_FAST_RECEIVER_SET"))
    return NULL;

  expr_t *key = args->data[0].val;
  expr_t *value = args->data[1].val;
  if (!key || !value)
    return NULL;

  if (ny_expr_has_known_dict_type(cg, scopes, depth, target)) {
    fun_sig *dict_set_sig = NULL;
    if (ny_env_enabled_default_on("NYTRIX_FAST_DICT_WRITE") &&
        !ny_env_enabled("NYTRIX_DISABLE_FAST_DICT_WRITE"))
      dict_set_sig = lookup_fun(cg, "__dict_write_fast", 0);
    if (!dict_set_sig)
      dict_set_sig = lookup_fun(cg, "std.core.dict_mod.dict_write", 0);
    if (!dict_set_sig)
      return NULL;
    LLVMValueRef call_args[3];
    call_args[0] =
        ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, target), "dict_set_target");
    call_args[1] =
        ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, key), "dict_set_key");
    call_args[2] =
        ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, value), "dict_set_value");
    if (!call_args[0] || !call_args[1] || !call_args[2]) {
      ny_diag_error(e->tok, "failed to evaluate dict set(...) arguments");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, dict_set_sig->type,
                          dict_set_sig->value, call_args, 3,
                          NY_LLVM_NAME(cg, "dict_set_receiver_direct"));
  }

  if (!ny_gencall_expr_is_int_index(cg, scopes, depth, key))
    return NULL;
  bool target_is_direct_list =
      ny_expr_is_direct_list_storage(cg, scopes, depth, target);
  if (!target_is_direct_list &&
      !ny_expr_has_known_list_type(cg, scopes, depth, target))
    return NULL;

  fun_sig *set_sig = lookup_fun(cg, "std.core.reflect.set", 0);
  if (!set_sig)
    set_sig = lookup_fun(cg, "std.core.set", 0);
  if (!set_sig)
    set_sig = lookup_fun(cg, "set", 0);

  bool key_is_safe_fast_index =
      ny_gencall_expr_is_safe_fast_set_index(cg, scopes, depth, key);
  bool key_is_existing_index =
      key_is_safe_fast_index &&
      ny_gencall_expr_in_list_len_min(cg, scopes, depth, target, key);
  bool target_is_trusted_f64_list =
      key_is_existing_index &&
      ny_f64_list_target_binding(cg, scopes, depth, target) != NULL;

  LLVMValueRef raw_int_list_set = ny_try_emit_raw_int_list_set(
      cg, scopes, depth, target, key, value, e->tok, key_is_safe_fast_index,
      key_is_existing_index);
  if (raw_int_list_set)
    return raw_int_list_set;
  if (ny_raw_int_list_target_binding(cg, scopes, depth, target)) {
    ny_diag_error(e->tok, "internal raw int-list proof failed for set(...)");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  if ((target_is_direct_list || target_is_trusted_f64_list) &&
      !ny_env_enabled("NYTRIX_GUARDED_FAST_SET") &&
      ny_env_enabled_default_on("NYTRIX_TRUSTED_FAST_SET")) {
    return ny_emit_trusted_fast_list_set(
        cg, scopes, depth, target, key, value, e->tok, set_sig,
        key_is_safe_fast_index, key_is_existing_index);
  }
  return ny_emit_fast_list_set(cg, scopes, depth, target, key, value, e->tok,
                               set_sig, key_is_safe_fast_index);
}

static LLVMValueRef
ny_emit_fast_indexable_get(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *target, expr_t *key, expr_t *default_expr,
                           token_t tok, bool assume_nonnegative) {
  LLVMValueRef target_v = gen_expr(cg, scopes, depth, target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef default_v =
      default_expr ? gen_expr(cg, scopes, depth, default_expr) : ny_c1(cg);
  if (!target_v || !key_v || !default_v) {
    ny_diag_error(tok, "failed to evaluate fast get(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  target_v = ny_cast_to_i64(cg, target_v, "fast_get_target");
  key_v = ny_cast_to_i64(cg, key_v, "fast_get_key");
  default_v = ny_cast_to_i64(cg, default_v, "fast_get_default");

  fun_sig *fallback_sig = ny_gencall_getter(cg);
  fun_sig *probe_sig = NULL;
  if (ny_env_enabled("NYTRIX_INDEX_READ_PARITY")) {
    probe_sig = lookup_fun(cg, "__index_read_probe", 0);
    if (probe_sig && !ny_sig_in_current_sigs(cg, probe_sig))
      probe_sig = NULL;
  }

  ny_dbg_loc(cg, tok);
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef tag_bb = ny_bb_fn(fn, "fast_get.tag");
  LLVMBasicBlockRef key_bb = ny_bb_fn(fn, "fast_get.key");
  LLVMBasicBlockRef bounds_bb = ny_bb_fn(fn, "fast_get.bounds");
  LLVMBasicBlockRef fast_load_bb = ny_bb_fn(fn, "fast_get.load");
  LLVMBasicBlockRef fast_default_bb = ny_bb_fn(fn, "fast_get.default");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "fast_get.slow");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "fast_get.join");

  LLVMValueRef is_ptr_pred = ny_build_is_ptr_pred(cg, target_v, "fast_get_ptr");
  ny_cond_br(cg, is_ptr_pred, tag_bb, slow_bb);

  ny_pos(cg, tag_bb);
  LLVMValueRef tag_addr = ny_sub(
      cg, target_v, LLVMConstInt(cg->type_i64, 8, false), "fast_get_tag_addr");
  LLVMValueRef tag_ptr = LLVMBuildIntToPtr(
      cg->builder, tag_addr, ny_ptr_i64_ty(cg), "fast_get_tag_ptr");
  LLVMValueRef tag_v = ny_load(cg, tag_ptr, NY_LLVM_NAME(cg, "fast_get_tag"));
  LLVMValueRef is_list = LLVMBuildICmp(cg->builder, LLVMIntEQ, tag_v,
                                       LLVMConstInt(cg->type_i64, 100, false),
                                       NY_LLVM_NAME(cg, "fast_get_is_list"));
  LLVMValueRef is_tuple = LLVMBuildICmp(cg->builder, LLVMIntEQ, tag_v,
                                        LLVMConstInt(cg->type_i64, 103, false),
                                        NY_LLVM_NAME(cg, "fast_get_is_tuple"));
  LLVMValueRef is_seq =
      ny_or(cg, is_list, is_tuple, NY_LLVM_NAME(cg, "fast_get_is_seq"));
  ny_cond_br(cg, is_seq, key_bb, slow_bb);

  ny_pos(cg, key_bb);
  LLVMValueRef key_is_int = ny_is_tagged_int(cg, key_v);
  ny_cond_br(cg, key_is_int, bounds_bb, slow_bb);

  ny_pos(cg, bounds_bb);
  LLVMValueRef target_ptr = LLVMBuildIntToPtr(
      cg->builder, target_v, ny_ptr_i64_ty(cg), "fast_get_ptr_i64");
  LLVMValueRef len_tagged =
      ny_load(cg, target_ptr, NY_LLVM_NAME(cg, "fast_get_len"));
  LLVMValueRef len_raw = ny_untag_int(cg, len_tagged);
  LLVMValueRef key_raw = ny_untag_int(cg, key_v);

  LLVMValueRef adj_idx = key_raw;
  LLVMValueRef low_ok = LLVMConstInt(cg->type_i1, 1, false);
  if (!assume_nonnegative) {
    LLVMValueRef is_neg = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_raw,
                                        LLVMConstInt(cg->type_i64, 0, false),
                                        NY_LLVM_NAME(cg, "fast_get_is_neg"));
    LLVMValueRef wrapped =
        ny_add(cg, key_raw, len_raw, NY_LLVM_NAME(cg, "fast_get_wrapped_idx"));
    adj_idx = ny_select(cg, is_neg, wrapped, key_raw,
                        NY_LLVM_NAME(cg, "fast_get_adj_idx"));
    low_ok = LLVMBuildICmp(cg->builder, LLVMIntSGE, adj_idx,
                           LLVMConstInt(cg->type_i64, 0, false),
                           NY_LLVM_NAME(cg, "fast_get_low_ok"));
  }
  LLVMValueRef high_ok = NULL;
  if (assume_nonnegative && ny_is_proven_int(cg, scopes, depth, key, key_v)) {
    high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, key_v, len_tagged,
                            NY_LLVM_NAME(cg, "fast_get_hi_ok_tagged"));
  } else {
    high_ok = LLVMBuildICmp(cg->builder, LLVMIntSLT, adj_idx, len_raw,
                            NY_LLVM_NAME(cg, "fast_get_hi_ok"));
  }
  LLVMValueRef in_bounds =
      ny_and(cg, low_ok, high_ok, NY_LLVM_NAME(cg, "fast_get_in_bounds"));
  ny_cond_br(cg, in_bounds, fast_load_bb, fast_default_bb);

  ny_pos(cg, fast_load_bb);
  LLVMValueRef scaled =
      LLVMBuildShl(cg->builder, adj_idx, LLVMConstInt(cg->type_i64, 3, false),
                   "fast_get_scaled");
  LLVMValueRef byte_off =
      LLVMBuildAdd(cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
                   "fast_get_off");
  LLVMValueRef elem_addr =
      ny_add(cg, target_v, byte_off, NY_LLVM_NAME(cg, "fast_get_addr"));
  LLVMValueRef elem_ptr = LLVMBuildIntToPtr(
      cg->builder, elem_addr, ny_ptr_i64_ty(cg), "fast_get_elem_ptr_i64");
  LLVMValueRef elem_val =
      ny_load(cg, elem_ptr, NY_LLVM_NAME(cg, "fast_get_elem"));
  if (probe_sig) {
    ny_dbg_loc(cg, tok);
    LLVMValueRef probe_args[3] = {
        tag_v,
        key_v,
        LLVMConstInt(cg->type_i64, 3, false),
    };
    (void)LLVMBuildCall2(cg->builder, probe_sig->type, probe_sig->value,
                         probe_args, 3,
                         NY_LLVM_NAME(cg, "fast_get_probe_fast"));
  }
  LLVMBasicBlockRef fast_load_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, fast_default_bb);
  if (probe_sig) {
    ny_dbg_loc(cg, tok);
    LLVMValueRef probe_args[3] = {
        tag_v,
        key_v,
        LLVMConstInt(cg->type_i64, 3, false),
    };
    (void)LLVMBuildCall2(cg->builder, probe_sig->type, probe_sig->value,
                         probe_args, 3,
                         NY_LLVM_NAME(cg, "fast_get_probe_default"));
  }
  LLVMBasicBlockRef fast_default_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_res = default_v;
  if (probe_sig) {
    ny_dbg_loc(cg, tok);
    LLVMValueRef probe_args[3] = {
        ny_c0(cg),
        key_v,
        ny_c1(cg),
    };
    (void)LLVMBuildCall2(cg->builder, probe_sig->type, probe_sig->value,
                         probe_args, 3,
                         NY_LLVM_NAME(cg, "fast_get_probe_slow"));
  }
  if (fallback_sig) {
    LLVMValueRef args[3] = {target_v, key_v, default_v};
    unsigned slow_argc = fallback_sig->arity;
    if (slow_argc < 2)
      slow_argc = 2;
    if (slow_argc > 3)
      slow_argc = 3;
    ny_dbg_loc(cg, tok);
    slow_res =
        LLVMBuildCall2(cg->builder, fallback_sig->type, fallback_sig->value,
                       args, slow_argc, NY_LLVM_NAME(cg, "fast_get_fallback"));
  }
  LLVMBasicBlockRef slow_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_get_result"));
  LLVMValueRef incoming_vals[3] = {elem_val, default_v, slow_res};
  LLVMBasicBlockRef incoming_bbs[3] = {fast_load_end_bb, fast_default_end_bb,
                                       slow_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 3);
  return phi;
}

static LLVMValueRef ny_emit_fast_int_dict_get(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *target,
                                              expr_t *key, expr_t *default_expr,
                                              token_t tok,
                                              fun_sig *fallback_sig) {
  LLVMValueRef target_v = gen_expr(cg, scopes, depth, target);
  LLVMValueRef key_v = gen_expr(cg, scopes, depth, key);
  LLVMValueRef default_v =
      default_expr ? gen_expr(cg, scopes, depth, default_expr) : ny_c1(cg);
  if (!target_v || !key_v || !default_v) {
    ny_diag_error(tok, "failed to evaluate fast dict get(...) operands");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  target_v = ny_cast_to_i64(cg, target_v, "fast_dict_target");
  key_v = ny_cast_to_i64(cg, key_v, "fast_dict_key");
  default_v = ny_cast_to_i64(cg, default_v, "fast_dict_default");

  ny_dbg_loc(cg, tok);
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef tag_bb = ny_bb_fn(fn, "fast_dict.tag");
  LLVMBasicBlockRef cap_bb = ny_bb_fn(fn, "fast_dict.cap");
  LLVMBasicBlockRef loop_bb = ny_bb_fn(fn, "fast_dict.loop");
  LLVMBasicBlockRef match_bb = ny_bb_fn(fn, "fast_dict.match");
  LLVMBasicBlockRef next_bb = ny_bb_fn(fn, "fast_dict.next");
  LLVMBasicBlockRef found_bb = ny_bb_fn(fn, "fast_dict.found");
  LLVMBasicBlockRef default_bb = ny_bb_fn(fn, "fast_dict.default");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "fast_dict.slow");
  LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "fast_dict.join");

  LLVMValueRef is_ptr_pred =
      ny_build_is_ptr_pred(cg, target_v, "fast_dict_ptr");
  ny_cond_br(cg, is_ptr_pred, tag_bb, slow_bb);

  ny_pos(cg, tag_bb);
  LLVMValueRef tag_addr = ny_sub(
      cg, target_v, LLVMConstInt(cg->type_i64, 8, false), "fast_dict_tag_addr");
  LLVMValueRef tag_ptr = LLVMBuildIntToPtr(
      cg->builder, tag_addr, ny_ptr_i64_ty(cg), "fast_dict_tag_ptr");
  LLVMValueRef tag_v = ny_load(cg, tag_ptr, NY_LLVM_NAME(cg, "fast_dict_tag"));
  LLVMValueRef is_dict = LLVMBuildICmp(cg->builder, LLVMIntEQ, tag_v,
                                       LLVMConstInt(cg->type_i64, 101, false),
                                       NY_LLVM_NAME(cg, "fast_dict_is_dict"));
  ny_cond_br(cg, is_dict, cap_bb, slow_bb);

  ny_pos(cg, cap_bb);
  LLVMValueRef cap_addr = ny_add(
      cg, target_v, LLVMConstInt(cg->type_i64, 8, false), "fast_dict_cap_addr");
  LLVMValueRef cap_ptr = LLVMBuildIntToPtr(
      cg->builder, cap_addr, ny_ptr_i64_ty(cg), "fast_dict_cap_ptr_i64");
  LLVMValueRef cap_tagged =
      ny_load(cg, cap_ptr, NY_LLVM_NAME(cg, "fast_dict_cap"));
  LLVMValueRef cap_raw =
      ny_build_untagged_or_raw_i64(cg, cap_tagged, "fast_dict_cap_raw");
  LLVMValueRef has_cap =
      LLVMBuildICmp(cg->builder, LLVMIntSGT, cap_raw, ny_c0(cg),
                    NY_LLVM_NAME(cg, "fast_dict_has_cap"));
  LLVMValueRef key_raw =
      ny_build_untagged_or_raw_i64(cg, key_v, "fast_dict_key_raw");
  LLVMValueRef key_tagged = ny_tag_int(cg, key_raw);
  LLVMValueRef mask = ny_sub(cg, cap_raw, LLVMConstInt(cg->type_i64, 1, false),
                             "fast_dict_mask");
  LLVMValueRef init_idx =
      ny_and(cg, key_raw, mask, NY_LLVM_NAME(cg, "fast_dict_init_idx"));
  LLVMBasicBlockRef cap_end_bb = ny_cur_block(cg);
  ny_cond_br(cg, has_cap, loop_bb, default_bb);

  ny_pos(cg, loop_bb);
  LLVMValueRef idx_phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_dict_idx"));
  LLVMValueRef probe_phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_dict_probe"));
  LLVMAddIncoming(idx_phi, &init_idx, &cap_end_bb, 1);
  LLVMValueRef zero = ny_c0(cg);
  LLVMAddIncoming(probe_phi, &zero, &cap_end_bb, 1);

  LLVMValueRef idx_times_24 =
      LLVMBuildMul(cg->builder, idx_phi, LLVMConstInt(cg->type_i64, 24, false),
                   "fast_dict_idx24");
  LLVMValueRef off =
      LLVMBuildAdd(cg->builder, idx_times_24,
                   LLVMConstInt(cg->type_i64, 16, false), "fast_dict_off");
  LLVMValueRef state_addr =
      LLVMBuildAdd(cg->builder, off, LLVMConstInt(cg->type_i64, 16, false),
                   "fast_dict_state_off");
  LLVMValueRef state_abs = ny_add(cg, target_v, state_addr,
                                  NY_LLVM_NAME(cg, "fast_dict_state_addr"));
  LLVMValueRef state_ptr = LLVMBuildIntToPtr(
      cg->builder, state_abs, ny_ptr_i64_ty(cg), "fast_dict_state_ptr");
  LLVMValueRef state_v =
      ny_load(cg, state_ptr, NY_LLVM_NAME(cg, "fast_dict_state"));
  LLVMValueRef is_empty =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, state_v, ny_c0(cg),
                    NY_LLVM_NAME(cg, "fast_dict_empty"));
  ny_cond_br(cg, is_empty, default_bb, match_bb);

  ny_pos(cg, match_bb);
  LLVMValueRef key_abs =
      ny_add(cg, target_v, off, NY_LLVM_NAME(cg, "fast_dict_key_addr"));
  LLVMValueRef key_ptr = LLVMBuildIntToPtr(
      cg->builder, key_abs, ny_ptr_i64_ty(cg), "fast_dict_key_ptr");
  LLVMValueRef slot_key =
      ny_load(cg, key_ptr, NY_LLVM_NAME(cg, "fast_dict_slot_key"));
  LLVMValueRef state_filled =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, state_v,
                    LLVMConstInt(cg->type_i64, ((uint64_t)1 << 1) | 1u, false),
                    NY_LLVM_NAME(cg, "fast_dict_filled"));
  LLVMValueRef key_eq =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, slot_key, key_tagged,
                    NY_LLVM_NAME(cg, "fast_dict_key_eq"));
  LLVMValueRef is_match =
      ny_and(cg, state_filled, key_eq, NY_LLVM_NAME(cg, "fast_dict_match"));
  ny_cond_br(cg, is_match, found_bb, next_bb);

  ny_pos(cg, next_bb);
  LLVMValueRef next_probe =
      ny_add(cg, probe_phi, LLVMConstInt(cg->type_i64, 1, false),
             "fast_dict_next_probe");
  LLVMValueRef next_idx_raw =
      ny_add(cg, idx_phi, LLVMConstInt(cg->type_i64, 1, false),
             "fast_dict_next_idx_raw");
  LLVMValueRef next_idx =
      ny_and(cg, next_idx_raw, mask, NY_LLVM_NAME(cg, "fast_dict_next_idx"));
  LLVMValueRef keep_going =
      LLVMBuildICmp(cg->builder, LLVMIntSLT, next_probe, cap_raw,
                    NY_LLVM_NAME(cg, "fast_dict_more"));
  LLVMBasicBlockRef next_end_bb = ny_cur_block(cg);
  LLVMAddIncoming(idx_phi, &next_idx, &next_end_bb, 1);
  LLVMAddIncoming(probe_phi, &next_probe, &next_end_bb, 1);
  ny_cond_br(cg, keep_going, loop_bb, default_bb);

  ny_pos(cg, found_bb);
  LLVMValueRef val_off =
      LLVMBuildAdd(cg->builder, off, LLVMConstInt(cg->type_i64, 8, false),
                   "fast_dict_val_off");
  LLVMValueRef val_abs =
      ny_add(cg, target_v, val_off, NY_LLVM_NAME(cg, "fast_dict_val_addr"));
  LLVMValueRef val_ptr = LLVMBuildIntToPtr(
      cg->builder, val_abs, ny_ptr_i64_ty(cg), "fast_dict_val_ptr");
  LLVMValueRef found_val =
      ny_load(cg, val_ptr, NY_LLVM_NAME(cg, "fast_dict_val"));
  LLVMBasicBlockRef found_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, default_bb);
  LLVMBasicBlockRef default_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_res = default_v;
  if (fallback_sig) {
    LLVMValueRef args[3] = {target_v, key_v, default_v};
    ny_dbg_loc(cg, tok);
    slow_res =
        LLVMBuildCall2(cg->builder, fallback_sig->type, fallback_sig->value,
                       args, 3, NY_LLVM_NAME(cg, "fast_dict_fallback"));
  }
  LLVMBasicBlockRef slow_end_bb = ny_cur_block(cg);
  ny_br(cg, join_bb);

  ny_pos(cg, join_bb);
  LLVMValueRef phi =
      ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "fast_dict_result"));
  LLVMValueRef incoming_vals[3] = {found_val, default_v, slow_res};
  LLVMBasicBlockRef incoming_bbs[3] = {found_end_bb, default_end_bb,
                                       slow_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 3);
  return phi;
}

static LLVMValueRef ny_try_fast_len_builtin(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *target,
                                            token_t tok) {
  if (!cg || !target)
    return 0;
  if (target->kind == NY_E_LITERAL && target->as.literal.kind == NY_LIT_STR)
    return LLVMConstInt(
        cg->type_i64, ((uint64_t)target->as.literal.as.s.len << 1) | 1u, false);
  uint64_t literal_collection_len = 0;
  if (ny_gencall_literal_collection_len(target, &literal_collection_len))
    return LLVMConstInt(cg->type_i64, (literal_collection_len << 1) | 1u,
                        false);
  expr_t *static_init = ny_gencall_static_init_expr(cg, scopes, depth, target);
  if (static_init && static_init->kind == NY_E_LITERAL &&
      static_init->as.literal.kind == NY_LIT_STR) {
    return LLVMConstInt(
        cg->type_i64, ((uint64_t)static_init->as.literal.as.s.len << 1) | 1u,
        false);
  }
  if (static_init &&
      (static_init->kind == NY_E_LIST || static_init->kind == NY_E_TUPLE))
    return LLVMConstInt(cg->type_i64,
                        ((uint64_t)static_init->as.list_like.len << 1) | 1u,
                        false);
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b =
        ny_gencall_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                  name_len, target->as.ident.hash);
    if (b && b->raw_int_list_mutation && b->raw_int_list_ptr &&
        b->raw_int_list_len > 0)
      return LLVMConstInt(cg->type_i64,
                          (((uint64_t)b->raw_int_list_len) << 1) | 1u, false);
  }

  const char *type_name = infer_expr_type(cg, scopes, depth, target);
  if (type_name && ny_gencall_type_is_nullable(type_name))
    return 0;

  bool header_len = ny_gencall_type_is(type_name, "list") ||
                    ny_gencall_type_is(type_name, "tuple") ||
                    ny_gencall_type_is(type_name, "dict") ||
                    ny_gencall_type_is(type_name, "set");
  bool side_header_len = ny_gencall_type_is(type_name, "str") ||
                         ny_gencall_type_is(type_name, "bytes");
  if (!header_len && ny_expr_is_direct_list_storage(cg, scopes, depth, target))
    header_len = true;
  if (!header_len && ny_expr_is_direct_dict_storage(cg, scopes, depth, target))
    header_len = true;
  if (!header_len && !side_header_len)
    return 0;

  LLVMValueRef obj_v = gen_expr(cg, scopes, depth, target);
  if (!obj_v) {
    ny_diag_error(tok, "failed to evaluate fast len(...) operand");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  obj_v = ny_cast_to_i64(cg, obj_v, "fast_len_obj");
  ny_dbg_loc(cg, tok);

  LLVMValueRef addr = obj_v;
  if (side_header_len)
    addr = ny_sub(cg, obj_v, LLVMConstInt(cg->type_i64, 16, false),
                  "fast_strlen_addr");
  LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, addr, ny_ptr_i64_ty(cg),
                                       "fast_len_ptr_i64");
  return ny_load(cg, ptr, NY_LLVM_NAME(cg, "fast_len"));
}

static fun_sig *ny_gencall_lookup_len_func(codegen_t *cg) {
  static const char *const names[] = {"std.core.len", "len",
                                      "std.core.reflect.len"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    fun_sig *sig = lookup_fun(cg, names[i], 0);
    if (sig && ny_sig_in_current_sigs(cg, sig) && sig->arity == 1)
      return sig;
  }
  return NULL;
}

static bool ny_gencall_is_std_iter_count_sig(fun_sig *sig) {
  return sig && sig->name && strcmp(sig->name, "std.core.iter.count") == 0;
}

static bool ny_gencall_is_std_iter_count_call(codegen_t *cg, const char *name,
                                              uint64_t name_hash, bool shadowed,
                                              expr_call_t *c) {
  if (!cg || !name || !c || c->args.len != 1)
    return false;
  if (strcmp(name, "std.core.iter.count") == 0)
    return true;
  if (strcmp(name, "count") != 0 || shadowed)
    return false;
  fun_sig *sig = lookup_fun(cg, name, name_hash);
  if (ny_gencall_is_std_iter_count_sig(sig))
    return true;
  sig = lookup_use_module_fun(cg, name, 1);
  return ny_gencall_is_std_iter_count_sig(sig);
}

static LLVMValueRef emit_load_layout(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e, expr_call_t *c);

static layout_field_info_t *
ny_gencall_layout_member_field(codegen_t *cg, const char *type_name,
                               const char *field_name,
                               layout_def_t **layout_out) {
  if (layout_out)
    *layout_out = NULL;
  const char *owner = ny_gencall_attached_owner(type_name);
  if (!cg || !owner || !*owner || !field_name || !*field_name)
    return NULL;
  layout_def_t *layout = lookup_layout(cg, owner);
  if (!layout)
    return NULL;
  for (size_t i = 0; i < layout->fields.len; ++i) {
    layout_field_info_t *field = &layout->fields.data[i];
    if (field->name && strcmp(field->name, field_name) == 0) {
      if (layout_out)
        *layout_out = layout;
      return field;
    }
  }
  return NULL;
}

static LLVMValueRef ny_gencall_layout_member_expr(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e,
                                                  layout_def_t *layout) {
  if (!cg || !e || !layout || !layout->name || !e->as.member.target ||
      !e->as.member.name)
    return NULL;

  expr_t layout_lit = {0};
  layout_lit.kind = NY_E_LITERAL;
  layout_lit.tok = e->tok;
  layout_lit.as.literal.kind = NY_LIT_STR;
  layout_lit.as.literal.as.s.data = layout->name;
  layout_lit.as.literal.as.s.len = strlen(layout->name);

  expr_t field_lit = {0};
  field_lit.kind = NY_E_LITERAL;
  field_lit.tok = e->tok;
  field_lit.as.literal.kind = NY_LIT_STR;
  field_lit.as.literal.as.s.data = e->as.member.name;
  field_lit.as.literal.as.s.len = strlen(e->as.member.name);

  call_arg_t args[3] = {
      {NULL, e->as.member.target}, {NULL, &layout_lit}, {NULL, &field_lit}};
  expr_t call = {0};
  call.kind = NY_E_CALL;
  call.tok = e->tok;
  call.as.call.args.data = args;
  call.as.call.args.len = 3;
  call.as.call.args.cap = 3;
  return emit_load_layout(cg, scopes, depth, &call, &call.as.call);
}

static bool ny_member_type_allows_dynamic_fallback(const char *type_name) {
  const char *owner = ny_gencall_attached_owner(type_name);
  if (!owner || !*owner || strcmp(owner, "any") == 0)
    return true;
  return strcmp(owner, "dict") == 0 || strcmp(owner, "nil") == 0;
}

static bool ny_member_is_vector_lane(const char *type_name,
                                     const char *member) {
  int dim = ny_gencall_vec_type_dim(type_name);
  if (dim <= 0 || !member)
    return false;
  return strcmp(member, "x") == 0 || strcmp(member, "y") == 0 ||
         (dim >= 3 && strcmp(member, "z") == 0) ||
         (dim >= 4 && strcmp(member, "w") == 0);
}

static bool ny_member_name_is_mathy(const char *name) {
  static const char *const names[] = {
      "abs",   "min",  "max",   "pow", "mod",  "clamp",     "clamp01",
      "sign",  "sqrt", "lerp",  "sin", "cos",  "tan",       "atan",
      "asin",  "acos", "exp",   "log", "log2", "log10",     "fmod",
      "floor", "ceil", "round", "gcd", "lcm",  "factorial", NULL};
  for (size_t i = 0; names[i]; ++i) {
    if (strcmp(name, names[i]) == 0)
      return true;
  }
  return false;
}

static bool ny_member_name_is_byte_helper(const char *name) {
  static const char *const names[] = {
      "hex",    "base64", "text",  "le16",       "be16",  "le32",
      "be32",   "le64",   "be64",  "u8",         "u16le", "u16be",
      "u32le",  "u32be",  "u64le", "u64be",      "xor",   "concat",
      "repeat", "rev",    "trim0", "bytes_long", NULL};
  for (size_t i = 0; names[i]; ++i) {
    if (strcmp(name, names[i]) == 0)
      return true;
  }
  return false;
}

static void ny_diag_static_member_hints(const char *owner, const char *name) {
  if (!name)
    return;
  if (ny_member_name_is_mathy(name)) {
    if (owner && (strcmp(owner, "list") == 0 || strcmp(owner, "dict") == 0 ||
                  strcmp(owner, "str") == 0 || strcmp(owner, "bytes") == 0 ||
                  strcmp(owner, "tuple") == 0 || strcmp(owner, "set") == 0)) {
      ny_diag_hint("'%s' is a numeric method; use an int/f32/f64 receiver",
                   name);
    } else {
      ny_diag_hint("numeric methods are provided by std.math");
      ny_diag_fix("add use std.math at file top");
    }
  } else if (ny_member_name_is_byte_helper(name)) {
    ny_diag_hint("byte-list helpers are provided by std.math.bin");
    ny_diag_fix("add use std.math.bin at file top");
  } else if (strcmp(name, "as_bytes") == 0 && owner &&
             strcmp(owner, "list") == 0) {
    ny_diag_hint("list byte aliases are .as_bytes, .bytes, and .to_bytes");
  }
}

static LLVMValueRef ny_member_unknown_static_expr(codegen_t *cg, token_t tok,
                                                  const char *kind,
                                                  const char *name,
                                                  const char *type_name) {
  const char *owner = ny_gencall_attached_owner(type_name);
  const char *shown = owner ? owner : (type_name ? type_name : "value");
  ny_diag_error(tok, "unknown %s '%s' for %s", kind ? kind : "member",
                name ? name : "<unknown>", shown);
  ny_diag_static_member_hints(owner, name);
  cg->had_error = 1;
  return ny_c0(cg);
}

static LLVMValueRef ny_member_requires_call_expr(codegen_t *cg, token_t tok,
                                                 const char *member,
                                                 fun_sig *sig) {
  const char *name =
      sig && sig->name ? sig->name : (member ? member : "<unknown>");
  size_t min_arity = ny_sig_min_arity(sig);
  size_t min_user = min_arity > 0 ? min_arity - 1 : 0;
  size_t max_user = (sig && sig->arity > 0) ? (size_t)sig->arity - 1 : 0;
  ny_diag_error(tok, "method '%s' requires a call", name);
  if (min_user == max_user)
    ny_diag_hint("expected %zu argument(s) after the receiver", min_user);
  else
    ny_diag_hint("expected %zu..%zu argument(s) after the receiver", min_user,
                 max_user);
  ny_diag_fix("write .%s(...)", member ? member : name);
  cg->had_error = 1;
  return ny_c0(cg);
}

LLVMValueRef ny_try_member_property_expr(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_MEMBER || !e->as.member.target ||
      !e->as.member.name)
    return 0;

  if (strcmp(e->as.member.name, "len") == 0) {
    LLVMValueRef fast_len =
        ny_try_fast_len_builtin(cg, scopes, depth, e->as.member.target, e->tok);
    if (fast_len)
      return fast_len;
  }

  const char *target_type =
      infer_expr_type(cg, scopes, depth, e->as.member.target);
  bool dynamic_fallback = ny_member_type_allows_dynamic_fallback(target_type);

  layout_def_t *layout = NULL;
  if (ny_gencall_layout_member_field(cg, target_type, e->as.member.name,
                                     &layout))
    return ny_gencall_layout_member_expr(cg, scopes, depth, e, layout);

  fun_sig *sig_found = ny_gencall_lookup_attached_method(
      cg, target_type ? target_type : "any", e->as.member.name);
  bool sig_from_any = false;
  if (!sig_found && target_type) {
    sig_found = ny_gencall_lookup_attached_method(cg, "any", e->as.member.name);
    sig_from_any = sig_found != NULL;
  }
  if (ny_sig_allows_zero_arg_property(sig_found)) {
    expr_t call = {0};
    call.kind = NY_E_MEMCALL;
    call.tok = e->tok;
    call.as.memcall.target = e->as.member.target;
    call.as.memcall.name = e->as.member.name;
    return gen_call_expr(cg, scopes, depth, &call);
  }
  if (sig_found && !dynamic_fallback && !sig_from_any)
    return ny_member_requires_call_expr(cg, e->tok, e->as.member.name,
                                        sig_found);

  if (ny_member_is_vector_lane(target_type, e->as.member.name))
    return 0;

  if (strcmp(e->as.member.name, "long") == 0) {
    fun_sig *long_sig = lookup_fun(cg, "__long", 0);
    if (long_sig) {
      LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.member.target);
      if (!target_v) {
        ny_diag_error(e->tok, "failed to evaluate .long target");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      target_v = ny_cast_to_i64(cg, target_v, "property_long_arg");
      ny_dbg_loc(cg, e->tok);
      return LLVMBuildCall2(cg->builder, long_sig->type, long_sig->value,
                            (LLVMValueRef[]){target_v}, 1,
                            NY_LLVM_NAME(cg, "property_long"));
    }
  }

  if (strcmp(e->as.member.name, "len") == 0) {
    fun_sig *len_sig = ny_gencall_lookup_len_func(cg);
    if (len_sig) {
      LLVMValueRef target_v = gen_expr(cg, scopes, depth, e->as.member.target);
      if (!target_v) {
        ny_diag_error(e->tok, "failed to evaluate .len target");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      target_v = ny_cast_to_i64(cg, target_v, "property_len_arg");
      ny_dbg_loc(cg, e->tok);
      return LLVMBuildCall2(cg->builder, len_sig->type, len_sig->value,
                            (LLVMValueRef[]){target_v}, 1,
                            NY_LLVM_NAME(cg, "property_len"));
    }
  }

  if (!dynamic_fallback)
    return ny_member_unknown_static_expr(cg, e->tok, "member",
                                         e->as.member.name, target_type);

  return 0;
}

static LLVMTypeRef abi_type_from_name(codegen_t *cg, const char *type_name) {
  if (!type_name || !*type_name)
    return cg->type_i64;
  type_name = abi_skip_nullable(type_name);
  layout_def_t *layout = abi_layout_from_name(cg, type_name);
  if (layout)
    return ny_layout_abi_carrier_type(cg, layout);
  if (abi_type_is_ptr(type_name))
    return cg->type_i8ptr;
  if (abi_type_is_tagged(type_name))
    return cg->type_i64;
  if (strcmp(type_name, "void") == 0)
    return LLVMVoidTypeInContext(cg->ctx);
  if (strcmp(type_name, "char") == 0 || strcmp(type_name, "i8") == 0 ||
      strcmp(type_name, "u8") == 0)
    return cg->type_i8;
  if (strcmp(type_name, "i16") == 0 || strcmp(type_name, "u16") == 0)
    return cg->type_i16;
  if (strcmp(type_name, "i32") == 0 || strcmp(type_name, "u32") == 0)
    return cg->type_i32;
  if (strcmp(type_name, "i64") == 0 || strcmp(type_name, "u64") == 0 ||
      strcmp(type_name, "handle") == 0)
    return cg->type_i64;
  if (strcmp(type_name, "i128") == 0 || strcmp(type_name, "u128") == 0)
    return cg->type_i128;
  if (strcmp(type_name, "f32") == 0)
    return cg->type_f32;
  if (strcmp(type_name, "f64") == 0)
    return cg->type_f64;
  if (strcmp(type_name, "f128") == 0)
    return cg->type_f128;
  if (abi_type_is_complex(type_name))
    return abi_complex_type(cg, type_name);
  return cg->type_i64;
}

static LLVMValueRef abi_untag_int(codegen_t *cg, LLVMValueRef v,
                                  bool is_signed) {
  if (!cg || !v)
    return v;
  if (LLVMTypeOf(v) != cg->type_i64)
    v = ny_cast_to_i64(cg, v, "abi_int_arg");

  LLVMValueRef shift = ny_c1(cg);

  LLVMValueRef lsb = ny_and(cg, v, shift, NY_LLVM_NAME(cg, "untag_lsb"));
  LLVMValueRef is_tagged =
      ny_eq(cg, lsb, shift, NY_LLVM_NAME(cg, "untag_tagged"));
  LLVMValueRef shifted =
      is_signed ? ny_ashr(cg, v, shift, NY_LLVM_NAME(cg, "untag_shifted"))
                : LLVMBuildLShr(cg->builder, v, shift,
                                NY_LLVM_NAME(cg, "untag_shifted"));
  fun_sig *f2i = lookup_fun(cg, "__flt_to_int", 0);
  fun_sig *b2i = lookup_fun(cg, "__bigint_to_int", 0);
  fun_sig *has_tag = lookup_fun(cg, "__has_tag", 0);
  if (!f2i)
    return ny_select(cg, is_tagged, shifted, v,
                     NY_LLVM_NAME(cg, "untag_result"));

  LLVMValueRef fn = LLVMGetBasicBlockParent(ny_cur_block(cg));
  LLVMBasicBlockRef int_bb = ny_bb_fn(fn, "abi_int.tagged");
  LLVMBasicBlockRef float_chk_bb = ny_bb_fn(fn, "abi_int.float_chk");
  LLVMBasicBlockRef float_bb = ny_bb_fn(fn, "abi_int.float");
  LLVMBasicBlockRef bigint_chk_bb = ny_bb_fn(fn, "abi_int.bigint_chk");
  LLVMBasicBlockRef bigint_bb = ny_bb_fn(fn, "abi_int.bigint");
  LLVMBasicBlockRef raw_bb = ny_bb_fn(fn, "abi_int.raw");
  LLVMBasicBlockRef done_bb = ny_bb_fn(fn, "abi_int.done");

  ny_cond_br(cg, is_tagged, int_bb, float_chk_bb);

  ny_pos(cg, int_bb);
  LLVMValueRef int_res = shifted;
  LLVMBasicBlockRef int_end_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, float_chk_bb);
  ny_cond_br(cg, ny_is_float(cg, v), float_bb, bigint_chk_bb);

  ny_pos(cg, float_bb);
  LLVMValueRef tagged_float_int =
      LLVMBuildCall2(cg->builder, f2i->type, f2i->value, &v, 1,
                     NY_LLVM_NAME(cg, "abi_flt_to_int"));
  LLVMValueRef float_res =
      is_signed
          ? ny_ashr(cg, tagged_float_int, shift, NY_LLVM_NAME(cg, "abi_flt_i"))
          : LLVMBuildLShr(cg->builder, tagged_float_int, shift,
                          NY_LLVM_NAME(cg, "abi_flt_u"));
  LLVMBasicBlockRef float_end_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, bigint_chk_bb);
  LLVMValueRef is_bigint = LLVMConstInt(cg->type_i1, 0, false);
  if (b2i && has_tag) {
    LLVMValueRef bigint_tag =
        ny_tag_int(cg, LLVMConstInt(cg->type_i64, TAG_BIGINT, false));
    LLVMValueRef has_args[2] = {v, bigint_tag};
    LLVMValueRef tagged_has_bigint =
        LLVMBuildCall2(cg->builder, has_tag->type, has_tag->value, has_args, 2,
                       NY_LLVM_NAME(cg, "abi_int_is_bigint"));
    is_bigint = ny_eq(cg, tagged_has_bigint, ny_ctrue(cg),
                      NY_LLVM_NAME(cg, "abi_int_bigint_pred"));
  }
  ny_cond_br(cg, is_bigint, bigint_bb, raw_bb);

  ny_pos(cg, bigint_bb);
  LLVMValueRef tagged_bigint_int =
      LLVMBuildCall2(cg->builder, b2i->type, b2i->value, &v, 1,
                     NY_LLVM_NAME(cg, "abi_bigint_to_int"));
  LLVMValueRef bigint_res =
      is_signed
          ? ny_ashr(cg, tagged_bigint_int, shift, NY_LLVM_NAME(cg, "abi_big_i"))
          : LLVMBuildLShr(cg->builder, tagged_bigint_int, shift,
                          NY_LLVM_NAME(cg, "abi_big_u"));
  LLVMBasicBlockRef bigint_end_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, raw_bb);
  LLVMValueRef raw_res = v;
  LLVMBasicBlockRef raw_end_bb = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "abi_int_phi"));
  LLVMValueRef incoming_vals[4] = {int_res, float_res, bigint_res, raw_res};
  LLVMBasicBlockRef incoming_bbs[4] = {int_end_bb, float_end_bb, bigint_end_bb,
                                       raw_end_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 4);
  return phi;
}

static LLVMValueRef abi_untag_proven_int_fast(codegen_t *cg, LLVMValueRef v) {
  if (!cg || !v)
    return v;
  if (LLVMTypeOf(v) != cg->type_i64)
    v = ny_cast_to_i64(cg, v, "proven_int_cast_arg");
  return LLVMBuildAShr(cg->builder, v, ny_c1(cg),
                       NY_LLVM_NAME(cg, "proven_int_cast_fast"));
}

static LLVMValueRef abi_cast_int(codegen_t *cg, LLVMValueRef v,
                                 LLVMTypeRef target, bool is_signed) {
  if (LLVMTypeOf(v) == target)
    return v;
  unsigned src_w = LLVMGetIntTypeWidth(LLVMTypeOf(v));
  unsigned dst_w = LLVMGetIntTypeWidth(target);
  if (dst_w < src_w) {
    return LLVMBuildTrunc(cg->builder, v, target,
                          NY_LLVM_NAME(cg, "int_trunc"));
  }
  if (dst_w > src_w) {
    return is_signed ? LLVMBuildSExt(cg->builder, v, target,
                                     NY_LLVM_NAME(cg, "int_sext"))
                     : LLVMBuildZExt(cg->builder, v, target,
                                     NY_LLVM_NAME(cg, "int_zext"));
  }
  return ny_bitcast(cg, v, target, NY_LLVM_NAME(cg, "int_cast"));
}

static int abi_float_rank(LLVMTypeRef ty) {
  if (!ty)
    return 0;
  switch (LLVMGetTypeKind(ty)) {
  case LLVMFloatTypeKind:
    return 32;
  case LLVMDoubleTypeKind:
    return 64;
  case LLVMFP128TypeKind:
    return 128;
  default:
    return 0;
  }
}

static LLVMValueRef abi_cast_float(codegen_t *cg, LLVMValueRef v,
                                   LLVMTypeRef target, const char *name) {
  if (!v || !target || LLVMTypeOf(v) == target)
    return v;
  int src_rank = abi_float_rank(LLVMTypeOf(v));
  int dst_rank = abi_float_rank(target);
  if (!src_rank || !dst_rank)
    return v;
  if (dst_rank > src_rank)
    return LLVMBuildFPExt(cg->builder, v, target, NY_LLVM_NAME(cg, name));
  if (dst_rank < src_rank)
    return LLVMBuildFPTrunc(cg->builder, v, target, NY_LLVM_NAME(cg, name));
  return ny_bitcast(cg, v, target, NY_LLVM_NAME(cg, name));
}

LLVMValueRef ny_coerce_to_abi_proven_int(codegen_t *cg, LLVMValueRef v,
                                         const char *type_name,
                                         bool proven_int) {
  if (!type_name || abi_type_is_tagged(type_name))
    return v;
  type_name = abi_skip_nullable(type_name);
  layout_def_t *layout = abi_layout_from_name(cg, type_name);
  if (layout) {
    if (layout->size > 16) {
      LLVMValueRef ptr = abi_layout_ptr_from_value(cg, v, layout->llvm_type,
                                                   "abi_layout_byval_ptr");
      if (LLVMTypeOf(ptr) != cg->type_i8ptr)
        ptr = LLVMBuildBitCast(cg->builder, ptr, cg->type_i8ptr,
                               NY_LLVM_NAME(cg, "abi_layout_byval_i8"));
      return ptr;
    }
    LLVMTypeRef carrier = ny_layout_abi_carrier_type(cg, layout);
    if (LLVMTypeOf(v) == carrier)
      return v;
    LLVMValueRef ptr =
        abi_layout_ptr_from_value(cg, v, carrier, "abi_layout_arg_ptr");
    return LLVMBuildLoad2(cg->builder, carrier, ptr,
                          NY_LLVM_NAME(cg, "abi_layout_arg"));
  }
  if (abi_type_is_fnptr(type_name)) {
    LLVMValueRef raw = abi_decode_native_fnptr(cg, v);
    return LLVMBuildIntToPtr(cg->builder, raw, cg->type_i8ptr,
                             NY_LLVM_NAME(cg, "arg_fnptr"));
  }
  if (abi_type_is_ptr(type_name)) {
    LLVMValueRef mask = LLVMConstInt(cg->type_i64, ~(uint64_t)1, false);
    LLVMValueRef raw = ny_and(cg, v, mask, NY_LLVM_NAME(cg, "ptr_untag"));
    return LLVMBuildIntToPtr(cg->builder, raw, cg->type_i8ptr,
                             NY_LLVM_NAME(cg, "arg_ptr"));
  }
  if (abi_type_is_float(type_name)) {
    LLVMTypeRef target = abi_type_from_name(cg, type_name);
    if (abi_float_rank(LLVMTypeOf(v)))
      return abi_cast_float(cg, v, target, "float_arg");

    fun_sig *unbox = ny_gencall_flt_unbox(cg);
    if (!unbox)
      return v;
    LLVMValueRef bits =
        LLVMBuildCall2(cg->builder, unbox->type, unbox->value, &v, 1, "");
    LLVMValueRef dbl = ny_bitcast(cg, bits, cg->type_f64, "");
    return abi_cast_float(cg, dbl, target, "float_arg");
  }
  if (abi_type_is_complex(type_name)) {
    fun_sig *re_sig = lookup_fun(cg, "__complex_re_bits", 0);
    fun_sig *im_sig = lookup_fun(cg, "__complex_im_bits", 0);
    if (!re_sig || !im_sig)
      return v;
    LLVMValueRef re_bits =
        LLVMBuildCall2(cg->builder, re_sig->type, re_sig->value, &v, 1,
                       NY_LLVM_NAME(cg, "complex_re_bits"));
    LLVMValueRef im_bits =
        LLVMBuildCall2(cg->builder, im_sig->type, im_sig->value, &v, 1,
                       NY_LLVM_NAME(cg, "complex_im_bits"));
    LLVMValueRef re =
        ny_bitcast(cg, re_bits, cg->type_f64, NY_LLVM_NAME(cg, "complex_re"));
    LLVMValueRef im =
        ny_bitcast(cg, im_bits, cg->type_f64, NY_LLVM_NAME(cg, "complex_im"));
    const char *complex_leaf = ny_type_leaf(type_name);
    if (complex_leaf && strcmp(complex_leaf, "c64") == 0) {
      re = LLVMBuildFPTrunc(cg->builder, re, cg->type_f32,
                            NY_LLVM_NAME(cg, "complex_re32"));
      im = LLVMBuildFPTrunc(cg->builder, im, cg->type_f32,
                            NY_LLVM_NAME(cg, "complex_im32"));
    }
    LLVMTypeRef cty = abi_complex_type(cg, type_name);
    LLVMValueRef agg = LLVMGetUndef(cty);
    agg = LLVMBuildInsertValue(cg->builder, agg, re, 0,
                               NY_LLVM_NAME(cg, "complex.ins.re"));
    agg = LLVMBuildInsertValue(cg->builder, agg, im, 1,
                               NY_LLVM_NAME(cg, "complex.ins.im"));
    return agg;
  }
  if (abi_type_is_signed_int(type_name)) {
    LLVMTypeRef target = abi_type_from_name(cg, type_name);
    LLVMValueRef raw = (proven_int && ny_proven_int_cast_fast_enabled(cg))
                           ? abi_untag_proven_int_fast(cg, v)
                           : abi_untag_int(cg, v, true);
    return abi_cast_int(cg, raw, target, true);
  }
  if (abi_type_is_unsigned_int(type_name)) {
    LLVMValueRef raw = (proven_int && ny_proven_int_cast_fast_enabled(cg))
                           ? abi_untag_proven_int_fast(cg, v)
                           : abi_untag_int(cg, v, false);
    LLVMTypeRef target = abi_type_from_name(cg, type_name);
    return abi_cast_int(cg, raw, target, false);
  }
  return v;
}

LLVMValueRef ny_coerce_to_abi(codegen_t *cg, LLVMValueRef v,
                              const char *type_name) {
  return ny_coerce_to_abi_proven_int(cg, v, type_name, false);
}

LLVMValueRef ny_box_abi_result(codegen_t *cg, LLVMValueRef v,
                               const char *type_name) {
  if (!type_name || abi_type_is_tagged(type_name))
    return v;
  type_name = abi_skip_nullable(type_name);
  if (strcmp(type_name, "void") == 0)
    return ny_c0(cg);
  layout_def_t *layout = abi_layout_from_name(cg, type_name);
  if (layout) {
    fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
    if (!malloc_sig) {
      ny_diag_error((token_t){0},
                    "__malloc required for native layout ABI return");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    LLVMValueRef size_arg =
        LLVMConstInt(cg->type_i64, (((uint64_t)layout->size << 1) | 1u), false);
    LLVMValueRef ptr_i64 =
        LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                       &size_arg, 1, NY_LLVM_NAME(cg, "abi_layout_alloc"));
    LLVMTypeRef carrier = ny_layout_abi_carrier_type(cg, layout);
    LLVMValueRef dst =
        abi_layout_ptr_from_value(cg, ptr_i64, carrier, "abi_layout_ret_ptr");
    ny_store(cg, dst, v);
    return ptr_i64;
  }
  if (abi_type_is_fnptr(type_name)) {
    LLVMValueRef raw = abi_raw_value_to_i64(cg, v, "ret_fnptr");
    return abi_encode_native_i64(cg, raw, "tag_fnptr");
  }
  if (strcmp(type_name, "cstr") == 0) {
    fun_sig *box = lookup_fun(cg, "__cstr_to_str", 0);
    if (!box)
      return ny_c0(cg);
    LLVMValueRef raw = ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, "ret_cstr"));
    LLVMValueRef native = abi_encode_native_i64(cg, raw, "ret_cstr_native");
    return LLVMBuildCall2(cg->builder, box->type, box->value, &native, 1,
                          NY_LLVM_NAME(cg, "cstr_to_str"));
  }
  if (abi_type_is_ptr(type_name)) {
    return ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, "ret_ptr"));
  }
  if (abi_type_is_float(type_name)) {
    fun_sig *box = ny_gencall_flt_box(cg);
    if (!box)
      return ny_c0(cg);
    LLVMValueRef dbl = v;
    if (strcmp(type_name, "f32") == 0) {
      dbl = LLVMBuildFPExt(cg->builder, v, cg->type_f64,
                           NY_LLVM_NAME(cg, "f32_to_f64"));
    } else if (strcmp(type_name, "f128") == 0) {
      dbl = LLVMBuildFPTrunc(cg->builder, v, cg->type_f64,
                             NY_LLVM_NAME(cg, "f128_to_f64"));
    }
    LLVMValueRef bits = ny_bitcast(cg, dbl, cg->type_i64, "");
    return LLVMBuildCall2(cg->builder, box->type, box->value, &bits, 1, "");
  }
  if (abi_type_is_complex(type_name)) {
    fun_sig *box = lookup_fun(cg, "__complex_new_bits", 0);
    if (!box)
      return ny_c0(cg);
    LLVMValueRef re = LLVMBuildExtractValue(cg->builder, v, 0,
                                            NY_LLVM_NAME(cg, "complex.ret.re"));
    LLVMValueRef im = LLVMBuildExtractValue(cg->builder, v, 1,
                                            NY_LLVM_NAME(cg, "complex.ret.im"));
    const char *complex_leaf = ny_type_leaf(type_name);
    if (complex_leaf && strcmp(complex_leaf, "c64") == 0) {
      re = LLVMBuildFPExt(cg->builder, re, cg->type_f64,
                          NY_LLVM_NAME(cg, "complex.re64"));
      im = LLVMBuildFPExt(cg->builder, im, cg->type_f64,
                          NY_LLVM_NAME(cg, "complex.im64"));
    }
    LLVMValueRef args[2] = {
        ny_bitcast(cg, re, cg->type_i64, NY_LLVM_NAME(cg, "complex.re.bits")),
        ny_bitcast(cg, im, cg->type_i64, NY_LLVM_NAME(cg, "complex.im.bits"))};
    return LLVMBuildCall2(cg->builder, box->type, box->value, args, 2,
                          NY_LLVM_NAME(cg, "complex.box"));
  }
  if (abi_type_is_signed_int(type_name) ||
      abi_type_is_unsigned_int(type_name)) {
    bool signed_int = abi_type_is_signed_int(type_name);
    LLVMTypeRef target = cg->type_i64;
    LLVMValueRef widened = abi_cast_int(cg, v, target, signed_int);
    LLVMValueRef sh = ny_shl(cg, widened, ny_c1(cg), "");
    return ny_or(cg, sh, ny_c1(cg), "");
  }
  return v;
}

static void add_extern_sig(codegen_t *cg, const char *name, int arity) {
  if (!cg || !name || !*name || arity < 0)
    return;
  if (lookup_fun_exact(cg, name))
    return;
  LLVMTypeRef *pt = NULL;
  if (arity > 0)
    pt = alloca(sizeof(LLVMTypeRef) * (size_t)arity);
  for (int i = 0; i < arity; i++)
    pt[i] = cg->type_i64;
  LLVMTypeRef ft = LLVMFunctionType(cg->type_i64, pt, (unsigned)arity, 0);
  LLVMValueRef f = ny_get_named_fn(cg, name);
  if (!f)
    f = LLVMAddFunction(cg->module, name, ft);
  fun_sig sig = {.name = ny_strdup(name),
                 .type = ft,
                 .value = f,
                 .stmt_t = NULL,
                 .arity = arity,
                 .is_variadic = false,
                 .is_extern = true,
                 .is_native_abi = true,
                 .effects = NY_FX_FFI,
                 .args_escape = true,
                 .args_mutated = true,
                 .returns_alias = true,
                 .effects_known = true,
                 .link_name = ny_strdup(name),
                 .return_type = NULL,
                 .owned = true,
                 .name_hash = 0};
  sig.min_arity = arity < 0 ? 0 : arity;
  sig.min_arity_known = true;
  vec_push(&cg->fun_sigs, sig);
}

static bool handle_extern_all_args(codegen_t *cg, ny_call_arg_list *args) {
  if (!args || args->len != 1)
    return false;
  expr_t *arg = args->data[0].val;
  if (!arg || arg->kind != NY_E_LIST)
    return false;
  for (size_t i = 0; i < arg->as.list_like.len; i++) {
    expr_t *item = arg->as.list_like.data[i];
    const char *name = NULL;
    int arity = 0;
    if (item->kind == NY_E_LITERAL && item->as.literal.kind == NY_LIT_STR) {
      name = item->as.literal.as.s.data;
      arity = 0;
    } else if ((item->kind == NY_E_LIST || item->kind == NY_E_TUPLE) &&
               item->as.list_like.len == 2) {
      expr_t *n = item->as.list_like.data[0];
      expr_t *a = item->as.list_like.data[1];
      if (n->kind == NY_E_LITERAL && n->as.literal.kind == NY_LIT_STR &&
          a->kind == NY_E_LITERAL && a->as.literal.kind == NY_LIT_INT) {
        name = n->as.literal.as.s.data;
        arity = (int)a->as.literal.as.i;
      }
    }
    if (!name || arity < 0) {
      ny_diag_error((token_t){0},
                    "extern_all expects list of names or [name, arity]");
      cg->had_error = 1;
      return true;
    }
    add_extern_sig(cg, name, arity);
  }
  return true;
}

static bool layout_query_arg(expr_t *arg, const char **out) {
  if (!arg || arg->kind != NY_E_LITERAL || arg->as.literal.kind != NY_LIT_STR)
    return false;
  *out = arg->as.literal.as.s.data;
  return true;
}

static const ny_diag_rule_t *
find_nonliteral_call_diag_rule(codegen_t *cg, const char *call_name,
                               int arg_index) {
  if (!cg || !cg->prog || !call_name)
    return NULL;
  for (size_t i = 0; i < cg->prog->diagnostic_rules.len; i++) {
    const ny_diag_rule_t *r = &cg->prog->diagnostic_rules.data[i];
    if (r->reject_non_literal && r->arg_index == arg_index && r->call_name &&
        strcmp(r->call_name, call_name) == 0)
      return r;
  }
  return NULL;
}

static void emit_nonliteral_call_diag(codegen_t *cg, token_t tok,
                                      const char *call_name, int arg_index,
                                      const char *fallback_msg,
                                      const char *fallback_fix) {
  const ny_diag_rule_t *rule =
      find_nonliteral_call_diag_rule(cg, call_name, arg_index);
  ny_diag_error(tok, "%s",
                (rule && rule->message) ? rule->message : fallback_msg);
  if (rule && rule->fix)
    ny_diag_fix("%s", rule->fix);
  else if (fallback_fix)
    ny_diag_fix("%s", fallback_fix);
}

static LLVMValueRef emit_layout_query(codegen_t *cg, token_t tok,
                                      const char *layout_name,
                                      const char *field_name, bool want_align,
                                      bool want_offset) {
  if (!layout_name) {
    ny_diag_error(tok, "layout query expects a string literal name");
    cg->had_error = 1;
    return ny_c1(cg);
  }
  layout_def_t *def = lookup_layout(cg, layout_name);
  if (!def) {
    ny_diag_error(tok, "unknown layout '%s'", layout_name);
    cg->had_error = 1;
    return ny_c1(cg);
  }
  size_t val = def->size;
  if (want_align)
    val = def->align;
  if (want_offset) {
    if (!field_name) {
      ny_diag_error(tok, "layout offset expects a field name");
      cg->had_error = 1;
      return ny_c1(cg);
    }
    bool found = false;
    for (size_t i = 0; i < def->fields.len; i++) {
      layout_field_info_t *fi = &def->fields.data[i];
      if (fi->name && strcmp(fi->name, field_name) == 0) {
        val = fi->offset;
        found = true;
        break;
      }
    }
    if (!found) {
      ny_diag_error(tok, "unknown field '%s' in layout '%s'", field_name,
                    layout_name);
      cg->had_error = 1;
      return ny_c1(cg);
    }
  }
  return LLVMConstInt(cg->type_i64, ((uint64_t)val << 1) | 1, false);
}

static LLVMValueRef layout_store_bool_value(codegen_t *cg, LLVMValueRef v) {
  if (!v)
    return NULL;
  if (LLVMTypeOf(v) == cg->type_bool)
    return v;
  if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind)
    v = ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, "layout_bool_ptr"));
  if (LLVMTypeOf(v) != cg->type_i64)
    v = LLVMBuildZExtOrBitCast(cg->builder, v, cg->type_i64,
                               NY_LLVM_NAME(cg, "layout_bool_i64"));
  return ny_eq(cg, v, ny_ctrue(cg), NY_LLVM_NAME(cg, "layout_bool"));
}

static LLVMValueRef layout_store_cast_int(codegen_t *cg, LLVMValueRef v,
                                          LLVMTypeRef target,
                                          const char *type_name) {
  if (!v || !target)
    return v;
  if (LLVMTypeOf(v) == target && target != cg->type_i64)
    return v;
  if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind)
    v = ny_ptr2i64(cg, v, NY_LLVM_NAME(cg, "layout_int_ptr"));
  if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMFloatTypeKind ||
      LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMDoubleTypeKind ||
      LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMFP128TypeKind) {
    bool is_unsigned = abi_type_is_unsigned_int(type_name);
    return is_unsigned ? LLVMBuildFPToUI(cg->builder, v, target,
                                         NY_LLVM_NAME(cg, "layout_f2u"))
                       : LLVMBuildFPToSI(cg->builder, v, target,
                                         NY_LLVM_NAME(cg, "layout_f2i"));
  }
  if (LLVMTypeOf(v) != cg->type_i64)
    v = LLVMBuildZExtOrBitCast(cg->builder, v, cg->type_i64,
                               NY_LLVM_NAME(cg, "layout_int_i64"));
  LLVMValueRef raw =
      ny_build_untagged_or_raw_i64(cg, v, NY_LLVM_NAME(cg, "layout_int_raw"));
  return abi_cast_int(cg, raw, target, !abi_type_is_unsigned_int(type_name));
}

static LLVMValueRef layout_store_cast_float(codegen_t *cg, LLVMValueRef v,
                                            LLVMTypeRef target,
                                            const char *type_name) {
  if (!v || !target)
    return v;
  LLVMTypeKind vk = LLVMGetTypeKind(LLVMTypeOf(v));
  if (LLVMTypeOf(v) == target)
    return v;
  if (vk == LLVMFloatTypeKind || vk == LLVMDoubleTypeKind ||
      vk == LLVMFP128TypeKind) {
    if (LLVMTypeOf(v) == cg->type_f32 && target != cg->type_f32)
      return LLVMBuildFPExt(cg->builder, v, target,
                            NY_LLVM_NAME(cg, "layout_fext"));
    if (LLVMTypeOf(v) != cg->type_f32 && target == cg->type_f32)
      return LLVMBuildFPTrunc(cg->builder, v, target,
                              NY_LLVM_NAME(cg, "layout_ftrunc"));
    return ny_bitcast(cg, v, target, NY_LLVM_NAME(cg, "layout_fcast"));
  }
  if (vk == LLVMIntegerTypeKind && LLVMTypeOf(v) != cg->type_i64) {
    v = LLVMBuildSExtOrBitCast(cg->builder, v, cg->type_i64,
                               NY_LLVM_NAME(cg, "layout_f_i64"));
  }
  LLVMValueRef raw = ny_coerce_to_abi(cg, v, type_name);
  if (LLVMTypeOf(raw) == target)
    return raw;
  if (LLVMGetTypeKind(LLVMTypeOf(raw)) == LLVMFloatTypeKind ||
      LLVMGetTypeKind(LLVMTypeOf(raw)) == LLVMDoubleTypeKind ||
      LLVMGetTypeKind(LLVMTypeOf(raw)) == LLVMFP128TypeKind) {
    if (target == cg->type_f32)
      return LLVMBuildFPTrunc(cg->builder, raw, target,
                              NY_LLVM_NAME(cg, "layout_raw_ftrunc"));
    return LLVMBuildFPExt(cg->builder, raw, target,
                          NY_LLVM_NAME(cg, "layout_raw_fext"));
  }
  return raw;
}

static LLVMValueRef layout_store_cast_ptr(codegen_t *cg, LLVMValueRef v,
                                          LLVMTypeRef target,
                                          const char *type_name) {
  if (!v || !target)
    return v;
  if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind)
    return LLVMBuildBitCast(cg->builder, v, target,
                            NY_LLVM_NAME(cg, "layout_ptr_cast"));
  if (LLVMTypeOf(v) != cg->type_i64)
    v = LLVMBuildZExtOrBitCast(cg->builder, v, cg->type_i64,
                               NY_LLVM_NAME(cg, "layout_ptr_i64"));
  LLVMValueRef raw =
      strcmp(abi_skip_nullable(type_name), "str") == 0
          ? v
          : ny_build_rt_untag_i64(cg, v, NY_LLVM_NAME(cg, "layout_ptr_raw"));
  return LLVMBuildIntToPtr(cg->builder, raw, target,
                           NY_LLVM_NAME(cg, "layout_ptr"));
}

static LLVMValueRef layout_store_cast_value(codegen_t *cg, LLVMValueRef v,
                                            layout_field_info_t *field,
                                            LLVMTypeRef target) {
  if (!cg || !v || !field || !field->type_name || !target)
    return v;
  const char *type_name = abi_skip_nullable(field->type_name);
  if (strcmp(type_name, "bool") == 0)
    return layout_store_bool_value(cg, v);
  LLVMTypeKind tk = LLVMGetTypeKind(target);
  if (tk == LLVMIntegerTypeKind)
    return layout_store_cast_int(cg, v, target, type_name);
  if (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind ||
      tk == LLVMFP128TypeKind)
    return layout_store_cast_float(cg, v, target, type_name);
  if (tk == LLVMPointerTypeKind)
    return layout_store_cast_ptr(cg, v, target, type_name);
  return v;
}

static LLVMValueRef emit_store_layout(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *e, expr_call_t *c) {
  if (!cg || !c)
    return ny_c0(cg);
  if (c->args.len < 2) {
    ny_diag_error(e->tok,
                  "store_layout expects ptr, layout name, and field values");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  const char *layout_name = NULL;
  if (!layout_query_arg(c->args.data[1].val, &layout_name)) {
    emit_nonliteral_call_diag(
        cg, e->tok, "store_layout", 1,
        "store_layout expects a string literal layout name as argument 2",
        "use store_layout(dst, \"LayoutName\", ...)");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  layout_def_t *def = lookup_layout(cg, layout_name);
  if (!def) {
    ny_diag_error(e->tok, "unknown layout '%s'", layout_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  if (c->args.len != def->fields.len + 2) {
    ny_diag_error(e->tok,
                  "store_layout('%s') expects %zu field value(s), got %zu",
                  layout_name, def->fields.len, c->args.len - 2);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMValueRef dst_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  if (!dst_v) {
    ny_diag_error(e->tok, "failed to evaluate store_layout destination");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMValueRef dst_raw = dst_v;
  if (LLVMGetTypeKind(LLVMTypeOf(dst_raw)) == LLVMPointerTypeKind) {
    dst_raw = ny_ptr2i64(cg, dst_raw, NY_LLVM_NAME(cg, "store_layout_dst_i64"));
  } else {
    dst_raw = ny_cast_to_i64(cg, dst_raw, "store_layout_dst");
    dst_raw = ny_build_rt_untag_i64(cg, dst_raw,
                                    NY_LLVM_NAME(cg, "store_layout_dst_raw"));
  }

  ny_dbg_loc(cg, e->tok);
  for (size_t i = 0; i < def->fields.len; i++) {
    layout_field_info_t *field = &def->fields.data[i];
    LLVMValueRef val_v = gen_expr(cg, scopes, depth, c->args.data[i + 2].val);
    if (!val_v) {
      ny_diag_error(e->tok, "failed to evaluate store_layout field '%s'",
                    field->name ? field->name : "<field>");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    type_layout_t tl = resolve_raw_layout(cg, field->type_name, e->tok);
    if (!tl.is_valid || !tl.llvm_type)
      return ny_c0(cg);
    LLVMValueRef raw = layout_store_cast_value(cg, val_v, field, tl.llvm_type);
    if (LLVMTypeOf(raw) != tl.llvm_type)
      raw = LLVMBuildBitCast(cg->builder, raw, tl.llvm_type,
                             NY_LLVM_NAME(cg, "layout_store_cast"));
    LLVMValueRef off =
        LLVMConstInt(cg->type_i64, (uint64_t)field->offset, false);
    LLVMValueRef addr =
        ny_add(cg, dst_raw, off, NY_LLVM_NAME(cg, "layout_store_addr"));
    LLVMValueRef ptr =
        LLVMBuildIntToPtr(cg->builder, addr, LLVMPointerType(tl.llvm_type, 0),
                          NY_LLVM_NAME(cg, "layout_store_ptr"));
    LLVMValueRef st = LLVMBuildStore(cg->builder, raw, ptr);
    LLVMSetAlignment(st, field->align ? (unsigned)field->align : 1);
  }
  return dst_v;
}

static LLVMValueRef emit_layout_construct(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e,
                                          expr_call_t *c, layout_def_t *def) {
  if (!cg || !c || !def)
    return ny_c0(cg);
  if (c->args.len != def->fields.len) {
    ny_diag_error(e->tok, "layout '%s' expects %zu field value(s), got %zu",
                  def->name ? def->name : "<layout>", def->fields.len,
                  c->args.len);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
  if (!malloc_sig) {
    ny_diag_error(e->tok, "__malloc required for layout constructor");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMValueRef size_arg =
      LLVMConstInt(cg->type_i64, (((uint64_t)def->size << 1) | 1u), false);
  LLVMValueRef dst_v =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     &size_arg, 1, NY_LLVM_NAME(cg, "layout_ctor_alloc"));
  LLVMValueRef dst_raw = ny_cast_to_i64(cg, dst_v, "layout_ctor_dst");
  dst_raw = ny_build_rt_untag_i64(cg, dst_raw,
                                  NY_LLVM_NAME(cg, "layout_ctor_dst_raw"));

  ny_dbg_loc(cg, e->tok);
  for (size_t i = 0; i < def->fields.len; i++) {
    layout_field_info_t *field = &def->fields.data[i];
    LLVMValueRef val_v = gen_expr(cg, scopes, depth, c->args.data[i].val);
    if (!val_v) {
      ny_diag_error(e->tok, "failed to evaluate layout constructor field '%s'",
                    field->name ? field->name : "<field>");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    type_layout_t tl = resolve_raw_layout(cg, field->type_name, e->tok);
    if (!tl.is_valid || !tl.llvm_type)
      return ny_c0(cg);
    LLVMValueRef raw = layout_store_cast_value(cg, val_v, field, tl.llvm_type);
    if (LLVMTypeOf(raw) != tl.llvm_type)
      raw = LLVMBuildBitCast(cg->builder, raw, tl.llvm_type,
                             NY_LLVM_NAME(cg, "layout_ctor_cast"));
    LLVMValueRef off =
        LLVMConstInt(cg->type_i64, (uint64_t)field->offset, false);
    LLVMValueRef addr =
        ny_add(cg, dst_raw, off, NY_LLVM_NAME(cg, "layout_ctor_addr"));
    LLVMValueRef ptr =
        LLVMBuildIntToPtr(cg->builder, addr, LLVMPointerType(tl.llvm_type, 0),
                          NY_LLVM_NAME(cg, "layout_ctor_ptr"));
    LLVMValueRef st = LLVMBuildStore(cg->builder, raw, ptr);
    LLVMSetAlignment(st, field->align ? (unsigned)field->align : 1);
  }
  return dst_v;
}

static LLVMValueRef emit_load_layout(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e, expr_call_t *c) {
  if (!cg || !c)
    return ny_c0(cg);
  if (c->args.len != 3) {
    ny_diag_error(e->tok,
                  "load_layout expects ptr, layout name, and field name");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  const char *layout_name = NULL;
  const char *field_name = NULL;
  bool layout_ok = layout_query_arg(c->args.data[1].val, &layout_name);
  bool field_ok = layout_query_arg(c->args.data[2].val, &field_name);
  if (!layout_ok || !field_ok) {
    emit_nonliteral_call_diag(
        cg, e->tok, "load_layout", layout_ok ? 2 : 1,
        "load_layout expects string literal layout and field names",
        "use load_layout(src, \"LayoutName\", \"field\")");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  layout_def_t *def = lookup_layout(cg, layout_name);
  if (!def) {
    ny_diag_error(e->tok, "unknown layout '%s'", layout_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  layout_field_info_t *field = NULL;
  for (size_t i = 0; i < def->fields.len; i++) {
    layout_field_info_t *it = &def->fields.data[i];
    if (it->name && strcmp(it->name, field_name) == 0) {
      field = it;
      break;
    }
  }
  if (!field) {
    ny_diag_error(e->tok, "unknown field '%s' in layout '%s'", field_name,
                  layout_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }

  type_layout_t tl = resolve_raw_layout(cg, field->type_name, e->tok);
  if (!tl.is_valid || !tl.llvm_type)
    return ny_c0(cg);

  LLVMValueRef src_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  if (!src_v) {
    ny_diag_error(e->tok, "failed to evaluate load_layout source");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMValueRef src_raw = src_v;
  if (LLVMGetTypeKind(LLVMTypeOf(src_raw)) == LLVMPointerTypeKind) {
    src_raw = ny_ptr2i64(cg, src_raw, NY_LLVM_NAME(cg, "load_layout_src_i64"));
  } else {
    src_raw = ny_cast_to_i64(cg, src_raw, "load_layout_src");
    src_raw = ny_build_rt_untag_i64(cg, src_raw,
                                    NY_LLVM_NAME(cg, "load_layout_src_raw"));
  }

  ny_dbg_loc(cg, e->tok);
  LLVMValueRef off = LLVMConstInt(cg->type_i64, (uint64_t)field->offset, false);
  LLVMValueRef addr =
      ny_add(cg, src_raw, off, NY_LLVM_NAME(cg, "layout_load_addr"));
  LLVMValueRef ptr =
      LLVMBuildIntToPtr(cg->builder, addr, LLVMPointerType(tl.llvm_type, 0),
                        NY_LLVM_NAME(cg, "layout_load_ptr"));
  LLVMValueRef raw = LLVMBuildLoad2(cg->builder, tl.llvm_type, ptr,
                                    NY_LLVM_NAME(cg, "layout_load_raw"));
  LLVMSetAlignment(raw, field->align ? (unsigned)field->align : 1);

  const char *type_name = abi_skip_nullable(field->type_name);
  if (type_name && strcmp(type_name, "bool") == 0) {
    LLVMValueRef pred = LLVMBuildICmp(cg->builder, LLVMIntNE, raw,
                                      LLVMConstNull(LLVMTypeOf(raw)),
                                      NY_LLVM_NAME(cg, "layout_load_bool"));
    return ny_gencall_tag_bool(cg, pred, NY_LLVM_NAME(cg, "layout_bool"));
  }

  LLVMTypeKind tk = LLVMGetTypeKind(LLVMTypeOf(raw));
  if (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind ||
      tk == LLVMFP128TypeKind) {
    if (LLVMTypeOf(raw) == cg->type_f32) {
      fun_sig *box32 = lookup_fun(cg, "__flt_box_val32", 0);
      if (!box32) {
        ny_diag_error(e->tok, "__flt_box_val32 not found for load_layout");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      LLVMValueRef bits32 = LLVMBuildBitCast(
          cg->builder, raw, cg->type_i32, NY_LLVM_NAME(cg, "layout_f32_bits"));
      LLVMValueRef bits64 = LLVMBuildZExt(cg->builder, bits32, cg->type_i64,
                                          NY_LLVM_NAME(cg, "layout_f32_ext"));
      LLVMValueRef tagged = ny_tag_int(cg, bits64);
      return LLVMBuildCall2(cg->builder, box32->type, box32->value,
                            (LLVMValueRef[]){tagged}, 1,
                            NY_LLVM_NAME(cg, "layout_f32_box"));
    }
    fun_sig *box = ny_gencall_flt_box(cg);
    if (!box) {
      ny_diag_error(e->tok, "__flt_box_val not found for load_layout");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    LLVMValueRef bits = LLVMBuildBitCast(cg->builder, raw, cg->type_i64,
                                         NY_LLVM_NAME(cg, "layout_f64_bits"));
    return LLVMBuildCall2(cg->builder, box->type, box->value,
                          (LLVMValueRef[]){bits}, 1,
                          NY_LLVM_NAME(cg, "layout_f64_box"));
  }

  LLVMValueRef as_i64 = raw;
  if (LLVMGetTypeKind(LLVMTypeOf(as_i64)) == LLVMPointerTypeKind) {
    as_i64 = ny_ptr2i64(cg, as_i64, NY_LLVM_NAME(cg, "layout_ptr_i64"));
  } else if (LLVMTypeOf(as_i64) != cg->type_i64) {
    as_i64 = abi_type_is_unsigned_int(type_name)
                 ? LLVMBuildZExtOrBitCast(cg->builder, as_i64, cg->type_i64,
                                          NY_LLVM_NAME(cg, "layout_int_zext"))
                 : LLVMBuildSExtOrBitCast(cg->builder, as_i64, cg->type_i64,
                                          NY_LLVM_NAME(cg, "layout_int_sext"));
  }
  if (type_name &&
      (strcmp(type_name, "str") == 0 || strcmp(type_name, "ptr") == 0 ||
       strcmp(type_name, "fnptr") == 0 || type_name[0] == '*')) {
    return as_i64;
  }
  return ny_tag_int(cg, as_i64);
}

static void report_missing_runtime_call_helper(codegen_t *cg, token_t tok,
                                               const char *name, size_t want) {
  ny_diag_error(tok, "undefined runtime call helper '%s'", name);
  const char *best_match = NULL;
  int best_delta = 1 << 30;
  int max_supported = -1;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const char *candidate = cg->fun_sigs.data[i].name;
    int ar = parse_runtime_call_arity(candidate);
    if (ar >= 0) {
      if (ar > max_supported)
        max_supported = ar;
      int delta = ar - (int)want;
      if (delta < 0)
        delta = -delta;
      if (delta < best_delta) {
        best_delta = delta;
        best_match = candidate;
      }
    }
    if (strstr(candidate, name) || strstr(name, candidate)) {
      best_match = candidate;
      break;
    }
  }
  if (best_match)
    ny_diag_hint("did you mean '%s'?", best_match);
  if (max_supported >= 0 && (int)want > max_supported) {
    ny_diag_hint("runtime supports function calls up to %d arguments",
                 max_supported);
  }
  ny_diag_hint("runtime/library mismatch can cause missing __callN helpers");
  cg->had_error = 1;
}

static bool check_call_arity_diag(codegen_t *cg, token_t tok,
                                  fun_sig *sig_found, bool is_variadic,
                                  int sig_arity, size_t call_argc,
                                  bool member_with_target) {
  size_t min_arity = (size_t)sig_arity;
  if (!is_variadic && sig_found)
    min_arity = ny_sig_min_arity(sig_found);
  if (!is_variadic &&
      (call_argc < min_arity || call_argc > (size_t)sig_arity)) {
    bool emit_diag = ny_diag_should_emit("arity_mismatch", tok,
                                         sig_found ? sig_found->name : "call");
    if (emit_diag) {
      ny_diag_error(tok, "arity mismatch for \033[1;37m'%s'\033[0m",
                    sig_found->name);
      if (min_arity != (size_t)sig_arity) {
        ny_diag_hint("expected %zu..%d arguments, got %zu", min_arity,
                     sig_arity, call_argc);
      } else {
        ny_diag_hint("expected %d arguments, got %zu", sig_arity, call_argc);
      }
      if (member_with_target)
        ny_diag_hint(
            "member calls pass the target object as the first argument");
      if (call_argc < min_arity) {
        ny_diag_fix("call '%s' with at least %zu argument(s)", sig_found->name,
                    min_arity);
      } else {
        ny_diag_fix("call '%s' with %d argument(s)", sig_found->name,
                    sig_arity);
      }

      if (sig_found && sig_found->stmt_t &&
          sig_found->stmt_t->kind == NY_S_FUNC) {
        ny_param_list *params = &sig_found->stmt_t->as.fn.params;
        char sig_buf[512] = "(";
        for (size_t k = 0; k < params->len; k++) {
          param_t *p = &params->data[k];
          if (p->name) {
            strncat(sig_buf, p->name, sizeof(sig_buf) - strlen(sig_buf) - 1);
            if (p->def)
              strncat(sig_buf, "?", sizeof(sig_buf) - strlen(sig_buf) - 1);
          } else {
            strncat(sig_buf, "_", sizeof(sig_buf) - strlen(sig_buf) - 1);
          }
          if (k < params->len - 1) {
            strncat(sig_buf, ", ", sizeof(sig_buf) - strlen(sig_buf) - 1);
          }
        }
        strncat(sig_buf, ")", sizeof(sig_buf) - strlen(sig_buf) - 1);
        ny_diag_hint("correct signature for '%s' is %s", sig_found->name,
                     sig_buf);
      }
    }

    if (sig_found && sig_found->name) {
      if (strcmp(sig_found->name, "std.os.ui.render.draw_text") == 0 ||
          strcmp(sig_found->name, "draw_text") == 0) {
        ny_diag_hint("signature is: (font, text, x, y, [color])");
      } else if (strcmp(sig_found->name, "std.os.ui.render.draw_line") == 0 ||
                 strcmp(sig_found->name, "draw_line") == 0) {
        ny_diag_hint("signature is: (start_v2, end_v2, color, [thickness])");
        ny_diag_hint(
            "use 'draw_line_2d' for (x1, y1, x2, y2, color, [thickness])");
      } else if (strcmp(sig_found->name, "std.os.ui.render.font_load") == 0 ||
                 strcmp(sig_found->name, "font_load") == 0) {
        ny_diag_hint("signature is: (path, size)");
      }
    }
    cg->had_error = 1;
    return false;
  }
  if (is_variadic && call_argc < (size_t)sig_arity - 1) {
    ny_diag_error(tok,
                  "not enough arguments for variadic \033[1;37m'%s'\033[0m",
                  sig_found->name);
    ny_diag_hint("expected at least %d arguments, got %zu", sig_arity - 1,
                 call_argc);
    ny_diag_fix("add %d more argument(s) or use a non-variadic overload",
                (sig_arity - 1) - (int)call_argc);
    cg->had_error = 1;
    return false;
  }
  return true;
}

static int ny_mono_env_int(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end == v)
    return fallback;
  if (n < 0)
    return 0;
  if (n > 1000000)
    return 1000000;
  return (int)n;
}

static bool ny_mono_enabled(codegen_t *cg) {
  const char *forced = getenv("NYTRIX_MONO_TYPES");
  if (!forced || !*forced)
    forced = getenv("NYTRIX_ENABLE_MONOMORPHIZATION");
  if (forced && *forced)
    return ny_env_truthy(forced);
  if (ny_env_enabled("NYTRIX_DISABLE_MONO_TYPES") ||
      ny_env_enabled("NYTRIX_DISABLE_MONOMORPHIZATION"))
    return false;
  if (ny_env_enabled("NYTRIX_MONO_LIST_ARGS"))
    return true;
  return ny_codegen_speed_profile_enabled(cg);
}

static bool ny_mono_trace_enabled(void) {
  return ny_env_enabled("NYTRIX_MONO_TRACE") ||
         ny_env_enabled("NYTRIX_TRACE_MONO_TYPES");
}

static LLVMValueRef ny_try_inline_simple_raw_int_call(codegen_t *cg,
                                                      scope *scopes,
                                                      size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return NULL;
  if (!ny_codegen_speed_profile_enabled(cg) &&
      !ny_env_enabled("NYTRIX_SIMPLE_RAW_INT_CALL_FAST"))
    return NULL;
  if (!ny_is_proven_int(cg, scopes, depth, e, NULL))
    return NULL;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw ||
      !ok)
    return NULL;
  if (!LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return NULL;
  cg->mono_inline_body_uses++;
  return ny_tag_int(cg, raw);
}

static const char *ny_mono_type_name(uint8_t kind) {
  switch ((ny_mono_type_kind_t)kind) {
  case NY_MONO_TYPE_INT:
    return "int";
  case NY_MONO_TYPE_F64:
    return "f64";
  case NY_MONO_TYPE_LIST:
    return "list";
  case NY_MONO_TYPE_F64_LIST:
    return "list";
  default:
    return NULL;
  }
}

static bool ny_mono_type_is_raw_scalar(uint8_t kind) {
  return kind == NY_MONO_TYPE_INT || kind == NY_MONO_TYPE_F64;
}

static char ny_mono_type_suffix(uint8_t kind) {
  switch ((ny_mono_type_kind_t)kind) {
  case NY_MONO_TYPE_INT:
    return 'i';
  case NY_MONO_TYPE_F64:
    return 'd';
  case NY_MONO_TYPE_LIST:
    return 'l';
  case NY_MONO_TYPE_F64_LIST:
    return 'q';
  default:
    return 'x';
  }
}

static ny_mono_type_kind_t ny_mono_expr_kind(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *expr) {
  if (!expr)
    return NY_MONO_TYPE_NONE;
  if (ny_is_proven_int(cg, scopes, depth, expr, NULL))
    return NY_MONO_TYPE_INT;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_FLOAT)
    return NY_MONO_TYPE_F64;
  const char *ty = infer_expr_type(cg, scopes, depth, expr);
  if (ny_type_is(ty, "f64"))
    return NY_MONO_TYPE_F64;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t name_len = (size_t)expr->tok.len;
    if (name_len == 0)
      name_len = strlen(expr->as.ident.name);
    binding *b = ny_gencall_lookup_binding(
        cg, scopes, depth, expr->as.ident.name, name_len, expr->as.ident.hash);
    if (b && b->is_f64_list_storage)
      return NY_MONO_TYPE_F64_LIST;
  }
  if (ny_type_is(ty, "list") ||
      ny_expr_is_direct_list_storage(cg, scopes, depth, expr))
    return NY_MONO_TYPE_LIST;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t name_len = (size_t)expr->tok.len;
    if (name_len == 0)
      name_len = strlen(expr->as.ident.name);
    binding *b = ny_gencall_lookup_binding(
        cg, scopes, depth, expr->as.ident.name, name_len, expr->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct))
      return NY_MONO_TYPE_F64;
  }
  return NY_MONO_TYPE_NONE;
}

static size_t ny_mono_expr_cost(expr_t *e);

static size_t ny_mono_stmt_cost(stmt_t *s) {
  if (!s)
    return 0;
  size_t cost = 1;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      cost += ny_mono_stmt_cost(s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      cost += ny_mono_expr_cost(s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    cost += ny_mono_expr_cost(s->as.expr.expr);
    break;
  case NY_S_IF:
    cost += ny_mono_expr_cost(s->as.iff.test);
    cost += ny_mono_stmt_cost(s->as.iff.init);
    cost += ny_mono_stmt_cost(s->as.iff.conseq);
    cost += ny_mono_stmt_cost(s->as.iff.alt);
    break;
  case NY_S_GUARD:
    cost += ny_mono_expr_cost(s->as.guard.value);
    cost += ny_mono_stmt_cost(s->as.guard.fallback);
    break;
  case NY_S_WHILE:
    cost += ny_mono_expr_cost(s->as.whl.test);
    cost += ny_mono_stmt_cost(s->as.whl.init);
    cost += ny_mono_stmt_cost(s->as.whl.body);
    cost += ny_mono_stmt_cost(s->as.whl.update);
    break;
  case NY_S_FOR:
    cost += ny_mono_stmt_cost(s->as.fr.init);
    cost += ny_mono_expr_cost(s->as.fr.cond);
    cost += ny_mono_expr_cost(s->as.fr.iterable);
    cost += ny_mono_stmt_cost(s->as.fr.body);
    cost += ny_mono_stmt_cost(s->as.fr.update);
    break;
  case NY_S_MATCH:
    cost += ny_mono_expr_cost(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      for (size_t j = 0; j < s->as.match.arms.data[i].patterns.len; j++)
        cost += ny_mono_expr_cost(s->as.match.arms.data[i].patterns.data[j]);
      cost += ny_mono_expr_cost(s->as.match.arms.data[i].guard);
      cost += ny_mono_stmt_cost(s->as.match.arms.data[i].conseq);
    }
    cost += ny_mono_stmt_cost(s->as.match.default_conseq);
    break;
  case NY_S_RETURN:
    cost += ny_mono_expr_cost(s->as.ret.value);
    break;
  default:
    break;
  }
  return cost;
}

static size_t ny_mono_expr_cost(expr_t *e) {
  if (!e)
    return 0;
  size_t cost = 1;
  switch (e->kind) {
  case NY_E_UNARY:
    cost += ny_mono_expr_cost(e->as.unary.right);
    break;
  case NY_E_BINARY:
    cost += ny_mono_expr_cost(e->as.binary.left) +
            ny_mono_expr_cost(e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    cost += ny_mono_expr_cost(e->as.logical.left) +
            ny_mono_expr_cost(e->as.logical.right);
    break;
  case NY_E_TERNARY:
    cost += ny_mono_expr_cost(e->as.ternary.cond) +
            ny_mono_expr_cost(e->as.ternary.true_expr) +
            ny_mono_expr_cost(e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    cost += ny_mono_expr_cost(e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; i++)
      cost += ny_mono_expr_cost(e->as.call.args.data[i].val);
    break;
  case NY_E_MEMCALL:
    cost += ny_mono_expr_cost(e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; i++)
      cost += ny_mono_expr_cost(e->as.memcall.args.data[i].val);
    break;
  case NY_E_INDEX:
    cost += ny_mono_expr_cost(e->as.index.target) +
            ny_mono_expr_cost(e->as.index.start) +
            ny_mono_expr_cost(e->as.index.stop) +
            ny_mono_expr_cost(e->as.index.step);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++)
      cost += ny_mono_expr_cost(e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      cost += ny_mono_expr_cost(e->as.dict.pairs.data[i].key);
      cost += ny_mono_expr_cost(e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        cost += ny_mono_expr_cost(e->as.fstring.parts.data[i].as.e);
    break;
  case NY_E_MATCH:
    cost += ny_mono_stmt_cost((stmt_t *)e->as.match.default_conseq);
    break;
  case NY_E_MEMBER:
    cost += ny_mono_expr_cost(e->as.member.target);
    break;
  case NY_E_PTR_TYPE:
    cost += ny_mono_expr_cost(e->as.ptr_type.target);
    break;
  case NY_E_DEREF:
    cost += ny_mono_expr_cost(e->as.deref.target);
    break;
  case NY_E_SIZEOF:
    cost += ny_mono_expr_cost(e->as.szof.target);
    break;
  case NY_E_TRY:
    cost += ny_mono_expr_cost(e->as.try_expr.target);
    break;
  default:
    break;
  }
  return cost;
}

static bool ny_mono_expr_refs_self(expr_t *e, const char *base_name,
                                   const char *tail_name);

static bool ny_mono_stmt_refs_self(stmt_t *s, const char *base_name,
                                   const char *tail_name) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      if (ny_mono_stmt_refs_self(s->as.block.body.data[i], base_name,
                                 tail_name))
        return true;
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      if (ny_mono_expr_refs_self(s->as.var.exprs.data[i], base_name, tail_name))
        return true;
    return false;
  case NY_S_EXPR:
    return ny_mono_expr_refs_self(s->as.expr.expr, base_name, tail_name);
  case NY_S_IF:
    return ny_mono_expr_refs_self(s->as.iff.test, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.iff.init, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.iff.conseq, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.iff.alt, base_name, tail_name);
  case NY_S_WHILE:
    return ny_mono_expr_refs_self(s->as.whl.test, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.whl.init, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.whl.body, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.whl.update, base_name, tail_name);
  case NY_S_FOR:
    return ny_mono_stmt_refs_self(s->as.fr.init, base_name, tail_name) ||
           ny_mono_expr_refs_self(s->as.fr.cond, base_name, tail_name) ||
           ny_mono_expr_refs_self(s->as.fr.iterable, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.fr.body, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.fr.update, base_name, tail_name);
  case NY_S_RETURN:
    return ny_mono_expr_refs_self(s->as.ret.value, base_name, tail_name);
  default:
    return false;
  }
}

static bool ny_mono_expr_refs_self(expr_t *e, const char *base_name,
                                   const char *tail_name) {
  if (!e)
    return false;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *n = e->as.call.callee->as.ident.name;
    if (n && ((base_name && strcmp(n, base_name) == 0) ||
              (tail_name && strcmp(n, tail_name) == 0)))
      return true;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    return ny_mono_expr_refs_self(e->as.unary.right, base_name, tail_name);
  case NY_E_BINARY:
    return ny_mono_expr_refs_self(e->as.binary.left, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.binary.right, base_name, tail_name);
  case NY_E_LOGICAL:
    return ny_mono_expr_refs_self(e->as.logical.left, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.logical.right, base_name, tail_name);
  case NY_E_TERNARY:
    return ny_mono_expr_refs_self(e->as.ternary.cond, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.ternary.true_expr, base_name,
                                  tail_name) ||
           ny_mono_expr_refs_self(e->as.ternary.false_expr, base_name,
                                  tail_name);
  case NY_E_CALL:
    if (ny_mono_expr_refs_self(e->as.call.callee, base_name, tail_name))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; i++)
      if (ny_mono_expr_refs_self(e->as.call.args.data[i].val, base_name,
                                 tail_name))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (ny_mono_expr_refs_self(e->as.memcall.target, base_name, tail_name))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; i++)
      if (ny_mono_expr_refs_self(e->as.memcall.args.data[i].val, base_name,
                                 tail_name))
        return true;
    return false;
  case NY_E_INDEX:
    return ny_mono_expr_refs_self(e->as.index.target, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.index.start, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.index.stop, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.index.step, base_name, tail_name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++)
      if (ny_mono_expr_refs_self(e->as.list_like.data[i], base_name, tail_name))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++)
      if (ny_mono_expr_refs_self(e->as.dict.pairs.data[i].key, base_name,
                                 tail_name) ||
          ny_mono_expr_refs_self(e->as.dict.pairs.data[i].value, base_name,
                                 tail_name))
        return true;
    return false;
  case NY_E_MEMBER:
    return ny_mono_expr_refs_self(e->as.member.target, base_name, tail_name);
  default:
    return false;
  }
}

static bool ny_mono_stmt_has_unsupported(stmt_t *s) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_TRY:
  case NY_S_DEFER:
  case NY_S_GOTO:
  case NY_S_LABEL:
  case NY_S_FUNC:
  case NY_S_EXTERN:
  case NY_S_MODULE:
  case NY_S_MACRO:
  case NY_S_INCLUDE:
    return true;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      if (ny_mono_stmt_has_unsupported(s->as.block.body.data[i]))
        return true;
    return false;
  case NY_S_IF:
    return ny_mono_stmt_has_unsupported(s->as.iff.init) ||
           ny_mono_stmt_has_unsupported(s->as.iff.conseq) ||
           ny_mono_stmt_has_unsupported(s->as.iff.alt);
  case NY_S_WHILE:
    if (!ny_env_enabled("NYTRIX_MONO_IMPERATIVE"))
      return true;
    return ny_mono_stmt_has_unsupported(s->as.whl.init) ||
           ny_mono_stmt_has_unsupported(s->as.whl.body) ||
           ny_mono_stmt_has_unsupported(s->as.whl.update);
  case NY_S_FOR:
    if (!ny_env_enabled("NYTRIX_MONO_IMPERATIVE"))
      return true;
    return ny_mono_stmt_has_unsupported(s->as.fr.init) ||
           ny_mono_stmt_has_unsupported(s->as.fr.body) ||
           ny_mono_stmt_has_unsupported(s->as.fr.update);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      if (ny_mono_stmt_has_unsupported(s->as.match.arms.data[i].conseq))
        return true;
    return ny_mono_stmt_has_unsupported(s->as.match.default_conseq);
  default:
    return false;
  }
}

static uint64_t ny_mono_key_hash(const char *base_name, const uint8_t *types,
                                 int arity) {
  uint64_t h = ny_hash64_cstr(base_name ? base_name : "");
  h = ny_hash64_u64(h, (uint64_t)(uint32_t)arity);
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++)
    h = ny_hash64_u64(h, (uint64_t)types[i] + ((uint64_t)i << 8));
  return h;
}

static bool ny_mono_types_equal(const uint8_t *a, const uint8_t *b, int arity) {
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++)
    if (a[i] != b[i])
      return false;
  return true;
}

static uint64_t ny_mono_key_hash_with_list_lens(
    uint64_t h, const bool *list_len_known, const int64_t *list_len_raw,
    int arity) {
  bool any_known = false;
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++) {
    if (list_len_known && list_len_known[i]) {
      any_known = true;
      break;
    }
  }
  if (!any_known)
    return h;
  h = ny_hash64_u64(h, UINT64_C(0x4e594d4f4e4f4c4c));
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++) {
    h = ny_hash64_u64(h, (uint64_t)(list_len_known && list_len_known[i]));
    if (list_len_known && list_len_known[i] && list_len_raw)
      h = ny_hash64_u64(h, (uint64_t)list_len_raw[i]);
  }
  return h;
}

static bool ny_mono_list_lens_equal(const ny_mono_specialization_t *spec,
                                    const bool *list_len_known,
                                    const int64_t *list_len_raw, int arity) {
  if (!spec)
    return false;
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++) {
    bool known = list_len_known && list_len_known[i];
    if (spec->arg_list_len_min_known[i] != known)
      return false;
    if (known && list_len_raw &&
        spec->arg_list_len_min_raw[i] != list_len_raw[i])
      return false;
  }
  return true;
}

static fun_sig *ny_mono_lookup_existing(codegen_t *cg, stmt_t *base_stmt,
                                        uint64_t key_hash, const uint8_t *types,
                                        const bool *list_len_known,
                                        const int64_t *list_len_raw,
                                        int arity) {
  if (!cg || !base_stmt)
    return NULL;
  for (size_t i = 0; i < cg->mono_specs.len; i++) {
    ny_mono_specialization_t *spec = &cg->mono_specs.data[i];
    if (spec->base_stmt == base_stmt && spec->key_hash == key_hash &&
        spec->arity == arity &&
        ny_mono_types_equal(spec->types, types, arity) &&
        ny_mono_list_lens_equal(spec, list_len_known, list_len_raw, arity)) {
      return lookup_fun_exact(cg, spec->specialized_name);
    }
  }
  return NULL;
}

static size_t ny_mono_count_for_base(codegen_t *cg, stmt_t *base_stmt) {
  size_t n = 0;
  if (!cg || !base_stmt)
    return 0;
  for (size_t i = 0; i < cg->mono_specs.len; i++)
    if (cg->mono_specs.data[i].base_stmt == base_stmt)
      n++;
  return n;
}

static const char *ny_mono_make_name(codegen_t *cg, const char *base_name,
                                     const uint8_t *types, int arity,
                                     uint64_t key_hash) {
  if (!cg || !base_name)
    return NULL;
  char type_buf[NY_MONO_MAX_ARITY + 1];
  int n = arity < NY_MONO_MAX_ARITY ? arity : NY_MONO_MAX_ARITY;
  for (int i = 0; i < n; i++)
    type_buf[i] = ny_mono_type_suffix(types[i]);
  type_buf[n] = '\0';
  char hash_buf[32];
  snprintf(hash_buf, sizeof(hash_buf), "%08llx",
           (unsigned long long)(key_hash & 0xffffffffu));
  size_t len = strlen(base_name) + strlen("__ny_mono_") + strlen(type_buf) + 1 +
               strlen(hash_buf) + 1;
  char *out = arena_alloc(cg->arena, len);
  snprintf(out, len, "%s__ny_mono_%s_%s", base_name, type_buf, hash_buf);
  return out;
}

static ny_mono_type_kind_t ny_mono_static_expr_kind(expr_t *e,
                                                    const char **names,
                                                    const uint8_t *types,
                                                    int arity) {
  if (!e)
    return NY_MONO_TYPE_NONE;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT)
      return NY_MONO_TYPE_INT;
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return NY_MONO_TYPE_F64;
    return NY_MONO_TYPE_NONE;
  case NY_E_IDENT:
    if (!e->as.ident.name)
      return NY_MONO_TYPE_NONE;
    for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++)
      if (names[i] && strcmp(names[i], e->as.ident.name) == 0)
        return (ny_mono_type_kind_t)types[i];
    return NY_MONO_TYPE_NONE;
  case NY_E_UNARY:
    return ny_mono_static_expr_kind(e->as.unary.right, names, types, arity);
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op)
      return NY_MONO_TYPE_NONE;
    bool arith = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                 strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                 strcmp(op, "%") == 0 || strcmp(op, "^") == 0;
    bool bit = strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
               strcmp(op, "^^") == 0 || strcmp(op, "<<") == 0 ||
               strcmp(op, ">>") == 0;
    if (!arith && !bit)
      return NY_MONO_TYPE_NONE;
    ny_mono_type_kind_t l =
        ny_mono_static_expr_kind(e->as.binary.left, names, types, arity);
    ny_mono_type_kind_t r =
        ny_mono_static_expr_kind(e->as.binary.right, names, types, arity);
    if (l == NY_MONO_TYPE_NONE || r == NY_MONO_TYPE_NONE)
      return NY_MONO_TYPE_NONE;
    if (l == NY_MONO_TYPE_F64 || r == NY_MONO_TYPE_F64)
      return arith ? NY_MONO_TYPE_F64 : NY_MONO_TYPE_NONE;
    return NY_MONO_TYPE_INT;
  }
  default:
    return NY_MONO_TYPE_NONE;
  }
}

static bool ny_mono_return_kind_walk(stmt_t *s, const char **names,
                                     const uint8_t *types, int arity,
                                     ny_mono_type_kind_t *out,
                                     bool *saw_return) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      if (!ny_mono_return_kind_walk(s->as.block.body.data[i], names, types,
                                    arity, out, saw_return))
        return false;
    return true;
  case NY_S_IF:
    return ny_mono_return_kind_walk(s->as.iff.conseq, names, types, arity, out,
                                    saw_return) &&
           ny_mono_return_kind_walk(s->as.iff.alt, names, types, arity, out,
                                    saw_return);
  case NY_S_WHILE:
    return ny_mono_return_kind_walk(s->as.whl.body, names, types, arity, out,
                                    saw_return) &&
           ny_mono_return_kind_walk(s->as.whl.update, names, types, arity, out,
                                    saw_return);
  case NY_S_FOR:
    return ny_mono_return_kind_walk(s->as.fr.body, names, types, arity, out,
                                    saw_return) &&
           ny_mono_return_kind_walk(s->as.fr.update, names, types, arity, out,
                                    saw_return);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      if (!ny_mono_return_kind_walk(s->as.match.arms.data[i].conseq, names,
                                    types, arity, out, saw_return))
        return false;
    return ny_mono_return_kind_walk(s->as.match.default_conseq, names, types,
                                    arity, out, saw_return);
  case NY_S_RETURN: {
    ny_mono_type_kind_t k =
        ny_mono_static_expr_kind(s->as.ret.value, names, types, arity);
    if (k == NY_MONO_TYPE_NONE)
      return false;
    if (!*saw_return) {
      *out = k;
      *saw_return = true;
      return true;
    }
    if (*out == k)
      return true;
    if ((*out == NY_MONO_TYPE_INT && k == NY_MONO_TYPE_F64) ||
        (*out == NY_MONO_TYPE_F64 && k == NY_MONO_TYPE_INT)) {
      *out = NY_MONO_TYPE_F64;
      return true;
    }
    return false;
  }
  case NY_S_EXPR: {
    ny_mono_type_kind_t k =
        ny_mono_static_expr_kind(s->as.expr.expr, names, types, arity);
    if (k == NY_MONO_TYPE_NONE)
      return false;
    if (!*saw_return) {
      *out = k;
      *saw_return = true;
      return true;
    }
    if (*out == k)
      return true;
    if ((*out == NY_MONO_TYPE_INT && k == NY_MONO_TYPE_F64) ||
        (*out == NY_MONO_TYPE_F64 && k == NY_MONO_TYPE_INT)) {
      *out = NY_MONO_TYPE_F64;
      return true;
    }
    return false;
  }
  default:
    return true;
  }
}

static bool ny_mono_is_single_return_shape(stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_RETURN || s->kind == NY_S_EXPR)
    return true;
  if (s->kind == NY_S_BLOCK && s->as.block.body.len == 1)
    return ny_mono_is_single_return_shape(s->as.block.body.data[0]);
  return false;
}

static ny_mono_type_kind_t
ny_mono_infer_return_kind(stmt_t *clone, const uint8_t *types, int arity) {
  if (!clone || clone->as.fn.return_type || arity <= 0)
    return NY_MONO_TYPE_NONE;
  if (!ny_mono_is_single_return_shape(clone->as.fn.body))
    return NY_MONO_TYPE_NONE;
  const char *names[NY_MONO_MAX_ARITY] = {0};
  for (int i = 0;
       i < arity && i < NY_MONO_MAX_ARITY && i < (int)clone->as.fn.params.len;
       i++)
    names[i] = clone->as.fn.params.data[i].name;
  ny_mono_type_kind_t ret = NY_MONO_TYPE_NONE;
  bool saw_return = false;
  if (!ny_mono_return_kind_walk(clone->as.fn.body, names, types, arity, &ret,
                                &saw_return) ||
      !saw_return)
    return NY_MONO_TYPE_NONE;
  return ret;
}

static const char *ny_mono_infer_return_type(stmt_t *clone,
                                             const uint8_t *types, int arity) {
  if (!clone || clone->as.fn.return_type || arity <= 0)
    return clone ? clone->as.fn.return_type : NULL;
  ny_mono_type_kind_t ret = ny_mono_infer_return_kind(clone, types, arity);
  if (ret == NY_MONO_TYPE_NONE)
    return NULL;
  return NULL;
}

static stmt_t *ny_mono_clone_func_stmt(codegen_t *cg, fun_sig *base_sig,
                                       const char *mono_name,
                                       const uint8_t *types, int arity) {
  if (!cg || !base_sig || !base_sig->stmt_t ||
      base_sig->stmt_t->kind != NY_S_FUNC)
    return NULL;
  stmt_t *base = base_sig->stmt_t;
  stmt_t *clone = arena_alloc(cg->arena, sizeof(*clone));
  if (!clone)
    return NULL;
  *clone = *base;
  clone->as.fn = base->as.fn;
  clone->as.fn.name = mono_name ? mono_name : base->as.fn.name;
  clone->as.fn.params = (ny_param_list){0};
  for (size_t i = 0; i < base->as.fn.params.len; i++) {
    param_t p = base->as.fn.params.data[i];
    if (!p.type && i < (size_t)arity) {
      const char *mono_type = ny_mono_type_name(types[i]);
      if (mono_type)
        p.type = mono_type;
    }
    vec_push_arena(cg->arena, &clone->as.fn.params, p);
  }
  if (!clone->as.fn.return_type)
    clone->as.fn.return_type = ny_mono_infer_return_type(clone, types, arity);
  sema_func_t *sema = arena_alloc(cg->arena, sizeof(*sema));
  if (!sema)
    return NULL;
  memset(sema, 0, sizeof(*sema));
  sema->resolved_return_type =
      clone->as.fn.return_type
          ? resolve_abi_type_name(cg, clone->as.fn.return_type, clone->tok)
          : cg->type_i64;
  for (size_t i = 0; i < clone->as.fn.params.len; i++) {
    const char *ptype = clone->as.fn.params.data[i].type;
    LLVMTypeRef pty =
        ptype ? resolve_abi_type_name(cg, ptype, clone->tok) : cg->type_i64;
    vec_push_arena(cg->arena, &sema->resolved_param_types, pty);
    if (i < NY_MONO_MAX_ARITY)
      sema->mono_param_kinds[i] = (i < (size_t)arity) ? types[i] : 0;
  }
  if (base->sema_kind == NY_STMT_SEMA_FUNC && base->sema) {
    sema_func_t *base_sema = (sema_func_t *)base->sema;
    sema->is_pure = base_sema->is_pure;
    sema->purity_known = base_sema->purity_known;
    sema->is_memo_safe = base_sema->is_memo_safe;
    sema->memo_known = base_sema->memo_known;
    sema->effects = base_sema->effects;
    sema->effects_known = base_sema->effects_known;
    sema->args_escape = base_sema->args_escape;
    sema->args_mutated = base_sema->args_mutated;
    sema->returns_alias = base_sema->returns_alias;
    sema->escape_known = base_sema->escape_known;
    sema->is_recursive = base_sema->is_recursive;
  }
  clone->sema = sema;
  clone->sema_kind = NY_STMT_SEMA_FUNC;
  return clone;
}

static expr_t *ny_mono_call_arg_for_param(expr_call_t *c, expr_memcall_t *mc,
                                          bool skip_target,
                                          size_t param_idx) {
  call_arg_t *user_args = c ? c->args.data : (mc ? mc->args.data : NULL);
  size_t user_args_len = c ? c->args.len : (mc ? mc->args.len : 0);
  if (mc && !skip_target && param_idx == 0)
    return mc->target;
  size_t user_idx = (mc && !skip_target) ? param_idx - 1u : param_idx;
  return user_idx < user_args_len ? user_args[user_idx].val : NULL;
}

static bool ny_mono_param_can_carry_list_len(stmt_t *fn, int idx,
                                             uint8_t kind) {
  if (kind == NY_MONO_TYPE_LIST || kind == NY_MONO_TYPE_F64_LIST)
    return true;
  if (!fn || idx < 0 || (size_t)idx >= fn->as.fn.params.len)
    return false;
  const char *ptype = fn->as.fn.params.data[idx].type;
  return ptype && ny_type_is(ptype, "list");
}

static fun_sig *ny_try_monomorphize_call(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *call,
                                         fun_sig *sig, expr_call_t *c,
                                         expr_memcall_t *mc, bool skip_target,
                                         size_t call_argc) {
  if (!cg || !sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
      !sig->stmt_t->as.fn.body)
    return NULL;
  if (cg->mono_emitting || !ny_mono_enabled(cg))
    return NULL;
  if (sig->is_extern || sig->is_variadic || sig->arity <= 0 ||
      sig->arity > NY_MONO_MAX_ARITY)
    return NULL;
  if (call_argc != (size_t)sig->arity)
    return NULL;
  stmt_t *fn = sig->stmt_t;
  if (fn->as.fn.attr_thread || fn->as.fn.attr_naked || fn->as.fn.attr_cache ||
      fn->as.fn.attr_consteval)
    return NULL;
  if (sig->is_recursive)
    return NULL;
  if (ny_is_stdlib_tok(fn->tok) && !ny_env_enabled("NYTRIX_MONO_STDLIB"))
    return NULL;
  if (ny_mono_stmt_has_unsupported(fn->as.fn.body))
    return NULL;
  const char *tail = sig->name ? strrchr(sig->name, '.') : NULL;
  tail = tail ? tail + 1 : sig->name;
  if (ny_mono_stmt_refs_self(fn->as.fn.body, sig->name, tail))
    return NULL;

  size_t body_cost = ny_mono_stmt_cost(fn->as.fn.body);
  int max_cost =
      ny_mono_env_int("NYTRIX_MONO_MAX_COST", fn->as.fn.attr_hot ? 768 : 256);
  if (max_cost > 0 && body_cost > (size_t)max_cost)
    return NULL;

  uint8_t types[NY_MONO_MAX_ARITY] = {0};
  bool arg_list_len_min_known[NY_MONO_MAX_ARITY] = {0};
  int64_t arg_list_len_min_raw[NY_MONO_MAX_ARITY] = {0};
  bool useful = false;
  bool has_keyword = false;
  call_arg_t *user_args = c ? c->args.data : (mc ? mc->args.data : NULL);
  size_t user_args_len = c ? c->args.len : (mc ? mc->args.len : 0);
  for (size_t i = 0; i < user_args_len; i++) {
    if (user_args[i].name) {
      has_keyword = true;
      break;
    }
  }
  if (has_keyword)
    return NULL;
  for (int i = 0; i < sig->arity && i < NY_MONO_MAX_ARITY; i++) {
    expr_t *arg_expr = ny_mono_call_arg_for_param(c, mc, skip_target, (size_t)i);
    if (!fn->as.fn.params.data[i].type) {
      ny_mono_type_kind_t kind =
          ny_mono_expr_kind(cg, scopes, depth, arg_expr);
      if (kind != NY_MONO_TYPE_NONE) {
        types[i] = (uint8_t)kind;
        useful = true;
      }
    }
    int64_t len_min = 0;
    if (ny_mono_param_can_carry_list_len(fn, i, types[i]) &&
        ny_gencall_list_len_min(cg, scopes, depth, arg_expr, &len_min)) {
      arg_list_len_min_known[i] = true;
      arg_list_len_min_raw[i] = len_min;
    }
  }
  if (!useful)
    return NULL;
  bool list_args_only =
      ny_env_enabled("NYTRIX_MONO_LIST_ARGS") &&
      !ny_codegen_speed_profile_enabled(cg) &&
      !ny_env_enabled("NYTRIX_MONO_TYPES") &&
      !ny_env_enabled("NYTRIX_ENABLE_MONOMORPHIZATION");
  if (list_args_only) {
    bool has_list_arg = false;
    for (int i = 0; i < sig->arity && i < NY_MONO_MAX_ARITY; i++) {
      if (types[i] == NY_MONO_TYPE_LIST || types[i] == NY_MONO_TYPE_F64_LIST) {
        has_list_arg = true;
        break;
      }
    }
    if (!has_list_arg)
      return NULL;
  }

  int max_global = ny_mono_env_int("NYTRIX_MONO_MAX_GLOBAL", 128);
  int max_per_fn = ny_mono_env_int("NYTRIX_MONO_MAX_PER_FN", 4);
  uint64_t key_hash = ny_mono_key_hash(sig->name, types, sig->arity);
  key_hash = ny_mono_key_hash_with_list_lens(
      key_hash, arg_list_len_min_known, arg_list_len_min_raw, sig->arity);
  fun_sig *existing =
      ny_mono_lookup_existing(cg, fn, key_hash, types,
                              arg_list_len_min_known, arg_list_len_min_raw,
                              sig->arity);
  if (existing)
    return existing;
  if (max_global > 0 && cg->mono_specs.len >= (size_t)max_global)
    return NULL;
  if (max_per_fn > 0 && ny_mono_count_for_base(cg, fn) >= (size_t)max_per_fn)
    return NULL;

  const char *mono_name =
      ny_mono_make_name(cg, sig->name, types, sig->arity, key_hash);
  if (!mono_name)
    return NULL;
  ny_mono_type_kind_t return_kind =
      ny_mono_infer_return_kind(fn, types, sig->arity);
  stmt_t *clone =
      ny_mono_clone_func_stmt(cg, sig, mono_name, types, sig->arity);
  if (!clone)
    return NULL;
  if (clone->sema_kind == NY_STMT_SEMA_FUNC && clone->sema) {
    sema_func_t *clone_sema = (sema_func_t *)clone->sema;
    for (int i = 0; i < sig->arity && i < NY_MONO_MAX_ARITY; i++) {
      if (!arg_list_len_min_known[i])
        continue;
      clone_sema->mono_param_list_len_min_known[i] = true;
      clone_sema->mono_param_list_len_min_raw[i] = arg_list_len_min_raw[i];
    }
  }
  ny_mono_specialization_t spec = {0};
  spec.base_name = sig->name;
  spec.specialized_name = mono_name;
  spec.base_stmt = fn;
  spec.specialized_stmt = clone;
  spec.key_hash = key_hash;
  spec.body_cost = body_cost;
  spec.arity = sig->arity;
  spec.return_kind = (uint8_t)return_kind;
  spec.raw_return_proven = ny_mono_type_is_raw_scalar(spec.return_kind);
  spec.raw_return_active = false;
  spec.inline_body_eligible =
      spec.raw_return_proven && ny_mono_is_single_return_shape(fn->as.fn.body);
  spec.accept_reason =
      spec.raw_return_proven
          ? "arg-specialized+raw-return-proof"
          : (body_cost <= (size_t)ny_mono_env_int(
                              "NYTRIX_MONO_ALWAYSINLINE_COST", 96)
                 ? "arg-specialized+inline-candidate"
                 : "arg-specialized");
  spec.return_policy =
      spec.raw_return_active
          ? "raw-return-active"
          : (spec.raw_return_proven ? "raw-return-proof-inactive"
                                    : "tagged-return");
  memcpy(spec.types, types, sizeof(spec.types));
  memcpy(spec.arg_list_len_min_known, arg_list_len_min_known,
         sizeof(spec.arg_list_len_min_known));
  memcpy(spec.arg_list_len_min_raw, arg_list_len_min_raw,
         sizeof(spec.arg_list_len_min_raw));
  vec_push(&cg->mono_specs, spec);

  bool old_emitting = cg->mono_emitting;
  cg->mono_emitting = true;
  scope mono_scopes[64] = {0};
  (void)scopes;
  gen_func(cg, clone, mono_name, mono_scopes, 0, NULL);
  cg->mono_emitting = old_emitting;
  fun_sig *mono_sig = lookup_fun_exact(cg, mono_name);
  if (mono_sig) {
    mono_sig->is_pure = sig->is_pure;
    mono_sig->is_memo_safe = sig->is_memo_safe;
    mono_sig->is_stable = sig->is_stable;
    mono_sig->effects = sig->effects;
    mono_sig->args_escape = sig->args_escape;
    mono_sig->args_mutated = sig->args_mutated;
    mono_sig->returns_alias = sig->returns_alias;
    mono_sig->effects_known = sig->effects_known;
    mono_sig->is_recursive = false;
    mono_sig->is_attached_method = sig->is_attached_method;
    if (mono_sig->value) {
      LLVMSetLinkage(mono_sig->value, LLVMInternalLinkage);
      if (!fn->as.fn.attr_noinline) {
        int inline_cost = ny_mono_env_int("NYTRIX_MONO_ALWAYSINLINE_COST", 96);
        if (inline_cost <= 0 || body_cost <= (size_t)inline_cost ||
            ny_mono_is_single_return_shape(fn->as.fn.body)) {
          add_fn_enum_attr(cg, mono_sig->value, "alwaysinline", 0);
          add_fn_enum_attr(cg, mono_sig->value, "hot", 0);
        }
      }
    }
  }
  if (mono_sig && ny_mono_trace_enabled()) {
    fprintf(stderr, "[mono] %s -> %s (", sig->name ? sig->name : "<anon>",
            mono_name);
    for (int i = 0; i < sig->arity; i++) {
      if (i)
        fputc(',', stderr);
      const char *tn = ny_mono_type_name(types[i]);
      fputs(tn ? tn : "_", stderr);
    }
    fprintf(stderr, ") at %s:%d\n",
            call && call->tok.filename ? call->tok.filename : "<unknown>",
            call ? call->tok.line : 0);
  }
  return mono_sig;
}

static bool ny_gencall_is_panic_site(const char *name) {
  if (!name || !*name)
    return false;
  if (strcmp(name, "__panic") == 0)
    return false;
  return ny_name_tail_is(name, "panic") || ny_name_tail_is(name, "set") ||
         ny_name_tail_is(name, "set_idx") || ny_name_tail_is(name, "put");
}

static bool ny_gencall_skip_panic_site_file(token_t tok) {
  const char *f = tok.filename;
  if (!f)
    return false;
  return strstr(f, "/lib/core/mod.ny") || strstr(f, "/lib/core/reflect.ny") ||
         strcmp(f, "lib/core/mod.ny") == 0 ||
         strcmp(f, "lib/core/reflect.ny") == 0;
}

static bool ny_gencall_name_tail_is(const char *name, const char *tail) {
  if (!name || !tail)
    return false;
  size_t n = strlen(name);
  size_t t = strlen(tail);
  if (n < t)
    return false;
  if (strcmp(name + n - t, tail) != 0)
    return false;
  return n == t || name[n - t - 1] == '.';
}

static bool ny_gencall_is_thread_call_api(fun_sig *sig, bool *is_launch) {
  if (is_launch)
    *is_launch = false;
  if (!sig || !sig->name)
    return false;
  if (ny_gencall_name_tail_is(sig->name, "thread_spawn_call"))
    return true;
  if (ny_gencall_name_tail_is(sig->name, "thread_launch_call")) {
    if (is_launch)
      *is_launch = true;
    return true;
  }
  return false;
}

static fun_sig *ny_gencall_lookup_source_file_fun(codegen_t *cg,
                                                  const char *tail_name,
                                                  token_t tok, size_t argc);

static fun_sig *ny_gencall_resolve_static_callable(codegen_t *cg,
                                                   expr_t *callable,
                                                   size_t argc) {
  if (!cg || !callable)
    return NULL;
  fun_sig *sig = NULL;
  if (callable->kind == NY_E_IDENT) {
    const char *name = callable->as.ident.name;
    uint64_t hash = callable->as.ident.hash;
    sig = resolve_overload(cg, name, argc, hash);
    if (!sig)
      sig = lookup_use_module_fun(cg, name, argc);
    if (!sig)
      sig = lookup_fun(cg, name, hash);
    if (!sig)
      sig = ny_gencall_lookup_source_file_fun(cg, name, callable->tok, argc);
  }
  if (!sig) {
    char *full = codegen_full_name(cg, callable, cg->arena);
    if (full)
      sig = lookup_fun(cg, full, 0);
  }
  if (sig && !ny_sig_in_current_sigs(cg, sig))
    return NULL;
  return sig;
}

static bool ny_gencall_sig_accepts_argc_local(const fun_sig *sig, size_t argc) {
  if (!sig)
    return false;
  int min_arity = sig->min_arity_known ? sig->min_arity : sig->arity;
  if (min_arity < 0)
    min_arity = 0;
  if (argc < (size_t)min_arity)
    return false;
  if (sig->is_variadic)
    return true;
  if (sig->arity < 0)
    return true;
  return argc <= (size_t)sig->arity;
}

static fun_sig *ny_gencall_lookup_source_file_fun(codegen_t *cg,
                                                  const char *tail_name,
                                                  token_t tok, size_t argc) {
  if (!cg || !tail_name || !*tail_name || strchr(tail_name, '.') ||
      !tok.filename || !*tok.filename)
    return NULL;
  fun_sig *best = NULL;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig || !sig->name || !sig->source_file)
      continue;
    if (strcmp(sig->source_file, tok.filename) != 0)
      continue;
    const char *dot = strrchr(sig->name, '.');
    const char *sig_tail = dot ? dot + 1 : sig->name;
    if (strcmp(sig_tail, tail_name) != 0)
      continue;
    if (!ny_gencall_sig_accepts_argc_local(sig, argc))
      continue;
    if (!ny_sig_in_current_sigs(cg, sig))
      continue;
    best = sig;
    if (dot)
      return best;
  }
  return best;
}

static const char *ny_gencall_sig_param_type(fun_sig *sig, size_t idx) {
  if (!sig)
    return NULL;
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
      idx < sig->stmt_t->as.fn.params.len)
    return sig->stmt_t->as.fn.params.data[idx].type;
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_EXTERN &&
      idx < sig->stmt_t->as.ext.params.len)
    return sig->stmt_t->as.ext.params.data[idx].type;
  if (idx < sig->param_types.len)
    return sig->param_types.data[idx];
  return NULL;
}

static bool ny_gencall_sig_has_native_param(fun_sig *sig) {
  if (!sig || !sig->is_native_abi)
    return false;
  size_t n = sig->arity > 0 ? (size_t)sig->arity : sig->param_types.len;
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
    n = sig->stmt_t->as.fn.params.len;
  else if (sig->stmt_t && sig->stmt_t->kind == NY_S_EXTERN)
    n = sig->stmt_t->as.ext.params.len;
  for (size_t i = 0; i < n; ++i) {
    const char *t = ny_gencall_sig_param_type(sig, i);
    if (abi_sig_param_needs_native_coerce(sig, t))
      return true;
  }
  return false;
}

LLVMValueRef ny_try_native_call_as_f64(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return NULL;
  expr_call_t *c = &e->as.call;
  for (size_t i = 0; i < c->args.len; ++i) {
    if (c->args.data[i].name)
      return NULL;
  }
  const char *fn_name = e->as.call.callee->as.ident.name;
  if (!fn_name)
    return NULL;
  fun_sig *sig = resolve_overload(cg, fn_name, c->args.len, 0);
  if (!sig || !sig->is_native_abi || sig->is_variadic || !sig->value ||
      !sig->type)
    return NULL;
  if (sig->arity < 0 || c->args.len != (size_t)sig->arity)
    return NULL;
  const char *ret_type =
      sig->return_type ? sig->return_type : sig->inferred_return_type;
  if (!ret_type || !(ny_type_is(ret_type, "f64") || ny_type_is(ret_type, "float")))
    return NULL;
  const char *abi_ret_type = sig->abi_return_type ? sig->abi_return_type : ret_type;
  if (!abi_ret_type ||
      !(ny_type_is(abi_ret_type, "f64") || ny_type_is(abi_ret_type, "float")))
    return NULL;
  LLVMTypeRef ret_ty = LLVMGetReturnType(sig->type);
  if (LLVMGetTypeKind(ret_ty) != LLVMDoubleTypeKind)
    return NULL;

  size_t argc = c->args.len;
  LLVMValueRef *args = NULL;
  if (argc > 0) {
    args = (LLVMValueRef *)alloca(sizeof(LLVMValueRef) * argc);
  }
  for (size_t i = 0; i < argc; ++i) {
    expr_t *arg = c->args.data[i].val;
    const char *ptype = ny_gencall_sig_param_type(sig, i);
    if (abi_sig_param_needs_native_coerce(sig, ptype) &&
        (ny_type_is(ptype, "f64") || ny_type_is(ptype, "float"))) {
      args[i] = gen_expr_as_f64(cg, scopes, depth, arg);
    } else if (abi_sig_param_needs_native_coerce(sig, ptype) &&
               ny_type_is(ptype, "f32")) {
      args[i] = LLVMBuildFPTrunc(cg->builder,
                                 gen_expr_as_f64(cg, scopes, depth, arg),
                                 cg->type_f32, NY_LLVM_NAME(cg, "f64_to_f32"));
    } else {
      LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
      if (!v)
        return NULL;
      args[i] = abi_sig_param_needs_native_coerce(sig, ptype)
                    ? ny_coerce_to_abi(cg, v, ptype)
                    : v;
    }
    if (!args[i])
      return NULL;
  }
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef res =
      LLVMBuildCall2(cg->builder, sig->type, sig->value, args, (unsigned)argc,
                     NY_LLVM_NAME(cg, "native_f64_call"));
  return res;
}

static LLVMValueRef ny_gencall_try_thread_call_api(codegen_t *cg, scope *scopes,
                                                   size_t depth, expr_t *e,
                                                   fun_sig *api_sig,
                                                   expr_call_t *c) {
  if (!cg || !e || !api_sig || !c)
    return NULL;
  bool is_launch = false;
  if (!ny_gencall_is_thread_call_api(api_sig, &is_launch))
    return NULL;
  if (c->args.len != 1 && c->args.len != 2)
    return NULL;

  expr_t *fn_expr = c->args.data[0].val;
  expr_t *args_expr = c->args.len >= 2 ? c->args.data[1].val : NULL;
  size_t thread_argc = 0;
  if (args_expr) {
    if (!ny_expr_is_list_or_tuple_lit(args_expr)) {
      fun_sig *known = ny_gencall_resolve_static_callable(cg, fn_expr, 0);
      if (ny_gencall_sig_has_native_param(known)) {
        ny_diag_error(args_expr->tok,
                      "thread_spawn_call for native-ABI typed functions "
                      "requires a literal argument list");
        ny_diag_hint("pass a literal list so the compiler can coerce "
                     "arguments, or use thread_spawn with one packed argument");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      return NULL;
    }
    thread_argc = args_expr->as.list_like.len;
  }
  if (thread_argc > 15) {
    ny_diag_error(e->tok, "thread_spawn_call supports up to 15 arguments");
    ny_diag_hint("pass a packed object through thread_spawn for larger inputs");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  fun_sig *target =
      ny_gencall_resolve_static_callable(cg, fn_expr, thread_argc);
  if (!target)
    return NULL;
  if (abi_sig_type_needs_native_coerce(cg, target, target->return_type)) {
    ny_diag_error(fn_expr->tok,
                  "thread_spawn_call target '%s' must return tagged any or "
                  "omit a native return annotation",
                  target->name ? target->name : "<callable>");
    ny_diag_hint("remove the native return annotation or return a boxed value");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  LLVMTypeRef ret_ty = LLVMGetReturnType(target->type);
  if (ret_ty != cg->type_i64) {
    ny_diag_error(fn_expr->tok,
                  "thread_spawn_call target '%s' must return an i64 Ny value",
                  target->name ? target->name : "<callable>");
    cg->had_error = 1;
    return ny_c0(cg);
  }

  LLVMValueRef argv_ptr = ny_c0(cg);
  LLVMValueRef *argv_values = NULL;
  if (thread_argc > 0) {
    argv_values = malloc(sizeof(LLVMValueRef) * thread_argc);
    if (!argv_values) {
      ny_diag_error(e->tok, "out of memory preparing thread arguments");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    for (size_t i = 0; i < thread_argc; ++i) {
      expr_t *arg_expr = args_expr->as.list_like.data[i];
      const char *ptype = ny_gencall_sig_param_type(target, i);
      LLVMValueRef v = gen_expr(cg, scopes, depth, arg_expr);
      if (!v) {
        ny_diag_error(arg_expr->tok, "failed to evaluate thread argument %zu",
                      i + 1);
        cg->had_error = 1;
        free(argv_values);
        return ny_c0(cg);
      }
      if (abi_sig_param_needs_native_coerce(target, ptype)) {
        if (abi_type_is_float(ptype)) {
          ny_diag_error(
              arg_expr->tok,
              "thread_spawn_call does not support native float parameters yet");
          ny_diag_hint("pass one packed argument to thread_spawn and unpack "
                       "inside the worker");
          cg->had_error = 1;
          free(argv_values);
          return ny_c0(cg);
        }
        bool proven_int = ny_is_proven_int(cg, scopes, depth, arg_expr, v);
        v = ny_coerce_to_abi_proven_int(cg, v, ptype, proven_int);
      }
      if (LLVMTypeOf(v) != cg->type_i64) {
        if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind) {
          v = ny_ptr2i64(cg, v, "thread_arg_ptr");
        } else {
          ny_diag_error(arg_expr->tok,
                        "thread_spawn_call argument %zu cannot be packed into "
                        "an i64 slot",
                        i + 1);
          cg->had_error = 1;
          free(argv_values);
          return ny_c0(cg);
        }
      }
      argv_values[i] = v;
    }
    LLVMTypeRef argv_ty = LLVMArrayType(cg->type_i64, (unsigned)thread_argc);
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef argv_stack = LLVMBuildAlloca(
        cg->builder, argv_ty, NY_LLVM_NAME(cg, "thread_api_argv"));
    LLVMSetAlignment(argv_stack, 16);
    for (size_t i = 0; i < thread_argc; ++i) {
      LLVMValueRef idxs[2] = {ny_c0(cg),
                              LLVMConstInt(cg->type_i64, (uint64_t)i, false)};
      LLVMValueRef slot =
          LLVMBuildGEP2(cg->builder, argv_ty, argv_stack, idxs, 2, "");
      ny_store(cg, slot, argv_values[i]);
    }
    argv_ptr = ny_ptr2i64(cg, argv_stack, "thread_api_argv_ptr");
    free(argv_values);
  }

  fun_sig *rt_sig = lookup_fun(
      cg, is_launch ? "__thread_launch_call" : "__thread_spawn_call", 0);
  if (!rt_sig) {
    ny_diag_error(e->tok, "missing runtime thread helper");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  LLVMValueRef fn_val =
      LLVMTypeOf(target->value) == cg->type_i64
          ? target->value
          : ny_ptr2i64(cg, target->value, NY_LLVM_NAME(cg, "thread_api_fn"));
  LLVMValueRef argc_val =
      LLVMConstInt(cg->type_i64, (((uint64_t)thread_argc << 1) | 1u), false);
  ny_dbg_loc(cg, e->tok);
  return LLVMBuildCall2(cg->builder, rt_sig->type, rt_sig->value,
                        (LLVMValueRef[]){fn_val, argc_val, argv_ptr}, 3,
                        is_launch ? "thread_launch_call" : "thread_spawn_call");
}

static const char *ny_fixed_scalar_cast_name(const char *name) {
  if (!name)
    return NULL;
  const char *tail = strrchr(name, '.');
  tail = tail ? tail + 1 : name;
  static const char *const k_casts[] = {"u8",  "u16", "u32", "u64", "i8", "i16",
                                        "i32", "i64", "f32", "f64", NULL};
  for (size_t i = 0; k_casts[i]; ++i) {
    if (strcmp(tail, k_casts[i]) == 0)
      return k_casts[i];
  }
  return NULL;
}

static LLVMValueRef ny_try_fixed_scalar_cast_intrinsic(codegen_t *cg,
                                                       scope *scopes,
                                                       size_t depth, expr_t *e,
                                                       expr_call_t *c,
                                                       const char *name) {
  const char *cast_name = ny_fixed_scalar_cast_name(name);
  if (!cast_name)
    return NULL;
  if (c->args.len != 1) {
    ny_diag_error(e->tok, "%s cast expects 1 argument", cast_name);
    ny_diag_hint("casts use one value, e.g. %s(x)", cast_name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  LLVMValueRef v = gen_expr(cg, scopes, depth, c->args.data[0].val);
  if (!v)
    return ny_c0(cg);
  bool proven_int = ny_is_proven_int(cg, scopes, depth, c->args.data[0].val, v);
  LLVMValueRef raw = ny_coerce_to_abi_proven_int(cg, v, cast_name, proven_int);
  return ny_box_abi_result(cg, raw, cast_name);
}

LLVMValueRef gen_call_expr(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e) {
  if (!cg)
    return NULL;
  if (!e) {
    cg->had_error = 1;
    return ny_c0(cg);
  }
  if (e->kind != NY_E_CALL && e->kind != NY_E_MEMCALL) {
    ny_diag_error(e->tok,
                  "internal error: expected call expression in gen_call_expr");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  expr_call_t *c = (e->kind == NY_E_CALL) ? &e->as.call : NULL;
  expr_memcall_t *mc = (e->kind == NY_E_MEMCALL) ? &e->as.memcall : NULL;
  if (c && !c->callee) {
    ny_diag_error(e->tok, "invalid call expression: missing callee");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  if (mc && (!mc->target || !mc->name || !*mc->name)) {
    ny_diag_error(e->tok, "invalid member call expression");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  LLVMValueRef callee = NULL;
  LLVMTypeRef ft = NULL;
  LLVMValueRef fv = NULL;
  bool is_variadic = false;
  int sig_arity = 0;
  bool has_sig = false;
  bool skip_target = false;
  fun_sig *sig_found = NULL;

  if (c && c->callee && c->callee->kind == NY_E_IDENT) {
    LLVMValueRef fast_numeric =
        ny_try_fast_numeric_builtin(cg, scopes, depth, e, c->callee, &c->args);
    if (fast_numeric)
      return fast_numeric;
    LLVMValueRef simple_raw_int_call =
        ny_try_inline_simple_raw_int_call(cg, scopes, depth, e);
    if (simple_raw_int_call)
      return simple_raw_int_call;
    LLVMValueRef scalar_cast = ny_try_fixed_scalar_cast_intrinsic(
        cg, scopes, depth, e, c, c->callee->as.ident.name);
    if (scalar_cast)
      return scalar_cast;
  }
  if (mc && mc->name && strcmp(mc->name, "append") == 0 && mc->args.len == 1) {
    LLVMValueRef fast_append = ny_try_emit_direct_list_append(
        cg, scopes, depth, mc->target, mc->args.data[0].val, e->tok);
    if (fast_append)
      return fast_append;
  }
  if (mc && mc->name && strcmp(mc->name, "len") == 0 && mc->args.len == 0) {
    LLVMValueRef fast_len =
        ny_try_fast_len_builtin(cg, scopes, depth, mc->target, e->tok);
    if (fast_len)
      return fast_len;
  }
  if (mc && mc->name && strcmp(mc->name, "get") == 0 &&
      (mc->args.len == 1 || mc->args.len == 2)) {
    LLVMValueRef fast_get =
        ny_try_emit_fast_receiver_get(cg, scopes, depth, e, mc->target,
                                      &mc->args);
    if (fast_get)
      return fast_get;
  }
  if (mc && mc->name &&
      (strcmp(mc->name, "set") == 0 || strcmp(mc->name, "set_idx") == 0) &&
      mc->args.len == 2) {
    LLVMValueRef fast_set =
        ny_try_emit_fast_receiver_set(cg, scopes, depth, e, mc->target,
                                      &mc->args);
    if (fast_set)
      return fast_set;
  }

  if (c && c->callee && c->callee->kind == NY_E_IDENT &&
      c->callee->as.ident.name) {
    const char *n = c->callee->as.ident.name;
    uint64_t n_hash = c->callee->as.ident.hash;
    size_t builtin_name_len = 0;
    uint64_t builtin_hash = 0;
    const char *builtin_name = ny_builtin_surface_name_for_callee(
        c->callee, &builtin_name_len, &builtin_hash);
    if (!builtin_name)
      builtin_name = n;
    bool builtin_name_shadowed = ny_builtin_name_shadowed_by_user_symbol(
        cg, scopes, depth, builtin_name, builtin_name_len, builtin_hash);
    fun_sig *exact_shadow = lookup_fun_exact(cg, builtin_name);
    if (exact_shadow && exact_shadow->stmt_t &&
        !ny_is_stdlib_tok(exact_shadow->stmt_t->tok))
      builtin_name_shadowed = true;
    if (!ny_gencall_precheck_safe_raw_memory_api(
            cg, e, scopes, depth, n, builtin_name_shadowed, c))
      return ny_c0(cg);
    if (!builtin_name_shadowed && strcmp(builtin_name, "__main") == 0 &&
        c->args.len == 0) {
      return ny_codegen_token_is_source_file(cg, e->tok) ? ny_ctrue(cg)
                                                         : ny_cfalse(cg);
    }
    LLVMValueRef const_runtime_tag = ny_try_const_runtime_tag_builtin(
        cg, builtin_name, builtin_name_shadowed, c);
    if (const_runtime_tag)
      return const_runtime_tag;
    LLVMValueRef direct_llvm = ny_try_direct_llvm_intrinsic(
        cg, scopes, depth, e, builtin_name, builtin_name_shadowed, c);
    if (direct_llvm)
      return direct_llvm;
    if (!builtin_name_shadowed && strcmp(builtin_name, "expand") == 0) {
      if (c->args.len != 1) {
        ny_diag_error(e->tok, "expand(expr) expects exactly one expression");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      char *json = ny_expr_to_json(c->args.data[0].val);
      fprintf(stderr, "[expand] %s:%d:%d\n%s\n",
              e->tok.filename ? e->tok.filename : "<unknown>", e->tok.line,
              e->tok.col, json ? json : "null");
      if (json)
        rt_free((int64_t)(uintptr_t)json);
      return gen_expr(cg, scopes, depth, c->args.data[0].val);
    }
    LLVMValueRef static_assert_result = ny_try_static_assert_builtin(
        cg, scopes, depth, e, builtin_name, builtin_name_shadowed, c);
    if (static_assert_result)
      return static_assert_result;
    LLVMValueRef compile_range_result = ny_try_compile_range_builtin(
        cg, scopes, depth, e, builtin_name, builtin_name_shadowed, c);
    if (compile_range_result)
      return compile_range_result;
    if (!ny_gencall_precheck_safe_raw_memory_api(
            cg, e, scopes, depth, builtin_name, builtin_name_shadowed, c))
      return ny_c0(cg);
    LLVMValueRef ct_type_check = ny_try_compile_time_type_builtin(
        cg, scopes, depth, e, builtin_name, builtin_name_shadowed, c);
    if (ct_type_check)
      return ct_type_check;
    LLVMValueRef direct_list_ctor = ny_try_emit_direct_list_ctor(
        cg, scopes, depth, e, c, builtin_name, builtin_name_shadowed);
    if (direct_list_ctor)
      return direct_list_ctor;
    if (ny_try_bad_std_call_type_diag(cg, scopes, depth, e, builtin_name,
                                      builtin_name_shadowed, c))
      return ny_c0(cg);
    if (!builtin_name_shadowed && ny_lookup_tagged_type(cg, n)) {
      fun_sig *type_name_sig = lookup_fun(cg, n, n_hash);
      if (!type_name_sig)
        type_name_sig = lookup_use_module_fun(cg, n, c->args.len);
      if (!type_name_sig) {
        if (c->args.len != 1) {
          ny_diag_error(e->tok, "type cast '%s(...)' expects exactly one value",
                        n);
          cg->had_error = 1;
          return ny_c0(cg);
        }
        LLVMValueRef cast_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
        return cast_v ? cast_v : ny_c0(cg);
      }
    }
    bool want_builtin_len =
        ny_gencall_builtin_name_is(builtin_name, "len", builtin_name_shadowed);
    if (want_builtin_len && c->args.len == 1) {
      LLVMValueRef fast_len = ny_try_fast_len_builtin(
          cg, scopes, depth, c->args.data[0].val, e->tok);
      if (fast_len)
        return fast_len;
    }
    bool want_builtin_append =
        ((!builtin_name_shadowed && strcmp(builtin_name, "append") == 0) ||
         strcmp(builtin_name, "std.core.append") == 0 ||
         strcmp(builtin_name, "std.core.reflect.append") == 0 ||
         ny_gencall_builtin_name_is(builtin_name, "append",
                                    builtin_name_shadowed));
    if (want_builtin_append && c->args.len == 2) {
      LLVMValueRef fast_append = ny_try_emit_direct_list_append(
          cg, scopes, depth, c->args.data[0].val, c->args.data[1].val, e->tok);
      if (fast_append)
        return fast_append;
    }
    if (ny_gencall_is_std_iter_count_call(cg, builtin_name, builtin_hash,
                                          builtin_name_shadowed, c)) {
      LLVMValueRef fast_count = ny_try_fast_len_builtin(
          cg, scopes, depth, c->args.data[0].val, e->tok);
      if (fast_count)
        return fast_count;
    }
    bool want_builtin_type =
        ny_gencall_builtin_name_is(builtin_name, "type", builtin_name_shadowed);
    if (want_builtin_type && c->args.len == 1) {
      const char *surface = ny_gencall_static_surface_type(
          cg, scopes, depth, c->args.data[0].val, 0);
      if (surface) {
        LLVMValueRef g = const_string_ptr(cg, surface, strlen(surface));
        return ny_load(cg, g, NY_LLVM_NAME(cg, "type_fast"));
      }
    }
    bool want_builtin_to_str =
        ny_gencall_builtin_name_is(builtin_name, "to_str",
                                   builtin_name_shadowed) ||
        ny_gencall_builtin_name_is(builtin_name, "str",
                                   builtin_name_shadowed);
    if (want_builtin_to_str && c->args.len == 1) {
      fun_sig *raw_to_str = lookup_fun(cg, "__to_str", 0);
      const char *arg_type =
          infer_expr_type(cg, scopes, depth, c->args.data[0].val);
      if (raw_to_str && ny_gencall_type_is_raw_to_str_scalar(arg_type)) {
        LLVMValueRef v = gen_expr(cg, scopes, depth, c->args.data[0].val);
        if (!v) {
          ny_diag_error(e->tok, "failed to evaluate to_str argument");
          cg->had_error = 1;
          return ny_c0(cg);
        }
        v = ny_cast_to_i64(cg, v, "to_str_arg");
        return LLVMBuildCall2(cg->builder, raw_to_str->type, raw_to_str->value,
                              (LLVMValueRef[]){v}, 1, "to_str_fast");
      }
    }
    LLVMValueRef fast_int_bin = ny_try_fast_int_binary_builtin(
        cg, scopes, depth, builtin_name, builtin_name_shadowed, c, e->tok);
    if (fast_int_bin)
      return fast_int_bin;
    LLVMValueRef fast_handle_mem = ny_try_fast_handle_memory_callsite(
        cg, e, scopes, depth, builtin_name, builtin_name_shadowed, c);
    if (fast_handle_mem)
      return fast_handle_mem;
    LLVMValueRef fast_tbuf = ny_try_fast_tbuf_builtin(
        cg, e, scopes, depth, builtin_name, builtin_name_shadowed, c);
    if (fast_tbuf)
      return fast_tbuf;
    LLVMValueRef fast_scalar = ny_try_fast_scalar_rt_builtin(
        cg, scopes, depth, e, builtin_name, builtin_name_shadowed, c);
    if (fast_scalar)
      return fast_scalar;
    LLVMValueRef fast_simmd = ny_try_fast_simmd_raw_builtin(
        cg, e, scopes, depth, builtin_name, builtin_name_shadowed, c);
    if (fast_simmd)
      return fast_simmd;
    LLVMValueRef fast_raw_mem = ny_try_fast_raw_memory_builtin(
        cg, e, scopes, depth, builtin_name, builtin_name_shadowed, c);
    if (fast_raw_mem)
      return fast_raw_mem;
    bool want_builtin_get =
        ((!builtin_name_shadowed && strcmp(builtin_name, "get") == 0) ||
         strcmp(builtin_name, "std.core.get") == 0 ||
         strcmp(builtin_name, "std.core.reflect.get") == 0 ||
         ny_gencall_builtin_name_is(builtin_name, "get",
                                    builtin_name_shadowed));
    bool get_target_is_direct_dict =
        want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        ny_expr_is_direct_dict_storage(cg, scopes, depth, c->args.data[0].val);
    bool get_target_is_known_dict =
        want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        (get_target_is_direct_dict ||
         ny_expr_has_known_dict_type(cg, scopes, depth, c->args.data[0].val));
    if (get_target_is_known_dict) {
      fun_sig *dict_get_sig = lookup_fun(cg, "std.core.dict_mod.dict_read", 0);
      if (dict_get_sig) {
        if (ny_gencall_expr_is_int_index(cg, scopes, depth,
                                         c->args.data[1].val) &&
            !ny_env_enabled("NYTRIX_GUARDED_FAST_DICT_GET") &&
            ny_env_enabled_default_on("NYTRIX_FAST_INT_DICT_GET")) {
          expr_t *default_expr =
              (c->args.len == 3) ? c->args.data[2].val : NULL;
          return ny_emit_fast_int_dict_get(
              cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
              default_expr, e->tok, dict_get_sig);
        }
        LLVMValueRef args[3];
        args[0] =
            ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, c->args.data[0].val),
                           "dict_get_target");
        args[1] =
            ny_cast_to_i64(cg, gen_expr(cg, scopes, depth, c->args.data[1].val),
                           "dict_get_key");
        args[2] =
            (c->args.len == 3)
                ? ny_cast_to_i64(
                      cg, gen_expr(cg, scopes, depth, c->args.data[2].val),
                      "dict_get_default")
                : ny_c1(cg);
        if (!args[0] || !args[1] || !args[2]) {
          ny_diag_error(e->tok, "failed to evaluate dict get(...) arguments");
          cg->had_error = 1;
          return ny_c0(cg);
        }
        ny_dbg_loc(cg, e->tok);
        return LLVMBuildCall2(cg->builder, dict_get_sig->type,
                              dict_get_sig->value, args, 3,
                              NY_LLVM_NAME(cg, "dict_get_direct"));
      }
    }
    bool get_target_is_direct_list =
        want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        ny_expr_is_direct_list_storage(cg, scopes, depth, c->args.data[0].val);
    bool get_target_is_raw_int_list =
        want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        ny_raw_int_list_target_binding(cg, scopes, depth,
                                       c->args.data[0].val) != NULL;
    bool get_target_is_known_list_like =
        want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        ny_expr_has_known_list_like_type(cg, scopes, depth,
                                         c->args.data[0].val);
    bool get_target_object_elided =
        want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        ny_static_int_list_target_object_elided(cg, scopes, depth,
                                                c->args.data[0].val);
    if (want_builtin_get && (c->args.len == 2 || c->args.len == 3) &&
        ny_gencall_expr_is_int_index(cg, scopes, depth, c->args.data[1].val) &&
        (get_target_is_raw_int_list || get_target_is_direct_list ||
         get_target_is_known_list_like)) {
      expr_t *default_expr = (c->args.len == 3) ? c->args.data[2].val : NULL;
      bool assume_nonnegative = ny_gencall_expr_is_safe_fast_set_index(
          cg, scopes, depth, c->args.data[1].val);
      bool assume_in_bounds =
          assume_nonnegative &&
          ny_gencall_expr_in_list_len_min(
              cg, scopes, depth, c->args.data[0].val, c->args.data[1].val);
      LLVMValueRef raw_int_list_get = ny_try_emit_raw_int_list_get(
          cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
          default_expr, e->tok, assume_nonnegative, assume_in_bounds);
      if (raw_int_list_get)
        return raw_int_list_get;
      LLVMValueRef static_int_list_get = ny_try_emit_static_int_list_get(
          cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
          default_expr, e->tok, assume_nonnegative, assume_in_bounds);
      if (static_int_list_get)
        return static_int_list_get;
      if (get_target_object_elided) {
        ny_diag_error(e->tok,
                      "internal static int-list proof failed for get(...)");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      if (get_target_is_direct_list &&
          !ny_env_enabled("NYTRIX_INDEX_READ_PARITY") &&
          !ny_env_enabled("NYTRIX_GUARDED_FAST_GET") &&
          ny_env_enabled_default_on("NYTRIX_TRUSTED_FAST_GET")) {
        return ny_emit_trusted_fast_indexable_get(
            cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
            default_expr, e->tok, assume_nonnegative, assume_in_bounds);
      }
      return ny_emit_fast_indexable_get(cg, scopes, depth, c->args.data[0].val,
                                        c->args.data[1].val, default_expr,
                                        e->tok, assume_nonnegative);
    }
    if (get_target_object_elided) {
      ny_diag_error(
          e->tok,
          "internal static int-list proof requires integer get(...) index");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    bool want_builtin_set_idx =
        ((!builtin_name_shadowed && strcmp(builtin_name, "set_idx") == 0) ||
         strcmp(builtin_name, "std.core.set_idx") == 0 ||
         strcmp(builtin_name, "std.core.reflect.set_idx") == 0 ||
         ny_gencall_builtin_name_is(builtin_name, "set_idx",
                                    builtin_name_shadowed));
    bool want_builtin_set =
        ((!builtin_name_shadowed && strcmp(builtin_name, "set") == 0) ||
         strcmp(builtin_name, "std.core.set") == 0 ||
         strcmp(builtin_name, "std.core.reflect.set") == 0 ||
         ny_gencall_builtin_name_is(builtin_name, "set",
                                    builtin_name_shadowed));
    if ((want_builtin_set_idx || want_builtin_set) && c->args.len == 3) {
      const char *base_set_name = want_builtin_set_idx ? "set_idx" : "set";
      const char *core_set_name =
          want_builtin_set_idx ? "std.core.set_idx" : "std.core.set";
      const char *reflect_set_name = want_builtin_set_idx
                                         ? "std.core.reflect.set_idx"
                                         : "std.core.reflect.set";
      if (ny_expr_has_known_dict_type(cg, scopes, depth, c->args.data[0].val)) {
        fun_sig *dict_set_sig = NULL;
        if (ny_env_enabled_default_on("NYTRIX_FAST_DICT_WRITE") &&
            !ny_env_enabled("NYTRIX_DISABLE_FAST_DICT_WRITE"))
          dict_set_sig = lookup_fun(cg, "__dict_write_fast", 0);
        if (!dict_set_sig)
          dict_set_sig = lookup_fun(cg, "std.core.dict_mod.dict_write", 0);
        if (dict_set_sig) {
          LLVMValueRef args[3];
          for (size_t i = 0; i < 3; ++i) {
            args[i] = gen_expr(cg, scopes, depth, c->args.data[i].val);
            if (!args[i]) {
              ny_diag_error(e->tok,
                            "failed to evaluate dict set(...) argument");
              cg->had_error = 1;
              return ny_c0(cg);
            }
            args[i] = ny_cast_to_i64(cg, args[i], "dict_set_arg");
          }
          ny_dbg_loc(cg, e->tok);
          return LLVMBuildCall2(cg->builder, dict_set_sig->type,
                                dict_set_sig->value, args, 3,
                                NY_LLVM_NAME(cg, "dict_set_direct"));
        }
      }
      fun_sig *set_sig = lookup_fun(
          cg, strcmp(n, core_set_name) == 0 ? core_set_name : base_set_name, 0);
      if (!set_sig && strcmp(n, base_set_name) == 0)
        set_sig = lookup_use_module_fun(cg, n, 3);
      if (!set_sig)
        set_sig = lookup_fun(cg, reflect_set_name, 0);
      bool key_is_safe_fast_index = ny_gencall_expr_is_safe_fast_set_index(
          cg, scopes, depth, c->args.data[1].val);
      bool key_is_existing_index =
          key_is_safe_fast_index &&
          ny_gencall_expr_in_list_len_min(
              cg, scopes, depth, c->args.data[0].val, c->args.data[1].val);
      bool set_target_is_direct_list = ny_expr_is_direct_list_storage(
          cg, scopes, depth, c->args.data[0].val);
      bool set_target_is_trusted_f64_list =
          key_is_existing_index &&
          ny_f64_list_target_binding(cg, scopes, depth,
                                     c->args.data[0].val) != NULL;
      LLVMValueRef raw_int_list_set = ny_try_emit_raw_int_list_set(
          cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
          c->args.data[2].val, e->tok, key_is_safe_fast_index,
          key_is_existing_index);
      if (raw_int_list_set)
        return raw_int_list_set;
      if (ny_raw_int_list_target_binding(cg, scopes, depth,
                                         c->args.data[0].val)) {
        ny_diag_error(e->tok,
                      "internal raw int-list proof failed for set_idx(...)");
        cg->had_error = 1;
        return ny_c0(cg);
      }
      if (ny_gencall_expr_is_int_index(cg, scopes, depth,
                                       c->args.data[1].val) &&
          (set_target_is_direct_list ||
           ny_expr_has_known_list_type(cg, scopes, depth,
                                       c->args.data[0].val))) {
        if ((set_target_is_direct_list || set_target_is_trusted_f64_list) &&
            !ny_env_enabled("NYTRIX_GUARDED_FAST_SET") &&
            ny_env_enabled_default_on("NYTRIX_TRUSTED_FAST_SET")) {
          return ny_emit_trusted_fast_list_set(
              cg, scopes, depth, c->args.data[0].val, c->args.data[1].val,
              c->args.data[2].val, e->tok, set_sig, key_is_safe_fast_index,
              key_is_existing_index);
        }
        return ny_emit_fast_list_set(cg, scopes, depth, c->args.data[0].val,
                                     c->args.data[1].val, c->args.data[2].val,
                                     e->tok, set_sig, key_is_safe_fast_index);
      }
      if (set_sig) {
        LLVMValueRef args[3];
        for (size_t i = 0; i < 3; ++i) {
          args[i] = gen_expr(cg, scopes, depth, c->args.data[i].val);
          if (!args[i]) {
            ny_diag_error(e->tok, "failed to evaluate set_idx(...) argument");
            cg->had_error = 1;
            return ny_c0(cg);
          }
          args[i] = ny_cast_to_i64(cg, args[i], "set_idx_arg");
        }
        ny_dbg_loc(cg, e->tok);
        return LLVMBuildCall2(cg->builder, set_sig->type, set_sig->value, args,
                              3, NY_LLVM_NAME(cg, "set_idx_generic"));
      }
    }
    bool want_load_item_checked = (strcmp(n, "__load_item") == 0);
    bool want_load_item_fast = (strcmp(n, "__load_item_fast") == 0);
    if ((want_load_item_checked || want_load_item_fast) && c->args.len == 2) {
      LLVMValueRef lst_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
      LLVMValueRef idx_v = gen_expr(cg, scopes, depth, c->args.data[1].val);
      if (!lst_v || !idx_v) {
        ny_diag_error(e->tok, "failed to evaluate arguments for %s", n);
        cg->had_error = 1;
        return ny_c0(cg);
      }
      lst_v = ny_cast_to_i64(cg, lst_v, "load_item_lst");
      idx_v = ny_cast_to_i64(cg, idx_v, "load_item_idx");
      ny_dbg_loc(cg, e->tok);
      if (want_load_item_fast) {
        LLVMValueRef idx_raw =
            ny_gencall_index_raw_i64(cg, scopes, depth, c->args.data[1].val,
                                     idx_v, "load_item_idx_raw");
        LLVMValueRef scaled = LLVMBuildShl(cg->builder, idx_raw,
                                           LLVMConstInt(cg->type_i64, 3, false),
                                           "load_item_scaled");
        LLVMValueRef byte_off = LLVMBuildAdd(
            cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
            "load_item_off");
        LLVMValueRef addr =
            ny_add(cg, lst_v, byte_off, NY_LLVM_NAME(cg, "load_item_addr"));
        LLVMValueRef ptr = LLVMBuildIntToPtr(
            cg->builder, addr, ny_ptr_i64_ty(cg), "load_item_ptr_i64");
        return ny_load(cg, ptr, NY_LLVM_NAME(cg, "load_item_val"));
      }
      LLVMValueRef is_ptr_pred =
          ny_build_is_ptr_pred(cg, lst_v, "load_item_ptr");
      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef ok_bb = ny_bb_fn(fn, "load_item.ok");
      LLVMBasicBlockRef fail_bb = ny_bb_fn(fn, "load_item.fail");
      LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "load_item.join");
      ny_cond_br(cg, is_ptr_pred, ok_bb, fail_bb);

      ny_pos(cg, ok_bb);

      LLVMValueRef idx_raw =
          ny_build_untagged_or_raw_i64(cg, idx_v, "load_item_idx_raw");
      LLVMValueRef scaled =
          ny_shl(cg, idx_raw, LLVMConstInt(cg->type_i64, 3, false),
                 "load_item_scaled");
      LLVMValueRef byte_off = ny_add(
          cg, scaled, LLVMConstInt(cg->type_i64, 16, false), "load_item_off");
      LLVMValueRef addr =
          ny_add(cg, lst_v, byte_off, NY_LLVM_NAME(cg, "load_item_addr"));
      LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, addr, ny_ptr_i64_ty(cg),
                                           "load_item_ptr_i64");
      LLVMValueRef ok_val = ny_load(cg, ptr, NY_LLVM_NAME(cg, "load_item_val"));
      LLVMBasicBlockRef ok_end_bb = ny_cur_block(cg);
      ny_br(cg, join_bb);

      ny_pos(cg, fail_bb);

      LLVMBasicBlockRef fail_end_bb = ny_cur_block(cg);
      ny_br(cg, join_bb);

      ny_pos(cg, join_bb);

      LLVMValueRef phi =
          ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "load_item_result"));
      LLVMValueRef incoming_vals[2] = {ok_val, ny_c0(cg)};
      LLVMBasicBlockRef incoming_bbs[2] = {ok_end_bb, fail_end_bb};
      LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
      return phi;
    }
    bool want_store_item_checked = (strcmp(n, "__store_item") == 0);
    bool want_store_item_fast = (strcmp(n, "__store_item_fast") == 0);
    if ((want_store_item_checked || want_store_item_fast) && c->args.len == 3) {
      LLVMValueRef lst_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
      LLVMValueRef idx_v = gen_expr(cg, scopes, depth, c->args.data[1].val);
      LLVMValueRef val_v = gen_expr(cg, scopes, depth, c->args.data[2].val);
      if (!lst_v || !idx_v || !val_v) {
        ny_diag_error(e->tok, "failed to evaluate arguments for %s", n);
        cg->had_error = 1;
        return ny_c0(cg);
      }
      lst_v = ny_cast_to_i64(cg, lst_v, "store_item_lst");
      idx_v = ny_cast_to_i64(cg, idx_v, "store_item_idx");
      val_v = ny_cast_to_i64(cg, val_v, "store_item_val");
      ny_dbg_loc(cg, e->tok);
      if (want_store_item_fast) {
        LLVMValueRef idx_raw =
            ny_gencall_index_raw_i64(cg, scopes, depth, c->args.data[1].val,
                                     idx_v, "store_item_idx_raw");
        LLVMValueRef scaled = LLVMBuildShl(cg->builder, idx_raw,
                                           LLVMConstInt(cg->type_i64, 3, false),
                                           "store_item_scaled");
        LLVMValueRef byte_off = LLVMBuildAdd(
            cg->builder, scaled, LLVMConstInt(cg->type_i64, 16, false),
            "store_item_off");
        LLVMValueRef addr =
            ny_add(cg, lst_v, byte_off, NY_LLVM_NAME(cg, "store_item_addr"));
        LLVMValueRef ptr = LLVMBuildIntToPtr(
            cg->builder, addr, ny_ptr_i64_ty(cg), "store_item_ptr_i64");
        ny_store(cg, ptr, val_v);
        return val_v;
      }
      LLVMValueRef is_ptr_pred =
          ny_build_is_ptr_pred(cg, lst_v, "store_item_ptr");
      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef ok_bb = ny_bb_fn(fn, "store_item.ok");
      LLVMBasicBlockRef fail_bb = ny_bb_fn(fn, "store_item.fail");
      LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "store_item.join");
      ny_cond_br(cg, is_ptr_pred, ok_bb, fail_bb);

      ny_pos(cg, ok_bb);

      LLVMValueRef idx_raw =
          ny_build_untagged_or_raw_i64(cg, idx_v, "store_item_idx_raw");
      LLVMValueRef scaled =
          ny_shl(cg, idx_raw, LLVMConstInt(cg->type_i64, 3, false),
                 "store_item_scaled");
      LLVMValueRef byte_off = ny_add(
          cg, scaled, LLVMConstInt(cg->type_i64, 16, false), "store_item_off");
      LLVMValueRef addr =
          ny_add(cg, lst_v, byte_off, NY_LLVM_NAME(cg, "store_item_addr"));
      LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, addr, ny_ptr_i64_ty(cg),
                                           "store_item_ptr_i64");
      ny_store(cg, ptr, val_v);
      LLVMBasicBlockRef ok_end_bb = ny_cur_block(cg);
      ny_br(cg, join_bb);

      ny_pos(cg, fail_bb);

      LLVMBasicBlockRef fail_end_bb = ny_cur_block(cg);
      ny_br(cg, join_bb);

      ny_pos(cg, join_bb);

      LLVMValueRef phi =
          ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "store_item_result"));
      LLVMValueRef incoming_vals[2] = {val_v, ny_c0(cg)};
      LLVMBasicBlockRef incoming_bbs[2] = {ok_end_bb, fail_end_bb};
      LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
      return phi;
    }
    bool want_is_ny_obj = (strcmp(n, "__is_ny_obj") == 0);
    bool want_is_str_obj = (strcmp(n, "__is_str_obj") == 0);
    bool want_tagof = (strcmp(n, "__tagof") == 0);

    bool use_fast_obj_intrinsics = false;
    if (use_fast_obj_intrinsics && (want_is_ny_obj || want_is_str_obj) &&
        c->args.len == 1) {
      LLVMValueRef arg_v = gen_expr(cg, scopes, depth, c->args.data[0].val);
      if (!arg_v) {
        ny_diag_error(e->tok, "failed to evaluate argument for %s", n);
        cg->had_error = 1;
        return ny_c0(cg);
      }
      if (LLVMTypeOf(arg_v) != cg->type_i64) {
        arg_v = ny_ptr2i64(cg, arg_v, "obj_intrinsic_arg");
      }
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef is_nonzero = ny_ne(cg, arg_v, ny_c0(cg), "obj_nz");
      LLVMValueRef low_bits = ny_and(
          cg, arg_v, LLVMConstInt(cg->type_i64, NY_VALUE_PTR_TAG_MASK, false),
          "obj_low_bits");
      LLVMValueRef align_ok = ny_eq(cg, low_bits, ny_c0(cg), "obj_align_ok");
      LLVMValueRef ptr_min = ny_ugt(
          cg, arg_v,
          LLVMConstInt(cg->type_i64, (uint64_t)NY_VALUE_PTR_MIN_ADDR, false),
          "obj_gt_min");
      LLVMValueRef is_ptr_pred = LLVMBuildAnd(
          cg->builder, is_nonzero,
          ny_and(cg, align_ok, ptr_min, NY_LLVM_NAME(cg, "obj_ptr_and")),
          "obj_is_ptr");

      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef ptr_bb = ny_bb_fn(fn, "obj.ptr");
      LLVMBasicBlockRef not_ptr_bb = ny_bb_fn(fn, "obj.not_ptr");
      LLVMBasicBlockRef join_bb = ny_bb_fn(fn, "obj.join");

      ny_cond_br(cg, is_ptr_pred, ptr_bb, not_ptr_bb);

      ny_pos(cg, ptr_bb);

      LLVMValueRef tag_addr = ny_sub(
          cg, arg_v, LLVMConstInt(cg->type_i64, 8, false), "obj_tag_addr");
      LLVMValueRef tag_ptr = LLVMBuildIntToPtr(
          cg->builder, tag_addr, ny_ptr_i64_ty(cg), "obj_tag_ptr");
      LLVMValueRef tag_v = ny_load(cg, tag_ptr, NY_LLVM_NAME(cg, "obj_tag"));
      LLVMValueRef ptr_result = ny_c0(cg);
      if (want_is_ny_obj) {
        LLVMValueRef ge_100 =
            LLVMBuildICmp(cg->builder, LLVMIntUGE, tag_v,
                          LLVMConstInt(cg->type_i64, 100, false), "obj_ge_100");
        LLVMValueRef le_255 =
            LLVMBuildICmp(cg->builder, LLVMIntULE, tag_v,
                          LLVMConstInt(cg->type_i64, 255, false), "obj_le_255");
        LLVMValueRef is_obj =
            ny_and(cg, ge_100, le_255, NY_LLVM_NAME(cg, "obj_is_ny_obj"));
        ptr_result = ny_select(cg, is_obj, ny_ctrue(cg), ny_cfalse(cg),
                               "obj_is_ny_obj_v");
      } else if (want_is_str_obj) {
        LLVMValueRef is_str = ny_eq(
            cg, tag_v, LLVMConstInt(cg->type_i64, 120, false), "obj_is_str");
        LLVMValueRef is_const_str =
            ny_eq(cg, tag_v, LLVMConstInt(cg->type_i64, 121, false),
                  "obj_is_const_str");
        LLVMValueRef is_any_str =
            ny_or(cg, is_str, is_const_str, NY_LLVM_NAME(cg, "obj_is_any_str"));
        ptr_result = ny_select(cg, is_any_str, ny_ctrue(cg), ny_cfalse(cg),
                               "obj_is_str_v");
      } else {
        LLVMValueRef tag_shift =
            LLVMBuildShl(cg->builder, tag_v, ny_c1(cg), "obj_tagof_shift");
        ptr_result = ny_or(cg, tag_shift, ny_c1(cg), "obj_tagof_v");
      }
      LLVMBasicBlockRef ptr_end_bb = ny_cur_block(cg);
      ny_br(cg, join_bb);

      ny_pos(cg, not_ptr_bb);

      LLVMValueRef not_ptr_result = want_tagof ? ny_c0(cg) : ny_cfalse(cg);
      LLVMBasicBlockRef not_ptr_end_bb = ny_cur_block(cg);
      ny_br(cg, join_bb);

      ny_pos(cg, join_bb);

      LLVMValueRef phi =
          ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "obj_intrinsic_v"));
      LLVMValueRef incoming_vals[2] = {ptr_result, not_ptr_result};
      LLVMBasicBlockRef incoming_bbs[2] = {ptr_end_bb, not_ptr_end_bb};
      LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
      return phi;
    }
    if ((strcmp(n, "print") == 0 || strcmp(n, "println") == 0) &&
        !builtin_name_shadowed) {
      fun_sig *p_int = lookup_fun(cg, "__print_int", 0);
      fun_sig *p_str = lookup_fun(cg, "__print_str_raw", 0);
      fun_sig *p_nl = lookup_fun(cg, "__print_newline", 0);
      fun_sig *t_str = lookup_fun(cg, "to_str", 0);
      if (!t_str)
        t_str = lookup_fun(cg, "__to_str", 0);
      if (p_int && p_str && p_nl && t_str) {
        LLVMValueRef space_v = 0;
        if (c->args.len > 1) {
          LLVMValueRef space_global = const_string_ptr(cg, " ", 1);
          space_v = ny_load(cg, space_global, "");
        }
        LLVMValueRef printed_slot = 0;
        if (c->args.len > 1) {
          printed_slot = build_alloca(cg, "print_has_value", ny_i1_ty(cg));
          LLVMBuildStore(cg->builder, ny_cbool(cg, 0), printed_slot);
        }
        for (size_t i = 0; i < c->args.len; i++) {
          LLVMValueRef v = gen_expr(cg, scopes, depth, c->args.data[i].val);
          if (!v)
            continue;
          if (ny_fast_path_enabled(cg, "NYTRIX_PRINT_PROVEN_INT_FAST") &&
              ny_is_proven_int(cg, scopes, depth, c->args.data[i].val, v)) {
            if (space_v) {
              LLVMValueRef printed =
                  LLVMBuildLoad2(cg->builder, ny_i1_ty(cg), printed_slot,
                                 "print_has_value_load");
              LLVMValueRef cur_fn = ny_cur_fn(cg);
              LLVMBasicBlockRef space_bb = ny_bb_fn(cur_fn, "print_space");
              LLVMBasicBlockRef value_bb = ny_bb_fn(cur_fn, "print_value");
              ny_cond_br(cg, printed, space_bb, value_bb);

              ny_pos(cg, space_bb);
              LLVMBuildCall2(cg->builder, p_str->type, p_str->value,
                             (LLVMValueRef[]){space_v}, 1, "");
              ny_br(cg, value_bb);

              ny_pos(cg, value_bb);
            }
            LLVMBuildCall2(cg->builder, p_int->type, p_int->value,
                           (LLVMValueRef[]){v}, 1, "");
            if (printed_slot)
              LLVMBuildStore(cg->builder, ny_cbool(cg, 1), printed_slot);
            continue;
          }
          const char *print_arg_type =
              infer_expr_type(cg, scopes, depth, c->args.data[i].val);
          if (ny_fast_path_enabled(cg, "NYTRIX_PRINT_PROVEN_STR_FAST") &&
              print_arg_type && print_arg_type[0] != '?' &&
              ny_type_is(print_arg_type, "str")) {
            if (space_v) {
              LLVMValueRef printed =
                  LLVMBuildLoad2(cg->builder, ny_i1_ty(cg), printed_slot,
                                 "print_has_value_load");
              LLVMValueRef cur_fn = ny_cur_fn(cg);
              LLVMBasicBlockRef space_bb = ny_bb_fn(cur_fn, "print_space");
              LLVMBasicBlockRef value_bb = ny_bb_fn(cur_fn, "print_value");
              ny_cond_br(cg, printed, space_bb, value_bb);

              ny_pos(cg, space_bb);
              LLVMBuildCall2(cg->builder, p_str->type, p_str->value,
                             (LLVMValueRef[]){space_v}, 1, "");
              ny_br(cg, value_bb);

              ny_pos(cg, value_bb);
            }
            LLVMBuildCall2(cg->builder, p_str->type, p_str->value,
                           (LLVMValueRef[]){v}, 1, "");
            if (printed_slot)
              LLVMBuildStore(cg->builder, ny_cbool(cg, 1), printed_slot);
            continue;
          }
          LLVMValueRef is_nil = LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                                              ny_cnil(cg), "print_is_nil");
          LLVMValueRef cur_fn = ny_cur_fn(cg);
          LLVMBasicBlockRef skip_bb = ny_bb_fn(cur_fn, "print_skip_nil");
          LLVMBasicBlockRef emit_bb = ny_bb_fn(cur_fn, "print_emit");
          LLVMBasicBlockRef next_bb = ny_bb_fn(cur_fn, "print_next");

          ny_cond_br(cg, is_nil, skip_bb, emit_bb);

          ny_pos(cg, skip_bb);
          ny_br(cg, next_bb);

          ny_pos(cg, emit_bb);
          if (space_v) {
            LLVMValueRef printed =
                LLVMBuildLoad2(cg->builder, ny_i1_ty(cg), printed_slot,
                               "print_has_value_load");
            LLVMBasicBlockRef space_bb = ny_bb_fn(cur_fn, "print_space");
            LLVMBasicBlockRef value_bb = ny_bb_fn(cur_fn, "print_value");
            ny_cond_br(cg, printed, space_bb, value_bb);

            ny_pos(cg, space_bb);
            LLVMBuildCall2(cg->builder, p_str->type, p_str->value,
                           (LLVMValueRef[]){space_v}, 1, "");
            ny_br(cg, value_bb);

            ny_pos(cg, value_bb);
          }
          LLVMValueRef is_int =
              LLVMBuildICmp(cg->builder, LLVMIntEQ,
                            ny_and(cg, v, ny_c1(cg), ""), ny_c1(cg), "is_int");

          LLVMBasicBlockRef int_bb = ny_bb_fn(cur_fn, "print_int");
          LLVMBasicBlockRef other_bb = ny_bb_fn(cur_fn, "print_other");

          ny_cond_br(cg, is_int, int_bb, other_bb);

          ny_pos(cg, int_bb);
          LLVMBuildCall2(cg->builder, p_int->type, p_int->value,
                         (LLVMValueRef[]){v}, 1, "");
          if (printed_slot)
            LLVMBuildStore(cg->builder, ny_cbool(cg, 1), printed_slot);
          ny_br(cg, next_bb);

          ny_pos(cg, other_bb);
          LLVMValueRef s_v =
              LLVMBuildCall2(cg->builder, t_str->type, t_str->value,
                             (LLVMValueRef[]){v}, 1, "to_str_res");
          LLVMBuildCall2(cg->builder, p_str->type, p_str->value,
                         (LLVMValueRef[]){s_v}, 1, "");
          if (printed_slot)
            LLVMBuildStore(cg->builder, ny_cbool(cg, 1), printed_slot);
          ny_br(cg, next_bb);

          ny_pos(cg, next_bb);
        }
        LLVMBuildCall2(cg->builder, p_nl->type, p_nl->value, NULL, 0, "");
        return ny_c1(cg);
      }
    }
    if (strcmp(n, "extern_all") == 0 || strcmp(n, "__extern_all") == 0) {
      if (handle_extern_all_args(cg, &c->args))
        return ny_c0(cg);
    }
    if (ny_gencall_builtin_name_is(builtin_name, "store_layout",
                                   builtin_name_shadowed)) {
      return emit_store_layout(cg, scopes, depth, e, c);
    }
    if (ny_gencall_builtin_name_is(builtin_name, "load_layout",
                                   builtin_name_shadowed)) {
      return emit_load_layout(cg, scopes, depth, e, c);
    }
    if (strcmp(n, "__layout_size") == 0 || strcmp(n, "__layout_align") == 0 ||
        strcmp(n, "__layout_offset") == 0) {
      bool want_align = strcmp(n, "__layout_align") == 0;
      bool want_offset = strcmp(n, "__layout_offset") == 0;
      if (want_offset) {
        if (c->args.len != 2) {
          ny_diag_error(e->tok, "%s expects 2 arguments", n);
          cg->had_error = 1;
          return ny_c1(cg);
        }
        const char *layout_name = NULL;
        const char *field_name = NULL;
        if (!layout_query_arg(c->args.data[0].val, &layout_name) ||
            !layout_query_arg(c->args.data[1].val, &field_name)) {
          ny_diag_error(e->tok, "%s expects string literal arguments", n);
          cg->had_error = 1;
          return ny_c1(cg);
        }
        return emit_layout_query(cg, e->tok, layout_name, field_name, false,
                                 true);
      }
      if (c->args.len != 1) {
        ny_diag_error(e->tok, "%s expects 1 argument", n);
        cg->had_error = 1;
        return ny_c1(cg);
      }
      const char *layout_name = NULL;
      if (!layout_query_arg(c->args.data[0].val, &layout_name)) {
        ny_diag_error(e->tok, "%s expects a string literal", n);
        cg->had_error = 1;
        return ny_c1(cg);
      }
      return emit_layout_query(cg, e->tok, layout_name, NULL, want_align,
                               false);
    }
  }
  if (mc && mc->name && strcmp(mc->name, "extern_all") == 0) {
    if (handle_extern_all_args(cg, &mc->args))
      return ny_c0(cg);
  }
  if (mc) {
    bool looked_like_module_target = false;
    const char *resolved_module_name = NULL;
    const char *mc_target_type = NULL;

    char module_expr_path[1024];
    if (mc->target && ny_resolve_module_expr_path(cg, scopes, depth, mc->target,
                                                  module_expr_path,
                                                  sizeof(module_expr_path))) {
      looked_like_module_target = true;
      resolved_module_name = module_expr_path;
      char resolved_fun[1280];
      if (ny_resolve_module_function_path(cg, module_expr_path,
                                          mc->name ? mc->name : "",
                                          resolved_fun, sizeof(resolved_fun))) {
        sig_found = lookup_fun(cg, resolved_fun, 0);
        if (sig_found && !ny_sig_in_current_sigs(cg, sig_found))
          sig_found = NULL;
        if (sig_found) {
          ft = sig_found->type;
          fv = sig_found->value;
          sig_arity = sig_found->arity;
          is_variadic = sig_found->is_variadic;
          has_sig = true;
          skip_target = true;
          callee = fv;
          goto static_call_handling;
        }
      }
    }

    if (mc->target && mc->target->kind == NY_E_IDENT) {
      const char *target_name = mc->target->as.ident.name;
      const char *module_name = target_name;
      size_t target_name_len = (size_t)mc->target->tok.len;
      if (target_name_len == 0)
        target_name_len = strlen(target_name);
      binding *target_binding =
          ny_gencall_lookup_binding(cg, scopes, depth, target_name,
                                    target_name_len, mc->target->as.ident.hash);
      bool target_value_defined = target_binding != NULL;
      bool is_alias = false;
      if (!target_value_defined) {
        const char *alias_module =
            ny_lookup_module_alias(cg, scopes, depth, target_name,
                                   target_name_len, mc->target->as.ident.hash);
        if (alias_module && *alias_module) {
          module_name = alias_module;
          is_alias = true;
        }
      }

      if (is_alias ||
          (lookup_fun(cg, target_name, 0) == NULL && !target_value_defined)) {
        looked_like_module_target = true;
        resolved_module_name = module_name;
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(
                cg, module_name, mc->name ? mc->name : "", resolved_fun,
                sizeof(resolved_fun))) {
          sig_found = lookup_fun(cg, resolved_fun, 0);
        }
        if (sig_found && !ny_sig_in_current_sigs(cg, sig_found))
          sig_found = NULL;
        if (sig_found) {
          ft = sig_found->type;
          fv = sig_found->value;
          sig_arity = sig_found->arity;
          is_variadic = sig_found->is_variadic;
          has_sig = true;
          skip_target = true;
          callee = fv;
          goto static_call_handling;
        }
        sig_found =
            ny_gencall_lookup_attached_method(cg, target_name, mc->name);
        if (sig_found) {
          ft = sig_found->type;
          fv = sig_found->value;
          sig_arity = sig_found->arity;
          is_variadic = sig_found->is_variadic;
          has_sig = true;
          skip_target = true;
          callee = fv;
          goto static_call_handling;
        }

        if (is_alias) {
          char dotted[1280];
          snprintf(dotted, sizeof(dotted), "%s.%s", module_name,
                   mc->name ? mc->name : "");
          report_undef_symbol(cg, dotted, e->tok);
          if (verbose_enabled >= 1)
            ny_diag_hint("alias '%s' resolves to module '%s'", target_name,
                         module_name);
          return ny_c0(cg);
        }
      }
    }
    if (!sig_found && looked_like_module_target)
      goto static_call_handling;
    if (!sig_found && mc->target) {
      mc_target_type = infer_expr_type(cg, scopes, depth, mc->target);
      sig_found =
          ny_gencall_lookup_attached_method(cg, mc_target_type, mc->name);
      if (!sig_found)
        sig_found = ny_gencall_lookup_attached_method(cg, "any", mc->name);
      if (sig_found) {
        ft = sig_found->type;
        fv = sig_found->value;
        sig_arity = sig_found->arity;
        is_variadic = sig_found->is_variadic;
        has_sig = true;
        skip_target = false;
        callee = fv;
        goto static_call_handling;
      }
    }
  static_call_handling:;
    if (!sig_found) {
      if (looked_like_module_target && resolved_module_name) {
        char dotted[1280];
        snprintf(dotted, sizeof(dotted), "%s.%s", resolved_module_name,
                 mc->name ? mc->name : "");
        report_undef_symbol(cg, dotted, e->tok);
        return ny_c0(cg);
      }
      if (mc->target &&
          !ny_member_type_allows_dynamic_fallback(mc_target_type)) {
        return ny_member_unknown_static_expr(cg, e->tok, "method", mc->name,
                                             mc_target_type);
      }

      fun_sig *getter = ny_gencall_getter(cg);
      if (getter && mc->name && strcmp(mc->name, "get") != 0) {
        LLVMValueRef target_val = gen_expr(cg, scopes, depth, mc->target);
        if (!target_val) {
          ny_diag_error(e->tok,
                        "failed to evaluate member call target for '%s'",
                        mc->name ? mc->name : "<unknown>");
          cg->had_error = 1;
          return ny_c0(cg);
        }
        LLVMValueRef name_global =
            const_string_ptr(cg, mc->name, strlen(mc->name));
        LLVMValueRef name_ptr = ny_load(cg, name_global, "");
        ny_dbg_loc(cg, e->tok);
        callee = LLVMBuildCall2(cg->builder, getter->type, getter->value,
                                (LLVMValueRef[]){target_val, name_ptr}, 2,
                                "dyn_func");
        if (mc->args.len == 0) {
          return callee;
        }
        ft = NULL;
        has_sig = false;
        skip_target = true;
        goto skip_static_handling;
      }

      report_undef_symbol(cg, mc->name, e->tok);
      return ny_c0(cg);
    }
    ft = sig_found->type;
    fv = sig_found->value;
    sig_arity = sig_found->arity;
    is_variadic = sig_found->is_variadic;
    has_sig = true;
    callee = fv;
  skip_static_handling:;
  } else {
    const char *name = (c && c->callee && c->callee->kind == NY_E_IDENT)
                           ? c->callee->as.ident.name
                           : NULL;
    uint64_t name_hash = (c && c->callee && c->callee->kind == NY_E_IDENT)
                             ? c->callee->as.ident.hash
                             : 0;
    if (name) {
      binding *b = ny_gencall_lookup_binding(cg, scopes, depth, name,
                                             strlen(name), name_hash);
      if (b) {
        b->is_used = true;
        if (b->direct_callable_sig &&
            ny_sig_in_current_sigs(cg, b->direct_callable_sig)) {
          sig_found = b->direct_callable_sig;
          ft = sig_found->type;
          fv = sig_found->value;
          sig_arity = sig_found->arity;
          is_variadic = sig_found->is_variadic;
          has_sig = true;
          callee = fv;
        } else {
          callee = b->is_slot ? ny_load(cg, b->value, "") : b->value;
        }
      }
    }
    if (!callee) {
      sig_found =
          name ? resolve_overload(cg, name, c->args.len, name_hash) : NULL;
      if (!sig_found && name)
        sig_found = lookup_use_module_fun(cg, name, c->args.len);
      if (!sig_found && name)
        sig_found = lookup_fun(cg, name, name_hash);
      if (!sig_found && name)
        sig_found =
            ny_gencall_lookup_source_file_fun(cg, name, e->tok, c->args.len);
      if (sig_found && !ny_sig_in_current_sigs(cg, sig_found))
        sig_found = NULL;
      if (sig_found) {
        ft = sig_found->type;
        fv = sig_found->value;
        sig_arity = sig_found->arity;
        is_variadic = sig_found->is_variadic;
        has_sig = true;
        callee = fv;
      } else {
        layout_def_t *layout_ctor = name ? lookup_layout(cg, name) : NULL;
        if (layout_ctor)
          return emit_layout_construct(cg, scopes, depth, e, c, layout_ctor);
        if (name) {
          if (ny_env_enabled("NYTRIX_DYNAMIC_GLOBAL_CALLS")) {
            fun_sig *globals_sig = ny_gencall_globals(cg);
            fun_sig *getter = ny_gencall_getter(cg);
            if (globals_sig && getter) {
              ny_dbg_loc(cg, e->tok);
              LLVMValueRef gtbl =
                  LLVMBuildCall2(cg->builder, globals_sig->type,
                                 globals_sig->value, NULL, 0, "");
              LLVMValueRef name_global =
                  const_string_ptr(cg, name, strlen(name));
              LLVMValueRef name_ptr = ny_load(cg, name_global, "");
              LLVMValueRef def_val = ny_c0(cg);
              LLVMValueRef gargs[3] = {gtbl, name_ptr, def_val};
              unsigned gargc = getter->arity >= 3 ? 3 : 2;
              ny_dbg_loc(cg, e->tok);
              callee = LLVMBuildCall2(cg->builder, getter->type, getter->value,
                                      gargs, gargc, "dyn_global");
              ft = NULL;
              has_sig = false;
            } else {
              report_undef_symbol(cg, name, e->tok);
              return ny_c0(cg);
            }
          } else {
            report_undef_symbol(cg, name, e->tok);
            return ny_c0(cg);
          }
        } else {
          callee = gen_expr(cg, scopes, depth, c->callee);
        }
      }
    }
  }
  if (!ft) {
    size_t n = c ? c->args.len : (mc->args.len + 1);
    char buf[32];
    snprintf(buf, sizeof(buf), "__call%zu", n);
    fun_sig *rsig = lookup_fun(cg, buf, 0);
    if (!rsig) {
      report_missing_runtime_call_helper(cg, e->tok, buf, n);
      return ny_c0(cg);
    }
    LLVMTypeRef rty = rsig->type;
    LLVMValueRef rval = rsig->value;
    if (!callee) {
      ny_diag_error(e->tok,
                    "call target resolved to %snone%s — the function or "
                    "variable is undefined",
                    clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
      ny_diag_hint(
          "check the spelling and ensure the function is defined or imported");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    LLVMValueRef callee_int = (LLVMTypeOf(callee) == cg->type_i64)
                                  ? callee
                                  : ny_ptr2i64(cg, callee, "callee_int");
    LLVMValueRef *call_args = malloc(sizeof(LLVMValueRef) * (n + 1));
    if (!call_args) {
      ny_diag_error(e->tok, "out of memory preparing dynamic call arguments");
      cg->had_error = 1;
      return ny_c0(cg);
    }
    call_args[0] = callee_int;
    if (c) {
      for (size_t i = 0; i < n; i++) {
        call_args[i + 1] = gen_expr(cg, scopes, depth, c->args.data[i].val);
        if (!call_args[i + 1]) {
          ny_diag_error(e->tok, "failed to evaluate argument %zu", i + 1);
          cg->had_error = 1;
          free(call_args);
          return ny_c0(cg);
        }
      }
    } else {
      call_args[1] = gen_expr(cg, scopes, depth, mc->target);
      if (!call_args[1]) {
        ny_diag_error(e->tok, "failed to evaluate member call target argument");
        cg->had_error = 1;
        free(call_args);
        return ny_c0(cg);
      }
      for (size_t i = 0; i < mc->args.len; i++) {
        call_args[i + 2] = gen_expr(cg, scopes, depth, mc->args.data[i].val);
        if (!call_args[i + 2]) {
          ny_diag_error(e->tok, "failed to evaluate argument %zu", i + 2);
          cg->had_error = 1;
          free(call_args);
          return ny_c0(cg);
        }
      }
    }
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef res =
        LLVMBuildCall2(cg->builder, rty, rval, call_args, (unsigned)n + 1, "");
    free(call_args);
    return res;
  }
  size_t call_argc =
      c ? c->args.len : (skip_target ? mc->args.len : mc->args.len + 1);
  fun_sig sig_snapshot = {0};
  fun_sig *sig_meta = NULL;
  fun_sig *mono_base_sig = NULL;
  LLVMTypeRef mono_base_ft = NULL;
  LLVMValueRef mono_base_callee = NULL;
  if (has_sig && sig_found) {
    if (ny_sig_in_current_sigs(cg, sig_found)) {
      sig_snapshot = *sig_found;
      sig_meta = &sig_snapshot;
    } else {
      has_sig = false;
      sig_found = NULL;
    }
  }

  if (has_sig) {
    fun_sig *mono_sig = ny_try_monomorphize_call(cg, scopes, depth, e, sig_meta,
                                                 c, mc, skip_target, call_argc);
    if (mono_sig && ny_sig_in_current_sigs(cg, mono_sig)) {
      mono_base_sig = sig_found;
      mono_base_ft = ft;
      mono_base_callee = callee;
      sig_snapshot = *mono_sig;
      sig_meta = &sig_snapshot;
      sig_found = mono_sig;
      ft = mono_sig->type;
      fv = mono_sig->value;
      callee = fv;
      sig_arity = mono_sig->arity;
      is_variadic = mono_sig->is_variadic;
    }
    if (!check_call_arity_diag(cg, e->tok, sig_meta, is_variadic, sig_arity,
                               call_argc, mc && !skip_target)) {
      return ny_c0(cg);
    }
    if (c) {
      LLVMValueRef thread_api =
          ny_gencall_try_thread_call_api(cg, scopes, depth, e, sig_meta, c);
      if (thread_api)
        return thread_api;
    }
  }

  bool native_variadic = has_sig && is_variadic && sig_meta &&
                         sig_meta->is_extern && sig_meta->is_native_abi;
  size_t sig_argc = (has_sig && is_variadic && !native_variadic)
                        ? (size_t)sig_arity
                        : (has_sig ? (size_t)sig_arity : call_argc);

  if (native_variadic && call_argc > sig_argc)
    sig_argc = call_argc;
  size_t final_argc = (sig_argc > call_argc) ? sig_argc : call_argc;
  LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * final_argc);
  if (!args) {
    ny_diag_error(e->tok, "out of memory preparing call arguments");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  size_t user_args_len = c ? c->args.len : mc->args.len;
  call_arg_t *user_args = c ? c->args.data : mc->args.data;
  bool proven_int_cast_fast = ny_proven_int_cast_fast_enabled(cg);
  expr_t **arg_exprs =
      proven_int_cast_fast ? calloc(final_argc, sizeof(expr_t *)) : NULL;
  LLVMValueRef *mono_tagged_args = NULL;
  if (proven_int_cast_fast && !arg_exprs) {
    ny_diag_error(e->tok, "out of memory preparing call argument proofs");
    cg->had_error = 1;
    free(args);
    return ny_c0(cg);
  }
  expr_t *default_expr = NULL;
  ny_param_list *func_params = NULL;
  if (has_sig && sig_meta && sig_meta->stmt_t) {
    if (sig_meta->stmt_t->kind == NY_S_FUNC)
      func_params = &sig_meta->stmt_t->as.fn.params;
    else if (sig_meta->stmt_t->kind == NY_S_EXTERN)
      func_params = &sig_meta->stmt_t->as.ext.params;
  }
  for (size_t i = 0; i < final_argc; i++) {
    size_t user_idx = (mc && !skip_target) ? (i - 1) : i;
    const char *param_type = (func_params && i < func_params->len)
                                 ? func_params->data[i].type
                                 : NULL;
    if (!param_type && sig_meta && i < sig_meta->param_types.len)
      param_type = sig_meta->param_types.data[i];
    expr_t *expr_for_check = NULL;
    if (mc && !skip_target && i == 0) {
      expr_for_check = mc->target;
      if (arg_exprs)
        arg_exprs[i] = expr_for_check;
      if (param_type && expr_for_check)
        ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                    expr_for_check, expr_for_check->tok,
                                    "argument");
      if (!param_type && expr_for_check &&
          !ny_gencall_check_math_contract(cg, scopes, depth, sig_meta,
                                          expr_for_check))
        goto call_fail;
      args[i] = gen_expr(cg, scopes, depth, mc->target);
      if (!args[i]) {
        ny_diag_error(e->tok, "failed to evaluate member target argument");
        cg->had_error = 1;
        goto call_fail;
      }
    } else if (has_sig && is_variadic && !native_variadic &&
               i == (size_t)sig_arity - 1) {

      fun_sig *ls_s = lookup_fun(cg, "__list_new", 0);
      fun_sig *st_s = lookup_fun(cg, "__store64_idx", 0);
      if (!ls_s || !st_s) {
        ny_diag_error(e->tok,
                      "variadic arguments require __list_new/rt_store64_idx "
                      "helpers");
        cg->had_error = 1;
        goto call_fail;
      }
      size_t var_count =
          (user_args_len > user_idx) ? (user_args_len - user_idx) : 0;
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef vl = LLVMBuildCall2(
          cg->builder, ls_s->type, ls_s->value,
          (LLVMValueRef[]){LLVMConstInt(
              cg->type_i64, (((uint64_t)var_count << 1) | 1u), false)},
          1, "");
      size_t out_i = 0;
      for (size_t j = user_idx; j < user_args_len; j++) {
        call_arg_t *a = &user_args[j];
        LLVMValueRef av = gen_expr(cg, scopes, depth, a->val);
        if (!av) {
          ny_diag_error(e->tok, "failed to evaluate variadic argument %zu",
                        j + 1);
          cg->had_error = 1;
          goto call_fail;
        }
        if (a->name) {
          fun_sig *ks_s = ny_gencall_kwarg(cg);
          if (!ks_s) {
            ny_diag_error(e->tok, "keyword args require '__kwarg'");
            cg->had_error = 1;
            goto call_fail;
          }
          LLVMValueRef name_runtime_global =
              const_string_ptr(cg, a->name, strlen(a->name));
          LLVMValueRef name_ptr = ny_load(cg, name_runtime_global, "");
          ny_dbg_loc(cg, e->tok);
          av = LLVMBuildCall2(cg->builder, ks_s->type, ks_s->value,
                              (LLVMValueRef[]){name_ptr, av}, 2, "");
        }
        uint64_t tagged_off =
            ((((uint64_t)16 + (uint64_t)out_i * 8u) << 1) | 1u);
        ny_dbg_loc(cg, e->tok);
        (void)LLVMBuildCall2(
            cg->builder, st_s->type, st_s->value,
            (LLVMValueRef[]){vl, LLVMConstInt(cg->type_i64, tagged_off, false),
                             av},
            3, "");
        out_i++;
      }

      (void)LLVMBuildCall2(
          cg->builder, st_s->type, st_s->value,
          (LLVMValueRef[]){
              vl, ny_c1(cg),
              LLVMConstInt(cg->type_i64, (((uint64_t)out_i << 1) | 1u), false)},
          3, "");
      args[i] = vl;
      break;
    } else if (user_idx < user_args_len) {
      expr_for_check = user_args[user_idx].val;
      if (arg_exprs)
        arg_exprs[i] = expr_for_check;
      if (param_type && expr_for_check)
        ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                    expr_for_check, expr_for_check->tok,
                                    "argument");
      if (!param_type && expr_for_check &&
          !ny_gencall_check_math_contract(cg, scopes, depth, sig_meta,
                                          expr_for_check))
        goto call_fail;
      args[i] = gen_expr(cg, scopes, depth, user_args[user_idx].val);
      if (!args[i]) {
        ny_diag_error(e->tok, "failed to evaluate argument %zu", i + 1);
        cg->had_error = 1;
        goto call_fail;
      }
    } else if (has_sig && sig_arity > (int)i && i < user_args_len) {
      args[i] = ny_c0(cg);
    } else {
      default_expr = NULL;
      if (has_sig && sig_meta && sig_meta->stmt_t &&
          sig_meta->stmt_t->kind == NY_S_FUNC) {
        func_params = &sig_meta->stmt_t->as.fn.params;
        size_t param_idx = i;
        if (param_idx < func_params->len) {
          default_expr = func_params->data[param_idx].def;
        }
      }
      if (default_expr) {
        if (arg_exprs)
          arg_exprs[i] = default_expr;
        if (param_type)
          ensure_expr_type_compatible(cg, scopes, depth, param_type,
                                      default_expr, default_expr->tok,
                                      "argument");
        if (!param_type && !ny_gencall_check_math_contract(
                               cg, scopes, depth, sig_meta, default_expr))
          goto call_fail;
        args[i] = gen_expr(cg, scopes, depth, default_expr);
        if (!args[i]) {
          ny_diag_error(e->tok, "failed to evaluate default argument %zu",
                        i + 1);
          cg->had_error = 1;
          goto call_fail;
        }
      } else {
        args[i] = ny_c0(cg);
      }
    }
  }
  if (mono_base_sig && final_argc > 0) {
    mono_tagged_args = malloc(sizeof(LLVMValueRef) * final_argc);
    if (!mono_tagged_args) {
      ny_diag_error(e->tok,
                    "out of memory preparing monomorphic fallback arguments");
      cg->had_error = 1;
      goto call_fail;
    }
    memcpy(mono_tagged_args, args, sizeof(LLVMValueRef) * final_argc);
  }
  if (has_sig && sig_meta) {
    ny_param_list *call_params = NULL;
    if (sig_meta->stmt_t && sig_meta->stmt_t->kind == NY_S_EXTERN) {
      call_params = &sig_meta->stmt_t->as.ext.params;
    } else if (sig_meta->stmt_t && sig_meta->stmt_t->kind == NY_S_FUNC) {
      call_params = &sig_meta->stmt_t->as.fn.params;
    }

    if (call_params && call_params->len > 0) {
      size_t max_conv = call_params->len;
      size_t call_limit =
          (has_sig && is_variadic) ? (size_t)sig_arity : final_argc;
      if (max_conv > call_limit)
        max_conv = call_limit;
      for (size_t i = 0; i < max_conv; i++) {
        if (sig_meta->is_variadic && (int)i >= sig_arity - 1)
          break;
        const char *tname = call_params->data[i].type;
        if (abi_sig_param_needs_native_coerce(sig_meta, tname)) {
          bool mono_int_arg = sig_meta->name &&
                              strstr(sig_meta->name, "__ny_mono_") && tname &&
                              ny_type_is(tname, "int");
          bool proven_int_arg = false;
          if (proven_int_cast_fast && arg_exprs && i < final_argc &&
              arg_exprs[i] &&
              (mono_int_arg || ny_proven_int_cast_fast_allowed(arg_exprs[i]))) {
            proven_int_arg =
                ny_is_proven_int(cg, scopes, depth, arg_exprs[i], args[i]);
          }
          args[i] =
              ny_coerce_to_abi_proven_int(cg, args[i], tname, proven_int_arg);
        }
      }
    } else if (sig_meta->param_types.len > 0) {
      size_t max_conv = sig_meta->param_types.len;
      size_t call_limit =
          (has_sig && is_variadic) ? (size_t)sig_arity : final_argc;
      if (max_conv > call_limit)
        max_conv = call_limit;
      for (size_t i = 0; i < max_conv; i++) {
        if (sig_meta->is_variadic && (int)i >= sig_arity - 1)
          break;
        const char *tname = sig_meta->param_types.data[i];
        if (abi_sig_param_needs_native_coerce(sig_meta, tname)) {
          bool proven_int_arg = false;
          if (proven_int_cast_fast && arg_exprs && i < final_argc &&
              arg_exprs[i] && ny_proven_int_cast_fast_allowed(arg_exprs[i]))
            proven_int_arg =
                ny_is_proven_int(cg, scopes, depth, arg_exprs[i], args[i]);
          args[i] =
              ny_coerce_to_abi_proven_int(cg, args[i], tname, proven_int_arg);
        }
      }
    }

    if (!call_params && sig_meta->param_types.len == 0 && ft &&
        sig_meta->is_extern && sig_meta->is_native_abi) {
      unsigned np = LLVMCountParamTypes(ft);
      LLVMTypeRef *pts = NULL;
      if (np > 0) {
        pts = (LLVMTypeRef *)alloca(sizeof(LLVMTypeRef) * np);
        LLVMGetParamTypes(ft, pts);
      }
      for (size_t i = 0; i < final_argc; i++) {
        const char *tname = NULL;
        if (i < (size_t)np) {

          LLVMTypeKind k = LLVMGetTypeKind(pts[i]);
          if (k == LLVMPointerTypeKind) {
            tname = "ptr";
          } else if (k == LLVMIntegerTypeKind) {
            switch (LLVMGetIntTypeWidth(pts[i])) {
            case 8:
              tname = "i8";
              break;
            case 16:
              tname = "i16";
              break;
            case 32:
              tname = "i32";
              break;
            case 64:

              tname = "u64";
              break;
            default:
              break;
            }
          } else if (k == LLVMFloatTypeKind) {
            tname = "f32";
          } else if (k == LLVMDoubleTypeKind) {
            tname = "f64";
          }
        } else if (native_variadic) {

          LLVMValueRef v = args[i];
          LLVMTypeRef vty = v ? LLVMTypeOf(v) : NULL;
          if (vty && LLVMGetTypeKind(vty) == LLVMPointerTypeKind) {

          } else {

            LLVMValueRef one = ny_c1(cg);
            LLVMValueRef lsb = ny_and(cg, v, one, "vi_lsb");
            LLVMValueRef is_tagged = ny_eq(cg, lsb, one, "vi_tagged");
            LLVMValueRef untagged =
                LLVMBuildLShr(cg->builder, v, one, "vi_untag");
            v = ny_select(cg, is_tagged, untagged, v, "vi_val");
            args[i] = v;
          }
          continue;
        }
        if (tname)
          args[i] = ny_coerce_to_abi(cg, args[i], tname);
      }
    }
  }
  if (has_sig && ny_gencall_is_thread_attr(sig_meta)) {
    if (is_variadic || final_argc > 15) {
      ny_diag_error(e->tok, "@thread call '%s' supports up to 15 arguments",
                    sig_meta->name ? sig_meta->name : "<anon>");
      ny_diag_hint("reduce arguments or pass a packed object");
      cg->had_error = 1;
      free(arg_exprs);
      free(mono_tagged_args);
      free(args);
      return ny_c0(cg);
    }
    LLVMTypeRef ret_ty = LLVMGetReturnType(ft);
    if (ret_ty != cg->type_i64) {
      ny_diag_error(e->tok,
                    "@thread function '%s' must return tagged int/any (i64)",
                    sig_meta->name ? sig_meta->name : "<anon>");
      cg->had_error = 1;
      free(arg_exprs);
      free(mono_tagged_args);
      free(args);
      return ny_c0(cg);
    }
    bool detach_stmt_call = cg->thread_detach_stmt_call;
    fun_sig *spawn_sig =
        detach_stmt_call ? NULL : lookup_fun(cg, "__thread_spawn_call", 0);
    fun_sig *launch_sig =
        detach_stmt_call ? lookup_fun(cg, "__thread_launch_call", 0) : NULL;
    fun_sig *join_sig =
        detach_stmt_call ? NULL : lookup_fun(cg, "__thread_join", 0);

    if ((!detach_stmt_call && (!spawn_sig || !join_sig)) ||
        (detach_stmt_call && !launch_sig)) {
      ny_diag_error(e->tok, "missing runtime thread helpers");
      if (detach_stmt_call) {
        ny_diag_hint("expected rt_thread_launch_call in runtime symbols");
      } else {
        ny_diag_hint("expected rt_thread_spawn_call/rt_thread_join in runtime "
                     "symbols");
      }
      cg->had_error = 1;
      free(arg_exprs);
      free(mono_tagged_args);
      free(args);
      return ny_c0(cg);
    }
    LLVMValueRef fn_val =
        (LLVMTypeOf(callee) == cg->type_i64)
            ? callee
            : ny_ptr2i64(cg, callee, NY_LLVM_NAME(cg, "thread_fn"));

    LLVMValueRef argc_val =
        LLVMConstInt(cg->type_i64, (((uint64_t)final_argc << 1) | 1), false);
    LLVMValueRef argv_ptr = ny_c0(cg);
    if (final_argc > 0) {
      LLVMTypeRef argv_ty = LLVMArrayType(cg->type_i64, (unsigned)final_argc);
      ny_dbg_loc(cg, e->tok);
      LLVMValueRef argv_stack = LLVMBuildAlloca(
          cg->builder, argv_ty, NY_LLVM_NAME(cg, "thread_argv"));
      LLVMSetAlignment(argv_stack, 16);
      for (size_t i = 0; i < final_argc; i++) {
        LLVMValueRef idxs[2] = {ny_c0(cg),
                                LLVMConstInt(cg->type_i64, (uint64_t)i, false)};
        LLVMValueRef slot =
            LLVMBuildGEP2(cg->builder, argv_ty, argv_stack, idxs, 2, "");
        ny_store(cg, slot, args[i]);
      }
      argv_ptr = ny_ptr2i64(cg, argv_stack, "thread_argv_ptr");
    }

    ny_dbg_loc(cg, e->tok);
    LLVMValueRef handle = NULL;
    if (detach_stmt_call) {
      LLVMBuildCall2(cg->builder, launch_sig->type, launch_sig->value,
                     (LLVMValueRef[]){fn_val, argc_val, argv_ptr}, 3,
                     "thread_launch");
    } else {
      handle = LLVMBuildCall2(cg->builder, spawn_sig->type, spawn_sig->value,
                              (LLVMValueRef[]){fn_val, argc_val, argv_ptr}, 3,
                              "thread_spawn");
    }
    if (detach_stmt_call) {
      free(arg_exprs);
      free(mono_tagged_args);
      free(args);
      return ny_c0(cg);
    }
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef joined =
        LLVMBuildCall2(cg->builder, join_sig->type, join_sig->value, &handle, 1,
                       "thread_join");
    free(arg_exprs);
    free(mono_tagged_args);
    free(args);
    return joined;
  }
  if (!ny_gencall_skip_panic_site_file(e->tok) &&
      ny_gencall_is_panic_site(
          c ? (c->callee && c->callee->kind == NY_E_IDENT
                   ? c->callee->as.ident.name
                   : NULL)
            : (sig_meta ? sig_meta->name : (mc ? mc->name : NULL)))) {
    ny_emit_trace_loc_force(cg, e->tok);
  }

  unsigned call_nargs =
      (unsigned)(native_variadic ? final_argc
                                 : (has_sig && is_variadic ? (size_t)sig_arity
                                                           : final_argc));
  if (!ft || !callee) {
    ny_diag_error(e->tok, "invalid call target");
    cg->had_error = 1;
    free(arg_exprs);
    free(mono_tagged_args);
    free(args);
    return ny_c0(cg);
  }

  if (has_sig && sig_meta && sig_meta->native_sret_return) {
    layout_def_t *layout = abi_layout_from_name(cg, sig_meta->return_type);
    fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
    if (!layout || !layout->llvm_type || !malloc_sig) {
      ny_diag_error(e->tok, "missing native struct-return support for '%s'",
                    sig_meta->name ? sig_meta->name : "<ffi>");
      cg->had_error = 1;
      free(arg_exprs);
      free(mono_tagged_args);
      free(args);
      return ny_c0(cg);
    }
    LLVMValueRef size_arg =
        LLVMConstInt(cg->type_i64, (((uint64_t)layout->size << 1) | 1u), false);
    LLVMValueRef ret_ptr_i64 =
        LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                       &size_arg, 1, NY_LLVM_NAME(cg, "ffi_sret_alloc"));
    LLVMValueRef ret_ptr = abi_layout_ptr_from_value(
        cg, ret_ptr_i64, layout->llvm_type, "ffi_sret_ptr");
    if (LLVMTypeOf(ret_ptr) != cg->type_i8ptr)
      ret_ptr = LLVMBuildBitCast(cg->builder, ret_ptr, cg->type_i8ptr,
                                 NY_LLVM_NAME(cg, "ffi_sret_i8ptr"));

    LLVMValueRef *sret_args =
        (LLVMValueRef *)malloc(sizeof(LLVMValueRef) * (call_nargs + 1));
    if (!sret_args) {
      ny_diag_error(e->tok,
                    "out of memory preparing native struct-return call");
      cg->had_error = 1;
      free(arg_exprs);
      free(mono_tagged_args);
      free(args);
      return ny_c0(cg);
    }
    sret_args[0] = ret_ptr;
    for (unsigned i = 0; i < call_nargs; i++)
      sret_args[i + 1] = args[i];
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef sret_call =
        LLVMBuildCall2(cg->builder, ft, callee, sret_args, call_nargs + 1, "");
    abi_apply_native_layout_call_attrs(cg, sret_call, sig_meta);
    free(sret_args);
    free(arg_exprs);
    free(mono_tagged_args);
    free(args);
    return ret_ptr_i64;
  }

  bool is_tail_call = false;
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  if (cg->tail_call_depth > 0 && cur_bb && ft && !is_variadic &&
      !sig_meta->is_extern) {
    LLVMValueRef parent_fn = cg->current_fn_value
                                 ? cg->current_fn_value
                                 : LLVMGetBasicBlockParent(cur_bb);
    if (parent_fn) {
      LLVMTypeRef callee_ret_ty = LLVMGetReturnType(ft);
      LLVMTypeRef parent_ret_ty =
          LLVMGetReturnType(LLVMGlobalGetValueType(parent_fn));
      is_tail_call = (callee_ret_ty == parent_ret_ty);
    }
  }

  LLVMValueRef res = 0;
  bool memo_alias_safe = has_sig && sig_meta && !sig_meta->args_escape &&
                         !sig_meta->args_mutated && !sig_meta->returns_alias;
  bool memo_impure_effect_safe =
      has_sig && sig_meta && sig_meta->effects_known &&
      (sig_meta->effects & (NY_FX_IO | NY_FX_FFI | NY_FX_THREAD)) == 0;
  bool impure_return_ok = has_sig && sig_meta &&
                          ny_memo_impure_return_allowed(sig_meta->return_type);
  bool cache_attr_forced =
      has_sig && sig_meta && ny_gencall_has_cache_attr(sig_meta);
  bool memo_enabled = cg->auto_memoize || cache_attr_forced;
  bool memo_impure_enabled = cg->auto_memoize_impure || cache_attr_forced;
  bool memo_for_impure = has_sig && sig_meta && !sig_meta->is_pure &&
                         memo_impure_enabled && sig_meta->is_memo_safe &&
                         memo_alias_safe && memo_impure_effect_safe &&
                         impure_return_ok;
  bool memo_eligible = has_sig && sig_meta && memo_alias_safe &&
                       (sig_meta->is_pure || memo_for_impure);
  bool did_mono_guarded_call = false;
  if (mono_base_sig && mono_base_ft && mono_base_callee && mono_tagged_args &&
      sig_meta && sig_meta->stmt_t && sig_meta->stmt_t->kind == NY_S_FUNC &&
      !is_variadic && LLVMGetReturnType(ft) == cg->type_i64 &&
      LLVMGetReturnType(mono_base_ft) == cg->type_i64) {
    ny_param_list *mono_params = &sig_meta->stmt_t->as.fn.params;
    LLVMValueRef guard = ny_cbool(cg, 1);
    bool needs_guard = false;
    size_t guard_n =
        mono_params->len < final_argc ? mono_params->len : final_argc;
    for (size_t i = 0; i < guard_n; i++) {
      const char *ptype = mono_params->data[i].type;
      if (ptype && ny_type_is(ptype, "int")) {
        if (arg_exprs && i < final_argc && arg_exprs[i] &&
            ny_is_proven_int(cg, scopes, depth, arg_exprs[i],
                             mono_tagged_args[i])) {
          continue;
        }
        guard = ny_and(cg, guard, ny_is_tagged_int(cg, mono_tagged_args[i]),
                       NY_LLVM_NAME(cg, "mono_arg_is_small_int"));
        needs_guard = true;
      }
    }
    if (needs_guard) {
      LLVMValueRef cur_fn = ny_cur_fn(cg);
      if (cur_fn) {
        LLVMBasicBlockRef fast_bb = ny_bb_fn(cur_fn, "mono.call.fast");
        LLVMBasicBlockRef slow_bb = ny_bb_fn(cur_fn, "mono.call.fallback");
        LLVMBasicBlockRef done_bb = ny_bb_fn(cur_fn, "mono.call.done");
        ny_cond_br(cg, guard, fast_bb, slow_bb);

        ny_pos(cg, fast_bb);
        ny_dbg_loc(cg, e->tok);
        LLVMValueRef fast_res =
            LLVMBuildCall2(cg->builder, ft, callee, args, call_nargs, "");
        LLVMBasicBlockRef fast_end = ny_cur_block(cg);
        if (!ny_has_terminator(cg))
          ny_br(cg, done_bb);

        ny_pos(cg, slow_bb);
        ny_dbg_loc(cg, e->tok);
        LLVMValueRef slow_res =
            LLVMBuildCall2(cg->builder, mono_base_ft, mono_base_callee,
                           mono_tagged_args, call_nargs, "");
        const char *slow_ret_type =
            sig_meta->abi_return_type ? sig_meta->abi_return_type
                                      : sig_meta->return_type;
        if (abi_sig_type_needs_native_coerce(cg, sig_meta, slow_ret_type)) {
          if (ny_type_is(slow_ret_type, "f32") ||
              ny_type_is(slow_ret_type, "f64") ||
              ny_type_is(slow_ret_type, "f128")) {
            slow_res = ny_unbox_float(cg, slow_res);
            if (ny_type_is(slow_ret_type, "f32"))
              slow_res = LLVMBuildFPTrunc(cg->builder, slow_res, cg->type_f32,
                                          NY_LLVM_NAME(cg, "mono_slow_f32"));
            else if (ny_type_is(slow_ret_type, "f128"))
              slow_res = LLVMBuildFPExt(cg->builder, slow_res, cg->type_f128,
                                        NY_LLVM_NAME(cg, "mono_slow_f128"));
          } else if (ny_type_is(slow_ret_type, "i64") ||
                     ny_type_is(slow_ret_type, "u64")) {
            slow_res = ny_untag_int(cg, slow_res);
          }
        }
        LLVMBasicBlockRef slow_end = ny_cur_block(cg);
        if (!ny_has_terminator(cg))
          ny_br(cg, done_bb);

        ny_pos(cg, done_bb);
        res = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "mono_call"));
        LLVMAddIncoming(res, (LLVMValueRef[]){fast_res, slow_res},
                        (LLVMBasicBlockRef[]){fast_end, slow_end}, 2);
        did_mono_guarded_call = true;
      }
    }
  }
  if (!did_mono_guarded_call && memo_enabled && memo_eligible &&
      !sig_meta->is_extern && !sig_meta->is_variadic &&
      !sig_meta->is_recursive && !ny_gencall_is_thread_attr(sig_meta)) {
    res =
        ny_build_memoized_direct_call(cg, e->tok, ft, callee, args, call_nargs,
                                      memo_enabled, memo_for_impure);
  } else if (!did_mono_guarded_call) {
    ny_dbg_loc(cg, e->tok);
    res = LLVMBuildCall2(cg->builder, ft, callee, args, call_nargs, "");
    abi_apply_native_layout_call_attrs(cg, res, sig_meta);

    if (is_tail_call && !sig_meta->is_extern && !sig_meta->is_variadic) {
      LLVMSetTailCallKind(res, LLVMTailCallKindTail);
    }
  }
  free(arg_exprs);
  free(mono_tagged_args);
  free(args);
  if (has_sig && sig_meta) {
    LLVMTypeRef ret_ty = LLVMGetReturnType(ft);
    if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
      return ny_c0(cg);
    const char *ret_type =
        sig_meta->abi_return_type ? sig_meta->abi_return_type
                                  : sig_meta->return_type;
    if (abi_sig_type_needs_native_coerce(cg, sig_meta, ret_type)) {
      return ny_box_abi_result(cg, res, ret_type);
    }
  }
  return res;

call_fail:
  free(arg_exprs);
  free(mono_tagged_args);
  free(args);
  return ny_c0(cg);
}
