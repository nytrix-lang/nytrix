LLVMValueRef gen_expr_list_stack_alloc(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  size_t item_count = e->as.list_like.len;

  LLVMValueRef mem_ptr = LLVMBuildAlloca(cg->builder, LLVMArrayType(cg->type_i64, 3 + item_count), "list_stack");
  LLVMSetAlignment(mem_ptr, 16);

  LLVMValueRef p_idx[2] = { ny_c0(cg), ny_c1(cg) };
  LLVMValueRef p_addr = LLVMBuildGEP2(cg->builder, LLVMArrayType(cg->type_i64, 3 + item_count), mem_ptr, p_idx, 2, "p_header");
  LLVMValueRef p = LLVMBuildPtrToInt(cg->builder, p_addr, cg->type_i64, "p_val");

  LLVMValueRef tag_addr = LLVMBuildIntToPtr(cg->builder, ny_sub(cg, p, ny_ci(cg, 8), ""), ny_ptr_i64_ty(cg), "");
  uint64_t tag = (e->kind == NY_E_LIST) ? TAG_LIST : TAG_TUPLE;
  ny_store(cg, tag_addr, ny_ci(cg, tag));

  LLVMValueRef len_addr = LLVMBuildIntToPtr(cg->builder, p, ny_ptr_i64_ty(cg), "");
  ny_store(cg, len_addr, ny_ci(cg, (item_count << 1) | 1));

  LLVMValueRef cap_addr = LLVMBuildIntToPtr(cg->builder, ny_add(cg, p, ny_ci(cg, 8), ""), ny_ptr_i64_ty(cg), "");
  ny_store(cg, cap_addr, ny_ci(cg, (item_count << 1) | 1));

  for (size_t i = 0; i < item_count; i++) {
    LLVMValueRef item_val = gen_expr(cg, scopes, depth, e->as.list_like.data[i]);
    LLVMValueRef item_addr = LLVMBuildIntToPtr(cg->builder, ny_add(cg, p, ny_ci(cg, 16 + i * 8), ""), ny_ptr_i64_ty(cg), "");
    ny_store(cg, item_addr, item_val);
  }

  return p;
}

static LLVMValueRef gen_expr_list_like(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *e) {  fun_sig *ls = lookup_fun(cg, "__list_new", 0);
  fun_sig *st = lookup_fun(cg, "__store_item_fast", 0);
  fun_sig *set_len = lookup_fun(cg, "__list_set_len", 0);
  if (!ls || !st || !set_len)
    return expr_fail(cg, e->tok,
                     "list literal requires "
                     "__list_new/__store_item_fast/__list_set_len helpers");
  ny_dbg_loc(cg, e->tok);
  size_t item_count = e->as.list_like.len;
  LLVMValueRef vl = LLVMBuildCall2(
      cg->builder, ls->type, ls->value,
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64,
                                    (((uint64_t)item_count << 1) | 1u), false)},
      1, "");
  for (size_t i = 0; i < item_count; i++) {
    if (i > 0 && i % 64 == 0) {
      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef cur_fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef next_bb = ny_bb_fn(cur_fn, "lst_chunk");
      ny_br(cg, next_bb);
      ny_pos(cg, next_bb);
    }
    LLVMValueRef item = gen_expr(cg, scopes, depth, e->as.list_like.data[i]);
    ny_dbg_loc(cg, e->tok);
    (void)LLVMBuildCall2(
        cg->builder, st->type, st->value,
        (LLVMValueRef[]){
            vl, LLVMConstInt(cg->type_i64, (((uint64_t)i << 1) | 1u), false),
            item},
        3, "");
  }
  (void)LLVMBuildCall2(
      cg->builder, set_len->type, set_len->value,
      (LLVMValueRef[]){vl,
                       LLVMConstInt(cg->type_i64,
                                    (((uint64_t)item_count << 1) | 1u), false)},
      2, "");
  return vl;
}

static LLVMValueRef gen_expr_dict(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *e) {
  fun_sig *ds = ny_helper_dict(cg);
  fun_sig *ss = ny_helper_set(cg);
  if (!ds || !ss)
    return expr_fail(cg, e->tok, "dict literal requires dict/set helpers");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef dl = LLVMBuildCall2(
      cg->builder, ds->type, ds->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.dict.pairs.len << 2) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
    if (i > 0 && i % 64 == 0) {
      LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
      LLVMValueRef cur_fn = LLVMGetBasicBlockParent(cur_bb);
      LLVMBasicBlockRef next_bb = ny_bb_fn(cur_fn, "dct_chunk");
      ny_br(cg, next_bb);
      ny_pos(cg, next_bb);
    }
    LLVMValueRef next = LLVMBuildCall2(
        cg->builder, ss->type, ss->value,
        (LLVMValueRef[]){
            dl, gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].key),
            gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].value)},
        3, "");
    if (next)
      dl = next;
  }
  return dl;
}

static LLVMValueRef gen_expr_set(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e) {
  fun_sig *ss = ny_helper_set(cg);
  fun_sig *as = ny_helper_set_add(cg);
  if (!ss || !as) {
    if (e->as.list_like.len == 0)
      return expr_fail(cg, e->tok, "use dict() for empty dict or {'key': val} for dict literal",
                       "'{}' is not valid syntax; use dict() for an empty dict");
    return expr_fail(cg, e->tok, "set literal requires set/set_add helpers");
  }
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef sl = LLVMBuildCall2(
      cg->builder, ss->type, ss->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.list_like.len << 1) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.list_like.len; i++)
    LLVMBuildCall2(cg->builder, as->type, as->value,
                   (LLVMValueRef[]){sl, gen_expr(cg, scopes, depth,
                                                 e->as.list_like.data[i])},
                   2, "");
  return sl;
}

static LLVMValueRef expr_gen_cond_i1(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e);

static LLVMValueRef gen_expr_logical(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  bool and = strcmp(e->as.logical.op, "&&") == 0;
  ny_null_narrow_list_t rhs_narrow;
  vec_init(&rhs_narrow);
  bool narrow_rhs =
      ny_null_narrow_collect_logical_rhs(e->as.logical.left, and, &rhs_narrow);
  LLVMValueRef left = expr_gen_cond_i1(cg, scopes, depth, e->as.logical.left);
  LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
  LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef rhs_bb = ny_bb_fn(f, "lrhs"), end_bb = ny_bb_fn(f, "lend");
  ny_dbg_loc(cg, e->tok);
  if (and)
    ny_cond_br(cg, left, rhs_bb, end_bb);
  else
    ny_cond_br(cg, left, end_bb, rhs_bb);

  ny_pos(cg, rhs_bb);

  ny_null_narrow_restore_list_t rhs_applied;
  if (narrow_rhs)
    ny_null_narrow_apply(cg, scopes, depth, &rhs_narrow, true, &rhs_applied);
  LLVMValueRef rv = gen_expr(cg, scopes, depth, e->as.logical.right);
  if (narrow_rhs)
    ny_null_narrow_restore(&rhs_applied);
  vec_free(&rhs_narrow);
  ny_br(cg, end_bb);
  LLVMBasicBlockRef rend_bb = ny_cur_block(cg);

  ny_pos(cg, end_bb);

  LLVMValueRef phi = ny_phi(cg, cg->type_i64, "");
  LLVMAddIncoming(phi,
                  (LLVMValueRef[]){and ? ny_cfalse(cg)
                                       : ny_ctrue(cg),
                                   rv},
                  (LLVMBasicBlockRef[]){cur_bb, rend_bb}, 2);
  return phi;
}

static bool ny_name_leaf_is_f64_cast(const char *name) {
  if (!name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  return strcmp(leaf, "float") == 0 || strcmp(leaf, "to_float") == 0 ||
         strcmp(leaf, "f64") == 0;
}

static bool ny_call_expr_is_f64_cast(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT || e->as.call.args.len != 1)
    return false;
  size_t surface_len = 0;
  uint64_t surface_hash = 0;
  const char *surface =
      ny_builtin_surface_name_for_callee(e->as.call.callee, &surface_len,
                                         &surface_hash);
  if (surface) {
    bool shadowed = ny_builtin_name_shadowed_by_user_symbol(
        cg, scopes, depth, surface, surface_len, surface_hash);
    return !shadowed && ny_name_leaf_is_f64_cast(surface);
  }
  return ny_name_tail_is(e->as.call.callee->as.ident.name, "f64");
}

static bool ny_is_f64_like_limited(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e, unsigned budget) {
  if (!e)
    return false;
  if (budget == 0)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_FLOAT;
  if (e->kind == NY_E_IDENT) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    return b && (b->is_f64_slot || b->is_f64_direct || b->is_f32_slot ||
                 b->is_f32_direct);
  }
  if (e->kind == NY_E_BINARY) {
    return ny_is_f64_like_limited(cg, scopes, depth, e->as.binary.left,
                                  budget - 1) ||
           ny_is_f64_like_limited(cg, scopes, depth, e->as.binary.right,
                                  budget - 1);
  }
  if (e->kind == NY_E_INDEX) {
    const char *t = infer_expr_type(cg, scopes, depth, e);
    return t && (strcmp(t, "f64") == 0 || strcmp(t, "f32") == 0 ||
                 strcmp(t, "float") == 0);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    if (ny_call_expr_is_f64_cast(cg, scopes, depth, e))
      return true;
    const char *fn_name = e->as.call.callee->as.ident.name;
    if (fn_name) {
      fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
      const char *ret_type =
          sig ? (sig->return_type ? sig->return_type
                                  : sig->inferred_return_type)
              : NULL;
      return ret_type && (strcmp(ret_type, "f64") == 0 ||
                          strcmp(ret_type, "f32") == 0 ||
                          strcmp(ret_type, "float") == 0);
    }
  }
  return false;
}

static bool ny_is_f64_like(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e) {
  return ny_is_f64_like_limited(cg, scopes, depth, e, 3);
}

