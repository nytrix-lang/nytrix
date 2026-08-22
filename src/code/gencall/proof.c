/*
 * Compile-time builtin analysis: constant scalar recognition, range proofs,
 * collection bounds, static assertions, and proof witness construction.
 */

#ifdef NYTRIX_HAS_Z3
#include <z3.h>
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

static bool ny_proof_nonnegative_div_range(int64_t lmin, int64_t lmax,
                                           int64_t rmin, int64_t rmax,
                                           int64_t *out_lo, int64_t *out_hi) {
  if (lmin < 0 || lmax < lmin || rmin <= 0 || rmax < rmin)
    return false;
  if (out_lo)
    *out_lo = lmin / rmax;
  if (out_hi)
    *out_hi = lmax / rmin;
  return true;
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
  idx_v = ny_llvm_cast_to_i64(cg, idx_v, name ? name : "idx");
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
  if (arg && arg->kind == NY_E_DEREF && arg->as.deref.target) {
    LLVMValueRef address = gen_expr(cg, scopes, depth, arg->as.deref.target);
    if (!address)
      return ny_c0(cg);
    if (LLVMGetTypeKind(LLVMTypeOf(address)) == LLVMPointerTypeKind)
      return ny_ptr2i64(cg, address, NY_LLVM_NAME(cg, "addr_of_deref"));
    if (LLVMGetTypeKind(LLVMTypeOf(address)) == LLVMIntegerTypeKind)
      return ny_llvm_cast_to_i64(cg, address, "addr_of_deref");
    ny_diag_error(arg->tok,
                  "addr_of(deref) target did not produce a pointer address");
    cg->had_error = 1;
    return ny_c0(cg);
  }
  if (!arg || arg->kind != NY_E_IDENT || !arg->as.ident.name) {
    ny_diag_error(arg ? arg->tok : e->tok,
                  "addr_of supports local and dereferenced pointer lvalues");
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
        return "nil";
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

/*
 * Cross-function proof propagation. A canonical-proposition evaluator
 * (strings produced by ny_proof_type_from_expr) resolves `name:X` through a
 * callback so proof<P> parameters, @proves conditions, and pure-function
 * output ranges can be checked against caller-side integer ranges.
 */

#define NY_PROOF_CANON_MAX_DEPTH 64

typedef bool (*ny_proof_name_range_fn)(void *ctx, const char *name,
                                       int64_t *out_lo, int64_t *out_hi);

/*
 * Split "binary:OP(l,r)" into op (opbuf) and owned l/r substrings.
 */
static bool ny_proof_canon_split_binary(const char *s, char *opbuf,
                                        size_t opcap, char **out_l,
                                        char **out_r) {
  if (strncmp(s, "binary:", 7) != 0)
    return false;
  const char *open = strchr(s, '(');
  if (!open)
    return false;
  size_t oplen = (size_t)(open - (s + 7));
  if (oplen == 0 || oplen >= opcap)
    return false;
  memcpy(opbuf, s + 7, oplen);
  opbuf[oplen] = '\0';
  const char *q = open + 1;
  int d = 1;
  const char *comma = NULL, *close = NULL;
  for (; *q; ++q) {
    if (*q == '(')
      ++d;
    else if (*q == ')') {
      if (--d == 0) {
        close = q;
        break;
      }
    } else if (*q == ',' && d == 1 && !comma) {
      comma = q;
    }
  }
  if (!comma || !close)
    return false;
  *out_l = ny_strndup(open + 1, (size_t)(comma - open - 1));
  *out_r = ny_strndup(comma + 1, (size_t)(close - comma - 1));
  return *out_l && *out_r;
}

/*
 * Range of a canonical arithmetic atom over a name environment.
 */
static bool ny_proof_canon_range(const char *s, ny_proof_name_range_fn env,
                                 void *ctx, int64_t *out_lo, int64_t *out_hi,
                                 int depth) {
  if (!s || depth > NY_PROOF_CANON_MAX_DEPTH)
    return false;
  if (strncmp(s, "int:", 4) == 0) {
    *out_lo = *out_hi = strtoll(s + 4, NULL, 10);
    return true;
  }
  if (strncmp(s, "name:", 5) == 0) {
    char name[256];
    const char *q = s + 5;
    size_t n = 0;
    while (*q && *q != ',' && *q != ')' && n + 1 < sizeof(name))
      name[n++] = *q++;
    name[n] = '\0';
    if (!env || !env(ctx, name, out_lo, out_hi))
      return false;
    return true;
  }
  if (strncmp(s, "unary:-(", 8) == 0) {
    size_t slen = strlen(s);
    if (slen <= 9 || s[slen - 1] != ')')
      return false;
    char *inner = ny_strndup(s + 8, slen - 9);
    int64_t ilo = 0, ihi = 0;
    bool ok =
        inner && ny_proof_canon_range(inner, env, ctx, &ilo, &ihi, depth + 1);
    free(inner);
    if (!ok || ilo == INT64_MIN || ihi == INT64_MIN)
      return false;
    *out_lo = -ihi;
    *out_hi = -ilo;
    return true;
  }
  if (strncmp(s, "binary:", 7) == 0) {
    char opbuf[8];
    char *l = NULL, *r = NULL;
    if (!ny_proof_canon_split_binary(s, opbuf, sizeof(opbuf), &l, &r)) {
      free(l);
      free(r);
      return false;
    }
    int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
    bool lok = ny_proof_canon_range(l, env, ctx, &lmin, &lmax, depth + 1);
    bool rok = ny_proof_canon_range(r, env, ctx, &rmin, &rmax, depth + 1);
    free(l);
    free(r);
    if (!lok || !rok)
      return false;
    int64_t lo = 0, hi = 0;
    bool ok = true;
    if (strcmp(opbuf, "+") == 0) {
      ok = ny_add_range_ok(lmin, rmin, &lo) &&
           ny_add_range_ok(lmax, rmax, &hi);
    } else if (strcmp(opbuf, "-") == 0) {
      ok = ny_sub_range_ok(lmin, rmax, &lo) &&
           ny_sub_range_ok(lmax, rmin, &hi);
    } else if (strcmp(opbuf, "*") == 0) {
      int64_t c[4];
      ok = ny_mul_range_ok(lmin, rmin, &c[0]) &&
           ny_mul_range_ok(lmin, rmax, &c[1]) &&
           ny_mul_range_ok(lmax, rmin, &c[2]) &&
           ny_mul_range_ok(lmax, rmax, &c[3]);
      if (ok) {
        lo = c[0];
        hi = c[0];
        for (int i = 1; i < 4; ++i) {
          if (c[i] < lo)
            lo = c[i];
          if (c[i] > hi)
            hi = c[i];
        }
      }
    } else if (strcmp(opbuf, "/") == 0) {
      ok = ny_proof_nonnegative_div_range(lmin, lmax, rmin, rmax,
                                          &lo, &hi);
    } else if (strcmp(opbuf, "%") == 0) {
      if (rmin <= 0 || rmax < rmin || lmin < 0)
        ok = false;
      else {
        lo = 0;
        hi = lmax < rmax ? lmax : rmax - 1;
      }
    } else if (strcmp(opbuf, "&") == 0) {
      if (rmin != rmax || rmax < 0 || lmin < 0)
        ok = false;
      else {
        lo = 0;
        hi = rmax;
      }
    } else if (strcmp(opbuf, "|") == 0 || strcmp(opbuf, "^^") == 0) {
      bool is_or = strcmp(opbuf, "|") == 0;
      int64_t c[4];
      c[0] = is_or ? (lmin | rmin) : (lmin ^ rmin);
      c[1] = is_or ? (lmin | rmax) : (lmin ^ rmax);
      c[2] = is_or ? (lmax | rmin) : (lmax ^ rmin);
      c[3] = is_or ? (lmax | rmax) : (lmax ^ rmax);
      lo = c[0];
      hi = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < lo)
          lo = c[i];
        if (c[i] > hi)
          hi = c[i];
      }
    } else if (strcmp(opbuf, "<<") == 0) {
      if (rmin != rmax || rmax < 0 || rmax >= 64)
        ok = false;
      else if (rmax == 0) {
        lo = lmin;
        hi = lmax;
      } else {
        int shift = (int)rmax;
        int top = 63 - shift;
        if (((lmin >> top) != 0 && (lmin >> top) != -1) ||
            ((lmax >> top) != 0 && (lmax >> top) != -1))
          ok = false;
        else {
          lo = (int64_t)((uint64_t)lmin << (unsigned)shift);
          hi = (int64_t)((uint64_t)lmax << (unsigned)shift);
        }
      }
    } else if (strcmp(opbuf, ">>") == 0) {
      if (rmin != rmax || rmax < 0 || rmax >= 64)
        ok = false;
      else {
        lo = lmin >> rmax;
        hi = lmax >> rmax;
      }
    } else {
      ok = false;
    }
    if (!ok || lo > hi)
      return false;
    *out_lo = lo;
    *out_hi = hi;
    return true;
  }
  return false;
}

