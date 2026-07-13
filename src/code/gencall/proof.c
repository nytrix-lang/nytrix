/* Compile-time builtin analysis: constant scalar recognition, range proofs,
 * collection bounds, static assertions, and proof witness construction. */

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
  if (arg && arg->kind == NY_E_DEREF && arg->as.deref.target) {
    LLVMValueRef address = gen_expr(cg, scopes, depth, arg->as.deref.target);
    if (!address)
      return ny_c0(cg);
    if (LLVMGetTypeKind(LLVMTypeOf(address)) == LLVMPointerTypeKind)
      return ny_ptr2i64(cg, address, NY_LLVM_NAME(cg, "addr_of_deref"));
    if (LLVMGetTypeKind(LLVMTypeOf(address)) == LLVMIntegerTypeKind)
      return ny_cast_to_i64(cg, address, "addr_of_deref");
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
                  strcmp(name, "assert_compile") == 0 ||
                  strcmp(name, "prove") == 0 ||
                  strcmp(name, "proof_matches") == 0);
}

static uint64_t ny_proof_proposition_digest(expr_t *condition) {
  char *json = ny_expr_to_json(condition);
  uint64_t digest = ny_hash64_cstr(json ? json : "null");
  if (json)
    rt_free((int64_t)(uintptr_t)json);
  /* Zero remains the legacy/no-certificate proof representation. */
  return digest ? digest : UINT64_C(0x9e3779b97f4a7c15);
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
  return want_proof
             ? LLVMConstInt(cg->type_i64,
                            ny_proof_proposition_digest(cond), false)
                    : ny_gencall_const_bool(cg, true, "static_assert_ok");
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