static bool ny_is_f64_arith_op(const char *op);

static bool ny_is_numeric_expr_like_limited(codegen_t *cg, scope *scopes, size_t depth,
                                            expr_t *e, unsigned budget) {
  if (!e)
    return false;
  if (budget == 0)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT ||
           e->as.literal.kind == NY_LIT_FLOAT;
  if (e->kind == NY_E_IDENT) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    return b && (b->is_int_slot || b->is_int_direct || b->is_f64_slot ||
                 b->is_f64_direct || b->is_f32_slot || b->is_f32_direct ||
                 (b->type_name &&
                  (strcmp(b->type_name, "int") == 0 ||
                   strcmp(b->type_name, "i64") == 0 ||
                   strcmp(b->type_name, "f32") == 0 ||
                   strcmp(b->type_name, "f64") == 0)));
  }
  if (e->kind == NY_E_BINARY) {
    if (expr_int_range(cg, scopes, depth, e, NULL, NULL))
      return true;
    const char *op = e->as.binary.op;
    if (!ny_is_f64_arith_op(op))
      return false;
    return ny_is_numeric_expr_like_limited(cg, scopes, depth, e->as.binary.left,
                                           budget - 1) &&
           ny_is_numeric_expr_like_limited(cg, scopes, depth, e->as.binary.right,
                                           budget - 1);
  }
  if (e->kind == NY_E_INDEX) {
    const char *t = infer_expr_type(cg, scopes, depth, e);
    return t && (strcmp(t, "int") == 0 || strcmp(t, "i64") == 0 ||
                 strcmp(t, "f32") == 0 || strcmp(t, "f64") == 0 ||
                 strcmp(t, "float") == 0);
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    if (ny_call_expr_is_f64_cast(cg, scopes, depth, e))
      return true;
    const char *fn_name = e->as.call.callee->as.ident.name;
    if (fn_name) {
      fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
      const char *ret_type =
          sig ? (sig->return_type ? sig->return_type
                                  : sig->inferred_return_type)
              : NULL;
      return ret_type && (strcmp(ret_type, "int") == 0 ||
                          strcmp(ret_type, "i64") == 0 ||
                          strcmp(ret_type, "f32") == 0 ||
                          strcmp(ret_type, "f64") == 0 ||
                          strcmp(ret_type, "float") == 0);
    }
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 &&
      (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
    const char *target_type =
        infer_expr_type(cg, scopes, depth, e->as.memcall.target);
    return target_type && (strcmp(target_type, "list") == 0 ||
                           strcmp(target_type, "tuple") == 0 ||
                           strncmp(target_type, "list<", 5) == 0 ||
                           strncmp(target_type, "tuple<", 6) == 0);
  }
  return false;
}

static bool ny_is_numeric_expr_like(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *e) {
  return ny_is_numeric_expr_like_limited(cg, scopes, depth, e, 3);
}

static bool ny_is_f64_arith_op(const char *op) {
   return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
          strcmp(op, "/") == 0;
}

static bool expr_is_cmp_op(const char *op) {
  return op && (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
}

static LLVMIntPredicate expr_icmp_pred_for_op(const char *op) {
  if (strcmp(op, "<") == 0)
    return LLVMIntSLT;
  if (strcmp(op, "<=") == 0)
    return LLVMIntSLE;
  if (strcmp(op, ">") == 0)
    return LLVMIntSGT;
  if (strcmp(op, ">=") == 0)
    return LLVMIntSGE;
  if (strcmp(op, "!=") == 0)
    return LLVMIntNE;
  return LLVMIntEQ;
}

static LLVMRealPredicate expr_fcmp_pred_for_op(const char *op) {
  if (strcmp(op, "<") == 0)
    return LLVMRealOLT;
  if (strcmp(op, "<=") == 0)
    return LLVMRealOLE;
  if (strcmp(op, ">") == 0)
    return LLVMRealOGT;
  if (strcmp(op, ">=") == 0)
    return LLVMRealOGE;
  if (strcmp(op, "!=") == 0)
    return LLVMRealUNE;
  return LLVMRealOEQ;
}

static LLVMValueRef expr_gen_cond_i1(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e)
    return LLVMConstInt(ny_i1_ty(cg), 0, false);

  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL)
    return LLVMConstInt(ny_i1_ty(cg), e->as.literal.as.b ? 1 : 0, false);

  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "!") == 0) {
    LLVMValueRef inner = expr_gen_cond_i1(cg, scopes, depth, e->as.unary.right);
    return LLVMBuildNot(cg->builder, inner, "expr_cond_not");
  }

  if (e->kind == NY_E_LOGICAL && e->as.logical.op) {
    bool and_op = strcmp(e->as.logical.op, "&&") == 0;
    bool or_op = strcmp(e->as.logical.op, "||") == 0;
    if (and_op || or_op) {
      ny_null_narrow_list_t rhs_narrow;
      vec_init(&rhs_narrow);
      bool narrow_rhs =
          ny_null_narrow_collect_logical_rhs(e->as.logical.left, and_op, &rhs_narrow);

      LLVMValueRef left = expr_gen_cond_i1(cg, scopes, depth, e->as.logical.left);
      LLVMBasicBlockRef left_bb = ny_cur_block(cg);
      LLVMValueRef fn = LLVMGetBasicBlockParent(left_bb);
      LLVMBasicBlockRef rhs_bb = ny_bb_fn(fn, "expr_cond_rhs");
      LLVMBasicBlockRef end_bb = ny_bb_fn(fn, "expr_cond_end");
      if (and_op)
        ny_cond_br(cg, left, rhs_bb, end_bb);
      else
        ny_cond_br(cg, left, end_bb, rhs_bb);

      ny_pos(cg, rhs_bb);
      ny_null_narrow_restore_list_t rhs_applied;
      if (narrow_rhs)
        ny_null_narrow_apply(cg, scopes, depth, &rhs_narrow, true, &rhs_applied);
      LLVMValueRef right = expr_gen_cond_i1(cg, scopes, depth, e->as.logical.right);
      if (narrow_rhs)
        ny_null_narrow_restore(&rhs_applied);
      vec_free(&rhs_narrow);
      LLVMBasicBlockRef rhs_done = ny_cur_block(cg);
      ny_br(cg, end_bb);

      ny_pos(cg, end_bb);
      LLVMValueRef phi = ny_phi(cg, ny_i1_ty(cg), "expr_cond_logic");
      LLVMValueRef short_value =
          LLVMConstInt(ny_i1_ty(cg), and_op ? 0 : 1, false);
      LLVMAddIncoming(phi, (LLVMValueRef[]){short_value, right},
                      (LLVMBasicBlockRef[]){left_bb, rhs_done}, 2);
      return phi;
    }
  }

  if (e->kind == NY_E_BINARY && expr_is_cmp_op(e->as.binary.op)) {
    const char *op = e->as.binary.op;
    expr_t *le = e->as.binary.left;
    expr_t *re = e->as.binary.right;
    if ((ny_is_f64_like(cg, scopes, depth, le) ||
         ny_is_f64_like(cg, scopes, depth, re)) &&
        ny_is_numeric_expr_like(cg, scopes, depth, le) &&
        ny_is_numeric_expr_like(cg, scopes, depth, re)) {
      LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, le);
      LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, re);
      return LLVMBuildFCmp(cg->builder, expr_fcmp_pred_for_op(op), lf, rf,
                           "expr_cond_fcmp");
    }

    if (ny_is_proven_int(cg, scopes, depth, le, NULL) &&
        ny_is_proven_int(cg, scopes, depth, re, NULL)) {
      LLVMValueRef l = gen_expr(cg, scopes, depth, le);
      LLVMValueRef r = gen_expr(cg, scopes, depth, re);
      return LLVMBuildICmp(cg->builder, expr_icmp_pred_for_op(op), l, r,
                           "expr_cond_icmp");
    }
  }

  return to_bool(cg, gen_expr(cg, scopes, depth, e));
}

static const char *ny_ownership_helper_name(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT || !e->as.call.callee->as.ident.name ||
      e->as.call.args.len != 1)
    return NULL;
  const char *n = e->as.call.callee->as.ident.name;
  const char *dot = strrchr(n, '.');
  n = dot ? dot + 1 : n;
  if (strcmp(n, "borrow") == 0 || strcmp(n, "own") == 0 || strcmp(n, "release") == 0 ||
      strcmp(n, "forget") == 0)
    return n;
  return NULL;
}

static LLVMValueRef gen_ownership_helper_expr(codegen_t *cg, scope *scopes, size_t depth,
                                              expr_t *e, const char *name) {
  expr_t *arg = e->as.call.args.data[0].val;
  if (strcmp(name, "borrow") == 0 || strcmp(name, "own") == 0)
    return gen_expr(cg, scopes, depth, arg);
  if (strcmp(name, "release") == 0) {
    LLVMValueRef v = gen_expr(cg, scopes, depth, arg);
    fun_sig *drop_sig = lookup_fun(cg, "__drop_owned", 0);
    if (drop_sig)
      LLVMBuildCall2(cg->builder, drop_sig->type, drop_sig->value, (LLVMValueRef[]){v}, 1,
                     "own.release");
    return ny_c0(cg);
  }
  return ny_c0(cg);
}

static bool ny_adt_type_is_param(enum_def_t *owner, const char *type_name) {
  if (!owner || !type_name)
    return false;
  for (size_t i = 0; i < owner->type_params.len; i++) {
    if (owner->type_params.data[i] && strcmp(owner->type_params.data[i], type_name) == 0)
      return true;
  }
  return false;
}