/*
 * Trinary truth decision over a canonical proposition: 1 true, -1 false,
 * 0 unknown. Mirrors ny_proof_range_compare over the string form.
 */
static int ny_proof_canon_decide(const char *s, ny_proof_name_range_fn env,
                                 void *ctx, int depth) {
  if (!s || depth > NY_PROOF_CANON_MAX_DEPTH) {
    ny_proof_debug(3, "canon_decide(depth=%d): '%s' -> 0 (limit/unset)",
                   depth, s ? s : "?");
    return 0;
  }
  if (strncmp(s, "bool:true", 9) == 0) {
    ny_proof_debug(3, "canon_decide(depth=%d): '%s' -> 1 (bool:true)", depth, s);
    return 1;
  }
  if (strncmp(s, "bool:false", 10) == 0) {
    ny_proof_debug(3, "canon_decide(depth=%d): '%s' -> -1 (bool:false)", depth, s);
    return -1;
  }
  if (strncmp(s, "int:", 4) == 0) {
    int d = strtoll(s + 4, NULL, 10) != 0 ? 1 : -1;
    ny_proof_debug(3, "canon_decide(depth=%d): '%s' -> %d (int literal)", depth,
                   s, d);
    return d;
  }
  if (strncmp(s, "name:", 5) == 0) {
    char name[256];
    const char *q = s + 5;
    size_t n = 0;
    while (*q && *q != ',' && *q != ')' && n + 1 < sizeof(name))
      name[n++] = *q++;
    name[n] = '\0';
    int64_t lo = 0, hi = 0;
    if (!env || !env(ctx, name, &lo, &hi)) {
      ny_proof_debug(3, "canon_decide(depth=%d): name '%s' -> 0 (no range)",
                     depth, name);
      return 0;
    }
    int d;
    if (lo > 0)
      d = 1;
    else if (hi < 0)
      d = -1;
    else if (lo == 0 && hi == 0)
      d = -1;
    else
      d = 0;
    ny_proof_debug(3,
                   "canon_decide(depth=%d): name '%s' range [%lld, %lld] -> %d",
                   depth, name, (long long)lo, (long long)hi, d);
    return d;
  }
  if (strncmp(s, "unary:!(", 8) == 0) {
    size_t slen = strlen(s);
    if (slen <= 9 || s[slen - 1] != ')')
      return 0;
    char *inner = ny_strndup(s + 8, slen - 9);
    int d = inner ? ny_proof_canon_decide(inner, env, ctx, depth + 1) : 0;
    int r = d == 0 ? 0 : -d;
    ny_proof_debug(3, "canon_decide(depth=%d): '!%s' -> %d", depth,
                   inner ? inner : "?", r);
    free(inner);
    return r;
  }
  if (strncmp(s, "binary:", 7) == 0) {
    char opbuf[8];
    char *l = NULL, *r = NULL;
    if (!ny_proof_canon_split_binary(s, opbuf, sizeof(opbuf), &l, &r)) {
      free(l);
      free(r);
      return 0;
    }
    int res = 0;
    if (strcmp(opbuf, "&&") == 0) {
      int dl = ny_proof_canon_decide(l, env, ctx, depth + 1);
      int dr = ny_proof_canon_decide(r, env, ctx, depth + 1);
      if (dl == -1 || dr == -1)
        res = -1;
      else if (dl == 1 && dr == 1)
        res = 1;
    } else if (strcmp(opbuf, "||") == 0) {
      int dl = ny_proof_canon_decide(l, env, ctx, depth + 1);
      int dr = ny_proof_canon_decide(r, env, ctx, depth + 1);
      if (dl == 1 || dr == 1)
        res = 1;
      else if (dl == -1 && dr == -1)
        res = -1;
    } else {
      int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
      bool lok = ny_proof_canon_range(l, env, ctx, &lmin, &lmax, depth + 1);
      bool rok = ny_proof_canon_range(r, env, ctx, &rmin, &rmax, depth + 1);
      if (lok && rok) {
        if (strcmp(opbuf, "==") == 0) {
          if (lmin == lmax && rmin == rmax && lmin == rmin)
            res = 1;
          else if (lmax < rmin || rmax < lmin)
            res = -1;
        } else if (strcmp(opbuf, "!=") == 0) {
          if (lmax < rmin || rmax < lmin)
            res = 1;
          else if (lmin == lmax && rmin == rmax && lmin == rmin)
            res = -1;
        } else if (strcmp(opbuf, "<") == 0) {
          if (lmax < rmin)
            res = 1;
          else if (lmin >= rmax)
            res = -1;
        } else if (strcmp(opbuf, "<=") == 0) {
          if (lmax <= rmin)
            res = 1;
          else if (lmin > rmax)
            res = -1;
        } else if (strcmp(opbuf, ">") == 0) {
          if (lmin > rmax)
            res = 1;
          else if (lmax <= rmin)
            res = -1;
        } else if (strcmp(opbuf, ">=") == 0) {
          if (lmin >= rmax)
            res = 1;
          else if (lmax < rmin)
            res = -1;
        }
      }
      ny_proof_debug(3,
                     "canon_decide(depth=%d): '%s' ranges "
                     "[%lld, %lld] %s [%lld, %lld] -> %d",
                     depth, opbuf, (long long)lmin, (long long)lmax, opbuf,
                     (long long)rmin, (long long)rmax, res);
    }
    free(l);
    free(r);
    ny_proof_debug(3, "canon_decide(depth=%d): binary '%s' -> %d", depth, opbuf,
                   res);
    return res;
  }
  ny_proof_debug(3, "canon_decide(depth=%d): '%s' -> 0 (unrecognized)", depth, s);
  return 0;
}


/*
 * 5.1: proof<P> parameter satisfaction at call sites
 */

 typedef struct ny_proof_call_env ny_proof_call_env_t;
 struct ny_proof_call_env {
   codegen_t *cg;
   scope *scopes;
   size_t depth;
   expr_t **param_args;  /* callee param index -> caller argument expr */
   const char **param_names;
   int nparams;
   ny_proof_call_env_t *parent;
 };
 static bool ny_proof_call_env_range(void *ctx, const char *name,
                                     int64_t *out_lo, int64_t *out_hi);

