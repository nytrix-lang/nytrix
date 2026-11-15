/*
 * Native lowering: classify function results and parameter ABI slots.
 */
/*
 * Call-result f64 classification shared by every callee-contract site: the
 * declared return annotation stays authoritative, and a resolved semantic
 * fact may only refine it while it remains coherent with that annotation.
 */
static bool nyr_fn_return_is_f64(const stmt_t *fn) {
  const char *rtype = fn->as.fn.return_type;
  const ny_expr_semantic_t *rsem = &fn->as.fn.return_semantic;
  return ny_native_type_name_is_f64(rtype) ||
         (rsem->resolved && rsem->rep == NY_SEM_REP_F64 &&
          ny_sem_rep_coherent_with_type(rtype, rsem->rep));
}

/*
 * Thread and async trampolines return through a dynamic-value boundary.
 * Result kinds: raw scalar integer, canonical dynamic value, and raw bool.
 */
static int nyr_fn_result_kind(nyr_builder_impl_t *b, const stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return 0;
  if (ny_type_name_is_bool(fn->as.fn.return_type))
    return 2;
  /*
   * Unannotated lambda bodies can still have a statically known boolean
   * result. Preserve that raw 0/1 callback ABI when the closure escapes into
   * an `any` container; otherwise the dynamic trampoline treats raw true (1)
   * as the tagged integer zero.
   */
  if (b && !fn->as.fn.return_type && fn->as.fn.body) {
    const stmt_t *body = fn->as.fn.body;
    const expr_t *tail = NULL;
    if (body->kind == NY_S_EXPR)
      tail = body->as.expr.expr;
    else if (body->kind == NY_S_BLOCK && body->as.block.body.len > 0) {
      const stmt_t *last =
          body->as.block.body.data[body->as.block.body.len - 1];
      if (last && last->kind == NY_S_EXPR)
        tail = last->as.expr.expr;
      else if (last && last->kind == NY_S_RETURN)
        tail = last->as.ret.value;
    }
    if (tail && nyr_expr_is_bool(b, tail))
      return 2;
    /*
     * Parser/query callbacks frequently return dictionaries, sequences, or
     * values fetched from an `any` container without an explicit return
     * annotation. Their trampoline must preserve the tagged object ABI.
     */
    if (tail && (nyr_expr_is_dict(b, tail) || nyr_expr_is_list(b, tail) ||
                 nyr_expr_is_any(b, (expr_t *)tail)))
      return 1;
  }
  if (fn->as.fn.return_semantic.resolved) {
    ny_sem_rep_t rep = fn->as.fn.return_semantic.rep;
    if (rep == NY_SEM_REP_TAGGED_DYNAMIC || rep == NY_SEM_REP_STRING ||
        rep == NY_SEM_REP_TYPED_BUFFER || rep == NY_SEM_REP_OBJECT ||
        rep == NY_SEM_REP_POINTER || rep == NY_SEM_REP_CLOSURE ||
        rep == NY_SEM_REP_AGGREGATE)
      return 1;
    return 0;
  }
  const char *type = fn->as.fn.return_type;
  if (ny_native_type_name_is_any(type) || ny_native_type_name_is_str(type) ||
      ny_native_type_name_is_list(type) || ny_native_type_name_is_bytes(type) ||
      ny_native_type_name_is_ptr(type) ||
      (type && (strcmp(type, "dict") == 0 || strcmp(type, "set") == 0 ||
                strcmp(type, "tuple") == 0)))
    return 1;
  return 0;
}

typedef struct nyr_param_abi_t {
  ny_sem_rep_t rep;
  nyr_param_type_t value_type;
  size_t extra_slots;
  bool try_dynamic;
  bool inferred_list;
  bool dynamic_sequence;
  bool is_list;
  bool is_str;
  bool is_any;
} nyr_param_abi_t;

