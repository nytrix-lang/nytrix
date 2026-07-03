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