static bool ny_proof_call_env_expr_range(ny_proof_call_env_t *e, expr_t *arg,
                                         int64_t *out_lo, int64_t *out_hi) {
   if (!e || !arg)
     return false;
   if (arg->kind == NY_E_LITERAL &&
       arg->as.literal.kind == NY_LIT_INT) {
     *out_lo = arg->as.literal.as.i;
     *out_hi = arg->as.literal.as.i;
     return true;
   }
   if (arg->kind == NY_E_UNARY && arg->as.unary.right &&
       arg->as.unary.op &&
       (strcmp(arg->as.unary.op, "+") == 0 ||
        strcmp(arg->as.unary.op, "-") == 0)) {
     int64_t lo = 0, hi = 0;
     if (!ny_proof_call_env_expr_range(e, arg->as.unary.right, &lo, &hi))
       return false;
     if (strcmp(arg->as.unary.op, "-") == 0) {
       if (lo == INT64_MIN || hi == INT64_MIN)
         return false;
       *out_lo = -hi;
       *out_hi = -lo;
     } else {
       *out_lo = lo;
       *out_hi = hi;
     }
     return true;
   }
   if (arg->kind == NY_E_IDENT && arg->as.ident.name) {
     for (int i = 0; i < e->nparams; ++i) {
       if (e->param_names[i] &&
           strcmp(e->param_names[i], arg->as.ident.name) == 0) {
         expr_t *mapped = e->param_args[i];
         if (!mapped)
           return false;
         ny_proof_debug(3, "call_env_expr: '%s' mapped kind=%d parent=%d",
                        arg->as.ident.name, mapped->kind, e->parent ? 1 : 0);
         if (mapped == arg)
           return e->parent &&
                  ny_proof_call_env_expr_range(e->parent, arg, out_lo,
                                               out_hi);
         if (e->parent && mapped->kind == NY_E_IDENT)
           return ny_proof_call_env_expr_range(e->parent, mapped, out_lo,
                                               out_hi);
         bool direct;
         if (mapped->kind == NY_E_IDENT)
           direct = ny_gencall_expr_int_range(
               e->cg, e->scopes, e->depth, mapped, out_lo, out_hi);
         else
           direct = ny_proof_call_env_expr_range(e, mapped, out_lo, out_hi);
         ny_proof_debug(3, "call_env_expr: direct mapped range %s [%lld, %lld]",
                        direct ? "known" : "unknown", (long long)*out_lo,
                        (long long)*out_hi);
         return direct;
       }
     }
   }
   char *full = ny_proof_type_from_expr(arg);
   if (full) {
     const char *inner = full;
     if (strncmp(inner, "proof<", 6) == 0)
       inner += 6;
     size_t len = strlen(inner);
     char *canon = len > 0 && inner[len - 1] == '>'
                       ? ny_strndup(inner, len - 1)
                       : ny_strdup(inner);
     free(full);
     if (canon) {
       bool ok = ny_proof_canon_range(canon, ny_proof_call_env_range, e,
                                      out_lo, out_hi, 0);
       free(canon);
       if (ok)
         return true;
     }
   }
   return ny_gencall_expr_int_range(e->cg, e->scopes, e->depth, arg, out_lo,
                                    out_hi);
 }

 static bool ny_proof_call_env_range(void *ctx, const char *name,
                                     int64_t *out_lo, int64_t *out_hi) {
   ny_proof_call_env_t *e = (ny_proof_call_env_t *)ctx;
   if (!e || !name)
     return false;
   for (int i = 0; i < e->nparams; ++i) {
     if (e->param_names[i] && strcmp(e->param_names[i], name) == 0) {
       bool ok = ny_proof_call_env_expr_range(e, e->param_args[i], out_lo,
                                              out_hi);
       ny_proof_debug(
           3, "call_env_range: param '%s' -> arg range %s [%lld, %lld]", name,
           ok ? "known" : "unknown", (long long)*out_lo, (long long)*out_hi);
       return ok;
     }
   }
    if (e->parent)
      return ny_proof_call_env_range(e->parent, name, out_lo, out_hi);
    size_t name_len = strlen(name);
    binding *b = lookup_binding_hash(e->cg, e->scopes, e->depth, name, name_len,
                                     ny_hash_name(name, name_len));
    if (b && b->has_int_range) {
      *out_lo = b->int_min_raw;
      *out_hi = b->int_max_raw;
      ny_proof_debug(3, "call_env_range: name '%s' binding range [%lld, %lld]",
                     name, (long long)*out_lo, (long long)*out_hi);
      return true;
    }
    ny_proof_debug(3, "call_env_range: no param or binding named '%s'", name);
    return false;
  }

/*
 * Verify every proof<P> parameter of a call against the caller-side argument
 * ranges. A parameter is satisfied only when its canonical proposition is
 * proved by the mapped ranges (decide == 1). Disproved and unknown
 * propositions are both rejected: absence of a counterexample is not proof.
 */
static bool ny_proof_call_params_ok(codegen_t *cg, scope *scopes, size_t depth,
                                    fun_sig *sig, stmt_t *callee_stmt,
                                    expr_call_t *c, expr_memcall_t *mc,
                                    token_t tok) {
  if (!sig)
    return true;
  int nparams = sig->arity > 0 ? sig->arity : (int)sig->param_types.len;
  if (nparams <= 0)
    return true;
  const char **names = NULL;
  expr_t **args = NULL;
  bool mapped = false;
  if (callee_stmt &&
      (callee_stmt->kind == NY_S_FUNC || callee_stmt->kind == NY_S_EXTERN)) {
    ny_param_list *pl = callee_stmt->kind == NY_S_FUNC
                            ? &callee_stmt->as.fn.params
                            : &callee_stmt->as.ext.params;
    names = calloc((size_t)nparams, sizeof(const char *));
    args = calloc((size_t)nparams, sizeof(expr_t *));
    if (!names || !args) {
      free(names);
      free(args);
      fprintf(stderr, "OOM in proof call-params check\n");
      exit(1);
    }
    {
      mapped = true;
      for (int i = 0; i < nparams; ++i) {
        if (i < (int)pl->len)
          names[i] = pl->data[i].name;
        if (mc) {
          if (i == 0)
            args[i] = mc->target;
          else if (i - 1 < (int)mc->args.len)
            args[i] = mc->args.data[i - 1].val;
        } else if (c && i < (int)c->args.len) {
          args[i] = c->args.data[i].val;
        }
        /*
         * named arguments override positional ones
         */
        ny_call_arg_list *al = c ? &c->args : (mc ? &mc->args : NULL);
        if (al) {
          size_t off = mc ? 1 : 0;
          for (size_t k = 0; k < al->len; ++k) {
            if (al->data[k].name && names[i] &&
                strcmp(al->data[k].name, names[i]) == 0 &&
                (int)k + (int)off == i)
              args[i] = al->data[k].val;
          }
        }
      }
    }
  }
  bool all_ok = true;
  for (int i = 0; i < nparams; ++i) {
    if (i >= (int)sig->param_types.len)
      continue;
    const char *pty = sig->param_types.data[i];
    if (!pty || strncmp(pty, "proof<", 6) != 0)
      continue;
    const char *inner = pty + 6;
    size_t ilen = strlen(inner);
    if (ilen == 0 || inner[ilen - 1] != '>')
      continue;
    char *canon = ny_strndup(inner, ilen - 1);
    if (!canon)
      continue;
    ny_proof_call_env_t env = {cg, scopes, depth, args, names, nparams, NULL};
    int d = ny_proof_canon_decide(canon,
                                  mapped ? ny_proof_call_env_range : NULL,
                                  &env, 0);
    ny_proof_debug_at(1, tok,
                      "proof<P> param %d: '%s' decide=%d (mapped=%d)", i, canon,
                      d, mapped ? 1 : 0);
    if (d != 1) {
      ny_diag_error(tok,
                    "proof parameter '%s' is not satisfied by the call "
                    "arguments: the mapped value ranges do not prove it",
                    canon);
      ny_diag_hint("bind the referenced values with def (or constrain them "
                   "with assert_compile_range) before the call");
      cg->had_error = 1;
      all_ok = false;
    }
    free(canon);
  }
  free(names);
  free(args);
  return all_ok;
}

