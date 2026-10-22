/*
 * Dictionary element reads (subscript, .get, .pop) keep the canonical
 * tagged slot word at the lowering boundary.  Arithmetic against such a
 * read must decode it once when the other operand is a proven raw integer;
 * this predicate identifies those reads.
 */
static bool nyr_expr_is_raw_integer_conversion(nyr_builder_t *b,
                                               const expr_t *e);

static bool nyr_value_is_tag_call(const nyr_builder_t *b, int value) {
  if (!b || value < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op != NYIR_CALL || !in->symbol)
      return false;
    /* The ABI record decides first: a row reporting a tagged-int or
     * tagged-value result marks a tagged producer regardless of its name,
     * and any other recorded result class marks a non-tagged one.  Only an
     * unrecorded symbol falls back to the exact-name test. */
    const ny_native_runtime_abi_decl_t *decl =
        ny_native_runtime_abi_decl(in->symbol);
    if (decl)
      return ny_native_runtime_abi_result_is_tagged_int(decl) ||
             ny_native_runtime_abi_result_is_tagged_value(decl);
    return strcmp(in->symbol, "rt_tag") == 0;
  }
  return false;
}

static bool nyr_value_is_untagged_i64_call(const nyr_builder_t *b, int value) {
  if (!b || value < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op != NYIR_CALL || !in->symbol)
      return false;
    /* The ABI record decides first: a row reporting a raw-int result marks
     * an already-decoded producer regardless of its name, and any other
     * recorded result class marks a still-tagged one.  Only an unrecorded
     * symbol falls back to the exact-name test. */
    const ny_native_runtime_abi_decl_t *decl =
        ny_native_runtime_abi_decl(in->symbol);
    if (decl)
      return ny_native_runtime_abi_result_is_raw_int(decl);
    return strcmp(in->symbol, "rt_any_to_i64") == 0;
  }
  return false;
}

static bool nyr_expr_is_dict_read(nyr_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  const expr_t *target = NULL;
  const char *method = NULL;
  if (e->kind == NY_E_INDEX) {
    target = e->as.index.target;
  } else if (e->kind == NY_E_MEMCALL) {
    target = e->as.memcall.target;
    method = e->as.memcall.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.args.len >= 1) {
    if (e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name &&
        strcmp(e->as.call.callee->as.ident.name, "get") == 0) {
      target = e->as.call.args.data[0].val;
      method = "get";
    } else if (e->as.call.callee->kind == NY_E_MEMBER &&
               e->as.call.callee->as.member.name &&
               (strcmp(e->as.call.callee->as.member.name, "get") == 0 ||
                strcmp(e->as.call.callee->as.member.name, "pop") == 0)) {
      target = e->as.call.callee->as.member.target;
      method = e->as.call.callee->as.member.name;
    }
  }
  if (!target)
    return false;
  if (method && strcmp(method, "get") != 0 && strcmp(method, "pop") != 0)
    return false;
  if (nyr_expr_is_dict(b, target))
    return true;
  if (target->kind == NY_E_IDENT && target->as.ident.name) {
    const nyr_local_t *local = nyr_find_local(b, target->as.ident.name);
    if (local && local->is_dict)
      return true;
  }
  return false;
}

/* Return the receiver for attached `receiver.get(...)` syntax regardless of
 * whether the parser retained it as a memcall or a normal call with a member
 * callee.  Both forms lower to the same runtime accessor, so representation
 * decisions must not depend on that surface-AST distinction. */
static const expr_t *nyr_attached_get_receiver(const expr_t *e) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0)
    return e->as.memcall.target;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name &&
      strcmp(e->as.call.callee->as.member.name, "get") == 0)
    return e->as.call.callee->as.member.target;
  return NULL;
}

/*
 * A subscript read into a proven parameterized scalar list uses the raw
 * elem-8 fast path and returns the undecoded payload word.  Running that
 * word through rt_any_to_i64 decodes raw odd integers as tagged (raw 1
 * becomes 0), so the equality normalization below must leave it alone.
 */
static bool nyr_index_read_is_proven_scalar(nyr_builder_t *b, const expr_t *e) {
  if (!e || e->kind != NY_E_INDEX || !e->as.index.target)
    return false;
  if (nyr_expr_is_bytes(b, e->as.index.target))
    return true;
  if (e->as.index.target->kind != NY_E_IDENT ||
      !e->as.index.target->as.ident.name)
    return false;
  const char *name = e->as.index.target->as.ident.name;
  if (nyr_index_of_typed_global_int_list(b, e))
    return true;
  /*
   * The whole-program append-shape proof also promotes an unannotated
   * top-level `mut xs = []` to raw elem-8 integer storage.  Its index lowering
   * emits a raw load, so comparisons must keep both operands in the raw
   * integer domain rather than treating that load as a tagged `any`.
   */
  if (nyr_index_of_promoted_top_mut(b, e))
    return true;
  if (!nyr_has_mutable_top_var_in_prog(b, b->prog, name)) {
    const expr_t *tuple = nyr_find_top_level_value(b, name);
    bool raw_int_tuple =
        tuple && tuple->kind == NY_E_TUPLE && tuple->as.list_like.len > 0;
    for (size_t i = 0; raw_int_tuple && i < tuple->as.list_like.len; ++i) {
      const expr_t *element = tuple->as.list_like.data[i];
      raw_int_tuple = element && element->kind == NY_E_LITERAL &&
                      element->as.literal.kind == NY_LIT_INT &&
                      element->tok.kind != NY_T_NIL;
    }
    if (raw_int_tuple)
      return true;
  }
  const nyr_local_t *seq = nyr_find_local_nearest(b, name);
  if (seq) {
    if (!seq->is_list || seq->is_any || seq->is_dyn_list || !seq->type_name ||
        strcmp(seq->type_name, "list") == 0 ||
        ny_native_type_name_is_dyn_list(seq->type_name))
      return false;
  } else {
    /* A top-level global initialized from a list literal has static raw
     * array storage (same ABI as the local literal case); `def K = [...]`
     * reads are raw machine words, and decoding them again halves every
     * odd element.  Heap-backed globals (zeros(n), append chains) keep their
     * dynamic accessor and stay decoded. */
    if (b->prog && nyr_has_mutable_top_var_in_prog(b, b->prog, name))
      return false;
    const expr_t *ginit = nyr_find_top_level_value(b, name);
    if (!ginit || (ginit->kind != NY_E_LIST && ginit->kind != NY_E_TUPLE))
      return false;
    if (ginit->kind == NY_E_TUPLE) {
      if (ginit->as.list_like.len == 0)
        return false;
      for (size_t i = 0; i < ginit->as.list_like.len; ++i) {
        const expr_t *element = ginit->as.list_like.data[i];
        if (!element || element->kind != NY_E_LITERAL ||
            element->as.literal.kind != NY_LIT_INT ||
            element->tok.kind == NY_T_NIL)
          return false;
      }
    }
  }
  const char *elem = nyr_expr_type_name(b, e);
  return elem && strcmp(elem, "int") == 0;
}

/*
 * An identifier whose local slot ABI is proven RAW_INT holds an undecoded
 * payload word.  When the other operand is also a raw payload (a proven
 * scalar list read), only a plain icmp is sound: rt_any_eq reads odd raw
 * words as tagged and therefore equates raw 2 with raw 5 (the tagged
 * spelling of 2).  Slot facts are maintained by the store boundaries here,
 * not by conservative semantic inference, so they do not carry the stale
 * HM rep hazard that plain call results have.
 */
static bool nyr_ident_local_is_raw_int(nyr_builder_t *b, const expr_t *e) {
  if (!e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  const nyr_local_t *local = nyr_find_local_nearest(b, e->as.ident.name);
  return local && !local->is_any && local->semantic_rep == NY_SEM_REP_RAW_INT;
}

static bool nyr_expr_is_dynamic_sequence_value(nyr_builder_t *b,
                                               const expr_t *e) {
  if (!e)
    return false;
  if (nyr_expr_is_dyn_list(b, e))
    return true;
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  const nyr_local_t *local = nyr_find_local_nearest(b, e->as.ident.name);
  return local && (local->is_any || nyr_local_is_dynamic_sequence(local));
}

/* Semantic inference may narrow an expression produced by dynamic arithmetic
 * to `int`, while the emitted producer still returns a tagged dynamic word.
 * Preserve that producer ABI when it is nested inside another arithmetic
 * expression. */
static bool nyr_arith_expr_is_dynamic(nyr_builder_t *b, const expr_t *e,
                                      unsigned depth) {
  if (!b || !e || depth > 8)
    return false;
  /* The whole-program proof overrides the conservative dynamic annotation
   * on an untyped top-level list's index node: its accessor is raw elem-8.
   * Apply this before is_any(), which intentionally reflects the AST's
   * dynamic fallback rather than the proven physical ABI. */
  if (nyr_index_of_promoted_top_mut(b, e))
    return false;
  /* A lowered arithmetic expression follows its resolved representation.  In
   * particular, dictionary reads are decoded before a scalar operator, so a
   * statically resolved RAW_INT result must remain raw when it is nested in a
   * comparison or another operator.  Looking only at the dynamic children
   * would incorrectly reclassify that producer as tagged. */
  if (e->kind == NY_E_BINARY && e->semantic.resolved &&
      e->semantic.rep == NY_SEM_REP_RAW_INT &&
      (!e->as.binary.op || strcmp(e->as.binary.op, "**") != 0))
    return false;
  if (nyr_expr_is_any(b, e))
    return true;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    if (local && (local->is_any || local->dyn_tag_slot >= 0))
      return true;
  }
  if (e->kind == NY_E_BINARY) {
    bool left = nyr_arith_expr_is_dynamic(b, e->as.binary.left, depth + 1);
    bool right = nyr_arith_expr_is_dynamic(b, e->as.binary.right, depth + 1);
    /* A mixed raw/dynamic operation is normalized to the raw scalar domain
     * when its surrounding contract is scalar.  Treating the result as
     * dynamic merely because one child came from an any sequence causes the
     * next operation in a left-associated chain to decode/tag the already
     * normalized value a second time.  Preserve dynamic arithmetic only when
     * both operands are dynamic, or when the enclosing ABI explicitly
     * requires an any result. */
    if (b->return_any || b->value_for_any_call)
      return left || right;
    return left && right;
  }
  return false;
}

/* A proven scalar list index is emitted as a raw native payload.  It must
 * remain in that representation for scalar comparisons; sending it through
 * the dynamic equality ABI would make an odd payload indistinguishable from
 * a tagged integer with a different value. */
static bool nyr_expr_is_typed_scalar_index(nyr_builder_t *b, const expr_t *e) {
  if (!b || !e || e->kind != NY_E_INDEX || !e->as.index.target)
    return false;
  const char *container = nyr_expr_type_name(b, e->as.index.target);
  /* During native lowering a mutable local can still carry the broad
   * expression-level `list` fact even though its declaration is
   * `list<int>`.  The declaration is the stronger ABI fact: an indexed
   * scalar must stay raw i64 for arithmetic.  Without this recovery,
   * `sum += xs[i]` routes the raw element through rt_any_to_i64 and halves
   * odd payloads (or turns them into zero). */
  if (e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
    if (local && local->type_name &&
        !ny_native_type_name_is_dyn_list(local->type_name))
      container = local->type_name;
  }
  const char *element = nyr_expr_type_name(b, e);
  if ((!element || strcmp(element, "unknown") == 0 ||
       strcmp(element, "any") == 0) &&
      container && strncmp(container, "list<", 5) == 0) {
    const char *start = container + 5;
    const char *end = strrchr(start, '>');
    if (end && end > start) {
      size_t n = (size_t)(end - start);
      element = nyr_builder_own_substring(b, start, n);
    }
  }
  if (!container || !element || strncmp(container, "list<", 5) != 0 ||
      ny_native_type_name_is_dyn_list(container))
    return false;
  return strcmp(element, "int") == 0 || strcmp(element, "i64") == 0 ||
         strcmp(element, "i32") == 0 || strcmp(element, "i16") == 0 ||
         strcmp(element, "i8") == 0 || strcmp(element, "u64") == 0 ||
         strcmp(element, "u32") == 0 || strcmp(element, "u16") == 0 ||
         strcmp(element, "u8") == 0;
}

/*
 * Emit an inline divide-by-zero guard matching the LLVM/JIT path
 * (ny_emit_f64_div_zero_guard / the int divmod slow path): when the divisor
 * compares equal to zero, panic with the given message ("division by zero"
 * or "modulo by zero").  The panic block is cold; the hot path is one
 * compare + branch.  rt_panic is noreturn (longjmp/exit), so the trailing
 * ret 0 is only reachable if the runtime is built without panic support.
 */
static bool nyr_emit_div_zero_guard(nyr_builder_t *b, nyr_op_t cmp_op,
                                    int divisor, const char *msg) {
  /*
   * Float divisors are tested by bit pattern, not IEEE equality: the
   * language panics on +0.0 but dividing by -0.0 follows IEEE and yields
   * -inf (the scalar-classes ABI contract, matching the C oracle).  f64/f32
   * SSA values are raw-bits i64 registers on every backend, so an i64
   * compare against 0 selects exactly +0.0.
   */
  int zero = cmp_op == NYIR_CMP_F32 || cmp_op == NYIR_CMP_F64
                 ? nyr_emit_const(b, 0)
                 : nyr_emit_const(b, 0);
  if (zero < 0)
    return false;
  int is_zero =
      nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = cmp_op == NYIR_CMP_F32 ||
                                                      cmp_op == NYIR_CMP_F64
                                                  ? NYIR_CMP_I64
                                                  : cmp_op,
                                        .dst = -1,
                                        .a = divisor,
                                        .b = zero,
                                        .cmp = NYIR_CMP_EQ});
  if (is_zero < 0)
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  int panic_label = b->next_label++;
  int ok_label = b->next_label++;
  if (!nyr_emit_br_if(b, is_zero, panic_label) || !nyr_emit_br(b, ok_label) ||
      !nyr_emit_label(b, panic_label))
    return false;
  int msg_v = nyr_emit_cstr_const(b, msg ? msg : "division by zero");
  if (msg_v < 0)
    return false;
  int tagged = nyr_emit_runtime_call(b, "rt_alloc_string", msg_v, -1, -1, 1, 0);
  if (tagged < 0 ||
      nyr_emit_runtime_call(b, "rt_panic", tagged, -1, -1, 1, 0) < 0)
    return false;
  /*
   * The cold panic block's return value must match the function's return
   * ABI: an i64 zero returned from a `-> number`/`-> f64` function failed
   * the scalar-class verify (the fdiv result path returns machine f64).
   */
  const char *rt_leaf = b->return_type ? ny_name_leaf(b->return_type) : NULL;
  bool f64_return =
      b->return_type && (ny_native_type_name_is_f64(b->return_type) ||
                         (rt_leaf && (strcmp(rt_leaf, "number") == 0 ||
                                      strcmp(rt_leaf, "numeric") == 0)));
  int zero_i = f64_return ? nyr_emit_const_f64(b, 0.0) : nyr_emit_const(b, 0);
  if (zero_i < 0)
    return false;
  /*
   * The panic block's ret terminates only that cold block; it must not mark
   * the enclosing statement/function as having returned, or the statement
   * iterators would truncate the body after the first guarded division.
   */
  bool saved_return = b->emitted_return;
  int saved_last_value = b->last_value;
  bool ok = nyr_emit_ret(b, zero_i);
  b->emitted_return = saved_return;
  b->last_value = saved_last_value;
  if (!ok)
    return false;
  return nyr_emit_label(b, ok_label);
}

static bool nyr_is_bitwise_operator(const char *op) {
  return op && (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 ||
                strcmp(op, ">>") == 0);
}

static bool nyr_expr_is_bitwise_tree(const nyr_builder_t *b, const expr_t *e,
                                     unsigned depth) {
  if (!e || depth > 16)
    return false;
  if (e->kind == NY_E_BINARY)
    return nyr_is_bitwise_operator(e->as.binary.op);
  if (e->kind != NY_E_IDENT)
    return false;
  const expr_t *value = nyr_find_top_level_value(b, e->as.ident.name);
  return value && value != e && nyr_expr_is_bitwise_tree(b, value, depth + 1);
}

static bool nyr_expr_is_immutable_nil(const nyr_builder_t *b, const expr_t *e,
                                      unsigned depth) {
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
  if (nyr_globaltab_has(e->as.ident.name))
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
      global_symbol = nyr_globaltab_name(qualified);
  }
  if (!global_symbol && b->current_fn_name) {
    const char *dot = strrchr(b->current_fn_name, '.');
    if (dot && dot != b->current_fn_name && dot[1]) {
      char qualified[512];
      int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                       (int)(dot - b->current_fn_name), b->current_fn_name,
                       e->as.ident.name);
      if (n > 0 && (size_t)n < sizeof(qualified))
        global_symbol = nyr_globaltab_name(qualified);
    }
  }
  if (global_symbol || nyr_globaltab_name_tail(e->as.ident.name))
    return false;
  const expr_t *value = nyr_find_top_level_value(b, e->as.ident.name);
  return value && value != e && nyr_expr_is_immutable_nil(b, value, depth + 1);
}

/* A top-level untyped binding (`def hout = zeros(8)`) owns the dynamic ABI:
 * its index reads return tagged words even though a same-shaped local of
 * list type would read raw slots.  Arithmetic over such reads must decode
 * them exactly once like any other dynamic-index operand; locals are
 * classified by nyr_local_is_dynamic_sequence elsewhere. */
static bool nyr_ident_is_global_dynamic_sequence(nyr_builder_t *b,
                                                 const char *name) {
  if (!b || !name || nyr_find_local_nearest(b, name))
    return false;
  const stmt_t *decl = nyr_find_top_level_var_stmt(b, name);
  if (!decl || decl->kind != NY_S_VAR)
    return false;
  const char *declared =
      (decl->as.var.types.len > 0 && decl->as.var.types.data[0])
          ? decl->as.var.types.data[0]
          : NULL;
  if (declared && declared[0] && strcmp(declared, "any") != 0)
    return false;
  const expr_t *init = nyr_find_top_level_value(b, name);
  return init && nyr_expr_is_list(b, init);
}

static bool nyr_is_builtin_int_cast(const nyr_builder_t *b,
                                    const expr_t *expr) {
  if (!b || !expr || expr->kind != NY_E_CALL || expr->as.call.args.len != 1 ||
      expr->as.call.args.data[0].name)
    return false;
  const char *leaf = ny_native_call_leaf(expr);
  return leaf && strcmp(leaf, "int") == 0 &&
         !nyr_user_defined_fn((nyr_builder_t *)b, leaf);
}

static const stmt_t *
ny_native_operator_target_function(const nyr_builder_impl_t *b,
                                   const stmt_t *op_stmt) {
  if (!b || !b->prog || !op_stmt || op_stmt->kind != NY_S_OPERATOR ||
      !op_stmt->as.oper.target || !*op_stmt->as.oper.target)
    return NULL;
  ny_native_fn_index_t *index = ny_native_fn_index_for(b);
  const char *target = op_stmt->as.oper.target;
  for (size_t i = 0; index && i < index->functions.count; ++i) {
    const stmt_t *fn = index->functions.funcs[i];
    if (!fn || fn->kind != NY_S_FUNC || !fn->as.fn.name)
      continue;
    if (strcmp(fn->as.fn.name, target) == 0)
      return fn;
    const char *module = ny_native_fn_module(index, b->prog, fn);
    if (!module || !*module)
      continue;
    size_t module_len = strlen(module);
    if (strncmp(target, module, module_len) == 0 && target[module_len] == '.' &&
        strcmp(target + module_len + 1, fn->as.fn.name) == 0)
      return fn;
  }
  return NULL;
}

