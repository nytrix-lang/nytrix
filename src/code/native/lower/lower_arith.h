static int ny_native_nir_lower_binary(ny_native_nir_builder_t *b,
                                   const expr_t *e) {
  if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left);
    bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right);
    if (left_list != right_list) {
      const expr_t *list_expr = left_list ? e->as.binary.left
                                          : e->as.binary.right;
      const expr_t *count_expr = left_list ? e->as.binary.right
                                           : e->as.binary.left;
      if (ny_native_nir_expr_is_f64(b, count_expr) ||
          ny_native_nir_expr_is_f32(b, count_expr) ||
          ny_native_nir_expr_is_cstr(b, count_expr) ||
          ny_native_nir_expr_is_any(b, count_expr)) {
        ny_native_nir_fail(
            b, "native NYIR lower: list repeat count must be an integer");
        return -1;
      }
      int list = ny_native_nir_lower_expr(b, list_expr);
      int count = ny_native_nir_lower_expr(b, count_expr);
      if (list < 0 || count < 0)
        return -1;
      int out = ny_native_nir_emit_runtime_call(
          b, "rt_native_tbuf_repeat", list, count, -1, 2, 0);
      if (out < 0)
        return -1;

      int source_len = ny_native_nir_peek_list_len_fact(b, list);
      int64_t repeat = -1;
      const expr_t *lit = count_expr;
      bool negate = false;
      if (lit && lit->kind == NY_E_UNARY && lit->as.unary.op &&
          (strcmp(lit->as.unary.op, "+") == 0 ||
           strcmp(lit->as.unary.op, "-") == 0)) {
        negate = strcmp(lit->as.unary.op, "-") == 0;
        lit = lit->as.unary.right;
      }
      if (lit && lit->kind == NY_E_LITERAL &&
          lit->as.literal.kind == NY_LIT_INT) {
        repeat = lit->as.literal.as.i;
        if (negate)
          repeat = repeat == INT64_MIN ? INT64_MIN : -repeat;
      }
      if (source_len >= 0 && repeat >= 0) {
        int out_len = -1;
        if (repeat == 0) {
          out_len = ny_native_nir_emit_const(b, 0);
        } else {
          int repeat_v = ny_native_nir_emit_const(b, repeat);
          out_len = repeat_v < 0
                        ? -1
                        : ny_native_nir_push_val(
                              b, NYIR_MUL_I64, source_len, repeat_v, 0, NULL);
        }
        if (out_len < 0 ||
            !ny_native_nir_record_list_len_fact(b, out, out_len))
          return -1;
      }
      int64_t source_bytes = ny_native_nir_peek_alloc_fact(b, list);
      if (source_bytes >= 0 && repeat >= 0 &&
          (repeat == 0 || source_bytes <= INT64_MAX / repeat))
        ny_native_nir_record_alloc_fact(b, out, source_bytes * repeat);
      return out;
    }
  }
  if (e->as.binary.op && strcmp(e->as.binary.op, "^") == 0) {
    int64_t folded = 0;
    if (ny_native_nir_fold_const_pow(e->as.binary.left,
                                     e->as.binary.right, &folded))
      return ny_native_nir_emit_const(b, folded);
    ny_native_nir_fail(
        b, "native NYIR lower: power requires non-negative integer constants without overflow");
    return -1;
  }
  nyir_op_t op = NYIR_NOP;
  nyir_cmp_t cmp = NYIR_CMP_EQ;
  bool is_cmp = ny_native_nir_cmp(e->as.binary.op, &cmp);
  if (!is_cmp && !ny_native_nir_binop(e->as.binary.op, &op)) {
    ny_native_nir_fail(b, "native NYIR lower: unsupported binary operator '%s'",
                       e->as.binary.op ? e->as.binary.op : "(null)");
    return -1;
  }
  bool expr_f32 = !ny_native_nir_expr_is_f64(b, e) &&
                  ny_native_nir_expr_is_f32(b, e);
  bool float_mod = !is_cmp && e->as.binary.op &&
                   strcmp(e->as.binary.op, "%") == 0 &&
                   (ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
                    ny_native_nir_expr_is_f64(b, e->as.binary.right));
  if (float_mod) {
    int left = ny_native_nir_lower_expr(b, e->as.binary.left);
    int right = ny_native_nir_lower_expr(b, e->as.binary.right);
    if (left < 0 || right < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, e->as.binary.left)) {
      left = ny_native_nir_emit_i64_to_f64(b, left);
      if (left < 0)
        return -1;
    }
    if (!ny_native_nir_expr_is_f64(b, e->as.binary.right)) {
      right = ny_native_nir_emit_i64_to_f64(b, right);
      if (right < 0)
        return -1;
    }
    return ny_native_nir_emit_runtime_call(
        b, "rt_native_fmod_f64", left, right, -1, 2, NYIR_INST_F_RET_F64);
  }
  if (!is_cmp && expr_f32 &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0 || strcmp(e->as.binary.op, "/") == 0)) {
    if (strcmp(e->as.binary.op, "+") == 0)
      op = NYIR_ADD_F32;
    else if (strcmp(e->as.binary.op, "-") == 0)
      op = NYIR_SUB_F32;
    else if (strcmp(e->as.binary.op, "*") == 0)
      op = NYIR_MUL_F32;
    else
      op = NYIR_DIV_F32;
  } else if (!is_cmp && ny_native_nir_expr_is_f64(b, e) &&
             (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
              strcmp(e->as.binary.op, "*") == 0 || strcmp(e->as.binary.op, "/") == 0)) {
    if (strcmp(e->as.binary.op, "+") == 0)
      op = NYIR_ADD_F64;
    else if (strcmp(e->as.binary.op, "-") == 0)
      op = NYIR_SUB_F64;
    else if (strcmp(e->as.binary.op, "*") == 0)
      op = NYIR_MUL_F64;
    else
      op = NYIR_DIV_F64;
  }
  bool left_f64 = ny_native_nir_expr_is_f64(b, e->as.binary.left);
  bool right_f64 = ny_native_nir_expr_is_f64(b, e->as.binary.right);
  bool left_f32 = ny_native_nir_expr_is_f32(b, e->as.binary.left);
  bool right_f32 = ny_native_nir_expr_is_f32(b, e->as.binary.right);
  bool left_cstr = ny_native_nir_expr_is_cstr(b, e->as.binary.left);
  bool right_cstr = ny_native_nir_expr_is_cstr(b, e->as.binary.right);
  bool left_any = ny_native_nir_expr_is_any(b, e->as.binary.left);
  bool right_any = ny_native_nir_expr_is_any(b, e->as.binary.right);
  int a = ny_native_nir_lower_expr(b, e->as.binary.left);
  int rhs = ny_native_nir_lower_expr(b, e->as.binary.right);
  if (a < 0 || rhs < 0)
    return -1;
  bool numeric_literal_sub =
      e->as.binary.op && strcmp(e->as.binary.op, "-") == 0 &&
      e->as.binary.left && e->as.binary.right &&
      e->as.binary.left->kind == NY_E_LITERAL &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.left->as.literal.kind == NY_LIT_INT &&
      e->as.binary.right->as.literal.kind == NY_LIT_INT;
  bool any_string_op =
      (left_any || right_any) && e->as.binary.op &&
      strcmp(e->as.binary.op, "+") == 0 && !left_f64 && !right_f64 &&
      !left_f32 && !right_f32;
  if ((left_cstr || right_cstr || any_string_op) && !numeric_literal_sub) {
    if (!left_cstr) {
      a = ny_native_nir_emit_runtime_call(
          b, left_any ? "rt_native_any_to_cstr" : "rt_native_i64_to_cstr",
          a, -1, -1, 1, 0);
    }
    if (!right_cstr) {
      rhs = ny_native_nir_emit_runtime_call(
          b, right_any ? "rt_native_any_to_cstr" : "rt_native_i64_to_cstr",
          rhs, -1, -1, 1, 0);
    }
    if (a < 0 || rhs < 0)
      return -1;
    if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "+") == 0) {
      int out = ny_native_nir_emit_runtime_call(
          b, "rt_native_cstr_concat", a, rhs, -1, 2, 0);
      int length = ny_native_nir_emit_known_cstr_concat_len(b, a, rhs, out);
      int tag = length < 0 ? -1 : ny_native_nir_emit_const(b, 121);
      if (out < 0 || length < 0 || tag < 0 ||
          !ny_native_nir_record_dyn_fact(
              b, out, NY_NATIVE_NIR_FACT_DYN_STR_LEN, length) ||
          !ny_native_nir_record_dyn_fact(
              b, out, NY_NATIVE_NIR_FACT_DYN_TAG, tag))
        return -1;
      return out;
    }
    if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE)) {
      int equal = ny_native_nir_emit_runtime_call(b, "rt_native_cstr_eq", a, rhs,
                                                  -1, 2, 0);
      if (equal < 0 || cmp == NYIR_CMP_EQ)
        return equal;
      int zero = ny_native_nir_emit_const(b, 0);
      return zero < 0 ? -1 : nyir_emit(&b->nyir,
          (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1, .a = equal, .b = zero,
                        .cmp = NYIR_CMP_EQ});
    }
    ny_native_nir_fail(
        b,
        "native NYIR lower: unsupported string operator '%s' at %s:%d in %s (left_cstr=%d right_cstr=%d)",
        e->as.binary.op ? e->as.binary.op : "(null)",
        e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
        b->current_fn_name ? b->current_fn_name : "<unknown>",
        left_cstr, right_cstr);
    return -1;
  }
  bool use_f64_cmp = is_cmp && (left_f64 || right_f64);
  bool use_f32_cmp = is_cmp && !use_f64_cmp && (left_f32 || right_f32);
  if ((!is_cmp && (op == NYIR_ADD_F32 || op == NYIR_SUB_F32 ||
                   op == NYIR_MUL_F32 || op == NYIR_DIV_F32)) ||
      use_f32_cmp) {
    if (!left_f32) {
      a = ny_native_nir_emit_i64_to_f32(b, a);
      if (a < 0)
        return -1;
    }
    if (!right_f32) {
      rhs = ny_native_nir_emit_i64_to_f32(b, rhs);
      if (rhs < 0)
        return -1;
    }
  } else if ((!is_cmp && (op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
                   op == NYIR_MUL_F64 || op == NYIR_DIV_F64)) ||
      use_f64_cmp) {
    if (!left_f64) {
      if (left_any) {
        a = ny_native_nir_emit_runtime_call(
            b, "rt_native_any_to_f64", a, -1, -1, 1, NYIR_INST_F_RET_F64);
      } else {
        a = ny_native_nir_emit_i64_to_f64(b, a);
      }
      if (a < 0)
        return -1;
    }
    if (!right_f64) {
      if (right_any) {
        rhs = ny_native_nir_emit_runtime_call(
            b, "rt_native_any_to_f64", rhs, -1, -1, 1, NYIR_INST_F_RET_F64);
      } else {
        rhs = ny_native_nir_emit_i64_to_f64(b, rhs);
      }
      if (rhs < 0)
        return -1;
    }
  }
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = use_f64_cmp ? NYIR_CMP_F64
                                                     : use_f32_cmp ? NYIR_CMP_F32
                                                     : is_cmp    ? NYIR_CMP_I64
                                                                 : op,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs,
                                               .cmp = cmp});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

/*
 * Lower a call expression into NYIR.  Handles intrinsics, builtins,
 * user functions, and extern ABI.  Extracted from
 * ny_native_nir_lower_expr.  The main switch still has ~1000 lines of
 * inline lowering for intrinsics, vector ops, and special forms.
 */