static LLVMValueRef gen_adt_constructor_expr(codegen_t *cg, scope *scopes, size_t depth,
                                             expr_t *e) {
  char *name = ny_adt_member_call_full_name(cg, e);
  if (!name)
    return NULL;
  enum_def_t *owner = NULL;
  enum_member_def_t *mem = lookup_enum_member_owner(cg, name, &owner);
  if (!mem || !owner || !mem->has_payload)
    return NULL;

  call_arg_t *args = NULL;
  size_t arg_count = 0;
  ny_expr_call_args(e, &args, &arg_count);
  size_t nfields = mem->fields.len ? mem->fields.len : 1;
  bool *seen_fields = alloca(sizeof(bool) * nfields);
  expr_t **field_exprs = alloca(sizeof(expr_t *) * nfields);
  memset(seen_fields, 0, sizeof(bool) * nfields);
  memset(field_exprs, 0, sizeof(expr_t *) * nfields);

  for (size_t i = 0; i < arg_count; i++) {
    call_arg_t *arg = &args[i];
    ssize_t idx = -1;
    const char *field_name = NULL;
    if (arg->name) {
      idx = ny_enum_member_field_index(mem, arg->name);
      field_name = arg->name;
      if (idx < 0) {
        return expr_fail(cg, e->tok, "unknown field '%s' for ADT variant '%s.%s'", arg->name,
                         owner->name, mem->name);
      }
    } else {
      if (i >= mem->fields.len) {
        return expr_fail(cg, e->tok, "too many positional fields for ADT variant '%s.%s'",
                         owner->name, mem->name);
      }
      idx = (ssize_t)i;
      field_name = mem->fields.data[idx].name;
    }
    if (seen_fields[idx]) {
      return expr_fail(cg, e->tok, "duplicate field '%s' for ADT variant '%s.%s'", field_name,
                       owner->name, mem->name);
    }
    seen_fields[idx] = true;
    field_exprs[idx] = arg->val;
  }

  for (size_t i = 0; i < mem->fields.len; i++) {
    if (!seen_fields[i]) {
      return expr_fail(cg, e->tok, "missing field '%s' for ADT variant '%s.%s'",
                       mem->fields.data[i].name, owner->name, mem->name);
    }
  }

  fun_sig *malloc_sig = lookup_fun(cg, "__malloc", 0);
  if (!malloc_sig)
    return expr_fail(cg, e->tok, "__malloc required for ADT constructor");

  size_t bytes = mem->fields.len * sizeof(int64_t);
  LLVMValueRef p =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){ny_ci(cg, ((uint64_t)bytes << 1) | 1u)}, 1,
                     NY_LLVM_NAME(cg, "adt_alloc"));
  LLVMValueRef tag_addr = LLVMBuildIntToPtr(cg->builder, ny_sub(cg, p, ny_ci(cg, 8), ""),
                                            ny_ptr_i64_ty(cg), "");
  ny_store(cg, tag_addr, ny_ci(cg, mem->runtime_tag));
  LLVMValueRef size_addr = LLVMBuildIntToPtr(cg->builder, ny_sub(cg, p, ny_ci(cg, 16), ""),
                                             ny_ptr_i64_ty(cg), "");
  ny_store(cg, size_addr, ny_ci(cg, ((uint64_t)bytes << 1) | 1u));

  for (size_t i = 0; i < mem->fields.len; i++) {
    enum_field_def_t *field = &mem->fields.data[i];
    if (field->type_name && strcmp(field->type_name, "any") != 0 &&
        !ny_adt_type_is_param(owner, field->type_name))
      ensure_expr_type_compatible(cg, scopes, depth, field->type_name, field_exprs[i],
                                  field_exprs[i] ? field_exprs[i]->tok : e->tok,
                                  "ADT constructor");
    LLVMValueRef val = gen_expr(cg, scopes, depth, field_exprs[i]);
    LLVMValueRef slot_addr =
        LLVMBuildIntToPtr(cg->builder, ny_add(cg, p, ny_ci(cg, (uint64_t)i * 8), ""),
                          ny_ptr_i64_ty(cg), "");
    ny_store(cg, slot_addr, val);
  }
  return p;
}

static void ny_emit_f64_div_zero_guard(codegen_t *cg, LLVMValueRef rf) {
  fun_sig *panic_sig = lookup_fun(cg, "__panic", 0);
  if (!panic_sig)
    return;
  LLVMValueRef zero = LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), 0.0);
  LLVMValueRef is_zero = LLVMBuildFCmp(cg->builder, LLVMRealOEQ, rf, zero, "fdiv_zero");
  LLVMBasicBlockRef cur = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(cur);
  LLVMBasicBlockRef panic_bb = ny_bb_fn(fn, "fdiv.zero.panic");
  LLVMBasicBlockRef ok_bb = ny_bb_fn(fn, "fdiv.zero.ok");
  ny_cond_br(cg, is_zero, panic_bb, ok_bb);

  ny_pos(cg, panic_bb);
  const char *msg = "division by zero";
  LLVMValueRef msg_global = const_string_ptr(cg, msg, strlen(msg));
  LLVMValueRef msg_ptr = ny_load(cg, msg_global, "fdiv_zero_msg");
  LLVMBuildCall2(cg->builder, panic_sig->type, panic_sig->value,
                 (LLVMValueRef[]){msg_ptr}, 1, "");
  LLVMBuildUnreachable(cg->builder);

  ny_pos(cg, ok_bb);
}

static bool ny_z3_range_excludes_zero(int64_t min_raw, int64_t max_raw,
                                      bool *decided) {
  if (decided)
    *decided = false;
#ifdef NYTRIX_HAS_Z3
  Z3_config cfg = Z3_mk_config();
  if (!cfg)
    return false;
  Z3_context z3 = Z3_mk_context(cfg);
  Z3_solver solver = Z3_mk_solver(z3);
  Z3_solver_inc_ref(z3, solver);

  Z3_sort i64 = Z3_mk_bv_sort(z3, 64);
  Z3_symbol x_sym = Z3_mk_string_symbol(z3, "divisor");
  Z3_ast x = Z3_mk_const(z3, x_sym, i64);
  Z3_ast lo = Z3_mk_int64(z3, min_raw, i64);
  Z3_ast hi = Z3_mk_int64(z3, max_raw, i64);
  Z3_ast zero = Z3_mk_int64(z3, 0, i64);

  Z3_solver_assert(z3, solver, Z3_mk_bvsge(z3, x, lo));
  Z3_solver_assert(z3, solver, Z3_mk_bvsle(z3, x, hi));
  Z3_solver_assert(z3, solver, Z3_mk_eq(z3, x, zero));
  Z3_lbool status = Z3_solver_check(z3, solver);
  bool excludes_zero = status == Z3_L_FALSE;
  if (decided)
    *decided = status != Z3_L_UNDEF;

  Z3_solver_dec_ref(z3, solver);
  Z3_del_context(z3);
  Z3_del_config(cfg);
  return excludes_zero;
#else
  (void)min_raw;
  (void)max_raw;
  return false;
#endif
}

static bool ny_f64_expr_proven_nonzero(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_INT)
      return e->as.literal.as.i != 0;
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return e->as.literal.as.f != 0.0;
  }
  if (ny_call_expr_is_f64_cast(cg, scopes, depth, e) &&
      e->as.call.args.len == 1) {
    expr_t *arg = e->as.call.args.data[0].val;
    int64_t cast_min_raw = 0, cast_max_raw = 0;
    if (expr_int_range(cg, scopes, depth, arg, &cast_min_raw,
                       &cast_max_raw)) {
      bool z3_decided = false;
      bool z3_nonzero =
          ny_z3_range_excludes_zero(cast_min_raw, cast_max_raw, &z3_decided);
      if (z3_decided)
        return z3_nonzero;
      return cast_min_raw > 0 || cast_max_raw < 0;
    }
  }
  int64_t min_raw = 0, max_raw = 0;
  if (expr_int_range(cg, scopes, depth, e, &min_raw, &max_raw)) {
    bool z3_decided = false;
    bool z3_nonzero = ny_z3_range_excludes_zero(min_raw, max_raw, &z3_decided);
    if (z3_decided)
      return z3_nonzero;
    return min_raw > 0 || max_raw < 0;
  }
  return false;
}

static LLVMValueRef ny_emit_f64_op(codegen_t *cg, const char *op,
                                   LLVMValueRef lf, LLVMValueRef rf,
                                   bool divisor_nonzero) {
  if (strcmp(op, "+") == 0)
    return LLVMBuildFAdd(cg->builder, lf, rf, "fadd");
  if (strcmp(op, "-") == 0)
    return LLVMBuildFSub(cg->builder, lf, rf, "fsub");
  if (strcmp(op, "*") == 0)
    return LLVMBuildFMul(cg->builder, lf, rf, "fmul");
  if (strcmp(op, "/") == 0) {
    if (!divisor_nonzero)
      ny_emit_f64_div_zero_guard(cg, rf);
    return LLVMBuildFDiv(cg->builder, lf, rf, "fdiv");
  }
  return NULL;
}