static int nyr_lower_binary(nyr_builder_t *b, const expr_t *e) {
  /* The representation fact is rewritten by the integer-arith exits below;
   * every other path (comparisons, strings, containers, f64) leaves it at 0
   * so a dynamic boundary never boxes a non-integer payload by accident. */
  b->last_arith_raw = 0;
  b->last_arith_tagged = false;
  b->last_arith_expr = e;

  /*
   * Shifts are integer-domain: an enclosing float context (the return
   * normalization of `-> number`/`-> f64` functions bumps f64_context_depth
   * around `1.0 / float((ij*(ij+1)) >> 1)`) must not pull the shift's
   * integer operands into the f64 domain — mul.f64 of integer values made
   * the raw shift bridge consume IEEE bits as a magnitude.
   */
  if (b->f64_context_depth != 0 && e->as.binary.op &&
      (strcmp(e->as.binary.op, "<<") == 0 ||
       strcmp(e->as.binary.op, ">>") == 0)) {
    unsigned saved_depth = b->f64_context_depth;
    b->f64_context_depth = 0;
    int shifted = nyr_lower_binary(b, e);
    b->f64_context_depth = saved_depth;
    return shifted;
  }
  bool int_cast_operand = nyr_is_builtin_int_cast(b, e->as.binary.left) ||
                          nyr_is_builtin_int_cast(b, e->as.binary.right);
  bool other_operand_f64 =
      (nyr_is_builtin_int_cast(b, e->as.binary.left) &&
       !(e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
         e->as.binary.right->as.literal.kind == NY_LIT_INT) &&
       nyr_expr_is_f64(b, e->as.binary.right)) ||
      (nyr_is_builtin_int_cast(b, e->as.binary.right) &&
       !(e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
         e->as.binary.left->as.literal.kind == NY_LIT_INT) &&
       nyr_expr_is_f64(b, e->as.binary.left));
  bool force_integer_domain = int_cast_operand && !other_operand_f64;
  bool inherited_f64 = b->f64_context_depth != 0 && !force_integer_domain;
  /* User-defined attached operators are represented as NY_S_OPERATOR
   * declarations rather than ordinary binary call nodes.  Dispatch them
   * before the scalar/list arithmetic ladder; otherwise a nominal object such
   * as `SelfBox` is treated as two raw pointer-sized integers. */
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve",
                           "function=%s op=%s decision=probe-attached-operator",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           e->as.binary.op ? e->as.binary.op : "<null>");
  const stmt_t *attached = nyr_find_operator(b, e);
  if (attached && attached->as.oper.left_type) {
    const char *actual_left = nyr_expr_type_name(b, e->as.binary.left);
    if (actual_left &&
        !ny_native_type_alias_eq(attached->as.oper.left_type, actual_left))
      attached = NULL;
  }
  if (attached && attached->as.oper.right_type) {
    const char *actual_right = nyr_expr_type_name(b, e->as.binary.right);
    if (actual_right &&
        !ny_native_type_alias_eq(attached->as.oper.right_type, actual_right))
      attached = NULL;
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve", "function=%s op=%s result=%s",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           e->as.binary.op ? e->as.binary.op : "<null>",
                           attached ? "attached" : "builtin");
  if (attached && attached->as.oper.target && attached->as.oper.target[0]) {
    const char *owner = (attached->as.oper.left_type &&
                         strcmp(attached->as.oper.left_type, "self") != 0)
                            ? attached->as.oper.left_type
                            : nyr_expr_type_name(b, e->as.binary.left);
    char symbol[512];
    const char *target = attached->as.oper.target;
    int n = (strchr(target, '.') || !owner)
                ? snprintf(symbol, sizeof(symbol), "%s", target)
                : snprintf(symbol, sizeof(symbol), "%s.%s", owner, target);
    if (n > 0 && (size_t)n < sizeof(symbol) && owner) {
      /* Operator targets are declarations in an impl, not names in the
       * core namespace.  Resolve the exact method first so imported
       * providers such as core.math.big keep their module-qualified symbol. */
      const char *target_leaf = ny_name_leaf(target);
      const stmt_t *target_fn =
          target_leaf
              ? nyr_find_attached_method(b, e->as.binary.left, target_leaf)
              : NULL;
      /*
       * The operator walk stores the canonical (module-qualified) target
       * spelling, which the function pool and the VM both index. The
       * attached-method probe can return the AST's shorter owner-qualified
       * name (`bigint.ne`), which nothing in the pool carries; accepting
       * it stranded runtime dispatch with an unresolved target. A probe
       * result may only UPGRADE the qualification (strictly more dotted),
       * never degrade the canonical spelling.
       */
      if (target_fn && target_fn->as.fn.name &&
          strlen(target_fn->as.fn.name) > strlen(symbol))
        snprintf(symbol, sizeof(symbol), "%s", target_fn->as.fn.name);
      /* Short-name lookup and expanded operator metadata can both select an
       * impl wrapper (for example `core.str.str.repeat`) instead of the
       * source helper `core.str.repeat`.  Prefer the canonical owner plus
       * leaf spelling when the function table contains it; this keeps
       * operator lowering on the same symbol as ordinary attached calls. */
      char qualified[512];
      int qn = target_leaf ? snprintf(qualified, sizeof(qualified),
                                      "core.%s.%s", owner, target_leaf)
                           : -1;
      if (!target_fn && qn > 0 && (size_t)qn < sizeof(qualified) &&
          nyr_find_user_function(b, qualified))
        snprintf(symbol, sizeof(symbol), "%s", qualified);
    }
    if (n <= 0 || (size_t)n >= sizeof(symbol)) {
      return nyr_fail(b, "native NYIR: attached operator target is too long"),
             -1;
    }
    /*
     * The collector resolves emitted call symbols by name against the
     * function table. When this spelling is not resolvable there (an impl
     * method indexed under a different qualified name), emitting the call
     * strands the runtime with an unresolved target (`bigint.ne`). Fall
     * through to the builtin paths instead — the compare bridge below
     * handles equality/order on bigint operands by value.
     */
    const stmt_t *target_decl =
        nyr_find_user_function((nyr_builder_t *)b, symbol);
    if (!target_decl)
      target_decl = ny_native_operator_target_function(b, attached);
    if (target_decl) {
      call_arg_t args[2];
      args[0] = (call_arg_t){.val = (expr_t *)e->as.binary.left};
      args[1] = (call_arg_t){.val = (expr_t *)e->as.binary.right};
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = symbol;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic = e->semantic;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.semantic.canonical_callee = symbol;
      call.semantic.canonical_callee_stmt = (stmt_t *)target_decl;
      call.as.call.callee = &callee;
      call.as.call.args.data = args;
      call.as.call.args.len = call.as.call.args.cap = 2;
      return nyr_lower_expr(b, &call);
    }
  }
  /* `lo..hi` is the language's inclusive range expression.  Loop lowering
   * handles this shape directly, but a range used as a value (for example
   * `(1..5).len` or an argument) must materialize the same representation so
   * all consumers observe the same endpoint semantics. */
  if (e->as.binary.op && strcmp(e->as.binary.op, "..") == 0) {
    int lo = nyr_lower_expr(b, e->as.binary.left);
    int hi = lo < 0 ? -1 : nyr_lower_expr(b, e->as.binary.right);
    int one = hi < 0 ? -1 : nyr_emit_const(b, 1);
    int inclusive_stop =
        hi < 0 || one < 0 ? -1 : nyr_emit_binop(b, NYIR_ADD_I64, hi, one);
    return lo < 0 || inclusive_stop < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_range_new_raw", lo,
                                       inclusive_stop, one, 3, 0);
  }
  if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0) {
    bool left_list = nyr_expr_is_list(b, e->as.binary.left);
    bool right_list = nyr_expr_is_list(b, e->as.binary.right);
    if (left_list != right_list) {
      const expr_t *list_expr =
          left_list ? e->as.binary.left : e->as.binary.right;
      const expr_t *count_expr =
          left_list ? e->as.binary.right : e->as.binary.left;
      bool concrete_list = list_expr->kind == NY_E_LIST;
      if (list_expr->kind == NY_E_IDENT) {
        nyr_local_t *local = nyr_find_local(b, list_expr->as.ident.name);
        concrete_list =
            local &&
            (local->is_list || local->semantic_rep == NY_SEM_REP_TYPED_BUFFER);
        if (!concrete_list) {
          const expr_t *global =
              nyr_find_top_level_value(b, list_expr->as.ident.name);
          concrete_list =
              global && global != list_expr && global->kind == NY_E_LIST;
        }
      } else if (list_expr->kind == NY_E_CALL ||
                 list_expr->kind == NY_E_MEMCALL) {
        concrete_list = nyr_expr_is_list(b, list_expr);
      }
      if (!concrete_list)
        goto scalar_multiply;
      /* Semantic list evidence can be stale for untyped multi-assignment
       * temporaries in arithmetic-heavy code.  A value proven to be a float
       * is scalar multiplication, never list repetition. */
      if (nyr_expr_is_f64(b, list_expr) || nyr_expr_is_f32(b, list_expr) ||
          nyr_expr_is_cstr(b, list_expr))
        goto scalar_multiply;
      if (nyr_expr_is_f64(b, count_expr) || nyr_expr_is_f32(b, count_expr) ||
          nyr_expr_is_cstr(b, count_expr)) {
        return nyr_fail(
                   b,
                   "native NYIR lower: list repeat count must be an integer"),
               -1;
      }
      /* A dynamically typed count (`mut w, h = size[0], size[1]` feeding
       * `[0.0] * w * h` in core.term) is legal Nytrix: unbox it at
       * runtime instead of rejecting a program the VM accepts. */
      bool count_is_dynamic = nyr_expr_is_any(b, count_expr);
      int list = nyr_lower_expr(b, list_expr);
      int count = nyr_lower_expr(b, count_expr);
      if (list < 0 || count < 0)
        return -1;
      if (count_is_dynamic) {
        count = nyr_emit_runtime_call(b, "rt_untag", count, -1, -1, 1, 0);
        if (count < 0)
          return -1;
      }
      int out =
          nyr_emit_runtime_call(b, "rt_tbuf_repeat", list, count, -1, 2, 0);
      if (out < 0)
        return -1;

      int source_len = nyr_peek_list_len_fact(b, list);
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
          out_len = nyr_emit_const(b, 0);
        } else {
          int repeat_v = nyr_emit_const(b, repeat);
          out_len = repeat_v < 0 ? -1
                                 : nyr_push_val(b, NYIR_MUL_I64, source_len,
                                                repeat_v, 0, NULL);
        }
        if (out_len < 0 || !nyr_record_list_len_fact(b, out, out_len))
          return -1;
      }
      int64_t source_bytes = nyr_peek_alloc_fact(b, list);
      if (source_bytes >= 0 && repeat >= 0 &&
          (repeat == 0 || source_bytes <= INT64_MAX / repeat))
        nyr_record_alloc_fact(b, out, source_bytes * repeat);
      return out;
    }
  }
