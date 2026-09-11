/*
 * Emit an inline divide-by-zero guard matching the LLVM/JIT path
 * (ny_emit_f64_div_zero_guard / the int divmod slow path): when the divisor
 * compares equal to zero, panic with the given message ("division by zero"
 * or "modulo by zero").  The panic block is cold; the hot path is one
 * compare + branch.  rt_panic is noreturn (longjmp/exit), so the trailing
 * ret 0 is only reachable if the runtime is built without panic support.
 */
static bool ny_native_nir_emit_div_zero_guard(ny_native_nir_builder_t *b,
                                              nyir_op_t cmp_op, int divisor,
                                              const char *msg) {
  int zero = cmp_op == NYIR_CMP_F32 ? ny_native_nir_emit_const_f32(b, 0.0)
              : cmp_op == NYIR_CMP_F64 ? ny_native_nir_emit_const_f64(b, 0.0)
                                       : ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return false;
  int is_zero = nyir_emit(&b->nyir,
                          (nyir_inst_t){.op = cmp_op,
                                        .dst = -1,
                                        .a = divisor,
                                        .b = zero,
                                        .cmp = NYIR_CMP_EQ});
  if (is_zero < 0)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  int panic_label = b->next_label++;
  int ok_label = b->next_label++;
  if (!ny_native_nir_emit_br_if(b, is_zero, panic_label) ||
      !ny_native_nir_emit_br(b, ok_label) ||
      !ny_native_nir_emit_label(b, panic_label))
    return false;
  int msg_v = ny_native_nir_emit_cstr_const(b, msg ? msg : "division by zero");
  if (msg_v < 0)
    return false;
  int tagged =
      ny_native_nir_emit_runtime_call(b, "rt_alloc_string", msg_v, -1, -1, 1, 0);
  if (tagged < 0 ||
      ny_native_nir_emit_runtime_call(b, "rt_panic", tagged, -1, -1, 1, 0) < 0)
    return false;
  int zero_i = ny_native_nir_emit_const(b, 0);
  if (zero_i < 0)
    return false;
  /*
   * The panic block's ret terminates only that cold block; it must not mark
   * the enclosing statement/function as having returned, or the statement
   * iterators would truncate the body after the first guarded division.
   */
  bool saved_return = b->emitted_return;
  bool ok = ny_native_nir_emit_ret(b, zero_i);
  b->emitted_return = saved_return;
  if (!ok)
    return false;
  return ny_native_nir_emit_label(b, ok_label);
}

static bool ny_native_nir_is_bitwise_operator(const char *op) {
  return op && (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 ||
                strcmp(op, ">>") == 0);
}

static bool ny_native_nir_expr_is_bitwise_tree(
    const ny_native_nir_builder_t *b, const expr_t *e, unsigned depth) {
  if (!e || depth > 16)
    return false;
  if (e->kind == NY_E_BINARY)
    return ny_native_nir_is_bitwise_operator(e->as.binary.op);
  if (e->kind != NY_E_IDENT)
    return false;
  const expr_t *value = ny_native_nir_find_top_level_value(b, e->as.ident.name);
  return value && value != e &&
         ny_native_nir_expr_is_bitwise_tree(b, value, depth + 1);
}

static bool ny_native_nir_expr_is_immutable_nil(
    const ny_native_nir_builder_t *b, const expr_t *e, unsigned depth) {
  if (!e || depth > 32)
    return false;
  if (ny_expr_is_nil_literal((expr_t *)e))
    return true;
  if (e->kind != NY_E_IDENT || !e->as.ident.name ||
      (e->semantic.resolved && e->semantic.mutable_value))
    return false;
  /* Top-level bindings are live storage, including `mut x = nil` and
   * bindings later assigned through a try/catch handler.  Their initializer
   * is not a constant proof; folding it here turns a valid runtime comparison
   * into a permanent false result. */
  if (ny_native_globaltab_has(e->as.ident.name))
    return false;
  /* Module globals are registered under their qualified symbol name.  A
   * bare reference inside that module must still be treated as live storage;
   * otherwise a mutable `mut registry = nil` is folded to an immutable nil
   * and guards such as `registry != nil` become permanently false. */
  const char *global_symbol = NULL;
  if (b->module_name && b->module_name[0]) {
    char qualified[512];
    int n = snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name,
                     e->as.ident.name);
    if (n > 0 && (size_t)n < sizeof(qualified))
      global_symbol = ny_native_globaltab_name(qualified);
  }
  if (!global_symbol && b->current_fn_name) {
    const char *dot = strrchr(b->current_fn_name, '.');
    if (dot && dot != b->current_fn_name && dot[1]) {
      char qualified[512];
      int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                       (int)(dot - b->current_fn_name), b->current_fn_name,
                       e->as.ident.name);
      if (n > 0 && (size_t)n < sizeof(qualified))
        global_symbol = ny_native_globaltab_name(qualified);
    }
  }
  if (global_symbol || ny_native_globaltab_name_tail(e->as.ident.name))
    return false;
  const expr_t *value = ny_native_nir_find_top_level_value(b, e->as.ident.name);
  return value && value != e &&
         ny_native_nir_expr_is_immutable_nil(b, value, depth + 1);
}