/*
 * 5.3: pure-function output-range cache
 */

typedef struct {
  const char **names;
  int64_t *lo;
  int64_t *hi;
  int n;
} ny_proof_pure_env_t;

static bool ny_proof_pure_env_range(void *ctx, const char *name,
                                    int64_t *out_lo, int64_t *out_hi) {
  ny_proof_pure_env_t *e = (ny_proof_pure_env_t *)ctx;
  for (int i = 0; i < e->n; ++i) {
    if (e->names[i] && strcmp(e->names[i], name) == 0) {
      *out_lo = e->lo[i];
      *out_hi = e->hi[i];
      return true;
    }
  }
  return false;
}

enum { NY_PROOF_PURE_MAX_ARGS = 16, NY_PROOF_PURE_CACHE = 64 };

static _Thread_local int ny_proof_pure_recursion;

/*
 * Output range of a call to a @pure function whose arguments all have known
 * ranges. Computed by normalizing the body's return expression and evaluating
 * the canonical form with parameter names mapped to argument ranges. Results
 * are cached by (function, argument ranges).
 */
static bool ny_gencall_pure_call_range(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e,
                                       int64_t *out_min, int64_t *out_max) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name)
    return false;
  if (++ny_proof_pure_recursion > 8) {
    --ny_proof_pure_recursion;
    return false;
  }
  const char *tail = ny_tail_name(e->as.call.callee->as.ident.name);
  size_t argc = e->as.call.args.len;
  fun_sig *sig = ny_gencall_lookup_source_file_fun(cg, tail, e->tok,
                                                   (int)argc);
  if (!sig || !sig->is_pure || !sig->stmt_t ||
      sig->stmt_t->kind != NY_S_FUNC || argc == 0 ||
      argc > NY_PROOF_PURE_MAX_ARGS) {
    --ny_proof_pure_recursion;
    return false;
  }
  ny_param_list *pl = &sig->stmt_t->as.fn.params;
  if (argc != pl->len) {
    --ny_proof_pure_recursion;
    return false;
  }
  const char *names[NY_PROOF_PURE_MAX_ARGS];
  int64_t alo[NY_PROOF_PURE_MAX_ARGS], ahi[NY_PROOF_PURE_MAX_ARGS];
  for (size_t i = 0; i < argc; ++i) {
    call_arg_t *arg = &e->as.call.args.data[i];
    if (arg->name)
      return (--ny_proof_pure_recursion, false); /* named args: skip */
    names[i] = pl->data[i].name;
    if (!ny_gencall_expr_int_range(cg, scopes, depth, arg->val, &alo[i],
                                   &ahi[i]))
      return (--ny_proof_pure_recursion, false);
  }
  uint64_t key = ny_hash64_cstr(tail);
  for (size_t i = 0; i < argc; ++i) {
    key = key * UINT64_C(1099511628211) ^ (uint64_t)alo[i];
    key = key * UINT64_C(1099511628211) ^ (uint64_t)ahi[i];
  }
  typedef struct {
    uint64_t key;
    int64_t lo, hi;
  } entry_t;
  static entry_t cache[NY_PROOF_PURE_CACHE];
  static int cache_len;
  for (int i = 0; i < cache_len; ++i) {
    if (cache[i].key == key) {
      *out_min = cache[i].lo;
      *out_max = cache[i].hi;
      --ny_proof_pure_recursion;
      return true;
    }
  }
  stmt_t *body = sig->stmt_t->as.fn.body;
  if (!body || body->kind != NY_S_BLOCK || body->as.block.body.len == 0)
    return (--ny_proof_pure_recursion, false);
  stmt_t *last = body->as.block.body.data[body->as.block.body.len - 1];
  expr_t *ret = NULL;
  if (last->kind == NY_S_RETURN && last->as.ret.value)
    ret = last->as.ret.value;
  else if (last->kind == NY_S_EXPR && last->as.expr.expr)
    ret = last->as.expr.expr;
  if (!ret)
    return (--ny_proof_pure_recursion, false);
  char *full = ny_proof_type_from_expr(ret);
  if (!full)
    return (--ny_proof_pure_recursion, false);
  const char *inner = full;
  if (strncmp(inner, "proof<", 6) == 0)
    inner += 6;
  size_t ilen = strlen(inner);
  char *canon =
      (ilen > 0 && inner[ilen - 1] == '>')
          ? ny_strndup(inner, ilen - 1)
          : ny_strdup(inner);
  free(full);
  if (!canon)
    return (--ny_proof_pure_recursion, false);
  ny_proof_pure_env_t env = {names, alo, ahi, (int)argc};
  int64_t lo = 0, hi = 0;
  bool ok = ny_proof_canon_range(canon, ny_proof_pure_env_range, &env, &lo,
                                 &hi, 0);
  free(canon);
  if (!ok)
    return (--ny_proof_pure_recursion, false);
  if (cache_len < NY_PROOF_PURE_CACHE) {
    cache[cache_len].key = key;
    cache[cache_len].lo = lo;
    cache[cache_len].hi = hi;
    ++cache_len;
  } else {
    cache[0].key = key;
    cache[0].lo = lo;
    cache[0].hi = hi;
  }
  *out_min = lo;
  *out_max = hi;
  --ny_proof_pure_recursion;
  return true;
}

/*
 * 5.2: @proves post-condition checking
 */

typedef struct {
  codegen_t *cg;
  scope *scopes;
  size_t depth;
  expr_t *result_expr;
} ny_proof_proves_env_t;

static bool ny_proof_proves_env_range(void *ctx, const char *name,
                                      int64_t *out_lo, int64_t *out_hi) {
  ny_proof_proves_env_t *e = (ny_proof_proves_env_t *)ctx;
  if (strcmp(name, "result") == 0)
    return ny_gencall_expr_int_range(e->cg, e->scopes, e->depth,
                                     e->result_expr, out_lo, out_hi);
  size_t name_len = strlen(name);
  binding *b = lookup_binding_hash(e->cg, e->scopes, e->depth, name, name_len,
                                   ny_hash_name(name, name_len));
  if (b && b->has_int_range) {
    *out_lo = b->int_min_raw;
    *out_hi = b->int_max_raw;
    return true;
  }
  return false;
}

static void ny_proof_collect_returns(stmt_t *s, expr_t **out, size_t cap,
                                     size_t *n) {
  if (!s || !out || *n >= cap)
    return;
  switch (s->kind) {
  case NY_S_RETURN:
    if (s->as.ret.value)
      out[(*n)++] = s->as.ret.value;
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len && *n < cap; ++i)
      ny_proof_collect_returns(s->as.block.body.data[i], out, cap, n);
    return;
  case NY_S_IF:
    ny_proof_collect_returns(s->as.iff.conseq, out, cap, n);
    ny_proof_collect_returns(s->as.iff.alt, out, cap, n);
    ny_proof_collect_returns(s->as.iff.init, out, cap, n);
    return;
  case NY_S_WHILE:
    ny_proof_collect_returns(s->as.whl.body, out, cap, n);
    return;
  case NY_S_FOR:
    ny_proof_collect_returns(s->as.fr.body, out, cap, n);
    return;
  case NY_S_TRY:
    ny_proof_collect_returns(s->as.tr.body, out, cap, n);
    return;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len && *n < cap; ++i)
      ny_proof_collect_returns(s->as.match.arms.data[i].conseq, out, cap, n);
    ny_proof_collect_returns(s->as.match.default_conseq, out, cap, n);
    return;
  default:
    return;
  }
}

