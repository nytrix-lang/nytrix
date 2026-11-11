/* C builtin expressions, included by parse.c before parse_primary. */
static c_value_t parse_builtin_choose_expr(c_parser_t *p) {
  size_t open = p->pos + 1;
  if (!tok_is(p, open, "(")) {
    fail(p, "__builtin_choose_expr requires three arguments");
    return (c_value_t){-1, C_BAD, NULL};
  }
  size_t first_comma = SIZE_MAX;
  size_t second_comma = SIZE_MAX;
  size_t close = SIZE_MAX;
  size_t paren_depth = 0;
  size_t bracket_depth = 0;
  size_t brace_depth = 0;
  for (size_t at = open + 1; at < p->token_count; ++at) {
    if (tok_is(p, at, "(")) {
      ++paren_depth;
    } else if (tok_is(p, at, ")")) {
      if (!paren_depth && !bracket_depth && !brace_depth) {
        close = at;
        break;
      }
      if (paren_depth)
        --paren_depth;
    } else if (tok_is(p, at, "[")) {
      ++bracket_depth;
    } else if (tok_is(p, at, "]") && bracket_depth) {
      --bracket_depth;
    } else if (tok_is(p, at, "{")) {
      ++brace_depth;
    } else if (tok_is(p, at, "}") && brace_depth) {
      --brace_depth;
    } else if (tok_is(p, at, ",") && !paren_depth && !bracket_depth &&
               !brace_depth) {
      if (first_comma == SIZE_MAX)
        first_comma = at;
      else if (second_comma == SIZE_MAX)
        second_comma = at;
    }
  }
  size_t condition_at = open + 1;
  int64_t condition = 0;
  bool previous_unsigned = p->global_const_unsigned;
  bool condition_ok = first_comma != SIZE_MAX &&
                      parse_global_const_expr(p, &condition_at, &condition) &&
                      condition_at == first_comma;
  p->global_const_unsigned = previous_unsigned;
  if (!condition_ok || second_comma == SIZE_MAX || close == SIZE_MAX ||
      second_comma >= close) {
    fail(p, "__builtin_choose_expr requires a constant condition and three "
            "arguments");
    return (c_value_t){-1, C_BAD, NULL};
  }
  size_t branch_start = condition ? first_comma + 1 : second_comma + 1;
  size_t branch_end = condition ? second_comma : close;
  p->pos = branch_start;
  c_value_t selected = parse_expr(p, 1);
  if (selected.value < 0 || p->pos != branch_end) {
    if (selected.value >= 0)
      fail(p, "unsupported __builtin_choose_expr branch expression");
    return (c_value_t){-1, C_BAD, NULL};
  }
  p->pos = close + 1;
  return selected;
}

static c_value_t parse_builtin_types_compatible(c_parser_t *p) {
  size_t at = p->pos;
  int64_t compatible = 0;
  if (!parse_global_types_compatible(p, &at, &compatible)) {
    fail(p, "__builtin_types_compatible_p requires two supported types");
    return (c_value_t){-1, C_BAD, NULL};
  }
  p->pos = at;
  return (c_value_t){
      emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = compatible}),
      C_I32, NULL};
}

static c_value_t parse_builtin_counted_by_ref(c_parser_t *p) {
  size_t open = p->pos + 1;
  size_t after = skip_balanced(p, open, "(", ")");
  if (!tok_is(p, open, "(") || after <= open + 1 || after > p->token_count ||
      !tok_is(p, after - 1, ")")) {
    fail(p, "__builtin_counted_by_ref requires one argument");
    return (c_value_t){-1, C_BAD, NULL};
  }
  /*
   * Its result type is void *, which Linux uses in a controlling _Generic.
   */
  p->pos = after;
  int value = emit_i64_constant(p, 0);
  return value < 0 ? (c_value_t){-1, C_BAD, NULL}
                   : (c_value_t){value, C_VOID_PTR, NULL};
}