static int ny_native_nir_lower_binary(ny_native_nir_builder_t *b,
                                   const expr_t *e) {
  /* User-defined attached operators are represented as NY_S_OPERATOR
   * declarations rather than ordinary binary call nodes.  Dispatch them
   * before the scalar/list arithmetic ladder; otherwise a nominal object such
   * as `SelfBox` is treated as two raw pointer-sized integers. */
  const stmt_t *attached = ny_native_nir_find_operator(b, e);
  if (attached && attached->as.oper.target &&
      attached->as.oper.target[0]) {
    int left = ny_native_nir_lower_expr(b, e->as.binary.left);
    int right = ny_native_nir_lower_expr(b, e->as.binary.right);
    if (left < 0 || right < 0)
      return -1;
    const char *owner = ny_native_nir_expr_type_name(b, e->as.binary.left);
    char symbol[512];
    const char *target = attached->as.oper.target;
    int n = (strchr(target, '.') || !owner)
                ? snprintf(symbol, sizeof(symbol), "%s", target)
                : snprintf(symbol, sizeof(symbol), "%s.%s", owner, target);
    if (n <= 0 || (size_t)n >= sizeof(symbol)) {
      ny_native_nir_fail(b, "native NYIR: attached operator target is too long");
      return -1;
    }
    return nyir_emit(&b->nyir,
                     (nyir_inst_t){.op = NYIR_CALL,
                                   .dst = -1,
                                   .a = left,
                                   .b = right,
                                   .c = -1,
                                   .imm = 2,
                                   .flags = 0,
                                   .symbol = symbol});
  }
  if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left);
    bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right);
    if (left_list != right_list) {
      const expr_t *list_expr = left_list ? e->as.binary.left
                                          : e->as.binary.right;
      const expr_t *count_expr = left_list ? e->as.binary.right
                                           : e->as.binary.left;
      bool concrete_list = list_expr->kind == NY_E_LIST;
      if (list_expr->kind == NY_E_IDENT) {
        ny_native_nir_local_t *local = ny_native_nir_find_local(
            b, list_expr->as.ident.name);
        concrete_list = local &&
                        (local->is_list ||
                         local->semantic_rep == NY_SEM_REP_TYPED_BUFFER);
        if (!concrete_list) {
          const expr_t *global = ny_native_nir_find_top_level_value(
              b, list_expr->as.ident.name);
          concrete_list = global && global != list_expr &&
                          global->kind == NY_E_LIST;
        }
      } else if (list_expr->kind == NY_E_CALL ||
                 list_expr->kind == NY_E_MEMCALL) {
        concrete_list = ny_native_nir_expr_is_list(b, list_expr);
      }
      if (!concrete_list)
        goto scalar_multiply;
      /* Semantic list evidence can be stale for untyped multi-assignment
       * temporaries in arithmetic-heavy code.  A value proven to be a float
       * is scalar multiplication, never list repetition. */
      if (ny_native_nir_expr_is_f64(b, list_expr) ||
          ny_native_nir_expr_is_f32(b, list_expr) ||
          ny_native_nir_expr_is_cstr(b, list_expr))
        goto scalar_multiply;
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
          b, "rt_tbuf_repeat", list, count, -1, 2, 0);
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
scalar_multiply:
  if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0 &&
      (ny_native_nir_expr_is_cstr(b, e->as.binary.left) ||
       ny_native_nir_expr_is_cstr(b, e->as.binary.right))) {
    const expr_t *str_expr = ny_native_nir_expr_is_cstr(
                                 b, e->as.binary.left)
                                 ? e->as.binary.left
                                 : e->as.binary.right;
    const expr_t *count_expr = str_expr == e->as.binary.left
                                   ? e->as.binary.right
                                   : e->as.binary.left;
    if (ny_native_nir_expr_is_f64(b, count_expr) ||
        ny_native_nir_expr_is_f32(b, count_expr) ||
        ny_native_nir_expr_is_any(b, count_expr))
      return ny_native_nir_fail(
          b, "native NYIR lower: string repeat count must be an integer");
    int str = ny_native_nir_lower_expr(b, str_expr);
    int count = ny_native_nir_lower_expr(b, count_expr);
    if (str < 0 || count < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, "rt_cstr_repeat", str, count, -1, 2, 0);
  }
  /* BigInt operators must stay in the handle domain.  The generic arithmetic
   * path below is intentionally raw-i64/f64 and would otherwise treat the
   * heap addresses as integers (or route `^` through floating power). */
  bool operator_left_bigint =
      ny_native_nir_expr_is_bigint(b, e->as.binary.left);
  bool operator_right_bigint =
      ny_native_nir_expr_is_bigint(b, e->as.binary.right);
  if (operator_left_bigint || operator_right_bigint) {
    const char *big_symbol = NULL;
    if (e->as.binary.op && strcmp(e->as.binary.op, "+") == 0)
      big_symbol = "__bigint_add";
    else if (e->as.binary.op && strcmp(e->as.binary.op, "-") == 0)
      big_symbol = "__bigint_sub";
    else if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0)
      big_symbol = "__bigint_mul";
    else if (e->as.binary.op && strcmp(e->as.binary.op, "/") == 0)
      big_symbol = "__bigint_div";
    else if (e->as.binary.op && strcmp(e->as.binary.op, "%") == 0)
      big_symbol = "__bigint_mod";
    else if (e->as.binary.op && strcmp(e->as.binary.op, "^") == 0)
      big_symbol = "__bigint_pow";
    else if (e->as.binary.op && strcmp(e->as.binary.op, "^^") == 0)
      big_symbol = "__bigint_xor";
    if (big_symbol) {
      int left = ny_native_nir_lower_expr(b, e->as.binary.left);
      int right = ny_native_nir_lower_expr(b, e->as.binary.right);
      if (left < 0 || right < 0)
        return -1;
      if (!operator_left_bigint)
        left = ny_native_nir_emit_runtime_call(
            b, "rt_bigint_from_i64_raw", left, -1, -1, 1, 0);
      if (!operator_right_bigint)
        right = ny_native_nir_emit_runtime_call(
            b, "rt_bigint_from_i64_raw", right, -1, -1, 1, 0);
      return left < 0 || right < 0
                 ? -1
                 : ny_native_nir_emit_runtime_call(b, big_symbol, left, right,
                                                   -1, 2, 0);
    }
  }
  if (e->as.binary.op && strcmp(e->as.binary.op, "^") == 0) {
    int64_t folded = 0;
    if (ny_native_nir_fold_const_pow(e->as.binary.left,
                                     e->as.binary.right, &folded)) {
      int c = ny_native_nir_emit_const(b, folded);
      if (c < 0)
        return -1;
      return c;
    }
    /*
     * Non-constant or negative/fractional exponent: fall back to the
     * runtime double-power instead of failing the whole compile.  Integer
     * operands are widened to f64; the result carries the float ABI flag.
     */
    int base_v = ny_native_nir_lower_expr(b, e->as.binary.left);
    int exp_v = ny_native_nir_lower_expr(b, e->as.binary.right);
    if (base_v < 0 || exp_v < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, e->as.binary.left)) {
      base_v = ny_native_nir_emit_i64_to_f64(b, base_v);
      if (base_v < 0)
        return -1;
    }
    if (!ny_native_nir_expr_is_f64(b, e->as.binary.right)) {
      exp_v = ny_native_nir_emit_i64_to_f64(b, exp_v);
      if (exp_v < 0)
        return -1;
    }
    return ny_native_nir_emit_runtime_call(
        b, "rt_f64_pow", base_v, exp_v, -1, 2, NYIR_INST_F_RET_F64);
  }
  /* Promote compile-time arithmetic that crosses the tagged integer boundary
   * before it reaches raw NYIR i64 arithmetic.  This is especially important
   * for module globals such as `(1 << 62) - 1 + 1`: the result is a bigint
   * handle, not the raw payload 1 << 62. */
  if (e->as.binary.op &&
      (strcmp(e->as.binary.op, "+") == 0 ||
       strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0)) {
    int64_t folded = 0;
    if (ny_native_nir_fold_top_level_int(b->prog, e, &folded, 0) &&
        (folded >= (INT64_C(1) << 62) ||
         folded <= -(INT64_C(1) << 62))) {
      int raw = ny_native_nir_emit_const(b, folded);
      return raw < 0 ? -1 : ny_native_nir_emit_runtime_call(
                              b, "rt_bigint_from_i64_raw", raw, -1, -1, 1, 0);
    }
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
        b, "rt_fmod_f64", left, right, -1, 2, NYIR_INST_F_RET_F64);
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
  bool left_scalar_get =
      e->as.binary.left && e->as.binary.left->kind == NY_E_CALL &&
      ny_native_call_leaf(e->as.binary.left) &&
      strcmp(ny_native_call_leaf(e->as.binary.left), "get") == 0 &&
      e->as.binary.left->as.call.args.len >= 2 &&
      (ny_native_nir_expr_is_list(
           b, e->as.binary.left->as.call.args.data[0].val) ||
       ny_native_nir_expr_is_dict(
           b, e->as.binary.left->as.call.args.data[0].val));
  bool right_scalar_get =
      e->as.binary.right && e->as.binary.right->kind == NY_E_CALL &&
      ny_native_call_leaf(e->as.binary.right) &&
      strcmp(ny_native_call_leaf(e->as.binary.right), "get") == 0 &&
      e->as.binary.right->as.call.args.len >= 2 &&
      (ny_native_nir_expr_is_list(
           b, e->as.binary.right->as.call.args.data[0].val) ||
       ny_native_nir_expr_is_dict(
           b, e->as.binary.right->as.call.args.data[0].val));
  /* The method spelling `list.get(i, d)` lowers to the same tagged dynamic
   * ABI as the free `get(...)` call, but semantic inference resolves its
   * result to the scalar element type, so the comparison path below treats
   * the boxed payload as a raw integer.  Detect it explicitly and unbox it
   * with the scalar `get` fix above.  Excluded when the method get itself
   * converts to f64 (`rt_any_to_f64` in its own boundary) because that path
   * returns raw float bits, not a tagged value. */
  bool left_scalar_get_mem =
      e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      (e->as.binary.left->as.memcall.args.len == 1 ||
       e->as.binary.left->as.memcall.args.len == 2) &&
      e->as.binary.left->as.memcall.target &&
      (ny_native_nir_expr_is_list(b, e->as.binary.left->as.memcall.target) ||
       ny_native_nir_expr_is_dict(b, e->as.binary.left->as.memcall.target)) &&
      !ny_native_nir_expr_is_dyn_list(b, e->as.binary.left->as.memcall.target) &&
      !ny_native_nir_expr_is_f64(b, e->as.binary.left) &&
      !(e->as.binary.left->as.memcall.args.len == 2 &&
        e->as.binary.left->as.memcall.args.data[1].val &&
        ny_native_nir_expr_is_f64(
            b, e->as.binary.left->as.memcall.args.data[1].val));
  bool right_scalar_get_mem =
      e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      (e->as.binary.right->as.memcall.args.len == 1 ||
       e->as.binary.right->as.memcall.args.len == 2) &&
      e->as.binary.right->as.memcall.target &&
      (ny_native_nir_expr_is_list(b, e->as.binary.right->as.memcall.target) ||
       ny_native_nir_expr_is_dict(b, e->as.binary.right->as.memcall.target)) &&
      !ny_native_nir_expr_is_dyn_list(b, e->as.binary.right->as.memcall.target) &&
      !ny_native_nir_expr_is_f64(b, e->as.binary.right) &&
      !(e->as.binary.right->as.memcall.args.len == 2 &&
        e->as.binary.right->as.memcall.args.data[1].val &&
        ny_native_nir_expr_is_f64(
            b, e->as.binary.right->as.memcall.args.data[1].val));
  /* Arithmetic on an `any` value produces a tagged dynamic result even when
   * HM refines the enclosing expression to int. Preserve that provenance so
   * comparisons use the dynamic ABI (for example `v % 2 == 0`). */
  bool left_dynamic_result = left_any;
  bool right_dynamic_result = right_any;
  /* A list `.get` is an any-valued boundary even when its receiver's
   * unparameterized list type made semantic inference look container-like.
   * Preserve that provenance so nested lists use structural equality instead
   * of pointer comparison and scalar slots are decoded exactly once. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      e->as.binary.left->as.memcall.target &&
      ny_native_nir_expr_is_dyn_list(
          b, e->as.binary.left->as.memcall.target))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      e->as.binary.right->as.memcall.target &&
      ny_native_nir_expr_is_dyn_list(
          b, e->as.binary.right->as.memcall.target))
    right_dynamic_result = true;
  /* Dictionary `.get` also returns a tagged dynamic value.  Preserve that
   * provenance even when semantic inference narrows the result to a scalar;
   * otherwise equality compares its tag against an unboxed literal. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      e->as.binary.left->as.memcall.target &&
      ny_native_nir_expr_is_dict(b, e->as.binary.left->as.memcall.target))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      e->as.binary.right->as.memcall.target &&
      ny_native_nir_expr_is_dict(b, e->as.binary.right->as.memcall.target))
    right_dynamic_result = true;
  /* Direct indexing is the same dynamic boundary as `.get`.  Keep the
   * provenance on the expression itself so `seq[1] == 2` uses the tagged
   * comparator just like `def v = seq[1]; v == 2`.  Without this marker the
   * native path treats the indexed value as a raw i64 and compares its boxed
   * payload against the tagged literal. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
      e->as.binary.left->as.index.target &&
      ny_native_nir_expr_is_dyn_list(b,
                                     e->as.binary.left->as.index.target))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
      e->as.binary.right->as.index.target &&
      ny_native_nir_expr_is_dyn_list(b,
                                     e->as.binary.right->as.index.target))
    right_dynamic_result = true;
  /* Typed list reads stay raw for scalar arithmetic, but a list element that
   * is compared with another sequence is a nested container boundary. Route
   * that case through the structural dynamic comparator. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      e->as.binary.left->as.memcall.target &&
      ny_native_nir_expr_is_list(b, e->as.binary.left->as.memcall.target) &&
      (ny_native_nir_expr_is_list(b, e->as.binary.right) ||
       ny_native_nir_expr_is_dyn_list(b, e->as.binary.right)))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      e->as.binary.right->as.memcall.target &&
      ny_native_nir_expr_is_list(b, e->as.binary.right->as.memcall.target) &&
      (ny_native_nir_expr_is_list(b, e->as.binary.left) ||
       ny_native_nir_expr_is_dyn_list(b, e->as.binary.left)))
    right_dynamic_result = true;
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_BINARY)
    left_dynamic_result =
        left_dynamic_result ||
        ny_native_nir_expr_is_any(b, e->as.binary.left->as.binary.left) ||
        ny_native_nir_expr_is_any(b, e->as.binary.left->as.binary.right);
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_BINARY &&
      e->as.binary.left->as.binary.op &&
      strcmp(e->as.binary.left->as.binary.op, "%") == 0 &&
      (ny_native_nir_expr_is_any(b, e->as.binary.left->as.binary.left) ||
       ny_native_nir_expr_is_any(b, e->as.binary.left->as.binary.right)))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_BINARY)
    right_dynamic_result =
        right_dynamic_result ||
        ny_native_nir_expr_is_any(b, e->as.binary.right->as.binary.left) ||
        ny_native_nir_expr_is_any(b, e->as.binary.right->as.binary.right);
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_BINARY &&
      e->as.binary.right->as.binary.op &&
      strcmp(e->as.binary.right->as.binary.op, "%") == 0 &&
      (ny_native_nir_expr_is_any(b, e->as.binary.right->as.binary.left) ||
       ny_native_nir_expr_is_any(b, e->as.binary.right->as.binary.right)))
    right_dynamic_result = true;
  bool left_bigint = ny_native_nir_expr_is_bigint(b, e->as.binary.left);
  bool right_bigint = ny_native_nir_expr_is_bigint(b, e->as.binary.right);
  bool left_user_call = e->as.binary.left &&
                        e->as.binary.left->kind == NY_E_CALL &&
                        e->as.binary.left->as.call.callee &&
                        e->as.binary.left->as.call.callee->kind == NY_E_IDENT &&
                        ny_native_nir_user_defined_fn(
                            b, e->as.binary.left->as.call.callee->as.ident.name);
  bool right_user_call = e->as.binary.right &&
                         e->as.binary.right->kind == NY_E_CALL &&
                         e->as.binary.right->as.call.callee &&
                         e->as.binary.right->as.call.callee->kind == NY_E_IDENT &&
                         ny_native_nir_user_defined_fn(
                             b, e->as.binary.right->as.call.callee->as.ident.name);
  /* Bitwise expressions operate on the native two's-complement i64 domain.
   * A large literal normally lowers to a boxed bigint because it cannot fit
   * in the tagged VM integer representation.  That representation is wrong
   * when the literal is the raw-i64 side of a bitwise expression (including
   * an equality against a rotated/shifted value). */
  bool bitwise_op = ny_native_nir_is_bitwise_operator(e->as.binary.op);
  bool left_bitwise = ny_native_nir_expr_is_bitwise_tree(
      b, e->as.binary.left, 0);
  bool right_bitwise = ny_native_nir_expr_is_bitwise_tree(
      b, e->as.binary.right, 0);
  bool raw_left = bitwise_op || right_bitwise;
  bool raw_right = bitwise_op || left_bitwise;
  int a = (raw_left && e->as.binary.left &&
           e->as.binary.left->kind == NY_E_LITERAL &&
           e->as.binary.left->as.literal.kind == NY_LIT_INT &&
           (e->as.binary.left->as.literal.as.i >= (INT64_C(1) << 62) ||
            e->as.binary.left->as.literal.as.i <= -(INT64_C(1) << 62)))
                ? ny_native_nir_emit_const(b, e->as.binary.left->as.literal.as.i)
                : ny_native_nir_lower_expr(b, e->as.binary.left);
  int rhs = (raw_right && e->as.binary.right &&
             e->as.binary.right->kind == NY_E_LITERAL &&
             e->as.binary.right->as.literal.kind == NY_LIT_INT &&
             (e->as.binary.right->as.literal.as.i >= (INT64_C(1) << 62) ||
              e->as.binary.right->as.literal.as.i <= -(INT64_C(1) << 62)))
                ? ny_native_nir_emit_const(b, e->as.binary.right->as.literal.as.i)
                : ny_native_nir_lower_expr(b, e->as.binary.right);
  if (a < 0 || rhs < 0)
    return -1;
  /* Free get() returns the tagged dynamic ABI for ordinary consumers.  A
   * statically scalar list read entering raw arithmetic must be unboxed once;
   * doing it here preserves both `print(get(...))` and integer accumulation. */
  if (!is_cmp && (left_scalar_get || left_scalar_get_mem)) {
    a = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    left_any = false;
    left_dynamic_result = false;
  }
  if (!is_cmp && (right_scalar_get || right_scalar_get_mem)) {
    rhs = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    right_any = false;
    right_dynamic_result = false;
  }
  if (a < 0 || rhs < 0)
    return -1;
  /* Nil is a dynamic sentinel, never a numeric zero.  A comparison such as
   * `any_float == nil` must cross the boxed-value boundary; letting the
   * numeric f64 path handle it turns nil into 0.0 and makes zero-valued
   * containers disappear.  Box raw floating operands exactly once here. */
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE) &&
      (ny_expr_is_nil_literal(e->as.binary.left) ||
       ny_expr_is_nil_literal(e->as.binary.right)) &&
      (left_any || right_any || left_f64 || right_f64 || left_f32 ||
       right_f32)) {
    if (left_f64) {
      int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", a, -1,
                                                 -1, 1, 0);
      a = bits < 0 ? -1 : ny_native_nir_emit_runtime_call(
                             b, "rt_flt_box_val", bits, -1, -1, 1, 0);
    }
    if (right_f64) {
      int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", rhs, -1,
                                                  -1, 1, 0);
      rhs = bits < 0 ? -1 : ny_native_nir_emit_runtime_call(
                               b, "rt_flt_box_val", bits, -1, -1, 1, 0);
    }
    if (a < 0 || rhs < 0)
      return -1;
    int equal = ny_native_nir_emit_runtime_call(b, "rt_any_eq", a, rhs, -1,
                                                2, 0);
    int true_imm = ny_native_nir_emit_const(b, NY_IMM_TRUE);
    return (equal < 0 || true_imm < 0)
               ? -1
               : nyir_emit(&b->nyir,
                           (nyir_inst_t){.op = NYIR_CMP_I64,
                                         .dst = -1,
                                         .a = equal,
                                         .b = true_imm,
                                         .cmp = cmp});
  }
  /* Untyped runtime/extern bridges (`__str_builder_append(0, "x") == 0`)
   * return raw machine words, not tagged dynamics.  When such a call is
   * compared against an integer or nil literal, both sides must stay raw:
   * the dynamic path would read the raw word as a dynamic value (where 0 is
   * nil) and tag the literal, so plain scalar identities fail.  Comparisons
   * against string/container operands keep the dynamic comparator. */
  if ((e->as.binary.left && e->as.binary.left->kind == NY_E_CALL &&
       e->as.binary.left->as.call.callee &&
       e->as.binary.left->as.call.callee->kind == NY_E_IDENT &&
       e->as.binary.left->as.call.callee->as.ident.name &&
       !ny_native_nir_user_defined_fn(
           b, e->as.binary.left->as.call.callee->as.ident.name) &&
       !ny_native_nir_find_imported_function(
           b, e->as.binary.left->as.call.callee->as.ident.name) &&
       !ny_native_nir_find_local(
           b, e->as.binary.left->as.call.callee->as.ident.name) &&
       !ny_native_nir_find_top_level_value(
           b, e->as.binary.left->as.call.callee->as.ident.name) &&
       e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
       (e->as.binary.right->as.literal.kind == NY_LIT_INT ||
        ny_expr_is_nil_literal(e->as.binary.right))) ||
      (e->as.binary.right && e->as.binary.right->kind == NY_E_CALL &&
       e->as.binary.right->as.call.callee &&
       e->as.binary.right->as.call.callee->kind == NY_E_IDENT &&
       e->as.binary.right->as.call.callee->as.ident.name &&
       !ny_native_nir_user_defined_fn(
           b, e->as.binary.right->as.call.callee->as.ident.name) &&
       !ny_native_nir_find_imported_function(
           b, e->as.binary.right->as.call.callee->as.ident.name) &&
       !ny_native_nir_find_local(
           b, e->as.binary.right->as.call.callee->as.ident.name) &&
       !ny_native_nir_find_top_level_value(
           b, e->as.binary.right->as.call.callee->as.ident.name) &&
       e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
       (e->as.binary.left->as.literal.kind == NY_LIT_INT ||
        ny_expr_is_nil_literal(e->as.binary.left)))) {
    left_any = false;
    right_any = false;
    left_dynamic_result = false;
    right_dynamic_result = false;
  }
  /* Any-valued operators consume the tagged dynamic ABI.  Integer literals
   * are normally emitted as raw NYIR scalars, so normalize a literal when it
   * is paired with an `any` operand before dispatching to the runtime helper
   * (for example `v % 2` inside an indirect iterator callback). */
  bool tag_operand = !is_cmp || cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE;
  if (tag_operand && !left_any && right_any && !left_f64 && !left_f32 &&
      !left_cstr && e->as.binary.left->kind == NY_E_IDENT &&
      !ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.right) &&
      e->as.binary.left->semantic.rep == NY_SEM_REP_RAW_INT) {
    a = ny_native_nir_emit_runtime_call(b, "rt_tag", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (tag_operand && left_any && !right_any && !right_f64 && !right_f32 &&
      !right_cstr &&
      !(e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
        e->as.binary.left->as.memcall.name &&
        strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
        e->as.binary.left->as.memcall.target &&
        ny_native_nir_expr_is_dict(b, e->as.binary.left->as.memcall.target)) &&
      !ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.left) &&
      e->as.binary.right->semantic.rep == NY_SEM_REP_RAW_INT) {
    rhs = ny_native_nir_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1, 0);
    if (rhs < 0)
      return -1;
  }
  if (tag_operand && e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
      e->as.binary.left->as.literal.kind == NY_LIT_INT && right_any &&
      !ny_expr_is_nil_literal(e->as.binary.left)) {
    a = ny_native_nir_emit_runtime_call(b, "rt_tag", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (tag_operand && e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.right->as.literal.kind == NY_LIT_INT &&
      !right_any &&
      !left_any &&
      left_dynamic_result &&
      !(e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
        e->as.binary.left->as.memcall.name &&
        strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
        e->as.binary.left->as.memcall.target &&
        ny_native_nir_expr_is_dict(b, e->as.binary.left->as.memcall.target)) &&
      !ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.left) &&
      !(e->as.binary.left && e->as.binary.left->kind == NY_E_BINARY &&
        e->as.binary.left->as.binary.op &&
        strcmp(e->as.binary.left->as.binary.op, "%") == 0) &&
      !ny_expr_is_nil_literal(e->as.binary.right)) {
    rhs = ny_native_nir_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1, 0);
    if (rhs < 0)
      return -1;
  }
  /* Dynamic modulo consumes the tagged-value runtime ABI, but this expression
   * is still a numeric NYIR result.  Unbox the helper result at this boundary
   * so a following comparison/branch does not mistake the tagged integer 1
   * (3) for the raw integer 3. */
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "%") == 0 &&
      (left_any || right_any || left_dynamic_result || right_dynamic_result) &&
      !left_f64 && !right_f64 && !left_f32 && !right_f32) {
    int mod = ny_native_nir_emit_runtime_call(b, "rt_any_mod", a, rhs, -1,
                                              2, 0);
    return mod < 0 ? -1
                   : ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", mod,
                                                      -1, -1, 1, 0);
  }

  /* Boolean literals normally use raw 0/1 for branch conditions.  When an
   * operator consumes an `any` value, compare against the canonical Ny
   * immediates instead; otherwise a stored `true` (8) is compared with raw
   * one and dynamic dictionary/list values appear unequal. */
  if (tag_operand && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_LITERAL &&
      e->as.binary.left->as.literal.kind == NY_LIT_BOOL &&
      (right_any || right_dynamic_result)) {
    a = ny_native_nir_emit_const(
        b, e->as.binary.left->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE);
    if (a < 0)
      return -1;
  }
  if (tag_operand && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.right->as.literal.kind == NY_LIT_BOOL &&
      (left_any || left_dynamic_result)) {
    rhs = ny_native_nir_emit_const(
        b, e->as.binary.right->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE);
    if (rhs < 0)
      return -1;
  }
  /* Bigint equality/order compares values, never heap handles. */
  if (is_cmp && (left_bigint || right_bigint)) {
    if (!left_bigint && !left_any)
      a = ny_native_nir_emit_runtime_call(
          b, "rt_bigint_from_i64_raw", a, -1, -1, 1, 0);
    if (!right_bigint && !right_any)
      rhs = ny_native_nir_emit_runtime_call(
          b, "rt_bigint_from_i64_raw", rhs, -1, -1, 1, 0);
    if (a < 0 || rhs < 0)
      return -1;
    int compared = ny_native_nir_emit_runtime_call(
        b, "rt_bigint_cmp_raw", a, rhs, -1, 2, 0);
    int zero = compared < 0 ? -1 : ny_native_nir_emit_const(b, 0);
    return compared < 0 || zero < 0
               ? -1
               : nyir_emit(&b->nyir,
                           (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1,
                                         .a = compared, .b = zero,
                                         .cmp = cmp});
  }
  /* An immutable top-level `def alias = nil` retains nil identity even
   * though native scalar storage uses raw zero for both nil and int zero.
   * Recover that semantic fact before the raw comparison; mutable values are
   * deliberately excluded because their contents can change after binding. */
  bool left_nil = ny_native_nir_expr_is_immutable_nil(b, e->as.binary.left, 0);
  bool right_nil = ny_native_nir_expr_is_immutable_nil(b, e->as.binary.right, 0);
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE)) {
    if (left_nil && right_nil)
      return ny_native_nir_emit_const(b, cmp == NYIR_CMP_EQ ? 1 : 0);
    if ((left_nil && e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL && !right_nil) ||
        (right_nil && e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL && !left_nil)) {
      return ny_native_nir_emit_const(b, cmp == NYIR_CMP_NE ? 1 : 0);
    }
    bool left_is_list = ny_native_nir_expr_is_list(b, e->as.binary.left) ||
                        ny_native_nir_expr_is_dyn_list(b, e->as.binary.left);
    bool right_is_list = ny_native_nir_expr_is_list(b, e->as.binary.right) ||
                         ny_native_nir_expr_is_dyn_list(b, e->as.binary.right);
    bool left_seq = left_is_list || ny_native_nir_expr_is_range(b, e->as.binary.left);
    bool right_seq = right_is_list || ny_native_nir_expr_is_range(b, e->as.binary.right);
    if (!left_any && !right_any && !left_dynamic_result && !right_dynamic_result &&
        ((left_seq && right_seq) || (left_seq && right_is_list) || (right_seq && left_is_list))) {
      /* Ranges are heap sequence objects; the tbuf comparator cannot inspect
       * them directly.  The dynamic comparator materializes the range and
       * preserves interpreter sequence equality semantics. */
      if (ny_native_nir_expr_is_range(b, e->as.binary.left) ||
          ny_native_nir_expr_is_range(b, e->as.binary.right)) {
        int equal = ny_native_nir_emit_runtime_call(
            b, "rt_any_eq", a, rhs, -1, 2, 0);
        int true_imm = ny_native_nir_emit_const(b, NY_IMM_TRUE);
        return (equal < 0 || true_imm < 0) ? -1 : nyir_emit(&b->nyir,
            (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1, .a = equal, .b = true_imm,
                          .cmp = cmp});
      }
      int equal = ny_native_nir_emit_runtime_call(
          b, "rt_tbuf_eq_raw", a, rhs, -1, 2, 0);
      int true_imm = ny_native_nir_emit_const(b, NY_IMM_TRUE);
      return (equal < 0 || true_imm < 0) ? -1 : nyir_emit(&b->nyir,
          (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1, .a = equal, .b = true_imm,
                        .cmp = cmp});
    }
    bool left_call_scalar = left_user_call && !left_any;
    bool right_call_scalar = right_user_call && !right_any;
    /* Dictionary indexing always crosses the tagged-value boundary, even
     * when type inference records the scalar payload as RAW_INT.  Compare
     * its canonical value through the dynamic equality helper rather than
     * comparing the tagged word with an unboxed literal. */
    bool left_dict_index = e->as.binary.left &&
                           e->as.binary.left->kind == NY_E_INDEX &&
                           e->as.binary.left->as.index.target &&
                           ny_native_nir_expr_is_dict(
                               b, e->as.binary.left->as.index.target);
    bool right_dict_index = e->as.binary.right &&
                            e->as.binary.right->kind == NY_E_INDEX &&
                            e->as.binary.right->as.index.target &&
                           ny_native_nir_expr_is_dict(
                               b, e->as.binary.right->as.index.target);
    bool left_dict_get = e->as.binary.left &&
                         e->as.binary.left->kind == NY_E_MEMCALL &&
                         e->as.binary.left->as.memcall.name &&
                         strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
                         e->as.binary.left->as.memcall.target &&
                         ny_native_nir_expr_is_dict(
                             b, e->as.binary.left->as.memcall.target);
    bool right_dict_get = e->as.binary.right &&
                          e->as.binary.right->kind == NY_E_MEMCALL &&
                          e->as.binary.right->as.memcall.name &&
                          strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
                          e->as.binary.right->as.memcall.target &&
                          ny_native_nir_expr_is_dict(
                              b, e->as.binary.right->as.memcall.target);
    /* A scalar dictionary lookup is already a tagged value at the runtime
     * boundary.  For a proven integer comparison, decode it once and stay in
     * the raw integer domain; this avoids passing an already-tagged value
     * through the LLVM any-argument adapter a second time. */
    if ((left_dict_get && e->as.binary.right &&
         e->as.binary.right->kind == NY_E_LITERAL &&
         e->as.binary.right->as.literal.kind == NY_LIT_INT) ||
        (right_dict_get && e->as.binary.left &&
         e->as.binary.left->kind == NY_E_LITERAL &&
         e->as.binary.left->as.literal.kind == NY_LIT_INT)) {
      if (left_dict_get) {
        a = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1,
                                            1, 0);
        left_any = false;
        left_dynamic_result = false;
      }
      if (right_dict_get) {
        rhs = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1,
                                              1, 0);
        right_any = false;
        right_dynamic_result = false;
      }
      if (a < 0 || rhs < 0)
        return -1;
    }
    if (left_dict_index || right_dict_index) {
      int equal = ny_native_nir_emit_runtime_call(
          b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = ny_native_nir_emit_const(b, NY_IMM_TRUE);
      return (equal < 0 || true_imm < 0)
                 ? -1
                 : nyir_emit(&b->nyir,
                             (nyir_inst_t){.op = NYIR_CMP_I64,
                                           .dst = -1,
                                           .a = equal,
                                           .b = true_imm,
                                           .cmp = cmp});
    }
    if ((left_dynamic_result || right_dynamic_result) &&
        !left_call_scalar && !right_call_scalar &&
        !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      /* Native memory intrinsics return a proven raw machine value.  Their
       * source-level `any` annotation must not tag only the literal side of
       * an equality such as `load64(p) == 17`. */
      if (ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
          ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.right))
        goto skip_dynamic_compare;
      int equal = ny_native_nir_emit_runtime_call(
            b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = ny_native_nir_emit_const(b, NY_IMM_TRUE);
      return (equal < 0 || true_imm < 0) ? -1 : nyir_emit(&b->nyir,
          (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1, .a = equal, .b = true_imm,
                        .cmp = cmp});
    }
  skip_dynamic_compare:;
  }
  bool numeric_literal_sub =
      e->as.binary.op && strcmp(e->as.binary.op, "-") == 0 &&
      e->as.binary.left && e->as.binary.right &&
      e->as.binary.left->kind == NY_E_LITERAL &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.left->as.literal.kind == NY_LIT_INT &&
      e->as.binary.right->as.literal.kind == NY_LIT_INT;
  bool left_bool = ny_native_nir_expr_is_bool(b, e->as.binary.left);
  bool right_bool = ny_native_nir_expr_is_bool(b, e->as.binary.right);
  bool left_list = ny_native_nir_expr_is_list(b, e->as.binary.left) ||
                   ny_native_nir_expr_is_dyn_list(b, e->as.binary.left);
  bool right_list = ny_native_nir_expr_is_list(b, e->as.binary.right) ||
                    ny_native_nir_expr_is_dyn_list(b, e->as.binary.right);
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "+") == 0) {
    if ((left_any || right_any || left_dynamic_result || right_dynamic_result) &&
        !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      if (ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
          ny_native_nir_expr_is_raw_dynamic_read(b, e->as.binary.right))
        return ny_native_nir_emit_runtime_call(
            b, "rt_raw_add", a, rhs, -1, 2, 0);
      return ny_native_nir_emit_runtime_call(
          b, "rt_any_add", a, rhs, -1, 2, 0);
    }
    if (left_list && right_list && !left_cstr && !right_cstr &&
        !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      return ny_native_nir_emit_runtime_call(
          b, "rt_tbuf_concat_raw", a, rhs, -1, 2, 0);
    }
    if ((left_list || right_list) &&
        !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      return ny_native_nir_emit_runtime_call(
          b, "rt_raw_add", a, rhs, -1, 2, 0);
    }
  }
  bool left_dict = ny_native_nir_expr_is_dict(b, e->as.binary.left);
  bool right_dict = ny_native_nir_expr_is_dict(b, e->as.binary.right);
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "*") == 0) {
    if (left_dict && right_dict) {
      return ny_native_nir_emit_runtime_call(
          b, "rt_vec_dot_raw", a, rhs, -1, 2, NYIR_INST_F_RET_F64);
    }
    if (left_dict && (right_f64 || right_f32)) {
      int s_f64 = right_f32 ? ny_native_nir_emit_f32_to_f64(b, rhs) : rhs;
      return ny_native_nir_emit_runtime_call(
          b, "rt_vec_mul_scalar_raw", a, s_f64, -1, 2, 0);
    }
    if (right_dict && (left_f64 || left_f32)) {
      int s_f64 = left_f32 ? ny_native_nir_emit_f32_to_f64(b, a) : a;
      return ny_native_nir_emit_runtime_call(
          b, "rt_vec_mul_scalar_raw", rhs, s_f64, -1, 2, 0);
    }
    if ((left_any || right_any) && !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      return ny_native_nir_emit_runtime_call(
          b, "rt_any_mul", a, rhs, -1, 2, 0);
    }
  }
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "/") == 0) {
    if (left_dict && (right_f64 || right_f32)) {
      int s_f64 = right_f32 ? ny_native_nir_emit_f32_to_f64(b, rhs) : rhs;
      return ny_native_nir_emit_runtime_call(
          b, "rt_vec_div_scalar_raw", a, s_f64, -1, 2, 0);
    }
    if ((left_any || right_any) && !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      return ny_native_nir_emit_runtime_call(
          b, "rt_any_div", a, rhs, -1, 2, 0);
    }
  }
  if ((left_cstr || right_cstr) && !numeric_literal_sub) {
    if (!left_cstr) {
      int conv = -1;
      if (left_any)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_any_to_cstr", a, -1, -1, 1, 0);
      else if (left_bool)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_bool_to_cstr", a, -1, -1, 1, 0);
      else if (left_f32) {
        int f64 = ny_native_nir_emit_f32_to_f64(b, a);
        conv = f64 < 0
                   ? -1
                   : ny_native_nir_emit_runtime_call(
                         b, "rt_f64_to_cstr_raw", f64, -1, -1, 1, 0);
      } else if (left_f64)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_f64_to_cstr_raw", a, -1, -1, 1, 0);
      else if (e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
               e->as.binary.left->as.literal.kind == NY_LIT_INT)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_i64_to_cstr_raw", a, -1, -1, 1, 0);
      else
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_any_to_cstr", a, -1, -1, 1, 0);
      a = conv;
    }
    if (!right_cstr) {
      int conv = -1;
      if (right_any)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_any_to_cstr", rhs, -1, -1, 1, 0);
      else if (right_bool)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_bool_to_cstr", rhs, -1, -1, 1, 0);
      else if (right_f32) {
        int f64 = ny_native_nir_emit_f32_to_f64(b, rhs);
        conv = f64 < 0
                   ? -1
                   : ny_native_nir_emit_runtime_call(
                         b, "rt_f64_to_cstr_raw", f64, -1, -1, 1, 0);
      } else if (right_f64)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_f64_to_cstr_raw", rhs, -1, -1, 1, 0);
      else if (e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
               e->as.binary.right->as.literal.kind == NY_LIT_INT)
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_i64_to_cstr_raw", rhs, -1, -1, 1, 0);
      else
        conv = ny_native_nir_emit_runtime_call(
            b, "rt_any_to_cstr", rhs, -1, -1, 1, 0);
      rhs = conv;
    }
    if (a < 0 || rhs < 0)
      return -1;
    if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "+") == 0) {
      int out = ny_native_nir_emit_runtime_call(
          b, "rt_cstr_concat", a, rhs, -1, 2, 0);
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
      int equal = ny_native_nir_emit_runtime_call(
          b, "rt_cstr_eq", a, rhs, -1, 2, 0);
      if (equal < 0 || cmp == NYIR_CMP_EQ)
        return equal;
      int zero = ny_native_nir_emit_const(b, 0);
      return zero < 0 ? -1 : nyir_emit(&b->nyir,
          (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1, .a = equal, .b = zero,
                        .cmp = NYIR_CMP_EQ});
    }
    if (is_cmp) {
      int compared = ny_native_nir_emit_runtime_call(
          b, "rt_cstr_cmp", a, rhs, -1, 2, 0);
      int zero = compared < 0 ? -1 : ny_native_nir_emit_const(b, 0);
      return compared < 0 || zero < 0
                 ? -1
                 : nyir_emit(&b->nyir,
                             (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1,
                                           .a = compared, .b = zero,
                                           .cmp = cmp});
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
  /* Bitwise NYIR instructions operate on raw i64 values.  A bigint operand is
   * a heap handle, so route shifts through the representation-aware bridge
   * instead of shifting the pointer bits. */
  if (!is_cmp && op == NYIR_SHL_I64)
    return ny_native_nir_emit_runtime_call(
        b, "rt_shl_raw", a, rhs, -1, 2, 0);
  /*
   * Dynamic parameters enter the native ABI as tagged values, while NYIR's
   * integer arithmetic instructions consume raw i64 payloads.  `+` normally
   * returned above through rt_any_add, but every other integer
   * operation (and the non-equality integer comparisons) must explicitly
   * unbox its dynamic operands.  Without this, `x * x` computes on the tag
   * encoding (7*7 for source value 3) and silently corrupts nested formulas.
   */
  bool integer_domain =
      op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_MUL_I64 ||
      op == NYIR_DIV_I64 || op == NYIR_MOD_I64 || op == NYIR_AND_I64 ||
      op == NYIR_OR_I64 || op == NYIR_XOR_I64 || op == NYIR_SHL_I64 ||
      op == NYIR_SAR_I64 || op == NYIR_ROR_I64 ||
      (is_cmp && !ny_native_nir_expr_is_f64(b, e) &&
                             !ny_native_nir_expr_is_f32(b, e));
  /* Only an any local is known to still carry the tagged incoming ABI here.
   * Binary/unary/call results already use the raw NYIR result ABI; using the
   * broad recursive `expr_is_any` predicate for those would untag an odd raw
   * integer by accident (e.g. the value 9). */
  bool left_tagged_input =
      left_any && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_IDENT &&
      ny_native_nir_find_local(b, e->as.binary.left->as.ident.name) != NULL;
  bool right_tagged_input =
      right_any && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_IDENT &&
      ny_native_nir_find_local(b, e->as.binary.right->as.ident.name) != NULL;
  if (integer_domain && left_tagged_input && !left_bigint) {
    a = ny_native_nir_emit_runtime_call(
        b, "rt_any_to_i64", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (integer_domain && right_tagged_input && !right_bigint) {
    rhs = ny_native_nir_emit_runtime_call(
        b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    if (rhs < 0)
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
      if (left_f32) {
        a = ny_native_nir_emit_f32_to_f64(b, a);
      } else if (left_any) {
        a = ny_native_nir_emit_runtime_call(
            b, "rt_any_to_f64", a, -1, -1, 1, NYIR_INST_F_RET_F64);
      } else {
        a = ny_native_nir_emit_i64_to_f64(b, a);
      }
      if (a < 0)
        return -1;
    }
    if (!right_f64) {
      if (right_f32) {
        rhs = ny_native_nir_emit_f32_to_f64(b, rhs);
      } else if (right_any) {
        rhs = ny_native_nir_emit_runtime_call(
            b, "rt_any_to_f64", rhs, -1, -1, 1, NYIR_INST_F_RET_F64);
      } else {
        rhs = ny_native_nir_emit_i64_to_f64(b, rhs);
      }
      if (rhs < 0)
        return -1;
    }
  }
  if (op == NYIR_DIV_F64 || op == NYIR_DIV_F32 || op == NYIR_DIV_I64 ||
      op == NYIR_MOD_I64) {
    /*
     * Skip the guard when the divisor is a statically-known non-zero
     * constant (mirrors the LLVM path's divisor_nonzero check); a divide
     * inside a hot loop then costs nothing extra.  Constant-zero divisors
     * are rejected downstream by the NYIR verify pass.
     */
    const expr_t *divisor_expr = e->as.binary.right;
    bool const_nonzero = divisor_expr && divisor_expr->kind == NY_E_LITERAL &&
                         ((divisor_expr->as.literal.kind == NY_LIT_FLOAT &&
                           divisor_expr->as.literal.as.f != 0.0) ||
                          (divisor_expr->as.literal.kind == NY_LIT_INT &&
                           divisor_expr->as.literal.as.i != 0));
    if (!const_nonzero) {
      const char *msg = op == NYIR_MOD_I64 ? "modulo by zero"
                                           : "division by zero";
      nyir_op_t cmp_op = op == NYIR_DIV_F64   ? NYIR_CMP_F64
                         : op == NYIR_DIV_F32 ? NYIR_CMP_F32
                                              : NYIR_CMP_I64;
      if (!ny_native_nir_emit_div_zero_guard(b, cmp_op, rhs, msg))
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