/*
 * Verify a function's @proves(cond) post-condition against each of its
 * return-value expressions. `result` in the condition denotes the returned
 * value; other names resolve through normal scope bindings. A post-condition
 * is accepted only when every returned value range proves it.
 */
void ny_proof_check_fn_proves(codegen_t *cg, scope *scopes, size_t depth,
                              stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC ||
      !fn_stmt->as.fn.attr_proves || !fn_stmt->as.fn.body)
    return;
  expr_t *cond = fn_stmt->as.fn.attr_proves;
  char *full = ny_proof_type_from_expr(cond);
  if (!full)
    return;
  const char *inner = full;
  if (strncmp(inner, "proof<", 6) == 0)
    inner += 6;
  size_t ilen = strlen(inner);
  char *canon = (ilen > 0 && inner[ilen - 1] == '>')
                    ? ny_strndup(inner, ilen - 1)
                    : ny_strdup(inner);
  free(full);
  if (!canon)
    return;
  expr_t *rets[16];
  size_t nret = 0;
  ny_proof_collect_returns(fn_stmt->as.fn.body, rets, 16, &nret);
  if (nret == 0) {
    stmt_t *body = fn_stmt->as.fn.body;
    if (body->kind == NY_S_BLOCK && body->as.block.body.len > 0) {
      stmt_t *last = body->as.block.body.data[body->as.block.body.len - 1];
      if (last->kind == NY_S_EXPR && last->as.expr.expr)
        rets[nret++] = last->as.expr.expr;
    }
  }
  for (size_t i = 0; i < nret; ++i) {
    ny_proof_proves_env_t env = {cg, scopes, depth, rets[i]};
    int d = ny_proof_canon_decide(canon, ny_proof_proves_env_range, &env, 0);
    if (d != 1) {
      ny_diag_error(cond->tok,
                    "@proves condition '%s' is not proved for a returned "
                    "value range", canon);
      ny_diag_hint("give every return expression a range that proves the "
                   "post-condition");
      cg->had_error = 1;
    }
  }
  free(canon);
}

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
    if (name && !shadowed && ny_name_tail_is(name, "len")) {
      int64_t len = 0;
      if (ny_gencall_list_len_min(cg, scopes, depth,
                                  e->as.call.args.data[0].val, &len)) {
        if (out_min)
          *out_min = len;
        if (out_max)
          *out_max = len;
        return true;
      }
    }
  }
  /*
   * `.len` member access (xs.len) range-extracts the same way len(xs) does.
   */
  if (e && e->kind == NY_E_MEMBER && e->as.member.name &&
      strcmp(e->as.member.name, "len") == 0) {
    int64_t len = 0;
    if (ny_gencall_list_len_min(cg, scopes, depth, e->as.member.target,
                                &len)) {
      if (out_min)
        *out_min = len;
      if (out_max)
        *out_max = len;
      return true;
    }
  }
  /*
   * Pure-function output-range cache (any arity).
   */
  if (e && e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    int64_t plo = 0, phi = 0;
    if (ny_gencall_pure_call_range(cg, scopes, depth, e, &plo, &phi)) {
      if (out_min)
        *out_min = plo;
      if (out_max)
        *out_max = phi;
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
    } else if (strcmp(op, "/") == 0) {
      if (!ny_proof_nonnegative_div_range(lmin, lmax, rmin, rmax,
                                          &lo, &hi))
        return false;
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
    } else if (strcmp(op, "|") == 0) {
      /*
       * Bitwise OR: worst case is a corner of the operand boxes.
       */
      int64_t c[4];
      c[0] = lmin | rmin;
      c[1] = lmin | rmax;
      c[2] = lmax | rmin;
      c[3] = lmax | rmax;
      lo = c[0];
      hi = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < lo)
          lo = c[i];
        if (c[i] > hi)
          hi = c[i];
      }
    } else if (strcmp(op, "^^") == 0) {
      /*
       * Bitwise XOR: same corner approach.
       */
      int64_t c[4];
      c[0] = lmin ^ rmin;
      c[1] = lmin ^ rmax;
      c[2] = lmax ^ rmin;
      c[3] = lmax ^ rmax;
      lo = c[0];
      hi = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < lo)
          lo = c[i];
        if (c[i] > hi)
          hi = c[i];
      }
    } else if (strcmp(op, "<<") == 0) {
      /*
       * Left shift: exact non-negative shift; reject on overflow.
       */
      if (rmin != rmax || rmax < 0 || rmax >= 64)
        return false;
      if (rmax == 0) {
        lo = lmin;
        hi = lmax;
      } else {
        int shift = (int)rmax;
        int top = 63 - shift;
        /*
         * l << shift overflows unless the top (64-shift) bits are uniform.
         */
        if ((lmin >> top) != 0 && (lmin >> top) != -1)
          return false;
        if ((lmax >> top) != 0 && (lmax >> top) != -1)
          return false;
        /*
         * Compute with unsigned shifts so -1 << 63 etc. stay defined.
         */
        lo = (int64_t)((uint64_t)lmin << (unsigned)shift);
        hi = (int64_t)((uint64_t)lmax << (unsigned)shift);
      }
    } else if (strcmp(op, ">>") == 0) {
      /*
       * Arithmetic right shift: exact non-negative shift.
       */
      if (rmin != rmax || rmax < 0 || rmax >= 64)
        return false;
      lo = lmin >> rmax;
      hi = lmax >> rmax;
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
  /*
   * Conditional (ternary) expressions: merge the branch ranges.
   */
  if (e && e->kind == NY_E_TERNARY) {
    int64_t tmin = 0, tmax = 0, fmin = 0, fmax = 0;
    bool t_ok = ny_gencall_expr_int_range(
        cg, scopes, depth, e->as.ternary.true_expr, &tmin, &tmax);
    bool f_ok = ny_gencall_expr_int_range(
        cg, scopes, depth, e->as.ternary.false_expr, &fmin, &fmax);
    if (t_ok && f_ok) {
      if (out_min)
        *out_min = tmin < fmin ? tmin : fmin;
      if (out_max)
        *out_max = tmax > fmax ? tmax : fmax;
      return true;
    }
    return false;
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
                  strcmp(name, "assert_compile") == 0 ||
                  strcmp(name, "prove") == 0 ||
                  strcmp(name, "proof_matches") == 0);
}

static uint64_t ny_proof_proposition_digest(expr_t *condition) {
  char *type_name = ny_proof_type_from_expr(condition);
  uint64_t digest = ny_hash64_cstr(type_name ? type_name : "proof<invalid>");
  ny_proof_debug(2, "digest: '%s' -> 0x%llx",
                 type_name ? type_name : "proof<invalid>",
                 (unsigned long long)digest);
  free(type_name);
  /*
   * Zero remains unavailable as a proof certificate.
   */
  return digest ? digest : UINT64_C(0x9e3779b97f4a7c15);
}
static bool ny_lemma_name_matches(const char *mod_prefix, const char *leaf,
                                  const char *name) {
  if (!leaf || !name)
    return false;
  if (strcmp(leaf, name) == 0)
    return true;
  if (!mod_prefix || !*mod_prefix)
    return false;
  size_t plen = strlen(mod_prefix);
  return strncmp(name, mod_prefix, plen) == 0 && name[plen] == '.' &&
         strcmp(name + plen + 1, leaf) == 0;
}