scalar_multiply:
  if (e->as.binary.op && strcmp(e->as.binary.op, "*") == 0 &&
      (nyr_expr_is_cstr(b, e->as.binary.left) ||
       nyr_expr_is_cstr(b, e->as.binary.right))) {
    const expr_t *str_expr = nyr_expr_is_cstr(b, e->as.binary.left)
                                 ? e->as.binary.left
                                 : e->as.binary.right;
    const expr_t *count_expr =
        str_expr == e->as.binary.left ? e->as.binary.right : e->as.binary.left;
    if (nyr_expr_is_f64(b, count_expr) || nyr_expr_is_f32(b, count_expr) ||
        nyr_expr_is_any(b, count_expr)) {
      return nyr_fail(
                 b,
                 "native NYIR lower: string repeat count must be an integer "
                 "(function=%s)",
                 b->current_fn_name ? b->current_fn_name : "<entry>"),
             -1;
    }
    int str = nyr_lower_expr(b, str_expr);
    int count = nyr_lower_expr(b, count_expr);
    if (str < 0 || count < 0)
      return -1;
    return nyr_emit_runtime_call(b, "rt_cstr_repeat", str, count, -1, 2, 0);
  }
  /* BigInt operators must stay in the handle domain.  The generic arithmetic
   * path below is intentionally raw-i64/f64 and would otherwise treat the
   * heap addresses as integers (or route `**` through floating power). */
  bool operator_left_bigint = nyr_expr_is_bigint(b, e->as.binary.left);
  bool operator_right_bigint = nyr_expr_is_bigint(b, e->as.binary.right);

  /*
   * `any` describes the slot ABI, not the value's runtime category. Keep a
   * BigInt classification when it was independently established from the
   * bound global initializer or a declared BigInt producer; only unproven
   * dynamic values stay on the generic arithmetic path.
   */
  /*
   * Memo-bypass: expr_is_bigint consults the type-query memo whose
   * persistent entries can hold misses from pre-collection queries.  For
   * calls to named user fns, resolve the return type straight from the
   * function table so the bigint operator bridge cannot lose an operand.
   */
  if ((!operator_left_bigint || !operator_right_bigint) && e->as.binary.left &&
      e->as.binary.right) {
    const expr_t *ops[2] = {e->as.binary.left, e->as.binary.right};
    bool *flags[2] = {&operator_left_bigint, &operator_right_bigint};
    for (int oi = 0; oi < 2; ++oi) {
      if (flags[oi] || ops[oi]->kind != NY_E_CALL || !ops[oi]->as.call.callee ||
          ops[oi]->as.call.callee->kind != NY_E_IDENT ||
          !ops[oi]->as.call.callee->as.ident.name)
        continue;
      const char *obl =
          ny_name_leaf(ops[oi]->as.call.callee->as.ident.name);
      const stmt_t *obf =
          obl ? nyr_find_user_function((nyr_builder_t *)b, obl) : NULL;
      if (obf && obf->kind == NY_S_FUNC && obf->as.fn.return_type &&
          ny_type_is(obf->as.fn.return_type, "bigint"))
        *flags[oi] = true;
    }
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve",
                           "function=%s op=%s decision=resolve-result-type",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           e->as.binary.op ? e->as.binary.op : "<null>");
  const char *resolved_type = nyr_expr_type_name(b, e);
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-opresolve",
                           "function=%s op=%s result-type=%s",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           e->as.binary.op ? e->as.binary.op : "<null>",
                           resolved_type ? resolved_type : "<unknown>");
  /*
   * Preserve each operand's representation independently.  The result type
   * can be BigInt even when one operand is a native int (for example
   * `bigint_value - 1`); routing that raw integer to the handle ABI without
   * boxing it makes the runtime interpret its bits as a pointer.  The bridge
   * below boxes only the operands not already known to be BigInt handles.
   */
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
      big_symbol = "__bigint_xor";
    if (big_symbol) {
      int left = nyr_lower_expr(b, e->as.binary.left);
      int right = nyr_lower_expr(b, e->as.binary.right);
      if (left < 0 || right < 0)
        return -1;
      /*
       * Register-class coherence: a float producer is not a BigInt handle.
       * A poisoned type fact can route a float-valued operand through this
       * bridge (for example an `any`-declared abs whose body is f64);
       * feeding an f64 register into the handle runtime trips the
       * scalar-class verifier. Resolve the contradiction as machine-f64
       * arithmetic on the numeric values (including fmod for `%`).
       */
      bool left_float =
          nyr_value_is_f64(b, left, 0) || nyr_value_is_f32(b, left, 0);
      bool right_float =
          nyr_value_is_f64(b, right, 0) || nyr_value_is_f32(b, right, 0);
      if ((left_float || right_float) && (strcmp(e->as.binary.op, "+") == 0 ||
                                          strcmp(e->as.binary.op, "-") == 0 ||
                                          strcmp(e->as.binary.op, "*") == 0 ||
                                          strcmp(e->as.binary.op, "/") == 0 ||
                                          strcmp(e->as.binary.op, "%") == 0)) {
        int lf = left;
        int rf = right;
        if (!left_float) {
          lf = nyr_value_scalar_class(b, left) == NYIR_SCALAR_I64_TAGGED
                   ? nyr_emit_runtime_call(b, "rt_any_to_f64", left, -1, -1, 1,
                                           NYIR_SCALAR_F64)
                   : nyr_emit_i64_to_f64(b, left);
        } else if (nyr_value_is_f32(b, left, 0)) {
          lf = nyr_emit_f32_to_f64(b, left);
        }
        if (!right_float) {
          rf = nyr_value_scalar_class(b, right) == NYIR_SCALAR_I64_TAGGED
                   ? nyr_emit_runtime_call(b, "rt_any_to_f64", right, -1, -1, 1,
                                           NYIR_SCALAR_F64)
                   : nyr_emit_i64_to_f64(b, right);
        } else if (nyr_value_is_f32(b, right, 0)) {
          rf = nyr_emit_f32_to_f64(b, right);
        }
        if (lf < 0 || rf < 0)
          return -1;
        if (strcmp(e->as.binary.op, "%") == 0)
          return nyr_emit_runtime_call(b, "rt_fmod_f64", lf, rf, -1, 2,
                                       NYIR_SCALAR_F64);
        nyr_op_t fop = strcmp(e->as.binary.op, "+") == 0   ? NYIR_ADD_F64
                        : strcmp(e->as.binary.op, "-") == 0 ? NYIR_SUB_F64
                        : strcmp(e->as.binary.op, "*") == 0 ? NYIR_MUL_F64
                                                            : NYIR_DIV_F64;
        return nyr_push_val(b, fop, lf, rf, 0, NULL);
      }
      if (!operator_left_bigint)
        left = nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", left, -1, -1,
                                     1, 0);
      if (!operator_right_bigint)
        right = nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", right, -1,
                                      -1, 1, 0);
      return left < 0 || right < 0
                 ? -1
                 : nyr_emit_runtime_call(b, big_symbol, left, right, -1, 2, 0);
    }
  }
  if (e->as.binary.op && strcmp(e->as.binary.op, "**") == 0) {
    int64_t folded = 0;
    /* A folded raw constant needs an extra boxing step when used at an `any`
     * call boundary. Keep the canonical core.pow path there: it returns
     * the tagged dynamic representation directly and avoids a raw/tagged
     * class transition being inferred from the source expression. */
    if (!b->value_for_any_call &&
        nyr_fold_const_pow(e->as.binary.left, e->as.binary.right, &folded)) {
      int c = nyr_emit_const(b, folded);
      if (c < 0)
        return -1;
      return c;
    }
    bool left_f64 = nyr_expr_is_f64(b, e->as.binary.left);
    bool right_f64 = nyr_expr_is_f64(b, e->as.binary.right);
    int64_t constant_exponent = 0;
    bool negative_integer_exponent =
        nyr_const_i64_expr(e->as.binary.right, &constant_exponent, 0) &&
        constant_exponent < 0;
    if (left_f64 || right_f64 || negative_integer_exponent) {
      int base_v = nyr_lower_expr(b, e->as.binary.left);
      int exp_v = nyr_lower_expr(b, e->as.binary.right);
      if (base_v < 0 || exp_v < 0)
        return -1;
      if (!left_f64) {
        base_v = nyr_emit_i64_to_f64(b, base_v);
        if (base_v < 0)
          return -1;
      }
      if (!right_f64) {
        exp_v = nyr_emit_i64_to_f64(b, exp_v);
        if (exp_v < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_f64_pow", base_v, exp_v, -1, 2,
                                   NYIR_SCALAR_F64);
    }
    const stmt_t *pow_fn = nyr_find_user_function(b, "core.pow");
    if (!pow_fn) {
      return nyr_fail(b,
                      "native NYIR lower: canonical core.pow is unavailable"),
             -1;
    }
    call_arg_t args[2] = {
        {.val = (expr_t *)e->as.binary.left},
        {.val = (expr_t *)e->as.binary.right},
    };
    expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
    callee.as.ident.name = pow_fn->as.fn.name;
    expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
    call.semantic.resolved = true;
    call.semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
    call.semantic.canonical_callee = pow_fn->as.fn.name;
    call.semantic.canonical_callee_stmt = (stmt_t *)pow_fn;
    call.as.call.callee = &callee;
    call.as.call.args.data = args;
    call.as.call.args.len = call.as.call.args.cap = 2;
    return nyr_lower_expr(b, &call);
  }
  /* Promote compile-time arithmetic that crosses the tagged integer boundary
   * before it reaches raw NYIR i64 arithmetic.  This is especially important
   * for module globals such as `(1 << 62) - 1 + 1`: the result is a bigint
   * handle, not the raw payload 1 << 62. */
  if (e->as.binary.op &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0)) {
    int64_t folded = 0;
    if (nyr_fold_top_level_int(b, NULL, e, &folded, 0) &&
        (folded >= (INT64_C(1) << 62) || folded <= -(INT64_C(1) << 62))) {
      int raw = nyr_emit_const(b, folded);
      return raw < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", raw,
                                             -1, -1, 1, 0);
    }
  }
  nyr_op_t op = NYIR_NOP;
  nyr_cmp_t cmp = NYIR_CMP_EQ;
  bool is_cmp = nyr_cmp(e->as.binary.op, &cmp);
  if (!is_cmp && !nyr_binop(e->as.binary.op, &op)) {
    return nyr_fail(b, "native NYIR lower: unsupported binary operator '%s'",
                    e->as.binary.op ? e->as.binary.op : "(null)"),
           -1;
  }
  bool left_local_f64 = false;
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_IDENT &&
      e->as.binary.left->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.binary.left->as.ident.name);
    left_local_f64 = local && !local->is_any &&
                     (local->is_f64 || local->semantic_rep == NY_SEM_REP_F64);
  }
  bool right_local_f64 = false;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_IDENT &&
      e->as.binary.right->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.binary.right->as.ident.name);
    right_local_f64 = local && !local->is_any &&
                      (local->is_f64 || local->semantic_rep == NY_SEM_REP_F64);
  }
  bool dbg_is_f64_expr = nyr_expr_is_f64(b, e);
  bool dbg_has_lit = nyr_expr_has_float_literal(e, 0);
  /*
   * Shifts are integer-domain operators: an enclosing float context (the
   * `1.0 /` in `1.0 / float((ij*(ij+1)) >> 1)`) must not pull the shift's
   * integer operands into mul.f64 — the f64 result then re-entered the raw
   * shift bridge as IEEE bits and spectral's divisor became garbage.
   */
  bool shift_op = e->as.binary.op && (strcmp(e->as.binary.op, "<<") == 0 ||
                                      strcmp(e->as.binary.op, ">>") == 0);
  bool expr_f64 = !force_integer_domain && !shift_op &&
                  (inherited_f64 || left_local_f64 || right_local_f64 ||
                   dbg_is_f64_expr || dbg_has_lit);
  bool operands_f32 = e->as.binary.left && e->as.binary.right &&
                      nyr_expr_is_f32(b, e->as.binary.left) &&
                      nyr_expr_is_f32(b, e->as.binary.right);
  bool operands_f64 = nyr_expr_is_f64(b, e->as.binary.left) ||
                      nyr_expr_is_f64(b, e->as.binary.right);
  if (operands_f32 && !operands_f64)
    expr_f64 = false;
  bool expr_f32 = !force_integer_domain && !expr_f64 &&
                  (nyr_expr_is_f32(b, e) || operands_f32);
  if (getenv("NY_DEBUG_ARITH_F32")) {
    const nyr_local_t *dl = NULL, *dr = NULL;
    if (e->as.binary.left && e->as.binary.left->kind == NY_E_IDENT)
      dl = nyr_find_local_nearest(b, e->as.binary.left->as.ident.name);
    if (e->as.binary.right && e->as.binary.right->kind == NY_E_IDENT)
      dr = nyr_find_local_nearest(b, e->as.binary.right->as.ident.name);
    fprintf(stderr,
            "[arith] '%s' inherited=%d lloc64=%d rloc64=%d isf64=%d lit=%d => "
            "f64=%d f32=%d | L(%s sem=%s res=%d rep=%d isf64=%d isf32=%d "
            "any=%d) R(%s sem=%s res=%d rep=%d)\n",
            e->as.binary.op ? e->as.binary.op : "?", (int)inherited_f64,
            (int)left_local_f64, (int)right_local_f64, (int)dbg_is_f64_expr,
            (int)dbg_has_lit, (int)expr_f64, (int)expr_f32, dl ? dl->name : "-",
            e->as.binary.left && e->as.binary.left->semantic.resolved
                ? ny_sem_rep_name(e->as.binary.left->semantic.rep)
                : "unres",
            e->as.binary.left && e->as.binary.left->semantic.resolved ? 1 : 0,
            dl ? dl->semantic_rep : -1, dl ? dl->is_f64 : -1,
            dl ? dl->is_f32 : -1, dl ? dl->is_any : -1, dr ? dr->name : "-",
            e->as.binary.right && e->as.binary.right->semantic.resolved
                ? ny_sem_rep_name(e->as.binary.right->semantic.rep)
                : "unres",
            e->as.binary.right && e->as.binary.right->semantic.resolved ? 1 : 0,
            dr ? dr->semantic_rep : -1);
  }
  /* A binary expression can inherit a stale floating semantic rep from an
   * untyped sequence element.  Its actual index ABI is authoritative: bare
   * list/any receivers are not f32/f64 unless their declared element type
   * says so explicitly. */
  if (expr_f32 || expr_f64) {
    const expr_t *operands[2] = {e->as.binary.left, e->as.binary.right};
    for (size_t oi = 0; oi < 2; ++oi) {
      const expr_t *operand = operands[oi];
      if (!operand || operand->kind != NY_E_INDEX ||
          !operand->as.index.target ||
          operand->as.index.target->kind != NY_E_IDENT ||
          !operand->as.index.target->as.ident.name)
        continue;
      const nyr_local_t *sequence =
          nyr_find_local(b, operand->as.index.target->as.ident.name);
      if (!sequence)
        continue;
      const char *decl = sequence->type_name;
      bool explicit_float = decl && (strstr(decl, "<f32>") != NULL ||
                                     strstr(decl, "<f64>") != NULL);
      if (!explicit_float && (sequence->is_list || sequence->is_any) &&
          !inherited_f64 && !left_local_f64 && !right_local_f64) {
        expr_f32 = false;
        expr_f64 = false;
        break;
      }
    }
  }
  bool float_mod = !is_cmp && e->as.binary.op &&
                   strcmp(e->as.binary.op, "%") == 0 &&
                   (nyr_expr_is_f64(b, e->as.binary.left) ||
                    nyr_expr_is_f64(b, e->as.binary.right));
  if (float_mod) {
    int left = nyr_lower_expr(b, e->as.binary.left);
    int right = nyr_lower_expr(b, e->as.binary.right);
    if (left < 0 || right < 0)
      return -1;
    if (!nyr_expr_is_f64(b, e->as.binary.left)) {
      left = nyr_emit_i64_to_f64(b, left);
      if (left < 0)
        return -1;
    }
    if (!nyr_expr_is_f64(b, e->as.binary.right)) {
      right = nyr_emit_i64_to_f64(b, right);
      if (right < 0)
        return -1;
    }
    return nyr_emit_runtime_call(b, "rt_fmod_f64", left, right, -1, 2,
                                 NYIR_SCALAR_F64);
  }
  /* Operand float facts must exist before the opcode selection below: a
   * member/impl read can classify f64 even when the whole-expression
   * inference stays conservative, and the integer op over f64-typed operands
   * is exactly what the machine-form gate rejects. */
  bool left_f64 = nyr_expr_is_f64(b, e->as.binary.left) ||
                  (expr_f64 && e->as.binary.left &&
                   (e->as.binary.left->kind == NY_E_BINARY ||
                    e->as.binary.left->kind == NY_E_UNARY) &&
                   nyr_expr_has_float_literal(e->as.binary.left, 0));
  bool right_f64 = nyr_expr_is_f64(b, e->as.binary.right) ||
                   (expr_f64 && e->as.binary.right &&
                    (e->as.binary.right->kind == NY_E_BINARY ||
                     e->as.binary.right->kind == NY_E_UNARY) &&
                    nyr_expr_has_float_literal(e->as.binary.right, 0));
  bool left_f32 = nyr_expr_is_f32(b, e->as.binary.left);
  bool right_f32 = nyr_expr_is_f32(b, e->as.binary.right);
  /* The exact local binding owns the operand ABI. A stale use-site semantic
   * float rep must not turn an explicitly integer comparison into fcmp. */
  const expr_t *operands[2] = {e->as.binary.left, e->as.binary.right};
  bool *operand_f64[2] = {&left_f64, &right_f64};
  bool *operand_f32[2] = {&left_f32, &right_f32};
  for (size_t oi = 0; oi < 2; ++oi) {
    const expr_t *operand = operands[oi];
    if (!operand || operand->kind != NY_E_IDENT || !operand->as.ident.name)
      continue;
    const nyr_local_t *local = nyr_find_local_ctx(b, operand->as.ident.name,
                                                  operand->as.ident.syntax_ctx);
    if (!local)
      local = nyr_find_local_nearest(b, operand->as.ident.name);
    if (local && local->type_name &&
        !ny_native_type_name_is_any(local->type_name)) {
      /* The slot's own fact decides the operand domain: the recorded local
       * representation first, then the declared type's representation, and
       * only then the raw name spelling. */
      ny_sem_rep_t rep = local->semantic_rep;
      if (rep != NY_SEM_REP_F64 && rep != NY_SEM_REP_F32)
        rep = ny_semantic_rep_for_type(local->type_name);
      if (rep == NY_SEM_REP_F64) {
        *operand_f64[oi] = true;
        *operand_f32[oi] = false;
      } else if (rep == NY_SEM_REP_F32) {
        *operand_f64[oi] = false;
        *operand_f32[oi] = true;
      } else {
        *operand_f64[oi] = ny_native_type_name_is_f64(local->type_name);
        *operand_f32[oi] = ny_native_type_name_is_f32(local->type_name);
      }
    }
  }
  if (!is_cmp && expr_f32 &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0 ||
       strcmp(e->as.binary.op, "/") == 0)) {
    if (strcmp(e->as.binary.op, "+") == 0)
      op = NYIR_ADD_F32;
    else if (strcmp(e->as.binary.op, "-") == 0)
      op = NYIR_SUB_F32;
    else if (strcmp(e->as.binary.op, "*") == 0)
      op = NYIR_MUL_F32;
    else
      op = NYIR_DIV_F32;
  } else if (!is_cmp && (expr_f64 || left_f64 || right_f64) &&
             (strcmp(e->as.binary.op, "+") == 0 ||
              strcmp(e->as.binary.op, "-") == 0 ||
              strcmp(e->as.binary.op, "*") == 0 ||
              strcmp(e->as.binary.op, "/") == 0)) {
    if (strcmp(e->as.binary.op, "+") == 0)
      op = NYIR_ADD_F64;
    else if (strcmp(e->as.binary.op, "-") == 0)
      op = NYIR_SUB_F64;
    else if (strcmp(e->as.binary.op, "*") == 0)
      op = NYIR_MUL_F64;
    else
      op = NYIR_DIV_F64;
  }
  bool trace_arithmetic = ny_dbg_is_enabled(NY_DBG_HEURISTIC);
  bool dynamic_evidence =
      trace_arithmetic && (nyr_expr_is_any(b, e->as.binary.left) ||
                           nyr_expr_is_any(b, e->as.binary.right));
  bool float_evidence =
      trace_arithmetic &&
      (expr_f64 || expr_f32 || left_f64 || right_f64 || left_f32 || right_f32 ||
       nyr_expr_has_float_literal(e, 0));
  if (!is_cmp && (float_evidence || dynamic_evidence) && trace_arithmetic &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0 ||
       strcmp(e->as.binary.op, "/") == 0)) {
    const char *selected = op == NYIR_ADD_F32   ? "f32.add"
                           : op == NYIR_SUB_F32 ? "f32.sub"
                           : op == NYIR_MUL_F32 ? "f32.mul"
                           : op == NYIR_DIV_F32 ? "f32.div"
                           : op == NYIR_ADD_F64 ? "f64.add"
                           : op == NYIR_SUB_F64 ? "f64.sub"
                           : op == NYIR_MUL_F64 ? "f64.mul"
                           : op == NYIR_DIV_F64 ? "f64.div"
                                                : "i64/dynamic";
    ny_dbg_heuristic_event(
        "native-arithmetic",
        "function=%s operator=%s candidate=%s evidence{dynamic_left=%d "
        "dynamic_right=%d inherited_f64=%d expr_f64=%d expr_f32=%d left_f64=%d "
        "right_f64=%d left_f32=%d right_f32=%d left_local_f64=%d "
        "right_local_f64=%d float_literal=%d}",
        b->current_fn_name ? b->current_fn_name : "<entry>", e->as.binary.op,
        selected, (int)nyr_expr_is_any(b, e->as.binary.left),
        (int)nyr_expr_is_any(b, e->as.binary.right), (int)inherited_f64,
        (int)expr_f64, (int)expr_f32, (int)left_f64, (int)right_f64,
        (int)left_f32, (int)right_f32, (int)left_local_f64,
        (int)right_local_f64, (int)nyr_expr_has_float_literal(e, 0));
  }
  bool left_cstr = nyr_expr_is_cstr(b, e->as.binary.left);
  bool right_cstr = nyr_expr_is_cstr(b, e->as.binary.right);
  /* `await` always returns through the dynamic ABI even when this expression
   * has no type fact yet. Keep that representation visible to its parent
   * operator; otherwise an inline `await task == 12` compares the tagged
   * result as a raw integer, while assigning the same result to a local works.
   */
  bool left_await_result = is_cmp && e->as.binary.left &&
                           e->as.binary.left->kind == NY_E_UNARY &&
                           e->as.binary.left->as.unary.op &&
                           strcmp(e->as.binary.left->as.unary.op, "await") == 0;
  bool right_await_result =
      is_cmp && e->as.binary.right && e->as.binary.right->kind == NY_E_UNARY &&
      e->as.binary.right->as.unary.op &&
      strcmp(e->as.binary.right->as.unary.op, "await") == 0;
  bool left_any = nyr_expr_is_any(b, e->as.binary.left) || left_await_result;
  bool right_any = nyr_expr_is_any(b, e->as.binary.right) || right_await_result;
  /* Untyped parameters may arrive as expanded dynamic triples even when the
   * enclosing expression proves that this arithmetic is floating-point. */
  if (inherited_f64 && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_IDENT &&
      e->as.binary.left->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local(b, e->as.binary.left->as.ident.name);
    left_any =
        left_any || (local && (local->is_any || local->dyn_tag_slot >= 0));
  }
  if (inherited_f64 && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_IDENT &&
      e->as.binary.right->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local(b, e->as.binary.right->as.ident.name);
    right_any =
        right_any || (local && (local->is_any || local->dyn_tag_slot >= 0));
  }
  bool left_scalar_get =
      e->as.binary.left && e->as.binary.left->kind == NY_E_CALL &&
      ny_native_call_leaf(e->as.binary.left) &&
      strcmp(ny_native_call_leaf(e->as.binary.left), "get") == 0 &&
      e->as.binary.left->as.call.args.len >= 2 &&
      nyr_expr_is_list(b, e->as.binary.left->as.call.args.data[0].val);
  bool right_scalar_get =
      e->as.binary.right && e->as.binary.right->kind == NY_E_CALL &&
      ny_native_call_leaf(e->as.binary.right) &&
      strcmp(ny_native_call_leaf(e->as.binary.right), "get") == 0 &&
      e->as.binary.right->as.call.args.len >= 2 &&
      nyr_expr_is_list(b, e->as.binary.right->as.call.args.data[0].val);
  /* A method `list.get(...)` can return a tagged dynamic value even when the
   * receiver itself is a statically declared list. Detect the actual lowered
   * result ABI rather than inferring it from the receiver's container type;
   * unbox that value before raw integer arithmetic. Float-context gets have
   * already crossed rt_any_to_f64 and return raw float bits instead. */
  bool left_scalar_get_mem =
      e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      (e->as.binary.left->as.memcall.args.len == 1 ||
       e->as.binary.left->as.memcall.args.len == 2) &&
      e->as.binary.left->as.memcall.target &&
      nyr_expr_is_list(b, e->as.binary.left->as.memcall.target) &&
      (nyr_expr_is_dyn_list(b, e->as.binary.left->as.memcall.target) ||
       nyr_expr_is_any(b, e->as.binary.left) ||
       (e->as.binary.left->semantic.resolved &&
        e->as.binary.left->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)) &&
      !nyr_expr_is_f64(b, e->as.binary.left) &&
      !(e->as.binary.left->as.memcall.args.len == 2 &&
        e->as.binary.left->as.memcall.args.data[1].val &&
        nyr_expr_is_f64(b, e->as.binary.left->as.memcall.args.data[1].val));
  bool right_scalar_get_mem =
      e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      (e->as.binary.right->as.memcall.args.len == 1 ||
       e->as.binary.right->as.memcall.args.len == 2) &&
      e->as.binary.right->as.memcall.target &&
      nyr_expr_is_list(b, e->as.binary.right->as.memcall.target) &&
      (nyr_expr_is_dyn_list(b, e->as.binary.right->as.memcall.target) ||
       nyr_expr_is_any(b, e->as.binary.right) ||
       (e->as.binary.right->semantic.resolved &&
        e->as.binary.right->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)) &&
      !nyr_expr_is_f64(b, e->as.binary.right) &&
      !(e->as.binary.right->as.memcall.args.len == 2 &&
        e->as.binary.right->as.memcall.args.data[1].val &&
        nyr_expr_is_f64(b, e->as.binary.right->as.memcall.args.data[1].val));
  /* Arithmetic on an `any` value produces a tagged dynamic result even when
   * HM refines the enclosing expression to int. Preserve that provenance so
   * comparisons use the dynamic ABI (for example `v % 2 == 0`). */
  bool left_dynamic_result = left_any;
  bool right_dynamic_result = right_any;
  /* The native shift adapters are raw scalar producers even though their
   * language declarations remain dynamically typed for the interpreter ABI.
   * Keep that provenance when they feed a bitwise tree; otherwise the generic
   * dynamic normalization below calls rt_any_to_i64 on an already-raw word
   * and corrupts every odd-valued shift result. */
  bool left_raw_scalar =
      !left_await_result &&
      (nyr_expr_is_raw_scalar(b, e->as.binary.left, 0) ||
       nyr_expr_is_raw_integer_conversion(b, e->as.binary.left));
  bool right_raw_scalar =
      !right_await_result &&
      (nyr_expr_is_raw_scalar(b, e->as.binary.right, 0) ||
       nyr_expr_is_raw_integer_conversion(b, e->as.binary.right));
  if (left_raw_scalar) {
    left_any = false;
    left_dynamic_result = false;
  }
  if (right_raw_scalar) {
    right_any = false;
    right_dynamic_result = false;
  }
  /* An arithmetic expression used as an argument to an `any` formal is
   * still typed arithmetic when both operands are statically raw integers.
   * Do the operation in the raw domain and let the outer call boundary box
   * the single result; sending the raw operands to rt_any_* makes odd raw
   * values look pre-tagged and halves them. */
  if (b->value_for_any_call) {
    int64_t folded_left = 0, folded_right = 0;
    /*
     * The callee return fact outranks the stale call-site stamp here too:
     * an untyped alias call (`fast_ref(7, 8)`) carries RAW_INT from HM
     * specialization while the value crosses the dynamic ABI, so it must
     * not count as a statically raw operand or the any-formal comparison
     * drops to the raw comparator.
     */
    bool left_int_literal = e->as.binary.left &&
                            e->as.binary.left->kind == NY_E_LITERAL &&
                            e->as.binary.left->as.literal.kind == NY_LIT_INT &&
                            !ny_expr_is_nil_literal(e->as.binary.left);
    bool right_int_literal =
        e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
        e->as.binary.right->as.literal.kind == NY_LIT_INT &&
        !ny_expr_is_nil_literal(e->as.binary.right);
    bool left_dynamic_fact = e->as.binary.left && !left_int_literal &&
                             nyr_expr_is_any(b, e->as.binary.left);
    bool right_dynamic_fact = e->as.binary.right && !right_int_literal &&
                              nyr_expr_is_any(b, e->as.binary.right);
    bool left_raw_context =
        left_int_literal ||
        (!left_dynamic_fact &&
         ((e->as.binary.left && e->as.binary.left->semantic.resolved &&
           e->as.binary.left->semantic.rep == NY_SEM_REP_RAW_INT) ||
          nyr_fold_top_level_int(b, NULL, e->as.binary.left, &folded_left, 0)));
    bool right_raw_context =
        right_int_literal ||
        (!right_dynamic_fact &&
         ((e->as.binary.right && e->as.binary.right->semantic.resolved &&
           e->as.binary.right->semantic.rep == NY_SEM_REP_RAW_INT) ||
          nyr_fold_top_level_int(b, NULL, e->as.binary.right, &folded_right,
                                 0)));
    if (left_raw_context && right_raw_context) {
      left_any = false;
      right_any = false;
      left_dynamic_result = false;
      right_dynamic_result = false;
    }
  }
  /* Free get on an any receiver lowers through rt_value_get_tagged, even
   * when a scalar fallback makes its semantic result look like a raw int. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_CALL &&
      ny_native_call_leaf(e->as.binary.left) &&
      strcmp(ny_native_call_leaf(e->as.binary.left), "get") == 0 &&
      e->as.binary.left->as.call.args.len >= 2 &&
      nyr_expr_is_any(b, e->as.binary.left->as.call.args.data[0].val))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_CALL &&
      ny_native_call_leaf(e->as.binary.right) &&
      strcmp(ny_native_call_leaf(e->as.binary.right), "get") == 0 &&
      e->as.binary.right->as.call.args.len >= 2 &&
      nyr_expr_is_any(b, e->as.binary.right->as.call.args.data[0].val))
    right_dynamic_result = true;
  /* A list `.get` is an any-valued boundary even when its receiver's
   * unparameterized list type made semantic inference look container-like.
   * Preserve that provenance so nested lists use structural equality instead
   * of pointer comparison and scalar slots are decoded exactly once. */
  const expr_t *left_get_receiver =
      nyr_attached_get_receiver(e->as.binary.left);
  const expr_t *right_get_receiver =
      nyr_attached_get_receiver(e->as.binary.right);
  if (left_get_receiver && nyr_expr_is_dyn_list(b, left_get_receiver))
    left_dynamic_result = true;
  if (right_get_receiver && nyr_expr_is_dyn_list(b, right_get_receiver))
    right_dynamic_result = true;
  /* A `get` result compared with a sequence is itself a container payload,
   * even when its receiver's generic return type was not recoverable from
   * the call's canonical declaration.  This matters for chains such as
   * `chunk(...).get(0, nil) == [1, 2]`: the accessor returns the nested
   * buffer handle and equality must inspect sequence contents, not compare
   * the handle word with the expected handle. */
  if (left_get_receiver && (nyr_expr_is_list(b, e->as.binary.right) ||
                            nyr_expr_is_dyn_list(b, e->as.binary.right) ||
                            nyr_expr_is_tuple(b, e->as.binary.right)))
    left_dynamic_result = true;
  if (right_get_receiver && (nyr_expr_is_list(b, e->as.binary.left) ||
                             nyr_expr_is_dyn_list(b, e->as.binary.left) ||
                             nyr_expr_is_tuple(b, e->as.binary.left)))
    right_dynamic_result = true;
  /* Dictionary `.get` also returns a tagged dynamic value.  Preserve that
   * provenance even when semantic inference narrows the result to a scalar;
   * otherwise equality compares its tag against an unboxed literal. */
  if (left_get_receiver && nyr_expr_is_dict(b, left_get_receiver))
    left_dynamic_result = true;
  if (right_get_receiver && nyr_expr_is_dict(b, right_get_receiver))
    right_dynamic_result = true;
  /* The parser represents attached calls such as `d.get(k)` as a call whose
   * callee is a member expression.  Preserve the same dynamic dictionary
   * read ABI as the direct NY_E_MEMCALL form; otherwise the later list
   * classifier can mistake the conservative typed-buffer carrier for a list
   * and emit sequence concatenation for scalar values. */
  if (nyr_expr_is_dict_read(b, e->as.binary.left))
    left_dynamic_result = true;
  if (nyr_expr_is_dict_read(b, e->as.binary.right))
    right_dynamic_result = true;
  /* Direct indexing is the same dynamic boundary as `.get`.  Keep the
   * provenance on the expression itself so `seq[1] == 2` uses the tagged
   * comparator just like `def v = seq[1]; v == 2`.  Without this marker the
   * native path treats the indexed value as a raw i64 and compares its boxed
   * payload against the tagged literal. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
      e->as.binary.left->as.index.target) {
    const expr_t *target = e->as.binary.left->as.index.target;
    bool dynamic_index = nyr_expr_is_dynamic_sequence_value(b, target);
    dynamic_index = dynamic_index || (nyr_expr_is_dyn_list(b, target) &&
                                      !nyr_expr_is_bytes(b, target) &&
                                      !nyr_expr_is_range(b, target));
    /* An unparameterized `list(n)` is represented by the managed dynamic
     * sequence ABI even when local inference only records `is_list`.  The
     * accessor returns a tagged word; only a proven `list<int>`-style index
     * may stay raw. */
    dynamic_index =
        dynamic_index ||
        (nyr_expr_is_list(b, target) &&
         !nyr_expr_is_typed_scalar_index(b, e->as.binary.left) &&
         !nyr_expr_is_bytes(b, target) && !nyr_expr_is_range(b, target));
    if (target->kind == NY_E_IDENT && target->as.ident.name) {
      const nyr_local_t *local =
          nyr_find_local_nearest(b, target->as.ident.name);
      dynamic_index =
          dynamic_index ||
          (local && local->is_list &&
           !nyr_expr_is_typed_scalar_index(b, e->as.binary.left)) ||
          (local && (local->is_any || nyr_local_is_dynamic_sequence(local)));
    }
    if (dynamic_index)
      left_dynamic_result = true;
  }
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
      e->as.binary.right->as.index.target) {
    const expr_t *target = e->as.binary.right->as.index.target;
    bool dynamic_index = nyr_expr_is_dynamic_sequence_value(b, target);
    dynamic_index = dynamic_index || (nyr_expr_is_dyn_list(b, target) &&
                                      !nyr_expr_is_bytes(b, target) &&
                                      !nyr_expr_is_range(b, target));
    dynamic_index =
        dynamic_index ||
        (nyr_expr_is_list(b, target) &&
         !nyr_expr_is_typed_scalar_index(b, e->as.binary.right) &&
         !nyr_expr_is_bytes(b, target) && !nyr_expr_is_range(b, target));
    if (target->kind == NY_E_IDENT && target->as.ident.name) {
      const nyr_local_t *local =
          nyr_find_local_nearest(b, target->as.ident.name);
      dynamic_index =
          dynamic_index ||
          (local && local->is_list &&
           !nyr_expr_is_typed_scalar_index(b, e->as.binary.right)) ||
          (local && (local->is_any || nyr_local_is_dynamic_sequence(local)));
    }
    if (dynamic_index)
      right_dynamic_result = true;
  }
  /* Typed list reads stay raw for scalar arithmetic, but a list element that
   * is compared with another sequence is a nested container boundary. Route
   * that case through the structural dynamic comparator. */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      e->as.binary.left->as.memcall.target &&
      nyr_expr_is_list(b, e->as.binary.left->as.memcall.target) &&
      (nyr_expr_is_list(b, e->as.binary.right) ||
       nyr_expr_is_dyn_list(b, e->as.binary.right)))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      e->as.binary.right->as.memcall.target &&
      nyr_expr_is_list(b, e->as.binary.right->as.memcall.target) &&
      (nyr_expr_is_list(b, e->as.binary.left) ||
       nyr_expr_is_dyn_list(b, e->as.binary.left)))
    right_dynamic_result = true;
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_BINARY)
    left_dynamic_result = left_dynamic_result ||
                          nyr_arith_expr_is_dynamic(b, e->as.binary.left, 0);
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_BINARY &&
      e->as.binary.left->as.binary.op &&
      strcmp(e->as.binary.left->as.binary.op, "%") == 0 &&
      (nyr_expr_is_any(b, e->as.binary.left->as.binary.left) ||
       nyr_expr_is_any(b, e->as.binary.left->as.binary.right)))
    left_dynamic_result = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_BINARY)
    right_dynamic_result = right_dynamic_result ||
                           nyr_arith_expr_is_dynamic(b, e->as.binary.right, 0);
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_BINARY &&
      e->as.binary.right->as.binary.op &&
      strcmp(e->as.binary.right->as.binary.op, "%") == 0 &&
      (nyr_expr_is_any(b, e->as.binary.right->as.binary.left) ||
       nyr_expr_is_any(b, e->as.binary.right->as.binary.right)))
    right_dynamic_result = true;
  bool left_bigint = nyr_expr_is_bigint(b, e->as.binary.left);
  bool right_bigint = nyr_expr_is_bigint(b, e->as.binary.right);
  bool left_user_call =
      e->as.binary.left && e->as.binary.left->kind == NY_E_CALL &&
      e->as.binary.left->as.call.callee &&
      e->as.binary.left->as.call.callee->kind == NY_E_IDENT &&
      nyr_user_defined_fn(b, e->as.binary.left->as.call.callee->as.ident.name);
  bool right_user_call =
      e->as.binary.right && e->as.binary.right->kind == NY_E_CALL &&
      e->as.binary.right->as.call.callee &&
      e->as.binary.right->as.call.callee->kind == NY_E_IDENT &&
      nyr_user_defined_fn(b, e->as.binary.right->as.call.callee->as.ident.name);
  /* Bitwise expressions operate on the native two's-complement i64 domain.
   * A large literal normally lowers to a boxed bigint because it cannot fit
   * in the tagged VM integer representation.  That representation is wrong
   * when the literal is the raw-i64 side of a bitwise expression (including
   * an equality against a rotated/shifted value). */
  bool bitwise_op = nyr_is_bitwise_operator(e->as.binary.op);
  /*
   * Module constants are evaluated in their defining module context. Fold
   * an entirely constant bitwise tree there, before its leaves acquire
   * conservative dynamic classes at an importing call site. This also keeps
   * the emitted value aligned with the declared raw-int formal ABI.
   */
  if (bitwise_op) {
    const char *constant_module =
        b->module_name && b->module_name[0]
            ? b->module_name
            : nyr_module_for_initializer_expr(b->prog, e);
    int64_t folded = 0;
    bool fold_ok = nyr_fold_top_level_int_in_module(b, NULL, constant_module, e,
                                                    &folded, 0);
    if (fold_ok) {
      int value = nyr_ir_emit(&b->nyir,
                            (nyr_inst_t){.op = NYIR_CONST_I64,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .imm = folded,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
      if (value < 0) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      b->last_arith_raw = 1;
      b->last_arith_expr = e;
      return value;
    }
  }
  bool left_bitwise = nyr_expr_is_bitwise_tree(b, e->as.binary.left, 0);
  bool right_bitwise = nyr_expr_is_bitwise_tree(b, e->as.binary.right, 0);
  bool raw_left = bitwise_op || right_bitwise;
  bool raw_right = bitwise_op || left_bitwise;
  bool propagate_f64 =
      !force_integer_domain && (inherited_f64 || nyr_expr_is_f64(b, e));
  if (propagate_f64)
    b->f64_context_depth++;
  unsigned saved_f64_depth = b->f64_context_depth;
  if (force_integer_domain)
    b->f64_context_depth = 0;
  int a = (raw_left && e->as.binary.left &&
           e->as.binary.left->kind == NY_E_LITERAL &&
           e->as.binary.left->as.literal.kind == NY_LIT_INT &&
           (e->as.binary.left->as.literal.as.i >= (INT64_C(1) << 62) ||
            e->as.binary.left->as.literal.as.i <= -(INT64_C(1) << 62)))
              ? nyr_emit_const(b, e->as.binary.left->as.literal.as.i)
              : nyr_lower_expr(b, e->as.binary.left);
  int rhs = (raw_right && e->as.binary.right &&
             e->as.binary.right->kind == NY_E_LITERAL &&
             e->as.binary.right->as.literal.kind == NY_LIT_INT &&
             (e->as.binary.right->as.literal.as.i >= (INT64_C(1) << 62) ||
              e->as.binary.right->as.literal.as.i <= -(INT64_C(1) << 62)))
                ? nyr_emit_const(b, e->as.binary.right->as.literal.as.i)
                : nyr_lower_expr(b, e->as.binary.right);
  if (force_integer_domain)
    b->f64_context_depth = saved_f64_depth;
  if (propagate_f64)
    b->f64_context_depth--;
  if (a < 0 || rhs < 0)
    return -1;
  /* A nested attached-get result is a container payload even when its
   * producer's conservative scalar class later clears `dynamic_result`.
   * Resolve sequence equality at the semantic boundary, before those
   * scalar-normalization rules run.  rt_any_eq accepts raw native tbuf
   * handles and tagged managed sequences, so this preserves both storage
   * representations and avoids comparing the handle word itself. */
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE)) {
    const expr_t *left_receiver = nyr_attached_get_receiver(e->as.binary.left);
    const expr_t *right_receiver =
        nyr_attached_get_receiver(e->as.binary.right);
    bool left_gets_sequence =
        left_receiver && (nyr_expr_is_list(b, e->as.binary.right) ||
                          nyr_expr_is_dyn_list(b, e->as.binary.right) ||
                          nyr_expr_is_tuple(b, e->as.binary.right));
    bool right_gets_sequence =
        right_receiver && (nyr_expr_is_list(b, e->as.binary.left) ||
                           nyr_expr_is_dyn_list(b, e->as.binary.left) ||
                           nyr_expr_is_tuple(b, e->as.binary.left));
    if (left_gets_sequence || right_gets_sequence) {
      int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
      if (equal < 0 || true_imm < 0)
        return -1;
      int compared = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                       .dst = -1,
                                                       .a = equal,
                                                       .b = true_imm,
                                                       .cmp = cmp});
      return nyr_box_bool(b, compared);
    }
  }
  /* A specialized direct call can decode its canonical dynamic return at
   * this expression boundary.  Respect the emitted value ABI: otherwise an
   * equality compares a raw result with a tagged literal through rt_any_eq. */
  if (nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_RAW ||
      nyr_value_is_untagged_i64_call(b, a)) {
    left_any = false;
    left_dynamic_result = false;
    /* A source-level `number` result can be a raw-int fast-path result for
     * intrinsics such as clamp/min/max. Do not keep a stale contextual f64
     * decision from HM once the emitted call's physical ABI is known. */
    left_f64 = false;
    left_f32 = false;
  }
  if (nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_RAW ||
      nyr_value_is_untagged_i64_call(b, rhs)) {
    right_any = false;
    right_dynamic_result = false;
    right_f64 = false;
    right_f32 = false;
  }
  bool shift_value_op =
      e->as.binary.op && (strcmp(e->as.binary.op, "<<") == 0 ||
                          strcmp(e->as.binary.op, ">>") == 0);
  /* Bitwise NYIR instructions consume raw i64 words, but a BigInt or an
   * unboxed dynamic value may be a managed heap handle. Keep those operands
   * in the tagged runtime ABI so the runtime can distinguish integers from
   * BigInt objects and apply arbitrary-precision two's-complement semantics.
   * Proven scalar operands still use the raw machine instruction below. */
  bool bitwise_value_op =
      e->as.binary.op &&
      (strcmp(e->as.binary.op, "&") == 0 || strcmp(e->as.binary.op, "|") == 0 ||
       strcmp(e->as.binary.op, "^") == 0);

  /*
   * A load8/load32/load64 leaf result is the raw slot payload by ABI, not a
   * tagged dynamic word.  Classifying it as `any` here made `i64buf_load(m, i)
   * & 0xFF` round-trip the payload through rt_tag + rt_and + rt_any_to_i64,
   * and the decode's rt_is_str probe then read arbitrary odd payload words as
   * candidate addresses — a value-changing, ASLR-dependent undefined read
   * (the pbkdf2 bench checksum differed on every AOT run).  Raw-read operands
   * stay in the machine-word domain and take the plain NYIR instruction.
   */
  if (bitwise_value_op) {
    if (nyr_expr_is_raw_dynamic_read(b, e->as.binary.left)) {
      left_any = false;
      left_dynamic_result = false;
    }
    if (nyr_expr_is_raw_dynamic_read(b, e->as.binary.right)) {
      right_any = false;
      right_dynamic_result = false;
    }
  }
  /*
   * A statically typed integer bitwise expression has a raw-i64 result ABI,
   * even when its imported constant leaves were conservatively loaded as
   * tagged values.  Decode those leaves once and keep the whole bitwise tree
   * in the integer domain.  Routing only the inner operations through the
   * tagged runtime ABI mixed tagged intermediates with raw outer operands
   * (the imported WindowBackendContract mask lost its highest capability).
   */
  bool typed_integer_bitwise = bitwise_value_op && e->semantic.resolved &&
                               e->semantic.rep == NY_SEM_REP_RAW_INT;
  if (typed_integer_bitwise) {
    nyr_scalar_class_t left_class = nyr_value_scalar_class(b, a);
    nyr_scalar_class_t right_class = nyr_value_scalar_class(b, rhs);
    if (left_class == NYIR_SCALAR_I64_TAGGED) {
      a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1,
                                NYIR_SCALAR_I64_RAW);
      if (a < 0)
        return -1;
    }
    if (right_class == NYIR_SCALAR_I64_TAGGED) {
      rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1,
                                  NYIR_SCALAR_I64_RAW);
      if (rhs < 0)
        return -1;
    }
    left_any = right_any = false;
    left_dynamic_result = right_dynamic_result = false;
    left_bigint = right_bigint = false;
  }
  /*
   * Use the representation-aware runtime bridge whenever an operand may be
   * dynamically tagged or may be a BigInt. Proven raw scalar operands stay
   * in NYIR: the bridge returns a tagged integer, which would otherwise be
   * mistaken for a raw result by consumers such as array indexing.
   */
  if (!is_cmp && bitwise_value_op &&
      (left_any || right_any || left_dynamic_result || right_dynamic_result ||
       left_bigint || right_bigint)) {
    /*
     * A nested dynamic bitwise expression already returns a canonical Ny
     * value.  Re-tagging its word treats (value << 1) | 1 as a raw integer
     * and doubles the value each time another operator is composed.  Only
     * raw scalar producers need the raw-to-tagged bridge here.
     */
    if (nyr_value_scalar_class(b, a) != NYIR_SCALAR_I64_TAGGED) {
      a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1,
                                NYIR_SCALAR_I64_TAGGED);
      if (a < 0)
        return -1;
    }
    if (nyr_value_scalar_class(b, rhs) != NYIR_SCALAR_I64_TAGGED) {
      rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
      if (rhs < 0)
        return -1;
    }
    const char *runtime_op = strcmp(e->as.binary.op, "&") == 0   ? "rt_and"
                             : strcmp(e->as.binary.op, "|") == 0 ? "rt_or"
                                                                 : "rt_xor";
    return nyr_emit_runtime_call(b, runtime_op, a, rhs, -1, 2,
                                 NYIR_SCALAR_I64_TAGGED);
  }
  bool left_value_f64 = nyr_value_is_f64(b, a, 0);
  bool right_value_f64 = nyr_value_is_f64(b, rhs, 0);
  bool left_value_f32 = nyr_value_is_f32(b, a, 0);
  bool right_value_f32 = nyr_value_is_f32(b, rhs, 0);
  /* A lowered operand's concrete scalar class is authoritative over stale
   * contextual AST facts for every scalar operation, including an earlier
   * BigInt guess. For example, a generic `abs` call may retain a broad
   * numeric type while its NYIR producer is concretely f64; routing that
   * value to rt_bigint_cmp_raw violates the call ABI. In particular, a
   * mutable f32 global can have a contextual f64 stamp at a use site; ORing
   * that stamp with its actual f32 load selects f64 arithmetic and treats the
   * f32 payload as an unconverted word.  Preserve AST inference only when the
   * lowered value is not itself classified as either float width (for
   * example, an integer literal awaiting its contextual conversion). */
  if (left_value_f64) {
    left_f64 = true;
    left_f32 = false;
    left_bigint = false;
  } else if (left_value_f32) {
    left_f64 = false;
    left_f32 = true;
    left_bigint = false;
  }
  if (right_value_f64) {
    right_f64 = true;
    right_f32 = false;
    right_bigint = false;
  } else if (right_value_f32) {
    right_f64 = false;
    right_f32 = true;
    right_bigint = false;
  }
  /* The opcode was initially selected from AST/context types before either
   * operand was lowered.  If both concrete operands resolve to f32, correct
   * a stale contextual f64 opcode as well; otherwise the f32 values reach a
   * double operation as unextended IEEE-754 payloads. */
  if (!is_cmp && (left_value_f32 || right_value_f32) && !left_value_f64 &&
      !right_value_f64 &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0 ||
       strcmp(e->as.binary.op, "/") == 0)) {
    if (strcmp(e->as.binary.op, "+") == 0)
      op = NYIR_ADD_F32;
    else if (strcmp(e->as.binary.op, "-") == 0)
      op = NYIR_SUB_F32;
    else if (strcmp(e->as.binary.op, "*") == 0)
      op = NYIR_MUL_F32;
    else
      op = NYIR_DIV_F32;
  }
  /* Use the lowered producer classes to finish opcode selection.  Typed
   * `number` calls can return f64 even when the call-site AST fact is
   * conservative; in a mixed integer/f64 expression, selecting I64 here
   * would either fail the machine-shape verifier or reinterpret the float.
   * The conversion block below then normalizes the integer peer to f64. */
  if (!is_cmp && (left_value_f64 || right_value_f64) &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
       strcmp(e->as.binary.op, "*") == 0 ||
       strcmp(e->as.binary.op, "/") == 0)) {
    if (strcmp(e->as.binary.op, "+") == 0)
      op = NYIR_ADD_F64;
    else if (strcmp(e->as.binary.op, "-") == 0)
      op = NYIR_SUB_F64;
    else if (strcmp(e->as.binary.op, "*") == 0)
      op = NYIR_MUL_F64;
    else
      op = NYIR_DIV_F64;
  }
  /* Free get() returns the tagged dynamic ABI for ordinary consumers.  A
   * statically scalar list read entering raw arithmetic must be unboxed once;
   * doing it here preserves both `print(get(...))` and integer accumulation. */
  bool right_int_literal = e->as.binary.right &&
                           e->as.binary.right->kind == NY_E_LITERAL &&
                           e->as.binary.right->as.literal.kind == NY_LIT_INT &&
                           !ny_expr_is_nil_literal(e->as.binary.right);
  bool left_int_literal = e->as.binary.left &&
                          e->as.binary.left->kind == NY_E_LITERAL &&
                          e->as.binary.left->as.literal.kind == NY_LIT_INT &&
                          !ny_expr_is_nil_literal(e->as.binary.left);
  /*
   * The callee return fact outranks the stale site stamp: a call through a
   * lambda/alias whose target returns the canonical tagged value carries a
   * stale RAW_INT rep from HM specialization, and trusting it here compares
   * tagged 15 (31) against the raw literal 15.
   */
  bool left_raw_i64 =
      left_int_literal ||
      nyr_expr_is_raw_integer_conversion(b, e->as.binary.left) ||
      (e->as.binary.left &&
       e->as.binary.left->semantic.rep == NY_SEM_REP_RAW_INT &&
       !nyr_expr_is_any(b, e->as.binary.left));
  bool right_raw_i64 =
      right_int_literal ||
      nyr_expr_is_raw_integer_conversion(b, e->as.binary.right) ||
      (e->as.binary.right &&
       e->as.binary.right->semantic.rep == NY_SEM_REP_RAW_INT &&
       !nyr_expr_is_any(b, e->as.binary.right));
  /* Arithmetic lowering records the physical result ABI for the last
   * lowered operand.  Prefer that evidence for dynamic modulo, whose helper
   * result is deliberately decoded before it reaches this comparison. */
  if (b->last_arith_raw && b->last_arith_expr == e->as.binary.left) {
    left_any = false;
    left_dynamic_result = false;
    left_raw_i64 = true;
  }
  if (b->last_arith_raw && b->last_arith_expr == e->as.binary.right) {
    right_any = false;
    right_dynamic_result = false;
    right_raw_i64 = true;
  }
  /* Dynamic modulo goes through rt_any_mod and is then unboxed by its own
   * lowering because the remainder feeds scalar branches/comparisons.  Mark
   * that produced side raw here; otherwise a comparison such as
   * `(v % 2) == 0` sends the untagged remainder to rt_any_eq. */
  const expr_t *left_expr = e->as.binary.left;
  bool left_dynamic_mod =
      left_expr && left_expr->kind == NY_E_BINARY && left_expr->as.binary.op &&
      strcmp(left_expr->as.binary.op, "%") == 0 &&
      (nyr_expr_is_any(b, left_expr->as.binary.left) ||
       nyr_expr_is_any(b, left_expr->as.binary.right)) &&
      !nyr_expr_is_f64(b, left_expr) && !nyr_expr_is_f32(b, left_expr);
  const expr_t *right_expr = e->as.binary.right;
  bool right_dynamic_mod =
      right_expr && right_expr->kind == NY_E_BINARY &&
      right_expr->as.binary.op && strcmp(right_expr->as.binary.op, "%") == 0 &&
      (nyr_expr_is_any(b, right_expr->as.binary.left) ||
       nyr_expr_is_any(b, right_expr->as.binary.right)) &&
      !nyr_expr_is_f64(b, right_expr) && !nyr_expr_is_f32(b, right_expr);
  if (left_dynamic_mod) {
    left_any = false;
    left_dynamic_result = false;
    left_raw_i64 = true;
  }
  if (right_dynamic_mod) {
    right_any = false;
    right_dynamic_result = false;
    right_raw_i64 = true;
  }
  /* A whole-program proven elem-8 int list read is a raw i64 producer even
   * though the checker left the node annotated dynamic while the list was
   * an untyped `[]` (the raw stamp is only published when the operand is
   * lowered, after these flags are computed).  Without this the plain
   * `acc + xs[i]` shape falls through to rt_any_add, which untags odd raw
   * payloads and halves them. */
  if (e->as.binary.left &&
      nyr_index_of_promoted_top_mut(b, e->as.binary.left)) {
    left_any = false;
    left_dynamic_result = false;
    left_raw_i64 = true;
  }
  if (e->as.binary.right &&
      nyr_index_of_promoted_top_mut(b, e->as.binary.right)) {
    right_any = false;
    right_dynamic_result = false;
    right_raw_i64 = true;
  }
  /* Local ABI metadata is more authoritative than the conservative semantic
   * node for aliases of decoded sequence elements (`b = k`, where `k` came
   * from `perm[0]`). Treat those loads as raw scalars before literal tagging.
   */
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_IDENT &&
      e->as.binary.left->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.binary.left->as.ident.name);
    if (local && !left_any && local->semantic_rep == NY_SEM_REP_RAW_INT) {
      left_dynamic_result = false;
      left_raw_i64 = true;
    }
  }
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_IDENT &&
      e->as.binary.right->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.binary.right->as.ident.name);
    if (local && !right_any && local->semantic_rep == NY_SEM_REP_RAW_INT) {
      right_dynamic_result = false;
      right_raw_i64 = true;
    }
  }
  /* A top-level global whose declaration initializer is an integer literal
   * owns a raw int cell for its lifetime (global slots freeze their
   * representation at declaration, and the store boundary decodes dynamic
   * reads into it).  Treat it as a proven raw operand like the local case
   * above; without this, `vis[cur] == pass_id` feeds its decoded raw left
   * to rt_any_eq, whose bridge reads the odd raw word 5 as tagged 2 and
   * reports equality with the stored 2. */
  if (!right_raw_i64 && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_IDENT &&
      e->as.binary.right->as.ident.name &&
      !nyr_find_local_nearest(b, e->as.binary.right->as.ident.name)) {
    const expr_t *ginit =
        nyr_find_top_level_value(b, e->as.binary.right->as.ident.name);
    if (ginit && ginit->kind == NY_E_LITERAL &&
        ginit->as.literal.kind == NY_LIT_INT && ginit->tok.kind != NY_T_NIL) {
      right_raw_i64 = true;
      right_any = false;
      right_dynamic_result = false;
    }
  }
  if (!left_raw_i64 && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_IDENT &&
      e->as.binary.left->as.ident.name &&
      !nyr_find_local_nearest(b, e->as.binary.left->as.ident.name)) {
    const expr_t *ginit =
        nyr_find_top_level_value(b, e->as.binary.left->as.ident.name);
    if (ginit && ginit->kind == NY_E_LITERAL &&
        ginit->as.literal.kind == NY_LIT_INT && ginit->tok.kind != NY_T_NIL) {
      left_raw_i64 = true;
      left_any = false;
      left_dynamic_result = false;
    }
  }
  /* Collection/string length is a raw machine count regardless of the
   * element/value representation of the receiver.  Preserve that fact when
   * it participates in arithmetic with a dynamic dictionary read; otherwise
   * the comparison path cannot see the raw peer and leaves the dictionary's
   * tagged integer encoded. */
  if (!left_raw_i64 && e->as.binary.left) {
    const expr_t *left = e->as.binary.left;
    left_raw_i64 = (left->kind == NY_E_MEMBER && left->as.member.name &&
                    strcmp(left->as.member.name, "len") == 0) ||
                   (left->kind == NY_E_MEMCALL && left->as.memcall.name &&
                    strcmp(left->as.memcall.name, "len") == 0 &&
                    left->as.memcall.args.len == 0);
  }
  if (!right_raw_i64 && e->as.binary.right) {
    const expr_t *right = e->as.binary.right;
    right_raw_i64 = (right->kind == NY_E_MEMBER && right->as.member.name &&
                     strcmp(right->as.member.name, "len") == 0) ||
                    (right->kind == NY_E_MEMCALL && right->as.memcall.name &&
                     strcmp(right->as.memcall.name, "len") == 0 &&
                     right->as.memcall.args.len == 0);
  }

  /* Keep proven scalar list reads and integer literals in one raw domain.
   * The unified NYIR path otherwise sees the conservative `any` annotation
   * on the surrounding operator and emits rt_any_eq for a raw list payload. */
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE) && !left_bigint &&
      !right_bigint &&
      ((nyr_index_read_is_proven_scalar(b, e->as.binary.left) &&
        (right_int_literal ||
         nyr_ident_local_is_raw_int(b, e->as.binary.right))) ||
       (nyr_index_read_is_proven_scalar(b, e->as.binary.right) &&
        (left_int_literal ||
         nyr_ident_local_is_raw_int(b, e->as.binary.left))))) {
    return nyr_ir_emit(
        &b->nyir,
        (nyr_inst_t){
            .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  }
  if (!left_raw_i64 && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_IDENT &&
      e->as.binary.left->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.binary.left->as.ident.name);
    left_raw_i64 = local && !local->is_f64 && !local->is_f32 &&
                   !local->is_list && !local->is_dict && !local->is_cstr &&
                   !left_any &&
                   (local->semantic_rep == NY_SEM_REP_RAW_INT ||
                    local->semantic_rep != NY_SEM_REP_TAGGED_DYNAMIC);
  }
  if (!right_raw_i64 && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_IDENT &&
      e->as.binary.right->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(b, e->as.binary.right->as.ident.name);
    right_raw_i64 = local && !local->is_f64 && !local->is_f32 &&
                    !local->is_list && !local->is_dict && !local->is_cstr &&
                    !right_any &&
                    (local->semantic_rep == NY_SEM_REP_RAW_INT ||
                     local->semantic_rep != NY_SEM_REP_TAGGED_DYNAMIC);
  }
  /* An untyped/any sequence index is returned as a tagged dynamic word by
   * the managed-sequence accessor.  When the surrounding arithmetic is a
   * scalar result, normalize those operands before the raw NYIR operation;
   * otherwise `1 + 3` is compiled as the tagged words `3 + 7`.  Preserve the
   * dynamic ABI when the expression itself is returned as any. */
  bool left_dynamic_index =
      e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
      e->as.binary.left->as.index.target &&
      ((e->as.binary.left->as.index.target->kind == NY_E_IDENT &&
        (nyr_local_is_dynamic_sequence(nyr_find_local_nearest(
             b, e->as.binary.left->as.index.target->as.ident.name)) ||
         nyr_ident_is_global_dynamic_sequence(
             b, e->as.binary.left->as.index.target->as.ident.name))) ||
       nyr_expr_is_dynamic_sequence_value(b,
                                          e->as.binary.left->as.index.target));
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
      e->as.binary.left->as.index.target &&
      nyr_expr_is_list(b, e->as.binary.left->as.index.target) &&
      !nyr_expr_is_typed_scalar_index(b, e->as.binary.left) &&
      !nyr_expr_is_bytes(b, e->as.binary.left->as.index.target) &&
      !nyr_expr_is_range(b, e->as.binary.left->as.index.target))
    left_dynamic_index = true;
  if (e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
      e->as.binary.left->as.index.target &&
      e->as.binary.left->as.index.target->kind == NY_E_IDENT &&
      e->as.binary.left->as.index.target->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_nearest(
        b, e->as.binary.left->as.index.target->as.ident.name);
    if (local && local->is_list &&
        !nyr_expr_is_typed_scalar_index(b, e->as.binary.left))
      left_dynamic_index = true;
  }
  bool right_dynamic_index =
      e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
      e->as.binary.right->as.index.target &&
      ((e->as.binary.right->as.index.target->kind == NY_E_IDENT &&
        (nyr_local_is_dynamic_sequence(nyr_find_local_nearest(
             b, e->as.binary.right->as.index.target->as.ident.name)) ||
         nyr_ident_is_global_dynamic_sequence(
             b, e->as.binary.right->as.index.target->as.ident.name))) ||
       nyr_expr_is_dynamic_sequence_value(b,
                                          e->as.binary.right->as.index.target));
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
      e->as.binary.right->as.index.target &&
      nyr_expr_is_list(b, e->as.binary.right->as.index.target) &&
      !nyr_expr_is_typed_scalar_index(b, e->as.binary.right) &&
      !nyr_expr_is_bytes(b, e->as.binary.right->as.index.target) &&
      !nyr_expr_is_range(b, e->as.binary.right->as.index.target))
    right_dynamic_index = true;
  if (e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
      e->as.binary.right->as.index.target &&
      e->as.binary.right->as.index.target->kind == NY_E_IDENT &&
      e->as.binary.right->as.index.target->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_nearest(
        b, e->as.binary.right->as.index.target->as.ident.name);
    if (local && local->is_list &&
        !nyr_expr_is_typed_scalar_index(b, e->as.binary.right))
      right_dynamic_index = true;
  }
  /* The container's conservative `list`/`any` type sets the dynamic-index
   * flag above before the whole-program append-shape proof is applied. The
   * proven top-level elem-8 receiver is physically raw, so clear that flag
   * here, after all dynamic-index inference has run; otherwise the later
   * arithmetic bridge calls rt_any_to_i64 on odd raw payloads. */
  if (nyr_index_of_promoted_top_mut(b, e->as.binary.left)) {
    left_any = false;
    left_dynamic_result = false;
    left_dynamic_index = false;
    left_raw_i64 = true;
  }
  if (nyr_index_of_promoted_top_mut(b, e->as.binary.right)) {
    right_any = false;
    right_dynamic_result = false;
    right_dynamic_index = false;
    right_raw_i64 = true;
  }
  /*
   * The lowered accessor's ABI outranks a stale scalar-looking source
   * annotation. Some inferred locals have an `int` element type while their
   * actual container is still descriptor-backed; their read is tagged
   * (`rt_tbuf_index_any_raw`), not the raw elem-8 result promised by a typed
   * list. When compared with a proven raw integer, normalize that physical
   * tagged word before emitting the scalar comparison.
   */
  if (is_cmp && e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
      !nyr_index_read_is_proven_scalar(b, e->as.binary.left) &&
      right_int_literal)
    left_dynamic_index = true;
  if (is_cmp && e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
      !nyr_index_read_is_proven_scalar(b, e->as.binary.right) &&
      left_int_literal)
    right_dynamic_index = true;
  bool preserve_dynamic_result = b->return_any || nyr_expr_is_any(b, e);
  /* A raw scalar peer proves that this arithmetic expression is still in the
   * machine-integer domain.  The surrounding expression may carry a
   * conservative `any` annotation because the other operand came from an
   * untyped sequence, but tagging the result here would make the next `+`
   * or bitwise operation decode it a second time.  Container stores and
   * genuine any-returning functions perform boxing at their own boundary. */
  /* Ordering comparisons over an untyped sequence still use the scalar
   * element domain.  The accessor returns a canonical tagged word, but the
   * generic compare path intentionally stays raw for `>`/`<`; decode the
   * indexed operands before that path.  Without this, `a[j] > key` compares
   * (2*value+1) against value and insertion/heapsort silently leave the
   * array partially ordered. */
  if (is_cmp && !expr_f64 && !expr_f32 && !left_f64 && !right_f64 &&
      !left_f32 && !right_f32) {
    if (left_dynamic_index &&
        !nyr_index_read_is_proven_scalar(b, e->as.binary.left)) {
      a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
      if (a < 0)
        return -1;
      left_any = false;
      left_dynamic_result = false;
      left_raw_i64 = true;
    }
    if (right_dynamic_index &&
        !nyr_index_read_is_proven_scalar(b, e->as.binary.right)) {
      rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
      if (rhs < 0)
        return -1;
      right_any = false;
      right_dynamic_result = false;
      right_raw_i64 = true;
    }
    /* Both operands are decoded raw i64 words now.  Emit a plain icmp:
     * rt_any_eq's tagged/raw bridge reads a raw odd right word (5) as
     * tagged 2 and reports equality with the decoded left (2), which turned
     * every early walk exit in havlak's top-level form into a false cycle. */
    if ((left_dynamic_index || right_dynamic_index) &&
        (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE) && left_raw_i64 &&
        right_raw_i64 && !left_any && !right_any)
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){
              .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  }
  /* Indexed reads from an untyped sequence are tagged at the accessor
   * boundary, but arithmetic still operates on raw integers.  Normalize them
   * even when the expression later returns to a dynamic container; the final
   * result is boxed once below.  Leaving this gated by preserve_dynamic_result
   * compiled `count[r] - 1` as raw(count[r]) - tagged(1), which made the
   * permutation benchmark never advance and eventually time out. */
  if (!is_cmp && (left_dynamic_index || right_dynamic_index) && !expr_f64 &&
      !expr_f32) {
    if (left_dynamic_index &&
        !nyr_index_read_is_proven_scalar(b, e->as.binary.left)) {
      a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
      if (a < 0)
        return -1;
      left_any = false;
      left_dynamic_result = false;
      left_raw_i64 = true;
    }
    if (right_dynamic_index &&
        !nyr_index_read_is_proven_scalar(b, e->as.binary.right)) {
      rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
      if (rhs < 0)
        return -1;
      right_any = false;
      right_dynamic_result = false;
      right_raw_i64 = true;
    }
  }
  /* Shifts sit after the sequence-read decode above: an indexed operand is
   * decoded to its raw payload first, so proven-raw pairs take the plain
   * NYIR shift and the dispatcher boxes the result at the dynamic boundary.
   * Routing them through rt_shr before that decode left the tagged result
   * word (2*(v>>24)+1) flowing into raw `&` masks, which cost sha256 the
   * low bit of every extracted digest byte. */
  if (!is_cmp && shift_value_op &&
      (left_bigint || right_bigint || left_any || right_any ||
       left_dynamic_result || right_dynamic_result)) {
    if (!left_bigint && !left_any && !left_dynamic_result) {
      a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1, 0);
      if (a < 0)
        return -1;
    }
    if (!right_bigint && !right_any && !right_dynamic_result) {
      rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1, 0);
      if (rhs < 0)
        return -1;
    }
    const char *runtime_op =
        strcmp(e->as.binary.op, "<<") == 0 ? "rt_shl" : "rt_shr";
    return nyr_emit_runtime_call(b, runtime_op, a, rhs, -1, 2, 0);
  }
  bool left_is_raw_list_get =
      e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
      e->as.binary.left->as.memcall.name &&
      strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
      e->as.binary.left->as.memcall.target &&
      nyr_expr_is_list(b, e->as.binary.left->as.memcall.target) &&
      nyr_value_scalar_class(b, a) != NYIR_SCALAR_I64_TAGGED;
  bool right_is_raw_list_get =
      e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
      e->as.binary.right->as.memcall.name &&
      strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
      e->as.binary.right->as.memcall.target &&
      nyr_expr_is_list(b, e->as.binary.right->as.memcall.target) &&
      nyr_value_scalar_class(b, rhs) != NYIR_SCALAR_I64_TAGGED;
  bool left_scalar_get_decoded = false;
  bool right_scalar_get_decoded = false;
  if ((!is_cmp || right_int_literal) &&
      (left_scalar_get || left_scalar_get_mem) && !left_is_raw_list_get &&
      !(left_dynamic_index && !expr_f64 && !expr_f32)) {
    a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    left_any = false;
    left_dynamic_result = false;
    left_raw_i64 = true;
    left_scalar_get_decoded = true;
  }
  if ((!is_cmp || left_int_literal) &&
      (right_scalar_get || right_scalar_get_mem) && !right_is_raw_list_get &&
      !(right_dynamic_index && !expr_f64 && !expr_f32)) {
    rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    right_any = false;
    right_dynamic_result = false;
    right_raw_i64 = true;
    right_scalar_get_decoded = true;
  }
  /* Dictionary subscript/get/pop reads keep the canonical tagged slot word
   * (the same ABI as .get).  When the other operand is a proven raw
   * integer, decode the dynamic side exactly once so the integer operation
   * stays in the raw domain; the rt_any_* helpers would otherwise read the
   * raw operand as a tagged integer and both words would be untagged. */
  bool left_dict_read = nyr_expr_is_dict_read(b, e->as.binary.left);
  bool right_dict_read = nyr_expr_is_dict_read(b, e->as.binary.right);
  if (left_dict_read && right_dict_read && !left_f64 && !right_f64 &&
      !left_f32 && !right_f32 && !left_cstr && !right_cstr && !left_bigint &&
      !right_bigint &&
      /*
       * The AST-level f64 flags can miss a float producer stamped by the
       * lowering itself (a layout member load re-boxed through
       * rt_any_to_f64).  The register class is authoritative: decoding an
       * f64-classified value with rt_any_to_i64 trips the call scalar-class
       * verifier and bit-mangles the comparison.
       */
      !nyr_value_is_f64(b, a, 0) && !nyr_value_is_f64(b, rhs, 0)) {
    bool dynamic_result = b->return_any || nyr_expr_is_any(b, e);
    /* Both sides are dynamic dictionary reads: decode both exactly once so
     * the raw integer op below sees true payloads. */
    /* An indexed dictionary read may already have crossed its tagged-to-raw
     * boundary in the sequence-read normalization above.  Preserve that
     * fact here: decoding it again treats an odd raw integer as a tagged
     * immediate (for example, 21 becomes 10) before the dynamic add. */
    if (!left_raw_i64)
      a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    if (!right_raw_i64)
      rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    if (a < 0 || rhs < 0)
      return -1;
    left_any = false;
    right_any = false;
    left_dynamic_result = false;
    right_dynamic_result = false;
    /* The arithmetic result still has the source expression's dynamic
     * contract.  Re-enter that ABI before the operator dispatcher sees it;
     * otherwise an outer equality receives raw 5 and compares it as a tagged
     * value. */
    if (dynamic_result) {
      a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1, 0);
      rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1, 0);
      if (a < 0 || rhs < 0)
        return -1;
      left_any = true;
      right_any = true;
      left_dynamic_result = true;
      right_dynamic_result = true;
    }
    /* The operands are raw after this normalization.  Clear the source
     * classification as well so the later one-sided dictionary bridge does
     * not decode the right operand a second time (63 -> 31 -> 15). */
    left_raw_i64 = false;
    right_raw_i64 = false;
  } else if (left_dict_read && !left_scalar_get_decoded && right_raw_i64 &&
             !right_any && !right_dynamic_result && !right_cstr && !left_f64 &&
             !right_f64 && !left_f32 && !right_f32 && !left_bigint &&
             !right_bigint && !nyr_expr_is_bool(b, e->as.binary.right) &&
             !nyr_index_read_is_proven_scalar(b, e->as.binary.left)) {
    a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
    left_any = false;
    left_dynamic_result = false;
    /* Both operands are proven raw i64 words at this point: the read was
     * decoded exactly once and the right side is a proven raw integer.
     * Equality must be a plain icmp — the rt_any_eq bridge reads odd raw
     * words as tagged (raw 5 == tagged 2) and reports false cycles in
     * `vis[cur] == pass_id` where vis holds 2 and pass_id is 5. */
    if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE))
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){
              .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  }
  if (right_dict_read && !right_scalar_get_decoded && left_raw_i64 &&
      !left_any && !left_dynamic_result && !left_cstr && !left_f64 &&
      !right_f64 && !left_f32 && !right_f32 && !left_bigint && !right_bigint &&
      !nyr_expr_is_bool(b, e->as.binary.left) &&
      !nyr_index_read_is_proven_scalar(b, e->as.binary.right)) {
    rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    if (rhs < 0)
      return -1;
    right_any = false;
    right_dynamic_result = false;
    if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE))
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){
              .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  }
  if (a < 0 || rhs < 0)
    return -1;
  /* Nullable sequence values use a raw null-handle representation in NYIR.
   * Keep a nil comparison in that representation even when the type pass
   * conservatively marks the expression dynamic; routing `nil == list` into
   * the generic value comparator loses the separate list-length slot and can
   * treat a static sequence payload as nil. */
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE) &&
      (ny_expr_is_nil_literal(e->as.binary.left) ||
       ny_expr_is_nil_literal(e->as.binary.right))) {
    const expr_t *other = ny_expr_is_nil_literal(e->as.binary.left)
                              ? e->as.binary.right
                              : e->as.binary.left;
    if (other &&
        (nyr_expr_is_list(b, other) || nyr_expr_is_dyn_list(b, other) ||
         nyr_expr_is_tuple(b, other) || nyr_expr_is_range(b, other)))
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){
              .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  }
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
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", a, -1, -1, 1, 0);
      a = bits < 0
              ? -1
              : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1, 1, 0);
    }
    if (right_f64) {
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", rhs, -1, -1, 1, 0);
      rhs = bits < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                             1, 0);
    }
    if (a < 0 || rhs < 0)
      return -1;
    int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
    int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
    return (equal < 0 || true_imm < 0)
               ? -1
               : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
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
       !(ny_native_call_leaf(e->as.binary.left) &&
         strcmp(ny_native_call_leaf(e->as.binary.left), "get") == 0) &&
       e->as.binary.left->as.call.callee->as.ident.name &&
       !nyr_user_defined_fn(b,
                            e->as.binary.left->as.call.callee->as.ident.name) &&
       !nyr_find_imported_function(
           b, e->as.binary.left->as.call.callee->as.ident.name) &&
       !nyr_find_local(b, e->as.binary.left->as.call.callee->as.ident.name) &&
       !nyr_find_top_level_value(
           b, e->as.binary.left->as.call.callee->as.ident.name) &&
       e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
       (e->as.binary.right->as.literal.kind == NY_LIT_INT ||
        ny_expr_is_nil_literal(e->as.binary.right))) ||
      (e->as.binary.right && e->as.binary.right->kind == NY_E_CALL &&
       e->as.binary.right->as.call.callee &&
       e->as.binary.right->as.call.callee->kind == NY_E_IDENT &&
       !(ny_native_call_leaf(e->as.binary.right) &&
         strcmp(ny_native_call_leaf(e->as.binary.right), "get") == 0) &&
       e->as.binary.right->as.call.callee->as.ident.name &&
       !nyr_user_defined_fn(
           b, e->as.binary.right->as.call.callee->as.ident.name) &&
       !nyr_find_imported_function(
           b, e->as.binary.right->as.call.callee->as.ident.name) &&
       !nyr_find_local(b, e->as.binary.right->as.call.callee->as.ident.name) &&
       !nyr_find_top_level_value(
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
  if (tag_operand && !bitwise_op && !left_any && right_any && !left_f64 &&
      !left_f32 && !left_cstr && e->as.binary.left->kind == NY_E_IDENT &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.right) &&
      e->as.binary.left->semantic.rep == NY_SEM_REP_RAW_INT &&
      !(e->as.binary.op && strcmp(e->as.binary.op, "+") == 0)) {
    a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (tag_operand && !bitwise_op && left_any && !right_any && !right_f64 &&
      !right_f32 && !right_cstr && e->as.binary.right->kind == NY_E_IDENT &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) &&
      e->as.binary.right->semantic.rep == NY_SEM_REP_RAW_INT &&
      !(e->as.binary.op && strcmp(e->as.binary.op, "+") == 0)) {
    rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1, 0);
    if (rhs < 0)
      return -1;
  }
  /* Dictionary .get reads keep the canonical tagged word, so pairing them
     with bool/int literals must normalize the literal side exactly as for
     any other dynamic operand; the old decode-based exclusion is gone. */
  if (tag_operand && !bitwise_op && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_LITERAL &&
      e->as.binary.left->as.literal.kind == NY_LIT_INT && !left_any &&
      (right_any || right_dynamic_result) &&
      !nyr_index_read_is_proven_scalar(b, e->as.binary.right) &&
      !ny_expr_is_nil_literal(e->as.binary.left)) {
    a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (tag_operand && !bitwise_op && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.right->as.literal.kind == NY_LIT_INT && !right_any &&
      (left_any || left_dynamic_result) &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) &&
      !nyr_index_read_is_proven_scalar(b, e->as.binary.left) &&
      !(e->as.binary.left && e->as.binary.left->kind == NY_E_BINARY &&
        e->as.binary.left->as.binary.op &&
        strcmp(e->as.binary.left->as.binary.op, "%") == 0) &&
      !ny_expr_is_nil_literal(e->as.binary.right)) {
    rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1, 0);
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
    int mod = nyr_emit_runtime_call(b, "rt_any_mod", a, rhs, -1, 2, 0);
    if (mod < 0)
      return -1;
    b->last_arith_raw = 1;
    b->last_arith_expr = e;
    return nyr_emit_runtime_call(b, "rt_any_to_i64", mod, -1, -1, 1, 0);
  }

  /* Bitwise NYIR operations consume raw machine integers.  A list index
   * through an untyped/dynamic sequence is a tagged value, so decode that
   * value before applying `&`, `|`, shifts, or xor.  This is the generic
   * boundary that keeps byte-oriented library code correct without teaching
   * the compiler about any particular helper such as bytes_to_str. */
  if (bitwise_op && !left_bigint && left_dynamic_result &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.left)) {
    a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (bitwise_op && !right_bigint && right_dynamic_result &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.right)) {
    rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    if (rhs < 0)
      return -1;
  }

  /* Boolean literals normally use raw 0/1 for branch conditions.  When an
   * operator consumes an `any` value, compare against the canonical Ny
   * immediates instead; otherwise a stored `true` (8) is compared with raw
   * one and dynamic dictionary/list values appear unequal. */
  if (tag_operand && e->as.binary.left &&
      e->as.binary.left->kind == NY_E_LITERAL &&
      e->as.binary.left->as.literal.kind == NY_LIT_BOOL &&
      (right_any || right_dynamic_result)) {
    a = nyr_emit_const(b, e->as.binary.left->as.literal.as.b ? NY_IMM_TRUE
                                                             : NY_IMM_FALSE);
    if (a < 0)
      return -1;
  }
  if (tag_operand && e->as.binary.right &&
      e->as.binary.right->kind == NY_E_LITERAL &&
      e->as.binary.right->as.literal.kind == NY_LIT_BOOL &&
      (left_any || left_dynamic_result)) {
    rhs = nyr_emit_const(b, e->as.binary.right->as.literal.as.b ? NY_IMM_TRUE
                                                                : NY_IMM_FALSE);
    if (rhs < 0)
      return -1;
  }
  /* Bigint equality/order compares values, never heap handles. */
  if (is_cmp && (left_bigint || right_bigint)) {
    /*
     * Register-class coherence: a float producer is not a BigInt handle.
     * A poisoned type fact can route a float-valued operand into this
     * bridge (an any-annotated compare whose body is f64); feeding the
     * f64 register to the handle runtime trips the scalar-class verifier.
     * Resolve the contradiction as a machine-float comparison instead.
     */
    bool cmp_left_float =
        nyr_value_is_f64(b, a, 0) || nyr_value_is_f32(b, a, 0);
    bool cmp_right_float =
        nyr_value_is_f64(b, rhs, 0) || nyr_value_is_f32(b, rhs, 0);
    if (cmp_left_float || cmp_right_float) {
      int lf = a, rf = rhs;
      if (!cmp_left_float)
        lf = nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_TAGGED
                 ? nyr_emit_runtime_call(b, "rt_any_to_f64", a, -1, -1, 1,
                                         NYIR_SCALAR_F64)
                 : nyr_emit_i64_to_f64(b, a);
      else if (nyr_value_is_f32(b, a, 0))
        lf = nyr_emit_f32_to_f64(b, a);
      if (!cmp_right_float)
        rf = nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_TAGGED
                 ? nyr_emit_runtime_call(b, "rt_any_to_f64", rhs, -1, -1, 1,
                                         NYIR_SCALAR_F64)
                 : nyr_emit_i64_to_f64(b, rhs);
      else if (nyr_value_is_f32(b, rhs, 0))
        rf = nyr_emit_f32_to_f64(b, rhs);
      if (lf < 0 || rf < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){
              .op = NYIR_CMP_F64, .dst = -1, .a = lf, .b = rf, .cmp = cmp});
    }
    if (!left_bigint && !left_any)
      a = nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", a, -1, -1, 1, 0);
    if (!right_bigint && !right_any)
      rhs =
          nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", rhs, -1, -1, 1, 0);
    if (a < 0 || rhs < 0)
      return -1;
    int compared =
        nyr_emit_runtime_call(b, "rt_bigint_cmp_raw", a, rhs, -1, 2, 0);
    int zero = compared < 0 ? -1 : nyr_emit_const(b, 0);
    return compared < 0 || zero < 0
               ? -1
               : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                   .dst = -1,
                                                   .a = compared,
                                                   .b = zero,
                                                   .cmp = cmp});
  }
  /* An immutable top-level `def alias = nil` retains nil identity even
   * though native scalar storage uses raw zero for both nil and int zero.
   * Recover that semantic fact before the raw comparison; mutable values are
   * deliberately excluded because their contents can change after binding. */
  bool left_nil = nyr_expr_is_immutable_nil(b, e->as.binary.left, 0);
  bool right_nil = nyr_expr_is_immutable_nil(b, e->as.binary.right, 0);
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE)) {
    if (left_nil && right_nil)
      return nyr_emit_const(b, cmp == NYIR_CMP_EQ ? 1 : 0);
    if ((left_nil && e->as.binary.right &&
         e->as.binary.right->kind == NY_E_LITERAL && !right_nil) ||
        (right_nil && e->as.binary.left &&
         e->as.binary.left->kind == NY_E_LITERAL && !left_nil)) {
      return nyr_emit_const(b, cmp == NYIR_CMP_NE ? 1 : 0);
    }
    /* A subscript read on a list yields an element whose slot payload may be
       a nested container handle; equality must route through the structural
       comparator exactly as for whole-list operands (zip2 pair rows). */
    bool left_is_list =
        nyr_expr_is_list(b, e->as.binary.left) ||
        nyr_expr_is_dyn_list(b, e->as.binary.left) ||
        nyr_expr_is_tuple(b, e->as.binary.left) ||
        (e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
         e->as.binary.left->as.index.target &&
         (nyr_expr_is_list(b, e->as.binary.left->as.index.target) ||
          nyr_expr_is_dyn_list(b, e->as.binary.left->as.index.target)));
    bool right_is_list =
        nyr_expr_is_list(b, e->as.binary.right) ||
        nyr_expr_is_dyn_list(b, e->as.binary.right) ||
        nyr_expr_is_tuple(b, e->as.binary.right) ||
        (e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
         e->as.binary.right->as.index.target &&
         (nyr_expr_is_list(b, e->as.binary.right->as.index.target) ||
          nyr_expr_is_dyn_list(b, e->as.binary.right->as.index.target)));
    bool left_seq = left_is_list || nyr_expr_is_range(b, e->as.binary.left);
    bool right_seq = right_is_list || nyr_expr_is_range(b, e->as.binary.right);
    if (!left_any && !right_any && !left_dynamic_result &&
        !right_dynamic_result && (left_seq || right_seq)) {
      /* Ranges are heap sequence objects; the tbuf comparator cannot inspect
       * them directly.  The dynamic comparator materializes the range and
       * preserves interpreter sequence equality semantics. */
      if (nyr_expr_is_range(b, e->as.binary.left) ||
          nyr_expr_is_range(b, e->as.binary.right)) {
        int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
        int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
        if (equal < 0 || true_imm < 0)
          return -1;
        int compared = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                         .dst = -1,
                                                         .a = equal,
                                                         .b = true_imm,
                                                         .cmp = cmp});
        return nyr_box_bool(b, compared);
      }
      /* Sequence operands may be raw native buffers, legacy managed lists, or
       * a mixture after an `any` receiver read.  The raw comparator only
       * understands the first representation; use the canonical structural
       * comparator at this semantic boundary. */
      int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
      if (equal < 0 || true_imm < 0)
        return -1;
      int compared = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                       .dst = -1,
                                                       .a = equal,
                                                       .b = true_imm,
                                                       .cmp = cmp});
      return nyr_box_bool(b, compared);
    }
    /*
     * Dynamic values use the tagged ABI while integer literals are emitted
     * as raw i64.  HM intentionally leaves `any == int` dynamic, so the
     * representation boundary must be handled here even when conservative
     * expression provenance was cleared above.  Comparing the tagged word
     * directly with a raw odd integer (e.g. tagged 3 == raw 3) is false.
     */
    bool left_eq_int_literal =
        e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
        e->as.binary.left->as.literal.kind == NY_LIT_INT &&
        !ny_expr_is_nil_literal(e->as.binary.left);
    bool right_eq_int_literal =
        e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
        e->as.binary.right->as.literal.kind == NY_LIT_INT &&
        !ny_expr_is_nil_literal(e->as.binary.right);
    bool left_tagged = nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_TAGGED;
    bool right_tagged =
        nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_TAGGED;
    if ((left_tagged && right_eq_int_literal && !right_tagged) ||
        (right_tagged && left_eq_int_literal && !left_tagged)) {
      if (left_eq_int_literal) {
        a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
        if (a < 0)
          return -1;
      }
      if (right_eq_int_literal) {
        rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
        if (rhs < 0)
          return -1;
      }
      int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
      if (equal < 0 || true_imm < 0)
        return -1;
      int compared = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                       .dst = -1,
                                                       .a = equal,
                                                       .b = true_imm,
                                                       .cmp = cmp});
      return nyr_box_bool(b, compared);
    }
    bool left_call_scalar = left_user_call && !left_any && !left_dynamic_result;
    bool right_call_scalar =
        right_user_call && !right_any && !right_dynamic_result;
    /* Dictionary indexing always crosses the tagged-value boundary, even
     * when type inference records the scalar payload as RAW_INT.  Compare
     * its canonical value through the dynamic equality helper rather than
     * comparing the tagged word with an unboxed literal. */
    bool left_dict_index =
        e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
        e->as.binary.left->as.index.target &&
        nyr_expr_is_dict(b, e->as.binary.left->as.index.target);
    bool right_dict_index =
        e->as.binary.right && e->as.binary.right->kind == NY_E_INDEX &&
        e->as.binary.right->as.index.target &&
        nyr_expr_is_dict(b, e->as.binary.right->as.index.target);
    bool left_dict_get =
        e->as.binary.left && e->as.binary.left->kind == NY_E_MEMCALL &&
        e->as.binary.left->as.memcall.name &&
        strcmp(e->as.binary.left->as.memcall.name, "get") == 0 &&
        e->as.binary.left->as.memcall.target &&
        nyr_expr_is_dict(b, e->as.binary.left->as.memcall.target);
    bool right_dict_get =
        e->as.binary.right && e->as.binary.right->kind == NY_E_MEMCALL &&
        e->as.binary.right->as.memcall.name &&
        strcmp(e->as.binary.right->as.memcall.name, "get") == 0 &&
        e->as.binary.right->as.memcall.target &&
        nyr_expr_is_dict(b, e->as.binary.right->as.memcall.target);
    /* A scalar dictionary read is a tagged value at the runtime boundary.
     * When the other operand is a proven raw integer, the shared
     * dict-read normalization above already decoded the read exactly once
     * and cleared the dynamic flags, so the raw compare below runs.  When
     * the operand is still tagged (dynamic or non-scalar other side),
     * compare canonical values through the dynamic equality helper rather
     * than comparing the tagged word with an unboxed literal. */
    if ((left_dict_get || left_dict_index) &&
        !(left_any || left_dynamic_result)) {
      /* already decoded by the dict-read normalization */
    } else if ((right_dict_get || right_dict_index) &&
               !(right_any || right_dynamic_result)) {
      /* already decoded by the dict-read normalization */
    } else if (left_dict_index || right_dict_index || left_dict_get ||
               right_dict_get) {
      int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
      return (equal < 0 || true_imm < 0)
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                     .dst = -1,
                                                     .a = equal,
                                                     .b = true_imm,
                                                     .cmp = cmp});
    }
    if ((left_dynamic_result || right_dynamic_result) && !left_call_scalar &&
        !right_call_scalar && !left_f64 && !right_f64 && !left_f32 &&
        !right_f32) {
      /* Native memory intrinsics return a proven raw machine value.  Their
       * source-level `any` annotation must not tag only the literal side of
       * an equality such as `load64(p) == 17`. */
      if (nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
          nyr_expr_is_raw_dynamic_read(b, e->as.binary.right))
        goto skip_dynamic_compare;
      /*
       * rt_any_eq speaks the tagged language-value ABI.  A cstr-classified
       * operand holds a raw C-string pointer in its slot (a str-declared
       * function's return crosses that way); handing it over unconverted
       * makes the helper fall through the string branch to the scalar
       * pointer compare and report false for equal strings.  Re-tag it once
       * here; rt_cstr_to_str is idempotent for managed strings.
       */
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("ABI_EQ", "%s path=rt_any_eq left_cstr=%d right_cstr=%d",
                      b->current_fn_name ? b->current_fn_name : "?", left_cstr,
                      right_cstr);
      if (left_cstr) {
        int conv = nyr_emit_runtime_call(b, "rt_cstr_to_str", a, -1, -1, 1, 0);
        if (conv < 0)
          return -1;
        a = conv;
      }
      if (right_cstr) {
        int conv =
            nyr_emit_runtime_call(b, "rt_cstr_to_str", rhs, -1, -1, 1, 0);
        if (conv < 0)
          return -1;
        rhs = conv;
      }
      /* Unary negatives and computed integer peers are raw producers too,
       * even though the earlier literal-only normalization misses them. The
       * dynamic comparator requires canonical values on both sides. */
      if (nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_RAW) {
        a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
        if (a < 0)
          return -1;
      }
      if (nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_RAW) {
        rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
        if (rhs < 0)
          return -1;
      }
      int equal = nyr_emit_runtime_call(b, "rt_any_eq", a, rhs, -1, 2, 0);
      int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
      return (equal < 0 || true_imm < 0)
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                     .dst = -1,
                                                     .a = equal,
                                                     .b = true_imm,
                                                     .cmp = cmp});
    }
  skip_dynamic_compare:;
  }
  bool numeric_literal_sub = e->as.binary.op &&
                             strcmp(e->as.binary.op, "-") == 0 &&
                             e->as.binary.left && e->as.binary.right &&
                             e->as.binary.left->kind == NY_E_LITERAL &&
                             e->as.binary.right->kind == NY_E_LITERAL &&
                             e->as.binary.left->as.literal.kind == NY_LIT_INT &&
                             e->as.binary.right->as.literal.kind == NY_LIT_INT;
  bool left_bool = nyr_expr_is_bool(b, e->as.binary.left);
  bool right_bool = nyr_expr_is_bool(b, e->as.binary.right);
  /* Boolean function calls cross the user-function ABI as boxed Ny
   * immediates, while comparisons/logical expressions and literals are often
   * still raw 0/1 inside NYIR.  Normalize both operands at this semantic
   * boundary so qualified calls, direct calls, and compound expressions all
   * compare the same representation. */
  if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE) && left_bool &&
      right_bool) {
    a = nyr_box_bool(b, a);
    rhs = nyr_box_bool(b, rhs);
    if (a < 0 || rhs < 0)
      return -1;
    return nyr_ir_emit(
        &b->nyir,
        (nyr_inst_t){
            .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  }
  bool left_list = nyr_expr_is_list(b, e->as.binary.left) ||
                   nyr_expr_is_dyn_list(b, e->as.binary.left);
  bool right_list = nyr_expr_is_list(b, e->as.binary.right) ||
                    nyr_expr_is_dyn_list(b, e->as.binary.right);
  /* The scalar-get decode above proves that operand was lowered as a scalar
   * element.  A conservative dyn-list fact on the same .get result must not
   * also route the operation to the container ABI: with both operands
   * decoded, `drop_y.get(x, 0) + drop_speed.get(x, 0)` reached
   * rt_tbuf_concat_raw with two raw integers and crashed every backend. */
  if (left_scalar_get_decoded)
    left_list = false;
  if (right_scalar_get_decoded)
    right_list = false;
  bool left_dict = nyr_expr_is_dict(b, e->as.binary.left);
  bool right_dict = nyr_expr_is_dict(b, e->as.binary.right);
  /* Source container hints can outlive a scalar assignment in native-only
   * lowering.  A direct vector helper is legal only for aggregate machine
   * carriers; tagged dynamic values stay on checked runtime dispatch. */
  nyr_scalar_class_t left_value_class = nyr_value_scalar_class(b, a);
  nyr_scalar_class_t right_value_class = nyr_value_scalar_class(b, rhs);
  bool left_vector = left_value_class == NYIR_SCALAR_OBJECT ||
                     left_value_class == NYIR_SCALAR_POINTER;
  bool right_vector = right_value_class == NYIR_SCALAR_OBJECT ||
                      right_value_class == NYIR_SCALAR_POINTER;
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "+") == 0) {
    if (left_dict && right_dict && left_vector && right_vector) {
      return nyr_emit_runtime_call(b, "rt_vec_add_raw", a, rhs, -1, 2, 0);
    }
    /* An indexed read from an untyped sequence is normalized to a raw i64
     * above.  Keep the complete integer expression in that same domain when
     * its peer is already raw (a typed local, literal, or bitwise tree).
     * Falling through to rt_any_add is an ABI violation: that helper expects
     * two tagged values, so a raw odd operand is decoded a second time. */
    if ((left_dynamic_index || right_dynamic_index) &&
        (left_raw_i64 || right_raw_i64) && !left_f64 && !right_f64 &&
        !left_f32 && !right_f32) {
      if ((left_any || left_dynamic_result) && !left_dynamic_index &&
          !left_raw_i64 &&
          !nyr_expr_is_raw_dynamic_read(b, e->as.binary.left)) {
        a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
        if (a < 0)
          return -1;
        left_any = false;
        left_dynamic_result = false;
        left_raw_i64 = true;
      }
      if ((right_any || right_dynamic_result) && !right_dynamic_index &&
          !right_raw_i64 &&
          !nyr_expr_is_raw_dynamic_read(b, e->as.binary.right)) {
        rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
        if (rhs < 0)
          return -1;
        right_any = false;
        right_dynamic_result = false;
        right_raw_i64 = true;
      }
      int sum = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = a, .b = rhs});
      if (sum < 0)
        return -1;
      /* The dynamic index is decoded above.  Decide whether this sum is raw
       * now, using the post-decode flags: the earlier semantic flags still
       * describe a tagged index and would tag every loop accumulator update. */
      bool raw_scalar_peer = (left_raw_i64 || right_raw_i64) && !left_any &&
                             !right_any && !left_dynamic_result &&
                             !right_dynamic_result && !expr_f64 && !expr_f32 &&
                             !left_f64 && !right_f64 && !left_f32 && !right_f32;
      if (preserve_dynamic_result && !raw_scalar_peer) {
        b->last_arith_raw = 0;
        return nyr_emit_runtime_call(b, "rt_tag", sum, -1, -1, 1, 0);
      }
      b->last_arith_raw = 1;
      b->last_arith_expr = e;
      return sum;
    }
    bool computed_dynamic_left = left_dynamic_result && e->as.binary.left &&
                                 (e->as.binary.left->kind == NY_E_BINARY ||
                                  e->as.binary.left->kind == NY_E_CALL ||
                                  e->as.binary.left->kind == NY_E_MEMCALL);
    bool computed_dynamic_right = right_dynamic_result && e->as.binary.right &&
                                  (e->as.binary.right->kind == NY_E_BINARY ||
                                   e->as.binary.right->kind == NY_E_CALL ||
                                   e->as.binary.right->kind == NY_E_MEMCALL);
    if (left_raw_i64 && (right_any || right_dynamic_result) &&
        (right_is_raw_list_get ||
         nyr_expr_is_raw_dynamic_read(b, e->as.binary.right)) &&
        (!computed_dynamic_right || !right_raw_i64) && !left_f64 &&
        !right_f64 && !left_f32 && !right_f32) {
      bool decoded_here = false;
      if (!right_is_raw_list_get &&
          !nyr_expr_is_raw_dynamic_read(b, e->as.binary.right)) {
        rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
        if (rhs < 0)
          return -1;
        decoded_here = true;
      }
      if (left_int_literal)
        a = nyr_emit_const(b, e->as.binary.left->as.literal.as.i);
      if (a < 0)
        return -1;
      int sum = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = a, .b = rhs});
      /* Re-tag only when this branch decoded a dynamic operand: the raw sum
       * is then boxed back for the any-classified binary.  When the operand
       * was already a raw producer (raw list get) the sum is raw and must
       * stay raw, or a typed accumulator stores the tagged encoding. */
      if ((b->return_any ||
           (decoded_here && nyr_expr_is_any(b, e) && !left_raw_i64)) &&
          sum >= 0) {
        b->last_arith_raw = 0;
        return nyr_emit_runtime_call(b, "rt_tag", sum, -1, -1, 1, 0);
      }
      b->last_arith_raw = 1;
      b->last_arith_expr = e;
      return sum;
    }
    if (right_raw_i64 && (left_any || left_dynamic_result) &&
        (left_is_raw_list_get ||
         nyr_expr_is_raw_dynamic_read(b, e->as.binary.left)) &&
        (!computed_dynamic_left || !left_raw_i64) && !left_f64 && !right_f64 &&
        !left_f32 && !right_f32) {
      bool decoded_here = false;
      if (!left_is_raw_list_get &&
          !nyr_expr_is_raw_dynamic_read(b, e->as.binary.left)) {
        a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
        if (a < 0)
          return -1;
        decoded_here = true;
      }
      if (right_int_literal)
        rhs = nyr_emit_const(b, e->as.binary.right->as.literal.as.i);
      if (rhs < 0)
        return -1;
      int sum = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = a, .b = rhs});
      if ((b->return_any ||
           (decoded_here && nyr_expr_is_any(b, e) && !right_raw_i64)) &&
          sum >= 0) {
        b->last_arith_raw = 0;
        return nyr_emit_runtime_call(b, "rt_tag", sum, -1, -1, 1, 0);
      }
      b->last_arith_raw = 1;
      b->last_arith_expr = e;
      return sum;
    }
    if ((left_any || right_any || left_dynamic_result ||
         right_dynamic_result) &&
        !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      /*
       * rt_any_add/rt_raw_add speak the tagged language-value ABI.  A
       * cstr-classified operand holds a raw C-string pointer in its slot
       * (for example the rt_cstr_concat result of an earlier `str + str`
       * link in the same expression); handing it over unconverted makes the
       * helper render the pointer bytes as "<ptr 0x...>" inside the built
       * string.  Re-tag it once at this boundary; rt_cstr_to_str is
       * idempotent for values that are already managed strings.
       */
      bool dynamic_add_raw =
          nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) ||
          nyr_expr_is_raw_dynamic_read(b, e->as.binary.right);
      if (ny_trace_enabled("NY_TRACE_ABI"))
        ny_trace_line("ABI_ADD",
                      "%s op=%s path=%s left_class=%d right_class=%d "
                      "left_any=%d right_any=%d left_raw=%d right_raw=%d "
                      "left_top_mut_index=%d right_top_mut_index=%d "
                      "left_cstr=%d right_cstr=%d",
                      b->current_fn_name ? b->current_fn_name : "?",
                      e->as.binary.op ? e->as.binary.op : "?",
                      dynamic_add_raw ? "rt_raw_add" : "rt_any_add",
                      (int)nyr_value_scalar_class(b, a),
                      (int)nyr_value_scalar_class(b, rhs),
                      left_any || left_dynamic_result,
                      right_any || right_dynamic_result, left_raw_i64,
                      right_raw_i64,
                      nyr_index_of_promoted_top_mut(b, e->as.binary.left),
                      nyr_index_of_promoted_top_mut(b, e->as.binary.right),
                      left_cstr, right_cstr);
      if (left_cstr) {
        int conv = nyr_emit_runtime_call(b, "rt_cstr_to_str", a, -1, -1, 1, 0);
        if (conv < 0)
          return -1;
        a = conv;
      }
      if (right_cstr) {
        int conv =
            nyr_emit_runtime_call(b, "rt_cstr_to_str", rhs, -1, -1, 1, 0);
        if (conv < 0)
          return -1;
        rhs = conv;
      }
      /* The dynamic arithmetic helpers consume tagged Ny values. A mixed
       * expression can reach this path with a genuinely raw integer on one
       * side (for example a raw accumulator plus the tagged result of
       * `min`). Source-level `any` flags describe the value contract, but
       * the emitted producer class is authoritative at this boundary. Box
       * only raw scalar integers; opaque pointers, objects, buffers, strings,
       * booleans, and BigInts have their own carriers and must pass intact. */
      if (!left_cstr && !nyr_expr_is_bool(b, e->as.binary.left) &&
          !nyr_expr_is_bigint(b, e->as.binary.left) &&
          nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_RAW) {
        a = nyr_emit_runtime_call(b, "rt_box_i64", a, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
        if (a < 0)
          return -1;
      }
      if (!right_cstr && !nyr_expr_is_bool(b, e->as.binary.right) &&
          !nyr_expr_is_bigint(b, e->as.binary.right) &&
          nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_RAW) {
        rhs = nyr_emit_runtime_call(b, "rt_box_i64", rhs, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
        if (rhs < 0)
          return -1;
      }
      if (dynamic_add_raw)
        return nyr_emit_runtime_call(b, "rt_raw_add", a, rhs, -1, 2, 0);
      return nyr_emit_runtime_call(b, "rt_any_add", a, rhs, -1, 2, 0);
    }
    if (left_list && right_list && !left_cstr && !right_cstr &&
        !left_dict_read && !right_dict_read && !left_f64 && !right_f64 &&
        !left_f32 && !right_f32) {
      return nyr_emit_runtime_call(b, "rt_tbuf_concat_raw", a, rhs, -1, 2, 0);
    }
    if ((left_list || right_list) && !left_dict_read && !right_dict_read &&
        !left_f64 && !right_f64 && !left_f32 && !right_f32) {
      return nyr_emit_runtime_call(b, "rt_raw_add", a, rhs, -1, 2, 0);
    }
  }
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "-") == 0) {
    if (left_dict && right_dict && left_vector && right_vector) {
      return nyr_emit_runtime_call(b, "rt_vec_sub_raw", a, rhs, -1, 2, 0);
    }
  }
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "*") == 0) {
    bool dynamic_mul =
        ((left_any || left_dynamic_result) && !left_f64 && !left_f32) ||
        ((right_any || right_dynamic_result) && !right_f64 && !right_f32);
    if (dynamic_mul && (left_f64 || right_f64 || left_f32 || right_f32)) {
      /* Dynamic multiplication also dispatches vector × scalar, including
       * scalar × vector. Preserve the tagged runtime result and box the
       * statically floating operand before `rt_any_mul` inspects its tag. */
      if (left_f64 || left_f32) {
        int f64 = left_f32 ? nyr_emit_f32_to_f64(b, a) : a;
        /*
         * The register class outranks the AST float flag: a slot stamped
         * f64 by the classifier can still hold a tagged dynamic producer.
         * Normalize by class before taking IEEE bits.
         */
        if (f64 >= 0 && !nyr_value_is_f64(b, f64, 0)) {
          f64 = nyr_value_scalar_class(b, f64) == NYIR_SCALAR_I64_TAGGED
                    ? nyr_emit_runtime_call(b, "rt_any_to_f64", f64, -1, -1, 1,
                                            NYIR_SCALAR_F64)
                : nyr_value_is_f32(b, f64, 0) ? nyr_emit_f32_to_f64(b, f64)
                                              : nyr_emit_i64_to_f64(b, f64);
        }
        int bits = f64 < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                   -1, 1, NYIR_SCALAR_I64_RAW);
        a = bits < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                             1, NYIR_SCALAR_I64_TAGGED);
      } else if (!left_any && !left_dynamic_result) {
        a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
      }
      if (right_f64 || right_f32) {
        int f64 = right_f32 ? nyr_emit_f32_to_f64(b, rhs) : rhs;
        /*
         * The register class outranks the AST float flag: a slot stamped
         * f64 by the classifier can still hold a tagged dynamic producer.
         * Normalize by class before taking IEEE bits.
         */
        if (f64 >= 0 && !nyr_value_is_f64(b, f64, 0)) {
          f64 = nyr_value_scalar_class(b, f64) == NYIR_SCALAR_I64_TAGGED
                    ? nyr_emit_runtime_call(b, "rt_any_to_f64", f64, -1, -1, 1,
                                            NYIR_SCALAR_F64)
                : nyr_value_is_f32(b, f64, 0) ? nyr_emit_f32_to_f64(b, f64)
                                              : nyr_emit_i64_to_f64(b, f64);
        }
        int bits = f64 < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                   -1, 1, NYIR_SCALAR_I64_RAW);
        rhs = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, NYIR_SCALAR_I64_TAGGED);
      } else if (!right_any && !right_dynamic_result) {
        rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
      }
      if (a < 0 || rhs < 0)
        return -1;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-arithmetic",
            "function=%s operator=* selected=dynamic reason=dynamic operand "
            "may be aggregate; float operands boxed",
            b->current_fn_name ? b->current_fn_name : "<entry>");
      b->last_arith_raw = 0;
      b->last_arith_expr = e;
      b->last_arith_tagged = true;
      return nyr_emit_runtime_call(b, "rt_any_mul", a, rhs, -1, 2,
                                   NYIR_SCALAR_I64_TAGGED);
    }
    if (left_dict && right_dict && left_vector && right_vector) {
      return nyr_emit_runtime_call(b, "rt_vec_dot_raw", a, rhs, -1, 2,
                                   NYIR_SCALAR_F64);
    }
    if (left_dict && left_vector && !left_f64 && !left_f32 &&
        (right_f64 || right_f32)) {
      int s_f64 = right_f32 ? nyr_emit_f32_to_f64(b, rhs) : rhs;
      return nyr_emit_runtime_call(b, "rt_vec_mul_scalar_raw", a, s_f64, -1, 2,
                                   0);
    }
    if (right_dict && right_vector && !right_f64 && !right_f32 &&
        (left_f64 || left_f32)) {
      int s_f64 = left_f32 ? nyr_emit_f32_to_f64(b, a) : a;
      return nyr_emit_runtime_call(b, "rt_vec_mul_scalar_raw", rhs, s_f64, -1,
                                   2, 0);
    }
    if ((left_any || right_any) && !left_f64 && !right_f64 && !left_f32 &&
        !right_f32) {
      /* Dynamic multiplication consumes tagged scalars on both sides,
       * including a raw loop counter beside a dictionary-derived value. */
      if (nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_RAW)
        a = nyr_emit_runtime_call(b, "rt_box_i64", a, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
      if (nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_RAW)
        rhs = nyr_emit_runtime_call(b, "rt_box_i64", rhs, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
      if (a < 0 || rhs < 0)
        return -1;
      b->last_arith_raw = 0;
      b->last_arith_expr = e;
      b->last_arith_tagged = true;
      return nyr_emit_runtime_call(b, "rt_any_mul", a, rhs, -1, 2,
                                   NYIR_SCALAR_I64_TAGGED);
    }
  }
  if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "/") == 0) {
    /* A float literal beside an `any` value does not prove scalar floating
     * arithmetic: the dynamic value may be a vector (Vector3 / 2.0 is one
     * such case). Keep that operation in the tagged runtime domain and box
     * the concrete float operand before dispatch. */
    bool dynamic_div =
        ((left_any || left_dynamic_result) && !left_f64 && !left_f32) ||
        ((right_any || right_dynamic_result) && !right_f64 && !right_f32);
    if (dynamic_div && (left_f64 || right_f64 || left_f32 || right_f32)) {
      if (left_f64 || left_f32) {
        int f64 = left_f32 ? nyr_emit_f32_to_f64(b, a) : a;
        /*
         * The register class outranks the AST float flag: a slot stamped
         * f64 by the classifier can still hold a tagged dynamic producer.
         * Normalize by class before taking IEEE bits.
         */
        if (f64 >= 0 && !nyr_value_is_f64(b, f64, 0)) {
          f64 = nyr_value_scalar_class(b, f64) == NYIR_SCALAR_I64_TAGGED
                    ? nyr_emit_runtime_call(b, "rt_any_to_f64", f64, -1, -1, 1,
                                            NYIR_SCALAR_F64)
                : nyr_value_is_f32(b, f64, 0) ? nyr_emit_f32_to_f64(b, f64)
                                              : nyr_emit_i64_to_f64(b, f64);
        }
        int bits = f64 < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                   -1, 1, NYIR_SCALAR_I64_RAW);
        a = bits < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                             1, NYIR_SCALAR_I64_TAGGED);
      } else if (!left_any && !left_dynamic_result) {
        a = nyr_emit_runtime_call(b, "rt_tag", a, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
      }
      if (right_f64 || right_f32) {
        int f64 = right_f32 ? nyr_emit_f32_to_f64(b, rhs) : rhs;
        /*
         * The register class outranks the AST float flag: a slot stamped
         * f64 by the classifier can still hold a tagged dynamic producer.
         * Normalize by class before taking IEEE bits.
         */
        if (f64 >= 0 && !nyr_value_is_f64(b, f64, 0)) {
          f64 = nyr_value_scalar_class(b, f64) == NYIR_SCALAR_I64_TAGGED
                    ? nyr_emit_runtime_call(b, "rt_any_to_f64", f64, -1, -1, 1,
                                            NYIR_SCALAR_F64)
                : nyr_value_is_f32(b, f64, 0) ? nyr_emit_f32_to_f64(b, f64)
                                              : nyr_emit_i64_to_f64(b, f64);
        }
        int bits = f64 < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                   -1, 1, NYIR_SCALAR_I64_RAW);
        rhs = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, NYIR_SCALAR_I64_TAGGED);
      } else if (!right_any && !right_dynamic_result) {
        rhs = nyr_emit_runtime_call(b, "rt_tag", rhs, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
      }
      if (a < 0 || rhs < 0)
        return -1;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-arithmetic",
            "function=%s operator=/ selected=dynamic reason=dynamic operand "
            "may be aggregate; float operands boxed",
            b->current_fn_name ? b->current_fn_name : "<entry>");
      b->last_arith_raw = 0;
      b->last_arith_expr = e;
      b->last_arith_tagged = true;
      return nyr_emit_runtime_call(b, "rt_any_div", a, rhs, -1, 2,
                                   NYIR_SCALAR_I64_TAGGED);
    }
    if (left_dict && right_dict && left_vector && right_vector) {
      return nyr_emit_runtime_call(b, "rt_vec_div_component_raw", a, rhs, -1, 2,
                                   0);
    }
    if (left_dict && left_vector && (right_f64 || right_f32)) {
      int s_f64 = right_f32 ? nyr_emit_f32_to_f64(b, rhs) : rhs;
      return nyr_emit_runtime_call(b, "rt_vec_div_scalar_raw", a, s_f64, -1, 2,
                                   0);
    }
    if ((left_any || right_any) && !left_f64 && !right_f64 && !left_f32 &&
        !right_f32) {
      b->last_arith_raw = 0;
      b->last_arith_expr = e;
      b->last_arith_tagged = true;
      return nyr_emit_runtime_call(b, "rt_any_div", a, rhs, -1, 2,
                                   NYIR_SCALAR_I64_TAGGED);
    }
  }
  /*
   * Bitwise operators never apply to strings, so a conservatively
   * string-typed operand (an int local stamped NY_SEM_REP_STRING by the
   * multi-assign typing in core.str) must not divert `&` into the
   * c-string path; it lowers as a raw integer operation.
   */
  bool cstr_path_bitwise = nyr_is_bitwise_operator(e->as.binary.op);
  if ((left_cstr || right_cstr) && !numeric_literal_sub && !cstr_path_bitwise) {
    if (left_cstr)
      nyr_stamp_local_load_class(b, a, NYIR_SCALAR_STRING);
    if (right_cstr)
      nyr_stamp_local_load_class(b, rhs, NYIR_SCALAR_STRING);
    if (!left_cstr) {
      int conv = -1;
      if (left_any)
        conv = nyr_emit_runtime_call(b, "rt_any_to_cstr", a, -1, -1, 1, 0);
      else if (left_bool)
        conv = nyr_emit_runtime_call(b, "rt_bool_to_cstr", a, -1, -1, 1, 0);
      else if (left_f32) {
        int f64 = nyr_emit_f32_to_f64(b, a);
        conv = f64 < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", f64, -1,
                                               -1, 1, 0);
      } else if (left_f64)
        conv = nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", a, -1, -1, 1, 0);
      else if (e->as.binary.left && e->as.binary.left->kind == NY_E_LITERAL &&
               e->as.binary.left->as.literal.kind == NY_LIT_INT)
        conv = nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", a, -1, -1, 1, 0);
      else
        conv = nyr_emit_runtime_call(b, "rt_any_to_cstr", a, -1, -1, 1, 0);
      a = conv;
    }
    if (!right_cstr) {
      int conv = -1;
      if (right_any)
        conv = nyr_emit_runtime_call(b, "rt_any_to_cstr", rhs, -1, -1, 1, 0);
      else if (right_bool)
        conv = nyr_emit_runtime_call(b, "rt_bool_to_cstr", rhs, -1, -1, 1, 0);
      else if (right_f32) {
        int f64 = nyr_emit_f32_to_f64(b, rhs);
        conv = f64 < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", f64, -1,
                                               -1, 1, 0);
      } else if (right_f64)
        conv =
            nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", rhs, -1, -1, 1, 0);
      else if (e->as.binary.right && e->as.binary.right->kind == NY_E_LITERAL &&
               e->as.binary.right->as.literal.kind == NY_LIT_INT)
        conv =
            nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", rhs, -1, -1, 1, 0);
      else
        conv = nyr_emit_runtime_call(b, "rt_any_to_cstr", rhs, -1, -1, 1, 0);
      rhs = conv;
    }
    if (a < 0 || rhs < 0)
      return -1;
    if (!is_cmp && e->as.binary.op && strcmp(e->as.binary.op, "+") == 0) {
      /*
       * The AST-level cstr proof can disagree with the register: an untyped
       * local the checker typed str may be computed in the f64 domain.
       * Converting an f64-class operand through the number-to-string bridge
       * keeps the raw concat contract (dgemm's repr path concatenated an
       * f64 register as a pointer and tripped the verify).
       */
      if (!left_cstr && nyr_value_is_f64(b, a, 0))
        a = nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", a, -1, -1, 1, 0);
      if (!right_cstr && nyr_value_is_f64(b, rhs, 0))
        rhs = nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", rhs, -1, -1, 1, 0);
      if (a < 0 || rhs < 0)
        return -1;
      int out = nyr_emit_runtime_call(b, "rt_cstr_concat", a, rhs, -1, 2, 0);
      int length = nyr_emit_known_cstr_concat_len(b, a, rhs, out);
      int tag = length < 0 ? -1 : nyr_emit_const(b, 121);
      if (out < 0 || length < 0 || tag < 0 ||
          !nyr_record_dyn_fact(b, out, NYR_FACT_DYN_STR_LEN, length) ||
          !nyr_record_dyn_fact(b, out, NYR_FACT_DYN_TAG, tag))
        return -1;
      return out;
    }
    if (is_cmp && (cmp == NYIR_CMP_EQ || cmp == NYIR_CMP_NE)) {
      int equal = nyr_emit_runtime_call(b, "rt_cstr_eq", a, rhs, -1, 2,
                                        NYIR_SCALAR_I64_RAW);
      if (equal < 0 || cmp == NYIR_CMP_EQ)
        return equal;
      int zero = nyr_emit_const(b, 0);
      return zero < 0 ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                          .dst = -1,
                                                          .a = equal,
                                                          .b = zero,
                                                          .cmp = NYIR_CMP_EQ});
    }
    if (is_cmp) {
      int compared = nyr_emit_runtime_call(b, "rt_cstr_cmp", a, rhs, -1, 2, 0);
      int zero = compared < 0 ? -1 : nyr_emit_const(b, 0);
      return compared < 0 || zero < 0
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                     .dst = -1,
                                                     .a = compared,
                                                     .b = zero,
                                                     .cmp = cmp});
    }
    return nyr_fail(b,
                    "native NYIR lower: unsupported string operator '%s' at "
                    "%s:%d in %s",
                    e->as.binary.op ? e->as.binary.op : "(null)",
                    e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
                    b->current_fn_name ? b->current_fn_name : "<unknown>"),
           -1;
  }
  /* Bitwise NYIR instructions operate on raw i64 values.  A bigint operand is
   * a heap handle, so route shifts through the representation-aware bridge
   * instead of shifting the pointer bits. */
  /*
   * Shifts are integer-domain operators.  Context propagation can stamp an
   * integer expression f64 inside a float-returning function
   * (`1.0 / float((ij * (ij+1)) >> 1)` pulled the shift's operands into
   * mul.f64), so an f64-classified register must re-enter the integer
   * domain before the raw handle bridge.  The shift value is integral, so
   * the conversion is exact.
   */
  if (!is_cmp && op == NYIR_SHL_I64 && (left_bigint || right_bigint)) {
    if (nyr_value_is_f64(b, a, 0)) {
      a = nyr_emit_runtime_call(b, "rt_f64_to_i64", a, -1, -1, 1, 0);
      if (a < 0)
        return -1;
    }
    if (nyr_value_is_f64(b, rhs, 0)) {
      rhs = nyr_emit_runtime_call(b, "rt_f64_to_i64", rhs, -1, -1, 1, 0);
      if (rhs < 0)
        return -1;
    }
    return nyr_emit_runtime_call(b, "rt_shl_raw", a, rhs, -1, 2, 0);
  }
  if (!is_cmp && op == NYIR_SAR_I64 && (left_bigint || right_bigint)) {
    if (nyr_value_is_f64(b, a, 0)) {
      a = nyr_emit_runtime_call(b, "rt_f64_to_i64", a, -1, -1, 1, 0);
      if (a < 0)
        return -1;
    }
    if (nyr_value_is_f64(b, rhs, 0)) {
      rhs = nyr_emit_runtime_call(b, "rt_f64_to_i64", rhs, -1, -1, 1, 0);
      if (rhs < 0)
        return -1;
    }
    return nyr_emit_runtime_call(b, "rt_shr_raw", a, rhs, -1, 2, 0);
  }
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
      (is_cmp && !expr_f64 && !left_f64 && !right_f64 &&
       !nyr_expr_is_f32(b, e) && !nyr_expr_is_f32(b, e->as.binary.left) &&
       !nyr_expr_is_f32(b, e->as.binary.right));
  /* Dynamic values can reach this raw integer path as locals or as computed
   * tagged results (for example the left side of `(xs[0] + xs[1]) + xs[2]`).
   * Decode those values before the raw NYIR operation.  A raw producer has
   * already cleared its dynamic provenance above, and raw dynamic reads are
   * explicitly excluded because their source ABI is an unboxed machine word. */
  bool left_tagged_input =
      (left_any || left_dynamic_result || nyr_value_is_tag_call(b, a)) &&
      (!left_raw_i64 || nyr_value_is_tag_call(b, a)) && e->as.binary.left &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.left) &&
      /*
       * The AST-level f64/f32 flags can miss a float producer stamped by
       * the lowering itself (a member load re-boxed through
       * rt_any_to_f64).  The register class is authoritative: decoding an
       * f64-classified value with rt_any_to_i64 trips the call scalar-class
       * verifier and feeds the (possibly fcmp) comparison bit-mangled
       * integers.
       */
      !nyr_value_is_f64(b, a, 0) && !nyr_value_is_f32(b, a, 0);
  bool right_tagged_input =
      (right_any || right_dynamic_result || nyr_value_is_tag_call(b, rhs)) &&
      (!right_raw_i64 || nyr_value_is_tag_call(b, rhs)) && e->as.binary.right &&
      !nyr_expr_is_raw_dynamic_read(b, e->as.binary.right) &&
      !nyr_value_is_f64(b, rhs, 0) && !nyr_value_is_f32(b, rhs, 0);
  if (integer_domain && left_tagged_input && !left_bigint) {
    a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    if (a < 0)
      return -1;
  }
  if (integer_domain && right_tagged_input && !right_bigint) {
    rhs = nyr_emit_runtime_call(b, "rt_any_to_i64", rhs, -1, -1, 1, 0);
    if (rhs < 0)
      return -1;
  }
  /* Recheck the final operand registers immediately before numeric
   * normalization. Earlier lowering branches can replace an operand after
   * the initial AST-driven opcode choice; the defining NYIR class is the
   * definitive evidence at this boundary. */
  bool final_left_f64 = a >= 0 && nyr_value_is_f64(b, a, 0);
  bool final_right_f64 = rhs >= 0 && nyr_value_is_f64(b, rhs, 0);
  bool final_left_f32 = a >= 0 && nyr_value_is_f32(b, a, 0);
  bool final_right_f32 = rhs >= 0 && nyr_value_is_f32(b, rhs, 0);
  if (final_left_f64 || final_right_f64) {
    left_f64 = final_left_f64;
    right_f64 = final_right_f64;
    left_f32 = final_left_f32;
    right_f32 = final_right_f32;
    if (!is_cmp && (strcmp(e->as.binary.op, "+") == 0 ||
                    strcmp(e->as.binary.op, "-") == 0 ||
                    strcmp(e->as.binary.op, "*") == 0 ||
                    strcmp(e->as.binary.op, "/") == 0)) {
      if (strcmp(e->as.binary.op, "+") == 0)
        op = NYIR_ADD_F64;
      else if (strcmp(e->as.binary.op, "-") == 0)
        op = NYIR_SUB_F64;
      else if (strcmp(e->as.binary.op, "*") == 0)
        op = NYIR_MUL_F64;
      else
        op = NYIR_DIV_F64;
    }
  } else if (final_left_f32 || final_right_f32) {
    left_f32 = final_left_f32;
    right_f32 = final_right_f32;
    left_f64 = false;
    right_f64 = false;
    if (!is_cmp && (strcmp(e->as.binary.op, "+") == 0 ||
                    strcmp(e->as.binary.op, "-") == 0 ||
                    strcmp(e->as.binary.op, "*") == 0 ||
                    strcmp(e->as.binary.op, "/") == 0)) {
      if (strcmp(e->as.binary.op, "+") == 0)
        op = NYIR_ADD_F32;
      else if (strcmp(e->as.binary.op, "-") == 0)
        op = NYIR_SUB_F32;
      else if (strcmp(e->as.binary.op, "*") == 0)
        op = NYIR_MUL_F32;
      else
        op = NYIR_DIV_F32;
    }
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && !is_cmp &&
      (final_left_f64 || final_right_f64 || final_left_f32 || final_right_f32))
    ny_dbg_heuristic_event(
        "native-arithmetic",
        "function=%s operator=%s resolved=%s producer_classes{left=%d "
        "right=%d} action=promote_and_convert",
        b->current_fn_name ? b->current_fn_name : "<entry>", e->as.binary.op,
        op == NYIR_ADD_F64 || op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
                op == NYIR_DIV_F64
            ? "f64"
            : "f32",
        (int)nyr_value_scalar_class(b, a), (int)nyr_value_scalar_class(b, rhs));
  bool use_f64_cmp = is_cmp && (left_f64 || right_f64);
  bool use_f32_cmp = is_cmp && !use_f64_cmp && (left_f32 || right_f32);
  if ((!is_cmp && (op == NYIR_ADD_F32 || op == NYIR_SUB_F32 ||
                   op == NYIR_MUL_F32 || op == NYIR_DIV_F32)) ||
      use_f32_cmp) {
    if (!left_f32) {
      a = nyr_emit_i64_to_f32(b, a);
      if (a < 0)
        return -1;
    }
    if (!right_f32) {
      rhs = nyr_emit_i64_to_f32(b, rhs);
      if (rhs < 0)
        return -1;
    }
  } else if ((!is_cmp && (op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
                          op == NYIR_MUL_F64 || op == NYIR_DIV_F64)) ||
             use_f64_cmp) {
    if (!left_f64) {
      if (left_f32) {
        a = nyr_emit_f32_to_f64(b, a);
      } else if (left_any ||
                 nyr_value_scalar_class(b, a) == NYIR_SCALAR_I64_TAGGED) {
        /*
         * The register class outranks the AST any-flag: a tagged producer
         * (the rt_any_mul bridge and siblings) carries a canonical dynamic
         * that must be decoded, not SIToFP-converted as an integer.
         */
        a = nyr_emit_runtime_call(b, "rt_any_to_f64", a, -1, -1, 1,
                                  NYIR_SCALAR_F64);
      } else {
        a = nyr_emit_i64_to_f64(b, a);
      }
      if (a < 0)
        return -1;
    }
    /*
     * The register class outranks the missed AST classification here: the
     * float() cast yields machine f64, but a stale AST-level fact (or a
     * probe-cache miss) classified it int, and i64.to.f64 of an f64
     * register reinterpreted the IEEE bits (spectral divided by garbage).
     */
    if (!right_f64 && rhs >= 0 && nyr_value_is_f64(b, rhs, 0))
      right_f64 = true;
    if (!right_f64) {
      if (right_f32) {
        rhs = nyr_emit_f32_to_f64(b, rhs);
      } else if (right_any ||
                 nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_TAGGED) {
        /*
         * The register class outranks the AST any-flag: a tagged producer
         * (the rt_any_mul bridge and siblings) carries a canonical dynamic
         * that must be decoded, not SIToFP-converted as an integer.
         */
        rhs = nyr_emit_runtime_call(b, "rt_any_to_f64", rhs, -1, -1, 1,
                                    NYIR_SCALAR_F64);
      } else {
        rhs = nyr_emit_i64_to_f64(b, rhs);
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
      const char *msg =
          op == NYIR_MOD_I64 ? "modulo by zero" : "division by zero";
      nyr_op_t cmp_op = op == NYIR_DIV_F64   ? NYIR_CMP_F64
                         : op == NYIR_DIV_F32 ? NYIR_CMP_F32
                                              : NYIR_CMP_I64;
      if (!nyr_emit_div_zero_guard(b, cmp_op, rhs, msg))
        return -1;
    }
  }
  bool raw_scalar_shift = !is_cmp && op == NYIR_SAR_I64 &&
                          nyr_expr_is_raw_scalar(b, e->as.binary.left, 0) &&
                          nyr_expr_is_raw_scalar(b, e->as.binary.right, 0);
  if (!is_cmp && (op == NYIR_SHL_I64 || op == NYIR_SAR_I64)) {
    /* Scalar shifts use the low six count bits. Materialize that contract
     * before inlining/constant propagation exposes negative or oversized
     * counts to the NYIR verifier; do not rely on host instruction masking.
     * BigInt shifts have already taken their separate runtime path above. */
    int64_t count = 0;
    if (!nyr_const_i64_expr(e->as.binary.right, &count, 0) || count < 0 ||
        count >= 64) {
      if (nyr_value_is_f64(b, rhs, 0))
        rhs = nyr_emit_runtime_call(b, "rt_f64_to_i64", rhs, -1, -1, 1, 0);
      int mask = nyr_emit_const(b, 63);
      rhs =
          rhs < 0 || mask < 0 ? -1 : nyr_emit_binop(b, NYIR_AND_I64, rhs, mask);
      if (rhs < 0)
        return -1;
    }
  }
  /*
   * Class-driven promotion: when both operands' defining instructions are
   * f64-classified (layout/dict member loads stamped by the C2 facts) and
   * the expression classification missed it, the integer op would bit-mangle
   * floats and the machine-form gate rejects the mismatch outright.  The
   * physical operand class wins over the missed expression classification.
   */
  if (!is_cmp && !expr_f64 && !left_f64 && !right_f64 && !left_f32 &&
      !right_f32 && a >= 0 && rhs >= 0 &&
      (op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_MUL_I64 ||
       op == NYIR_DIV_I64) &&
      nyr_value_is_f64(b, a, 0) && nyr_value_is_f64(b, rhs, 0)) {
    op = op == NYIR_ADD_I64   ? NYIR_ADD_F64
         : op == NYIR_SUB_I64 ? NYIR_SUB_F64
         : op == NYIR_MUL_I64 ? NYIR_MUL_F64
                              : NYIR_DIV_F64;
    expr_f64 = true;
    b->last_arith_raw = 0;
  }
  int v =
      raw_scalar_shift
          ? (/* The AST-level raw-scalar proof can miss that context
               propagation computed an operand in the f64 domain (a
               float-returning function pulled `(ij*(ij+1)) >> 1` into
               mul.f64).  The shift is integer-domain: re-enter it. */
             nyr_value_is_f64(b, a, 0)
                 ? (a = nyr_emit_runtime_call(b, "rt_f64_to_i64", a, -1, -1, 1,
                                              0))
                 : a,
             nyr_value_is_f64(b, rhs, 0)
                 ? (rhs = nyr_emit_runtime_call(b, "rt_f64_to_i64", rhs, -1, -1,
                                                1, 0))
                 : rhs,
             a < 0 || rhs < 0
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                     .dst = -1,
                                                     .a = a,
                                                     .b = rhs,
                                                     .scalar_class =
                                                         NYIR_SCALAR_I64_RAW}))
          : nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){
                          .op = use_f64_cmp   ? NYIR_CMP_F64
                                : use_f32_cmp ? NYIR_CMP_F32
                                : is_cmp      ? NYIR_CMP_I64
                                              : op,
                          .dst = -1,
                          .a = a,
                          .b = rhs,
                          .cmp = cmp,
                          /* Float arithmetic must carry
                           * its result class: consumers
                           * (promotion, the machine type
                           * map) read it back off the
                           * defining instruction. */
                          .scalar_class =
                              op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
                                      op == NYIR_MUL_F64 || op == NYIR_DIV_F64
                                  ? NYIR_SCALAR_F64
                              : op == NYIR_ADD_F32 || op == NYIR_SUB_F32 ||
                                      op == NYIR_MUL_F32 || op == NYIR_DIV_F32
                                  ? NYIR_SCALAR_F32
                              : integer_domain && !is_cmp
                                  ? NYIR_SCALAR_I64_RAW
                                  : NYIR_SCALAR_UNKNOWN});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  /*
   * Record the payload representation for the dynamic boundaries (any-local
   * stores, any formals): the integer emitters above deliberately produce a
   * raw i64 even when typing stamps this expression TAGGED_DYNAMIC, and the
   * destination, not this producer, owns the single boxing decision.
   */
  b->last_arith_raw = 0;
  b->last_arith_expr = e;
  if (v >= 0 && !is_cmp && raw_scalar_shift)
    b->last_arith_raw = 1;
  if (v >= 0 && !is_cmp && !use_f64_cmp && !use_f32_cmp &&
      (op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_MUL_I64 ||
       op == NYIR_DIV_I64 || op == NYIR_MOD_I64 || op == NYIR_AND_I64 ||
       op == NYIR_OR_I64 || op == NYIR_XOR_I64 || op == NYIR_SHL_I64 ||
       op == NYIR_SAR_I64 || op == NYIR_ROR_I64))
    b->last_arith_raw = 1;
  if (v >= 0 && !is_cmp && final_left_f64 && final_right_f64 &&
      (op == NYIR_ADD_F64 || op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
       op == NYIR_DIV_F64)) {
    /* With semantic analysis skipped, a binary expression can still be
     * proven f64 from both emitted operands. Preserve that proof so an
     * inferred local does not box the result and turn the next operation
     * into a dynamic-any helper call. */
    ((expr_t *)e)->semantic.rep = NY_SEM_REP_F64;
    ((expr_t *)e)->semantic.resolved = true;
  }
  if (v >= 0 && !is_cmp && preserve_dynamic_result &&
      (left_dynamic_index || right_dynamic_index) && !expr_f64 && !expr_f32) {
    v = nyr_emit_runtime_call(b, "rt_tag", v, -1, -1, 1, 0);
    b->last_arith_raw = 0;
  }
  return v;
}
