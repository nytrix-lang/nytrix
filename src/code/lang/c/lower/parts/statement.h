/* Statement and control-flow lowering, included by parse.c. */
static size_t statement_end(c_parser_t *p, size_t at) {
  if (tok_is(p, at, "{"))
    return skip_balanced(p, at, "{", "}");
  if (tok_is(p, at, "if")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    at = statement_end(p, at);
    if (tok_is(p, at, "else"))
      at = statement_end(p, at + 1);
    return at;
  }
  if (tok_is(p, at, "while")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    return statement_end(p, at);
  }
  if (tok_is(p, at, "switch")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    return statement_end(p, at);
  }
  if (tok_is(p, at, "for")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    return statement_end(p, at);
  }
  if (tok_is(p, at, "do")) {
    size_t after_body = statement_end(p, at + 1);
    if (!tok_is(p, after_body, "while"))
      return p->token_count;
    ++after_body;
    after_body = skip_balanced(p, after_body, "(", ")");
    return tok_is(p, after_body, ";") ? after_body + 1 : p->token_count;
  }
  if (at < p->token_count && tok_ident(p->tokens[at]) && tok_is(p, at + 1, ":"))
    return statement_end(p, at + 2);
  size_t parens = 0, brackets = 0, braces = 0;
  for (; at < p->token_count; ++at) {
    if (tok_is(p, at, "("))
      ++parens;
    else if (tok_is(p, at, ")")) {
      if (parens)
        --parens;
    } else if (tok_is(p, at, "["))
      ++brackets;
    else if (tok_is(p, at, "]")) {
      if (brackets)
        --brackets;
    } else if (tok_is(p, at, "{"))
      ++braces;
    else if (tok_is(p, at, "}")) {
      if (braces)
        --braces;
    } else if (tok_is(p, at, ";") && !parens && !brackets && !braces) {
      return at + 1;
    }
  }
  return at;
}

static bool c_switch_case_value(c_parser_t *p, size_t start, size_t end,
                                int64_t *out);

static bool lower_switch_in_scope(c_parser_t *p, c_cleanup_scope_t *scope,
                                  size_t limit, bool *returned) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  c_value_t condition = parse_expr(p, 0);
  if (condition.value < 0 || !expect(p, ")")) {
    fail(p, "switch requires an integer expression");
    return false;
  }
  if (!c_type_is_integer(condition.type)) {
    fail(p, "switch expression must have an integer type");
    return false;
  }
  size_t body_start = p->pos;
  size_t body_after = skip_balanced(p, body_start, "{", "}");
  if (!tok_is(p, body_start, "{") || body_after > limit ||
      body_after <= body_start + 1) {
    fail(p, "malformed switch body");
    return false;
  }
  size_t body_end = body_after - 1;
  scope->end = body_end;
  c_switch_case_t *cases = calloc(body_end - body_start, sizeof(*cases));
  if (!cases) {
    fail(p, "out of memory collecting switch labels");
    return false;
  }
  size_t case_count = 0;
  size_t default_index = SIZE_MAX;
  size_t brace_depth = 0;
  size_t paren_depth = 0;
  size_t bracket_depth = 0;
  for (size_t at = body_start + 1; at < body_end;) {
    if (!paren_depth && !bracket_depth && tok_is(p, at, "switch")) {
      size_t nested_end = statement_end(p, at);
      if (nested_end <= at || nested_end > body_end) {
        free(cases);
        fail(p, "malformed nested switch");
        return false;
      }
      at = nested_end;
      continue;
    }
    bool is_default =
        !paren_depth && !bracket_depth && tok_is(p, at, "default");
    bool is_case = !paren_depth && !bracket_depth && tok_is(p, at, "case");
    if (!is_default && !is_case) {
      if (tok_is(p, at, "("))
        ++paren_depth;
      else if (tok_is(p, at, ")") && paren_depth)
        --paren_depth;
      else if (tok_is(p, at, "["))
        ++bracket_depth;
      else if (tok_is(p, at, "]") && bracket_depth)
        --bracket_depth;
      else if (tok_is(p, at, "{"))
        ++brace_depth;
      else if (tok_is(p, at, "}") && brace_depth)
        --brace_depth;
      ++at;
      continue;
    }
    size_t expr_start = at + 1;
    size_t colon = expr_start;
    size_t range = SIZE_MAX;
    size_t nesting = 0;
    for (; colon < body_end; ++colon) {
      if (tok_is(p, colon, "(") || tok_is(p, colon, "["))
        ++nesting;
      else if ((tok_is(p, colon, ")") || tok_is(p, colon, "]")) && nesting)
        --nesting;
      else if (!nesting && tok_is(p, colon, ":"))
        break;
      else if (!nesting && tok_is(p, colon, "...") && range == SIZE_MAX)
        range = colon;
    }
    if (colon == body_end || case_count == body_end - body_start ||
        (is_default && default_index != SIZE_MAX)) {
      free(cases);
      fail(p, "malformed or duplicate switch label");
      return false;
    }
    int64_t value = 0;
    int64_t range_end = 0;
    size_t value_end = range == SIZE_MAX ? colon : range;
    if (!is_default &&
        (!c_switch_case_value(p, expr_start, value_end, &value) ||
         (range != SIZE_MAX &&
          !c_switch_case_value(p, range + 1, colon, &range_end)))) {
      free(cases);
      fail(p, "switch case must be an integer constant literal");
      return false;
    }
    if (range == SIZE_MAX)
      range_end = value;
    if (!is_default && range_end < value) {
      free(cases);
      fail(p, "switch case range has decreasing bounds");
      return false;
    }
    for (size_t i = 0; !is_default && i < case_count; ++i) {
      if (!cases[i].is_default && value <= cases[i].range_end &&
          cases[i].value <= range_end) {
        free(cases);
        fail(p, "duplicate switch case value");
        return false;
      }
    }
    size_t id = (size_t)p->next_label++;
    cases[case_count] = (c_switch_case_t){.token_pos = at,
                                          .token_after = colon + 1,
                                          .id = id,
                                          .value = value,
                                          .range_end = range_end,
                                          .is_default = is_default,
                                          .is_outer_label = brace_depth == 0};
    if (is_default)
      default_index = case_count;
    ++case_count;
    at = colon + 1;
  }
  if (p->next_label > INT_MAX - (int)case_count - 1) {
    free(cases);
    fail(p, "C control-flow label capacity exceeded");
    return false;
  }
  size_t exit_label = (size_t)p->next_label++;
  c_switch_case_t *saved_cases = p->switch_cases;
  size_t saved_case_count = p->switch_case_count;
  size_t *saved_break = p->break_target;
  size_t *saved_break_cleanup_count = p->break_cleanup_count;
  size_t break_cleanup_count = p->local_count;
  p->switch_cases = cases;
  p->switch_case_count = case_count;
  p->break_target = &exit_label;
  p->break_cleanup_count = &break_cleanup_count;

  bool ok = true;
  size_t first_outer_label = body_end;
  for (size_t i = 0; i < case_count; ++i) {
    if (cases[i].is_outer_label && cases[i].token_pos < first_outer_label)
      first_outer_label = cases[i].token_pos;
  }
  size_t prefix = body_start + 1;
  while (prefix < first_outer_label) {
    p->pos = prefix;
    size_t statement_after = statement_end(p, prefix);
    if (statement_after <= prefix || statement_after > first_outer_label) {
      fail(p, "malformed declaration before switch label");
      ok = false;
      break;
    }
    if (starts_scalar_type(p) || starts_aggregate_type(p)) {
      bool initialized = false;
      for (size_t i = prefix; i < statement_after; ++i)
        initialized |= tok_is(p, i, "=");
      if (initialized) {
        fail(p,
             "initialized declarations before switch labels are unsupported");
        ok = false;
        break;
      }
      bool declaration_returns = false;
      if (!lower_statement_range(p, prefix, statement_after,
                                 &declaration_returns)) {
        ok = false;
        break;
      }
    }
    prefix = statement_after;
  }
  p->pos = body_start + 1;

  size_t switch_ir_start = p->out->len;
  for (size_t i = 0; ok && i < case_count; ++i) {
    if (cases[i].is_default)
      continue;
    int lower = emit_value(
        p,
        (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = cases[i].value});
    int match = -1;
    if (cases[i].value == cases[i].range_end) {
      match = emit_i64_compare(p, condition.value, lower, NYIR_CMP_EQ);
    } else {
      int upper = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                             .dst = -1,
                                             .imm = cases[i].range_end});
      int above_lower =
          emit_i64_compare(p, condition.value, lower, NYIR_CMP_GE);
      int below_upper =
          emit_i64_compare(p, condition.value, upper, NYIR_CMP_LE);
      if (above_lower >= 0 && below_upper >= 0)
        match = emit_value(p, (nyr_inst_t){.op = NYIR_AND_I64,
                                           .dst = -1,
                                           .a = above_lower,
                                           .b = below_upper});
    }
    ok = lower >= 0 && match >= 0 &&
         emit_branch_if(p, match, (int64_t)cases[i].id);
  }
  size_t fallback =
      default_index == SIZE_MAX ? exit_label : cases[default_index].id;
  if (ok)
    ok = emit_branch(p, (int64_t)fallback);

  bool final_case_returns = false;
  for (size_t i = 0; ok && i < case_count; ++i) {
    if (!cases[i].is_outer_label)
      continue;
    ok = emit_label(p, (int64_t)cases[i].id);
    if (!ok)
      break;
    size_t next_outer = i + 1;
    while (next_outer < case_count && !cases[next_outer].is_outer_label)
      ++next_outer;
    size_t segment_end =
        next_outer < case_count ? cases[next_outer].token_pos : body_end;
    p->pos = cases[i].token_after;
    bool segment_returns = false;
    ok = lower_statement_range(p, p->pos, segment_end, &segment_returns);
    if (next_outer == case_count)
      final_case_returns = segment_returns;
  }
  p->break_target = saved_break;
  p->break_cleanup_count = saved_break_cleanup_count;
  p->switch_cases = saved_cases;
  p->switch_case_count = saved_case_count;
  free(cases);
  bool branches_to_exit = false;
  if (ok && default_index != SIZE_MAX && final_case_returns &&
      p->out->len > switch_ir_start &&
      p->out->data[p->out->len - 1].op == NYIR_RET) {
    for (size_t i = switch_ir_start; i < p->out->len; ++i) {
      nyr_inst_t *inst = &p->out->data[i];
      if ((inst->op == NYIR_BR || inst->op == NYIR_BR_IF) &&
          inst->imm == (int64_t)exit_label) {
        branches_to_exit = true;
        break;
      }
    }
  }
  bool switch_returns = default_index != SIZE_MAX && final_case_returns &&
                        p->out->len > switch_ir_start &&
                        p->out->data[p->out->len - 1].op == NYIR_RET &&
                        !branches_to_exit;
  c_type_t result_type = p->function->result;
  bool aggregate_switch_returns = switch_returns && result_type == C_AGG;
  if (ok && !aggregate_switch_returns)
    ok = emit_label(p, (int64_t)exit_label);
  if (ok && switch_returns) {
    if (result_type == C_BAD) {
      ok = emit(p,
                (nyr_inst_t){.op = NYIR_RET,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .scalar_class = NYIR_SCALAR_UNKNOWN},
                NULL);
    } else if (c_type_is_integer(result_type) ||
               c_type_is_pointer(result_type) || c_type_is_float(result_type)) {
      int zero = emit_value(
          p,
          (nyr_inst_t){.op = result_type == C_F32   ? NYIR_CONST_F32
                             : result_type == C_F64 ? NYIR_CONST_F64
                                                    : NYIR_CONST_I64,
                       .dst = -1,
                       .scalar_class = c_type_is_float(result_type)
                                           ? c_float_scalar_class(result_type)
                                           : NYIR_SCALAR_I64_RAW});
      if (zero < 0) {
        ok = false;
      } else {
        ok = emit(
            p,
            (nyr_inst_t){.op = NYIR_RET,
                         .dst = -1,
                         .a = zero,
                         .b = -1,
                         .scalar_class = c_type_is_float(result_type)
                                             ? c_float_scalar_class(result_type)
                                             : NYIR_SCALAR_I64_RAW},
            NULL);
      }
    } else if (aggregate_switch_returns) {
      /* All reachable switch arms already return the aggregate result. */
    } else {
      ok = false;
      fail(p, "switch with all-returning cases has an unsupported result type");
    }
  }
  if (!ok)
    return false;
  p->pos = body_after;
  *returned = switch_returns;
  return true;
}

static bool lower_switch(c_parser_t *p, size_t limit, bool *returned) {
  c_cleanup_scope_t scope;
  if (!push_tag_scope(p, &scope, limit))
    return false;
  bool ok = lower_switch_in_scope(p, &scope, limit, returned);
  pop_tag_scope(p);
  return ok;
}

static c_function_t *lower_return_noreturn_call(c_parser_t *p,
                                                size_t expression_start) {
  if (expression_start + 1 >= p->token_count ||
      !tok_ident(p->tokens[expression_start]) ||
      !tok_is(p, expression_start + 1, "("))
    return NULL;
  c_function_t *called = find_function(p, p->tokens[expression_start]);
  return called && called->no_return && p->out->len &&
                 p->out->data[p->out->len - 1].op == NYIR_CALL
             ? called
             : NULL;
}

