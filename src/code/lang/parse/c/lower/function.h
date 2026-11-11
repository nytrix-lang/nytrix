/* Function label collection, storage hoisting, and compilation. */
static bool collect_function_labels(c_parser_t *p, c_function_t *fn) {
  size_t candidate_count = 0;
  size_t conditional_depth = 0;
  size_t paren_depth = 0;
  for (size_t i = fn->body_start; i + 1 < fn->body_end; ++i) {
    if (tok_is(p, i, "(")) {
      ++paren_depth;
    } else if (tok_is(p, i, ")")) {
      if (paren_depth)
        --paren_depth;
    }
    if (paren_depth) {
      if (tok_is(p, i, ":") && i > fn->body_start + 1 &&
          tok_ident(p->tokens[i - 1]) && !c_label_type_keyword(p, i - 1) &&
          (tok_is(p, i - 2, "{") || tok_is(p, i - 2, ";") ||
           tok_is(p, i - 2, "}")))
        ++candidate_count;
      continue;
    }
    if (tok_is(p, i, "?")) {
      ++conditional_depth;
    } else if (tok_is(p, i, ":")) {
      if (conditional_depth) {
        --conditional_depth;
      } else if (i > fn->body_start && tok_ident(p->tokens[i - 1]) &&
                 !tok_is(p, i - 1, "default") &&
                 !c_label_type_keyword(p, i - 1) &&
                 !c_case_label_colon(p, fn, i)) {
        ++candidate_count;
      }
    }
  }
  if (!candidate_count)
    return true;
  if (candidate_count > SIZE_MAX / sizeof(*p->labels)) {
    fail(p, "C function has too many labels");
    return false;
  }
  p->labels = calloc(candidate_count, sizeof(*p->labels));
  if (!p->labels) {
    fail(p, "out of memory allocating C labels");
    return false;
  }
  conditional_depth = 0;
  paren_depth = 0;
  for (size_t i = fn->body_start; i + 1 < fn->body_end; ++i) {
    if (tok_is(p, i, "(")) {
      ++paren_depth;
    } else if (tok_is(p, i, ")")) {
      if (paren_depth)
        --paren_depth;
    }
    if (paren_depth) {
      if (tok_is(p, i, ":") && i > fn->body_start + 1 &&
          tok_ident(p->tokens[i - 1]) && !tok_is(p, i - 1, "default") &&
          (tok_is(p, i - 2, "{") || tok_is(p, i - 2, ";") ||
           tok_is(p, i - 2, "}"))) {
        if (!register_function_label(p, fn, i - 1))
          return false;
      }
      continue;
    }
    if (tok_is(p, i, "?")) {
      ++conditional_depth;
      continue;
    }
    if (!tok_is(p, i, ":"))
      continue;
    if (conditional_depth) {
      --conditional_depth;
      continue;
    }
    if (i == fn->body_start || !tok_ident(p->tokens[i - 1]))
      continue;
    if (c_label_type_keyword(p, i - 1))
      continue;
    if (tok_is(p, i - 1, "default") || c_case_label_colon(p, fn, i))
      continue;
    if (!register_function_label(p, fn, i - 1))
      return false;
  }
  return true;
}

/*
 * C fixed-size automatic objects occupy storage once per invocation, including
 * declarations and aggregate-result temporaries inside loops. Keep separate
 * ALLOCA roots for alias analysis, but execute them only in the entry block.
 * A variadic register snapshot must precede any register-clobbering storage.
 */
static bool hoist_fixed_storage(c_parser_t *p) {
  nyr_func_t *f = p->out;
  size_t first = f->len && f->data[0].op == NYIR_VA_SAVE ? 1 : 0;
  size_t count = 0;
  bool needs_move = false;
  for (size_t i = first; i < f->len; ++i) {
    if (f->data[i].op == NYIR_ALLOCA) {
      if (i != first + count)
        needs_move = true;
      ++count;
    }
  }
  if (!needs_move)
    return true;
  if (count > SIZE_MAX - f->len ||
      f->len + count > SIZE_MAX / sizeof(*f->data)) {
    fail(p, "C automatic storage instruction capacity exceeded");
    return false;
  }
  size_t needed = f->len + count;
  if (f->cap < needed) {
    nyr_inst_t *data = realloc(f->data, needed * sizeof(*data));
    if (!data) {
      fail(p, "out of memory arranging C automatic storage");
      return false;
    }
    f->data = data;
    f->cap = needed;
  }
  size_t saved = 0;
  for (size_t i = first; i < f->len; ++i)
    if (f->data[i].op == NYIR_ALLOCA)
      f->data[f->len + saved++] = f->data[i];
  size_t write = f->len;
  for (size_t i = f->len; i-- > first;)
    if (f->data[i].op != NYIR_ALLOCA) {
      --write;
      if (write != i)
        f->data[write] = f->data[i];
    }
  memcpy(f->data + first, f->data + f->len, count * sizeof(*f->data));
  return true;
}