static bool ny_f64_raw_int_expr_shape_supported(expr_t *e, unsigned budget) {
  if (!e || budget == 0)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_INT;
  case NY_E_IDENT:
    return true;
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op)
      return false;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0 || strcmp(op, "&") == 0) {
      return ny_f64_raw_int_expr_shape_supported(e->as.binary.left,
                                                 budget - 1) &&
             ny_f64_raw_int_expr_shape_supported(e->as.binary.right,
                                                 budget - 1);
    }
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0 ||
        strcmp(op, ">>") == 0) {
      int64_t rhs_lit = 0;
      if (!ny_expr_literal_i64(e->as.binary.right, &rhs_lit))
        return false;
      if ((strcmp(op, "/") == 0 || strcmp(op, "%") == 0) && rhs_lit <= 0)
        return false;
      if (strcmp(op, ">>") == 0 && (rhs_lit < 0 || rhs_lit >= 64))
        return false;
      return ny_f64_raw_int_expr_shape_supported(e->as.binary.left,
                                                 budget - 1);
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ny_f64_inline_try_raw_int_value(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e,
                                            LLVMValueRef *raw_out,
                                            int64_t *min_out,
                                            int64_t *max_out) {
  if (raw_out)
    *raw_out = NULL;
  if (!cg || !e || !ny_f64_raw_int_expr_shape_supported(e, 24))
    return false;
  int64_t min_raw = 0, max_raw = 0;
  if (!expr_int_range(cg, scopes, depth, e, &min_raw, &max_raw))
    return false;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw ||
      !ok || !LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return false;
  if (raw_out)
    *raw_out = raw;
  if (min_out)
    *min_out = min_raw;
  if (max_out)
    *max_out = max_raw;
  return true;
}

static LLVMValueRef ny_try_raw_int_expr_as_f64(codegen_t *cg, scope *scopes,
                                               size_t depth, expr_t *e) {
  if (!cg || !e || ny_env_enabled("NYTRIX_DISABLE_F64_RAW_INT_EXPR"))
    return NULL;
  if (!ny_f64_raw_int_expr_shape_supported(e, 24))
    return NULL;
  int64_t min_raw = 0, max_raw = 0;
  if (!expr_int_range(cg, scopes, depth, e, &min_raw, &max_raw))
    return NULL;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw ||
      !ok || !LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return NULL;
  (void)min_raw;
  (void)max_raw;
  return LLVMBuildSIToFP(cg->builder, raw, cg->type_f64,
                         NY_LLVM_NAME(cg, "raw_int_i2f"));
}

static bool ny_f64_inline_body_supported(stmt_t *body) {
  if (!body)
    return false;
  if (body->kind == NY_S_RETURN || body->kind == NY_S_EXPR)
    return true;
  if (body->kind != NY_S_BLOCK || body->as.block.body.len == 0)
    return false;
  for (size_t i = 0; i < body->as.block.body.len; ++i) {
    stmt_t *s = body->as.block.body.data[i];
    if (i + 1 == body->as.block.body.len)
      return ny_f64_inline_body_supported(s);
    if (!s || s->kind != NY_S_VAR || s->as.var.is_mut ||
        s->as.var.is_del || s->as.var.is_destructure ||
        s->as.var.names.len != s->as.var.exprs.len)
      return false;
  }
  return false;
}

static void ny_f64_inline_bind_raw_int(codegen_t *cg, scope *scopes,
                                       size_t depth, const char *name,
                                       LLVMValueRef raw, const char *explicit_type,
                                       int64_t min_raw, int64_t max_raw) {
  if (!cg || !name || !raw)
    return;
  LLVMValueRef tagged = ny_tag_int(cg, raw);
  scope_bind(cg, scopes, depth, name, tagged, NULL, false,
             explicit_type ? explicit_type : "int", false);
  binding *b = &scopes[depth].vars.data[scopes[depth].vars.len - 1];
  b->is_int_direct = true;
  b->is_int_raw_direct = true;
  b->raw_int_value = raw;
  b->has_int_range = true;
  b->int_min_raw = min_raw;
  b->int_max_raw = max_raw;
}

static void ny_f64_inline_bind_value(codegen_t *cg, scope *scopes,
                                     size_t depth, const char *name,
                                     LLVMValueRef value, const char *explicit_type,
                                     expr_t *source_expr) {
  if (!name || !value)
    return;
  const char *inferred_type =
      explicit_type ? explicit_type : infer_expr_type(cg, scopes, depth, source_expr);
  if (LLVMTypeOf(value) == cg->type_f64 || LLVMTypeOf(value) == cg->type_f32) {
    const char *bind_type =
        explicit_type ? explicit_type
                      : (LLVMTypeOf(value) == cg->type_f32 ? "f32" : "f64");
    scope_bind(cg, scopes, depth, name, value, NULL, false, bind_type, false);
    binding *b = &scopes[depth].vars.data[scopes[depth].vars.len - 1];
    if (LLVMTypeOf(value) == cg->type_f32)
      b->is_f32_direct = true;
    else
      b->is_f64_direct = true;
    return;
  }
  value = expr_cast_to_i64(cg, value, "inline_f64_value");
  bool proven_int = ny_type_is(inferred_type, "int") ||
                    ny_type_is(inferred_type, "i64") ||
                    ny_is_proven_int(cg, scopes, depth, source_expr, value);
  bool proven_f64 = ny_type_is(inferred_type, "f64") ||
                    ny_type_is(inferred_type, "f32") ||
                    ny_type_is(inferred_type, "float") ||
                    ny_is_f64_like(cg, scopes, depth, source_expr);
  int64_t min_raw = 0, max_raw = 0;
  bool has_int_range =
      proven_int && expr_int_range(cg, scopes, depth, source_expr, &min_raw,
                                   &max_raw);
  const char *bind_type =
      explicit_type ? explicit_type : (proven_int ? "int" : (proven_f64 ? "f64" : NULL));
  scope_bind(cg, scopes, depth, name, value, NULL, false, bind_type, false);
  binding *b = &scopes[depth].vars.data[scopes[depth].vars.len - 1];
  if (proven_int) {
    b->is_int_direct = true;
    b->is_int_raw_direct = true;
    b->raw_int_value = ny_untag_int(cg, value);
    if (has_int_range) {
      b->has_int_range = true;
      b->int_min_raw = min_raw;
      b->int_max_raw = max_raw;
    }
  } else if (proven_f64) {
    b->is_f64_direct = true;
  }
}

static LLVMValueRef ny_emit_inline_f64_body(codegen_t *cg, scope *scopes,
                                            size_t depth, stmt_t *body) {
  if (!body)
    return NULL;
  if (body->kind == NY_S_RETURN)
    return gen_expr_as_f64(cg, scopes, depth, body->as.ret.value);
  if (body->kind == NY_S_EXPR)
    return gen_expr_as_f64(cg, scopes, depth, body->as.expr.expr);
  if (body->kind != NY_S_BLOCK)
    return NULL;
  for (size_t i = 0; i < body->as.block.body.len; ++i) {
    stmt_t *s = body->as.block.body.data[i];
    if (i + 1 == body->as.block.body.len)
      return ny_emit_inline_f64_body(cg, scopes, depth, s);
    if (!s || s->kind != NY_S_VAR)
      return NULL;
    stmt_var_t *var = &s->as.var;
    for (size_t k = 0; k < var->names.len; ++k) {
      expr_t *init = var->exprs.data[k];
      const char *decl_type = k < var->types.len ? var->types.data[k] : NULL;
      LLVMValueRef raw = NULL;
      int64_t min_raw = 0, max_raw = 0;
      if (ny_f64_inline_try_raw_int_value(cg, scopes, depth, init, &raw,
                                          &min_raw, &max_raw)) {
        ny_f64_inline_bind_raw_int(cg, scopes, depth, var->names.data[k], raw,
                                   decl_type, min_raw, max_raw);
        continue;
      }
      LLVMValueRef v = gen_expr(cg, scopes, depth, init);
      ny_f64_inline_bind_value(cg, scopes, depth, var->names.data[k], v,
                               decl_type, init);
    }
  }
  return NULL;
}

static LLVMValueRef ny_try_inline_call_as_f64(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *call_expr,
                                              fun_sig *sig) {
  if (!cg || !scopes || !call_expr || call_expr->kind != NY_E_CALL || !sig ||
      sig->is_variadic || sig->is_extern || sig->is_recursive || !sig->stmt_t ||
      sig->stmt_t->kind != NY_S_FUNC || !sig->stmt_t->as.fn.body)
    return NULL;
  if (!ny_codegen_speed_profile_enabled(cg) &&
      !ny_env_enabled("NYTRIX_INLINE_F64_HELPERS"))
    return NULL;
  if (ny_is_stdlib_tok(sig->stmt_t->tok))
    return NULL;
  expr_call_t *call = &call_expr->as.call;
  ny_param_list *params = &sig->stmt_t->as.fn.params;
  if (call->args.len != params->len || params->len > 8)
    return NULL;
  for (size_t i = 0; i < call->args.len; ++i)
    if (call->args.data[i].name)
      return NULL;
  if (!ny_f64_inline_body_supported(sig->stmt_t->as.fn.body))
    return NULL;

  LLVMValueRef arg_values[8] = {0};
  const char *arg_types[8] = {0};
  bool arg_is_int[8] = {0};
  bool arg_is_f64[8] = {0};
  bool arg_has_range[8] = {0};
  LLVMValueRef arg_raw_values[8] = {0};
  int64_t arg_min_raw[8] = {0};
  int64_t arg_max_raw[8] = {0};
  for (size_t i = 0; i < call->args.len; ++i) {
    expr_t *arg = call->args.data[i].val;
    if (!arg)
      return NULL;
    const char *arg_type = infer_expr_type(cg, scopes, depth, arg);
    bool proven_int = ny_type_is(arg_type, "int") ||
                      ny_type_is(arg_type, "i64") ||
                      ny_is_proven_int(cg, scopes, depth, arg, NULL);
    bool proven_f64 = ny_type_is(arg_type, "f64") ||
                      ny_type_is(arg_type, "f32") ||
                      ny_type_is(arg_type, "float") ||
                      ny_is_f64_like(cg, scopes, depth, arg);
    LLVMValueRef raw = NULL;
    bool raw_arg = proven_int &&
                   ny_f64_inline_try_raw_int_value(cg, scopes, depth, arg,
                                                   &raw, &arg_min_raw[i],
                                                   &arg_max_raw[i]);
    LLVMValueRef v = raw_arg ? ny_tag_int(cg, raw)
                             : ((proven_f64 && !proven_int)
                                    ? gen_expr_as_f64(cg, scopes, depth, arg)
                                    : gen_expr(cg, scopes, depth, arg));
    if (!v)
      return NULL;
    if (!(proven_f64 && !proven_int))
      v = expr_cast_to_i64(cg, v, "inline_f64_arg");
    arg_values[i] = v;
    arg_raw_values[i] = raw_arg ? raw : NULL;
    arg_is_int[i] = proven_int;
    arg_is_f64[i] = proven_f64 && !proven_int;
    arg_has_range[i] =
        raw_arg ||
        (proven_int &&
         expr_int_range(cg, scopes, depth, arg, &arg_min_raw[i],
                        &arg_max_raw[i]));
    arg_types[i] = proven_int ? "int" : (proven_f64 ? "f64" : NULL);
  }

  size_t inline_depth = depth + 1;
  memset(&scopes[inline_depth], 0, sizeof(scopes[inline_depth]));
  for (size_t i = 0; i < params->len; ++i) {
    param_t *p = &params->data[i];
    if (!p->name || !arg_values[i])
      continue;
    const char *ptype = p->type ? p->type : arg_types[i];
    scope_bind(cg, scopes, inline_depth, p->name, arg_values[i], NULL, false,
               ptype, false);
    binding *b =
        &scopes[inline_depth].vars.data[scopes[inline_depth].vars.len - 1];
    if (arg_is_int[i]) {
      b->is_int_direct = true;
      b->is_int_raw_direct = true;
      b->raw_int_value = arg_raw_values[i] ? arg_raw_values[i]
                                           : ny_untag_int(cg, arg_values[i]);
      if (arg_has_range[i]) {
        b->has_int_range = true;
        b->int_min_raw = arg_min_raw[i];
        b->int_max_raw = arg_max_raw[i];
      }
    } else if (arg_is_f64[i]) {
      b->is_f64_direct = true;
    }
  }

  LLVMValueRef out =
      ny_emit_inline_f64_body(cg, scopes, inline_depth, sig->stmt_t->as.fn.body);
  scope_pop(scopes, &inline_depth);
  return out;
}

LLVMValueRef gen_expr_as_f64(codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *e) {
  if (!e)
    return LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), 0.0);
  LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->ctx);

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      double d = e->as.literal.as.f;
      if (e->as.literal.hint == NY_LIT_HINT_F32)
        d = (double)(float)d;
      return LLVMConstReal(f64_ty, d);
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      return LLVMConstReal(f64_ty, (double)e->as.literal.as.i);
    }
    break;
  case NY_E_IDENT: {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct)) {
      b->is_used = true;
      return b->is_slot
                 ? LLVMBuildLoad2(cg->builder, cg->type_f64, b->value, "f64_ld")
                 : b->value;
    }
    if (b && (b->is_f32_slot || b->is_f32_direct)) {
      b->is_used = true;
      LLVMValueRef f32_val =
          b->is_slot
              ? LLVMBuildLoad2(cg->builder, cg->type_f32, b->value, "f32_ld")
              : b->value;
      return LLVMBuildFPExt(cg->builder, f32_val, cg->type_f64, "f2f");
    }

    if (b && b->type_name && strcmp(b->type_name, "int") == 0) {
      b->is_used = true;
      LLVMValueRef raw =
          (!b->is_slot && b->is_int_raw_direct && b->raw_int_value)
              ? b->raw_int_value
              : ny_untag_int(cg, b->is_slot ? ny_load(cg, b->value, "") : b->value);
      return LLVMBuildSIToFP(cg->builder, raw, f64_ty, "i2f");
    }
    break;
  }
  case NY_E_BINARY: {
    LLVMValueRef raw_int_f64 = ny_try_raw_int_expr_as_f64(cg, scopes, depth, e);
    if (raw_int_f64)
      return raw_int_f64;
    const char *op = e->as.binary.op;
    if (!ny_is_f64_arith_op(op))
      break;
    if (!ny_is_numeric_expr_like(cg, scopes, depth, e->as.binary.left) ||
        !ny_is_numeric_expr_like(cg, scopes, depth, e->as.binary.right))
      break;

    LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, e->as.binary.left);
    LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, e->as.binary.right);
    bool divisor_nonzero =
        strcmp(op, "/") == 0 &&
        ny_f64_expr_proven_nonzero(cg, scopes, depth, e->as.binary.right);
    return ny_emit_f64_op(cg, op, lf, rf, divisor_nonzero);
  }
  case NY_E_UNARY:
    if (e->as.unary.op && e->as.unary.right) {
      if (strcmp(e->as.unary.op, "+") == 0)
        return gen_expr_as_f64(cg, scopes, depth, e->as.unary.right);
      if (strcmp(e->as.unary.op, "-") == 0)
        return LLVMBuildFNeg(cg->builder,
                             gen_expr_as_f64(cg, scopes, depth, e->as.unary.right),
                             "fneg");
    }
    break;
  case NY_E_TERNARY: {
    LLVMValueRef cond = expr_gen_cond_i1(cg, scopes, depth, e->as.ternary.cond);
    LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
    LLVMValueRef fn = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = ny_bb_fn(fn, "tern_f64_true");
    LLVMBasicBlockRef false_bb = ny_bb_fn(fn, "tern_f64_false");
    LLVMBasicBlockRef end_bb = ny_bb_fn(fn, "tern_f64_end");
    ny_cond_br(cg, cond, true_bb, false_bb);

    ny_pos(cg, true_bb);
    LLVMValueRef true_val =
        gen_expr_as_f64(cg, scopes, depth, e->as.ternary.true_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef true_end_bb = ny_cur_block(cg);

    ny_pos(cg, false_bb);
    LLVMValueRef false_val =
        gen_expr_as_f64(cg, scopes, depth, e->as.ternary.false_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef false_end_bb = ny_cur_block(cg);

    ny_pos(cg, end_bb);
    LLVMValueRef phi = ny_phi(cg, cg->type_f64, "tern_f64");
    LLVMAddIncoming(phi, (LLVMValueRef[]){true_val, false_val},
                    (LLVMBasicBlockRef[]){true_end_bb, false_end_bb}, 2);
    return phi;
  }
  case NY_E_CALL: {
    LLVMValueRef f64buf_load =
        ny_try_fast_f64buf_load_as_f64(cg, scopes, depth, e);
    if (f64buf_load)
      return f64buf_load;
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      size_t surface_len = 0;
      uint64_t surface_hash = 0;
      const char *surface = ny_builtin_surface_name_for_callee(
          e->as.call.callee, &surface_len, &surface_hash);
      bool builtin_shadowed =
          !surface || ny_builtin_name_shadowed_by_user_symbol(
                          cg, scopes, depth, surface, surface_len, surface_hash);
      const char *fn_name = e->as.call.callee->as.ident.name;
      if (fn_name) {
        const char *leaf = strrchr(fn_name, '.');
        leaf = leaf ? leaf + 1 : fn_name;
        const char *surface_leaf = surface ? strrchr(surface, '.') : NULL;
        surface_leaf = surface_leaf ? surface_leaf + 1 : surface;
        if (!builtin_shadowed && e->as.call.args.len == 1 &&
            ((surface_leaf && (strcmp(surface_leaf, "float") == 0 ||
                               strcmp(surface_leaf, "to_float") == 0 ||
                               strcmp(surface_leaf, "f64") == 0)) ||
             strcmp(leaf, "std.core.float") == 0 ||
             strcmp(leaf, "std.core.to_float") == 0 ||
             strcmp(leaf, "std.core.f64") == 0 ||
             strcmp(leaf, "float") == 0 || strcmp(leaf, "to_float") == 0 ||
             strcmp(leaf, "f64") == 0)) {
          expr_t *arg = e->as.call.args.data[0].val;
          if (!arg)
            return LLVMConstReal(f64_ty, 0.0);
          if (ny_is_proven_int(cg, scopes, depth, arg, NULL)) {
            LLVMValueRef tagged = gen_expr(cg, scopes, depth, arg);
            LLVMValueRef raw = ny_untag_int(cg, tagged);
            return LLVMBuildSIToFP(cg->builder, raw, f64_ty, "i2f");
          }
          return gen_expr_as_f64(cg, scopes, depth, arg);
        }
        fun_sig *sig = resolve_overload(cg, fn_name, e->as.call.args.len, 0);
        const char *ret_type =
            sig ? (sig->return_type ? sig->return_type
                                    : sig->inferred_return_type)
                : NULL;
        if (!ret_type)
          ret_type = infer_expr_type(cg, scopes, depth, e);
        if (sig && ret_type &&
            (strcmp(ret_type, "f64") == 0 || strcmp(ret_type, "f32") == 0 ||
             strcmp(ret_type, "float") == 0)) {
          LLVMValueRef inlined =
              ny_try_inline_call_as_f64(cg, scopes, depth, e, sig);
          if (inlined)
            return inlined;
          LLVMValueRef native = ny_try_native_call_as_f64(cg, scopes, depth, e);
          if (native)
            return native;
          LLVMValueRef call_result = gen_call_expr(cg, scopes, depth, e);
          return ny_unbox_float(cg, call_result);
        }
        if (sig && ret_type && strcmp(ret_type, "int") == 0) {
          LLVMValueRef call_result = gen_call_expr(cg, scopes, depth, e);
          return LLVMBuildSIToFP(cg->builder, ny_untag_int(cg, call_result),
                                 f64_ty, "i2f.call");
        }
      }
    }
    break;
  }
  case NY_E_MEMCALL: {
    LLVMValueRef f64_get = ny_try_emit_f64_list_get_as_f64(cg, scopes, depth, e);
    if (f64_get)
      return f64_get;
    break;
  }
  case NY_E_INDEX: {
    const char *t = infer_expr_type(cg, scopes, depth, e);
    if (t && (strcmp(t, "f64") == 0 || strcmp(t, "f32") == 0 ||
              strcmp(t, "float") == 0)) {
      return ny_unbox_known_numeric_float(cg, gen_expr(cg, scopes, depth, e));
    }
    break;
  }
  default:
    break;
  }

  LLVMValueRef v = gen_expr(cg, scopes, depth, e);

  if (e->kind == NY_E_IDENT) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct)) {
      LLVMValueRef ptr =
          LLVMBuildIntToPtr(cg->builder, v, LLVMPointerType(f64_ty, 0), "");
      return LLVMBuildLoad2(cg->builder, f64_ty, ptr, "f64_load");
    }
  }

  return ny_unbox_float(cg, v);
}

