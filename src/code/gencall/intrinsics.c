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
                  "get expects a string, bytes, list, tuple, dict, range, or vector, got '%s'",
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