static stmt_t *ny_find_lemma_in_stmt(stmt_t *s, const char *name,
                                     const char *mod_prefix, unsigned depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_LEMMA && s->as.lemma.name &&
      ny_lemma_name_matches(mod_prefix, s->as.lemma.name, name)) {
    ny_proof_debug(3, "find_lemma_in_stmt: '%s' matches '%s.%s'", name,
                   mod_prefix ? mod_prefix : "", s->as.lemma.name);
    return s;
  }
  if (s->kind == NY_S_MODULE) {
    const char *prefix = mod_prefix;
    if (s->as.module.name && *s->as.module.name)
      prefix = s->as.module.name;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      stmt_t *found =
          ny_find_lemma_in_stmt(s->as.module.body.data[i], name, prefix,
                                depth + 1);
      if (found)
        return found;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      stmt_t *found =
          ny_find_lemma_in_stmt(s->as.block.body.data[i], name, mod_prefix,
                                depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static stmt_t *ny_find_lemma(codegen_t *cg, const char *name) {
  if (!cg || !cg->prog || !name)
    return NULL;
  for (size_t i = 0; i < cg->registry.lemmas.len; ++i) {
    lemma_def_t *def = cg->registry.lemmas.data[i];
    if (def && def->stmt &&
        ny_lemma_name_matches(def->module_name, def->name, name)) {
      ny_proof_debug(2, "find_lemma('%s'): registry hit '%s.%s'", name,
                     def->module_name ? def->module_name : "", def->name);
      return def->stmt;
    }
  }
  for (size_t i = 0; i < cg->prog->body.len; ++i) {
    stmt_t *found = ny_find_lemma_in_stmt(cg->prog->body.data[i], name, NULL, 0);
    if (found) {
      ny_proof_debug(2, "find_lemma('%s'): program tree hit", name);
      return found;
    }
  }
  for (size_t i = 0; i < cg->extra_progs.len; ++i) {
    program_t *prog = cg->extra_progs.data[i];
    if (!prog)
      continue;
    for (size_t j = 0; j < prog->body.len; ++j) {
      stmt_t *found =
          ny_find_lemma_in_stmt(prog->body.data[j], name, NULL, 0);
      if (found) {
        ny_proof_debug(2, "find_lemma('%s'): extra program '%zu' hit", name, i);
        return found;
      }
    }
  }
  ny_proof_debug(2, "find_lemma('%s'): not found", name);
  return NULL;
}

static stmt_t *ny_resolve_lemma_callee(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *callee) {
  if (!cg || !callee)
    return NULL;
  if (callee->kind == NY_E_IDENT && callee->as.ident.name) {
    stmt_t *lemma = ny_find_lemma(cg, callee->as.ident.name);
    ny_proof_debug_at(1, callee->tok, "lemma callee ident '%s' -> %s",
                      callee->as.ident.name,
                      lemma && lemma->as.lemma.name ? lemma->as.lemma.name
                                                    : "none");
    return lemma;
  }
  if (callee->kind != NY_E_MEMBER || !callee->as.member.name)
    return NULL;
  char mod_path[1024];
  if (!ny_resolve_module_expr_path(cg, scopes, depth, callee->as.member.target,
                                   mod_path, sizeof(mod_path))) {
    ny_proof_debug_at(1, callee->tok,
                      "lemma callee member '%s': module path unresolved",
                      callee->as.member.name);
    return NULL;
  }
  char resolved[1280];
  if (!ny_resolve_module_function_path(cg, mod_path, callee->as.member.name,
                                       resolved, sizeof(resolved))) {
    ny_proof_debug_at(1, callee->tok,
                      "lemma callee member '%s' in '%s': full path unresolved",
                      callee->as.member.name, mod_path);
    return NULL;
  }
  stmt_t *lemma = ny_find_lemma(cg, resolved);
  ny_proof_debug_at(1, callee->tok,
                    "lemma callee member resolved '%s' -> %s", resolved,
                    lemma && lemma->as.lemma.name ? lemma->as.lemma.name
                                                  : "none");
  return lemma;
}
 /*
  * Evaluate a lemma proposition with call-site arguments substituted by
  * range-backed environments. Calls are handled structurally so a composed
  * lemma cannot be accepted merely because its definition was previously
  * marked proved.
  */
 static int ny_proof_lemma_expr_decide(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *expr,
                                       ny_proof_call_env_t *env,
                                       int recursion) {
   if (!expr || recursion > NY_PROOF_CANON_MAX_DEPTH)
     return 0;
   if (expr->kind == NY_E_CALL || expr->kind == NY_E_MEMCALL) {
     expr_t callee_storage;
     memset(&callee_storage, 0, sizeof(callee_storage));
     expr_t *callee = NULL;
     ny_call_arg_list *args = NULL;
     if (expr->kind == NY_E_CALL) {
       callee = expr->as.call.callee;
       args = &expr->as.call.args;
     } else {
       callee_storage.kind = NY_E_MEMBER;
       callee_storage.tok = expr->tok;
       callee_storage.as.member.target = expr->as.memcall.target;
       callee_storage.as.member.name = expr->as.memcall.name;
       callee = &callee_storage;
       args = &expr->as.memcall.args;
     }
     stmt_t *lemma =
         ny_resolve_lemma_callee(cg, scopes, depth, callee);
     if (!lemma || !args ||
         args->len != lemma->as.lemma.params.len ||
         args->len > 64)
       return 0;
     expr_t *mapped_args[64] = {0};
     const char *mapped_names[64] = {0};
     for (size_t i = 0; i < args->len; ++i) {
       mapped_args[i] = args->data[i].val;
       mapped_names[i] = lemma->as.lemma.params.data[i].name;
     }
     ny_proof_call_env_t nested = {
         cg, scopes, depth, mapped_args, mapped_names, (int)args->len, env};
     return ny_proof_lemma_expr_decide(
         cg, scopes, depth, lemma->as.lemma.proposition, &nested,
         recursion + 1);
   }
   if ((expr->kind == NY_E_LOGICAL && expr->as.logical.left &&
        expr->as.logical.right && expr->as.logical.op) ||
       (expr->kind == NY_E_BINARY && expr->as.binary.left &&
        expr->as.binary.right && expr->as.binary.op &&
        (strcmp(expr->as.binary.op, "&&") == 0 ||
         strcmp(expr->as.binary.op, "||") == 0))) {
     expr_t *left = expr->kind == NY_E_LOGICAL ? expr->as.logical.left
                                               : expr->as.binary.left;
     expr_t *right = expr->kind == NY_E_LOGICAL ? expr->as.logical.right
                                                : expr->as.binary.right;
     const char *op = expr->kind == NY_E_LOGICAL ? expr->as.logical.op
                                                 : expr->as.binary.op;
     int left_decision = ny_proof_lemma_expr_decide(
         cg, scopes, depth, left, env, recursion + 1);
     int right_decision = ny_proof_lemma_expr_decide(
         cg, scopes, depth, right, env, recursion + 1);
     if (strcmp(op, "&&") == 0) {
       if (left_decision == -1 || right_decision == -1)
         return -1;
       return left_decision == 1 && right_decision == 1 ? 1 : 0;
     }
     if (left_decision == 1 || right_decision == 1)
       return 1;
     return left_decision == -1 && right_decision == -1 ? -1 : 0;
   }
   if (expr->kind == NY_E_UNARY && expr->as.unary.right &&
       expr->as.unary.op && strcmp(expr->as.unary.op, "!") == 0) {
     int result = ny_proof_lemma_expr_decide(
         cg, scopes, depth, expr->as.unary.right, env, recursion + 1);
     return result == 0 ? 0 : -result;
   }
   char *full = ny_proof_type_from_expr(expr);
   if (!full)
     return 0;
   const char *inner = full;
   if (strncmp(inner, "proof<", 6) == 0)
     inner += 6;
   size_t len = strlen(inner);
   char *canon = len > 0 && inner[len - 1] == '>'
                     ? ny_strndup(inner, len - 1)
                     : ny_strdup(inner);
   free(full);
   if (!canon)
     return 0;
   int result = ny_proof_canon_decide(
       canon, ny_proof_call_env_range, env, recursion);
   free(canon);
   return result;
 }


static LLVMValueRef ny_try_lemma_proof(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *cond,
                                       token_t tok) {
  if (!cg || !cond)
    return NULL;
  expr_t callee_storage;
  memset(&callee_storage, 0, sizeof(callee_storage));
  expr_t *callee = NULL;
  ny_call_arg_list *call_args = NULL;
  if (cond->kind == NY_E_CALL && cond->as.call.callee) {
    callee = cond->as.call.callee;
    call_args = &cond->as.call.args;
  } else if (cond->kind == NY_E_MEMCALL) {
    callee_storage.kind = NY_E_MEMBER;
    callee_storage.tok = cond->tok;
    callee_storage.as.member.target = cond->as.memcall.target;
    callee_storage.as.member.name = cond->as.memcall.name;
    callee = &callee_storage;
    call_args = &cond->as.memcall.args;
  }
  stmt_t *lemma = callee ? ny_resolve_lemma_callee(cg, scopes, depth, callee) : NULL;
  if (!lemma) {
    ny_proof_debug_at(1, cond->tok, "lemma proof: callee is not a lemma");
    return NULL;
  }
  const char *name = lemma->as.lemma.name;
  if (call_args->len != lemma->as.lemma.params.len) {
    ny_proof_debug_at(1, cond->tok,
                      "lemma proof: '%s' arity mismatch: got %zu, expects %zu",
                      name, call_args->len, lemma->as.lemma.params.len);
    ny_diag_error(tok, "lemma '%s' expects %zu argument(s), got %zu", name,
                  lemma->as.lemma.params.len, call_args->len);
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, false, "lemma_bad_arity");
  }
  const size_t n = lemma->as.lemma.params.len;
  if (n > 64) {
    ny_diag_error(tok, "lemma '%s' exceeds the 64-parameter proof bound", name);
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, false, "lemma_too_many_params");
  }
  bool proved = false;
  for (size_t i = 0; i < cg->registry.lemmas.len; ++i) {
    lemma_def_t *def = cg->registry.lemmas.data[i];
    if (def && def->stmt == lemma) {
      proved = def->proved;
      break;
    }
  }
  if (!proved) {
    ny_diag_error(tok, "lemma '%s' is not proved", name);
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, false, "lemma_unproved_definition");
  }
  const char *names[64] = {0};
  expr_t *args[64] = {0};
  for (size_t i = 0; i < n; ++i) {
    names[i] = lemma->as.lemma.params.data[i].name;
    args[i] = call_args->data[i].val;
  }
  ny_proof_call_env_t env = {cg, scopes, depth, args, names, (int)n, NULL};
  int decision = ny_proof_lemma_expr_decide(
      cg, scopes, depth, lemma->as.lemma.proposition, &env, 0);
  ny_proof_debug_at(1, cond->tok,
                    "lemma proof: '%s' with %zu arg(s) -> decision %d", name,
                    n, decision);
  if (decision != 1) {
    ny_diag_error(tok, "lemma '%s' could not prove the supplied arguments", name);
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, false, "lemma_unproved");
  }
  uint64_t digest = ny_proof_proposition_digest(cond);
  ny_proof_debug_at(1, cond->tok, "lemma proof: '%s' accepted, digest 0x%llx",
                    name, (unsigned long long)digest);
  return LLVMConstInt(cg->type_i64, digest, false);
}