static bool lower_return(c_parser_t *p, bool *returned) {
  if (p->function->result == C_BAD) {
    if (!accept(p, ";")) {
      size_t expression_start = p->pos;
      c_value_t value = parse_expr(p, 1);
      c_function_t *noreturn = lower_return_noreturn_call(p, expression_start);
      if (noreturn && noreturn->result == C_BAD) {
        if (!expect(p, ";"))
          return false;
        *returned = true;
        return true;
      }
      if ((p->err && p->err_len && p->err[0]) || value.type != C_BAD ||
          !expect(p, ";")) {
        fail(p, "void function return expression must have void type");
        return false;
      }
    }
    if (!lower_cleanup_range(p, 0, p->local_count) ||
        !emit(p,
              (nyr_inst_t){.op = NYIR_RET,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .scalar_class = NYIR_SCALAR_UNKNOWN},
              NULL))
      return false;
    *returned = true;
    return true;
  }
  size_t expression_start = p->pos;
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 && lower_return_noreturn_call(p, expression_start)) {
    if (!expect(p, ";"))
      return false;
    *returned = true;
    return true;
  }
  if (value.value < 0 || !expect(p, ";"))
    return false;
  c_type_t expected = p->function->result;
  if (p->function->result_is_bool &&
      (c_type_is_integer(value.type) || c_type_is_pointer(value.type) ||
       c_type_is_float(value.type))) {
    value.value = lower_boolean_value(p, value);
    if (value.value < 0)
      return false;
    value.type = expected;
  }

  if (value.type != expected && !c_pointer_compatible(value.type, expected) &&
      !(c_type_is_integer(value.type) && c_type_is_integer(expected)) &&
      !(c_type_is_pointer(expected) && p->last_value_null_pointer_constant)) {
    char message[128];
    snprintf(message, sizeof(message),
             "return type mismatch (expression=%d, function=%d)", value.type,
             expected);
    fail(p, message);
    return false;
  }
  if (expected == C_AGG) {
    /*
     * MEMORY-class results use the hidden caller buffer; register-class
     * results load their ABI pieces into RET.
     */
    const c_agg_type_t *agg = value.agg ? value.agg : p->function->result_agg;
    uint32_t packed = 0;
    if (!agg || !nyc_classify_aggregate(agg, p->abi, 0, &packed)) {
      fail(p, "aggregate return type is not supported by the C ABI");
      return false;
    }
    if (nyc_sysv_return_is_memory(packed)) {
      int hidden =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                     .dst = -1,
                                     .imm = 0,
                                     .scalar_class = NYIR_SCALAR_I64_RAW});
      if (hidden < 0)
        return false;
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                             .dst = -1,
                             .a = hidden,
                             .b = value.value,
                             .imm = (int64_t)agg->size},
                NULL) ||
          !lower_cleanup_range(p, 0, p->local_count) ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_RET,
                             .dst = -1,
                             .a = hidden,
                             .b = -1,
                             .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL))
        return false;
      *returned = true;
      return true;
    }
    unsigned class0 = NYIR_ARG_AGG_CLASS(packed, 0);
    if (class0 == NYIR_ARG_CLASS_HFA_F64) {
      if (!lower_cleanup_range(p, 0, p->local_count) ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_RET,
                             .dst = -1,
                             .a = value.value,
                             .b = -1,
                             .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL))
        return false;
      *returned = true;
      return true;
    }
    int piece0 =
        lower_load_field(p, value.value, class0 == NYIR_ARG_CLASS_SSE, false);
    if (piece0 < 0)
      return false;
    int piece1 = -1;
    unsigned class1 = NYIR_ARG_AGG_CLASS(packed, 1);
    if (class1 == NYIR_ARG_CLASS_INTEGER || class1 == NYIR_ARG_CLASS_SSE) {
      int high = lower_add_const(p, value.value, 8);
      if (high < 0)
        return false;
      piece1 = lower_load_field(p, high, class1 == NYIR_ARG_CLASS_SSE, false);
      if (piece1 < 0)
        return false;
    }
    if (!lower_cleanup_range(p, 0, p->local_count) ||
        !emit(p,
              (nyr_inst_t){.op = NYIR_RET,
                           .dst = -1,
                           .a = piece0,
                           .b = piece1,
                           .scalar_class = class0 == NYIR_ARG_CLASS_SSE
                                               ? NYIR_SCALAR_F64
                                               : NYIR_SCALAR_I64_RAW},
              NULL))
      return false;
    *returned = true;
    return true;
  }
  if (c_type_is_integer(expected) && value.type != expected) {
    value.value = normalize_integer(p, expected, value.value);
    if (value.value < 0)
      return false;
  }
  if (!lower_cleanup_range(p, 0, p->local_count) ||
      !emit(p,
            (nyr_inst_t){.op = NYIR_RET,
                         .dst = -1,
                         .a = value.value,
                         .b = -1,
                         .scalar_class = c_type_is_float(expected)
                                             ? c_float_scalar_class(expected)
                                             : NYIR_SCALAR_I64_RAW},
            NULL))
    return false;
  *returned = true;
  return true;
}

static bool lower_if_arm(c_parser_t *p, size_t body_start, size_t body_end,
                         bool is_block, bool *returned) {
  size_t local_count = p->local_count;
  size_t scope_start = p->scope_start;
  size_t enum_constant_count = p->enum_constant_count;
  size_t enum_tag_count = p->enum_tag_count;
  c_cleanup_scope_t scope;
  if (is_block) {
    p->scope_start = p->local_count;
    if (!push_tag_scope(p, &scope, body_end)) {
      p->scope_start = scope_start;
      return false;
    }
  }
  bool returns = false;
  bool ok = lower_statement_range(p, body_start, body_end, &returns);
  if (ok && is_block && !returns)
    ok = lower_cleanup_range(p, local_count, p->local_count);
  if (is_block)
    pop_tag_scope(p);
  p->enum_constant_count = enum_constant_count;
  p->enum_tag_count = enum_tag_count;
  p->scope_start = scope_start;
  p->local_count = local_count;
  if (ok)
    *returned = returns;
  return ok;
}

static bool statement_range_has_control_flow(const c_parser_t *p, size_t start,
                                             size_t end) {
  for (size_t i = start; i < end; ++i) {
    if (tok_is(p, i, "goto") || tok_is(p, i, "break") ||
        tok_is(p, i, "continue") || tok_is(p, i, ":"))
      return true;
  }
  return false;
}

static bool statement_range_has_used_static(const c_parser_t *p, size_t start,
                                            size_t end) {
  bool has_static = false;
  bool has_used = false;
  for (size_t i = start; i < end; ++i) {
    has_static |= tok_is(p, i, "static");
    has_used |= tok_is(p, i, "used") || tok_is(p, i, "__used") ||
                tok_is(p, i, "__used__");
  }
  return has_static && has_used;
}

static bool lower_if(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  c_value_t condition = parse_expr(p, 0);
  if (condition.value < 0 || !expect(p, ")"))
    return false;
  int truth = lower_truth_value(p, condition);
  size_t true_start = p->pos;
  size_t false_start = statement_end(p, true_start);
  if (false_start > limit) {
    fail(p, "if body exceeds function body");
    return false;
  }
  bool has_else = tok_is(p, false_start, "else");
  size_t true_end = false_start;
  size_t true_body = true_start;
  bool true_is_block = tok_is(p, true_body, "{");
  if (true_is_block) {
    ++true_body;
    --true_end;
  }
  size_t false_body = false_start + 1;
  size_t false_end = false_start;
  size_t after_if = false_start;
  bool false_is_block = false;
  if (has_else) {
    false_end = statement_end(p, false_body);
    if (false_end > limit) {
      fail(p, "else body exceeds function body");
      return false;
    }
    after_if = false_end;
    false_is_block = tok_is(p, false_body, "{");
    if (false_is_block) {
      ++false_body;
      --false_end;
    }
  }
  int64_t condition_constant = 0;
  bool condition_is_constant =
      c_ir_boolean_constant(p, truth, &condition_constant);
  bool dead_arm_has_used_static =
      condition_is_constant &&
      (condition_constant
           ? has_else &&
                 statement_range_has_used_static(p, false_body, false_end)
           : statement_range_has_used_static(p, true_body, true_end));
  bool true_has_control_flow =
      statement_range_has_control_flow(p, true_body, true_end);
  bool false_has_control_flow =
      has_else && statement_range_has_control_flow(p, false_body, false_end);
  if (condition_is_constant && !dead_arm_has_used_static &&
      !true_has_control_flow && !false_has_control_flow) {
    size_t dead_body = condition_constant ? false_body : true_body;
    size_t dead_end = condition_constant ? false_end : true_end;
    bool dead_is_block = condition_constant ? false_is_block : true_is_block;
    bool has_dead_arm = condition_constant ? has_else : true;
    if (has_dead_arm) {
      c_parser_t probe = *p;
      nyr_func_t probe_out;
      expression_probe_begin(&probe, &probe_out);
      bool probe_returns = false;
      bool probe_ok = lower_if_arm(&probe, dead_body, dead_end, dead_is_block,
                                   &probe_returns);
      expression_probe_end(&probe);
      if (!probe_ok)
        return false;
    }
    bool has_selected_arm = condition_constant || has_else;
    bool selected_returns = false;
    if (has_selected_arm) {
      size_t selected_body = condition_constant ? true_body : false_body;
      size_t selected_end = condition_constant ? true_end : false_end;
      bool selected_is_block =
          condition_constant ? true_is_block : false_is_block;
      if (!lower_if_arm(p, selected_body, selected_end, selected_is_block,
                        &selected_returns))
        return false;
    }
    *returned = selected_returns;
    p->pos = after_if;
    return true;
  }
  int true_label = p->next_label++;
  int false_label = p->next_label++;
  int merge_label = p->next_label++;
  if (!emit_branch_if(p, truth, true_label))
    return false;
  if (has_else) {
    if (!emit_branch(p, false_label))
      return false;
  } else if (!emit_branch(p, merge_label)) {
    return false;
  }
  if (!emit_label(p, true_label))
    return false;
  size_t true_local_count = p->local_count;
  size_t true_scope_start = p->scope_start;
  size_t true_enum_constant_count = p->enum_constant_count;
  size_t true_enum_tag_count = p->enum_tag_count;
  c_cleanup_scope_t true_scope;
  if (true_is_block) {
    p->scope_start = p->local_count;
    if (!push_tag_scope(p, &true_scope, true_end)) {
      p->scope_start = true_scope_start;
      return false;
    }
  }
  bool true_returns = false;
  if (!lower_statement_range(p, true_body, true_end, &true_returns)) {
    if (true_is_block)
      pop_tag_scope(p);
    p->enum_constant_count = true_enum_constant_count;
    p->enum_tag_count = true_enum_tag_count;
    p->scope_start = true_scope_start;
    return false;
  }
  if (true_is_block && !true_returns &&
      !lower_cleanup_range(p, true_local_count, p->local_count)) {
    pop_tag_scope(p);
    p->enum_constant_count = true_enum_constant_count;
    p->enum_tag_count = true_enum_tag_count;
    p->scope_start = true_scope_start;
    return false;
  }
  if (true_is_block)
    pop_tag_scope(p);
  p->enum_constant_count = true_enum_constant_count;
  p->enum_tag_count = true_enum_tag_count;
  p->scope_start = true_scope_start;
  p->local_count = true_local_count;
  if (!true_returns && !emit_branch(p, merge_label))
    return false;
  if (!has_else) {
    if (!emit_label(p, merge_label))
      return false;
    p->pos = false_start;
    *returned = false;
    return true;
  }

  if (!emit_label(p, false_label))
    return false;
  size_t false_local_count = p->local_count;
  size_t false_scope_start = p->scope_start;
  size_t false_enum_constant_count = p->enum_constant_count;
  size_t false_enum_tag_count = p->enum_tag_count;
  c_cleanup_scope_t false_scope;
  if (false_is_block) {
    p->scope_start = p->local_count;
    if (!push_tag_scope(p, &false_scope, false_end)) {
      p->scope_start = false_scope_start;
      return false;
    }
  }
  bool false_returns = false;
  if (!lower_statement_range(p, false_body, false_end, &false_returns)) {
    if (false_is_block)
      pop_tag_scope(p);
    p->enum_constant_count = false_enum_constant_count;
    p->enum_tag_count = false_enum_tag_count;
    p->scope_start = false_scope_start;
    return false;
  }
  if (false_is_block && !false_returns &&
      !lower_cleanup_range(p, false_local_count, p->local_count)) {
    pop_tag_scope(p);
    p->enum_constant_count = false_enum_constant_count;
    p->enum_tag_count = false_enum_tag_count;
    p->scope_start = false_scope_start;
    return false;
  }
  if (false_is_block)
    pop_tag_scope(p);
  p->enum_constant_count = false_enum_constant_count;
  p->enum_tag_count = false_enum_tag_count;
  p->scope_start = false_scope_start;
  p->local_count = false_local_count;
  if (!false_returns && !emit_branch(p, merge_label))
    return false;
  if (!true_returns || !false_returns) {
    if (!emit_label(p, merge_label))
      return false;
  }
  p->pos = after_if;
  *returned = true_returns && false_returns;
  return true;
}

static bool lower_while(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  size_t header_label = (size_t)p->next_label++;
  size_t body_label = (size_t)p->next_label++;
  size_t exit_label = (size_t)p->next_label++;
  if (!emit_label(p, (int64_t)header_label))
    return false;
  c_value_t condition = parse_expr(p, 0);
  if (condition.value < 0 || !expect(p, ")"))
    return false;
  int truth = lower_truth_value(p, condition);
  size_t body_start = p->pos;
  size_t after_body = statement_end(p, body_start);
  if (after_body > limit || after_body <= body_start) {
    fail(p, "malformed while-loop body");
    return false;
  }
  size_t body_end = after_body;
  bool body_is_block = tok_is(p, body_start, "{");
  if (body_is_block) {
    ++body_start;
    --body_end;
  }
  if (!emit_branch_if(p, truth, (int64_t)body_label) ||
      !emit_branch(p, (int64_t)exit_label) ||
      !emit_label(p, (int64_t)body_label))
    return false;
  size_t saved_local_count = p->local_count;
  size_t saved_scope_start = p->scope_start;
  c_cleanup_scope_t body_scope;
  if (body_is_block) {
    p->scope_start = p->local_count;
    if (!push_tag_scope(p, &body_scope, body_end)) {
      p->scope_start = saved_scope_start;
      return false;
    }
  }
  size_t *saved_break_target = p->break_target;
  size_t *saved_continue_target = p->continue_target;
  size_t *saved_break_cleanup_count = p->break_cleanup_count;
  size_t *saved_continue_cleanup_count = p->continue_cleanup_count;
  size_t loop_cleanup_count = saved_local_count;
  p->break_target = &exit_label;
  p->continue_target = &header_label;
  p->break_cleanup_count = &loop_cleanup_count;
  p->continue_cleanup_count = &loop_cleanup_count;
  bool body_returns = false;
  if (!lower_statement_range(p, body_start, body_end, &body_returns)) {
    if (body_is_block)
      pop_tag_scope(p);
    p->break_target = saved_break_target;
    p->continue_target = saved_continue_target;
    p->break_cleanup_count = saved_break_cleanup_count;
    p->continue_cleanup_count = saved_continue_cleanup_count;
    p->scope_start = saved_scope_start;
    return false;
  }
  p->break_target = saved_break_target;
  p->continue_target = saved_continue_target;
  p->break_cleanup_count = saved_break_cleanup_count;
  p->continue_cleanup_count = saved_continue_cleanup_count;
  if (body_is_block && !body_returns &&
      !lower_cleanup_range(p, saved_local_count, p->local_count)) {
    pop_tag_scope(p);
    p->scope_start = saved_scope_start;
    return false;
  }
  if (body_is_block)
    pop_tag_scope(p);
  p->scope_start = saved_scope_start;
  p->local_count = saved_local_count;
  if (!emit_branch(p, (int64_t)header_label) ||
      !emit_label(p, (int64_t)exit_label))
    return false;
  p->pos = after_body;
  *returned = false;
  return true;
}