static bool compile_function(c_parser_t *parser, c_function_t *fn,
                             nyr_func_t *out) {
  size_t saved_enum_constant_count = parser->enum_constant_count;
  size_t saved_enum_tag_count = parser->enum_tag_count;
  size_t saved_tag_scope_id = parser->tag_scope_id;
  size_t saved_tag_scope_depth = parser->tag_scope_depth;
  free(parser->labels);
  parser->labels = NULL;
  parser->label_count = 0;
  /*
   * Parameters and inserted locals are fully initialized before lookup.
   * Resetting the live count avoids clearing a translation-unit-sized table
   * for every function.
   */
  parser->function = fn;
  parser->tag_scope_id = fn->body_start + 1;
  parser->tag_scope_stack[0] = parser->tag_scope_id;
  parser->tag_scope_depth = 1;
  if (parser->next_tag_scope_id < parser->token_count)
    parser->next_tag_scope_id = parser->token_count;
  parser->out = out;
  parser->lvalue_count = 0;
  parser->va_save_area = -1;
  parser->pos = fn->body_start;
  parser->param_count = fn->param_count;
  parser->next_local = fn->param_count;
  parser->local_count = fn->param_count;
  parser->scope_start = 0;
  parser->break_target = NULL;
  parser->continue_target = NULL;
  parser->next_label = 0;
  parser->agg_base = -1;
  parser->agg_area_next = 0;
  parser->last_type_const = false;
  parser->last_type_const_pointer = false;
  parser->last_value_const = false;
  parser->last_value_null_pointer_constant = false;
  parser->last_type_agg = NULL;
  parser->last_value_const_addr = false;
  if (!collect_function_labels(parser, fn))
    goto fail;
  /*
   * MEMORY-class aggregate returns take a hidden caller buffer; register
   * classes return their ABI pieces through RET.
   */
  bool sret_result = false;
  uint32_t result_packed = 0;
  if (fn->result == C_AGG) {
    if (!fn->result_agg || !fn->result_agg->complete) {
      snprintf(parser->err, parser->err_len,
               "aggregate return type must be a complete struct or union");
      goto fail;
    }
    if (!nyc_classify_aggregate(fn->result_agg, parser->abi, 0,
                                &result_packed)) {
      snprintf(parser->err, parser->err_len,
               "aggregate return type is not supported by the C ABI");
      goto fail;
    }
    sret_result = nyc_sysv_return_is_memory(result_packed);
  }
  size_t hidden_params = sret_result ? 1u : 0u;
  bool has_agg_param = false;
  for (size_t i = 0; i < fn->param_count; ++i)
    if (fn->params[i].type == C_AGG)
      has_agg_param = true;
  out->param_count = fn->param_count + hidden_params;
  if (out->param_count) {
    out->param_types = calloc(out->param_count, sizeof(*out->param_types));
    if (!out->param_types) {
      snprintf(parser->err, parser->err_len,
               "out of memory allocating C parameter metadata");
      goto fail;
    }
    if (has_agg_param) {
      out->param_abi = calloc(out->param_count, sizeof(*out->param_abi));
      if (!out->param_abi) {
        snprintf(parser->err, parser->err_len,
                 "out of memory allocating C parameter ABI metadata");
        goto fail;
      }
    }
  }
  if (sret_result) {
    /*
     * The hidden result buffer arrives in rdi like any leading integer
     * argument.
     */
    out->param_types[0] = NYIR_PARAM_I64;
    parser->locals[0] =
        (c_local_t){.name = {0}, .type = C_I64, .slot = 0, .is_const = true};
  }
  parser->next_local = out->param_count;
  parser->local_count = out->param_count;
  for (size_t i = 0; i < fn->param_count; ++i) {
    c_param_t param = fn->params[i];
    size_t slot = hidden_params + i;
    if (!param.name.len) {
      snprintf(parser->err, parser->err_len,
               "function definition parameters require names");
      goto fail;
    }
    for (size_t previous = 0; previous < i; ++previous) {
      if (tok_same(fn->params[previous].name, param.name)) {
        snprintf(parser->err, parser->err_len,
                 "duplicate parameter name in C function");
        goto fail;
      }
    }
    if (param.type == C_AGG) {
      uint32_t packed = 0;
      if (!param.agg || !param.agg->complete) {
        snprintf(parser->err, parser->err_len,
                 "aggregate parameter type must be a complete struct or "
                 "union");
        goto fail;
      }
      if (!nyc_classify_argument(param.agg, parser->abi, 0, &packed)) {
        snprintf(parser->err, parser->err_len,
                 "aggregate parameter type in `%.*s` is not supported by "
                 "the C ABI",
                 (int)(fn->name.len > 120 ? 120 : fn->name.len),
                 fn->name.start);
        goto fail;
      }
      out->param_types[slot] = NYIR_PARAM_AGGREGATE;
      out->param_abi[slot] = packed;
      parser->locals[slot] = (c_local_t){.name = param.name,
                                         .type = C_AGG,
                                         .slot = (int)slot,
                                         .is_const = param.is_const,
                                         .agg = param.agg};
      continue;
    }
    out->param_types[slot] = param.type == C_F32   ? NYIR_PARAM_F32
                             : param.type == C_F64 ? NYIR_PARAM_F64
                                                   : NYIR_PARAM_I64;
    if (parser->abi == NYC_ABI_I386 && c_type_is_integer(param.type) &&
        c_type_size(parser->abi, param.type) == 8) {
      if (slot >= NYIR_CALL_MAX_ARGS) {
        fail(parser, "i386 C functions support at most 64 ABI parameters");
        goto fail;
      }
      out->i386_wide_params |= UINT64_C(1) << slot;
    }
    parser->locals[slot] =
        (c_local_t){.name = param.name,
                    .type = param.type,
                    .pointer_base_type = param.pointer_base_type,
                    .pointer_depth = param.pointer_depth,
                    .is_long_long = param.is_long_long,
                    .function_result_type = param.function_result_type,
                    .slot = (int)slot,
                    .is_const = param.is_const,
                    .pointee_const = param.pointee_const,
                    .is_volatile = param.is_volatile,
                    .pointee_volatile = param.pointee_volatile,
                    .pointee_array_size = param.pointee_array_size,
                    .pointee_array = param.pointee_array,
                    .volatile_address = -1,
                    .agg = param.agg};
  }
  if (fn->variadic) {
    parser->va_save_area =
        emit_value(parser, (nyr_inst_t){.op = NYIR_VA_SAVE,
                                        .dst = -1,
                                        .imm = 176,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
    if (parser->va_save_area < 0)
      goto fail;
  }
  for (size_t i = 0; i < fn->param_count; ++i) {
    c_param_t param = fn->params[i];
    if (!param.is_volatile)
      continue;
    size_t slot = hidden_params + i;
    c_local_t *local = &parser->locals[slot];
    local->volatile_address =
        emit_value(parser, (nyr_inst_t){.op = NYIR_ALLOCA,
                                        .dst = -1,
                                        .imm = 8,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
    int incoming = emit_value(
        parser,
        (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                     .dst = -1,
                     .imm = (int64_t)slot,
                     .scalar_class = c_type_is_float(param.type)
                                         ? c_float_scalar_class(param.type)
                                         : NYIR_SCALAR_I64_RAW});
    int value = incoming;
    if (value >= 0 && c_type_is_integer(param.type))
      value = normalize_integer(parser, param.type, value);
    if (local->volatile_address < 0 || value < 0 ||
        !lower_local_store(parser, local, value))
      goto fail;
  }
  for (size_t i = 0; i < fn->param_count; ++i) {
    if (!c_type_is_integer(fn->params[i].type) ||
        c_type_size(parser->abi, fn->params[i].type) >= 8)
      continue;
    if (fn->params[i].is_volatile)
      continue;
    size_t slot = hidden_params + i;
    if (!c_type_is_integer(fn->params[i].type))
      continue;
    int incoming =
        emit_value(parser, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                        .dst = -1,
                                        .imm = (int64_t)slot,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
    int normalized =
        incoming >= 0 ? normalize_integer(parser, fn->params[i].type, incoming)
                      : -1;
    if (normalized < 0 ||
        !emit(parser,
              (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                           .dst = -1,
                           .a = normalized,
                           .imm = (int64_t)slot,
                           .scalar_class = NYIR_SCALAR_I64_RAW},
              NULL))
      goto fail;
  }
  for (size_t i = 0; i < fn->param_count; ++i)
    if (fn->params[i].type == C_I64_PTR || fn->params[i].type == C_U64_PTR)
      out->param_types[hidden_params + i] = NYIR_PARAM_I64;
  if (fn->result == C_AGG) {
    out->return_aggregate_size = NYIR_ARG_AGG_SIZE(result_packed);
    out->return_aggregate_classes[0] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 0);
    out->return_aggregate_classes[1] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 1);
  }
  /*
   * Carry reachability across top-level statements until a label is reached.
   */
  bool returned = false;
  while (parser->pos < fn->body_end) {
    size_t start = parser->pos;
    size_t end = statement_end(parser, start);
    if (end <= start || end > fn->body_end) {
      fail(parser, "malformed function statement");
      goto fail;
    }
    if (!lower_statement_range(parser, start, end, &returned))
      goto fail;
    parser->pos = end;
  }
  out->i386_regparm = (uint8_t)c_function_regparm(parser, fn);
  bool is_void = fn->result == C_BAD;
  if (is_void && (!parser->out->len ||
                  parser->out->data[parser->out->len - 1].op != NYIR_RET)) {
    if (!lower_cleanup_range(parser, parser->param_count,
                             parser->local_count) ||
        !emit(parser,
              (nyr_inst_t){.op = NYIR_RET,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .scalar_class = NYIR_SCALAR_UNKNOWN},
              NULL))
      goto fail;
  }
  bool ends_without_fallthrough =
      parser->out->len &&
      (parser->out->data[parser->out->len - 1].op == NYIR_BR ||
       (parser->out->len >= 2 &&
        parser->out->data[parser->out->len - 1].op == NYIR_LABEL &&
        parser->out->data[parser->out->len - 2].op == NYIR_BR &&
        parser->out->data[parser->out->len - 2].imm !=
            parser->out->data[parser->out->len - 1].imm));
  bool falls_through =
      !parser->out->len ||
      (parser->out->data[parser->out->len - 1].op != NYIR_RET &&
       !ends_without_fallthrough);
  if (falls_through && !is_void && fn->result != C_AGG) {
    nyr_scalar_class_t zero_class = c_type_is_float(fn->result)
                                        ? c_float_scalar_class(fn->result)
                                        : NYIR_SCALAR_I64_RAW;
    nyr_inst_t zero = {.op = fn->result == C_F32   ? NYIR_CONST_F32
                             : fn->result == C_F64 ? NYIR_CONST_F64
                                                   : NYIR_CONST_I64,
                       .dst = -1,
                       .scalar_class = zero_class};
    int value = emit_value(parser, zero);
    if (value < 0 ||
        !lower_cleanup_range(parser, parser->param_count,
                             parser->local_count) ||
        !emit(parser,
              (nyr_inst_t){.op = NYIR_RET,
                           .dst = -1,
                           .a = value,
                           .b = -1,
                           .scalar_class = zero_class},
              NULL))
      goto fail;
  } else if (falls_through && fn->result == C_AGG) {
    snprintf(parser->err, parser->err_len,
             "%.*s: aggregate function body must return a value",
             (int)fn->name.len, fn->name.start);
    goto fail;
  }
  if (is_void)
    parser->out->data[parser->out->len - 1].scalar_class = NYIR_SCALAR_UNKNOWN;
  else if (c_type_is_float(fn->result))
    parser->out->data[parser->out->len - 1].scalar_class =
        c_float_scalar_class(fn->result);
  else if (fn->result == C_AGG)
    parser->out->data[parser->out->len - 1].scalar_class =
        out->return_aggregate_classes[0] == NYIR_ARG_CLASS_SSE
            ? NYIR_SCALAR_F64
            : NYIR_SCALAR_I64_RAW;
  else
    parser->out->data[parser->out->len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
  if (!hoist_fixed_storage(parser))
    goto fail;
  free(parser->labels);
  parser->labels = NULL;
  parser->label_count = 0;
  parser->enum_constant_count = saved_enum_constant_count;
  parser->enum_tag_count = saved_enum_tag_count;
  parser->tag_scope_id = saved_tag_scope_id;
  parser->tag_scope_depth = saved_tag_scope_depth;
  return true;
fail:
  free(parser->labels);
  parser->labels = NULL;
  parser->label_count = 0;
  parser->enum_constant_count = saved_enum_constant_count;
  parser->enum_tag_count = saved_enum_tag_count;
  parser->tag_scope_id = saved_tag_scope_id;
  parser->tag_scope_depth = saved_tag_scope_depth;
  return false;
}