/*
 * Range-backed comparison decision. Returns:
 *    1  proposition is trivially true from known integer ranges
 *   -1  proposition is trivially false from known integer ranges
 *    0  ranges do not decide the proposition
 * Sound by construction: ranges are compiler-computed and conservative.
 */
static int ny_proof_range_compare(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *cond) {
  if (!cond || cond->kind != NY_E_BINARY || !cond->as.binary.op)
    return 0;
  const char *op = cond->as.binary.op;
  if (strcmp(op, "==") != 0 && strcmp(op, "!=") != 0 &&
      strcmp(op, "<") != 0 && strcmp(op, "<=") != 0 &&
      strcmp(op, ">") != 0 && strcmp(op, ">=") != 0)
    return 0;
  int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
  if (!ny_gencall_expr_int_range(cg, scopes, depth, cond->as.binary.left,
                                 &lmin, &lmax) ||
      !ny_gencall_expr_int_range(cg, scopes, depth, cond->as.binary.right,
                                 &rmin, &rmax))
    return 0;
  int res = 0;
  if (strcmp(op, "==") == 0) {
    if (lmin == lmax && rmin == rmax && lmin == rmin)
      res = 1;
    else if (lmax < rmin || rmax < lmin)
      res = -1;
  }
  if (strcmp(op, "!=") == 0) {
    if (lmax < rmin || rmax < lmin)
      res = 1;
    else if (lmin == lmax && rmin == rmax && lmin == rmin)
      res = -1;
  }
  if (strcmp(op, "<") == 0) {
    if (lmax < rmin)
      res = 1;
    else if (lmin >= rmax)
      res = -1;
  }
  if (strcmp(op, "<=") == 0) {
    if (lmax <= rmin)
      res = 1;
    else if (lmin > rmax)
      res = -1;
  }
  if (strcmp(op, ">") == 0) {
    if (lmin > rmax)
      res = 1;
    else if (lmax <= rmin)
      res = -1;
  }
  /*
   * >=
   */
  if (res == 0) {
    if (lmin >= rmax)
      res = 1;
    else if (lmax < rmin)
      res = -1;
  }
  ny_proof_debug_at(2, cond->tok,
                    "range_compare: [%lld, %lld] %s [%lld, %lld] -> %d",
                    (long long)lmin, (long long)lmax, op, (long long)rmin,
                    (long long)rmax, res);
  return res;
}

/*
 * Decide whether a proposition is trivially true from known integer ranges
 * without requiring LLVM constant evaluation. Understands:
 *   - comparisons on range-tracked operands (==, !=, <, <=, >, >=)
 *   - A || B where either side is trivially true
 *   - !A where A is trivially false
 * This lets prove()/static_assert accept computed range checks directly,
 * without comptime{ return ... } wrappers.
 */