static bool nyr_param_abi(const nyr_builder_impl_t *b, const stmt_t *fn,
                          size_t index, nyr_param_abi_t *out) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len || !out)
    return false;
  const param_t *param = &fn->as.fn.params.data[index];
  const char *type = param->type;
  bool try_dynamic = nyr_param_is_try_dynamic(b, fn, index);
  bool inferred_list = !try_dynamic && nyr_param_is_inferred_list(fn, index);
  bool dynamic_sequence =
      !try_dynamic && nyr_param_uses_dynamic_sequence_abi(fn, index);
  ny_sem_rep_t rep = try_dynamic
                         ? NY_SEM_REP_TAGGED_DYNAMIC
                         : (param->semantic.resolved ? param->semantic.rep
                                                     : NY_SEM_REP_UNRESOLVED);
  if (!try_dynamic && type && *type)
    rep = nyr_rep_for_type(b, type);
  bool is_list = !try_dynamic && !dynamic_sequence &&
                 (ny_native_type_name_is_list(type) || inferred_list ||
                  (!type && rep == NY_SEM_REP_TYPED_BUFFER));
  bool is_str = !try_dynamic && (ny_native_type_name_is_str(type) ||
                                 (!type && rep == NY_SEM_REP_STRING));
  bool is_any = try_dynamic || dynamic_sequence ||
                ny_native_type_name_is_any(type) ||
                (!type && !is_list && !is_str && rep != NY_SEM_REP_RAW_INT &&
                 rep != NY_SEM_REP_F64 && rep != NY_SEM_REP_F32);
  nyr_param_type_t value_type =
      (ny_native_type_name_is_f64(type) || (!type && rep == NY_SEM_REP_F64))
          ? NYIR_PARAM_F64
      : (ny_native_type_name_is_f32(type) || (!type && rep == NY_SEM_REP_F32))
          ? NYIR_PARAM_F32
      : (type && (ny_type_is(type, "u8") || ny_type_is(type, "bool")) &&
         b->options && b->options->native_backend == NY_NATIVE_BACKEND_X86_64)
          ? NYIR_PARAM_U8
      : (type && ny_type_is(type, "i8") && b->options &&
         b->options->native_backend == NY_NATIVE_BACKEND_X86_64)
          ? NYIR_PARAM_I8
          : NYIR_PARAM_I64;
  *out = (nyr_param_abi_t){
      .rep = rep,
      .value_type = value_type,
      .extra_slots = is_list ? 1 : ((is_str || is_any) ? 2 : 0),
      .try_dynamic = try_dynamic,
      .inferred_list = inferred_list,
      .dynamic_sequence = dynamic_sequence,
      .is_list = is_list,
      .is_str = is_str,
      .is_any = is_any,
  };
  return true;
}

static bool nyr_set_param_types(nyr_builder_impl_t *b, const stmt_t *fn) {
  if (!b || !fn || fn->kind != NY_S_FUNC)
    return false;
  if (fn->as.fn.return_semantic.resolved &&
      !ny_sem_rep_coherent_with_type(fn->as.fn.return_type,
                                     fn->as.fn.return_semantic.rep))
    return nyr_fail(b,
                    "native NYIR lower: function return type and semantic ABI "
                    "representation disagree");
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const param_t *param = &fn->as.fn.params.data[i];
    if (param->semantic.resolved &&
        !ny_sem_rep_coherent_with_type(param->type, param->semantic.rep))
      return nyr_fail(b, "native NYIR lower: parameter type and semantic ABI "
                         "representation disagree");
  }
  size_t source_count = fn->as.fn.params.len;
  const ny_native_lambda_entry_t *lambda =
      ny_native_lambda_find_name(ny_native_lambda_cache_for(b), fn->as.fn.name);
  bool hidden_env = lambda && lambda->capture_count > 0;
  size_t count = source_count + (hidden_env ? 1 : 0);
  nyr_param_abi_t *param_abis =
      source_count ? calloc(source_count, sizeof(*param_abis)) : NULL;
  if (source_count && !param_abis)
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  for (size_t i = 0; i < source_count; ++i) {
    if (!nyr_param_abi(b, fn, i, &param_abis[i])) {
      free(param_abis);
      return nyr_fail(b, "native NYIR lower: cannot resolve parameter ABI");
    }
    if (param_abis[i].extra_slots > SIZE_MAX - count) {
      free(param_abis);
      return nyr_fail(b, "native NYIR lower: parameter ABI is too large");
    }
    count += param_abis[i].extra_slots;
  }
  if (!count) {
    free(param_abis);
    return true;
  }
  b->nyir.param_types = calloc(count, sizeof(*b->nyir.param_types));
  if (!b->nyir.param_types) {
    free(param_abis);
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  }
  b->nyir.param_count = count;
  if (source_count) {
    b->nyir.source_param_widths = calloc(source_count, sizeof(uint8_t));
    if (!b->nyir.source_param_widths) {
      free(param_abis);
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    }
    b->nyir.source_param_count = source_count;
  }
  size_t abi_index = 0;
  if (hidden_env)
    b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
  for (size_t i = 0; i < source_count; ++i) {
    const nyr_param_abi_t *param_abi = &param_abis[i];
    b->nyir.source_param_widths[i] = (uint8_t)(1 + param_abi->extra_slots);
    b->nyir.param_types[abi_index++] = param_abi->value_type;
    if (param_abi->is_list)
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    else if (param_abi->is_str || param_abi->is_any) {
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
      b->nyir.param_types[abi_index++] = NYIR_PARAM_I64;
    }
  }
  NY_COMPILER_ASSERT(
      abi_index == count,
      "native parameter count disagrees with parameter ABI slots");
  free(param_abis);
  return true;
}