static LLVMValueRef ny_try_emit_mono_raw_int_expr(codegen_t *cg, scope *scopes, size_t depth,
                                                  expr_t *e) {
  if (!cg || !e || cg->mono_raw_expr_disabled)
    return NULL;
  bool mono_mode = cg->mono_emitting;
  bool proven_fast =
      !mono_mode && ny_fast_path_enabled(cg, "NYTRIX_PROVEN_RAW_INT_EXPR_FAST") &&
      ny_is_proven_int(cg, scopes, depth, e, NULL);
  if (!mono_mode && !proven_fast)
    return NULL;
  if (!mono_mode && e->kind == NY_E_BINARY && e->as.binary.op &&
      (strcmp(e->as.binary.op, "+") == 0 ||
       strcmp(e->as.binary.op, "-") == 0) &&
      ((e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
        e->as.binary.left->as.literal.kind == NY_LIT_INT) ||
       (e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
        e->as.binary.right->as.literal.kind == NY_LIT_INT))) {
    return NULL;
  }
  if (!mono_mode && e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "%") == 0 && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.right->as.literal.kind == NY_LIT_INT &&
      e->as.binary.right->as.literal.as.i > 0) {
    return NULL;
  }
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw || !ok)
    return NULL;

  if (!mono_mode) {
    if (!LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
      return NULL;
    return ny_tag_int(cg, raw);
  }

  LLVMValueRef cur_fn = ny_cur_fn(cg);
  if (!cur_fn)
    return NULL;
  LLVMBasicBlockRef fast_bb = ny_bb_fn(cur_fn, "mono.raw.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(cur_fn, "mono.raw.slow");
  LLVMBasicBlockRef done_bb = ny_bb_fn(cur_fn, "mono.raw.done");
  ny_cond_br(cg, ok, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef fast_value = ny_tag_int(cg, raw);
  LLVMBasicBlockRef fast_end = ny_cur_block(cg);
  ny_br(cg, done_bb);

  ny_pos(cg, slow_bb);
  bool old_disabled = cg->mono_raw_expr_disabled;
  cg->mono_raw_expr_disabled = true;
  LLVMValueRef slow_value = gen_expr(cg, scopes, depth, e);
  cg->mono_raw_expr_disabled = old_disabled;
  LLVMBasicBlockRef slow_end = ny_cur_block(cg);
  if (!ny_has_terminator(cg))
    ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "mono_raw_int"));
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_value, slow_value},
                  (LLVMBasicBlockRef[]){fast_end, slow_end}, 2);
  return phi;
}