static bool lower_do_while(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (p->next_label > INT_MAX - 3) {
    fail(p, "C control-flow label capacity exceeded");
    return false;
  }
  size_t body_label = (size_t)p->next_label++;
  size_t condition_label = (size_t)p->next_label++;
  size_t exit_label = (size_t)p->next_label++;
  if (!emit_label(p, (int64_t)body_label))
    return false;
  size_t body_start = p->pos;
  size_t after_body = statement_end(p, body_start);
  if (after_body > limit || after_body <= body_start ||
      !tok_is(p, after_body, "while")) {
    fail(p, "malformed do-while body or missing trailing while condition");
    return false;
  }
  size_t body_end = after_body;
  bool body_is_block = tok_is(p, body_start, "{");
  if (body_is_block) {
    ++body_start;
    --body_end;
  }
  size_t saved_local_count = p->local_count;
  size_t saved_scope_start = p->scope_start;
  size_t body_ir_start = p->out->len;
  c_cleanup_scope_t body_scope;
  if (body_is_block) {
    p->scope_start = p->local_count;
    if (!push_tag_scope(p, &body_scope, body_end)) {
      p->scope_start = saved_scope_start;
      return false;
    }
  }
  size_t *saved_break_target = p->break_target;
  size_t *saved_continue_target = p->continue_target;
  size_t *saved_break_cleanup_count = p->break_cleanup_count;
  size_t *saved_continue_cleanup_count = p->continue_cleanup_count;
  size_t loop_cleanup_count = saved_local_count;
  p->break_target = &exit_label;
  p->continue_target = &condition_label;
  p->break_cleanup_count = &loop_cleanup_count;
  p->continue_cleanup_count = &loop_cleanup_count;
  bool body_returns = false;
  if (!lower_statement_range(p, body_start, body_end, &body_returns)) {
    if (body_is_block)
      pop_tag_scope(p);
    p->break_target = saved_break_target;
    p->continue_target = saved_continue_target;
    p->break_cleanup_count = saved_break_cleanup_count;
    p->continue_cleanup_count = saved_continue_cleanup_count;
    p->scope_start = saved_scope_start;
    return false;
  }
  p->break_target = saved_break_target;
  p->continue_target = saved_continue_target;
  p->break_cleanup_count = saved_break_cleanup_count;
  p->continue_cleanup_count = saved_continue_cleanup_count;
  if (body_is_block && !body_returns &&
      !lower_cleanup_range(p, saved_local_count, p->local_count)) {
    pop_tag_scope(p);
    p->scope_start = saved_scope_start;
    return false;
  }
  if (body_is_block)
    pop_tag_scope(p);
  p->scope_start = saved_scope_start;
  p->local_count = saved_local_count;
  bool body_branches_to_condition = false;
  bool body_branches_to_exit = false;
  for (size_t i = body_ir_start; i < p->out->len; ++i) {
    const nyr_inst_t *in = &p->out->data[i];
    if (in->op != NYIR_BR && in->op != NYIR_BR_IF)
      continue;
    body_branches_to_condition |= in->imm == (int64_t)condition_label;
    body_branches_to_exit |= in->imm == (int64_t)exit_label;
  }
  if (body_returns && !body_branches_to_condition) {
    p->pos = after_body + 1;
    if (!expect(p, "("))
      return false;
    c_parser_t condition_probe = *p;
    nyr_func_t condition_out;
    expression_probe_begin(&condition_probe, &condition_out);
    c_value_t condition = parse_expr(&condition_probe, 0);
    bool parsed = condition.value >= 0 && expect(&condition_probe, ")") &&
                  expect(&condition_probe, ";");
    size_t after_condition = condition_probe.pos;
    expression_probe_end(&condition_probe);
    if (!parsed)
      return false;
    p->pos = after_condition;
    if (body_branches_to_exit) {
      if (!emit_label(p, (int64_t)exit_label))
        return false;
      *returned = false;
    } else {
      *returned = true;
    }
    return true;
  }
  if (!emit_label(p, (int64_t)condition_label))
    return false;
  p->pos = after_body + 1;
  if (!expect(p, "("))
    return false;
  c_value_t condition = parse_expr(p, 0);
  if (condition.value < 0 || !expect(p, ")") || !expect(p, ";"))
    return false;
  int truth = lower_truth_value(p, condition);
  if (truth < 0)
    return false;
  int64_t condition_constant = 0;
  bool unconditional_loop =
      c_ir_nonnegative_constant(p, truth, &condition_constant) &&
      condition_constant != 0;
  if (unconditional_loop) {
    for (size_t i = body_ir_start; i < p->out->len; ++i) {
      const nyr_inst_t *inst = &p->out->data[i];
      if (inst->op == NYIR_BR && inst->imm == (int64_t)exit_label) {
        unconditional_loop = false;
        break;
      }
    }
  }
  if (unconditional_loop) {
    if (!emit_branch(p, (int64_t)body_label))
      return false;
    *returned = false;
    return true;
  }
  if (!emit_branch_if(p, truth, (int64_t)body_label) ||
      !emit_branch(p, (int64_t)exit_label) ||
      !emit_label(p, (int64_t)exit_label))
    return false;
  *returned = false;
  return true;
}

static bool lower_for_in_scope(c_parser_t *p, size_t body_start,
                               size_t after_body, bool *returned) {
  size_t saved_local_count = p->local_count;
  size_t saved_scope_start = p->scope_start;
  p->scope_start = saved_local_count;
  if (accept(p, ";")) {
    /*
     * Empty initializer.
     */
  } else {
    size_t auto_type_pos = p->pos;
    bool auto_type_const = false;
    bool auto_type_volatile = false;
    while (tok_is(p, auto_type_pos, "const") ||
           tok_is(p, auto_type_pos, "volatile")) {
      auto_type_const |= tok_is(p, auto_type_pos, "const");
      auto_type_volatile |= tok_is(p, auto_type_pos, "volatile");
      ++auto_type_pos;
    }
    if (tok_is(p, auto_type_pos, "__auto_type") ||
        tok_is(p, auto_type_pos, "auto")) {
      p->pos = auto_type_pos + 1;
      ny_ctok_t name;
      if (!parse_name(p, &name))
        return false;
      ny_ctok_t local_section = {0};
      ny_ctok_t local_cleanup = {0};
      if (!parse_trailing_local_attributes(p, &local_section, &local_cleanup) ||
          local_section.len || !expect(p, "="))
        return false;
      c_value_t initial = parse_expr(p, 1);
      if (initial.value < 0 || initial.type == C_BAD || initial.type == C_AGG ||
          (!c_type_is_integer(initial.type) && !c_type_is_float(initial.type) &&
           !c_type_is_pointer(initial.type))) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "__auto_type initializer must have a scalar type");
        return false;
      }
      if (!add_local(p, name, initial.type, initial.value, auto_type_const,
                     auto_type_volatile))
        return false;
      p->locals[p->local_count - 1].cleanup = local_cleanup;
      p->locals[p->local_count - 1].agg = (c_agg_type_t *)initial.agg;
      if (!expect(p, ";"))
        return false;
    } else if (starts_scalar_type(p) || starts_aggregate_type(p)) {
      size_t declaration_start = p->pos;
      c_type_t type;
      if (!parse_type(p, &type, false))
        return false;
      if (type == C_AGG) {
        p->pos = declaration_start;
        if (!lower_aggregate_declaration(p, false) || !expect(p, ";"))
          return false;
      } else if ((p->last_type_const && !c_type_is_pointer(type)) ||
                 (!c_type_is_integer(type) && !c_type_is_float(type) &&
                  !c_type_is_pointer(type))) {
        fail(p, "for-loop initializer must be a mutable scalar");
        return false;
      } else {
        bool is_volatile = p->last_type_volatile;
        c_agg_type_t *decl_agg = p->last_type_agg;
        c_type_t decl_pointer_base_type = p->last_type_base_type;
        unsigned decl_pointer_depth = p->last_type_pointer_count;
        c_array_shape_t decl_pointee_array = p->last_type_pointee_array;
        bool declaration_ok = true;
        do {
          ny_ctok_t name;
          if (!parse_parenthesized_declarator_name(p, &name)) {
            declaration_ok = false;
            break;
          }
          ny_ctok_t local_section = {0};
          ny_ctok_t local_cleanup = {0};
          if (!parse_trailing_local_attributes(p, &local_section,
                                               &local_cleanup) ||
              local_section.len) {
            declaration_ok = false;
            break;
          }
          int initial_value = -1;
          if (accept(p, "=")) {
            c_value_t initial = parse_expr(p, 1);
            if (initial.value < 0 ||
                !(initial.type == type ||
                  (c_type_is_integer(initial.type) &&
                   c_type_is_integer(type)) ||
                  (c_type_is_pointer(initial.type) &&
                   c_pointer_compatible(initial.type, type)) ||
                  (c_type_is_pointer(type) && c_type_is_integer(initial.type) &&
                   p->last_value_null_pointer_constant))) {
              if (initial.value >= 0)
                fail(p, "for-loop initializer type must match its declaration");
              declaration_ok = false;
              break;
            }
            initial_value = initial.value;
          }
          if (!add_local(p, name, type, initial_value, false, is_volatile)) {
            declaration_ok = false;
            break;
          }
          c_local_t *local = &p->locals[p->local_count - 1];
          local->agg = decl_agg;
          local->pointer_base_type = decl_pointer_base_type;
          local->pointer_depth = decl_pointer_depth;
          local->pointee_array = decl_pointee_array;
          local->cleanup = local_cleanup;
        } while (accept(p, ","));
        if (!declaration_ok || !expect(p, ";"))
          return false;
      }
    } else {
      c_value_t ignored = parse_expr(p, 0);
      if ((ignored.value < 0 && p->err && p->err_len && p->err[0]) ||
          !expect(p, ";"))
        return false;
    }
  }

  size_t header_label = (size_t)p->next_label++;
  size_t body_label = (size_t)p->next_label++;
  size_t step_label = (size_t)p->next_label++;
  size_t exit_label = (size_t)p->next_label++;
  if (!emit_label(p, (int64_t)header_label))
    return false;
  c_value_t condition;
  if (tok_is(p, p->pos, ";")) {
    int value =
        emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 1});
    condition = (c_value_t){value, C_I64, NULL};
  } else {
    condition = parse_expr(p, 0);
  }
  if (condition.value < 0 || !expect(p, ";"))
    return false;
  int truth = lower_truth_value(p, condition);

  size_t step_start = p->pos;
  size_t step_end = body_start - 1;
  if (step_start > step_end) {
    fail(p, "malformed for-loop header");
    return false;
  }
  p->pos = body_start;
  size_t body_end = after_body;
  bool body_is_block = tok_is(p, body_start, "{");
  if (body_is_block) {
    ++body_start;
    --body_end;
  }
  if (!emit_branch_if(p, truth, (int64_t)body_label) ||
      !emit_branch(p, (int64_t)exit_label) ||
      !emit_label(p, (int64_t)body_label))
    return false;
  size_t body_local_count = p->local_count;
  size_t body_scope_start = p->scope_start;
  c_cleanup_scope_t body_scope;
  if (body_is_block) {
    p->scope_start = p->local_count;
    if (!push_tag_scope(p, &body_scope, body_end)) {
      p->scope_start = body_scope_start;
      return false;
    }
  }
  size_t *saved_break_target = p->break_target;
  size_t *saved_continue_target = p->continue_target;
  size_t *saved_break_cleanup_count = p->break_cleanup_count;
  size_t *saved_continue_cleanup_count = p->continue_cleanup_count;
  /*
   * The for-initializer lives until the loop exit label. Break and continue
   * clean only the body; cleaning the initializer there as well runs its
   * cleanup attribute a second time when control reaches that label.
   */
  size_t break_cleanup_count = body_local_count;
  size_t continue_cleanup_count = body_local_count;
  p->break_target = &exit_label;
  p->continue_target = &step_label;
  p->break_cleanup_count = &break_cleanup_count;
  p->continue_cleanup_count = &continue_cleanup_count;
  bool body_returns = false;
  if (!lower_statement_range(p, body_start, body_end, &body_returns)) {
    if (body_is_block)
      pop_tag_scope(p);
    p->break_target = saved_break_target;
    p->continue_target = saved_continue_target;
    p->break_cleanup_count = saved_break_cleanup_count;
    p->continue_cleanup_count = saved_continue_cleanup_count;
    p->scope_start = body_scope_start;
    return false;
  }
  p->break_target = saved_break_target;
  p->continue_target = saved_continue_target;
  p->break_cleanup_count = saved_break_cleanup_count;
  p->continue_cleanup_count = saved_continue_cleanup_count;
  if (body_is_block && !body_returns &&
      !lower_cleanup_range(p, body_local_count, p->local_count)) {
    pop_tag_scope(p);
    p->scope_start = body_scope_start;
    return false;
  }
  if (body_is_block)
    pop_tag_scope(p);
  p->scope_start = body_scope_start;
  p->local_count = body_local_count;
  if (!emit_label(p, (int64_t)step_label))
    return false;
  p->pos = step_start;
  while (p->pos < step_end) {
    c_value_t ignored = parse_expr(p, 1);
    if (ignored.value < 0 && p->err && p->err_len && p->err[0])
      return false;
    if (p->pos < step_end) {
      if (!accept(p, ",") || p->pos == step_end) {
        fail(p, "for-loop step contains an unsupported expression");
        return false;
      }
    }
  }
  if (p->pos != step_end) {
    fail(p, "for-loop step contains an unsupported expression");
    return false;
  }
  if (!emit_branch(p, (int64_t)header_label) ||
      !emit_label(p, (int64_t)exit_label) ||
      !lower_cleanup_range(p, saved_local_count, p->local_count))
    return false;
  p->local_count = saved_local_count;
  p->scope_start = saved_scope_start;
  p->pos = after_body;
  *returned = false;
  return true;
}

static bool lower_for(c_parser_t *p, size_t limit, bool *returned) {
  size_t header = ++p->pos;
  if (!expect(p, "("))
    return false;
  size_t body = skip_balanced(p, header, "(", ")");
  if (body <= header + 1 || body >= limit) {
    fail(p, "unterminated for-loop header or missing body");
    return false;
  }
  size_t after_body = statement_end(p, body);
  if (after_body > limit || after_body <= body) {
    fail(p, "malformed for-loop body");
    return false;
  }
  c_cleanup_scope_t scope;
  enter_cleanup_scope(p, &scope, after_body);
  bool ok = lower_for_in_scope(p, body, after_body, returned);
  p->cleanup_scope = scope.parent;
  return ok;
}