static bool ny_proof_is_trivial_truth(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *cond,
                                      const char **out_reason) {
  if (!cond) {
    if (out_reason)
      *out_reason = "missing condition";
    return false;
  }
  if (cond->kind == NY_E_LOGICAL && cond->as.binary.op &&
      strcmp(cond->as.binary.op, "||") == 0) {
    if (ny_proof_is_trivial_truth(cg, scopes, depth,
                                  cond->as.binary.left, NULL) ||
        ny_proof_is_trivial_truth(cg, scopes, depth,
                                  cond->as.binary.right, NULL))
      return true;
  }
  if (cond->kind == NY_E_UNARY && cond->as.unary.op &&
      strcmp(cond->as.unary.op, "!") == 0 && cond->as.unary.right) {
    if (ny_proof_range_compare(cg, scopes, depth, cond->as.unary.right) < 0)
      return true;
  }
  int r = ny_proof_range_compare(cg, scopes, depth, cond);
  if (out_reason) {
    if (r > 0)
      *out_reason = "trivial truth via known integer ranges";
    else if (r < 0)
      *out_reason = "trivial falsehood via known integer ranges";
    else
      *out_reason = "known integer ranges do not decide the proposition";
  }
  ny_proof_debug_at(1, cond->tok, "trivial_truth -> %d (%s)", r > 0,
                    out_reason ? *out_reason : "");
  return r > 0;
}
bool ny_proof_check_loop_invariant(codegen_t *cg, scope *scopes,
                                   size_t depth, expr_t *invariant) {
  if (!cg || !invariant) {
    if (cg)
      cg->had_error = 1;
    return false;
  }
  const char *reason = NULL;
  if (ny_proof_is_trivial_truth(cg, scopes, depth, invariant, &reason)) {
    ny_proof_debug_at(1, invariant->tok, "loop invariant proved by ranges");
    return true;
  }
  char cex[128] = {0};
  if (ny_proof_try_solver(cg, scopes, depth, invariant, cex, sizeof(cex))) {
    ny_proof_debug_at(1, invariant->tok, "loop invariant proved by solver");
    return true;
  }
  ny_proof_debug_at(1, invariant->tok, "loop invariant NOT proved");
  ny_diag_error(invariant->tok, "loop invariant must be provable at entry and each iteration");
  if (reason)
    ny_diag_hint("%s", reason);
  if (cex[0])
    ny_diag_hint("counterexample candidate: %s", cex);
  cg->had_error = 1;
  return false;
}

static LLVMValueRef ny_try_static_assert_builtin(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 const char *name,
                                                 bool shadowed,
                                                 expr_call_t *c) {
  if (!cg || !e || !name || shadowed || !ny_compile_assert_name_is(name))
    return NULL;
  if (strcmp(name, "proof_matches") == 0) {
    if (!c || c->args.len != 2) {
      ny_diag_error(e->tok,
                    "proof_matches expects a proof witness and proposition");
      cg->had_error = 1;
      return ny_gencall_const_bool(cg, false, "proof_matches_bad_arity");
    }
    LLVMValueRef witness = gen_expr(cg, scopes, depth, c->args.data[0].val);
    uint64_t expected =
        ny_proof_proposition_digest(c->args.data[1].val);
    ny_proof_debug_at(1, e->tok,
                      "proof_matches: witness expr, expected digest 0x%llx",
                      (unsigned long long)expected);
    LLVMValueRef matches = ny_eq(
        cg, witness, LLVMConstInt(cg->type_i64, expected, false),
        "proof_digest_matches");
    return ny_select(cg, matches,
                     LLVMConstInt(cg->type_i64, NY_IMM_TRUE, false),
                     LLVMConstInt(cg->type_i64, NY_IMM_FALSE, false),
                     "proof_matches");
  }
  if (!c || c->args.len < 1 || c->args.len > 2) {
    ny_diag_error(e->tok, "%s expects condition and optional message", name);
    ny_diag_hint(
        "use %s(comptime{ return cond }, \"message\") for computed checks",
        name);
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, true, "static_assert_bad_arity");
  }

  char msg_buf[512];
  bool want_proof = strcmp(name, "prove") == 0;
  const char *msg = want_proof ? "proof obligation failed"
                               : "static assertion failed";
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
  LLVMValueRef lemma_proof =
      ny_try_lemma_proof(cg, scopes, depth, cond, e->tok);
  if (lemma_proof) {
    ny_proof_debug_at(1, e->tok, "%s: accepted via lemma instantiation", name);
    return lemma_proof;
  }

  /*
   * Range-backed trivial proof: accept comparisons decided by known integer
   * ranges before falling through to LLVM constant evaluation. This makes
   * prove(n > 0) and static_assert(i < n) work on range-tracked values.
   */
  const char *trivial_reason = NULL;
  if (ny_proof_is_trivial_truth(cg, scopes, depth, cond, &trivial_reason)) {
    ny_proof_debug_at(1, e->tok, "%s: accepted via known integer ranges", name);
    return want_proof
               ? LLVMConstInt(cg->type_i64,
                              ny_proof_proposition_digest(cond), false)
               : ny_gencall_const_bool(cg, true, "static_assert_trivial");
  }

  /*
   * Solver-backed proof: a dependency-free Presburger Fourier–Motzkin subset,
   * plus Z3 when the compiler was built with NYTRIX_HAS_Z3. The solver is
   * consultative: it returns true only when the negation of the proposition
   * is proven unsatisfiable, so it can only accept a genuine proof; unknown
   * results fall through to constant evaluation below.
   */
  {
    char solver_cex[128] = {0};
    if (ny_proof_try_solver(cg, scopes, depth, cond, solver_cex,
                            sizeof(solver_cex))) {
      ny_proof_debug_at(1, e->tok, "%s: accepted via solver", name);
      return want_proof
                 ? LLVMConstInt(cg->type_i64,
                                ny_proof_proposition_digest(cond), false)
                 : ny_gencall_const_bool(cg, true, "static_assert_solver");
    }
  }

  LLVMValueRef v = gen_expr(cg, scopes, depth, cond);
  bool truthy = false;
  if (!ny_gencall_const_truthy(v, &truthy)) {
    int64_t cmin = 0, cmax = 0;
    bool has_crange =
        ny_gencall_expr_int_range(cg, scopes, depth, cond, &cmin, &cmax);
    ny_proof_debug_at(1, e->tok,
                      "%s: condition not constant (range %s[%lld, %lld]); "
                      "reporting dynamic failure",
                      name, has_crange ? "" : "unknown ",
                      (long long)cmin, (long long)cmax);
    ny_diag_error(cond ? cond->tok : e->tok,

                  "%s condition must be known at compile time", name);
    if (has_crange) {
      ny_diag_hint("the compiler knows this expression has integer range "
                   "[%lld, %lld] but cannot prove the proposition; wrap "
                   "computed checks in comptime{ return ... } or bind the "
                   "value with def so its range is exact",
                   (long long)cmin, (long long)cmax);
    } else {
      ny_diag_hint("wrap computed checks in comptime{ return ... } or pass a "
                   "constant expression");
    }
    cg->had_error = 1;
    return ny_gencall_const_bool(cg, true, "static_assert_dynamic");
  }
  if (!truthy) {
    ny_proof_debug_at(1, e->tok, "%s: condition constant-false", name);
    ny_diag_error(cond ? cond->tok : e->tok, "%s", msg);
    cg->had_error = 1;
  }
  ny_proof_debug_at(1, e->tok, "%s: constant eval -> %s", name,
                    truthy ? "true" : "false");
  return want_proof
             ? LLVMConstInt(cg->type_i64,
                            ny_proof_proposition_digest(cond), false)
                    : ny_gencall_const_bool(cg, true, "static_assert_ok");
}

bool ny_proof_check_lemma_definition(codegen_t *cg, stmt_t *lemma_stmt) {
  if (!cg || !lemma_stmt || lemma_stmt->kind != NY_S_LEMMA ||
      !lemma_stmt->as.lemma.proposition)
    return false;
  char cex[128] = {0};
  if (ny_proof_try_solver(cg, NULL, 0, lemma_stmt->as.lemma.proposition,
                          cex, sizeof(cex)))
    return true;
  ny_diag_error(lemma_stmt->tok, "lemma '%s' is not universally provable",
                lemma_stmt->as.lemma.name ? lemma_stmt->as.lemma.name
                                           : "<unnamed>");
  ny_diag_hint("lemma parameters are universally quantified; establish the "
               "proposition for every parameter value");
  if (cex[0])
    ny_diag_hint("counterexample candidate: %s", cex);
  cg->had_error = 1;
  return false;
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