static size_t ny_int_literal_value_len(token_t tok) {
  size_t len = (size_t)tok.len;
  const char *s = tok.lexeme;
  if (!s || len < 2)
    return len;
  size_t i = len;
  while (i > 0 && isdigit((unsigned char)s[i - 1]))
    i--;
  if (i == 0 || i == len)
    return len;
  char c = s[i - 1];
  if (c != 'i' && c != 'I' && c != 'u' && c != 'U')
    return len;
  size_t suffix_start = i - 1;
  if (suffix_start > 0 && s[suffix_start - 1] == '_')
    return suffix_start - 1;
  return suffix_start;
}

static int ny_int_literal_digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

static char *ny_dec_mul_add_small(const char *dec, unsigned base, unsigned digit) {
  const char *src = (dec && *dec) ? dec : "0";
  size_t len = strlen(src);
  size_t cap = len + 16;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  size_t pos = cap - 1;
  buf[pos] = '\0';
  unsigned carry = digit;
  for (ssize_t i = (ssize_t)len - 1; i >= 0; --i) {
    char c = src[i];
    if (c < '0' || c > '9') {
      free(buf);
      return NULL;
    }
    unsigned v = (unsigned)(c - '0') * base + carry;
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (v % 10u));
    carry = v / 10u;
  }
  while (carry > 0) {
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (carry % 10u));
    carry /= 10u;
  }
  while (buf[pos] == '0' && buf[pos + 1] != '\0')
    pos++;
  char *out = ny_strdup(buf + pos);
  free(buf);
  return out;
}

static char *ny_int_literal_decimal_from_token(token_t tok, int64_t fallback) {
  char fallback_buf[64];
  snprintf(fallback_buf, sizeof(fallback_buf), "%" PRId64, fallback);
  if (!tok.lexeme || tok.len <= 0)
    return ny_strdup(fallback_buf);

  size_t len = ny_int_literal_value_len(tok);
  const char *s = tok.lexeme;
  unsigned base = 10;
  size_t i = 0;
  if (len > 2 && s[0] == '0') {
    if (s[1] == 'x' || s[1] == 'X') {
      base = 16;
      i = 2;
    } else if (s[1] == 'b' || s[1] == 'B') {
      base = 2;
      i = 2;
    } else if (s[1] == 'o' || s[1] == 'O') {
      base = 8;
      i = 2;
    }
  }

  char *dec = ny_strdup("0");
  if (!dec)
    return NULL;
  bool saw_digit = false;
  for (; i < len; i++) {
    if (s[i] == '_')
      continue;
    int digit = ny_int_literal_digit_value(s[i]);
    if (digit < 0 || (unsigned)digit >= base) {
      free(dec);
      return ny_strdup(fallback_buf);
    }
    saw_digit = true;
    char *next = ny_dec_mul_add_small(dec, base, (unsigned)digit);
    free(dec);
    if (!next)
      return NULL;
    dec = next;
  }
  if (!saw_digit) {
    free(dec);
    return ny_strdup(fallback_buf);
  }
  return dec;
}