static bool lower_statement_range(c_parser_t *p, size_t start, size_t end,
                                  bool *returned) {
  size_t saved_pos = p->pos;
  p->pos = start;
  while (p->pos < end) {
    if (accept(p, ";"))
      continue;
    if (accept(p, "_Pragma")) {
      if (!expect(p, "(") || current(p)->kind != NY_CTOK_STRING) {
        fail(p, "_Pragma requires a string-literal argument");
        break;
      }
      ny_ctok_t pragma = *current(p);
      static const char unroll_prefix[] = "\"GCC unroll ";
      if (pragma.len <= sizeof(unroll_prefix) - 1 ||
          memcmp(pragma.start, unroll_prefix, sizeof(unroll_prefix) - 1) != 0 ||
          pragma.start[pragma.len - 1] != '"') {
        fail(p, "unsupported function-scope _Pragma directive");
        break;
      }
      ++p->pos;
      if (!expect(p, ")"))
        break;
      continue;
    }
    if (accept(p, "_Static_assert")) {
      int64_t condition = 0;
      size_t sa_pos = p->pos;
      if (!expect(p, "(") || !parse_global_const_expr(p, &p->pos, &condition) ||
          !expect(p, ",")) {
        if (getenv("NYC_KERNEL_COMPAT")) {
          if (p->err)
            p->err[0] = 0;
          p->pos = sa_pos;
          int depth = 0;
          bool in_sa = false;
          while (p->pos < p->token_count) {
            if (tok_is(p, p->pos, "(")) {
              ++depth;
              in_sa = true;
            } else if (tok_is(p, p->pos, ")")) {
              if (--depth <= 0 && in_sa) {
                ++p->pos;
                break;
              }
            } else if (depth <= 0 && in_sa && tok_is(p, p->pos, ";"))
              break;
            ++p->pos;
          }
          (void)accept(p, ";");
          continue;
        }
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "invalid block _Static_assert constant expression");
        break;
      }
      bool message_found = false;
      while (current(p)->kind == NY_CTOK_STRING) {
        message_found = true;
        ++p->pos;
      }
      if (!message_found || !expect(p, ")") || !expect(p, ";")) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "invalid block _Static_assert diagnostic string");
        break;
      }
      if (!condition && !getenv("NYC_KERNEL_COMPAT")) {
        fail(p, "block _Static_assert condition is false");
        break;
      }
      continue;
    }
    if (*returned) {
      size_t label_pos = p->pos;
      while (label_pos + 1 < end) {
        bool known_label = false;
        if (tok_ident(p->tokens[label_pos]) && tok_is(p, label_pos + 1, ":")) {
          for (size_t i = 0; i < p->label_count; ++i) {
            if (tok_same(p->labels[i].name, p->tokens[label_pos])) {
              known_label = true;
              break;
            }
          }
        }
        for (size_t i = 0; !known_label && i < p->switch_case_count; ++i) {
          if (p->switch_cases[i].token_pos == label_pos) {
            known_label = true;
            break;
          }
        }
        if (known_label) {
          /*
           * Re-enter at the enclosing statement, not at a nested label.
           * Loops and switches establish control-flow targets while lowering
           * their bodies; skipping directly to a label loses those targets.
           */
          *returned = false;
          break;
        }
        ++label_pos;
      }
      if (*returned) {
        p->pos = end;
        break;
      }
    }
    if (accept(p, ";"))
      continue;
    if (tok_is(p, p->pos, "asm") || tok_is(p, p->pos, "__asm") ||
        tok_is(p, p->pos, "__asm__")) {
      size_t asm_ir_start = p->out->len;
      if (!lower_inline_asm_statement(p)) {
        /*
         * Some GNU asm paths finish lowering their outputs before reaching a
         * trailing control-flow statement. Keep that statement when the IR
         * effect was emitted and no diagnostic was recorded.
         */
        if ((!p->err || !p->err_len || !p->err[0]) &&
            p->out->len > asm_ir_start && p->pos < end &&
            tok_is(p, p->pos, "break") && p->break_target)
          continue;
        if ((!p->err || !p->err_len || !p->err[0]) && p->pos < end) {
          char message[128];
          ny_ctok_t at = *current(p);
          snprintf(
              message, sizeof(message),
              "inline assembly lowering stopped before `%.*s` at token %zu",
              (int)(at.len > 48 ? 48 : at.len), at.start ? at.start : "",
              p->pos);
          fail(p, message);
        }
        break;
      }
      continue;
    }
    if (tok_is(p, p->pos, "case") || tok_is(p, p->pos, "default")) {
      size_t label_pos = p->pos;
      c_switch_case_t *label = NULL;
      for (size_t i = 0; i < p->switch_case_count; ++i) {
        if (p->switch_cases[i].token_pos == label_pos) {
          label = &p->switch_cases[i];
          break;
        }
      }
      if (!label || !emit_label(p, (int64_t)label->id))
        break;
      p->pos = label->token_after;
      continue;
    }
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, ":")) {
      ny_ctok_t name = *current(p);
      c_label_t *label = find_label_at(p, name, p->pos);
      if (!label) {
        fail(p, "could not resolve C label");
        break;
      }
      p->pos += 2;
      if (!parse_function_type_attributes(p))
        break;
      if (!emit_label(p, (int64_t)label->id))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "{")) {
      size_t block_start = p->pos;
      size_t block_after = skip_balanced(p, block_start, "{", "}");
      if (block_after > end || block_after <= block_start + 1) {
        fail(p, "malformed compound statement");
        break;
      }
      size_t saved_local_count = p->local_count;
      size_t saved_scope_start = p->scope_start;
      size_t saved_enum_constant_count = p->enum_constant_count;
      size_t saved_enum_tag_count = p->enum_tag_count;
      p->scope_start = p->local_count;
      size_t saved_typedef_count = p->typedef_count;
      bool block_returns = false;
      c_cleanup_scope_t scope;
      if (!push_tag_scope(p, &scope, block_after - 1)) {
        p->scope_start = saved_scope_start;
        break;
      }
      if (!lower_statement_range(p, block_start + 1, block_after - 1,
                                 &block_returns)) {
        pop_tag_scope(p);
        p->enum_constant_count = saved_enum_constant_count;
        p->enum_tag_count = saved_enum_tag_count;
        p->typedef_count = saved_typedef_count;
        typedef_index_invalidate(p);
        p->scope_start = saved_scope_start;
        break;
      }
      if (!block_returns &&
          !lower_cleanup_range(p, saved_local_count, p->local_count)) {
        pop_tag_scope(p);
        break;
      }
      pop_tag_scope(p);
      p->enum_constant_count = saved_enum_constant_count;
      p->enum_tag_count = saved_enum_tag_count;
      p->typedef_count = saved_typedef_count;
      typedef_index_invalidate(p);
      p->local_count = saved_local_count;
      p->scope_start = saved_scope_start;
      p->pos = block_after;
      *returned = block_returns;
      continue;
    }
    if (accept(p, "__label__")) {
      do {
        ny_ctok_t local_label;
        if (!parse_name(p, &local_label)) {
          fail(p, "GNU local label declaration requires label names");
          break;
        }
      } while (accept(p, ","));
      if (p->err && p->err_len && p->err[0])
        break;
      if (!expect(p, ";"))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "enum")) {
      size_t body = p->pos + 1;
      if (tok_ident(p->tokens[body]))
        ++body;
      if (tok_is(p, body, "{")) {
        size_t after = skip_balanced(p, body, "{", "}");
        if (after < p->token_count && tok_is(p, after, ";")) {
          c_type_t enum_type;
          if (!parse_type(p, &enum_type, true) || !expect(p, ";"))
            break;
          continue;
        }
      }
    }
    if (accept(p, "return")) {
      if (!lower_return(p, returned))
        break;
      continue;
    }
    if (accept(p, "break")) {
      if (!p->break_target) {
        fail(p, "break statement is not inside a loop");
        break;
      }
      if (!expect(p, ";") || !p->break_cleanup_count ||
          !lower_cleanup_range(p, *p->break_cleanup_count, p->local_count) ||
          !emit_branch(p, (int64_t)*p->break_target)) {
        if ((!p->err || !p->err_len || !p->err[0]))
          fail(p, "failed to lower break statement");
        break;
      }
      *returned = true;
      continue;
    }
    if (accept(p, "continue")) {
      if (!p->continue_target) {
        fail(p, "continue statement is not inside a loop");
        break;
      }
      if (!expect(p, ";") || !p->continue_cleanup_count ||
          !lower_cleanup_range(p, *p->continue_cleanup_count, p->local_count) ||
          !emit_branch(p, (int64_t)*p->continue_target))
        break;
      *returned = true;
      continue;
    }
    if (accept(p, "goto")) {
      size_t goto_pos = p->pos;
      if (accept(p, "*")) {
        c_value_t target = parse_expr(p, 1);
        if (target.value < 0 || !c_type_is_pointer(target.type) ||
            !expect(p, ";")) {
          fail(p, "computed goto requires a pointer expression");
          break;
        }
        bool ok = true;
        for (size_t i = 0; i < p->label_count && ok; ++i) {
          c_label_t *label = &p->labels[i];
          if (p->function->name.len > INT_MAX ||
              p->function->name.len > SIZE_MAX - 32) {
            fail(p, "computed-goto label symbol is too long");
            ok = false;
            break;
          }
          size_t symbol_capacity = p->function->name.len + 32;
          char *symbol = malloc(symbol_capacity);
          if (!symbol) {
            fail(p, "out of memory lowering computed goto");
            ok = false;
            break;
          }
          int symbol_len = snprintf(symbol, symbol_capacity, ".Lnyir_%.*s_L%zu",
                                    (int)p->function->name.len,
                                    p->function->name.start, label->id);
          if (symbol_len < 0 || (size_t)symbol_len >= symbol_capacity) {
            free(symbol);
            fail(p, "computed-goto label symbol is too long");
            ok = false;
            break;
          }
          int address =
              emit_value(p, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                         .dst = -1,
                                         .symbol = symbol,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          free(symbol);
          int matches = address < 0 ? -1
                                    : emit_i64_compare(p, target.value, address,
                                                       NYIR_CMP_EQ);
          int edge = matches >= 0 ? lower_cleanup_edge(p, label) : -1;
          ok = edge >= 0 && emit_branch_if(p, matches, edge);
        }
        if (!ok)
          break;
        if (p->next_label == INT_MAX) {
          fail(p, "C function has too many control-flow labels");
          break;
        }
        int64_t invalid_target = (int64_t)p->next_label++;
        if (!emit_branch(p, invalid_target) || !emit_label(p, invalid_target) ||
            !emit_branch(p, invalid_target) || p->next_label == INT_MAX ||
            !emit_label(p, p->next_label++)) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "failed to lower computed goto");
          break;
        }
        continue;
      }
      ny_ctok_t name;
      if (!parse_name(p, &name) || !expect(p, ";"))
        break;
      c_label_t *label = find_label_at(p, name, goto_pos);
      if (!label) {
        fail(p, "goto target is not defined in this function");
        break;
      }
      int edge = lower_cleanup_edge(p, label);
      if (edge < 0 || !emit_branch(p, edge))
        break;
      /*
       * Keep following source statements in a separate, unreachable block.
       * A later label can still be reached by another branch.
       */
      if (p->next_label == INT_MAX) {
        fail(p, "C function has too many control-flow labels");
        break;
      }
      if (!emit_label(p, p->next_label++))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "if")) {
      if (!lower_if(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "switch")) {
      if (!lower_switch(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "while")) {
      if (!lower_while(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "do")) {
      if (!lower_do_while(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "for")) {
      if (!lower_for(p, end, returned))
        break;
      continue;
    }
    if (tok_is_attribute(p, p->pos) &&
        (tok_is(p, p->pos + 3, "fallthrough") ||
         tok_is(p, p->pos + 3, "__fallthrough__"))) {
      ++p->pos;
      if (!expect(p, "(") || !expect(p, "(") ||
          !(accept(p, "fallthrough") || accept(p, "__fallthrough__")) ||
          !expect(p, ")") || !expect(p, ")") || !expect(p, ";")) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "malformed fallthrough statement attribute");
        break;
      }
      continue;
    }
    if (accept(p, "typedef")) {
      c_type_t alias_type;
      if (!parse_type(p, &alias_type, true))
        break;
      c_type_t function_result_type = alias_type;
      c_agg_type_t *function_result_agg = alias_type == C_AGG ||
                                                  alias_type == C_AGG_PTR ||
                                                  alias_type == C_AGG_PTR_PTR
                                              ? p->last_type_agg
                                              : NULL;
      bool function_pointer =
          p->pos + 4 < p->token_count && tok_is(p, p->pos, "(") &&
          tok_is(p, p->pos + 1, "*") && tok_ident(p->tokens[p->pos + 2]) &&
          tok_is(p, p->pos + 3, ")") && tok_is(p, p->pos + 4, "(");
      ny_ctok_t name;
      if (function_pointer) {
        if (!parse_function_pointer_declarator(p, &name) || !expect(p, ";")) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "unterminated local function-pointer typedef");
          break;
        }
        alias_type = C_FUNC_PTR;
      } else if (!parse_name(p, &name) || !expect(p, ";")) {
        break;
      }
      c_agg_type_t *alias_agg = alias_type == C_AGG || alias_type == C_AGG_PTR
                                    ? p->last_type_agg
                                    : NULL;
      if (p->typedef_count >= p->typedef_capacity || find_typedef(p, name)) {
        fail(p, "duplicate or excessive local C typedef");
        break;
      }
      p->typedefs[p->typedef_count++] = (c_typedef_t){
          .name = name,
          .type = alias_type,
          .is_void = alias_type == C_BAD && !function_pointer,
          .agg = alias_agg,
          .function_result_type =
              function_pointer ? function_result_type : C_BAD,
          .function_result_agg = function_pointer ? function_result_agg : NULL};
      if (!typedef_index_ensure(p)) {
        fail(p, "out of memory indexing local C typedef declarations");
        break;
      }
      continue;
    }
    if (tok_is(p, p->pos, "extern")) {
      bool matched = false;
      if (!try_collect_char_array(p, &matched))
        break;
      if (matched)
        continue;
      if (!try_collect_integer_arrays(p, &matched))
        break;
      if (matched)
        continue;
      if (!try_collect_scalar_global(p, &matched))
        break;
      if (matched)
        continue;
      c_function_t declaration = {0};
      if (!parse_signature(p, &declaration) || !expect(p, ";")) {
        free(declaration.params);
        break;
      }
      c_function_t *known = find_function(p, declaration.name);
      if (known) {
        free(declaration.params);
      } else if (p->function_count >= p->function_capacity) {
        free(declaration.params);
        fail(p, "C frontend function-table capacity exceeded");
        break;
      } else {
        size_t function_index = p->function_count++;
        p->functions[function_index] = declaration;
        if (!function_index_add(p, function_index)) {
          fail(p, "out of memory indexing block-scope extern function");
          break;
        }
      }
      continue;
    }
    if (starts_scalar_type(p)) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.type_lookahead = true;
      c_type_t return_type = C_BAD;
      bool function_prototype = parse_type(&probe, &return_type, true) &&
                                !tok_is_attribute(&probe, probe.pos) &&
                                tok_ident(*current(&probe)) &&
                                tok_is(&probe, probe.pos + 1, "(");
      if (function_prototype) {
        c_function_t declaration = {0};
        ny_ctok_t local_function_section = {0};
        if (!parse_signature(p, &declaration) ||
            !parse_trailing_file_scope_attributes(p, &local_function_section) ||
            !expect(p, ";")) {
          free(declaration.params);
          break;
        }
        c_function_t *known = find_function(p, declaration.name);
        if (known) {
          free(declaration.params);
        } else if (p->function_count >= p->function_capacity) {
          free(declaration.params);
          fail(p, "C frontend function-table capacity exceeded");
          break;
        } else {
          size_t function_index = p->function_count++;
          p->functions[function_index] = declaration;
          if (!function_index_add(p, function_index)) {
            fail(p, "out of memory indexing block-scope function");
            break;
          }
        }
        continue;
      }
    }
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "(") &&
        find_function(p, *current(p))) {
      size_t call_after = skip_balanced(p, p->pos + 1, "(", ")");
      if (call_after < p->token_count &&
          (tok_is(p, call_after, "->") || tok_is(p, call_after, ".") ||
           tok_is(p, call_after, "["))) {
        /*
         * A call through a member of the returned object is a statement
         * when the callee is void. That result is C_BAD with no value.
         */
        c_value_t expression = parse_expr(p, 0);
        if ((expression.value < 0 && expression.type != C_BAD) ||
            (p->err && p->err_len && p->err[0]) || !expect(p, ";")) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "could not lower call through member or subscript");
          break;
        }
        continue;
      }
      ny_ctok_t called_name = *current(p);
      c_function_t *called_function = find_function(p, called_name);
      bool call_terminates_path =
          ny_ctok_eq(called_name, "__builtin_unreachable") ||
          (called_function && called_function->no_return);
      p->pos += 2;
      (void)parse_call(p, called_name);
      if ((p->err && p->err_len && p->err[0]) || !expect(p, ";"))
        break;
      if (call_terminates_path)
        *returned = true;
      continue;
    }
    if (!starts_scalar_type(p) && tok_ident(*current(p)) &&
        tok_is(p, p->pos + 1, "(")) {
      bool call_terminates_path = tok_is(p, p->pos, "__builtin_unreachable");
      (void)parse_expr(p, 0);
      if ((p->err && p->err_len && p->err[0]) || !expect(p, ";"))
        break;
      if (call_terminates_path)
        *returned = true;
      continue;
    }
    if (tok_is(p, p->pos, "static")) {
      bool matched_static_array = false;
      if (!try_collect_string_pointer_array(p, &matched_static_array, true))
        break;
      if (matched_static_array)
        continue;
    }
    size_t auto_type_pos = p->pos;
    bool auto_type_const = false;
    bool auto_type_volatile = false;
    while (tok_is(p, auto_type_pos, "const") ||
           tok_is(p, auto_type_pos, "volatile")) {
      auto_type_const |= tok_is(p, auto_type_pos, "const");
      auto_type_volatile |= tok_is(p, auto_type_pos, "volatile");
      ++auto_type_pos;
    }
    if (tok_is(p, auto_type_pos, "__auto_type") ||
        tok_is(p, auto_type_pos, "auto")) {
      p->pos = auto_type_pos + 1;
      ny_ctok_t name;
      if (!parse_name(p, &name))
        break;
      size_t local_alignment = p->last_type_alignment;
      ny_ctok_t local_mode = p->last_type_mode;
      ny_ctok_t local_section = {0};
      if (!parse_trailing_file_scope_attributes(p, &local_section) ||
          local_section.len || p->last_type_alignment != local_alignment ||
          !tok_same(p->last_type_mode, local_mode)) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "unsupported placement or type attribute on __auto_type");
        break;
      }
      if (!expect(p, "=")) {
        fail(p, "__auto_type declarations require an initializer");
        break;
      }
      size_t initializer_start = p->pos;
      c_value_t initial = parse_expr(p, 1);
      if (initial.value < 0 || initial.type == C_BAD) {
        if (!p->err || !p->err_len || !p->err[0])
          fail(p, "__auto_type initializer must have a complete C type");
        break;
      }
      if (initial.type == C_AGG) {
        const c_agg_type_t *aggregate = initial.agg;
        if (!aggregate || !aggregate->complete || !aggregate->size) {
          fail(p, "__auto_type aggregate initializer has incomplete type");
          break;
        }
        int storage =
            emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                       .dst = -1,
                                       .imm = (int64_t)aggregate->size,
                                       .scalar_class = NYIR_SCALAR_I64_RAW});
        if (storage < 0 ||
            !lower_copy_struct(p, storage, initial.value, aggregate->size) ||
            !add_local(p, name, C_AGG, storage, auto_type_const,
                       auto_type_volatile)) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "failed to initialize __auto_type aggregate local");
          break;
        }
        c_local_t *local = find_local(p, name);
        if (!local)
          break;
        local->agg = (c_agg_type_t *)aggregate;
        if (!expect(p, ";"))
          break;
        continue;
      }
      if (!add_local(p, name, initial.type, -1, auto_type_const,
                     auto_type_volatile)) {
        break;
      }
      c_local_t *local = find_local(p, name);
      if (!local)
        break;
      local->agg = (c_agg_type_t *)initial.agg;
      size_t constant_pos = initializer_start;
      int64_t integer_constant = 0;
      c_parser_t constant_probe = *p;
      bool has_integer_constant =
          c_type_is_integer(initial.type) &&
          parse_global_const_expr(&constant_probe, &constant_pos,
                                  &integer_constant) &&
          constant_pos == p->pos;
      if (!lower_local_store(p, local, initial.value))
        break;
      local->has_integer_constant = has_integer_constant;
      local->integer_constant = integer_constant;
      if (!expect(p, ";"))
        break;
      continue;
    }
    c_type_t type;
    size_t static_type_pos = p->pos + 1;
    while (tok_is(p, static_type_pos, "const") ||
           tok_is(p, static_type_pos, "volatile"))
      ++static_type_pos;
    const c_typedef_t *static_aggregate_alias =
        tok_is(p, p->pos, "static") && static_type_pos < p->token_count &&
                tok_ident(p->tokens[static_type_pos])
            ? find_typedef(p, p->tokens[static_type_pos])
            : NULL;
    const c_typedef_t *aggregate_alias =
        tok_ident(*current(p)) ? find_typedef(p, *current(p)) : NULL;
    size_t qualified_type_pos = p->pos;
    while (tok_is(p, qualified_type_pos, "const") ||
           tok_is(p, qualified_type_pos, "volatile"))
      ++qualified_type_pos;
    const c_typedef_t *qualified_aggregate_alias =
        tok_ident(p->tokens[qualified_type_pos])
            ? find_typedef(p, p->tokens[qualified_type_pos])
            : NULL;
    bool aggregate_declaration =
        starts_aggregate_type(p) || tok_is(p, qualified_type_pos, "struct") ||
        tok_is(p, qualified_type_pos, "union") ||
        tok_is(p, qualified_type_pos, "typeof") ||
        tok_is(p, qualified_type_pos, "__typeof__") ||
        tok_is(p, qualified_type_pos, "__typeof") ||
        tok_is(p, qualified_type_pos, "__typeof_unqual__") ||
        (qualified_aggregate_alias &&
         qualified_aggregate_alias->type == C_AGG) ||
        (aggregate_alias && aggregate_alias->type == C_AGG) ||
        (tok_is(p, p->pos, "static") &&
         (tok_is(p, static_type_pos, "struct") ||
          tok_is(p, static_type_pos, "union") ||
          (static_aggregate_alias && static_aggregate_alias->type == C_AGG)));
    if (!aggregate_declaration && tok_is(p, p->pos, "static")) {
      c_parser_t static_probe = *p;
      char static_error[1] = {0};
      static_probe.err = static_error;
      static_probe.err_len = sizeof(static_error);
      static_probe.type_lookahead = true;
      ++static_probe.pos;
      c_type_t static_type;
      if (parse_type(&static_probe, &static_type, false)) {
        while (c_type_is_pointer(static_type)) {
          c_type_t pointee = c_remove_pointer_levels(static_type, 1);
          if (pointee == C_BAD)
            break;
          static_type = pointee;
        }
        aggregate_declaration = static_type == C_AGG;
      }
    }
    if (aggregate_declaration) {
      c_parser_t type_probe = *p;
      type_probe.type_lookahead = true;
      if (tok_is(&type_probe, type_probe.pos, "static"))
        ++type_probe.pos;
      c_type_t declared_type;
      if (parse_type(&type_probe, &declared_type, false)) {
        c_type_t aggregate_base = declared_type;
        while (c_type_is_pointer(aggregate_base)) {
          c_type_t pointee = c_remove_pointer_levels(aggregate_base, 1);
          if (pointee == C_BAD)
            break;
          aggregate_base = pointee;
        }
        bool mixed_pointer_declarators = false;
        if (declared_type != C_AGG && aggregate_base == C_AGG &&
            !tok_is(p, p->pos, "static")) {
          unsigned paren_depth = 0, bracket_depth = 0, brace_depth = 0;
          for (size_t i = type_probe.pos; i < p->token_count; ++i) {
            if (!paren_depth && !bracket_depth && !brace_depth) {
              if (tok_is(p, i, ",")) {
                size_t next = i + 1;
                while (tok_is(p, next, "const") || tok_is(p, next, "volatile"))
                  ++next;
                if (!tok_is(p, next, "*")) {
                  mixed_pointer_declarators = true;
                  break;
                }
              }
              if (tok_is(p, i, ";"))
                break;
            }
            if (tok_is(p, i, "("))
              ++paren_depth;
            else if (tok_is(p, i, ")") && paren_depth)
              --paren_depth;
            else if (tok_is(p, i, "["))
              ++bracket_depth;
            else if (tok_is(p, i, "]") && bracket_depth)
              --bracket_depth;
            else if (tok_is(p, i, "{"))
              ++brace_depth;
            else if (tok_is(p, i, "}") && brace_depth)
              --brace_depth;
          }
        }
        bool pointer_to_array_declarator =
            tok_is(&type_probe, type_probe.pos, "(") &&
            tok_is(&type_probe, type_probe.pos + 1, "*");
        if ((starts_aggregate_type(p) ||
             (aggregate_base == C_AGG &&
              (declared_type == C_AGG || mixed_pointer_declarators))) &&
            !pointer_to_array_declarator) {
          if (!lower_aggregate_declaration(p, true))
            break;
          continue;
        }
      }
    }
    if (starts_scalar_type(p) || starts_aggregate_type(p) ||
        (aggregate_alias && aggregate_alias->type == C_AGG) ||
        (static_aggregate_alias && static_aggregate_alias->type == C_AGG)) {
      bool is_static_scalar = tok_is(p, p->pos, "static");
      size_t type_start = p->pos;
      if (!parse_type(p, &type, true))
        break;
      c_agg_type_t *decl_agg = p->last_type_agg;
      c_type_t decl_pointer_base_type = p->last_type_base_type;
      unsigned decl_pointer_depth = p->last_type_pointer_count;
      bool decl_pointer_base_valid = p->last_type_base_valid;
      size_t decl_pointee_array_size = p->last_type_array_size;
      c_array_shape_t decl_pointee_array = p->last_type_pointee_array;
      bool decl_typedef_array = p->last_type_is_array;
      c_array_shape_t decl_array_shape = p->last_type_array;
      if (decl_typedef_array && !decl_array_shape.rank &&
          c_type_is_pointer(type))
        decl_typedef_array = false;
      bool decl_is_bool = p->last_type_bool;
      c_type_t decl_function_result_type = p->last_type_function_result_type;
      c_agg_type_t *decl_function_result_agg = p->last_type_function_result_agg;
      /*
       * Array brackets on a function-pointer typedef can clear the result
       * carried by parse_type; recover it from the base alias so an element
       * call keeps the real return type.
       */
      if (decl_function_result_type == C_BAD) {
        const c_typedef_t *base_alias = find_typedef(p, p->tokens[type_start]);
        if (base_alias && base_alias->function_result_type != C_BAD) {
          decl_function_result_type = base_alias->function_result_type;
          decl_function_result_agg = base_alias->function_result_agg;
        }
      }
      bool is_const = c_type_is_pointer(type) ? p->last_type_const_pointer
                                              : p->last_type_const;
      bool is_volatile = p->last_type_volatile;
      size_t static_fp_name = p->pos + 2;
      if (tok_is(p, static_fp_name, "const"))
        ++static_fp_name;
      if (is_static_scalar && (c_type_is_integer(type) || type == C_FUNC_PTR) &&
          tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "*") &&
          tok_ident(p->tokens[static_fp_name]) &&
          tok_is(p, static_fp_name + 1, "[")) {
        ++p->pos;
        if (!expect(p, "*")) {
          break;
        }
        bool pointer_const = accept(p, "const");
        ny_ctok_t name;
        if (!parse_name(p, &name)) {
          fail(p, "malformed static local function-pointer array");
          break;
        }
        size_t array_start = p->pos;
        if (!expect(p, "[") || !expect(p, "]") || !expect(p, ")")) {
          fail(p, "malformed static local function-pointer array");
          break;
        }
        if (accept(p, "(")) {
          size_t parameters_after = skip_balanced(p, p->pos - 1, "(", ")");
          if (parameters_after >= p->token_count) {
            fail(p, "unterminated static local function-pointer array type");
            break;
          }
        } else if (decl_function_result_type == C_BAD) {
          fail(p,
               "static local function-pointer array requires a function type");
          break;
        }
        p->pos = array_start;
        if (!lower_static_scalar_declaration(
                p, C_FUNC_PTR, name, is_const || pointer_const, is_volatile,
                NULL, (ny_ctok_t){0})) {
          break;
        }
        p->locals[p->local_count - 1].function_result_type =
            decl_function_result_type != C_BAD ? decl_function_result_type
                                               : type;
        continue;
      }
      if (type == C_BAD) {
        int function_pointer_array = lower_local_function_pointer_array(p);
        if (function_pointer_array < 0)
          break;
        if (function_pointer_array > 0)
          continue;
      }
      ny_ctok_t function_pointer_name = {0};
      if (type == C_BAD &&
          parse_function_pointer_returning_function_pointer_declarator(
              p, &function_pointer_name)) {
        int initial = emit_value(
            p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
        if (initial < 0 || !add_local(p, function_pointer_name, C_FUNC_PTR,
                                      initial, is_const, is_volatile))
          break;
        find_local(p, function_pointer_name)->function_result_type = C_FUNC_PTR;
        if (!expect(p, ";"))
          break;
        continue;
      }
      if (parse_function_pointer_declarator(p, &function_pointer_name)) {
        int initial = emit_value(
            p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
        if (initial < 0 || !add_local(p, function_pointer_name, C_FUNC_PTR,
                                      initial, is_const, is_volatile))
          break;
        c_local_t *function_pointer = find_local(p, function_pointer_name);
        function_pointer->function_result_type = type;
        if (accept(p, "=")) {
          c_value_t value = parse_expr(p, 1);
          bool compatible = value.type == C_FUNC_PTR ||
                            value.type == C_VOID_PTR ||
                            (c_type_is_integer(value.type) &&
                             p->last_value_null_pointer_constant);
          if (value.value < 0 || !compatible ||
              !emit(p,
                    (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                 .dst = -1,
                                 .a = value.value,
                                 .imm = (int64_t)function_pointer->slot,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY},
                    NULL)) {
            if (!p->err || !p->err_len || !p->err[0])
              fail(p,
                   "function-pointer initializer must be a function pointer");
            break;
          }
        }
        if (!expect(p, ";"))
          break;
        continue;
      }
      if (type == C_BAD) {
        function_pointer_name = (ny_ctok_t){0};
        unsigned pointer_depth = 0;
        if (parse_nested_function_pointer_declarator(p, &function_pointer_name,
                                                     &pointer_depth)) {
          c_type_t function_pointer_type =
              pointer_depth == 2 ? C_FUNC_PTR_PTR : C_OPAQUE_PTR;
          int initial_value = emit_value(
              p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 0});
          if (initial_value < 0 ||
              !add_local(p, function_pointer_name, function_pointer_type,
                         initial_value, is_const, is_volatile) ||
              !expect(p, ";"))
            break;
          continue;
        }
      }
      unsigned declared_stars = 0;
      for (size_t i = type_start; i < p->pos; ++i)
        declared_stars += tok_is(p, i, "*");
      c_type_t base_type = decl_pointer_base_valid
                               ? decl_pointer_base_type
                               : c_remove_pointer_levels(type, declared_stars);
      if (type == C_AGG_ARRAY_PTR && base_type == C_BAD) {
        base_type = C_AGG;
        if (decl_pointer_depth == 0)
          decl_pointer_depth = 1;
      }
      if (base_type == C_BAD && decl_pointer_depth == 0) {
        fail(p, "unsupported pointer declarator in local declaration");
        break;
      }
      bool declaration_ok = true;
      bool static_array = false;
      bool first_declarator = true;
      do {
        unsigned local_pointer_depth = decl_pointer_depth;
        bool declarator_is_const = is_const;
        bool declarator_is_volatile = false;
        bool declarator_has_pointer = false;
        if (!first_declarator) {
          unsigned stars = 0;
          bool pointer_const = false;
          bool pointer_volatile = false;
          while (accept(p, "*")) {
            ++stars;
            pointer_const = false;
            pointer_volatile = false;
            for (;;) {
              if (accept(p, "const"))
                pointer_const = true;
              else if (accept(p, "volatile"))
                pointer_volatile = true;
              else if (accept(p, "restrict") || accept(p, "__restrict") ||
                       accept(p, "__restrict__")) {
              } else
                break;
            }
          }
          if (stars) {
            declarator_has_pointer = true;
            declarator_is_const = pointer_const;
            declarator_is_volatile = pointer_volatile;
          }
          type = c_add_pointer_levels(base_type, stars);
          unsigned base_pointer_depth =
              decl_pointer_depth >= declared_stars
                  ? decl_pointer_depth - declared_stars
                  : 0;
          local_pointer_depth = base_pointer_depth + stars;
          if (type == C_BAD) {
            fail(p, "unsupported pointer declarator in local declaration");
            declaration_ok = false;
            break;
          }
        }
        first_declarator = false;
        ny_ctok_t leading_section = {0};
        ny_ctok_t leading_cleanup = {0};
        if (!parse_trailing_local_attributes(p, &leading_section,
                                             &leading_cleanup)) {
          declaration_ok = false;
          break;
        }
        bool pointee_volatile =
            is_volatile && c_type_is_arithmetic_pointer(type);
        unsigned char pointee_seg =
            c_type_is_pointer(type) ? p->last_type_seg : C_SEG_NONE;
        if (!declarator_has_pointer)
          declarator_is_volatile = is_volatile && !pointee_volatile;
        ny_ctok_t name;
        size_t local_pointee_array_size = 0;
        c_array_shape_t local_pointee_array = decl_pointee_array;
        if (accept(p, "(")) {
          unsigned pointer_levels = 0;
          while (accept(p, "*"))
            ++pointer_levels;
          if (pointer_levels != 1 || !parse_name(p, &name) || !expect(p, ")")) {
            if (!p->err[0])
              fail(p, "unsupported pointer-to-array local declarator");
            declaration_ok = false;
            break;
          }
          type = c_add_pointer_levels(base_type, pointer_levels);
          size_t array_elements = 1;
          unsigned array_rank = 0;
          size_t element_size = base_type == C_AGG && decl_agg
                                    ? decl_agg->size
                                    : c_type_size(p->abi, base_type);
          local_pointer_depth += pointer_levels;
          local_pointee_array = (c_array_shape_t){
              .element_size = element_size,
              .element_type = base_type,
              .agg = decl_agg,
              .pointer_to_array = true,
          };
          while (accept(p, "[")) {
            uint64_t extent = 0;
            if (++array_rank > C_MAX_ARRAY_RANK || !element_size ||
                !parse_array_extent_expr(p, 1, &extent) || !extent ||
                extent > 65536 || !expect(p, "]") ||
                array_elements > SIZE_MAX / (size_t)extent) {
              fail(p, "invalid pointer-to-array local extent");
              declaration_ok = false;
              break;
            }
            local_pointee_array.extent[array_rank - 1] = (size_t)extent;
            array_elements *= (size_t)extent;
          }
          if (!declaration_ok)
            break;
          if (!array_rank || array_elements > SIZE_MAX / element_size ||
              type == C_BAD) {
            fail(p, "invalid pointer-to-array local declarator");
            declaration_ok = false;
            break;
          }
          local_pointee_array.rank = array_rank;
          local_pointee_array_size = array_elements * element_size;
        } else if (!parse_name(p, &name)) {
          declaration_ok = false;
          break;
        }
        ny_ctok_t local_asm_register = {0};
        if (!try_parse_asm_label(p, &local_asm_register)) {
          declaration_ok = false;
          break;
        }
        size_t local_asm_marker = SIZE_MAX;
        if (local_asm_register.len) {
          if (!p->function || local_asm_register.start[0] != '"') {
            fail(p, "fixed-register local variables require a string register "
                    "name");
            declaration_ok = false;
            break;
          }
          local_asm_marker = p->out->len;
          if (!emit(p,
                    (nyr_inst_t){.op = NYIR_INLINE_ASM,
                                 .dst = -1,
                                 .a = -1,
                                 .symbol = "__nyc_unsupported_inline_asm:fixed-"
                                           "register local variable"},
                    NULL)) {
            declaration_ok = false;
            break;
          }
        }
        size_t local_alignment = p->last_type_alignment;
        ny_ctok_t local_mode = p->last_type_mode;
        /*
         * Attributes written after the type but before the declarator name
         * live in last_type_section; carry them onto static storage too.
         */
        ny_ctok_t local_section = p->last_type_section;
        p->last_type_section = (ny_ctok_t){0};
        if (leading_section.len) {
          if (local_section.len) {
            fail(p, "multiple section attributes on one local declarator");
            declaration_ok = false;
            break;
          }
          local_section = leading_section;
        }
        ny_ctok_t local_cleanup = p->last_type_cleanup;
        p->last_type_cleanup = (ny_ctok_t){0};
        if (leading_cleanup.len) {
          if (local_cleanup.len) {
            fail(p, "multiple cleanup attributes on one local declarator");
            declaration_ok = false;
            break;
          }
          local_cleanup = leading_cleanup;
        }
        if (!parse_trailing_local_attributes(p, &local_section,
                                             &local_cleanup)) {
          declaration_ok = false;
          break;
        }
        /*
         * An alignment request at or below the type's natural alignment
         * cannot weaken the object, and automatic arrays are already aligned
         * to 16 bytes. Such requests need no extra placement work.
         */
        size_t natural_alignment = c_type_alignment(p->abi, type);
        bool alignment_satisfied =
            p->last_type_alignment == local_alignment ||
            p->last_type_alignment <= natural_alignment ||
            (p->last_type_is_array && p->last_type_alignment <= 16);
        if ((local_section.len && !is_static_scalar) || !alignment_satisfied ||
            !tok_same(p->last_type_mode, local_mode)) {
          if (!getenv("NYC_KERNEL_COMPAT")) {
            char message[192];
            snprintf(message, sizeof(message),
                     "placement and type attributes on local declarations are "
                     "unsupported (section=%zu alignment=%zu natural=%zu "
                     "mode_changed=%d)",
                     local_section.len, p->last_type_alignment,
                     natural_alignment,
                     !tok_same(p->last_type_mode, local_mode));
            fail(p, message);
            declaration_ok = false;
            break;
          }
          p->last_type_alignment = local_alignment;
          p->last_type_mode = local_mode;
          if (!is_static_scalar)
            local_section = (ny_ctok_t){0};
        }
        if (is_static_scalar) {
          if (local_cleanup.len) {
            fail(p, "cleanup attribute requires an automatic local object");
            declaration_ok = false;
            break;
          }
          if (!lower_static_scalar_declaration(
                  p, type, name, declarator_is_const, declarator_is_volatile,
                  decl_agg, local_section)) {
            declaration_ok = false;
            break;
          }
          static_array = true;
          break;
        }
        if (!is_static_scalar &&
            (tok_is(p, p->pos, "[") || decl_typedef_array)) {
          if (local_cleanup.len) {
            fail(p, "cleanup attribute on local arrays is unsupported");
            declaration_ok = false;
            break;
          }
          size_t extents[C_MAX_ARRAY_RANK] = {0};
          size_t count = 1;
          unsigned rank = 0;
          bool inferred_outer = false;
          if (decl_typedef_array) {
            rank = decl_array_shape.rank;
            if (!rank || rank > C_MAX_ARRAY_RANK) {
              fail(p, "array typedef has an invalid dimension count");
              declaration_ok = false;
              break;
            }
            for (unsigned dimension = 0; dimension < rank; ++dimension) {
              extents[dimension] = decl_array_shape.extent[dimension];
              if (!extents[dimension] ||
                  count > (size_t)INT32_MAX / extents[dimension]) {
                fail(p, "array typedef exceeds automatic local capacity");
                declaration_ok = false;
                break;
              }
              count *= extents[dimension];
            }
            if (!declaration_ok)
              break;
          }
          while (accept(p, "[")) {
            if (rank == 0 && accept(p, "]")) {
              inferred_outer = true;
              ++rank;
              continue;
            }
            uint64_t extent = 0;
            if (rank == C_MAX_ARRAY_RANK ||
                !parse_array_extent_expr(p, 1, &extent) ||
                (!extent && rank != 0) || extent > INT32_MAX ||
                !expect(p, "]") ||
                (extent && count > (size_t)INT32_MAX / (size_t)extent)) {
              char message[192];
              ny_ctok_t at = *current(p);
              snprintf(message, sizeof(message),
                       "automatic scalar array `%.*s`: invalid dimension %u "
                       "near `%.*s`",
                       (int)(name.len > 48 ? 48 : name.len), name.start,
                       rank + 1, (int)(at.len > 48 ? 48 : at.len), at.start);
              fail(p, message);
              declaration_ok = false;
              break;
            }
            extents[rank++] = (size_t)extent;
            count = extent ? count * (size_t)extent : 0;
          }
          if (!declaration_ok)
            break;
          ny_ctok_t array_section = {0};
          ny_ctok_t array_cleanup = {0};
          if (!parse_trailing_local_attributes(p, &array_section,
                                               &array_cleanup)) {
            declaration_ok = false;
            break;
          }
          /*
           * Fixed NYIR allocas are 16-byte aligned by the x86-64 backend.
           * A larger requested alignment is honored by over-allocating and
           * rounding the base up; placement and mode changes stay rejected.
           */
          size_t array_alignment = p->last_type_alignment;
          if (array_section.len || array_cleanup.len ||
              !tok_same(p->last_type_mode, local_mode) ||
              array_alignment > 65536u) {
            if (getenv("NYC_KERNEL_COMPAT")) {
              /* Kernel code uses local array attributes NYC cannot fully honor;
                 ignore placement/alignment for compilation coverage. */
              array_alignment = 0;
            } else {
              fail(p, "unsupported placement or over-alignment on local array");
              declaration_ok = false;
              break;
            }
          }
          bool initialized = accept(p, "=");
          size_t initializer_count = 0;
          unsigned char *string_initializer = NULL;
          size_t string_initializer_len = 0;
          size_t string_initializer_capacity = 0;
          bool wide_string_initializer =
              initialized && rank == 1 && type == C_U16 &&
              current(p)->kind == NY_CTOK_STRING && current(p)->len >= 2 &&
              current(p)->start[0] == 'L';
          bool initialized_from_string =
              initialized && rank == 1 &&
              ((type == C_CHAR || type == C_U8) || wide_string_initializer) &&
              current(p)->kind == NY_CTOK_STRING;
          if (initialized) {
            if (initialized_from_string) {
              while (current(p)->kind == NY_CTOK_STRING) {
                unsigned char *part = NULL;
                size_t part_len = 0;
                bool part_wide =
                    current(p)->len >= 2 && current(p)->start[0] == 'L';
                if (part_wide != wide_string_initializer ||
                    !(part_wide ? decode_wide_string_token(*current(p), &part,
                                                           &part_len)
                                : decode_string_token(*current(p), &part,
                                                      &part_len)) ||
                    !append_asm_bytes(&string_initializer,
                                      &string_initializer_len,
                                      &string_initializer_capacity, part,
                                      part_len >= (part_wide ? 2u : 1u)
                                          ? part_len - (part_wide ? 2u : 1u)
                                          : 0)) {
                  free(part);
                  free(string_initializer);
                  fail(p, "invalid automatic character-array string");
                  declaration_ok = false;
                  break;
                }
                free(part);
                ++p->pos;
              }
              if (!declaration_ok ||
                  !append_asm_bytes(&string_initializer,
                                    &string_initializer_len,
                                    &string_initializer_capacity,
                                    wide_string_initializer ? "\0\0" : "\0",
                                    wide_string_initializer ? 2 : 1)) {
                free(string_initializer);
                declaration_ok = false;
                break;
              }
              initializer_count = wide_string_initializer
                                      ? string_initializer_len / 2
                                      : string_initializer_len;
              if (inferred_outer) {
                count = initializer_count;
                extents[0] = count;
              } else if (initializer_count > count) {
                free(string_initializer);
                fail(p, "string initializer exceeds automatic array bound");
                declaration_ok = false;
                break;
              }
            } else if (!tok_is(p, p->pos, "{")) {
              declaration_ok = false;
              break;
            } else {
              size_t pos = p->pos + 1;
              unsigned braces = 0, parens = 0, brackets = 0;
              bool has_element = !tok_is(p, pos, "}");
              initializer_count = has_element ? 1 : 0;
              for (; pos < p->token_count; ++pos) {
                if (tok_is(p, pos, "{")) {
                  ++braces;
                } else if (tok_is(p, pos, "}")) {
                  if (braces == 0 && parens == 0 && brackets == 0)
                    break;
                  if (braces)
                    --braces;
                } else if (tok_is(p, pos, "(")) {
                  ++parens;
                } else if (tok_is(p, pos, ")") && parens) {
                  --parens;
                } else if (tok_is(p, pos, "[")) {
                  ++brackets;
                } else if (tok_is(p, pos, "]") && brackets) {
                  --brackets;
                } else if (tok_is(p, pos, ",") && braces == 0 && parens == 0 &&
                           brackets == 0 && !tok_is(p, pos + 1, "}")) {
                  ++initializer_count;
                }
              }
              if (pos == p->token_count ||
                  (inferred_outer && initializer_count == 0) ||
                  (inferred_outer && rank != 1)) {
                fail(p, "cannot infer automatic array extent from initializer");
                declaration_ok = false;
                break;
              }
              if (inferred_outer) {
                count = count_global_aggregate_array_initializer(p, p->pos);
                if (!count || count > 65536 ||
                    count > SIZE_MAX / (size_t)c_type_size(p->abi, type)) {
                  fail(p,
                       "cannot infer automatic array extent from initializer");
                  declaration_ok = false;
                  break;
                }
                extents[0] = count;
              } else if (initializer_count > count) {
                fail(p, "too many values in automatic array initializer");
                declaration_ok = false;
                break;
              }
            }
          } else if (inferred_outer) {
            fail(p, "automatic array without an initializer needs a bound");
            declaration_ok = false;
            break;
          }
          if (declarator_is_const && !initialized) {
            fail(p, "const local array requires an initializer");
            declaration_ok = false;
            break;
          }
          size_t width = type == C_AGG && decl_agg ? decl_agg->size
                                                   : c_type_size(p->abi, type);
          if (count > SIZE_MAX / width || count * width > INT32_MAX) {
            fail(p, "automatic scalar array exceeds frontend capacity");
            free(string_initializer);
            declaration_ok = false;
            break;
          }
          size_t array_bytes = count * width;
          if (array_alignment > 16) {
            if (array_bytes > (size_t)INT32_MAX - (array_alignment - 1)) {
              fail(p, "aligned automatic array exceeds frontend capacity");
              free(string_initializer);
              declaration_ok = false;
              break;
            }
            array_bytes += array_alignment - 1;
          }
          int base =
              emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .imm = (int64_t)array_bytes,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
          if (base >= 0 && array_alignment > 16) {
            int bias = emit_i64_constant(p, (int64_t)(array_alignment - 1));
            int added = bias < 0
                            ? -1
                            : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = base,
                                                         .b = bias});
            int mask = emit_i64_constant(p, ~(int64_t)(array_alignment - 1));
            base = added < 0 || mask < 0
                       ? -1
                       : emit_value(p, (nyr_inst_t){.op = NYIR_AND_I64,
                                                    .dst = -1,
                                                    .a = added,
                                                    .b = mask});
          }
          p->has_auto_arrays = true;
          if (base < 0 || !add_local(p, name, C_I64, base, false, false)) {
            free(string_initializer);
            declaration_ok = false;
            break;
          }
          c_local_t *local = &p->locals[p->local_count - 1];
          local->type = type;
          local->agg = decl_agg;
          local->pointer_base_type = decl_pointer_base_type;
          local->pointer_depth = local_pointer_depth;
          if (decl_agg && base_type == C_AGG &&
              (type == C_AGG_PTR || type == C_AGG_PTR_PTR))
            local->agg = decl_agg;
          local->is_const = declarator_is_const;
          local->is_volatile = declarator_is_volatile;
          local->pointee_volatile = pointee_volatile;
          local->pointee_seg = pointee_seg;
          local->function_result_type = decl_function_result_type;
          if (decl_function_result_agg)
            local->agg = decl_function_result_agg;
          local->address_taken = true;
          local->array_count = count;
          local->array_rank = rank;
          memcpy(local->array_extent, extents, sizeof(extents));
          uint32_t flags = type == C_U8 || type == C_CHAR ? NYIR_INST_F_MEM_BYTE
                           : type == C_U16                ? NYIR_INST_F_MEM_U16
                           : type == C_I32 || type == C_U32 || type == C_F32
                               ? NYIR_INST_F_MEM_I32
                               : 0;
          if (initialized) {
            if (type == C_AGG) {
              if (!decl_agg || !decl_agg->complete ||
                  !lower_zero_fill(p, base, count * width)) {
                declaration_ok = false;
                free(string_initializer);
                break;
              }
            } else if (count * width >= 128 && !p->no_builtin && !is_volatile &&
                       !pointee_volatile) {
              if (!lower_zero_fill(p, base, count * width)) {
                declaration_ok = false;
              }
            } else {
              int zero =
                  emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1});
              for (size_t i = 0; i < count && zero >= 0; ++i) {
                int address =
                    lower_add_const(p, base, (int64_t)(i * (size_t)width));
                if (address < 0 ||
                    !emit(p,
                          (nyr_inst_t){.op = NYIR_STORE_I64,
                                       .dst = -1,
                                       .a = address,
                                       .c = zero,
                                       .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .flags = flags,
                                       .scalar_class = NYIR_SCALAR_I64_RAW},
                          NULL)) {
                  declaration_ok = false;
                  break;
                }
              }
            }
            if (!declaration_ok ||
                (!initialized_from_string && !expect(p, "{"))) {
              declaration_ok = false;
              free(string_initializer);
              break;
            }
            if (initialized_from_string) {
              for (size_t i = 0; i < initializer_count; ++i) {
                int64_t element =
                    wide_string_initializer
                        ? (int64_t)((uint16_t)string_initializer[i * 2] |
                                    (uint16_t)string_initializer[i * 2 + 1]
                                        << 8)
                        : string_initializer[i];
                int element_value = emit_value(
                    p, (nyr_inst_t){
                           .op = NYIR_CONST_I64, .dst = -1, .imm = element});
                int address =
                    lower_add_const(p, base, (int64_t)(i * (size_t)width));
                if (element_value < 0 || address < 0 ||
                    !emit(p,
                          (nyr_inst_t){.op = NYIR_STORE_I64,
                                       .dst = -1,
                                       .a = address,
                                       .c = element_value,
                                       .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .flags = flags,
                                       .scalar_class = NYIR_SCALAR_I64_RAW},
                          NULL)) {
                  declaration_ok = false;
                  break;
                }
              }
              free(string_initializer);
              if (!declaration_ok)
                break;
              continue;
            }
            size_t next_index = 0;
            for (size_t i = 0; i < initializer_count; ++i) {
              size_t first = next_index;
              size_t last = next_index;
              if (accept(p, "[")) {
                int64_t first_index = -1;
                int64_t last_index = -1;
                if (!parse_global_const_expr(p, &p->pos, &first_index) ||
                    first_index < 0) {
                  fail(p, "invalid automatic array initializer designator");
                  declaration_ok = false;
                  break;
                }
                last_index = first_index;
                if (accept(p, "...") &&
                    (!parse_global_const_expr(p, &p->pos, &last_index) ||
                     last_index < first_index)) {
                  fail(p, "invalid automatic array range designator");
                  declaration_ok = false;
                  break;
                }
                if (!expect(p, "]") || !expect(p, "=") ||
                    (uint64_t)last_index >= count) {
                  fail(p, "automatic array initializer designator is out of "
                          "bounds");
                  declaration_ok = false;
                  break;
                }
                first = (size_t)first_index;
                last = (size_t)last_index;
              } else if (next_index >= count) {
                fail(p, "too many values in automatic array initializer");
                declaration_ok = false;
                break;
              }
              c_value_t element = parse_expr(p, 1);
              bool compatible =
                  (element.type == type &&
                   (type != C_AGG || element.agg == decl_agg)) ||
                  c_pointer_compatible(element.type, type) ||
                  (c_type_is_integer(element.type) &&
                   c_type_is_integer(type)) ||
                  (c_type_is_pointer(type) && c_type_is_integer(element.type) &&
                   p->last_value_null_pointer_constant);
              if (element.value < 0 || !compatible) {
                if (element.value >= 0)
                  fail(p, "automatic array initializer has incompatible type");
                declaration_ok = false;
                break;
              }
              if (type == C_AGG) {
                for (size_t index = first; index <= last; ++index) {
                  int address = lower_add_const(
                      p, base, (int64_t)(index * (size_t)width));
                  if (address < 0 ||
                      !lower_copy_struct(p, address, element.value, width)) {
                    declaration_ok = false;
                    break;
                  }
                }
                if (!declaration_ok)
                  break;
                next_index = last + 1;
                if (i + 1 < initializer_count) {
                  if (!expect(p, ",")) {
                    declaration_ok = false;
                    break;
                  }
                } else {
                  (void)accept(p, ",");
                }
                continue;
              }
              if (c_type_is_integer(type)) {
                element.value = normalize_integer(p, type, element.value);
                if (element.value < 0) {
                  declaration_ok = false;
                  break;
                }
              }
              for (size_t index = first; index <= last; ++index) {
                int address =
                    lower_add_const(p, base, (int64_t)(index * (size_t)width));
                if (address < 0 ||
                    !emit(p,
                          (nyr_inst_t){.op = NYIR_STORE_I64,
                                       .dst = -1,
                                       .a = address,
                                       .c = element.value,
                                       .effects = NYIR_EFFECT_WRITE_MEMORY |
                                                  NYIR_EFFECT_MAY_TRAP,
                                       .flags = flags,
                                       .scalar_class =
                                           c_type_is_float(type)
                                               ? c_float_scalar_class(type)
                                               : NYIR_SCALAR_I64_RAW},
                          NULL)) {
                  declaration_ok = false;
                  break;
                }
              }
              if (!declaration_ok)
                break;
              next_index = last + 1;
              if (i + 1 < initializer_count) {
                if (!expect(p, ",")) {
                  declaration_ok = false;
                  break;
                }
              } else {
                (void)accept(p, ",");
              }
            }
            if (!declaration_ok || !expect(p, "}")) {
              declaration_ok = false;
              break;
            }
          }
          continue;
        }
        int initial_value = -1;
        bool initialized = accept(p, "=");
        if (initialized) {
          if (c_type_is_complex(type)) {
            size_t complex_bytes = type == C_C64 ? 8 : type == C_C128 ? 16 : 32;
            size_t complex_offset = 0;
            if (!agg_area_alloc(p, complex_bytes, &complex_offset) ||
                !add_local(p, name, C_AGG, p->agg_base, declarator_is_const,
                           declarator_is_volatile)) {
              declaration_ok = false;
              break;
            }
            c_local_t *complex_local = &p->locals[p->local_count - 1];
            complex_local->agg = c_complex_aggregate(type);
            int complex_base = lower_local_load(p, complex_local);
            c_value_t complex_initial = parse_expr(p, 1);
            if (complex_base < 0 || complex_initial.value < 0 ||
                !lower_complex_initialize(p, complex_base, complex_local->agg,
                                          complex_initial)) {
              declaration_ok = false;
              break;
            }
            continue;
          }
          if (!add_local(p, name, type, -1, declarator_is_const,
                         declarator_is_volatile)) {
            declaration_ok = false;
            break;
          }
          c_local_t *local = &p->locals[p->local_count - 1];
          if (local_asm_register.len) {
            local->fixed_register = local_asm_register;
            local->fixed_register_marker = local_asm_marker;
          }
          local->cleanup = local_cleanup;
          local->agg = decl_agg;
          local->pointer_base_type = decl_pointer_base_type;
          local->pointer_depth = local_pointer_depth;
          if (decl_agg && base_type == C_AGG &&
              (type == C_AGG_PTR || type == C_AGG_PTR_PTR))
            local->agg = decl_agg;
          local->function_result_type = decl_function_result_type;
          local->is_bool = decl_is_bool && !c_type_is_pointer(type);
          if (decl_function_result_agg)
            local->agg = decl_function_result_agg;
          local->pointee_const = c_type_is_pointer(type) && p->last_type_const;
          local->pointee_volatile = pointee_volatile;
          local->pointee_seg = pointee_seg;
          local->pointee_array_size = local_pointee_array_size
                                          ? local_pointee_array_size
                                          : decl_pointee_array_size;
          local->pointee_array = local_pointee_array;
          size_t initializer_start = p->pos;
          c_value_t initial = parse_expr(p, 1);
          bool int128_scalar_zero = false;
          if (type == C_AGG && decl_agg && decl_agg->is_int128 &&
              c_type_is_integer(initial.type)) {
            c_parser_t constant_probe = *p;
            size_t constant_pos = initializer_start;
            int64_t constant_value = 1;
            int128_scalar_zero =
                parse_global_const_expr(&constant_probe, &constant_pos,
                                        &constant_value) &&
                constant_pos == p->pos && constant_value == 0;
          }
          bool pointer_to_bool =
              local->is_bool && c_type_is_pointer(initial.type);
          if (initial.value < 0 ||
              !(initial.type == type ||
                c_pointer_compatible(initial.type, type) ||
                (c_type_is_integer(initial.type) && c_type_is_integer(type)) ||
                (c_type_is_integer(initial.type) && c_type_is_float(type)) ||
                (c_type_is_float(initial.type) && c_type_is_float(type)) ||
                pointer_to_bool ||
                (c_type_is_pointer(type) &&
                 p->last_value_null_pointer_constant) ||
                (getenv("NYC_KERNEL_COMPAT") &&
                 ((c_type_is_pointer(type) &&
                   c_type_is_integer(initial.type)) ||
                  (c_type_is_integer(type) &&
                   c_type_is_pointer(initial.type)) ||
                  (c_type_is_pointer(type) &&
                   c_type_is_pointer(initial.type)))) ||
                int128_scalar_zero)) {
            if (initial.value >= 0) {
              char message[128];
              snprintf(message, sizeof(message),
                       "local initializer type mismatch (target %d, value %d)",
                       type, initial.type);
              fail(p, message);
            }
            declaration_ok = false;
            break;
          }
          if (pointer_to_bool) {
            initial.value = lower_boolean_value(p, initial);
            if (initial.value < 0) {
              declaration_ok = false;
              break;
            }
            initial.type = C_U8;
          }
          if (c_type_is_integer(initial.type) && c_type_is_float(type)) {
            initial.value = emit_value(
                p, (nyr_inst_t){.op = type == C_F32 ? NYIR_I64_TO_F32
                                                    : NYIR_I64_TO_F64,
                                .dst = -1,
                                .a = initial.value,
                                .scalar_class = c_float_scalar_class(type)});
            if (initial.value < 0) {
              declaration_ok = false;
              break;
            }
          } else if (c_type_is_float(initial.type) && c_type_is_float(type) &&
                     initial.type != type) {
            initial.value = emit_value(
                p, (nyr_inst_t){.op = type == C_F32 ? NYIR_F64_TO_F32
                                                    : NYIR_F32_TO_F64,
                                .dst = -1,
                                .a = initial.value,
                                .scalar_class = c_float_scalar_class(type)});
            if (initial.value < 0) {
              declaration_ok = false;
              break;
            }
          }
          initial_value = initial.value;
          if (c_type_is_integer(type)) {
            initial_value = normalize_integer(p, type, initial_value);
            if (initial_value < 0) {
              declaration_ok = false;
              break;
            }
          }
          size_t constant_pos = initializer_start;
          int64_t integer_constant = 0;
          c_parser_t constant_probe = *p;
          bool has_integer_constant =
              c_type_is_integer(type) &&
              parse_global_const_expr(&constant_probe, &constant_pos,
                                      &integer_constant) &&
              constant_pos == p->pos;
          local = find_local(p, name);
          bool stored = false;
          if (local && int128_scalar_zero) {
            int address = emit_value(
                p, (nyr_inst_t){.op = NYIR_ADDR_LOCAL,
                                .dst = -1,
                                .imm = (int64_t)local->slot,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
            int zero = emit_i64_constant(p, 0);
            stored = address >= 0 && zero >= 0 &&
                     lower_int128_store(p, address, 0, zero) >= 0 &&
                     lower_int128_store(p, address, 8, zero) >= 0;
          } else if (local) {
            stored = lower_local_store(p, local, initial_value);
          }
          if (!stored) {
            declaration_ok = false;
            break;
          }
          local->has_integer_constant = has_integer_constant;
          local->integer_constant = integer_constant;
        } else {
          c_type_t local_type = type;
          int local_initial = initial_value;
          if (c_type_is_complex(type)) {
            size_t complex_bytes = type == C_C64 ? 8 : type == C_C128 ? 16 : 32;
            size_t complex_offset = 0;
            if (!agg_area_alloc(p, complex_bytes, &complex_offset)) {
              declaration_ok = false;
              break;
            }
            local_type = C_AGG;
            local_initial = p->agg_base;
          }
          if (!add_local(p, name, local_type, local_initial,
                         declarator_is_const, declarator_is_volatile)) {
            declaration_ok = false;
            break;
          }
          c_local_t *local = &p->locals[p->local_count - 1];
          if (local_asm_register.len) {
            local->fixed_register = local_asm_register;
            local->fixed_register_marker = local_asm_marker;
          }
          local->cleanup = local_cleanup;
          local->agg = decl_agg;
          local->pointer_base_type = decl_pointer_base_type;
          local->pointer_depth = local_pointer_depth;
          if (decl_agg && base_type == C_AGG &&
              (type == C_AGG_PTR || type == C_AGG_PTR_PTR))
            local->agg = decl_agg;
          local->function_result_type = decl_function_result_type;
          local->is_bool = decl_is_bool && !c_type_is_pointer(type);
          if (decl_function_result_agg)
            local->agg = decl_function_result_agg;
          local->pointee_const = c_type_is_pointer(type) && p->last_type_const;
          local->is_volatile = declarator_is_volatile;
          local->pointee_volatile = pointee_volatile;
          local->pointee_seg = pointee_seg;
          local->pointee_array_size = local_pointee_array_size
                                          ? local_pointee_array_size
                                          : decl_pointee_array_size;
          local->pointee_array = local_pointee_array;
          if (c_type_is_complex(type))
            local->agg = c_complex_aggregate(type);
        }
      } while (accept(p, ","));
      if (!declaration_ok || (!static_array && !expect(p, ";")))
        break;
      continue;
    }
    if (accept(p, ";"))
      continue;
    size_t comma_expression_end = statement_end(p, p->pos);
    size_t comma_parens = 0, comma_brackets = 0, comma_braces = 0;
    bool comma_expression = false;
    for (size_t at = p->pos; at < comma_expression_end; ++at) {
      if (tok_is(p, at, "("))
        ++comma_parens;
      else if (tok_is(p, at, ")") && comma_parens)
        --comma_parens;
      else if (tok_is(p, at, "["))
        ++comma_brackets;
      else if (tok_is(p, at, "]") && comma_brackets)
        --comma_brackets;
      else if (tok_is(p, at, "{"))
        ++comma_braces;
      else if (tok_is(p, at, "}") && comma_braces)
        --comma_braces;
      else if (tok_is(p, at, ",") && !comma_parens && !comma_brackets &&
               !comma_braces) {
        comma_expression = true;
        break;
      }
    }
    if (comma_expression) {
      c_value_t ignored = parse_expr(p, 0);
      if ((ignored.value < 0 && ignored.type != C_BAD) ||
          (p->err && p->err_len && p->err[0]) || !expect(p, ";"))
        break;
      continue;
    }
    bool pointer_store_statement = false;
    if (tok_is(p, p->pos, "*")) {
      size_t end = statement_end(p, p->pos);
      size_t paren_depth = 0, bracket_depth = 0, brace_depth = 0;
      for (size_t at = p->pos + 1; at < end; ++at) {
        if (!paren_depth && !bracket_depth && !brace_depth &&
            is_assignment_operator(p, at)) {
          pointer_store_statement = true;
          break;
        }
        if (tok_is(p, at, "("))
          ++paren_depth;
        else if (tok_is(p, at, ")") && paren_depth)
          --paren_depth;
        else if (tok_is(p, at, "["))
          ++bracket_depth;
        else if (tok_is(p, at, "]") && bracket_depth)
          --bracket_depth;
        else if (tok_is(p, at, "{"))
          ++brace_depth;
        else if (tok_is(p, at, "}") && brace_depth)
          --brace_depth;
      }
    }
    if (pointer_store_statement && accept(p, "*")) {
      unsigned segment_flags;
      c_value_t address = parse_pointer_store_address(p, &segment_flags);
      bool read_only =
          p->last_value_pointee_const || c_value_pointee_const(address);
      if (!lower_pointer_store(p, address, read_only, false, segment_flags)) {
        if (!p->err[0]) {
          char message[128];
          snprintf(message, sizeof(message),
                   "pointer store failed for C type %d in `%.*s` at `%.*s`",
                   address.type,
                   p->function ? (int)(p->function->name.len > 32
                                           ? 32
                                           : p->function->name.len)
                               : 0,
                   p->function ? p->function->name.start : "",
                   (int)(current(p)->len > 32 ? 32 : current(p)->len),
                   current(p)->start);
          fail(p, message);
        }
        break;
      }
      continue;
    }
    bool indexed_postfix = false;
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "[")) {
      size_t close = skip_balanced(p, p->pos + 1, "[", "]");
      indexed_postfix = close < p->token_count &&
                        (tok_is(p, close, "++") || tok_is(p, close, "--"));
    }
    if (!indexed_postfix && tok_ident(*current(p)) &&
        tok_is(p, p->pos + 1, "[")) {
      ny_ctok_t name = *current(p);
      c_local_t *indexed = find_local(p, name);
      c_global_t *indexed_global = indexed ? NULL : find_global(p, name);
      bool global_array = indexed_global && indexed_global->is_array &&
                          indexed_global->type != C_AGG;
      bool global_pointer = indexed_global && !indexed_global->is_array &&
                            c_type_is_pointer(indexed_global->type);
      size_t indexed_close =
          (indexed || indexed_global) && tok_is(p, p->pos + 1, "[")
              ? skip_balanced(p, p->pos + 1, "[", "]")
              : p->token_count;
      bool aggregate_pointer_member =
          indexed && indexed->type == C_AGG_PTR && indexed->agg &&
          (!indexed->array_rank ||
           (indexed_close < p->token_count &&
            (tok_is(p, indexed_close, "->") || tok_is(p, indexed_close, "."))));
      bool global_aggregate_pointer_member =
          indexed_global && indexed_global->type == C_AGG_PTR &&
          indexed_global->agg && !indexed_global->is_array &&
          indexed_close < p->token_count &&
          (tok_is(p, indexed_close, "->") || tok_is(p, indexed_close, "."));
      bool global_aggregate_pointer_array_member =
          indexed_global && indexed_global->type == C_AGG_PTR &&
          indexed_global->agg && indexed_global->is_array &&
          indexed_close < p->token_count &&
          (tok_is(p, indexed_close, "->") || tok_is(p, indexed_close, "."));
      size_t pointer_array_member_end = indexed_close + 2;
      bool global_aggregate_pointer_array_call =
          global_aggregate_pointer_array_member &&
          tok_ident(p->tokens[indexed_close + 1]) &&
          tok_is(p, pointer_array_member_end, "(");
      bool global_function_pointer_array_call =
          indexed_global && indexed_global->is_function_pointer_array &&
          indexed_close < p->token_count && tok_is(p, indexed_close, "(");
      bool local_function_pointer_array_call =
          indexed && indexed->type == C_FUNC_PTR && indexed->array_rank &&
          indexed_close < p->token_count && tok_is(p, indexed_close, "(");
      if (global_aggregate_pointer_array_call ||
          global_function_pointer_array_call ||
          local_function_pointer_array_call) {
        c_value_t ignored = parse_expr(p, 1);
        if ((ignored.value < 0 && ignored.type != C_BAD) ||
            (p->err && p->err_len && p->err[0]) || !expect(p, ";"))
          break;
        continue;
      }
      bool nested_pointer_assignment =
          indexed && c_type_is_pointer(indexed->type) &&
          (indexed->pointer_depth > 1 || indexed->type == C_U16_PTR_PTR ||
           indexed->type == C_AGG_PTR_PTR);
      bool nested_array_pointer_assignment =
          indexed && indexed->array_rank && c_type_is_pointer(indexed->type) &&
          indexed_close < p->token_count && tok_is(p, indexed_close, "[");
      if (((indexed && indexed->type == C_AGG &&
            !(indexed->array_rank && indexed->pointer_depth)) ||
           aggregate_pointer_member || global_aggregate_pointer_member ||
           nested_pointer_assignment || nested_array_pointer_assignment ||
           global_aggregate_pointer_array_member ||
           (indexed_global && indexed_global->type == C_AGG &&
            indexed_global->is_array)) &&
          !global_aggregate_pointer_array_call) {
        /*
         * struct array element assignment: points[i].field = v / points[i] = v
         */
        if (!lower_scalar_assignment(p, true))
          break;
        continue;
      }
      ++p->pos;
      c_local_t *local = indexed;
      ++p->pos;
      c_value_t index = parse_expr(p, 1);
      bool local_array = local && local->array_rank != 0;
      bool pointer_local = local && c_type_is_object_pointer(local->type);
      if ((!local && !global_array && !global_pointer) ||
          (local && !local_array && !pointer_local) || index.value < 0 ||
          !c_type_is_integer(index.type) || !expect(p, "]")) {
        fail(p,
             "indexed assignment requires a pointer and signed integer index");
        break;
      }
      int base = -1;
      if (local) {
        base = lower_local_load(p, local);
      } else {
        static const char data_prefix[] = "__nyc_data_";
        static const char extern_prefix[] = "__nyc_extern_";
        const char *prefix =
            indexed_global->is_external || indexed_global->is_weak
                ? extern_prefix
                : data_prefix;
        size_t prefix_len = strlen(prefix);
        size_t symbol_len = prefix_len + indexed_global->name.len;
        char *symbol = malloc(symbol_len + 1);
        if (!symbol) {
          fail(p, "out of memory lowering a file-scope array assignment");
          break;
        }
        memcpy(symbol, prefix, prefix_len);
        memcpy(symbol + prefix_len, indexed_global->name.start,
               indexed_global->name.len);
        symbol[symbol_len] = '\0';
        base = emit_value(
            p,
            (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = symbol});
        free(symbol);
        if (base >= 0 && global_pointer)
          base =
              emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = base,
                                         .effects = NYIR_EFFECT_READ_MEMORY |
                                                    NYIR_EFFECT_MAY_TRAP,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
      }
      if (base < 0)
        break;
      if (!local && global_array && indexed_global->array_rank > 1) {
        c_array_shape_t shape = indexed_global->array_shape;
        int element_address = base;
        c_value_t dimension_index = index;
        bool indexed_ok =
            shape.rank == indexed_global->array_rank && shape.element_size != 0;
        for (unsigned dimension = 0; indexed_ok && dimension < shape.rank;
             ++dimension) {
          if (dimension) {
            if (!accept(p, "[")) {
              indexed_ok = false;
              break;
            }
            dimension_index = parse_expr(p, 1);
            if (dimension_index.value < 0 ||
                !c_type_is_integer(dimension_index.type) || !expect(p, "]")) {
              indexed_ok = false;
              break;
            }
          }
          size_t stride = shape.element_size;
          for (unsigned inner = dimension + 1; inner < shape.rank; ++inner) {
            if (shape.extent[inner] &&
                stride > SIZE_MAX / shape.extent[inner]) {
              fail(p, "multidimensional array row stride overflows");
              indexed_ok = false;
              break;
            }
            stride *= shape.extent[inner];
          }
          if (!indexed_ok || stride > INT64_MAX) {
            if (!p->err[0])
              fail(p, "multidimensional array row stride exceeds the supported "
                      "range");
            indexed_ok = false;
            break;
          }
          int scale = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                 .dst = -1,
                                                 .imm = (int64_t)stride});
          int offset =
              scale < 0 ? -1
                        : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                     .dst = -1,
                                                     .a = dimension_index.value,
                                                     .b = scale});
          element_address =
              offset < 0 ? -1
                         : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                      .dst = -1,
                                                      .a = element_address,
                                                      .b = offset});
          indexed_ok = element_address >= 0;
        }
        if (!indexed_ok)
          break;
        c_value_t element = {element_address,
                             c_array_pointer_type(indexed_global->type), NULL};
        if (!lower_pointer_store(p, element, indexed_global->is_const, false,
                                 0))
          break;
        continue;
      }
      c_type_t base_pointer =
          local            ? (local_array ? c_array_pointer_type(local->type)
                              : local->type == C_CHAR_PTR_PTR ||
                                      local->type == C_I8_PTR_PTR ||
                                      local->type == C_AGG_PTR_PTR ||
                                      local->type == C_FUNC_PTR_PTR ||
                                      local->type == C_OPAQUE_PTR_PTR
                                  ? C_OPAQUE_PTR
                                  : local->type)
          : global_pointer ? indexed_global->type
          : indexed_global->is_pointer_array ? C_OPAQUE_PTR
          : indexed_global->type == C_CHAR_PTR
              ? C_CHAR_PTR
              : c_array_pointer_type(indexed_global->type);
      c_type_t element_pointer =
          local            ? (local_array ? c_array_pointer_type(local->type)
                              : local->type == C_CHAR_PTR_PTR ||
                                      local->type == C_I8_PTR_PTR ||
                                      local->type == C_AGG_PTR_PTR ||
                                      local->type == C_FUNC_PTR_PTR ||
                                      local->type == C_OPAQUE_PTR_PTR
                                  ? C_OPAQUE_PTR
                                  : local->type)
          : global_pointer ? indexed_global->type
          : indexed_global->is_pointer_array
              ? indexed_global->is_function_pointer_array ? C_FUNC_PTR_PTR
                : indexed_global->type == C_CHAR_PTR      ? C_CHAR_PTR_PTR
                : indexed_global->type == C_AGG_PTR       ? C_AGG_PTR_PTR
                                                          : C_OPAQUE_PTR_PTR
          : indexed_global->type == C_CHAR_PTR
              ? C_CHAR_PTR
              : c_array_pointer_type(indexed_global->type);
      int address =
          lower_pointer_index_address(p, base_pointer, base, index.value);
      if (address < 0)
        break;
      c_value_t pointer = {address, element_pointer,
                           indexed_global && indexed_global->type == C_AGG_PTR
                               ? indexed_global->agg
                               : NULL};
      if (!lower_pointer_store(
              p, pointer,
              local ? ((local->is_const && !pointer_local) ||
                       local->pointee_const)
                    : indexed_global->is_const,
              false,
              local && pointer_local ? c_seg_flags(local->pointee_seg) : 0))
        break;
      continue;
    }
    if (tok_ident(*current(p))) {
      size_t postfix_pos = p->pos;
      while ((tok_is(p, postfix_pos + 1, ".") ||
              tok_is(p, postfix_pos + 1, "->")) &&
             tok_ident(p->tokens[postfix_pos + 2]))
        postfix_pos += 2;
      if ((tok_is(p, postfix_pos + 1, "++") ||
           tok_is(p, postfix_pos + 1, "--")) &&
          tok_is(p, postfix_pos + 2, ";")) {
        c_value_t ignored = parse_expr(p, 1);
        if (ignored.value < 0 || !expect(p, ";"))
          break;
        continue;
      }
    }
    bool aggregate_member_call = false;
    if (tok_ident(*current(p)) &&
        (tok_is(p, p->pos + 1, ".") || tok_is(p, p->pos + 1, "->"))) {
      size_t member_pos = p->pos;
      while (tok_is(p, member_pos + 1, ".") ||
             tok_is(p, member_pos + 1, "->")) {
        member_pos += 2;
        if (member_pos >= p->token_count || !tok_ident(p->tokens[member_pos]))
          break;
      }
      aggregate_member_call = tok_is(p, member_pos + 1, "(");
    }
    if (tok_ident(*current(p)) &&
        (is_assignment_operator(p, p->pos + 1) ||
         (!aggregate_member_call &&
          (find_local(p, *current(p)) ||
           ((find_global(p, *current(p)) != NULL) &&
            (find_global(p, *current(p))->type == C_AGG ||
             find_global(p, *current(p))->type == C_AGG_PTR))) &&
          (tok_is(p, p->pos + 1, ".") || tok_is(p, p->pos + 1, "->"))))) {
      c_global_t *global = find_global(p, *current(p));
      if (global && !find_local(p, *current(p)) &&
          is_assignment_operator(p, p->pos + 1)) {
        if (!lower_global_assignment(p, global, true))
          break;
        continue;
      }
      if (!lower_scalar_assignment(p, true))
        break;
      continue;
    }
    if (((tok_is(p, p->pos, "++") || tok_is(p, p->pos, "--")) &&
         tok_ident(p->tokens[p->pos + 1]) && tok_is(p, p->pos + 2, ";")) ||
        (tok_ident(*current(p)) &&
         (tok_is(p, p->pos + 1, "++") || tok_is(p, p->pos + 1, "--")) &&
         tok_is(p, p->pos + 2, ";"))) {
      c_value_t ignored = parse_expr(p, 1);
      if (ignored.value < 0 || !expect(p, ";"))
        break;
      continue;
    }
    size_t parenthesized_deref_after =
        tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "*")
            ? skip_balanced(p, p->pos, "(", ")")
            : p->token_count;
    if (parenthesized_deref_after < p->token_count && tok_is(p, p->pos, "(") &&
        tok_is(p, p->pos + 1, "*") &&
        is_assignment_operator(p, parenthesized_deref_after)) {
      p->pos += 2;
      unsigned segment_flags;
      c_value_t address = parse_pointer_store_address(p, &segment_flags);
      if (address.value < 0 ||
          !lower_pointer_store(p, address,
                               p->last_value_pointee_const ||
                                   c_value_pointee_const(address),
                               true, segment_flags))
        break;
      continue;
    }
    bool expression_terminates_path =
        tok_is(p, p->pos, "__builtin_unreachable") &&
        tok_is(p, p->pos + 1, "(");
    size_t expression_start = p->pos;
    c_value_t expression = parse_expr(p, 0);
    bool expression_parsed =
        p->pos > expression_start && (!p->err || !p->err_len || !p->err[0]) &&
        (expression.value >= 0 || expression.type == C_BAD);
    if (expression_parsed && expect(p, ";")) {
      if (expression_terminates_path)
        *returned = true;
      continue;
    }
    char message[192];
    snprintf(message, sizeof(message),
             "unsupported C statement beginning with `%.*s`",
             (int)(current(p)->len > 120 ? 120 : current(p)->len),
             current(p)->start);
    fail(p, message);
    break;
  }
  bool ok =
      (p->pos == end || *returned) && (!p->err || !p->err_len || !p->err[0]);
  if (!ok && p->err && p->err_len && !p->err[0]) {
    ny_ctok_t token = *current(p);
    ny_ctok_t first = start < p->token_count ? p->tokens[start] : token;
    char message[384];
    ny_ctok_t previous = p->pos ? p->tokens[p->pos - 1] : (ny_ctok_t){0};
    ny_ctok_t next =
        p->pos + 1 < p->token_count ? p->tokens[p->pos + 1] : (ny_ctok_t){0};
    snprintf(message, sizeof(message),
             "could not lower C statement at %zu/%zu (`%.*s`; starts "
             "`%.*s`; prev `%.*s`, next `%.*s`; scalar=%d function=%d "
             "typedef=%d)",
             p->pos, end, (int)(token.len > 120 ? 120 : token.len),
             token.start ? token.start : "",
             (int)(first.len > 36 ? 36 : first.len),
             first.start ? first.start : "",
             (int)(previous.len > 40 ? 40 : previous.len),
             previous.start ? previous.start : "",
             (int)(next.len > 40 ? 40 : next.len), next.start ? next.start : "",
             starts_scalar_type(p), find_function(p, token) != NULL,
             find_typedef(p, token) != NULL);
    fail(p, message);
  }
  if (ok && start != saved_pos && !*returned)
    p->pos = saved_pos;
  return ok;
}
