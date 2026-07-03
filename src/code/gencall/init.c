#include "base/util.h"
#include "parse/json.h"
#include "../priv.h"
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

static bool ny_gencall_builtin_name_is(const char *name, const char *tail,
                                       bool shadowed);

static LLVMValueRef ny_try_addr_of_local_intrinsic(codegen_t *cg, scope *scopes,
                                                   size_t depth, expr_t *e,
                                                   expr_call_t *c,
                                                   const char *builtin_name,
                                                   bool builtin_shadowed) {
  if (!ny_gencall_builtin_name_is(builtin_name, "addr_of", builtin_shadowed))
    return NULL;
  if (!c || c->args.len != 1 || c->args.data[0].name) {
    ny_diag_error(e->tok, "addr_of(local) expects exactly one positional local");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  expr_t *arg = c->args.data[0].val;
  if (!arg || arg->kind != NY_E_IDENT || !arg->as.ident.name) {
    ny_diag_error(arg ? arg->tok : e->tok,
                  "addr_of(local) currently supports local identifiers only");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  size_t name_len = (size_t)arg->tok.len;
  if (name_len == 0)
    name_len = strlen(arg->as.ident.name);
  binding *b = ny_gencall_lookup_binding(cg, scopes, depth, arg->as.ident.name,
                                         name_len, arg->as.ident.hash);
  if (!b) {
    ny_diag_error(arg->tok, "addr_of(local) could not resolve '%s'",
                  arg->as.ident.name);
    cg->had_error = 1;
    return ny_c0(cg);
  }
  if (b->raw_int_value && b->is_int_slot)
    return ny_ptr2i64(cg, b->raw_int_value, NY_LLVM_NAME(cg, "addr_of_raw_int"));
  if (b->is_slot && b->value)
    return ny_ptr2i64(cg, b->value, NY_LLVM_NAME(cg, "addr_of_slot"));
  ny_diag_error(arg->tok, "addr_of(local) requires an addressable stack local");
  ny_diag_hint("use an addressable local such as 'mut i64 %s = ...'",
               arg->as.ident.name);
  cg->had_error = 1;
  return ny_c0(cg);
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

#include "intrinsics.c"
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
#include "abi.c"
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
#include "mono.c"
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
    LLVMValueRef addr_of_local = ny_try_addr_of_local_intrinsic(
        cg, scopes, depth, e, c, builtin_name, builtin_name_shadowed);
    if (addr_of_local)
      return addr_of_local;
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