static LLVMValueRef gen_expr_inner(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {

  if (cg->builder) {
    LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
    if (cur_bb && LLVMGetBasicBlockTerminator(cur_bb)) {
      return LLVMGetUndef(cg->type_i64);
    }
  }
  if (!e || cg->had_error)
    return ny_c0(cg);

  ny_dbg_loc(cg, e->tok);

  switch (e->kind) {
  case NY_E_COMPTIME:
    return gen_comptime_eval(cg, e->as.comptime_expr.body);
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      int64_t raw = e->as.literal.as.i;
      if (ny_small_int_fits_i64(raw))
        return LLVMConstInt(cg->type_i64, ((uint64_t)raw << 1) | 1, true);
      char *dec = ny_int_literal_decimal_from_token(e->tok, raw);
      const char *lit = dec ? dec : "0";
      LLVMValueRef str_runtime_global = const_string_ptr(cg, lit, strlen(lit));
      free(dec);
      LLVMValueRef str_ptr = ny_load(cg, str_runtime_global, "big_lit_str");
      fun_sig *big_from_str = lookup_fun(cg, "__bigint_from_str", 0);
      if (!big_from_str)
        return expr_fail(cg, e->tok, "builtin __bigint_from_str missing");
      return LLVMBuildCall2(cg->builder, big_from_str->type,
                            big_from_str->value, (LLVMValueRef[]){str_ptr}, 1,
                            "big_lit");
    }
    if (e->as.literal.kind == NY_LIT_BOOL)
      return e->as.literal.as.b ? ny_ctrue(cg) : ny_cfalse(cg);
    if (e->as.literal.kind == NY_LIT_STR) {

      LLVMValueRef str_runtime_global =
          const_string_ptr(cg, e->as.literal.as.s.data, e->as.literal.as.s.len);

      return ny_load(cg, str_runtime_global, "str_ptr");
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      fun_sig *box_sig = ny_helper_flt_box(cg);
      if (!box_sig) {
        return expr_fail(cg, e->tok, "__flt_box_val not found");
      }
      double fval_d = e->as.literal.as.f;
      if (e->as.literal.hint == NY_LIT_HINT_F32) {
        float f32 = (float)fval_d;
        fval_d = (double)f32;
      }
      LLVMValueRef fval =
          LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), fval_d);
      return LLVMBuildCall2(
          cg->builder, box_sig->type, box_sig->value,
          (LLVMValueRef[]){ny_bitcast(cg, fval, cg->type_i64, "")}, 1, "");
    }
    return ny_c0(cg);
  case NY_E_IDENT: {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = expr_lookup_binding(cg, scopes, depth, e->as.ident.name, name_len,
                                     e->as.ident.hash);
    if (b) {
      if (cg->comptime && b->stmt_t && !ny_is_stdlib_tok(b->stmt_t->tok) &&
          b->value && LLVMIsAGlobalVariable(b->value)) {
        ny_ct_fast_val_t folded = ny_ct_fast_none();
        if (ny_try_eval_binding_comptime_const(cg, b, e->as.ident.name,
                                               &folded, 0)) {
          LLVMValueRef v = ny_ct_fast_to_llvm_value(cg, &folded, e->tok);
          ny_ct_fast_val_free(&folded);
          if (v)
            return v;
        }
        ny_ct_fast_val_free(&folded);
        return expr_fail(cg, e->tok,
                         "comptime cannot capture runtime global '%s'",
                         e->as.ident.name);
      }
      return expr_value_from_binding(cg, b);
    }

    fun_sig *s = lookup_fun(cg, e->as.ident.name, e->as.ident.hash);
    if (s) {
      bool boxed_callable =
          !s->is_extern && ny_named_callable_values_need_closure(cg);
      LLVMValueRef sv =
          (boxed_callable || ny_fun_sig_needs_tagged_callable_adapter(s))
              ? ny_fun_sig_tagged_callable_adapter(cg, s, e->tok, boxed_callable)
              : s->value;
      if (boxed_callable) {
        LLVMValueRef raw = ny_ptr2i64(cg, sv, "");
        return ny_box_callable_closure(cg, raw, e->tok);
      }

      return ny_ptr2i64(cg, sv, "");
    }

    enum_member_def_t *emd = lookup_enum_member(cg, e->as.ident.name);
    if (emd) {
      return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1, true);
    }

    LLVMValueRef host_ident = ny_try_host_platform_ident(cg, e->as.ident.name);
    if (host_ident)
      return host_ident;

    report_undef_symbol(cg, e->as.ident.name, e->tok);
    return ny_c0(cg);
  }
  case NY_E_UNARY: {
    return gen_expr_unary(cg, scopes, depth, e);
  }
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    expr_t *le = e->as.binary.left, *re = e->as.binary.right;

    if (op && strcmp(op, "..") == 0)
      return gen_range_expr(cg, scopes, depth, e);

    bool is_user_eq = op && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
    bool in_std_module = cg->current_module_name &&
                         (strncmp(cg->current_module_name, "std.", 4) == 0 ||
                          strncmp(cg->current_module_name, "lib.", 4) == 0);
    if (is_user_eq && !in_std_module) {
      ny_ct_fast_val_t folded = {0};
      if (ny_try_eval_comptime_expr_fast(cg, e, &folded, 0) && folded.kind == NY_CT_FAST_BOOL) {
        bool folded_bool = folded.b;
        ny_ct_fast_val_free(&folded);
        return folded_bool ? ny_ctrue(cg) : ny_cfalse(cg);
      }
      ny_ct_fast_val_free(&folded);
      if (lookup_fun(cg, "std.core.eq", 0)) {
        expr_t callee = {0};
        callee.kind = NY_E_IDENT;
        callee.tok = e->tok;
        callee.as.ident.name = "std.core.eq";
        call_arg_t args[2] = {{.val = le}, {.val = re}};
        expr_t call = {0};
        call.kind = NY_E_CALL;
        call.tok = e->tok;
        call.as.call.callee = &callee;
        call.as.call.args.data = args;
        call.as.call.args.len = 2;
        call.as.call.args.cap = 2;
        LLVMValueRef eq_res = gen_call_expr(cg, scopes, depth, &call);
        if (strcmp(op, "!=") == 0)
          return ny_select(cg, to_bool(cg, eq_res), ny_cfalse(cg), ny_ctrue(cg),
                           "ne.user");
        if (LLVMTypeOf(eq_res) == ny_i1_ty(cg))
          return ny_select(cg, eq_res, ny_ctrue(cg), ny_cfalse(cg),
                           "eq.user.tagged");
        return eq_res;
      }
    }

    LLVMValueRef mono_raw_int = ny_try_emit_mono_raw_int_expr(cg, scopes, depth, e);
    if (mono_raw_int)
      return mono_raw_int;

    bool is_arith = ny_is_f64_arith_op(op);
    bool is_cmp = (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                   strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);

    if (is_arith || is_cmp) {
      if ((ny_is_f64_like(cg, scopes, depth, le) ||
           ny_is_f64_like(cg, scopes, depth, re)) &&
          ny_is_numeric_expr_like(cg, scopes, depth, le) &&
          ny_is_numeric_expr_like(cg, scopes, depth, re)) {
        ny_dbg_loc(cg, e->tok);
        LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, le);
        LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, re);

        if (is_arith) {
          bool divisor_nonzero =
              strcmp(op, "/") == 0 &&
              ny_f64_expr_proven_nonzero(cg, scopes, depth, re);
          LLVMValueRef res = ny_emit_f64_op(cg, op, lf, rf, divisor_nonzero);
          fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
          if (box_sig) {
            return LLVMBuildCall2(
                cg->builder, box_sig->type, box_sig->value,
                (LLVMValueRef[]){ny_bitcast(cg, res, cg->type_i64, "")}, 1,
                "box");
          }
        } else {
          LLVMRealPredicate pred = LLVMRealOEQ;
          if (strcmp(op, "<") == 0)
            pred = LLVMRealOLT;
          else if (strcmp(op, "<=") == 0)
            pred = LLVMRealOLE;
          else if (strcmp(op, ">") == 0)
            pred = LLVMRealOGT;
          else if (strcmp(op, ">=") == 0)
            pred = LLVMRealOGE;
          else if (strcmp(op, "==") == 0)
            pred = LLVMRealOEQ;
          else if (strcmp(op, "!=") == 0)
            pred = LLVMRealUNE;

          LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, lf, rf, "fcmp");
          return ny_select(cg, cmp, ny_ctrue(cg), ny_cfalse(cg), "tag_bool");
        }
      }
    }

    LLVMValueRef l = gen_expr(cg, scopes, depth, le);
    LLVMValueRef r = gen_expr(cg, scopes, depth, re);
    ny_dbg_loc(cg, e->tok);
    return gen_binary(cg, scopes, depth, op, l, r, le, re);
  }
  case NY_E_CALL:
    {
      LLVMValueRef adt = gen_adt_constructor_expr(cg, scopes, depth, e);
      if (adt)
        return adt;
      const char *own_helper = ny_ownership_helper_name(e);
      if (own_helper)
        return gen_ownership_helper_expr(cg, scopes, depth, e, own_helper);
      if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
        const char *fn_name = e->as.call.callee->as.ident.name;
        fun_sig *sig = fn_name ? resolve_overload(cg, fn_name,
                                                  e->as.call.args.len, 0)
                               : NULL;
        const char *ret_type =
            sig ? (sig->return_type ? sig->return_type
                                    : sig->inferred_return_type)
                : NULL;
        if (!ret_type)
          ret_type = infer_expr_type(cg, scopes, depth, e);
        if (ret_type &&
            (strcmp(ret_type, "f64") == 0 || strcmp(ret_type, "f32") == 0 ||
             strcmp(ret_type, "float") == 0)) {
          LLVMValueRef f64v = ny_try_inline_call_as_f64(cg, scopes, depth, e, sig);
          if (f64v) {
            fun_sig *box_sig = ny_helper_flt_box(cg);
            if (!box_sig)
              return expr_fail(cg, e->tok, "__flt_box_val not found");
            return LLVMBuildCall2(
                cg->builder, box_sig->type, box_sig->value,
                (LLVMValueRef[]){ny_bitcast(cg, f64v, cg->type_i64, "")}, 1,
                "inline_f64_box");
          }
        }
      }
    }
    return gen_call_expr(cg, scopes, depth, e);
  case NY_E_MEMCALL:
    {
      LLVMValueRef adt = gen_adt_constructor_expr(cg, scopes, depth, e);
      if (adt)
        return adt;
    }
    return gen_call_expr(cg, scopes, depth, e);
  case NY_E_INDEX: {
    return gen_expr_index(cg, scopes, depth, e);
  }
  case NY_E_LIST:
  case NY_E_TUPLE: {
    return gen_expr_list_like(cg, scopes, depth, e);
  }
  case NY_E_DICT: {
    return gen_expr_dict(cg, scopes, depth, e);
  }
  case NY_E_SET: {
    return gen_expr_set(cg, scopes, depth, e);
  }
  case NY_E_PTR_TYPE:
    return ny_c0(cg);
  case NY_E_DEREF: {
    LLVMValueRef ptr = gen_expr(cg, scopes, depth, e->as.deref.target);

    ny_dbg_loc(cg, e->tok);
    LLVMValueRef raw_ptr = LLVMBuildIntToPtr(cg->builder, ptr, cg->type_i8ptr,
                                             NY_LLVM_NAME(cg, "raw_ptr"));
    return ny_load(cg, raw_ptr, NY_LLVM_NAME(cg, "deref"));
  }
  case NY_E_MEMBER: {

    char *full_name = codegen_full_name(cg, e, cg->arena);
    if (full_name) {
      enum_member_def_t *emd = lookup_enum_member(cg, full_name);
      if (emd) {
        return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1,
                            true);
      }
      if (!emd && cg->current_module_name && *cg->current_module_name) {
        size_t cur_len = strlen(cg->current_module_name);
        bool already_scoped =
            strncmp(full_name, cg->current_module_name, cur_len) == 0 &&
            full_name[cur_len] == '.';
        if (already_scoped)
          goto member_enum_scoped_done;
        size_t scoped_len = strlen(cg->current_module_name) + 1 +
                            strlen(full_name) + 1;
        char *scoped_name = arena_alloc(cg->arena, scoped_len);
        snprintf(scoped_name, scoped_len, "%s.%s", cg->current_module_name,
                 full_name);
        emd = lookup_enum_member(cg, scoped_name);
        if (emd) {
          return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1,
                              true);
        }
      }
    member_enum_scoped_done:;
      binding *gb = expr_lookup_binding(cg, NULL, 0, full_name, strlen(full_name),
                                        ny_hash64(full_name, strlen(full_name)));
      if (gb)
        return expr_value_from_binding(cg, gb);
    }
    char alias_full_name[512];
    if (e->as.member.target && e->as.member.name) {
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, scopes, depth, e->as.member.target,
                                      module_path, sizeof(module_path)) &&
          ny_resolve_module_function_path(cg, module_path, e->as.member.name,
                                          resolved_fun,
                                          sizeof(resolved_fun))) {
        fun_sig *fs = lookup_fun(cg, resolved_fun, 0);
        if (fs)
          return ny_ptr2i64(cg, fs->value, "");
      }
    }
    binding *alias_gb = expr_member_module_alias_global(cg, scopes, depth, e, alias_full_name,
                                                        sizeof(alias_full_name));
    if (alias_gb) {
      alias_gb->is_used = true;
      return expr_value_from_binding(cg, alias_gb);
    }
    const char *alias_module = expr_member_module_alias_target(cg, scopes, depth, e);
    if (alias_module) {
      const char *alias_name = e->as.member.target->as.ident.name;
      ny_diag_error(e->tok, "module '%s' has no exported member '%s'",
                    alias_module, e->as.member.name);
      ny_diag_hint("alias '%s' resolves to module '%s'", alias_name, alias_module);
      ny_diag_fix("export '%s' from '%s' or import the module that defines it",
                  e->as.member.name, alias_module);
      cg->had_error = 1;
      return ny_c0(cg);
    }

    LLVMValueRef typed_property = ny_try_member_property_expr(cg, scopes, depth, e);
    if (typed_property)
      return typed_property;

    LLVMValueRef target = gen_expr(cg, scopes, depth, e->as.member.target);
    LLVMValueRef key_str_global =
        const_string_ptr(cg, e->as.member.name, strlen(e->as.member.name));
    LLVMValueRef key_str = ny_load(cg, key_str_global, "");

    fun_sig *get_sig = ny_helper_get(cg);
    if (!get_sig) {
      ny_diag_hint(
          "the 'std.core' module provides the standard 'get' implementation");
      ny_diag_fix("add 'use std.core' to your script");
      return expr_fail(
          cg, e->tok,
          "Member access on a dynamic object requires the 'get' function.");
    }

    LLVMValueRef args[16];
    args[0] = target;
    args[1] = key_str;
    unsigned arg_count = get_sig->arity;
    if (arg_count < 2)
      arg_count = 2;
    if (arg_count > 16)
      arg_count = 16;
    for (unsigned i = 2; i < arg_count; i++) {
      args[i] = ny_c1(cg);
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, get_sig->type, get_sig->value, args,
                          arg_count, "");
  }
  case NY_E_SIZEOF: {
    const char *type_name = NULL;
    if (e->as.szof.is_type)
      type_name = e->as.szof.type_name;
    if (!type_name && e->as.szof.target &&
        e->as.szof.target->kind == NY_E_IDENT) {
      type_name = e->as.szof.target->as.ident.name;
    }
    if (!type_name) {
      ny_diag_error(e->tok, "sizeof expects a type name");
      cg->had_error = 1;
      return ny_c1(cg);
    }
    type_layout_t tl = resolve_raw_layout(cg, type_name, e->tok);
    if (!tl.is_valid) {
      cg->had_error = 1;
      return ny_c1(cg);
    }
    uint64_t sz = (uint64_t)tl.size;
    return LLVMConstInt(cg->type_i64, (sz << 1) | 1ULL, false);
  }
  case NY_E_LOGICAL: {
    return gen_expr_logical(cg, scopes, depth, e);
  }
  case NY_E_TERNARY: {
    LLVMValueRef cond = expr_gen_cond_i1(cg, scopes, depth, e->as.ternary.cond);
    LLVMBasicBlockRef cur_bb = ny_cur_block(cg);
    LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = ny_bb_fn(f, "tern_true");
    LLVMBasicBlockRef false_bb = ny_bb_fn(f, "tern_false");
    LLVMBasicBlockRef end_bb = ny_bb_fn(f, "tern_end");
    ny_dbg_loc(cg, e->tok);
    ny_cond_br(cg, cond, true_bb, false_bb);

    ny_pos(cg, true_bb);

    LLVMValueRef true_val =
        gen_expr(cg, scopes, depth, e->as.ternary.true_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef true_end_bb = ny_cur_block(cg);

    ny_pos(cg, false_bb);

    LLVMValueRef false_val =
        gen_expr(cg, scopes, depth, e->as.ternary.false_expr);
    ny_br(cg, end_bb);
    LLVMBasicBlockRef false_end_bb = ny_cur_block(cg);

    ny_pos(cg, end_bb);

    ny_dbg_loc(cg, e->tok);
    LLVMValueRef phi = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "tern"));
    LLVMAddIncoming(phi, (LLVMValueRef[]){true_val, false_val},
                    (LLVMBasicBlockRef[]){true_end_bb, false_end_bb}, 2);
    return phi;
  }
  case NY_E_ASM: {
    unsigned nargs = e->as.as_asm.args.len;
    LLVMValueRef llvm_args[nargs > 0 ? nargs : 1];
    LLVMTypeRef arg_types[nargs > 0 ? nargs : 1];
    for (unsigned i = 0; i < nargs; ++i) {
      llvm_args[i] = gen_expr(cg, scopes, depth, e->as.as_asm.args.data[i]);
      arg_types[i] = cg->type_i64;
    }
    LLVMTypeRef func_type =
        LLVMFunctionType(cg->type_i64, arg_types, nargs, false);
    LLVMValueRef asm_val = LLVMConstInlineAsm(
        func_type, e->as.as_asm.code, e->as.as_asm.constraints, true, false);
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, func_type, asm_val, llvm_args, nargs,
                          "");
  }
  case NY_E_FSTRING: {

    LLVMValueRef empty_runtime_global = const_string_ptr(cg, "", 0);
    LLVMValueRef res = ny_load(cg, empty_runtime_global, "");
    fun_sig *cs = ny_helper_str_concat(cg), *ts = ny_helper_to_str(cg);
    if (!cs || !cs->type || !cs->value)
      return expr_fail(cg, e->tok, "__str_concat not found for f-string lowering");
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t p = e->as.fstring.parts.data[i];
      LLVMValueRef pv;
      if (p.kind == NY_FSP_STR) {
        LLVMValueRef part_runtime_global =
            const_string_ptr(cg, p.as.s.data, p.as.s.len);
        pv = ny_load(cg, part_runtime_global, "");
      } else {
        if (!ts || !ts->type || !ts->value)
          return expr_fail(cg, e->tok, "__to_str not found for f-string lowering");
        pv = LLVMBuildCall2(
            cg->builder, ts->type, ts->value,
            (LLVMValueRef[]){gen_expr(cg, scopes, depth, p.as.e)}, 1, "");
      }
      ny_dbg_loc(cg, e->tok);
      res = LLVMBuildCall2(cg->builder, cs->type, cs->value,
                           (LLVMValueRef[]){res, pv}, 2, "");
    }
    return res;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {

    return gen_closure(cg, scopes, depth, e->as.lambda.params,
                       e->as.lambda.body, e->as.lambda.is_variadic,
                       e->as.lambda.return_type, "__lambda");
  }
  case NY_E_EMBED: {
    const char *fname = e->as.embed.path;
    FILE *f = fopen(fname, "rb");
    if (!f) {
      char cwd[1024];
      if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = '\0';
      return expr_fail(cg, e->tok,
                       "failed to open file for embed: %s (cwd: %s)", fname,
                       cwd);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
      fclose(f);
      return expr_fail(cg, e->tok, "OOM reading file for embed");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
      free(buf);
      fclose(f);
      return expr_fail(cg, e->tok, "failed to read file for embed: %s", fname);
    }
    fclose(f);
    LLVMValueRef g = const_string_ptr(cg, buf, (size_t)size);
    free(buf);
    return ny_load(cg, g, NY_LLVM_NAME(cg, "embed_ptr"));
  }
  case NY_E_TRY: {
    LLVMValueRef res = gen_expr(cg, scopes, depth, e->as.unary.right);
    LLVMValueRef f = ny_cur_fn(cg);
    LLVMBasicBlockRef ok_bb = ny_bb_fn(f, "try_ok");
    LLVMBasicBlockRef err_bb = ny_bb_fn(f, "try_err");

    fun_sig *is_ok_sig = lookup_fun(cg, "__is_ok", 0);
    if (!is_ok_sig) {
      return expr_fail(cg, e->tok, "__is_ok not found for '?' operator");
    }
    LLVMValueRef is_ok = LLVMBuildCall2(cg->builder, is_ok_sig->type,
                                        is_ok_sig->value, &res, 1, "");
    ny_dbg_loc(cg, e->tok);
    ny_cond_br(cg, to_bool(cg, is_ok), ok_bb, err_bb);

    ny_pos(cg, err_bb);

    if (cg->result_store_val) {
      ny_store(cg, cg->result_store_val, res);
    } else {
      emit_defers(cg, scopes, depth, cg->func_root_idx);
      ny_cg_emit_trace_return(cg, res, cg->current_fn_ret_type);
      ny_cg_emit_trace_exit(cg);
      LLVMBuildRet(cg->builder, res);
    }

    ny_pos(cg, ok_bb);

    fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap", 0);
    if (!unwrap_sig) {
      return expr_fail(cg, e->tok, "__unwrap not found for '?' operator");
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, unwrap_sig->type, unwrap_sig->value,
                          &res, 1, "unwrapped");
  }
  case NY_E_MATCH: {
    LLVMValueRef old_store = cg->result_store_val;
    LLVMValueRef slot = build_alloca(cg, "match_res", cg->type_i64);
    ny_store(cg, slot, ny_c1(cg));
    cg->result_store_val = slot;
    stmt_t fake = {.kind = NY_S_MATCH, .as.match = e->as.match, .tok = e->tok};
    size_t d = depth;
    gen_stmt(cg, scopes, &d, &fake, cg->func_root_idx, true);
    cg->result_store_val = old_store;
    return ny_load(cg, slot, "");
  }
  default: {
    return expr_fail(cg, e->tok,
                     "unsupported expression kind %d (token kind %d)", e->kind,
                     e->tok.kind);
  }
  }
}

LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {
  if (!ny_codegen_expr_kind_profile_enabled())
    return gen_expr_inner(cg, scopes, depth, e);
  if (!g_expr_kind_profile_registered) {
    atexit(ny_codegen_expr_kind_profile_report);
    g_expr_kind_profile_registered = 1;
  }
  int kind = e ? (int)e->kind : 63;
  int slot = g_expr_kind_depth;
  if (slot >= 0 && slot < (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
    g_expr_kind_child_ms[slot] = 0.0;
  g_expr_kind_depth++;
  ny_tick_t start = ny_ticks_now();
  LLVMValueRef out = gen_expr_inner(cg, scopes, depth, e);
  double total_ms = ny_ticks_elapsed_ms(start);
  g_expr_kind_depth--;
  double child_ms = 0.0;
  if (slot >= 0 && slot < (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
    child_ms = g_expr_kind_child_ms[slot];
  if (g_expr_kind_depth > 0) {
    int parent = g_expr_kind_depth - 1;
    if (parent >= 0 &&
        parent < (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
      g_expr_kind_child_ms[parent] += total_ms;
  }
  ny_codegen_expr_kind_profile_add(kind, total_ms, child_ms);
  return out;
}
