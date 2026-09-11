typedef struct {
  const char *name;
  int64_t value;
} ny_native_proof_binding_t;

static bool ny_native_proof_binding_range(void *ctx, const char *name,
                                          int64_t *lo, int64_t *hi) {
  const ny_native_proof_binding_t *binding = ctx;
  if (strcmp(name, binding->name) != 0)
    return false;
  *lo = *hi = binding->value;
  return true;
}

/*
 * Semantic-first early-leaf resolution.  The intrinsic groups below decide
 * raw vs dynamic by the call's canonical callee when the frontend resolved
 * one (attached-method sugar, aliases, and re-exports can spell the same
 * intrinsic differently); the surface spelling is only a fallback for
 * unresolvable/synthesized nodes (e.g. @inline primitive wrapper bodies).
 */
static const char *ny_native_nir_early_leaf(const expr_t *e) {
  if (e && e->semantic.member_call_kind != NY_SEM_CALL_NONE &&
      e->semantic.canonical_callee) {
    const char *cleaf = ny_native_leaf_name(e->semantic.canonical_callee);
    if (cleaf && *cleaf)
      return cleaf;
  }
  return ny_native_call_leaf(e);
}

static bool ny_native_nir_leaf_in(const char *leaf, const char *const *set,
                                  size_t n) {
  if (!leaf)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (strcmp(leaf, set[i]) == 0)
      return true;
  return false;
}

/* Untyped functions whose parameters participate directly in scalar
 * operators use the raw NYIR integer ABI.  Semantic inference deliberately
 * leaves these legacy declarations unresolved, but the lowered body still
 * exposes the ABI fact.  Recover it structurally so callers do not append
 * dynamic value metadata to a raw two-argument function (which shifts the
 * remaining arguments and corrupts the call). */
static bool ny_native_nir_param_has_scalar_op_expr(const expr_t *e,
                                                   const char *param) {
  if (!e || !param)
    return false;
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    const char *op = e->as.binary.op;
    bool scalar_op = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                     strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                     strcmp(op, "%") == 0 || strcmp(op, "<<") == 0 ||
                     strcmp(op, ">>") == 0 || strcmp(op, "&") == 0 ||
                     strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
                     strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                     strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                     strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;
    if (scalar_op) {
      const expr_t *l = e->as.binary.left;
      const expr_t *r = e->as.binary.right;
      if ((l && l->kind == NY_E_IDENT && l->as.ident.name &&
           strcmp(l->as.ident.name, param) == 0) ||
          (r && r->kind == NY_E_IDENT && r->as.ident.name &&
           strcmp(r->as.ident.name, param) == 0))
        return true;
    }
    return ny_native_nir_param_has_scalar_op_expr(e->as.binary.left, param) ||
           ny_native_nir_param_has_scalar_op_expr(e->as.binary.right, param);
  }
  if (e->kind == NY_E_UNARY)
    return ny_native_nir_param_has_scalar_op_expr(e->as.unary.right, param);
  if (e->kind == NY_E_LOGICAL)
    return ny_native_nir_param_has_scalar_op_expr(e->as.logical.left, param) ||
           ny_native_nir_param_has_scalar_op_expr(e->as.logical.right, param);
  if (e->kind == NY_E_TERNARY)
    return ny_native_nir_param_has_scalar_op_expr(e->as.ternary.cond, param) ||
           ny_native_nir_param_has_scalar_op_expr(e->as.ternary.true_expr, param) ||
           ny_native_nir_param_has_scalar_op_expr(e->as.ternary.false_expr, param);
  return false;
}

static bool ny_native_nir_untyped_param_is_raw(const stmt_t *fn,
                                               const char *param) {
  /*
   * The callee's parameter semantic is the authoritative ABI fact: an untyped
   * parameter is registered as a tagged-dynamic local unless inference proved
   * a raw integer slot (lower_stmt.h param registration keys on exactly this
   * semantic).  The historical body-shape heuristic ("used in a scalar
   * operator") disagreed with that registration — the callee lowered `x * x`
   * through rt_any_mul while the caller delivered a raw literal, so odd raw
   * arguments were untagged as their halved value (foo(7) computed 3*3).
   */
  if (!fn || fn->kind != NY_S_FUNC || !param)
    return false;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!fn->as.fn.params.data[i].name ||
        strcmp(fn->as.fn.params.data[i].name, param) != 0)
      continue;
    const ny_expr_semantic_t *sem = &fn->as.fn.params.data[i].semantic;
    return sem->resolved && sem->rep == NY_SEM_REP_RAW_INT;
  }
  return false;
}

static bool ny_native_nir_mapped_proof_holds(const char *proof_type,
                                             const char *param, int64_t value) {
  if (!proof_type || !param || !*param)
    return false;
  size_t len = strlen(proof_type);
  if (len < 7 || strncmp(proof_type, "proof<", 6) != 0 ||
      proof_type[len - 1] != '>')
    return false;
  char *proposition = ny_strndup(proof_type + 6, len - 7);
  if (!proposition)
    return false;
  ny_native_proof_binding_t binding = {param, value};
  int decision = ny_proof_canon_decide(
      proposition, ny_native_proof_binding_range, &binding, 0);
  free(proposition);
  return decision == 1;
}

static int ny_native_nir_lower_call(ny_native_nir_builder_t *b,
                                    const expr_t *e) {
  /* `range` is variadic at the language level, while the old AST ABI models
   * its packed argument list as an `any` aggregate.  NYIR has no aggregate
   * varargs representation, so lower the three supported forms directly to
   * the raw range allocator. */
  const char *early_leaf = ny_native_call_leaf(e);
  /* Semantic-first view shared by the raw-vs-dynamic intrinsic families:
   * __shl, bitwise primitives, ptr_add, and range. The canonical callee
   * leaf wins over the surface spelling so aliases/attached sugar keep the
   * raw path; spelling remains the fallback for unresolved nodes. */
  const char *canon_leaf = ny_native_nir_early_leaf(e);
  /* The public __shl intrinsic is tagged in the interpreter ABI, but native
   * NYIR arguments are raw scalar integers. Route it through the raw adapter
   * so overflow promotes to BigInt instead of wrapping the machine shift. */
  if (canon_leaf && strcmp(canon_leaf, "__shl") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int left = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int right = left < 0 ? -1
                         : ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (left < 0 || right < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_shl_raw", left, right,
                                           -1, 2, 0);
  }
  /* Primitive bitwise helpers are tagged in the interpreter, while native
   * scalar callers (flags, masks, ABI fields) carry raw i64 values. Preserve
   * the raw domain when both operands are statically scalar; routing these
   * through rt_or would shift each value as if it were a tagged integer and
   * silently turn masks such as 1|2 into 5. Dynamic operands keep the normal
   * call path below. */
  if (canon_leaf && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      ny_native_nir_leaf_in(canon_leaf,
                            (const char *const[]){
                                "__or", "bor", "__and", "band", "__xor",
                                "bxor",
                            },
                            6)) {
    const expr_t *left_expr = e->as.call.args.data[0].val;
    const expr_t *right_expr = e->as.call.args.data[1].val;
    bool dynamic = ny_native_nir_expr_is_any(b, left_expr) ||
                   ny_native_nir_expr_is_any(b, right_expr);
    /* Primitive wrappers are untyped, but a function with an explicit raw
     * integer result (feature masks, flags, ABI words) establishes the
     * scalar domain for its local bitwise accumulation. */
    if (dynamic && b->current_fn_name) {
      const stmt_t *owner =
          ny_native_nir_find_user_function(b, b->current_fn_name);
      if (owner && owner->as.fn.return_type &&
          !ny_native_type_name_is_any(owner->as.fn.return_type) &&
          !ny_native_type_name_is_list(owner->as.fn.return_type) &&
          !ny_native_type_name_is_f64(owner->as.fn.return_type) &&
          !ny_native_type_name_is_f32(owner->as.fn.return_type))
        dynamic = false;
    }
    if (!dynamic) {
      int left = ny_native_nir_lower_expr(b, left_expr);
      int right = left < 0 ? -1 : ny_native_nir_lower_expr(b, right_expr);
      if (left < 0 || right < 0)
        return -1;
      int op = (strcmp(canon_leaf, "__or") == 0 ||
                strcmp(canon_leaf, "bor") == 0)
                   ? NYIR_OR_I64
                   : ((strcmp(canon_leaf, "__and") == 0 ||
                       strcmp(canon_leaf, "band") == 0)
                          ? NYIR_AND_I64
                          : NYIR_XOR_I64);
      return ny_native_nir_emit_binop(b, op, left, right);
    }
  }
  if (canon_leaf && strcmp(canon_leaf, "ptr_add") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int ptr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int off = ptr < 0 ? -1
                      : ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    return off < 0 ? -1
                   : ny_native_nir_emit_runtime_call(b, "rt_ptr_add_i64",
                                                     ptr, off, -1, 2, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "range") == 0 ||
       strcmp(canon_leaf, "range_new_raw") == 0) &&
      e->as.call.args.len >= 1 && e->as.call.args.len <= 3) {
    bool named = false;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      named = named || e->as.call.args.data[i].name != NULL;
    if (!named) {
      int start = ny_native_nir_emit_const(b, 0);
      int stop = -1;
      int step = ny_native_nir_emit_const(b, 1);
      if (e->as.call.args.len == 1) {
        stop = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      } else {
        start = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
        stop = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (e->as.call.args.len == 3)
          step = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      }
      if (start < 0 || stop < 0 || step < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_range_new", start, stop,
                                             step, 3, 0);
    }
  }
  if (early_leaf && strcmp(early_leaf, "unwrap_or") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int v = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int def_val = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (v < 0 || def_val < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_result_unwrap_or_raw", v,
                                           def_val, -1, 2, 0);
  }
  /* These std.core helpers still have a legacy heap-list implementation in
   * source for the interpreter.  Native lists are tbufs, so route the
   * implementation-sensitive operations through the ABI bridge even when the
   * formal parameter is `any`. */
  if (early_leaf && strcmp(early_leaf, "_clone_list") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int list = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return list < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, "rt_tbuf_clone_raw",
                                                      list, -1, -1, 1, 0);
  }
  if (early_leaf &&
      (strcmp(early_leaf, "swap_items") == 0 ||
       strcmp(early_leaf, "swap") == 0) &&
      e->as.call.args.len == 3 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name && !e->as.call.args.data[2].name) {
    int list = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int left = list < 0
                   ? -1
                   : ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int right = left < 0
                    ? -1
                    : ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    return right < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_tbuf_swap",
                                                       list, left, right, 3, 0);
  }
  /* Native list buffers are mutable.  Preserve the language-level clear
   * operation's in-place semantics; cloning here leaves the caller's list
   * unchanged and makes the following observation of the list surprising. */
  if (early_leaf && strcmp(early_leaf, "clear") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_list(b, e->as.call.args.data[0].val)) {
    int list = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return list < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, "rt_tbuf_clear_raw",
                                                      list, -1, -1, 1, 0);
  }
  /* `count` is the sequence capability's length operation.  Its stdlib
   * implementation dispatches through the same `.len` surface, so use the
   * native length bridge for lists, strings, bytes, ranges, and dynamic
   * sequence parameters alike. */
  if (early_leaf && strcmp(early_leaf, "count") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    if (ny_native_nir_user_defined_fn(b, e->as.call.callee->as.ident.name))
      goto ordinary_call;
    int sequence = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return sequence < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_sequence_len_raw",
                                                 sequence, -1, -1, 1, 0);
  }
  /* std.core.sort/sorted dispatch through the boxed list ABI in the VM, but
   * native literals and list locals are raw typed buffers.  Use the runtime's
   * tbuf-aware sorter and clone only for the non-mutating `sorted` surface. */
  if (early_leaf &&
      (strcmp(early_leaf, "sort") == 0 || strcmp(early_leaf, "sorted") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_list(b, e->as.call.args.data[0].val)) {
    int list = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (list < 0)
      return -1;
    if (strcmp(early_leaf, "sorted") == 0)
      list = ny_native_nir_emit_runtime_call(b, "rt_tbuf_clone_raw", list,
                                             -1, -1, 1, 0);
    return list < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, "__sort_list", list,
                                                      -1, -1, 1, 0);
  }
  if (early_leaf &&
      (strcmp(early_leaf, "sort") == 0 || strcmp(early_leaf, "sorted") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_range(b, e->as.call.args.data[0].val)) {
    int range = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int list = range < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b, "rt_range_values_raw", range, -1, -1, 1,
                               0);
    return list < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, "__sort_list", list,
                                                      -1, -1, 1, 0);
  }
  /* The legacy `any` ABI expands each operand to value/length/tag triples.
   * NYIR keeps the native value in one raw slot, so structural equality only
   * needs the two value positions. */
  if (early_leaf && strcmp(early_leaf, "eq") == 0 &&
      (e->as.call.args.len == 2 || e->as.call.args.len == 6)) {
    size_t rhs_index = e->as.call.args.len == 6 ? 3u : 1u;
    int left = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int right =
        ny_native_nir_lower_expr(b, e->as.call.args.data[rhs_index].val);
    return left < 0 || right < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_any_eq", left,
                                                 right, -1, 2, 0);
  }
  /* A qualified module call (`core_ref.get(x)`) is represented by the parser
   * as a member-shaped callee, but semantic attached-method resolution may
   * also annotate it as `std.core.any.get`.  The alias is a namespace, not a
   * runtime receiver: canonicalize it before the attached-call path can
   * materialize `core_ref` as an ELF data symbol. */
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.target &&
      e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
      e->as.call.callee->as.member.target->as.ident.name &&
      e->as.call.callee->as.member.name) {
    const char *alias = e->as.call.callee->as.member.target->as.ident.name;
    /* A parameter/local shadows an imported module alias.  Without this
     * guard `io.set(...)` in the networking helpers becomes the unresolved
     * namespace symbol `std.core.io.set` instead of a dictionary method. */
    const char *module = ny_native_nir_find_local(b, alias)
                             ? NULL
                             : ny_native_nir_resolve_use_alias(b, alias);
    if (!module && !ny_native_nir_find_local(b, alias)) {
      ny_native_nir_builder_t probe = *b;
      probe.source_file = NULL;
      probe.locals = NULL;
      probe.local_count = probe.local_cap = 0;
      module = ny_native_nir_resolve_use_alias(&probe, alias);
    }
    if (module) {
      char canonical[512];
      int n = snprintf(canonical, sizeof(canonical), "%s.%s", module,
                       e->as.call.callee->as.member.name);
      if (n <= 0 || (size_t)n >= sizeof(canonical)) {
        ny_native_nir_fail(b,
                           "native NYIR lower: module call name is too long");
        return -1;
      }
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = canonical;
      expr_t direct = *e;
      direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
      direct.semantic.canonical_callee = NULL;
      direct.semantic.canonical_callee_stmt = NULL;
      direct.as.call.callee = &callee;
      return ny_native_nir_lower_call(b, &direct);
    }
  }
  if (e->as.call.callee &&
      (e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED ||
       e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT)) {
    const char *canonical = e->semantic.canonical_callee;
    char attached_name[512];
    /* Semantic lookup can choose an unrelated same-named stdlib method when
     * the receiver is the result of a nominal `self` call.  Recover the
     * concrete impl owner from the local declaration and prefer its attached
     * method before constructing the direct call. */
    if (e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.name) {
      const stmt_t *method = ny_native_nir_find_attached_method(
          b, e->as.call.callee->as.member.target,
          e->as.call.callee->as.member.name);
      const char *owner = ny_native_nir_expr_type_name(
          b, e->as.call.callee->as.member.target);
      if (method && owner && method->as.fn.name) {
        int n = snprintf(attached_name, sizeof(attached_name), "%s.%s", owner,
                         method->as.fn.name);
        if (n > 0 && (size_t)n < sizeof(attached_name))
          canonical = attached_name;
      }
    }
    if (canonical && e->as.call.args.len + 1 <= NYIR_CALL_MAX_ARGS) {
      char base[512];
      expr_t receiver_ident = {.kind = NY_E_IDENT, .tok = e->tok};
      expr_t *receiver = NULL;
      if (e->as.call.callee->kind == NY_E_MEMBER) {
        receiver = e->as.call.callee->as.member.target;
      } else if (e->as.call.callee->kind == NY_E_IDENT) {
        const char *spelled = e->as.call.callee->as.ident.name;
        const char *dot = spelled ? strrchr(spelled, '.') : NULL;
        if (dot && dot > spelled) {
          size_t base_len = (size_t)(dot - spelled);
          if (base_len >= sizeof(base)) {
            ny_native_nir_fail(
                b, "native NYIR lower: semantic receiver name is too long");
            return -1;
          }
          memcpy(base, spelled, base_len);
          base[base_len] = '\0';
          receiver_ident.as.ident.name = base;
          receiver = &receiver_ident;
        }
      }
      if (!receiver)
        goto ordinary_call;
      /*
       * A canonical attached call whose receiver is a bare identifier with no
       * local, function, or top-level value binding is a module-namespace
       * reference (for example `it.map` where `it` aliases `std.core.iter`).
       * The semantic layer resolves it to the canonical module function, but
       * the receiver is not a runtime value: lowering it materializes the
       * module registry as a real argument and shifts every actual argument
       * out of its parameter slot (the list lands in the fnptr slot and the
       * callback is dropped).  Rebuild the call without the receiver so the
       * ordinary direct-call path pairs the real arguments with the callee
       * parameters.
       */
      bool namespace_receiver = false;
      if (receiver && receiver->kind == NY_E_IDENT && receiver->as.ident.name) {
        const char *rcv = receiver->as.ident.name;
        /* Import aliases may also be materialized in the top-level value
         * table as module registries.  The alias fact is authoritative: a
         * qualified import call is a namespace call even when that registry
         * happens to be discoverable as a value. */
        namespace_receiver = ny_native_nir_resolve_use_alias(b, rcv) != NULL ||
                             (!ny_native_nir_find_local(b, rcv) &&
                              !ny_native_nir_find_user_function(b, rcv) &&
                              !ny_native_nir_find_top_level_value(b, rcv));
      }
      if (namespace_receiver) {
        expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
        callee.as.ident.name = canonical;
        expr_t direct = {.kind = NY_E_CALL, .tok = e->tok};
        direct.semantic = e->semantic;
        direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
        direct.semantic.canonical_callee = NULL;
        direct.semantic.canonical_callee_stmt = NULL;
        direct.as.call.callee = &callee;
        direct.as.call.args.data = e->as.call.args.data;
        direct.as.call.args.len = direct.as.call.args.cap = e->as.call.args.len;
        return ny_native_nir_lower_expr(b, &direct);
      }
      const char *canonical_leaf = strrchr(canonical, '.');
      canonical_leaf = canonical_leaf ? canonical_leaf + 1 : canonical;
      if (strcmp(canonical_leaf, "clear") == 0 && e->as.call.args.len == 0 &&
          ny_native_nir_expr_is_list(b, receiver)) {
        int list = ny_native_nir_lower_expr(b, receiver);
        int copy = list < 0
                       ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_tbuf_clone_raw", list, -1, -1, 1, 0);
        return copy < 0 ? -1
                        : ny_native_nir_emit_runtime_call(
                              b, "rt_tbuf_clear_raw", copy, -1, -1, 1, 0);
      }
      if ((strcmp(canonical_leaf, "delete") == 0 ||
           strcmp(canonical_leaf, "remove") == 0) &&
          e->as.call.args.len == 1 && ny_native_nir_expr_is_dict(b, receiver)) {
        int dict = ny_native_nir_lower_expr(b, receiver);
        int key = ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
        if (dict < 0 || key < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_dict_delete_str_raw"
                : "rt_dict_delete_raw",
            dict, key, -1, 2, 0);
      }
      if (strcmp(canonical_leaf, "set") == 0 && e->as.call.args.len == 2 &&
          ny_native_nir_expr_is_dict(b, receiver)) {
        int dict = ny_native_nir_lower_expr(b, receiver);
        int key = ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
        int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (dict < 0 || key < 0 || value < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b, ny_native_nir_dict_set_symbol(b, e->as.call.args.data[0].val),
            dict, key, value, 3, 0);
      }
      if ((strcmp(canonical_leaf, "get") == 0 ||
           strcmp(canonical_leaf, "dict_get") == 0) &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        bool receiver_is_dict = ny_native_nir_expr_is_dict(b, receiver);
        if (receiver &&
            (receiver->kind == NY_E_CALL || receiver->kind == NY_E_MEMCALL))
          receiver_is_dict = false;
        int dict = ny_native_nir_lower_expr(b, receiver);
        int key =
            receiver_is_dict
                ? ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val)
                : ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
        if (!receiver_is_dict &&
            !ny_native_nir_expr_is_any(b, e->as.call.args.data[0].val) &&
            !ny_native_nir_expr_is_cstr(b, e->as.call.args.data[0].val)) {
          int one = ny_native_nir_emit_const(b, 1);
          int shifted =
              one < 0 ? -1
                      : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = key,
                                                          .b = one});
          key = shifted < 0
                    ? -1
                    : ny_native_nir_emit_binop(b, NYIR_OR_I64, shifted, one);
        }
        int fallback =
            e->as.call.args.len == 2
                ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                : ny_native_nir_emit_const(b, 0);
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        if (!receiver_is_dict) {
          int got = ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged",
                                                    dict, key, fallback, 3, 0);
          /* An integer literal default types the expression as a raw scalar;
           * the canonical accessor returns the tagged word, so decode here
           * (v.get(0, 0) > 2 otherwise compares tagged 3 > 2 and every
           * nested list survives the filter). */
          if (got >= 0 && e->as.call.args.len == 2 &&
              e->as.call.args.data[1].val &&
              e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
              e->as.call.args.data[1].val->as.literal.kind == NY_LIT_INT &&
              e->as.call.args.data[1].val->tok.kind != NY_T_NIL)
            return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", got,
                                                   -1, -1, 1, 0);
          return got;
        }
        return ny_native_nir_emit_runtime_call(
            b, ny_native_nir_dict_get_symbol(b, e->as.call.args.data[0].val),
            dict, key, fallback, 3, 0);
      }
      call_arg_t args[NYIR_CALL_MAX_ARGS] = {{.val = receiver}};
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        args[i + 1] = e->as.call.args.data[i];
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = canonical;
      expr_t direct = {.kind = NY_E_CALL, .tok = e->tok};
      direct.semantic = e->semantic;
      direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
      direct.semantic.canonical_callee = NULL;
      direct.semantic.canonical_callee_stmt = NULL;
      direct.as.call.callee = &callee;
      direct.as.call.args.data = args;
      direct.as.call.args.len = direct.as.call.args.cap =
          e->as.call.args.len + 1;
      return ny_native_nir_lower_expr(b, &direct);
    }
  }
ordinary_call:
  if (!e->as.call.callee) {
    ny_native_nir_fail(b, "native NYIR lower: only direct calls are supported");
    return -1;
  }
  const expr_call_t *c = &e->as.call;
  const stmt_t *direct_callee_fn = NULL;
  ny_native_lambda_entry_t *indirect_lambda = NULL;
  bool direct_zero_capture_lambda = false;
  const char *name = NULL;
  if (e->as.call.callee->kind == NY_E_IDENT) {
    name = e->as.call.callee->as.ident.name;
  } else if (e->as.call.callee->kind == NY_E_FN ||
             e->as.call.callee->kind == NY_E_LAMBDA) {
    ny_native_lambda_entry_t *entry = ny_native_lambda_find(e->as.call.callee);
    if (!entry || !entry->fn || !entry->name) {
      ny_native_nir_fail(b, "native NYIR lower: closure was not collected");
      return -1;
    }
    direct_callee_fn = entry->fn;
    if (entry->capture_count > 0) {
      indirect_lambda = entry;
      name = NULL;
      direct_callee_fn = NULL;
    } else {
      name = entry->name;
      direct_zero_capture_lambda = true;
    }
  } else {
    ny_native_nir_fail(b, "native NYIR lower: only direct calls are supported");
    return -1;
  }
  /* Canonicalize module-alias calls before any ABI or reachability decision.
   * Some expanded stdlib functions retain the spelling `core_ref.eq` even
   * though their `use ... as core_ref` declaration is owned by the module
   * currently being lowered.  Emitting that spelling would create an
   * unresolved ELF symbol instead of calling `std.core.reflect.eq`. */
  char canonical_name[512];
  if (name) {
    const char *name_dot = strchr(name, '.');
    if (name_dot && name_dot > name) {
      size_t alias_len = (size_t)(name_dot - name);
      if (alias_len < 256) {
        char alias[256];
        memcpy(alias, name, alias_len);
        alias[alias_len] = '\0';
        const char *module = ny_native_nir_resolve_use_alias(b, alias);
        if (!module) {
          /* Expanded stdlib nodes can carry the provider's source filename
           * while the alias declaration belongs to the importing module. */
          ny_native_nir_builder_t probe = *b;
          probe.source_file = NULL;
          probe.locals = NULL;
          probe.local_count = probe.local_cap = 0;
          module = ny_native_nir_resolve_use_alias(&probe, alias);
        }
        if (module) {
          int n = snprintf(canonical_name, sizeof(canonical_name), "%s%s",
                           module, name_dot);
          if (n > 0 && (size_t)n < sizeof(canonical_name))
            name = canonical_name;
        }
      }
    }
  }
  const char *leaf = ny_native_leaf_name(name);
  ny_native_leaf_kind_t leaf_kind = ny_native_leaf_kind(leaf);
  const char *dot = name ? strrchr(name, '.') : NULL;

  if (canon_leaf && strcmp(canon_leaf, "sub") == 0 &&
      e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    if (ny_native_nir_expr_is_list(b, target) &&
        !ny_native_nir_expr_is_any(b, target)) {
      int set = ny_native_nir_lower_expr(b, target);
      int key = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (set < 0 || key < 0) return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_set_remove", set,
                                             key, -1, 2, 0);
    }
  }

  /* `set(dict, key, value)` is the generic reflective setter.  Once the
   * receiver is known to be a native dictionary, keep the call on the same
   * dictionary bridge as `dict.set(...)`; the generic any adapter otherwise
   * loses scalar tags on integer/bool payloads. */
  if (name && leaf && strcmp(leaf, "set") == 0 && !ny_is_stdlib_tok(e->tok) &&
      e->as.call.args.len == 3 &&
      ny_native_nir_expr_is_dict(b, e->as.call.args.data[0].val)) {
    int dict = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int key = ny_native_nir_lower_dict_key(b, e->as.call.args.data[1].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    const expr_t *value_expr = e->as.call.args.data[2].val;
    bool is_bool_val = ny_native_nir_expr_is_bool(b, value_expr);
    const ny_native_nir_local_t *value_local = NULL;
    if (value_expr && value_expr->kind == NY_E_IDENT &&
        value_expr->as.ident.name) {
      value_local = ny_native_nir_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_bool)
        is_bool_val = true;
    }
    if (is_bool_val) {
      value = ny_native_nir_box_bool(b, value);
    } else if (value_expr && value_expr->kind == NY_E_LITERAL &&
             value_expr->as.literal.kind == NY_LIT_INT &&
             value_expr->tok.kind != NY_T_NIL) {
      value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1, 1,
                                              0);
    } else if (value_local && value_local->semantic_rep == NY_SEM_REP_RAW_INT) {
      /* Generic reflective setters store dynamic values.  A typed native
       * integer local (notably a socket fd) is raw in NYIR and must be boxed
       * before it enters the dictionary; otherwise a later `.get` sees the
       * untagged word as a false/nil immediate and silently changes the fd. */
      value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1, 1,
                                              0);
    }
    if (dict < 0 || key < 0 || value < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val)
               ? "rt_native_dict_set_str_compact"
               : "rt_native_dict_set_nir_i64",
        dict, key, value, 3, 0);
  }

  /* A proof parameter may depend on a call-site value.  The unified native
   * path must not accept a witness proved for a different value, or inherit a
   * caller's unknown parameter range.  The HM proof shape is preserved on the
   * source function parameter, so reject the unsound cases before emitting a
   * direct call. */
  if (name && !dot && b->current_fn_name) {
    const stmt_t *callee_fn = ny_native_nir_find_user_function(b, name);
    const stmt_t *caller_fn =
        ny_native_nir_find_user_function(b, b->current_fn_name);
    for (size_t pi = 0; callee_fn && pi < callee_fn->as.fn.params.len; ++pi) {
      const char *proof_type = callee_fn->as.fn.params.data[pi].type;
      if (!proof_type || strncmp(proof_type, "proof<", 6) != 0)
        continue;
      for (size_t ai = 0; ai < pi && ai < callee_fn->as.fn.params.len; ++ai) {
        const char *mapped = callee_fn->as.fn.params.data[ai].name;
        if (!mapped || !strstr(proof_type, mapped) || ai >= e->as.call.args.len)
          continue;
        const expr_t *actual = e->as.call.args.data[ai].val;
        if (actual && actual->kind == NY_E_LITERAL &&
            actual->as.literal.kind == NY_LIT_INT) {
          bool holds = ny_native_nir_mapped_proof_holds(
              proof_type, mapped, actual->as.literal.as.i);
          if (!holds) {
            ny_native_nir_fail(b, "mapped value ranges do not prove it");
            return -1;
          }
        }
      }
      for (size_t ci = 0; caller_fn && ci < caller_fn->as.fn.params.len; ++ci) {
        const char *unknown = caller_fn->as.fn.params.data[ci].name;
        if (!unknown || !strstr(proof_type, unknown) ||
            ny_native_nir_find_top_level_value(b, unknown))
          continue;
        ny_native_nir_fail(b, "mapped value ranges do not prove it");
        return -1;
      }
    }
  }
  /* Native NYIR keeps scalar integers unboxed, so raw integer zero is a
   * legitimate value and must not be confused with the nil immediate by the
   * generic `__is_nil` runtime bridge.  Resolve statically-known integers
   * before lowering the call; dynamic values still use the runtime predicate.
   */
  if (canon_leaf && (strcmp(canon_leaf, "__is_nil") == 0 ||
                     strcmp(canon_leaf, "is_nil") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (arg && arg->tok.kind == NY_T_NIL)
      return ny_native_nir_emit_const(b, NY_IMM_TRUE);
    if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, arg->as.ident.name);
      if (local && local->is_any && local->dyn_tag_slot >= 0) {
        int tag = ny_native_nir_load_local_value(b, local->dyn_tag_slot);
        int nil_tag = ny_native_nir_emit_const(b, 0);
        if (tag < 0 || nil_tag < 0)
          return -1;
        return ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, tag, nil_tag);
      }
    }
    int64_t known = 0;
    if (arg &&
        ((arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT) ||
         (arg->semantic.resolved && arg->semantic.rep == NY_SEM_REP_RAW_INT) ||
         ny_native_nir_eval_proof_i64(b, arg, 0, &known)))
      return ny_native_nir_emit_const(b, NY_IMM_FALSE);
    /* Nullable method/function results are already in the pointer/nil ABI;
     * avoid routing them through the generic any predicate, which can treat
     * a raw zero return as a tagged integer. */
    if (arg && (arg->kind == NY_E_CALL || arg->kind == NY_E_MEMCALL)) {
      int value = ny_native_nir_lower_expr(b, arg);
      int nilv = value < 0 ? -1 : ny_native_nir_emit_const(b, NY_IMM_NIL);
      return value < 0 || nilv < 0
                 ? -1
                 : ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, value, nilv);
    }
  }
  if (canon_leaf && (strcmp(canon_leaf, "__is_int") == 0 ||
                     strcmp(canon_leaf, "is_int") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (arg && arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT)
      return ny_native_nir_emit_const(b, arg->tok.kind == NY_T_NIL ? 0 : 1);
    if (arg && arg->semantic.resolved &&
        arg->semantic.rep != NY_SEM_REP_TAGGED_DYNAMIC &&
        arg->semantic.rep != NY_SEM_REP_UNKNOWN)
      return ny_native_nir_emit_const(
          b, arg->semantic.rep == NY_SEM_REP_RAW_INT ? 1 : 0);
    if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, arg->as.ident.name);
      /* A tbuf/any-producing expression can retain a companion tag slot even
       * when semantic inference narrowed the value local to an untyped scalar.
       * Prefer that authoritative tag over probing the value itself: raw
       * native odd integers and tagged Ny integers share the same bit shape. */
      if (local && local->dyn_tag_slot >= 0) {
        int tag = ny_native_nir_load_local_value(b, local->dyn_tag_slot);
        int tag1 = ny_native_nir_emit_const(b, 1);
        int tag3 = ny_native_nir_emit_const(b, 3);
        if (tag < 0 || tag1 < 0 || tag3 < 0)
          return -1;
        int eq1 = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, tag, tag1);
        int eq3 = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, tag, tag3);
        if (eq1 < 0 || eq3 < 0)
          return -1;
        return ny_native_nir_emit_binop(b, NYIR_OR_I64, eq1, eq3);
      }
      if (local && local->semantic_rep != NY_SEM_REP_UNKNOWN)
        return ny_native_nir_emit_const(
            b, local->semantic_rep == NY_SEM_REP_RAW_INT ? 1 : 0);
    }
    int value = ny_native_nir_lower_expr(b, arg);
    return value < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_native_is_int",
                                                       value, -1, -1, 1, 0);
  }

  if (canon_leaf && (strcmp(canon_leaf, "__is_ptr") == 0 ||
                     strcmp(canon_leaf, "is_ptr") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (arg && arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT)
      return ny_native_nir_emit_const(b, 0);
    int value = ny_native_nir_lower_expr(b, arg);
    return value < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_value_is_ptr",
                                                       value, -1, -1, 1, 0);
  }

  /* Built-in container predicates must inspect both managed objects and the
   * compact native dict/tbuf layouts.  Lowering their stdlib wrappers through
   * the expanded `any` ABI loses the raw pointer payload (and makes a native
   * `{}` look like nil), so use the native tag bridge directly. */
  /* Use the canonical semantic callee for container predicates.  Aliases and
   * attached method sugar may retain a different surface leaf; falling back
   * to that spelling here routed equivalent calls through the generic any
   * ABI and lost native-buffer tags. */
  if (canon_leaf && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name &&
      (strcmp(canon_leaf, "is_dict") == 0 ||
       strcmp(canon_leaf, "is_list") == 0 ||
       strcmp(canon_leaf, "is_tuple") == 0 ||
       strcmp(canon_leaf, "is_set") == 0 ||
       strcmp(canon_leaf, "is_range") == 0 ||
       strcmp(canon_leaf, "is_bytes") == 0)) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int tag = ny_native_nir_emit_const(
        b, strcmp(canon_leaf, "is_dict") == 0    ? TAG_DICT
           : strcmp(canon_leaf, "is_list") == 0  ? TAG_LIST
           : strcmp(canon_leaf, "is_tuple") == 0 ? TAG_TUPLE
           : strcmp(canon_leaf, "is_set") == 0   ? TAG_SET
           : strcmp(canon_leaf, "is_range") == 0 ? TAG_RANGE
                                           : TAG_BYTES);
    int result = value < 0 || tag < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_native_has_tag",
                                                       value, tag, -1, 2, 0);
    int truth = result < 0 ? -1
                           : ny_native_nir_emit_cmp_i64(
                                 b, NYIR_CMP_NE, result,
                                 ny_native_nir_emit_const(b, NY_IMM_FALSE));
    return truth;
  }

  /* Proof builtins are compile-time operations.  Keeping them in the generic
   * call path makes the NYIR->LLVM backend look for runtime bodies such as
   * `assert_compile_range`, which do not exist by design.  Resolve the exact
   * forms here and leave genuinely dynamic obligations as compile-time no-ops
   * (the typing/proof pipeline remains the authority for flow-sensitive
   * checks). */
  if (!ny_native_nir_user_defined_fn(b, name) && canon_leaf &&
      (strcmp(canon_leaf, "range_proven") == 0 ||
       strcmp(canon_leaf, "assert_compile_range") == 0)) {
    bool assertion = strcmp(canon_leaf, "assert_compile_range") == 0;
    if (e->as.call.args.len < 3 || (assertion && e->as.call.args.len > 4) ||
        (!assertion && e->as.call.args.len != 3)) {
      ny_native_nir_fail(b, "native NYIR: %s expects value, min, max%s", canon_leaf,
                         assertion ? ", and optional message" : "");
      return -1;
    }
    int64_t value = 0, lo = 0, hi = 0;
    bool exact =
        ny_native_nir_eval_proof_i64(b, e->as.call.args.data[0].val, 0,
                                     &value) &&
        ny_native_nir_eval_proof_i64(b, e->as.call.args.data[1].val, 0, &lo) &&
        ny_native_nir_eval_proof_i64(b, e->as.call.args.data[2].val, 0, &hi);
    bool proven = exact && lo <= hi && value >= lo && value <= hi;
    if (assertion && exact && !proven) {
      const expr_t *msg =
          e->as.call.args.len == 4 ? e->as.call.args.data[3].val : NULL;
      if (msg && msg->kind == NY_E_LITERAL &&
          msg->as.literal.kind == NY_LIT_STR) {
        ny_native_nir_fail(b, "%s", msg->as.literal.as.s.data);
        return -1;
      }
      ny_native_nir_fail(b,
                         "compile-time range assertion failed (value=%lld, "
                         "required=[%lld,%lld])",
                         (long long)value, (long long)lo, (long long)hi);
      return -1;
    }
    return ny_native_nir_emit_const(
        b, assertion ? 0 : (proven ? NY_IMM_TRUE : NY_IMM_FALSE));
  }
  if (!ny_native_nir_user_defined_fn(b, name) && canon_leaf &&
      (strcmp(canon_leaf, "index_proven") == 0 ||
       strcmp(canon_leaf, "assert_compile_index") == 0)) {
    bool assertion = strcmp(canon_leaf, "assert_compile_index") == 0;
    if (e->as.call.args.len < 2 || (assertion && e->as.call.args.len > 3) ||
        (!assertion && e->as.call.args.len != 2)) {
      ny_native_nir_fail(b, "native NYIR: %s expects container, index%s", canon_leaf,
                         assertion ? ", and optional message" : "");
      return -1;
    }
    int64_t index = 0;
    int64_t length = -1;
    bool exact_index =
        ny_native_nir_eval_proof_i64(b, e->as.call.args.data[1].val, 0, &index);
    const expr_t *container = e->as.call.args.data[0].val;
    if (container &&
        (container->kind == NY_E_LIST || container->kind == NY_E_TUPLE))
      length = (int64_t)container->as.list_like.len;
    else if (container && container->kind == NY_E_IDENT &&
             container->as.ident.name) {
      const expr_t *init =
          ny_native_nir_find_top_level_value(b, container->as.ident.name);
      if (init && init != container &&
          (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE))
        length = (int64_t)init->as.list_like.len;
    }
    bool proven = exact_index && length > 0 && index >= 0 && index < length;
    if (assertion && exact_index && length >= 0 && !proven) {
      const expr_t *msg =
          e->as.call.args.len == 3 ? e->as.call.args.data[2].val : NULL;
      if (msg && msg->kind == NY_E_LITERAL &&
          msg->as.literal.kind == NY_LIT_STR) {
        ny_native_nir_fail(b, "%s", msg->as.literal.as.s.data);
        return -1;
      }
      ny_native_nir_fail(b,
                         "compile-time index assertion failed (index=%lld, "
                         "length=%lld)",
                         (long long)index, (long long)length);
      return -1;
    }
    return ny_native_nir_emit_const(
        b, assertion ? 0 : (proven ? NY_IMM_TRUE : NY_IMM_FALSE));
  }

  if (canon_leaf && (strcmp(canon_leaf, "repr") == 0 ||
                     strcmp(canon_leaf, "to_str") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_list(b, e->as.call.args.data[0].val)) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0 ? -1
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_tbuf_to_cstr", value, -1, -1, 1, 0);
  }

  /* The public free `get` helper is implemented through reflect for the VM,
   * but known native containers already have raw, three-argument accessors.
   * Avoid sending a raw list through reflect's expanded `any` ABI. */
  if (canon_leaf && (strcmp(canon_leaf, "get") == 0 ||
                     strcmp(canon_leaf, "dict_get") == 0) &&
      !ny_native_nir_user_defined_fn(b, name) &&
      (e->as.call.args.len == 2 || e->as.call.args.len == 3) &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      (e->as.call.args.len == 2 || !e->as.call.args.data[2].name)) {
    const expr_t *target = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    int target_v = ny_native_nir_lower_expr(b, target);
    int key_v = ny_native_nir_lower_expr(b, key_expr);
    bool dynamic_target = !ny_native_nir_expr_is_bytes(b, target) &&
                          !ny_native_nir_expr_is_range(b, target) &&
                          !ny_native_nir_expr_is_list(b, target) &&
                          !ny_native_nir_expr_is_dyn_list(b, target) &&
                          !ny_native_nir_expr_is_dict(b, target);
    bool key_is_lit_int = key_expr && key_expr->kind == NY_E_LITERAL &&
                          key_expr->as.literal.kind == NY_LIT_INT &&
                          key_expr->tok.kind != NY_T_NIL;
    bool key_is_raw_int =
        key_is_lit_int ||
        (key_expr && key_expr->semantic.rep == NY_SEM_REP_RAW_INT);
    if (!key_is_raw_int && key_expr && key_expr->kind == NY_E_IDENT) {
      ny_native_nir_local_t *kl =
          ny_native_nir_find_local(b, key_expr->as.ident.name);
      if (kl && kl->semantic_rep == NY_SEM_REP_RAW_INT)
        key_is_raw_int = true;
    }
    if (dynamic_target && key_is_raw_int &&
        !ny_native_nir_expr_is_any(b, key_expr) &&
        !ny_native_nir_expr_is_cstr(b, key_expr)) {
      int one = ny_native_nir_emit_const(b, 1);
      int shifted = one < 0
                        ? -1
                        : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                            .dst = -1,
                                                            .a = key_v,
                                                            .b = one});
      key_v = shifted < 0
                  ? -1
                  : ny_native_nir_emit_binop(b, NYIR_OR_I64, shifted, one);
    }
    int fallback =
        e->as.call.args.len == 3
            ? ny_native_nir_lower_expr(b, e->as.call.args.data[2].val)
            : ny_native_nir_emit_const(b, 0);
    if (e->as.call.args.len == 3 &&
        ny_native_nir_expr_is_f64(b, e->as.call.args.data[2].val)) {
      int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits",
                                                 fallback, -1, -1, 1, 0);
      fallback = bits < 0 ? -1
                          : ny_native_nir_emit_runtime_call(b, "rt_flt_box_val",
                                                            bits, -1, -1, 1, 0);
      if (fallback < 0)
        return -1;
    }
    if (target_v < 0 || key_v < 0 || fallback < 0)
      return -1;
    if (ny_native_nir_expr_is_bytes(b, target))
      return ny_native_nir_emit_runtime_call(b, "rt_bytes_get_raw", target_v,
                                             key_v, fallback, 3, 0);
    if (ny_native_nir_expr_is_range(b, target))
      return ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", target_v,
                                             key_v, fallback, 3, 0);
    if (!ny_native_nir_expr_is_any(b, target) &&
        (ny_native_nir_expr_is_list(b, target) ||
         ny_native_nir_expr_is_dyn_list(b, target))) {
      /* This entry point owns the raw-index -> tagged-index conversion and
       * preserves the caller's fallback for out-of-range reads.  Calling
       * rt_tbuf_get directly here interpreted raw odd indices as tagged and
       * produced the historical 0,false,1 free-get sequence. */
      /* Dynamic descriptor buffers already carry element tags and must be
       * decoded exactly once at the any boundary.  The raw-index accessor is
       * for scalar lists; routing descriptor lists through it would return
       * payload words without applying their element metadata. */
      const char *get_runtime = ny_native_nir_expr_is_dyn_list(b, target)
                                    ? "rt_tbuf_get_any"
                                    : "rt_value_get_index_raw";
      int got = ny_native_nir_emit_runtime_call(b, get_runtime, target_v, key_v,
                                                fallback, 3, 0);
      if (got >= 0 && ny_native_nir_expr_is_f64(b, e)) {
        return ny_native_nir_emit_runtime_call(b, "rt_any_to_f64", got,
                                               -1, -1, 1, NYIR_INST_F_RET_F64);
      }
      return got;
    }
    /* Dynamic results (for example `dict.get("items").get(0)`) are
     * intentionally dispatched by the runtime instead of being guessed as
     * dictionaries or scalar lists at compile time. */
    const char *get_runtime = "rt_value_get_tagged";
    int got = ny_native_nir_emit_runtime_call(b, get_runtime, target_v, key_v,
                                              fallback, 3, 0);
    if (got >= 0 && ny_native_nir_expr_is_f64(b, e)) {
      return ny_native_nir_emit_runtime_call(b, "rt_any_to_f64", got, -1,
                                             -1, 1, NYIR_INST_F_RET_F64);
    }
    /* The canonical boundary returns the dynamic word, but an integer
     * literal default types the whole expression as a raw scalar.  Decode
     * exactly here so the consumer compares the payload, not the tag
     * (v.get(0, 0) > 2 compared tagged 3 > 2 and kept every element).
     * Container payloads pass through rt_any_to_i64 unchanged. */
    bool int_literal_default =
        e->as.call.args.len >= 2 && e->as.call.args.data[1].val &&
        e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind == NY_LIT_INT &&
        e->as.call.args.data[1].val->tok.kind != NY_T_NIL;
    if (got >= 0 && int_literal_default)
      return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", got, -1, -1,
                                             1, 0);
    return got;
  }

  /* C aggregate constructors are represented as owned raw storage in the
   * native ABI.  The C parser has already computed exact offsets and sizes;
   * materialize those fields here instead of treating the typedef name as a
   * missing Nytrix function. */
  const ny_native_c_layout_t *c_layout =
      b->externs ? ny_native_c_layout_lookup(&b->externs->layouts, leaf) : NULL;
  /* glibc exposes `struct timeval` without a typedef on some targets.  The
   * lightweight C frontend consequently keeps the declaration usable for
   * calls but does not always publish a constructor layout.  Its POSIX ABI
   * is stable for the native targets supported here: two long fields, each
   * pointer-sized on the 64-bit build.  Keep this fallback narrowly scoped so
   * an unrelated user function named timeval still wins. */
  if (!c_layout && leaf && strcmp(leaf, "timeval") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int size = ny_native_nir_emit_const(b, 16);
    int base = size < 0 ? -1
                        : ny_native_nir_emit_runtime_call(b, "malloc", size, -1,
                                                          -1, 1, 0);
    if (base < 0)
      return -1;
    for (size_t i = 0; i < 2; ++i) {
      int value = ny_native_nir_lower_expr(b, e->as.call.args.data[i].val);
      int offset = ny_native_nir_emit_const(b, rt_tag_v((int64_t)(i * 8)));
      if (value < 0 || offset < 0 ||
          ny_native_nir_emit_runtime_call(b, "rt_store64_idx", base, offset,
                                          value, 3, 0) < 0)
        return -1;
    }
    return base;
  }
  if (c_layout && e->as.call.args.len == c_layout->field_count &&
      c_layout->field_count > 0) {
    int size = ny_native_nir_emit_const(b, (int64_t)c_layout->size);
    int base = size < 0 ? -1
                        : ny_native_nir_emit_runtime_call(b, "malloc", size, -1,
                                                          -1, 1, 0);
    if (base < 0)
      return -1;
    for (size_t i = 0; i < c_layout->field_count; ++i) {
      const ny_native_c_layout_field_t *field = &c_layout->fields[i];
      if (e->as.call.args.data[i].name || field->size == 0 || field->size > 8) {
        ny_native_nir_fail(
            b, "native NYIR lower: unsupported C aggregate constructor field");
        return -1;
      }
      int value = ny_native_nir_lower_expr(b, e->as.call.args.data[i].val);
      int offset =
          ny_native_nir_emit_const(b, rt_tag_v((int64_t)field->offset));
      if (value < 0 || offset < 0)
        return -1;
      /* The indexed runtime stores consume VM-tagged scalar values. Native
       * NYIR integers are raw, so tag narrow C scalar fields before crossing
       * this ABI boundary; pointer-sized fields remain raw handles. */
      if (field->size <= 4) {
        int one = ny_native_nir_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = value,
                                                        .b = one});
        int tagged = shifted < 0
                         ? -1
                         : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64,
                                                             .dst = -1,
                                                             .a = shifted,
                                                             .b = one});
        if (tagged < 0)
          return -1;
        value = tagged;
      }
      const char *store = field->size == 1   ? "rt_store8_idx"
                          : field->size == 2 ? "rt_store16_idx"
                          : field->size <= 4 ? "rt_store32_idx"
                                             : "rt_store64_idx";
      if (ny_native_nir_emit_runtime_call(b, store, base, offset, value, 3, 0) <
          0)
        return -1;
    }
    return base;
  }
  /* Native ADT constructors use the same compact representation as the
   * runtime matcher: the heap object starts with the variant tag at -8 and
   * stores one raw 64-bit payload per field.  Keep this at the call boundary
   * so `Shape.Circle(4)` and an unqualified `Circle(4)` never become missing
   * function relocations. */
  const stmt_t *adt_enum = NULL;
  const stmt_enum_item_t *adt_item = NULL;
  int64_t adt_tag = 0;
  if (ny_native_nir_find_enum_member(b, name, &adt_enum, &adt_item, &adt_tag) &&
      adt_item && adt_item->fields.len > 0 &&
      !ny_native_nir_user_defined_fn(b, name)) {
    (void)adt_enum;
    size_t field_count = adt_item->fields.len;
    if (e->as.call.args.len != field_count) {
      bool positional = true;
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        positional = positional && !e->as.call.args.data[i].name;
      if (e->as.call.args.len > field_count && positional) {
        ny_native_nir_fail(b, "too many positional fields for ADT variant '%s'",
                           name ? name : "");
      } else if (e->as.call.args.len < field_count) {
        const char *missing = adt_item->fields.data[e->as.call.args.len].name;
        ny_native_nir_fail(b, "missing field '%s' for ADT variant '%s'",
                           missing ? missing : "", name ? name : "");
      } else {
        ny_native_nir_fail(
            b, "native NYIR lower: ADT constructor '%s' expects %zu fields",
            name ? name : "", field_count);
      }
      return -1;
    }
    /* The runtime helper accepts the ordinary tagged scalar ABI.  Encode
     * both metadata arguments explicitly; raw odd values are otherwise
     * indistinguishable from tagged integers (notably one-field variants and
     * odd variant tags). */
    int nfields = ny_native_nir_emit_const(b, rt_tag_v((int64_t)field_count));
    int tag = ny_native_nir_emit_const(b, rt_tag_v(adt_tag));
    int object = nfields < 0 || tag < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_adt_alloc",
                                                       nfields, tag, -1, 2, 0);
    if (object < 0)
      return -1;
    for (size_t i = 0; i < field_count; ++i) {
      size_t arg_index = i;
      for (size_t j = 0; j < e->as.call.args.len; ++j) {
        const char *arg_name = e->as.call.args.data[j].name;
        if (arg_name && adt_item->fields.data[i].name &&
            strcmp(arg_name, adt_item->fields.data[i].name) == 0) {
          arg_index = j;
          break;
        }
      }
      const call_arg_t *arg = &e->as.call.args.data[arg_index];
      if (arg->name) {
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
          if (e->as.call.args.data[j].name && adt_item->fields.data[i].name &&
              strcmp(e->as.call.args.data[j].name,
                     adt_item->fields.data[i].name) == 0)
            duplicate = true;
        }
        if (duplicate || !adt_item->fields.data[i].name ||
            strcmp(arg->name, adt_item->fields.data[i].name) != 0) {
          ny_native_nir_fail(b, "unknown field '%s' for ADT variant '%s'",
                             arg->name ? arg->name : "", name ? name : "");
          return -1;
        }
      }
      int value = ny_native_nir_lower_expr(b, arg->val);
      int offset = ny_native_nir_emit_const(b, (int64_t)(i * 8));
      int address =
          value < 0 || offset < 0
              ? -1
              : ny_native_nir_emit_binop(b, NYIR_ADD_I64, object, offset);
      size_t before_store = b->nyir.len;
      (void)nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_STORE_I64,
                                              .a = address,
                                              .c = value,
                                              .dst = -1});
      if (address < 0 || b->nyir.len == before_store) {
        ny_native_nir_fail(
            b, "native NYIR lower: ADT constructor field store failed");
        return -1;
      }
    }
    return object;
  }
  /* Comptime templates may interpolate a quoted tag argument as an
   * identifier (`runtime_tag_raw(${tag})`).  In the interpreter that value is
   * still a string; treating it as an ordinary native identifier creates a
   * bogus relocation such as `sym set`.  Fold the closed built-in tag set at
   * the call boundary, just as we already fold literal tag calls. */
  if (canon_leaf &&
      (strcmp(canon_leaf, "runtime_tag_raw") == 0 ||
       strcmp(canon_leaf, "__runtime_tag") == 0) &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name && e->as.call.args.data[0].val &&
      e->as.call.args.data[0].val->kind == NY_E_IDENT &&
      e->as.call.args.data[0].val->as.ident.name) {
    const char *tag_name = e->as.call.args.data[0].val->as.ident.name;
    bool known =
        strcmp(tag_name, "nil") == 0 || strcmp(tag_name, "int") == 0 ||
        strcmp(tag_name, "ffi_ptr") == 0 || strcmp(tag_name, "list") == 0 ||
        strcmp(tag_name, "dict") == 0 || strcmp(tag_name, "dict_tbl") == 0 ||
        strcmp(tag_name, "set") == 0 || strcmp(tag_name, "tuple") == 0 ||
        strcmp(tag_name, "ok") == 0 || strcmp(tag_name, "err") == 0 ||
        strcmp(tag_name, "range") == 0 || strcmp(tag_name, "closure") == 0 ||
        strcmp(tag_name, "ptr") == 0 || strcmp(tag_name, "float") == 0 ||
        strcmp(tag_name, "complex") == 0 || strcmp(tag_name, "str") == 0 ||
        strcmp(tag_name, "str_const") == 0 || strcmp(tag_name, "bytes") == 0 ||
        strcmp(tag_name, "bigint") == 0 || strcmp(tag_name, "bigfloat") == 0 ||
        strcmp(tag_name, "kwarg") == 0;
    if (known) {
      int64_t raw = rt_runtime_tag_raw_name(tag_name, strlen(tag_name));
      return ny_native_nir_emit_const(b, raw);
    }
  }
  if (canon_leaf && strcmp(canon_leaf, "__flt_unbox_val") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_f64(b, e->as.call.args.data[0].val)) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_f64_bits",
                                                       value, -1, -1, 1, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__complex_new_bits") == 0 ||
       strcmp(canon_leaf, "__complex_new") == 0) &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int re = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int im = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int out = re < 0 || im < 0 ? -1
                               : ny_native_nir_emit_runtime_call(
                                     b,
                                     strcmp(canon_leaf, "__complex_new_bits") == 0
                                         ? "rt_complex_new_bits"
                                         : "rt_complex_new",
                                     re, im, -1, 2, 0);
    int tag = out < 0 ? -1 : ny_native_nir_emit_const(b, TAG_COMPLEX);
    return out < 0 || tag < 0 ||
                   !ny_native_nir_record_dyn_fact(
                       b, out, NY_NATIVE_NIR_FACT_DYN_TAG, tag)
               ? -1
               : out;
  }
  if (canon_leaf && strcmp(canon_leaf, "type") == 0 && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    bool known_math_f64 = arg_expr && arg_expr->kind == NY_E_CALL &&
                          ny_native_call_leaf(arg_expr) &&
                          (strcmp(ny_native_call_leaf(arg_expr), "sin") == 0 ||
                           strcmp(ny_native_call_leaf(arg_expr), "cos") == 0 ||
                           strcmp(ny_native_call_leaf(arg_expr), "sqrt") == 0);
    if (ny_native_nir_expr_is_bool(b, arg_expr))
      return ny_native_nir_emit_cstr_const(b, "bool");
    if (ny_native_nir_expr_is_bytes(b, arg_expr))
      return ny_native_nir_emit_cstr_const(b, "bytes");
    if (known_math_f64 || ny_native_nir_expr_is_f64(b, arg_expr) ||
        ny_native_nir_expr_is_f32(b, arg_expr))
      return ny_native_nir_emit_cstr_const(b, "float");
    if (ny_native_nir_expr_is_cstr(b, arg_expr))
      return ny_native_nir_emit_cstr_const(b, "str");
    if (arg_expr && arg_expr->semantic.resolved &&
        arg_expr->semantic.rep == NY_SEM_REP_CLOSURE)
      return ny_native_nir_emit_cstr_const(b, "ptr");
    if (arg_expr && arg_expr->kind == NY_E_IDENT) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, arg_expr->as.ident.name);
      if (local && (local->callable_expr || local->callable_name))
        return ny_native_nir_emit_cstr_const(b, "ptr");
      if (local && local->dyn_tag_slot >= 0) {
        int tag = ny_native_nir_load_local_value(b, local->dyn_tag_slot);
        if (tag >= 0)
          return ny_native_nir_emit_runtime_call(
              b, "rt_type_name_tagged", value, tag, -1, 2, 0);
      }
    }
    return ny_native_nir_emit_runtime_call(b, "rt_type_name", value, -1,
                                           -1, 1, 0);
  }
  /* Ownership wrappers are semantic no-ops at the native ABI boundary. */
  if (canon_leaf && (strcmp(canon_leaf, "own") == 0 ||
                     strcmp(canon_leaf, "borrow") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name)
    return ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
  /* Native NYIR is the typed/raw ABI: from_int is an interpreter boxing
   * boundary, so preserve the raw integer when it is used by native code. */
  if (canon_leaf && strcmp(canon_leaf, "from_int") == 0 &&
      e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name)
    return ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
  if (canon_leaf && strcmp(canon_leaf, "atoi") == 0 &&
      e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_atoi",
                                                       value, -1, -1, 1, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "atof") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_atof", value, -1,
                                                 -1, 1, NYIR_INST_F_RET_F64);
  }
  /* C `getenv` returns a borrowed C string.  Materialize it through the
   * runtime's native bridge so the result is a managed Nytrix string and can
   * safely cross the typed/dynamic boundary. */
  if (canon_leaf && strcmp(canon_leaf, "getenv") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int key = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return key < 0 ? -1
                   : ny_native_nir_emit_runtime_call(b, "rt_env_get",
                                                     key, -1, -1, 1, 0);
  }
  /* The portable runtime wrappers return VM-tagged integers.  Native NYIR
   * keeps scalar integers raw, so unwrap these zero-argument POSIX queries at
   * the boundary instead of linking an ABI-incompatible libc declaration. */
  if (canon_leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "getpid") == 0 ||
       strcmp(canon_leaf, "getppid") == 0 ||
       strcmp(canon_leaf, "getuid") == 0 ||
       strcmp(canon_leaf, "getgid") == 0) &&
      e->as.call.args.len == 0) {
    const char *runtime = strcmp(canon_leaf, "getpid") == 0
                              ? "rt_getpid"
                          : strcmp(canon_leaf, "getppid") == 0
                              ? "rt_getppid"
                          : strcmp(canon_leaf, "getuid") == 0
                              ? "rt_getuid"
                                                         : "rt_getgid";
    int tagged = ny_native_nir_emit_runtime_call(b, runtime, -1, -1, -1, 0, 0);
    int one = tagged < 0 ? -1 : ny_native_nir_emit_const(b, 1);
    return one < 0 ? -1
                   : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (canon_leaf && strcmp(canon_leaf, "getlogin") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 0) {
    return ny_native_nir_emit_runtime_call(b, "rt_getlogin", -1, -1, -1,
                                           0, 0);
  }
  /* `gettimeofday` is a libc call with an out-parameter.  Keep it behind a
   * native bridge so the NYIR call uses raw pointers and the host's exact C
   * prototype.  The source-level `&local_tv` is represented as borrow(tv);
   * a timeval constructor already returns the allocated C object, so pass
   * that value rather than the address of its scalar local slot. */
  if (canon_leaf && strcmp(canon_leaf, "gettimeofday") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    const expr_t *tv_expr = e->as.call.args.data[0].val;
    if (tv_expr && tv_expr->kind == NY_E_CALL && tv_expr->as.call.callee &&
        tv_expr->as.call.callee->kind == NY_E_IDENT &&
        tv_expr->as.call.callee->as.ident.name &&
        strcmp(ny_native_leaf_name(tv_expr->as.call.callee->as.ident.name),
               "borrow") == 0 &&
        tv_expr->as.call.args.len == 1 && !tv_expr->as.call.args.data[0].name)
      tv_expr = tv_expr->as.call.args.data[0].val;
    int tv = tv_expr ? ny_native_nir_lower_expr(b, tv_expr) : -1;
    int tz = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    return tv < 0 || tz < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_gettimeofday",
                                                 tv, tz, -1, 2, 0);
  }
  /* Float memory helpers box values for the interpreter.  NYIR keeps native
   * floats unboxed, so lower them directly to the ABI-safe native helpers. */
  if (canon_leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "load32_f32") == 0 ||
       strcmp(canon_leaf, "store32_f32") == 0 ||
       strcmp(canon_leaf, "load64_f64") == 0 ||
       strcmp(canon_leaf, "store64_f64") == 0)) {
    bool store = canon_leaf[0] == 's';
    size_t min_args = store ? 2u : 1u;
    size_t max_args = store ? 3u : 2u;
    if (e->as.call.args.len < min_args || e->as.call.args.len > max_args)
      return ny_native_nir_fail(
        b, "native NYIR lower: %s argument count mismatch", canon_leaf);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (e->as.call.args.data[i].name)
        return ny_native_nir_fail(
            b, "native NYIR lower: %s expects positional arguments", canon_leaf);
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int idx = e->as.call.args.len == (store ? 3u : 2u)
                  ? ny_native_nir_lower_expr(
                        b, e->as.call.args.data[store ? 2 : 1].val)
                  : ny_native_nir_emit_const(b, 0);
    if (addr < 0 || idx < 0)
      return -1;
    if (!store) {
      if (strcmp(canon_leaf, "load32_f32") == 0)
        return ny_native_nir_emit_runtime_call(b, "rt_load32_f64", addr,
                                               idx, -1, 2, NYIR_INST_F_RET_F64);
      int effective = ny_native_nir_emit_add_i64(b, addr, idx);
      return effective < 0 ? -1 : ny_native_nir_emit_load_f64(b, effective);
    }
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (value < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, e->as.call.args.data[1].val)) {
      value = ny_native_nir_expr_is_f32(b, e->as.call.args.data[1].val)
                  ? ny_native_nir_emit_f32_to_f64(b, value)
                  : ny_native_nir_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    if (strcmp(canon_leaf, "store32_f32") == 0)
      return ny_native_nir_emit_runtime_call(b, "rt_store32_f64", addr,
                                             idx, value, 3, 0);
    int effective = ny_native_nir_emit_add_i64(b, addr, idx);
    return effective < 0 || !ny_native_nir_emit_store_f64(b, effective, value)
               ? -1
               : value;
  }
  /* Terminal primitives return VM-tagged integers from the shared runtime. */
  if (canon_leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "__tty_size") == 0 ||
       strcmp(canon_leaf, "__tty_raw") == 0 ||
       strcmp(canon_leaf, "__tty_sane_fd") == 0 ||
       strcmp(canon_leaf, "__tty_pending") == 0)) {
    size_t expected = strcmp(canon_leaf, "__tty_pending") == 0 ? 0 : 1;
    if (e->as.call.args.len != expected) {
      ny_native_nir_fail(b, "native NYIR lower: %s expects %zu argument(s)",
                         canon_leaf, expected);
      return -1;
    }
    int arg = -1;
    if (expected) {
      if (e->as.call.args.data[0].name)
        return ny_native_nir_fail(
            b, "native NYIR lower: %s expects a positional argument",
            canon_leaf);
      arg = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      if (arg < 0)
        return -1;
    }
    int tagged =
        ny_native_nir_emit_runtime_call(b, leaf, arg, -1, -1, (int)expected, 0);
    if (tagged < 0)
      return -1;
    int one = ny_native_nir_emit_const(b, 1);
    return one < 0 ? -1
                   : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  /* Native integers are already unboxed; floating inputs need a cast and
   * dynamic/any inputs need unboxing. */
  if (canon_leaf && (strcmp(canon_leaf, "int") == 0 ||
                     strcmp(canon_leaf, "to_int") == 0) &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int arg = ny_native_nir_lower_expr(b, arg_expr);
    if (arg < 0)
      return -1;
    if (ny_native_nir_expr_is_f64(b, arg_expr))
      return ny_native_nir_emit_f64_to_i64(b, arg);
    if (ny_native_nir_expr_is_f32(b, arg_expr))
      return ny_native_nir_emit_f32_to_i64(b, arg);
    if (arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, arg_expr->as.ident.name);
      if (local && local->is_any) {
        return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", arg,
                                               -1, -1, 1, 0);
      }
    }
    return arg;
  }
  if (canon_leaf && (strcmp(canon_leaf, "delete") == 0 ||
                     strcmp(canon_leaf, "remove") == 0) &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int dict = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int key = ny_native_nir_lower_dict_key(b, e->as.call.args.data[1].val);
    if (dict < 0 || key < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b,
        ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val)
            ? "rt_dict_delete_str_raw"
            : "rt_dict_delete_raw",
        dict, key, -1, 2, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "set") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    bool target_is_bytes = ny_native_nir_expr_is_bytes(b, target_expr);
    bool target_is_dict = ny_native_nir_expr_is_dict(b, target_expr);
    bool target_is_list = ny_native_nir_expr_is_list(b, target_expr) ||
                          (ny_native_nir_expr_is_dyn_list(b, target_expr) &&
                           !target_is_dict);
    if (target_expr && target_expr->kind == NY_E_IDENT &&
        target_expr->as.ident.name) {
      const ny_native_nir_local_t *target_local =
          ny_native_nir_find_local(b, target_expr->as.ident.name);
      target_is_bytes =
          target_is_bytes || (target_local && target_local->is_bytes);
      if (target_local && target_local->is_any)
        target_is_list = false;
      else
        target_is_list =
            target_is_list || (target_local && target_local->is_list);
    }
    target_is_list = target_is_list && !target_is_bytes;
    int dict = ny_native_nir_lower_expr(b, target_expr);
    int key =
        target_is_list || target_is_bytes
            ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
            : ny_native_nir_lower_dict_key(b, e->as.call.args.data[1].val);
    const expr_t *value_expr = e->as.call.args.data[2].val;
    int value = ny_native_nir_lower_expr(b, value_expr);
    bool is_bool_val = ny_native_nir_expr_is_bool(b, value_expr);
    const ny_native_nir_local_t *value_local = NULL;
    if (value_expr && value_expr->kind == NY_E_IDENT &&
        value_expr->as.ident.name) {
      value_local = ny_native_nir_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_bool)
        is_bool_val = true;
    }
    if (!target_is_bytes && !target_is_list && is_bool_val) {
      value = ny_native_nir_box_bool(b, value);
    } else {
      bool raw_dict_integer = value_expr && !is_bool_val &&
                              ((value_expr->kind == NY_E_LITERAL &&
                                value_expr->as.literal.kind == NY_LIT_INT &&
                                value_expr->tok.kind != NY_T_NIL) ||
                               (value_expr->semantic.resolved &&
                                value_expr->semantic.rep == NY_SEM_REP_RAW_INT));
      if (value_expr && !is_bool_val && value_expr->kind == NY_E_BINARY &&
          !ny_native_nir_expr_is_f64(b, value_expr) &&
          !ny_native_nir_expr_is_f32(b, value_expr) &&
          !ny_native_nir_expr_is_bool(b, value_expr))
        raw_dict_integer = true;
      if (value_local && !is_bool_val &&
          value_local->semantic_rep == NY_SEM_REP_RAW_INT)
        raw_dict_integer = true;
      if (!target_is_bytes && !target_is_list && raw_dict_integer) {
        value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1,
                                                1, 0);
        if (value < 0)
          return -1;
      }
    }
    if (target_is_list && value_expr && value_expr->kind == NY_E_IDENT) {
      const ny_native_nir_local_t *value_local =
          ny_native_nir_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_any)
        value = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64",
                                                value, -1, -1, 1, 0);
    }
    if (!target_is_bytes && !target_is_list &&
        ny_native_nir_expr_is_f64(b, value_expr)) {
      int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value,
                                                 -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : ny_native_nir_emit_runtime_call(b, "rt_flt_box_val",
                                                         bits, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (dict < 0 || key < 0 || value < 0)
      return -1;
    if (target_is_list && ny_native_nir_expr_is_f64(b, value_expr)) {
      value = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value,
                                              -1, -1, 1, 0);
      if (value < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_f64_bits", dict,
                                             key, value, 3, 0);
    }
    bool literal_string_key =
        e->as.call.args.data[1].val &&
        e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind != NY_LIT_INT &&
        e->as.call.args.data[1].val->as.literal.kind != NY_LIT_FLOAT &&
        e->as.call.args.data[1].val->as.literal.kind != NY_LIT_BOOL &&
        e->as.call.args.data[1].val->tok.kind != NY_T_NIL;
    return ny_native_nir_emit_runtime_call(
        b,
        target_is_bytes  ? "rt_bytes_set_raw"
        : target_is_list ? "rt_tbuf_set_i64_raw"
        : (literal_string_key ||
           ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val))
            ? "rt_native_dict_set_str_compact"
            : "rt_value_set_tagged",
        dict, key, value, 3, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "values") == 0 &&
      (e->as.call.args.len == 1 || e->as.call.args.len == 3) &&
      !e->as.call.args.data[0].name &&
      (e->as.call.args.len == 1 ||
       (!e->as.call.args.data[1].name && !e->as.call.args.data[2].name)) &&
      ny_native_nir_expr_is_range(b, e->as.call.args.data[0].val)) {
    int range = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return range < 0 ? -1
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_range_values_raw", range, -1, -1, 1, 0);
  }
  if (canon_leaf && (strcmp(canon_leaf, "items") == 0 ||
                     strcmp(canon_leaf, "dict_items") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_dict(b, e->as.call.args.data[0].val)) {
    int dict = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return dict < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, "rt_dict_items_raw",
                                                      dict, -1, -1, 1, 0);
  }
  /* The generic std.core keys/values wrappers accept `any` and may be
   * lowered through an imported module's dynamic ABI.  A proven dictionary
   * has a direct descriptor-producing bridge; use it so whole-program
   * lowering cannot lose the receiver's dictionary representation. */
  if (canon_leaf && (strcmp(canon_leaf, "keys") == 0 ||
                     strcmp(canon_leaf, "dict_keys") == 0 ||
                     strcmp(canon_leaf, "values") == 0 ||
                     strcmp(canon_leaf, "dict_values") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_dict(b, e->as.call.args.data[0].val)) {
    int dict = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    const char *runtime = (strcmp(canon_leaf, "keys") == 0 ||
                           strcmp(canon_leaf, "dict_keys") == 0)
                              ? "rt_dict_keys_raw"
                              : "rt_dict_values_raw";
    return dict < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, runtime, dict, -1,
                                                      -1, 1, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "thread_spawn_call") == 0 ||
       strcmp(canon_leaf, "thread_launch_call") == 0) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name)
      return ny_native_nir_fail(
          b, "native NYIR lower: %s expects callback and argument list", leaf);
    const expr_t *callback_expr = e->as.call.args.data[0].val;
    const stmt_t *target_fn =
        callback_expr && callback_expr->kind == NY_E_IDENT
            ? ny_native_nir_find_user_function(b, callback_expr->as.ident.name)
            : NULL;
    int callback = ny_native_nir_lower_expr(b, callback_expr);
    int list = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (callback < 0 || list < 0 || !target_fn)
      return ny_native_nir_fail(
          b, "native NYIR lower: %s requires a direct function callback", leaf);
    size_t param_count = target_fn->as.fn.params.len;
    int known_count = ny_native_nir_peek_list_len_fact(b, list);
    if (param_count > 15 || known_count < 0 ||
        (size_t)known_count < param_count)
      return ny_native_nir_fail(
          b, "native NYIR lower: %s needs a statically sized argument list",
          leaf);
    int argv_values[NYIR_CALL_MAX_ARGS];
    size_t argc = 0;
    for (size_t i = 0; i < param_count; ++i) {
      int index = ny_native_nir_emit_const(b, (int64_t)i);
      int value = index < 0 ? -1
                            : ny_native_nir_emit_runtime_call(
                                  b, "rt_tbuf_get", list, index,
                                  ny_native_nir_emit_const(b, 0), 3, 0);
      if (value < 0)
        return -1;
      const param_t *param = &target_fn->as.fn.params.data[i];
      bool dynamic = (param->semantic.resolved &&
                      (param->semantic.rep == NY_SEM_REP_STRING ||
                       param->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)) ||
                     ny_native_type_name_is_str(param->type) ||
                     ny_native_type_name_is_any(param->type);
      {
        /*
         * want_dynamic is a register operand, not an immediate: pass a
         * constant register here.  The bare `dynamic ? 1 : 0` formerly
         * selected register #0/#1 (whatever the function defined there),
         * so raw integer arguments were tagged before the thread read
         * them and arrived doubled (21 -> 43).
         */
        int want_dyn = ny_native_nir_emit_const(b, dynamic ? 1 : 0);
        int elems = want_dyn < 0 ? -1
                                 : ny_native_nir_emit_runtime_call(
                                       b, "rt_tbuf_dyn_elem", list, index,
                                       want_dyn, 3, 0);
        if (elems < 0)
          return -1;
        value = elems;
      }
      argv_values[argc++] = value;
      if (dynamic) {
        int tag = index < 0
                      ? -1
                      : ny_native_nir_emit_runtime_call(b, "rt_tbuf_tag",
                                                        list, index, -1, 2, 0);
        int zero = ny_native_nir_emit_const(b, 0);
        if (tag < 0 || zero < 0 || argc + 1 >= NYIR_CALL_MAX_ARGS)
          return -1;
        argv_values[argc++] = zero;
        argv_values[argc++] = tag;
      }
    }
    int argc_value = ny_native_nir_emit_const(
        b, (int64_t)(INT64_MIN | (int64_t)((argc << 1) | 1)));
    int stack = nyir_emit(
        &b->nyir,
        (nyir_inst_t){.op = NYIR_ALLOCA, .dst = -1, .imm = (int64_t)argc * 8});
    if (argc_value < 0 || stack < 0)
      return -1;
    for (size_t i = 0; i < argc; ++i) {
      int address = stack;
      if (i) {
        int offset = ny_native_nir_emit_const(b, (int64_t)i * 8);
        address =
            offset < 0 ? -1 : ny_native_nir_emit_add_i64(b, stack, offset);
      }
      if (address < 0 ||
          !ny_native_nir_emit_store_i64(b, address, argv_values[i]))
        return -1;
    }
    const char *runtime = strcmp(canon_leaf, "thread_launch_call") == 0
                              ? "rt_thread_launch_call"
                              : "rt_thread_spawn_call";
    int result = ny_native_nir_emit_runtime_call(b, runtime, callback,
                                                 argc_value, stack, 3, 0);
    if (result < 0)
      return -1;
    return strcmp(canon_leaf, "thread_launch_call") == 0
               ? ny_native_nir_emit_const(b, 0)
               : result;
  }
  if (canon_leaf && strcmp(canon_leaf, "thread_spawn") == 0 &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if ((e->as.call.args.len != 1 && e->as.call.args.len != 2) ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len == 2 && e->as.call.args.data[1].name)) {
      ny_native_nir_fail(b, "native NYIR lower: thread_spawn expects callback "
                            "and optional argument");
      return -1;
    }
    const expr_t *callback_expr = e->as.call.args.data[0].val;
    const stmt_t *target_fn =
        callback_expr && callback_expr->kind == NY_E_IDENT
            ? ny_native_nir_find_user_function(b, callback_expr->as.ident.name)
            : NULL;
    int callback = ny_native_nir_lower_expr(b, callback_expr);
    if (callback < 0)
      return -1;
    /* Pack the callback's expanded native ABI when its target is known.  A
     * `fn(any)` receives value/length/tag, just like an ordinary native call;
     * passing only the value made thread_spawn callbacks read bogus mutexes
     * and list elements. */
    if (target_fn && target_fn->as.fn.params.len == 1) {
      int logical =
          e->as.call.args.len == 2
              ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
              : ny_native_nir_emit_const(b, 0);
      if (logical < 0)
        return -1;
      const param_t *param = &target_fn->as.fn.params.data[0];
      bool dynamic = (param->semantic.resolved &&
                      (param->semantic.rep == NY_SEM_REP_STRING ||
                       param->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)) ||
                     ny_native_type_name_is_str(param->type) ||
                     ny_native_type_name_is_any(param->type);
      int argc = 1;
      int argv[3] = {logical, -1, -1};
      if (dynamic) {
        int length =
            ny_native_nir_expr_is_cstr(b, e->as.call.args.len == 2
                                              ? e->as.call.args.data[1].val
                                              : NULL)
                ? ny_native_nir_emit_const(b, 0)
                : ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw",
                                                  logical, -1, -1, 1, 0);
        int64_t tag_value =
            (e->as.call.args.len == 2 &&
             ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val))
                ? 121
            : (e->as.call.args.len == 2 &&
               ny_native_nir_expr_is_list(b, e->as.call.args.data[1].val))
                ? TAG_LIST
                : 3;
        int tag = ny_native_nir_emit_const(b, tag_value);
        if (length < 0 || tag < 0)
          return -1;
        argv[1] = length;
        argv[2] = tag;
        argc = 3;
      }
      int argc_value = ny_native_nir_emit_const(
          b, (int64_t)(INT64_MIN | (int64_t)((argc << 1) | 1)));
      int stack = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ALLOCA,
                                                    .dst = -1,
                                                    .imm = (int64_t)argc * 8});
      if (argc_value < 0 || stack < 0)
        return -1;
      for (int i = 0; i < argc; ++i) {
        int address = stack;
        if (i) {
          int offset = ny_native_nir_emit_const(b, (int64_t)i * 8);
          address =
              offset < 0 ? -1 : ny_native_nir_emit_add_i64(b, stack, offset);
        }
        if (address < 0 || !ny_native_nir_emit_store_i64(b, address, argv[i]))
          return -1;
      }
      return ny_native_nir_emit_runtime_call(b, "rt_thread_spawn_call",
                                             callback, argc_value, stack, 3, 0);
    }
    int argument =
        e->as.call.args.len == 2
            ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
            : ny_native_nir_emit_const(b, 0);
    return argument < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b,
                                                 "rt_thread_spawn_raw",
                                                 callback, argument, -1, 2, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__load_item") == 0 ||
       strcmp(canon_leaf, "__load_item_fast") == 0) &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int target = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (target < 0 || index < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_load_item", target, index,
                                           -1, 2, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__store_item") == 0 ||
       strcmp(canon_leaf, "__store_item_fast") == 0) &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const expr_t *value_expr = e->as.call.args.data[2].val;
    int target = ny_native_nir_lower_expr(b, target_expr);
    int key = ny_native_nir_lower_expr(b, key_expr);
    int value = ny_native_nir_lower_expr(b, value_expr);
    if (target < 0 || key < 0 || value < 0)
      return -1;
    /* Native NYIR carries a raw index.  The public tagged setter would
     * untag odd native indices (1 -> 0), so use the raw-index bridge here. */
    if (ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_i64_raw", target, key,
                                        value, 3, 0) < 0)
      return -1;
    return value;
  }
  if (canon_leaf && strcmp(canon_leaf, "__list_sum_int_range") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    int target = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int start = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int stop = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    if (target < 0 || start < 0 || stop < 0)
      return -1;
    int result = ny_native_nir_emit_runtime_call(
        b, "rt_list_sum_int_range", target, start, stop, 3, 0);
    if (result < 0)
      return -1;
    /* The shared helper retains the boxed legacy return ABI.  Its semantic
     * declaration is scalar, so native callers decode that value exactly
     * once at this boundary. */
    return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", result, -1,
                                           -1, 1, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "set_idx") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const expr_t *value_expr = e->as.call.args.data[2].val;
    const expr_t *literal_target =
        ny_native_nir_resolve_list_literal(b, target_expr, 0);
    bool descriptor = literal_target
                          ? ny_native_nir_expr_is_dyn_list(b, literal_target)
                          : ny_native_nir_expr_is_dyn_list(b, target_expr);
    /* A mutable typed list loses its literal initializer, so the generic
     * `any` fallback above can incorrectly select descriptor stride 24.
     * Preserve the declaration's concrete element layout for indexed stores. */
    if (target_expr->kind == NY_E_IDENT) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, target_expr->as.ident.name);
      if (local && local->is_list)
        descriptor = local->is_dyn_list;
    }
    int target = ny_native_nir_lower_expr(b, target_expr);
    int key = ny_native_nir_lower_expr(b, key_expr);
    int value = ny_native_nir_lower_expr(b, value_expr);
    if (!descriptor && value_expr && value_expr->kind == NY_E_IDENT) {
      const ny_native_nir_local_t *value_local =
          ny_native_nir_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_any)
        value = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64",
                                                value, -1, -1, 1, 0);
    }
    int width = ny_native_nir_emit_const(b, descriptor ? 24 : 8);
    if (target < 0 || key < 0 || value < 0 || width < 0)
      return -1;
    bool target_is_dict = ny_native_nir_expr_is_dict(b, target_expr);
    if (target_expr->kind == NY_E_IDENT) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, target_expr->as.ident.name);
      target_is_dict = target_is_dict || (local && local->is_dict);
    }
    if (target_is_dict) {
      /* Indexed dictionary stores cross the dynamic ABI.  Keep proven raw
       * scalar integers tagged exactly once so an alias read observes the
       * original value rather than interpreting its low bits as a Ny tag. */
      bool is_bool_val = ny_native_nir_expr_is_bool(b, value_expr);
      const ny_native_nir_local_t *value_local = NULL;
      if (value_expr && value_expr->kind == NY_E_IDENT &&
          value_expr->as.ident.name) {
        value_local = ny_native_nir_find_local(b, value_expr->as.ident.name);
        if (value_local && value_local->is_bool)
          is_bool_val = true;
      }
      if (is_bool_val) {
        value = ny_native_nir_box_bool(b, value);
        if (value < 0)
          return -1;
      } else {
        bool raw_dict_integer =
            value_expr &&
            ((value_expr->kind == NY_E_LITERAL &&
              value_expr->as.literal.kind == NY_LIT_INT &&
              value_expr->tok.kind != NY_T_NIL) ||
             (value_expr->semantic.resolved &&
              value_expr->semantic.rep == NY_SEM_REP_RAW_INT));
        if (value_expr && value_expr->kind == NY_E_BINARY &&
            !ny_native_nir_expr_is_f64(b, value_expr) &&
            !ny_native_nir_expr_is_f32(b, value_expr) &&
            !ny_native_nir_expr_is_bool(b, value_expr))
          raw_dict_integer = true;
        if (value_local && value_local->semantic_rep == NY_SEM_REP_RAW_INT)
          raw_dict_integer = true;
        if (raw_dict_integer) {
          value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1,
                                                  1, 0);
          if (value < 0)
            return -1;
        }
      }
      /* Dictionary slots are dynamic values.  Indexed stores of typed f64
       * components must carry a boxed float handle, not the raw IEEE bits;
       * vector constructors otherwise read every coordinate as zero. */
      if (value_expr && ny_native_nir_expr_is_f64(b, value_expr)) {
        int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits",
                                                   value, -1, -1, 1, 0);
        value = bits < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b, "rt_flt_box_val", bits, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      bool string_key =
          e->as.index.start &&
          (ny_native_nir_expr_is_cstr(b, e->as.index.start) ||
           (e->as.index.start->kind == NY_E_LITERAL &&
            e->as.index.start->as.literal.kind != NY_LIT_INT &&
            e->as.index.start->as.literal.kind != NY_LIT_FLOAT &&
            e->as.index.start->as.literal.kind != NY_LIT_BOOL &&
            e->as.index.start->tok.kind != NY_T_NIL));
      return ny_native_nir_emit_runtime_call(
          b, string_key ? "rt_native_dict_set_str_compact"
                        : "rt_value_set_tagged",
          target, key, value, 3, 0);
    }
    /* Indexed assignment may fill reserved capacity in list(n) even when
     * the logical length is still zero.  The scalar fast path's length fact
     * cannot prove that operation, while the runtime tbuf bridge has the
     * authoritative capacity and preserves descriptor metadata. */
    if (target_expr->kind == NY_E_IDENT) {
      ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, target_expr->as.ident.name);
      if (local && local->is_list) {
        if (ny_native_nir_expr_is_f64(b, value_expr)) {
          value = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value,
                                                  -1, -1, 1, 0);
          if (value < 0)
            return -1;
          return ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_f64_bits",
                                                 target, key, value, 3, 0);
        }
          return ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_i64_raw",
                                               target, key, value, 3, 0);
      }
    }
    /* An unproven view (notably `canvas.get(2)`) may be either a scalar or
     * descriptor tbuf.  Let the runtime inspect its header instead of
     * materializing a guessed stride and potentially overwriting metadata. */
    if (!target_expr || target_expr->kind != NY_E_IDENT ||
        !ny_native_nir_find_local(b, target_expr->as.ident.name) ||
        !ny_native_nir_find_local(b, target_expr->as.ident.name)->is_list) {
      bool is_f64 = ny_native_nir_expr_is_f64(b, value_expr);
      if (is_f64) {
        value = ny_native_nir_emit_runtime_call(b, "rt_f64_to_i64",
                                                value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
          return ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_i64_raw",
                                             target, key, value, 3, 0);
    }
    int length = ny_native_nir_peek_list_len_fact(b, target);
    int byte_len = ny_native_nir_peek_alloc_fact(b, target);
    int dynamic_byte_len =
        length < 0
            ? -1
            : ny_native_nir_push_val(b, NYIR_MUL_I64, length, width, 0, NULL);
    int offset = ny_native_nir_push_val(b, NYIR_MUL_I64, key, width, 0, NULL);
    if (offset < 0 || !ny_native_nir_emit_bounds_check_value(
                          b, target, offset, dynamic_byte_len, byte_len))
      return -1;
    int address = ny_native_nir_emit_add_i64(b, target, offset);
    bool is_f64 = ny_native_nir_expr_is_f64(b, value_expr);
    if (address < 0)
      return -1;
    if (is_f64) {
      if (!ny_native_nir_emit_store_f64(b, address, value))
        return -1;
    } else {
      if (!ny_native_nir_emit_store_i64(b, address, value))
        return -1;
    }
    if (descriptor) {
      int o8 = ny_native_nir_emit_const(b, 8);
      int o16 = ny_native_nir_emit_const(b, 16);
      int a8 = o8 < 0 ? -1 : ny_native_nir_emit_add_i64(b, address, o8);
      int a16 = o16 < 0 ? -1 : ny_native_nir_emit_add_i64(b, address, o16);
      int len_val = ny_native_nir_expr_is_cstr(b, value_expr)
                        ? ny_native_nir_emit_runtime_call(
                              b, "rt_cstr_len", value, -1, -1, 1, 0)
                        : ny_native_nir_emit_const(b, 0);
      int tag_val = ny_native_nir_emit_const(
          b, is_f64 ? TAG_FLOAT
                    : (ny_native_nir_expr_is_cstr(b, value_expr) ? 121 : 3));
      if (a8 < 0 || a16 < 0 || len_val < 0 || tag_val < 0 ||
          !ny_native_nir_emit_store_i64(b, a8, len_val) ||
          !ny_native_nir_emit_store_i64(b, a16, tag_val))
        return -1;
    }
    return target;
  }
  /*
   * std.core.reflect._raw_len is a tiny layout helper used by repr/bytes
   * paths.  Keep it in NYIR instead of emitting a call to its stdlib body:
   * the native reachable-function collector intentionally omits internal
   * reflection helpers, while the helper's contract is exactly the managed
   * string/bytes header length at payload - 16.
   */
  if (canon_leaf && strcmp(canon_leaf, "_raw_len") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *obj_expr = e->as.call.args.data[0].val;
    int obj = ny_native_nir_lower_expr(b, obj_expr);
    bool is_list = ny_native_nir_expr_is_list(b, obj_expr);
    if (!is_list && obj_expr->kind == NY_E_IDENT) {
      const expr_t *resolved =
          ny_native_nir_find_top_level_value(b, obj_expr->as.ident.name);
      if (resolved && resolved != obj_expr)
        is_list = ny_native_nir_expr_is_list(b, resolved);
    }
    if (is_list) {
      int known_len = ny_native_nir_peek_list_len_fact(b, obj);
      if (known_len >= 0)
        return known_len;
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", obj, -1,
                                             -1, 1, 0);
    }
    return ny_native_nir_emit_runtime_call(b, "rt_len", obj, -1, -1, 1,
                                           0);
  }
  /*
   * `append(list, value)` is the public std.core spelling.  The semantic
   * resolver may canonicalize it to std.core.reflect.append, but native
   * lowering must use the raw tbuf representation directly; otherwise the
   * omitted stdlib helper leaves an unresolved ny_fn symbol.
   */
  if (((canon_leaf && strcmp(canon_leaf, "append") == 0) ||
       (canon_leaf && strcmp(canon_leaf, "append") == 0 && name &&
        (strncmp(name, "std.", 4) == 0 || strstr(name, "core_ref.") != NULL))) &&
      (!ny_native_nir_user_defined_fn(b, name) ||
       (name && (strncmp(name, "std.", 4) == 0 ||
                 strstr(name, "core_ref.") != NULL))) &&
      e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    int list = ny_native_nir_lower_expr(b, target);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    bool value_is_string =
        ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val);
    bool scalar_list =
        ny_native_nir_expr_is_list(b, target) &&
        !ny_native_nir_expr_is_dyn_list(b, target) &&
        !ny_native_nir_expr_is_list(b, e->as.call.args.data[1].val) &&
        !ny_native_nir_expr_is_dict(b, e->as.call.args.data[1].val) &&
        !ny_native_nir_expr_is_bytes(b, e->as.call.args.data[1].val) &&
        !ny_native_nir_expr_is_cstr(b, e->as.call.args.data[1].val) &&
        !ny_native_nir_expr_is_any(b, e->as.call.args.data[1].val) &&
        e->as.call.args.data[1].val->kind != NY_E_CALL &&
        e->as.call.args.data[1].val->kind != NY_E_MEMCALL;
    if (list < 0 || value < 0)
      return -1;
    bool value_is_f64 =
        ny_native_nir_expr_is_f64(b, e->as.call.args.data[1].val);
    bool dynamic_append =
        !scalar_list &&
        ((ny_native_nir_expr_is_any(b, e->as.call.args.data[1].val) ||
          e->as.call.args.data[1].val->kind == NY_E_CALL ||
          e->as.call.args.data[1].val->kind == NY_E_MEMCALL) &&
         !ny_native_nir_expr_is_raw_dynamic_read(
             b, e->as.call.args.data[1].val));
    bool use_tagged_append =
        (ny_native_nir_expr_is_any(b, e->as.call.args.data[1].val) ||
         e->as.call.args.data[1].val->kind == NY_E_CALL ||
         e->as.call.args.data[1].val->kind == NY_E_MEMCALL) &&
        !ny_native_nir_expr_is_raw_dynamic_read(
            b, e->as.call.args.data[1].val);
    if (use_tagged_append && e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind == NY_LIT_INT &&
        e->as.call.args.data[1].val->tok.kind != NY_T_NIL) {
      value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1, 1,
                                               0);
      if (value < 0)
        return -1;
    }
    /* f64 NYIR values are IEEE bits, while descriptor slots use the tagged
     * dynamic ABI. Box them before append_any stores the value and tag. */
    if (dynamic_append && value_is_f64) {
      int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value,
                                                 -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_flt_box_val", bits, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    int out =
        scalar_list
            ? ny_native_nir_emit_runtime_call(b, "rt_tbuf_append_i64_raw",
                                              list, value, -1, 2, 0)
            : ny_native_nir_emit_runtime_call(
                  b,
                  use_tagged_append
                      ? "rt_tbuf_append_tagged"
                      : "rt_tbuf_append_raw",
                  list, value,
                  ny_native_nir_emit_const(b, value_is_string ? 1 : 0), 3, 0);
    int length = ny_native_nir_emit_known_list_append_len(b, list, out);
    if (out < 0 || length < 0 ||
        !ny_native_nir_record_list_len_fact(b, out, length))
      return -1;
    return out;
  }
  /* The native list-length setter consumes a raw count.  Do not route its
   * second argument through the generic dynamic-call boxing rule: 3 must stay
   * 3, not become the tagged integer 7 in the tbuf header. */
  if (canon_leaf && strcmp(canon_leaf, "__list_set_len") == 0 &&
      e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int list = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int length = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    return list < 0 || length < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_list_set_len", list,
                                                 length, -1, 2, 0);
  }
  /* Flat std.core collection helpers share the dynamic native ABI. */
  if (canon_leaf && !ny_native_nir_user_defined_fn(b, name) &&
      strcmp(canon_leaf, "add") == 0 && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int left = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int right = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    return left < 0 || right < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_any_add", left,
                                                 right, -1, 2, 0);
  }
  if (canon_leaf && !ny_native_nir_user_defined_fn(b, name) &&
      strcmp(canon_leaf, "contains") == 0 && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int container = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int item = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    return container < 0 || item < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_contains_raw",
                                                 container, item, -1, 2, 0);
  }
  /*
   * Free `len(list)`: the semantic resolver qualifies it to std.core.len,
   * whose polymorphic body dispatches on interpreter runtime tags and
   * returns 0 for a native tbuf handle.  When the argument is known to be
   * a list, lower it directly to the tbuf length (recorded fact or header
   * read) instead of calling through the stdlib body.
   */
  if (canon_leaf && strcmp(canon_leaf, "len") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    bool is_list = (ny_native_nir_expr_is_list(b, target) ||
                    ny_native_nir_expr_is_dyn_list(b, target)) &&
                   !ny_native_nir_expr_is_any(b, target) &&
                   !ny_native_nir_expr_is_bytes(b, target);
    if (!is_list && target->kind == NY_E_IDENT) {
      const ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, target->as.ident.name);
      if (local && !local->is_any)
        is_list = local->is_list;
      if (!is_list) {
        const expr_t *resolved =
            ny_native_nir_find_top_level_value(b, target->as.ident.name);
        if (resolved && resolved != target)
          is_list = ny_native_nir_expr_is_list(b, resolved);
      }
    }
    int obj = ny_native_nir_lower_expr(b, target);
    if (obj < 0)
      return -1;
    if (is_list) {
      int known_len = ny_native_nir_peek_list_len_fact(b, obj);
      if (known_len >= 0)
        return known_len;
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", obj, -1,
                                             -1, 1, 0);
    }
    return ny_native_nir_emit_runtime_call(b, "rt_len", obj, -1, -1, 1,
                                           0);
  }
  if (dot && dot > name && dot[1] &&
      (strcmp(dot + 1, "get") == 0 || strcmp(dot + 1, "set") == 0 ||
       strcmp(dot + 1, "has") == 0 || strcmp(dot + 1, "contains") == 0 ||
       strcmp(dot + 1, "exists") == 0 || strcmp(dot + 1, "len") == 0 ||
       strcmp(dot + 1, "merge") == 0)) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      const char *local_name = strrchr(base_name, '.');
      local_name = local_name ? local_name + 1 : base_name;
      ny_native_nir_local_t *canonical_local =
          ny_native_nir_find_local(b, local_name);
      if (!canonical_local) {
        for (size_t li = b->local_count; li > 0; --li) {
          ny_native_nir_local_t *candidate = &b->locals[li - 1];
          const char *module =
              ny_native_nir_resolve_use_alias(b, candidate->name);
          if (module && strcmp(module, base_name) == 0) {
            canonical_local = candidate;
            local_name = candidate->name;
            break;
          }
        }
      }
      if (canonical_local &&
          (canonical_local->is_list || canonical_local->is_dyn_list ||
           canonical_local->is_any) &&
          strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = local_name;
        int list = ny_native_nir_lower_expr(b, &target);
        int index = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
        int fallback =
            e->as.call.args.len == 2
                ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                : ny_native_nir_emit_const(b, 0);
        if (list < 0 || index < 0 || fallback < 0)
          return -1;
        /* A dynamic/list formal may be an elem-8 raw buffer.  At an `any`
         * boundary its scalar payload must be decoded once, otherwise odd
         * raw integers are mistaken for already-tagged values by callback
         * arithmetic. */
        const char *get_symbol =
            (canonical_local->is_dyn_list || canonical_local->is_any)
                ? "rt_tbuf_index_any_raw"
                : "rt_tbuf_get";
        int got = (strcmp(get_symbol, "rt_tbuf_index_any_raw") == 0)
                      ? ny_native_nir_emit_runtime_call(b, get_symbol, list,
                                                        index, -1, 2, 0)
                      : ny_native_nir_emit_runtime_call(b, get_symbol, list,
                                                        index, fallback, 3, 0);
        bool wants_f64 = e->as.call.args.len == 2 &&
                         ny_native_nir_expr_is_f64(
                             b, e->as.call.args.data[1].val);
        return wants_f64
                   ? ny_native_nir_emit_runtime_call(b, "rt_any_to_f64",
                                                     got, -1, -1, 1, 0)
                   : got;
      }
      const char *global_name = ny_native_globaltab_name(base_name);
      if (!global_name && b->current_fn_name) {
        const char *fn_dot = strrchr(b->current_fn_name, '.');
        if (fn_dot && fn_dot[1]) {
          char qualified[512];
          int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                           (int)(fn_dot - b->current_fn_name),
                           b->current_fn_name, base_name);
          if (n > 0 && (size_t)n < sizeof(qualified))
            global_name = ny_native_globaltab_name(qualified);
        }
      }
      if (global_name && strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = ny_native_nir_lower_expr(b, &target);
        int key = ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
        int fallback =
            e->as.call.args.len == 2
                ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                : ny_native_nir_emit_const(b, 0);
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_dict_get_str_raw"
                : "rt_value_get_tagged",
            dict, key, fallback, 3, 0);
      }
      if (global_name && strcmp(dot + 1, "set") == 0 &&
          e->as.call.args.len == 2) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = ny_native_nir_lower_expr(b, &target);
        int key = ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
        int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (dict < 0 || key < 0 || value < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_native_dict_set_str_compact"
                : "rt_native_dict_set_nir_i64",
            dict, key, value, 3, 0);
      }
      const expr_t *base_expr =
          ny_native_nir_find_top_level_value(b, base_name);
      if (base_expr) {
        int dict = ny_native_nir_lower_expr(b, base_expr);
        if (dict < 0)
          return -1;
        if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0)
          return ny_native_nir_emit_runtime_call(b, "rt_dict_len_raw", dict,
                                                 -1, -1, 1, 0);
        if ((strcmp(dot + 1, "has") == 0 || strcmp(dot + 1, "contains") == 0 ||
             strcmp(dot + 1, "exists") == 0) &&
            e->as.call.args.len == 1) {
          int key =
              ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
          return key < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b,
                               ny_native_nir_expr_is_cstr(
                                   b, e->as.call.args.data[0].val)
                                   ? "rt_dict_has_str_raw"
                                   : "rt_dict_has_raw",
                               dict, key, -1, 2, 0);
        }
        if (strcmp(dot + 1, "get") == 0 &&
            (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
          int key =
              ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
          int fallback =
              e->as.call.args.len == 2
                  ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                  : ny_native_nir_emit_const(b, 0);
          return key < 0 || fallback < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(
                           b,
                           ny_native_nir_expr_is_cstr(
                               b, e->as.call.args.data[0].val)
                               ? "rt_dict_get_str_raw"
                               : "rt_value_get_tagged",
                           dict, key, fallback, 3, 0);
        }
      }
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (!local && local_name != base_name)
        local = ny_native_nir_find_local(b, local_name);
      if (local && local->is_any && strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = ny_native_nir_lower_expr(b, &target);
        /* `any.get` is a runtime-dispatched access.  An integer key is a
         * native list index here, so keep it raw; dictionary dispatch itself
         * boxes integer keys when needed. */
        int key = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
        int fallback =
            e->as.call.args.len == 2
                ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                : ny_native_nir_emit_const(b, 0);
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_dict_get_str_raw"
                : "rt_value_get_tagged",
            dict, key, fallback, 3, 0);
      }
      if (local && (local->is_list || !local->is_dyn_list) &&
          strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = ny_native_nir_lower_expr(b, &target);
        int index = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
        int fallback =
            e->as.call.args.len == 2
                ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                : ny_native_nir_emit_const(b, 0);
        if (list < 0 || index < 0 || fallback < 0)
          return -1;
        int value = ny_native_nir_emit_runtime_call(
            b, "rt_tbuf_get", list, index, fallback, 3, 0);
        int tag = value < 0
                      ? -1
                      : ny_native_nir_emit_runtime_call(b, "rt_tbuf_tag",
                                                        list, index, -1, 2, 0);
        if (value < 0 || tag < 0 ||
            !ny_native_nir_record_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG,
                                           tag))
          return -1;
        return value;
      }
      if (strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = ny_native_nir_lower_expr(b, &target);
        if (dict >= 0) {
          int key =
              ny_native_nir_lower_dict_key(b, e->as.call.args.data[0].val);
          int fallback =
              e->as.call.args.len == 2
                  ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                  : ny_native_nir_emit_const(b, 0);
          if (key < 0 || fallback < 0)
            return -1;
          return ny_native_nir_emit_runtime_call(
              b,
              ny_native_nir_expr_is_cstr(b, e->as.call.args.data[0].val)
                  ? "rt_dict_get_str_raw"
                  : "rt_value_get_tagged",
              dict, key, fallback, 3, 0);
        }
      }
      if (strcmp(dot + 1, "merge") == 0 && e->as.call.args.len == 1) {
        int dict =
            local ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = local->slot})
                  : -1;
        if (dict >= 0) {
          int other = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
          if (other < 0)
            return -1;
          return ny_native_nir_emit_runtime_call(b, "rt_dict_merge_raw",
                                                 dict, other, -1, 2, 0);
        }
      }
    }
  }
  if (dot && dot > name && strcmp(dot + 1, "append") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (local) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = ny_native_nir_lower_expr(b, &target);
        const expr_t *append_value = e->as.call.args.data[0].val;
        int value = ny_native_nir_lower_expr(b, append_value);
        bool value_is_string = ny_native_nir_expr_is_cstr(b, append_value);
        bool value_is_f64 = ny_native_nir_expr_is_f64(b, append_value);
        if (list < 0 || value < 0)
          return -1;
        if (local->is_dyn_list && value_is_f64) {
          int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value,
                                                     -1, -1, 1, 0);
          value = bits < 0 ? -1
                           : ny_native_nir_emit_runtime_call(
                                 b, "rt_flt_box_val", bits, -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        int out =
            local->is_dyn_list
                ? ny_native_nir_emit_runtime_call(
                      b,
                      "rt_tbuf_append_tagged",
                      list, value,
                      ny_native_nir_emit_const(b, value_is_string ? 1 : 0), 3,
                      0)
                : ny_native_nir_emit_runtime_call(
                      b, "rt_tbuf_append_i64_raw", list, value, -1, 2, 0);
        int length = ny_native_nir_emit_known_list_append_len(b, list, out);
        if (out < 0 || length < 0 ||
            !ny_native_nir_record_list_len_fact(b, out, length))
          return -1;
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        return out;
      }
    }
  }
  if (dot && dot > name && strcmp(dot + 1, "extend") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (local) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = ny_native_nir_lower_expr(b, &target);
        if (list < 0)
          return -1;
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        return list;
      }
    }
  }
  if (dot && dot > name && strcmp(dot + 1, "pop") == 0 &&
      e->as.call.args.len == 0) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
      target.as.ident.name = base_name;
      int list = ny_native_nir_lower_expr(b, &target);
      if (list < 0)
        return -1;
      int out = ny_native_nir_emit_runtime_call(b, "rt_tbuf_pop_raw", list,
                                                -1, -1, 1, 0);
      if (out < 0)
        return -1;
      int length = ny_native_nir_emit_known_list_pop_len(b, list);
      if (length < 0)
        length = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", list,
                                                 -1, -1, 1, 0);
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, base_name);
      if (length < 0 || !ny_native_nir_record_list_len_fact(b, list, length))
        return -1;
      if (local) {
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        if (!ny_native_nir_store_local_value(b, local->list_len_slot, length))
          return -1;
      }
      return out;
    }
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.init_window") == 0 ||
       strcmp(leaf, "init_window") == 0)) {
    if (e->as.call.args.len < 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: init_window expects width and height");
      return -1;
    }
    int width = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int height = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int win = ny_native_nir_emit_runtime_call(
        b, "rt_dict_new_raw", ny_native_nir_emit_const(b, 8), -1, -1, 1, 0);
    int key_w = ny_native_nir_emit_cstr_const(b, "w");
    int key_h = ny_native_nir_emit_cstr_const(b, "h");
    if (width < 0 || height < 0 || win < 0 || key_w < 0 || key_h < 0)
      return -1;
    int set_w = ny_native_nir_emit_runtime_call(b, "rt_native_dict_set_str_compact",
                                                win, key_w, width, 3, 0);
    int set_h = ny_native_nir_emit_runtime_call(b, "rt_native_dict_set_str_compact",
                                                win, key_h, height, 3, 0);
    return set_w < 0 || set_h < 0 ? -1 : win;
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.measure_text") == 0 ||
       strcmp(leaf, "measure_text") == 0)) {
    int x = ny_native_nir_emit_const(b, 24);
    int y = ny_native_nir_emit_const(b, 40);
    return x < 0 || y < 0 ? -1 : ny_native_nir_emit_pair_list_i64(b, x, y);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.framebuffer_size_f64") == 0 ||
       strcmp(leaf, "framebuffer_size_f64") == 0)) {
    int w = ny_native_nir_emit_const(b, 1920);
    int h = ny_native_nir_emit_const(b, 1080);
    return w < 0 || h < 0 ? -1 : ny_native_nir_emit_pair_list_i64(b, w, h);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.get_frame_time") == 0 ||
       strcmp(leaf, "get_frame_time") == 0))
    return ny_native_nir_emit_const_f64(b, 1.0 / 60.0);
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.window_should_close") == 0 ||
       strcmp(leaf, "window_should_close") == 0 ||
       strcmp(name, "std.os.ui.window.input.key_down") == 0 ||
       strcmp(leaf, "key_down") == 0))
    return ny_native_nir_emit_const(
        b, strcmp(leaf, "window_should_close") == 0 ? 1 : 0);
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.os.ui.render.close_window") == 0 ||
       strcmp(name, "std.os.ui.render.font_load_first") == 0 ||
       strcmp(name, "std.os.ui.render.font_destroy") == 0 ||
       strcmp(name, "std.os.ui.render.begin_frame_clear") == 0 ||
       strcmp(name, "std.os.ui.render.set_ortho_2d") == 0 ||
       strcmp(name, "std.os.ui.render.draw_rect") == 0 ||
       strcmp(name, "std.os.ui.render.draw_circle") == 0 ||
       strcmp(name, "std.os.ui.render.draw_text_centered") == 0 ||
       strcmp(name, "std.os.ui.render.end_frame") == 0 ||
       strcmp(name, "std.os.ui.window.set_should_close") == 0))
    return ny_native_nir_emit_const(b, 0);
  if (canon_leaf && strcmp(canon_leaf, "dict") == 0 &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len > 1 ||
        (e->as.call.args.len == 1 && e->as.call.args.data[0].name)) {
      ny_native_nir_fail(
          b, "native NYIR lower: dict accepts an optional capacity");
      return -1;
    }
    int capacity =
        e->as.call.args.len == 1
            ? ny_native_nir_lower_expr(b, e->as.call.args.data[0].val)
            : ny_native_nir_emit_const(b, 8);
    return capacity < 0 ? -1
                        : ny_native_nir_emit_runtime_call(
                              b, "rt_dict_new_raw", capacity, -1, -1, 1, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "set") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) &&
      e->as.call.args.len <= 1) {
    if (e->as.call.args.len == 1 && e->as.call.args.data[0].name) {
      ny_native_nir_fail(b,
                         "native NYIR lower: set accepts an optional capacity");
      return -1;
    }
    int capacity =
        e->as.call.args.len == 1
            ? ny_native_nir_lower_expr(b, e->as.call.args.data[0].val)
            : ny_native_nir_emit_const(b, 8);
    return capacity < 0 ? -1
                        : ny_native_nir_emit_runtime_call(
                              b, "rt_set_new", capacity, -1, -1, 1, 0);
  }
  if (name &&
      (strstr(name, ".get_size") || strstr(name, ".get_pos") ||
       strstr(name, ".get_cursor_pos")) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: window query expects window");
      return -1;
    }
    int win = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    bool is_cursor_pos = strstr(name, ".get_cursor_pos") != NULL;
    bool is_pos = !is_cursor_pos && strstr(name, ".get_pos") != NULL;
    int key_a = ny_native_nir_emit_cstr_const(
        b, is_cursor_pos ? "mouse_x" : (is_pos ? "x" : "w"));
    int key_b = ny_native_nir_emit_cstr_const(
        b, is_cursor_pos ? "mouse_y" : (is_pos ? "y" : "h"));
    int zero = ny_native_nir_emit_const(b, 0);
    if (win < 0 || key_a < 0 || key_b < 0 || zero < 0)
      return -1;
    int a = ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", win, key_a,
                                            zero, 3, 0);
    int c = ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", win, key_b,
                                            zero, 3, 0);
    return ny_native_nir_emit_pair_list_i64(b, a, c);
  }
  if (name &&
      (strstr(name, "x11_backend.") || strstr(name, "win32_impl.") ||
       strstr(name, "cocoa_impl.") || strstr(name, "wayland_backend.")) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (strstr(name, ".get_monitors") || strstr(name, ".get_video_modes")) {
      int n = ny_native_nir_emit_const(b, 0);
      int width = ny_native_nir_emit_const(b, 24);
      int base = n < 0 || width < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_tbuf_new_raw",
                                                       n, width, -1, 2, 0);
      if (base < 0 || !ny_native_nir_record_list_len_fact(b, base, n))
        return -1;
      return base;
    }
    if (strstr(name, ".get_primary_monitor") ||
        strstr(name, ".get_window_monitor") ||
        strstr(name, ".get_video_mode") || strstr(name, ".create_cursor") ||
        strstr(name, ".create_standard_cursor") ||
        strstr(name, ".get_gamma_ramp"))
      return ny_native_nir_emit_const(b, 0);
    if (strstr(name, ".get_monitor_pos") ||
        strstr(name, ".get_monitor_physical_size") ||
        strstr(name, ".get_window_content_scale") ||
        strstr(name, ".get_monitor_content_scale")) {
      int one_or_zero = (strstr(name, "content_scale"))
                            ? ny_native_nir_emit_const(b, 1)
                            : ny_native_nir_emit_const(b, 0);
      return ny_native_nir_emit_pair_list_i64(b, one_or_zero, one_or_zero);
    }
    if (strstr(name, ".get_key_scancode"))
      return ny_native_nir_emit_const(b, -1);
    if (strstr(name, ".get_key_name") || strstr(name, ".get_clipboard") ||
        strstr(name, ".get_primary_selection"))
      return ny_native_nir_emit_cstr_const(b, "");
  }
  if (name && strstr(name, ".get_mouse_button_state") &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "native NYIR lower: get_mouse_button_state expects "
                            "window and button");
      return -1;
    }
    int win = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int button = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int outer_key = ny_native_nir_emit_cstr_const(b, "mouse_buttons");
    int zero = ny_native_nir_emit_const(b, 0);
    if (win < 0 || button < 0 || outer_key < 0 || zero < 0)
      return -1;
    int states = ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", win,
                                                 outer_key, zero, 3, 0);
    if (states < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", states,
                                           button, zero, 3, 0);
  }
  if (name && strstr(name, ".get_key_state") &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: get_key_state expects window and key");
      return -1;
    }
    int win = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int key = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int outer_key = ny_native_nir_emit_cstr_const(b, "key_states");
    int zero = ny_native_nir_emit_const(b, 0);
    if (win < 0 || key < 0 || outer_key < 0 || zero < 0)
      return -1;
    int states = ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", win,
                                                 outer_key, zero, 3, 0);
    if (states < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_value_get_tagged", states, key,
                                           zero, 3, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(name, "std.core.str.replace") == 0 ||
       strcmp(name, "std.core.replace") == 0 ||
       strcmp(leaf, "str_replace") == 0)) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b,
                         "native NYIR lower: str_replace expects three values");
      return -1;
    }
    int args[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
      const expr_t *arg_expr = e->as.call.args.data[i].val;
      args[i] = ny_native_nir_lower_expr(b, arg_expr);
      if (args[i] < 0)
        return -1;
      if (!ny_native_nir_expr_is_cstr(b, arg_expr)) {
        args[i] = ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_any(b, arg_expr) ? "rt_any_to_cstr"
                                                   : "rt_i64_to_cstr_raw",
            args[i], -1, -1, 1, 0);
        if (args[i] < 0)
          return -1;
      }
    }
    int out = ny_native_nir_emit_runtime_call(b, "rt_cstr_replace",
                                              args[0], args[1], args[2], 3, 0);
    int length = out < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b, "rt_cstr_len", out, -1, -1, 1, 0);
    int tag = length < 0 ? -1 : ny_native_nir_emit_const(b, 121);
    if (out < 0 || length < 0 || tag < 0 ||
        !ny_native_nir_record_dyn_fact(b, out, NY_NATIVE_NIR_FACT_DYN_STR_LEN,
                                       length) ||
        !ny_native_nir_record_dyn_fact(b, out, NY_NATIVE_NIR_FACT_DYN_TAG, tag))
      return -1;
    return out;
  }
  /*
   * std.core.str is `@inline fn str(any v) str { to_str(v) }`, so routing it
   * through the same formatter dispatch is semantically identical — and
   * avoids compiling the stdlib body, which would hit the any-parameter ABI
   * path.  A user-defined (non-stdlib) `str` skips this via the guard below.
   */
  if ((strcmp(leaf, "str") == 0 || strcmp(leaf, "to_str") == 0 ||
       strcmp(leaf, "__to_str") == 0) &&
      (!ny_native_nir_user_defined_fn(b, name) || strcmp(leaf, "to_str") == 0 ||
       strcmp(leaf, "__to_str") == 0)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: to_str expects one positional value");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    if (ny_native_nir_expr_is_immutable_nil(b, arg_expr, 0))
      return ny_native_nir_emit_cstr_const(b, "nil");
    int arg = ny_native_nir_lower_expr(b, arg_expr);
    if (arg < 0)
      return -1;
    if (ny_native_nir_expr_is_cstr(b, arg_expr))
      return arg;
    /*
     * Route by the argument's actual kind: bools and floats must not go
     * through rt_i64_to_cstr_raw (which would print their raw bits as an
     * integer).  f32 is widened to f64 first; everything else falls through
     * to the tagged-any or i64 formatter.
     */
    if (ny_native_nir_expr_is_bool(b, arg_expr))
      return ny_native_nir_emit_runtime_call(b, "rt_bool_to_cstr", arg,
                                             -1, -1, 1, 0);
    if (ny_native_nir_expr_is_bigfloat(b, arg_expr))
      return ny_native_nir_emit_runtime_call(b, "rt_bigfloat_to_str", arg,
                                             -1, -1, 1, 0);
    /* BigInt handles take precedence over conservative numeric inference;
     * operator expressions can otherwise be mislabeled as f64 and stringify
     * the pointer through the floating formatter. */
    if (ny_native_nir_expr_is_bigint(b, arg_expr))
      return ny_native_nir_emit_runtime_call(b, "rt_any_to_cstr", arg,
                                             -1, -1, 1, 0);
    if (ny_native_nir_expr_is_f32(b, arg_expr)) {
      int f64 = ny_native_nir_emit_f32_to_f64(b, arg);
      if (f64 < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_f64_to_cstr_raw", f64,
                                             -1, -1, 1, 0);
    }
    if (ny_native_nir_expr_is_f64(b, arg_expr))
      return ny_native_nir_emit_runtime_call(b, "rt_f64_to_cstr_raw", arg,
                                             -1, -1, 1, 0);
    /* A typed bigint is a heap handle, not a raw integer.  The native string
     * ABI still returns an untagged C string, so use the any formatter after
     * preserving the handle instead of returning rt_bigint_to_str's tagged
     * string object directly. */
    /* __bigint_to_int is the raw native-index bridge.  Its source-level
     * result is still commonly fed to to_str; treating that raw i64 as a
     * tagged any shifts values at the i64 boundary (notably INT64_MAX). */
    if (arg_expr->kind == NY_E_CALL && arg_expr->as.call.callee &&
        arg_expr->as.call.callee->kind == NY_E_IDENT &&
        strcmp(arg_expr->as.call.callee->as.ident.name, "__bigint_to_int") == 0)
      return ny_native_nir_emit_runtime_call(b, "rt_i64_to_cstr_raw", arg,
                                             -1, -1, 1, 0);
    return ny_native_nir_emit_runtime_call(
        b,
        ny_native_nir_expr_is_any(b, arg_expr) ? "rt_any_to_cstr"
                                               : "rt_i64_to_cstr_raw",
        arg, -1, -1, 1, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) && canon_leaf &&
      strcmp(canon_leaf, "abs") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: abs expects one value");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    bool use_f64 = ny_native_nir_expr_is_f64(b, arg_expr);
    int zero = use_f64 ? ny_native_nir_emit_const_f64(b, 0.0)
                       : ny_native_nir_emit_const(b, 0);
    int neg =
        use_f64 ? ny_native_nir_push_val(b, NYIR_SUB_F64, zero, value, 0, NULL)
                : ny_native_nir_push_val(b, NYIR_SUB_I64, zero, value, 0, NULL);
    if (zero < 0 || neg < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(
        b, use_f64 ? "rt_f64_max" : "rt_i64_max", value, neg, -1,
        2, use_f64 ? NYIR_INST_F_RET_F64 : 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "clamp") == 0 || strcmp(canon_leaf, "lerp") == 0)) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "native NYIR lower: %s expects three values", leaf);
      return -1;
    }
    const expr_t *a_expr = e->as.call.args.data[0].val;
    const expr_t *b_expr = e->as.call.args.data[1].val;
    const expr_t *c_expr = e->as.call.args.data[2].val;
    int a = ny_native_nir_lower_expr(b, a_expr);
    int b_arg = ny_native_nir_lower_expr(b, b_expr);
    int c = ny_native_nir_lower_expr(b, c_expr);
    if (a < 0 || b_arg < 0 || c < 0)
      return -1;
    bool use_f64 = ny_native_nir_expr_is_f64(b, a_expr) ||
                   ny_native_nir_expr_is_f64(b, b_expr) ||
                   ny_native_nir_expr_is_f64(b, c_expr);
    if (use_f64) {
      if (!ny_native_nir_expr_is_f64(b, a_expr))
        a = ny_native_nir_emit_i64_to_f64(b, a);
      if (!ny_native_nir_expr_is_f64(b, b_expr))
        b_arg = ny_native_nir_emit_i64_to_f64(b, b_arg);
      if (!ny_native_nir_expr_is_f64(b, c_expr))
        c = ny_native_nir_emit_i64_to_f64(b, c);
      if (a < 0 || b_arg < 0 || c < 0)
        return -1;
      if (strcmp(canon_leaf, "lerp") == 0) {
        int diff = ny_native_nir_push_val(b, NYIR_SUB_F64, b_arg, a, 0, NULL);
        int scaled = diff < 0 ? -1
                              : ny_native_nir_push_val(b, NYIR_MUL_F64, diff, c,
                                                       0, NULL);
        return scaled < 0 ? -1
                          : ny_native_nir_push_val(b, NYIR_ADD_F64, a, scaled,
                                                   0, NULL);
      }
      int lo = ny_native_nir_emit_runtime_call(b, "rt_f64_max", a, b_arg,
                                               -1, 2, NYIR_INST_F_RET_F64);
      return lo < 0 ? -1
                    : ny_native_nir_emit_runtime_call(b, "rt_f64_min",
                                                      lo, c, -1, 2,
                                                      NYIR_INST_F_RET_F64);
    }
    if (strcmp(canon_leaf, "lerp") == 0) {
      int diff = ny_native_nir_push_val(b, NYIR_SUB_I64, b_arg, a, 0, NULL);
      int scaled =
          diff < 0 ? -1
                   : ny_native_nir_push_val(b, NYIR_MUL_I64, diff, c, 0, NULL);
      return scaled < 0
                 ? -1
                 : ny_native_nir_push_val(b, NYIR_ADD_I64, a, scaled, 0, NULL);
    }
    int lo = ny_native_nir_emit_runtime_call(b, "rt_i64_max", a, b_arg,
                                             -1, 2, 0);
    return lo < 0 ? -1
                  : ny_native_nir_emit_runtime_call(b, "rt_i64_min", lo,
                                                    c, -1, 2, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "min") == 0 || strcmp(canon_leaf, "max") == 0)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "native NYIR lower: min/max expects two values");
      return -1;
    }
    const expr_t *left_expr = e->as.call.args.data[0].val;
    const expr_t *right_expr = e->as.call.args.data[1].val;
    bool use_f64 = ny_native_nir_expr_is_f64(b, left_expr) ||
                   ny_native_nir_expr_is_f64(b, right_expr);
    int left = ny_native_nir_lower_expr(b, left_expr);
    int right = ny_native_nir_lower_expr(b, right_expr);
    if (left < 0 || right < 0)
      return -1;
    if (use_f64) {
      if (!ny_native_nir_expr_is_f64(b, left_expr))
        left = ny_native_nir_emit_i64_to_f64(b, left);
      if (!ny_native_nir_expr_is_f64(b, right_expr))
        right = ny_native_nir_emit_i64_to_f64(b, right);
      if (left < 0 || right < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(
          b,
          strcmp(canon_leaf, "min") == 0 ? "rt_f64_min" : "rt_f64_max",
          left, right, -1, 2, NYIR_INST_F_RET_F64);
    }
    return ny_native_nir_emit_runtime_call(
        b, strcmp(canon_leaf, "min") == 0 ? "rt_i64_min" : "rt_i64_max",
        left, right, -1, 2, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "int") == 0 &&
      !ny_native_nir_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name &&
      ny_native_nir_expr_is_f64(b, e->as.call.args.data[0].val)) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_f64_to_i64", value, -1,
                                           -1, 1, 0);
  }
  if (leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "__flt_sin") == 0 ||
       strcmp(canon_leaf, "__flt_cos") == 0)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: %s requires one positional argument", leaf);
      return -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg);
    if (value < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, arg)) {
      value = ny_native_nir_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    if (b->options && b->options->native_backend == NY_NATIVE_BACKEND_X86_64)
      return ny_native_nir_push_val(
          b, strcmp(canon_leaf, "__flt_sin") == 0 ? NYIR_SIN_F64 : NYIR_COS_F64,
          value, -1, 0, NULL);
    return ny_native_nir_emit_runtime_call(
        b,
        strcmp(canon_leaf, "__flt_sin") == 0 ? "rt_sin_f64"
                                       : "rt_cos_f64",
        value, -1, -1, 1, NYIR_INST_F_RET_F64);
  }
  /* C math headers do not consistently expose the return representation to
   * the lightweight FFI parser.  Normalize the common scalar entry points to
   * the runtime f64 ABI before generic external-call lowering; otherwise the
   * callee returns in XMM0 while the caller reads an integer register. */
  if (leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "sin") == 0 || strcmp(canon_leaf, "cos") == 0 ||
       strcmp(canon_leaf, "sqrt") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, e->as.call.args.data[0].val)) {
      value = ny_native_nir_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    const char *runtime = strcmp(canon_leaf, "sin") == 0   ? "rt_sin_f64"
                          : strcmp(canon_leaf, "cos") == 0 ? "rt_cos_f64"
                                                     : "rt_sqrt_f64";
    return ny_native_nir_emit_runtime_call(b, runtime, value, -1, -1, 1,
                                           NYIR_INST_F_RET_F64);
  }
  if (canon_leaf && !ny_native_nir_user_defined_fn(b, name)) {
    const char *c_symbol = NULL;
    int c_argc = 0;
    if (strcmp(canon_leaf, "memcpy") == 0 ||
        strcmp(canon_leaf, "memmove") == 0 ||
        strcmp(canon_leaf, "memset") == 0 ||
        strcmp(canon_leaf, "memcmp") == 0 ||
        strcmp(canon_leaf, "memchr") == 0) {
      c_symbol = canon_leaf;
      c_argc = 3;
    } else if (strcmp(canon_leaf, "strchr") == 0 ||
               strcmp(canon_leaf, "strcmp") == 0) {
      c_symbol = canon_leaf;
      c_argc = 2;
    }
    if (c_symbol) {
      if (e->as.call.args.len != (size_t)c_argc) {
        ny_native_nir_fail(b,
                           "native NYIR lower: %s expects %d positional values",
                           canon_leaf, c_argc);
        return -1;
      }
      int c_args[3] = {-1, -1, -1};
      for (int i = 0; i < c_argc; ++i) {
        if (e->as.call.args.data[i].name) {
          ny_native_nir_fail(
              b, "native NYIR lower: %s expects positional values", canon_leaf);
          return -1;
        }
        c_args[i] = ny_native_nir_lower_expr(b, e->as.call.args.data[i].val);
        if (c_args[i] < 0)
          return -1;
      }
      return ny_native_nir_emit_runtime_call(b, c_symbol, c_args[0], c_args[1],
                                             c_args[2], c_argc, 0);
    }
  }
  if (name &&
      (strstr(name, "x11_backend.") || strstr(name, "win32_impl.") ||
       strstr(name, "cocoa_impl.") || strstr(name, "wayland_backend.")) &&
      (strstr(name, ".set_pos") || strstr(name, ".set_size") ||
       strstr(name, ".set_title") || strstr(name, ".set_cursor_pos") ||
       strstr(name, ".set_input_mode") || strstr(name, ".set_window_") ||
       strstr(name, ".show_window") || strstr(name, ".hide_window") ||
       strstr(name, ".focus_window") || strstr(name, ".post_empty_event")) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    return ny_native_nir_emit_const(b, 0);
  }
  ny_native_nir_local_t *named_local = ny_native_nir_find_local(b, name);
  const expr_t *named_global =
      name ? ny_native_nir_find_top_level_value(b, name) : NULL;
  bool named_callable_alias =
      (named_local &&
       (named_local->callable_expr || named_local->callable_name)) ||
      (named_global && named_global->kind == NY_E_IDENT &&
       named_global->as.ident.name &&
       ny_native_nir_find_user_function(b, named_global->as.ident.name));
  if (name && named_local && !named_callable_alias &&
      !ny_native_nir_user_defined_fn(b, name)) {
    /* An untyped callable parameter is a raw closure/function value in the
     * native ABI.  Preserve the indirect call instead of treating the local
     * as an ordinary value binding (the old zero result made thunk() a no-op).
     * rt_call0 understands both raw code pointers and heap closures. */
    if ((named_local->is_any ||
         named_local->semantic_rep == NY_SEM_REP_CLOSURE) &&
        e->as.call.args.len == 0) {
      int callable = ny_native_nir_lower_expr(b, e->as.call.callee);
      return callable < 0 ? -1
                          : ny_native_nir_emit_runtime_call(
                                b, "rt_call0", callable, -1, -1, 1, 0);
    }
    /* Non-zero-argument callable locals continue into the general indirect
     * closure path below; returning zero here silently erased `f(x)` calls in
     * functions with an untyped/fnptr parameter. */
    if (!(named_local->is_any ||
          named_local->semantic_rep == NY_SEM_REP_CLOSURE ||
          (named_global && named_global->kind == NY_E_CALL)))
      return ny_native_nir_emit_const(b, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) && canon_leaf &&
      strcmp(canon_leaf, "panic") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name)
      return ny_native_nir_fail(
          b, "native NYIR lower: panic expects one positional argument");
    int message = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int tagged = message < 0 ? -1
                             : ny_native_nir_emit_runtime_call(
                                   b, "rt_alloc_string", message, -1, -1, 1, 0);
    if (tagged < 0 || ny_native_nir_emit_runtime_call(b, "rt_panic", tagged, -1,
                                                      -1, 1, 0) < 0)
      return -1;
    return ny_native_nir_emit_const(b, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) && canon_leaf &&
      (strcmp(canon_leaf, "prove") == 0 ||
       strcmp(canon_leaf, "static_assert") == 0 ||
       strcmp(canon_leaf, "assert_compile") == 0)) {
    if (e->as.call.args.len == 0) {
      ny_native_nir_fail(b, "native NYIR: %s expects a compile-time condition",
                         canon_leaf);
      return -1;
    }
    const expr_t *condition = e->as.call.args.data[0].val;
    int64_t proof_value = 0;
    bool exact_proof_value = condition && ny_native_nir_eval_proof_i64(
                                              b, condition, 0, &proof_value);
    bool unresolved_local = false;
    if (condition && condition->kind == NY_E_BINARY) {
      const expr_t *parts[2] = {condition->as.binary.left,
                                condition->as.binary.right};
      for (size_t i = 0; i < 2; ++i) {
        const expr_t *part = parts[i];
        if (part && part->kind == NY_E_IDENT && part->as.ident.name &&
            ny_native_nir_find_local(b, part->as.ident.name))
          unresolved_local = true;
      }
    }
    if (strcmp(canon_leaf, "prove") == 0 && !exact_proof_value && condition &&
        (condition->kind == NY_E_IDENT || unresolved_local) &&
        b->current_fn_name) {
      if (unresolved_local) {
        ny_native_nir_fail(b, "prove condition must be known at compile time");
        return -1;
      }
      const stmt_t *fn =
          ny_native_nir_find_user_function(b, b->current_fn_name);
      for (size_t i = 0; fn && i < fn->as.fn.params.len; ++i) {
        const param_t *param = &fn->as.fn.params.data[i];
        if (param->name && strcmp(param->name, condition->as.ident.name) == 0 &&
            param->type && strcmp(param->type, "bool") == 0) {
          ny_native_nir_fail(b,
                             "prove condition must be known at compile time");
          return -1;
        }
      }
    }
    /* A lemma invocation carrying an ordinary runtime call is not a
     * compile-time witness. Keep the native proof path aligned with the
     * legacy checker instead of hashing the unresolved proposition as if it
     * were already established. */
    if (strcmp(canon_leaf, "prove") == 0 && condition &&
        condition->kind == NY_E_CALL && condition->as.call.callee &&
        condition->as.call.args.len > 0) {
      for (size_t ai = 0; ai < condition->as.call.args.len; ++ai) {
        const expr_t *arg = condition->as.call.args.data[ai].val;
        if (arg && (arg->kind == NY_E_CALL || arg->kind == NY_E_MEMCALL)) {
          ny_native_nir_fail(
              b, "lemma could not prove the supplied arguments: arguments must "
                 "be compile-time values or proven ranges");
          return -1;
        }
      }
    }
    if (exact_proof_value && proof_value == 0) {
      const char *message = strcmp(canon_leaf, "prove") == 0
                                ? "proof obligation failed"
                                : "static assertion failed";
      if (e->as.call.args.len >= 2 && e->as.call.args.data[1].val &&
          e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
          e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR &&
          e->as.call.args.data[1].val->as.literal.as.s.data)
        message = e->as.call.args.data[1].val->as.literal.as.s.data;
      ny_native_nir_fail(b, "%s", message);
      return -1;
    }
    /* A proof witness is represented by the same canonical proposition digest
     * used by the legacy backend.  This keeps proof_matches meaningful in
     * native code while static_assert/assert_compile remain erased. */
    if (strcmp(canon_leaf, "prove") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.data[0].val) {
      char *proof_type =
          ny_proof_type_from_expr((expr_t *)e->as.call.args.data[0].val);
      if (proof_type) {
        uint64_t digest = ny_hash64_cstr(proof_type);
        free(proof_type);
        return ny_native_nir_emit_const(b, (int64_t)digest);
      }
    }
    if (strcmp(leaf, "assert_compile") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.data[0].val &&
        e->as.call.args.data[0].val->kind == NY_E_CALL) {
      const expr_t *nested = e->as.call.args.data[0].val;
      const char *nested_leaf = ny_native_call_leaf(nested);
      if (nested_leaf && strcmp(nested_leaf, "range_proven") == 0 &&
          nested->as.call.args.len == 3) {
        int64_t value = 0, lo = 0, hi = 0;
        bool exact =
            ny_native_nir_eval_proof_i64(b, nested->as.call.args.data[0].val, 0,
                                         &value) &&
            ny_native_nir_eval_proof_i64(b, nested->as.call.args.data[1].val, 0,
                                         &lo) &&
            ny_native_nir_eval_proof_i64(b, nested->as.call.args.data[2].val, 0,
                                         &hi);
        if (exact && (lo > hi || value < lo || value > hi)) {
          ny_native_nir_fail(b, "native NYIR: assert_compile failed for "
                                "range_proven");
          return -1;
        }
      }
    }
    return ny_native_nir_emit_const(b, 0);
  }
  if (!ny_native_nir_user_defined_fn(b, name) && leaf &&
      strcmp(leaf, "proof_matches") == 0) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR: proof_matches expects witness and proposition");
      return -1;
    }
    const expr_t *witness = e->as.call.args.data[0].val;
    const expr_t *init =
        witness && witness->kind == NY_E_IDENT
            ? ny_native_nir_find_top_level_value(b, witness->as.ident.name)
            : NULL;
    uint64_t actual = 0;
    bool have_actual = false;
    if (init && init->kind == NY_E_CALL && init->as.call.args.len >= 1 &&
        init->as.call.args.data[0].val) {
      const char *wleaf = ny_native_call_leaf(init);
      if (wleaf && strcmp(wleaf, "prove") == 0) {
        char *type =
            ny_proof_type_from_expr((expr_t *)init->as.call.args.data[0].val);
        if (type) {
          actual = ny_hash64_cstr(type);
          free(type);
          have_actual = true;
        }
      }
    }
    if (!have_actual && witness && witness->kind == NY_E_CALL &&
        witness->as.call.args.len >= 1) {
      const char *wleaf = ny_native_call_leaf(witness);
      if (wleaf && strcmp(wleaf, "prove") == 0) {
        char *type = ny_proof_type_from_expr(
            (expr_t *)witness->as.call.args.data[0].val);
        if (type) {
          actual = ny_hash64_cstr(type);
          free(type);
          have_actual = true;
        }
      }
    }
    char *expected_type =
        ny_proof_type_from_expr((expr_t *)e->as.call.args.data[1].val);
    bool matches = false;
    if (have_actual && expected_type) {
      matches = actual == ny_hash64_cstr(expected_type);
    }
    free(expected_type);
    return ny_native_nir_emit_const(b, matches ? NY_IMM_TRUE : NY_IMM_FALSE);
  }
  if (leaf && strcmp(leaf, "__tagof") == 0 &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: __tagof expects one value");
      return -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    if (arg_expr && arg_expr->semantic.resolved) {
      if (arg_expr->semantic.rep == NY_SEM_REP_RAW_INT)
        return ny_native_nir_emit_const(b, 1);
      if (arg_expr->semantic.rep == NY_SEM_REP_F64 ||
          arg_expr->semantic.rep == NY_SEM_REP_F32)
        return ny_native_nir_emit_const(b, TAG_FLOAT);
      if (arg_expr->semantic.rep == NY_SEM_REP_STRING)
        return ny_native_nir_emit_const(b, 121);
      if (arg_expr->semantic.rep == NY_SEM_REP_CLOSURE)
        return ny_native_nir_emit_const(b, 122);
    }
    int value = ny_native_nir_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    int tag = ny_native_nir_peek_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG);
    if (tag >= 0)
      return tag;
    if (ny_native_nir_expr_is_cstr(b, arg_expr))
      return ny_native_nir_emit_const(b, 121);
    if (ny_native_nir_expr_is_f64(b, arg_expr) ||
        ny_native_nir_expr_is_f32(b, arg_expr))
      return ny_native_nir_emit_const(b, TAG_FLOAT);
    /* Fall through to rt_tagof for pointer/container types (tbuf, dict,
       string, closure, etc.).  Only small ints and floats are known at
       compile time above; the !is_any fallback was wrong for tuples,
       lists, sets, and dicts whose NYIR type is not 'any'. */
    const char *tag_symbol = ny_native_runtime_symbol_for_expr(
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL,
        leaf, e);
    if (!tag_symbol)
      return -1;
    return ny_native_nir_emit_runtime_call(b, tag_symbol, value, -1, -1, 1, 0);
  }
  if (leaf && strcmp(leaf, "_big_is_rt") == 0 && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int tag = ny_native_nir_emit_const(b, TAG_BIGINT);
    if (value < 0 || tag < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_native_has_tag", value, tag,
                                           -1, 2, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_IS_STR &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(b, "native NYIR lower: is_str expects one value");
      return -1;
    }
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    int tag = ny_native_nir_peek_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG);
    if (tag < 0)
      return ny_native_nir_emit_runtime_call(b, "rt_is_str", value, -1,
                                             -1, 1, 0);
    /* The tag slot may carry either string spelling: rt_value_tag reports
     * TAG_STR (120) for managed handles while literal descriptors record
     * TAG_STR_CONST (121).  Matching only 121 made _iter_is_seq reject
     * managed strings and flatten kept them unsplit. */
    int want_const = ny_native_nir_emit_const(b, 121);
    int want_str = ny_native_nir_emit_const(b, 120);
    int eq_const = want_const < 0
                       ? -1
                       : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                           .dst = -1,
                                                           .a = tag,
                                                           .b = want_const,
                                                           .cmp = NYIR_CMP_EQ});
    int eq_str =
        want_str < 0 || eq_const < 0
            ? -1
            : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                .dst = -1,
                                                .a = tag,
                                                .b = want_str,
                                                .cmp = NYIR_CMP_EQ});
    return eq_str < 0 ? -1
                      : ny_native_nir_emit_binop(b, NYIR_OR_I64, eq_const,
                                                 eq_str);
  }
  if (leaf_kind == NY_NATIVE_LEAF_INTRINSIC) {
    /*
     * Capability-gated portable intrinsics: lower a small set to pure NYIR
     * (SWAR), not LLVM. Unknown names still fail explicitly.
     */
    if (e->as.call.args.len < 1 || !e->as.call.args.data[0].val ||
        e->as.call.args.data[0].val->kind != NY_E_LITERAL ||
        e->as.call.args.data[0].val->as.literal.kind != NY_LIT_STR) {
      ny_native_nir_fail(b, "native NYIR lower: intrinsic(...) first argument "
                            "must be a string literal name");
      return -1;
    }
    const char *iname = e->as.call.args.data[0].val->as.literal.as.s.data;
    size_t iname_len = e->as.call.args.data[0].val->as.literal.as.s.len;
    if (!iname || iname_len == 0) {
      ny_native_nir_fail(b, "native NYIR lower: empty intrinsic name");
      return -1;
    }
    if (iname_len == 9 && memcmp(iname, "ctpop.i64", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: ctpop.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * SWAR popcount with logical shifts (SAR + clear high fill bits).
       */
      int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int one = ny_native_nir_emit_const(b, 1);
      int two = ny_native_nir_emit_const(b, 2);
      int four = ny_native_nir_emit_const(b, 4);
      int eight = ny_native_nir_emit_const(b, 8);
      int sixteen = ny_native_nir_emit_const(b, 16);
      int thirtytwo = ny_native_nir_emit_const(b, 32);
      int mask7f = ny_native_nir_emit_const(b, 0x7f);
      /*
       * Logical-shift masks: (1<<(64-n))-1
       */
      int m1 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
      int m2 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
      int m4 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
      int m8 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
      int m16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
      int m32 = ny_native_nir_emit_const(b, (int64_t)0x00000000ffffffffLL);
      if (c1 < 0 || c2 < 0 || c4 < 0 || one < 0 || two < 0 || four < 0 ||
          eight < 0 || sixteen < 0 || thirtytwo < 0 || mask7f < 0 || m1 < 0 ||
          m2 < 0 || m4 < 0 || m8 < 0 || m16 < 0 || m32 < 0)
        return -1;
#define NY_LSHR(outv, src, sh, msk)                                            \
  do {                                                                         \
    int _t = nyir_emit(                                                        \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = (src), .b = (sh)});  \
    if (_t < 0)                                                                \
      return -1;                                                               \
    (outv) = nyir_emit(                                                        \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = _t, .b = (msk)});    \
    if ((outv) < 0)                                                            \
      return -1;                                                               \
  } while (0)
      int t;
      NY_LSHR(t, x, one, m1);
      t = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = t, .b = c1});
      if (t < 0)
        return -1;
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      int a = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = c2});
      int b2;
      NY_LSHR(b2, x, two, m2);
      if (a < 0)
        return -1;
      b2 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = b2, .b = c2});
      if (b2 < 0)
        return -1;
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = a, .b = b2});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, four, m4);
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = c4});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, eight, m8);
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, sixteen, m16);
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
      NY_LSHR(t, x, thirtytwo, m32);
      x = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = x, .b = t});
      if (x < 0)
        return -1;
#undef NY_LSHR
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = mask7f});
    }
    if ((iname_len == 8 && memcmp(iname, "cttz.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0)) {
      int is_ctlz = (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0);
      /*
       * cttz via ctpop((x & -x) - 1); zero input yields 64.
       * ctlz via cttz(bitreverse(x)).
       */
      if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
          e->as.call.args.data[1].name) {
        ny_native_nir_fail(b,
                           "native NYIR lower: cttz/ctlz.i64 expects a value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      int zero_undef = e->as.call.args.len == 3
                           ? ny_native_nir_lower_expr(b, e->as.call.args.data[2].val)
                           : ny_native_nir_emit_const(b, 0);
      if (zero_undef < 0)
        return -1;
      /* Use the host's well-defined scalar bit-count primitive here.  The
       * portable SWAR fallback below remains available for targets without
       * the runtime bridge, but the JIT/LLVM path must preserve the intrinsic
       * zero-input contract exactly. */
      (void)zero_undef; /* raw bridge preserves the defined zero result */
      int raw_result = ny_native_nir_emit_runtime_call(
          b, is_ctlz ? "rt_simmd_clz64_i64" : "rt_simmd_ctz64_i64", x,
          -1, -1, 1, 0);
      return raw_result;
      if (is_ctlz) {
        /*
         * Bitreverse x via SWAR so cttz on the result yields ctlz.
         */
        int bc1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
        int bc2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
        int bc4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
        int bc8 = ny_native_nir_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
        int bc16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
        int b1 = ny_native_nir_emit_const(b, 1);
        int b2c = ny_native_nir_emit_const(b, 2);
        int b4c = ny_native_nir_emit_const(b, 4);
        int b8 = ny_native_nir_emit_const(b, 8);
        int b16 = ny_native_nir_emit_const(b, 16);
        int b32 = ny_native_nir_emit_const(b, 32);
        if (bc1 < 0 || bc2 < 0 || bc4 < 0 || bc8 < 0 || bc16 < 0 || b1 < 0 ||
            b2c < 0 || b4c < 0 || b8 < 0 || b16 < 0 || b32 < 0)
          return -1;
        int bm63 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
        int bm62 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
        int bm60 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
        int bm56 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
        int bm48 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
        if (bm63 < 0 || bm62 < 0 || bm60 < 0 || bm56 < 0 || bm48 < 0)
          return -1;
        /*
         * SWAR bit-reverse steps.
         */
        int br;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b1});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm63});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc1});
        if (br < 0)
          return -1;
        int bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc1});
        if (bl < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b1});
        if (bl < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b2c});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm62});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc2});
        if (br < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc2});
        if (bl < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b2c});
        if (bl < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b4c});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm60});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc4});
        if (br < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc4});
        if (bl < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b4c});
        if (bl < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b8});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm56});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc8});
        if (br < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc8});
        if (bl < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b8});
        if (bl < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b16});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bm48});
        if (br < 0)
          return -1;
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = br, .b = bc16});
        if (br < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = bc16});
        if (bl < 0)
          return -1;
        bl = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = bl, .b = b16});
        if (bl < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = br, .b = bl});
        if (x < 0)
          return -1;
        /*
         * final 32-bit swap
         */
        br = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = b32});
        if (br < 0)
          return -1;
        x = nyir_emit(&b->nyir,
                      (nyir_inst_t){.op = NYIR_AND_I64,
                                    .dst = -1,
                                    .a = x,
                                    .b = ny_native_nir_emit_const(
                                        b, (int64_t)0x00000000FFFFFFFFLL)});
        if (x < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = x, .b = b32});
        if (x < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = x, .b = br});
        if (x < 0)
          return -1;
      }
      int zero = ny_native_nir_emit_const(b, 0);
      int one = ny_native_nir_emit_const(b, 1);
      int sixtyfour = ny_native_nir_emit_const(b, 64);
      if (zero < 0 || one < 0 || sixtyfour < 0)
        return -1;
      /*
       * is_zero = (x == 0)
       */
      int is_zero = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = x,
                                                      .b = zero,
                                                      .cmp = NYIR_CMP_EQ});
      if (is_zero < 0)
        return -1;
      /*
       * lowest = x & -x  (0 - x for negate)
       */
      int neg = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = zero, .b = x});
      if (neg < 0)
        return -1;
      int lowest = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = x, .b = neg});
      if (lowest < 0)
        return -1;
      int lm1 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = lowest, .b = one});
      if (lm1 < 0)
        return -1;
      /*
       * Inline SWAR popcount of lm1 (same as ctpop).
       */
      int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int two = ny_native_nir_emit_const(b, 2);
      int four = ny_native_nir_emit_const(b, 4);
      int eight = ny_native_nir_emit_const(b, 8);
      int sixteen = ny_native_nir_emit_const(b, 16);
      int thirtytwo = ny_native_nir_emit_const(b, 32);
      int mask7f = ny_native_nir_emit_const(b, 0x7f);
      if (c1 < 0 || c2 < 0 || c4 < 0 || two < 0 || four < 0 || eight < 0 ||
          sixteen < 0 || thirtytwo < 0 || mask7f < 0)
        return -1;
      int t = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = lm1, .b = one});
      if (t < 0)
        return -1;
      t = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = t, .b = c1});
      if (t < 0)
        return -1;
      int px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = lm1, .b = t});
      if (px < 0)
        return -1;
      int a = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = px, .b = c2});
      int b2 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = px, .b = two});
      if (a < 0 || b2 < 0)
        return -1;
      b2 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = b2, .b = c2});
      if (b2 < 0)
        return -1;
      px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = a, .b = b2});
      if (px < 0)
        return -1;
      t = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = px, .b = four});
      if (t < 0)
        return -1;
      px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = px, .b = c4});
      if (px < 0)
        return -1;
      t = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = px, .b = eight});
      if (t < 0)
        return -1;
      px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      t = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = px, .b = sixteen});
      if (t < 0)
        return -1;
      px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      t = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                            .dst = -1,
                                            .a = px,
                                            .b = thirtytwo});
      if (t < 0)
        return -1;
      px = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = px, .b = t});
      if (px < 0)
        return -1;
      int pop = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = px, .b = mask7f});
      if (pop < 0)
        return -1;
      /*
       * result = is_zero ? 64 : pop  — use select via arithmetic:
       * is_zero * 64 + (1-is_zero)*pop, but we only have binary ops.
       * (is_zero * 64) | ((is_zero ^ 1) * pop)
       */
      int notz = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = is_zero, .b = one});
      if (notz < 0)
        return -1;
      int term0 = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_MUL_I64,
                                                    .dst = -1,
                                                    .a = is_zero,
                                                    .b = sixtyfour});
      int term1 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = notz, .b = pop});
      if (term0 < 0 || term1 < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = term0, .b = term1});
    }
    if ((iname_len == 8 && memcmp(iname, "umax.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "umin.i64", 8) == 0)) {
      /*
       * Treat as signed for Nytrix i64 values in native path (same select).
       */
      if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
          e->as.call.args.data[2].name) {
        ny_native_nir_fail(
            b, "native NYIR lower: umax/umin.i64 expects two values");
        return -1;
      }
      bool is_max = iname[1] == 'm' && iname[2] == 'a';
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int y = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      if (x < 0 || y < 0)
        return -1;
      int c = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                  .dst = -1,
                                  .a = x,
                                  .b = y,
                                  .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
      int one = ny_native_nir_emit_const(b, 1);
      if (c < 0 || one < 0)
        return -1;
      int nc = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = c, .b = one});
      if (nc < 0)
        return -1;
      int t0 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = c, .b = x});
      int t1 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
      if (t0 < 0 || t1 < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
    }
    if ((iname_len == 8 && memcmp(iname, "smax.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "smin.i64", 8) == 0)) {
      if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
          e->as.call.args.data[2].name) {
        ny_native_nir_fail(
            b, "native NYIR lower: smax/smin.i64 expects two values");
        return -1;
      }
      bool is_max = iname_len >= 4 && iname[1] == 'm' && iname[2] == 'a';
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int y = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      if (x < 0 || y < 0)
        return -1;
      /*
       * result = (x > y) ? x : y  via arithmetic select:
       * c = (x > y); c*x + (1-c)*y
       */
      int c = nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                  .dst = -1,
                                  .a = x,
                                  .b = y,
                                  .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
      int one = ny_native_nir_emit_const(b, 1);
      if (c < 0 || one < 0)
        return -1;
      int nc = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = c, .b = one});
      if (nc < 0)
        return -1;
      int t0 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = c, .b = x});
      int t1 = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
      if (t0 < 0 || t1 < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
    }
    if (iname_len == 14 && memcmp(iname, "bitreverse.i64", 14) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(
            b, "native NYIR lower: bitreverse.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * Portable bit reverse via parallel SWAR.
       */
      int c1 = ny_native_nir_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = ny_native_nir_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = ny_native_nir_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int c8 = ny_native_nir_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
      int c16 = ny_native_nir_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
      int one = ny_native_nir_emit_const(b, 1);
      int two = ny_native_nir_emit_const(b, 2);
      int four = ny_native_nir_emit_const(b, 4);
      int eight = ny_native_nir_emit_const(b, 8);
      int sixteen = ny_native_nir_emit_const(b, 16);
      int thirtytwo = ny_native_nir_emit_const(b, 32);
      if (c1 < 0 || c2 < 0 || c4 < 0 || c8 < 0 || c16 < 0 || one < 0 ||
          two < 0 || four < 0 || eight < 0 || sixteen < 0 || thirtytwo < 0)
        return -1;
      /*
       * x = ((x >> 1) & c1) | ((x & c1) << 1) etc. Use SAR+mask for >>
       */
      int m63 = ny_native_nir_emit_const(b, (int64_t)0x7fffffffffffffffLL);
      int m62 = ny_native_nir_emit_const(b, (int64_t)0x3fffffffffffffffLL);
      int m60 = ny_native_nir_emit_const(b, (int64_t)0x0fffffffffffffffLL);
      int m56 = ny_native_nir_emit_const(b, (int64_t)0x00ffffffffffffffLL);
      int m48 = ny_native_nir_emit_const(b, (int64_t)0x0000ffffffffffffLL);
      int m32 = ny_native_nir_emit_const(b, (int64_t)0x00000000ffffffffLL);
      if (m63 < 0 || m62 < 0 || m60 < 0 || m56 < 0 || m48 < 0 || m32 < 0)
        return -1;
#define NY_BREV_STEP(xin, sh, msk, cm)                                         \
  do {                                                                         \
    int _r = nyir_emit(                                                        \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = (xin), .b = (sh)});  \
    if (_r < 0)                                                                \
      return -1;                                                               \
    _r = nyir_emit(                                                            \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = _r, .b = (msk)});    \
    if (_r < 0)                                                                \
      return -1;                                                               \
    _r = nyir_emit(                                                            \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = _r, .b = (cm)});     \
    if (_r < 0)                                                                \
      return -1;                                                               \
    int _l = nyir_emit(                                                        \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = (xin), .b = (cm)});  \
    if (_l < 0)                                                                \
      return -1;                                                               \
    _l = nyir_emit(                                                            \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = _l, .b = (sh)});     \
    if (_l < 0)                                                                \
      return -1;                                                               \
    (xin) = nyir_emit(                                                         \
        &b->nyir,                                                              \
        (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = _r, .b = _l});        \
    if ((xin) < 0)                                                             \
      return -1;                                                               \
  } while (0)
      NY_BREV_STEP(x, one, m63, c1);
      NY_BREV_STEP(x, two, m62, c2);
      NY_BREV_STEP(x, four, m60, c4);
      NY_BREV_STEP(x, eight, m56, c8);
      NY_BREV_STEP(x, sixteen, m48, c16);
      /*
       * final 32-bit swap
       */
      {
        int r = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                  .dst = -1,
                                                  .a = x,
                                                  .b = thirtytwo});
        if (r < 0)
          return -1;
        r = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = r, .b = m32});
        if (r < 0)
          return -1;
        int l = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                  .dst = -1,
                                                  .a = x,
                                                  .b = thirtytwo});
        if (l < 0)
          return -1;
        x = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = r, .b = l});
        if (x < 0)
          return -1;
      }
#undef NY_BREV_STEP
      return x;
    }
    if (iname_len == 7 && memcmp(iname, "abs.i64", 7) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: abs.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      int zero = ny_native_nir_emit_const(b, 0);
      int sixtythree = ny_native_nir_emit_const(b, 63);
      if (zero < 0 || sixtythree < 0)
        return -1;
      /*
       * abs via (x ^ (x>>63)) - (x>>63) arithmetic.
       */
      int s = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                .dst = -1,
                                                .a = x,
                                                .b = sixtythree});
      if (s < 0)
        return -1;
      int y = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = x, .b = s});
      if (y < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = y, .b = s});
    }
    if (iname_len == 9 && memcmp(iname, "bswap.i64", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: bswap.i64 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * Portable byte swap via shifts and masks (no host asm).
       */
      int c8 = ny_native_nir_emit_const(b, 8);
      int c16 = ny_native_nir_emit_const(b, 16);
      int c24 = ny_native_nir_emit_const(b, 24);
      int c32 = ny_native_nir_emit_const(b, 32);
      int c40 = ny_native_nir_emit_const(b, 40);
      int c48 = ny_native_nir_emit_const(b, 48);
      int c56 = ny_native_nir_emit_const(b, 56);
      int mff = ny_native_nir_emit_const(b, (int64_t)0xff);
      if (c8 < 0 || c16 < 0 || c24 < 0 || c32 < 0 || c40 < 0 || c48 < 0 ||
          c56 < 0 || mff < 0)
        return -1;
      int acc = -1;
      int shifts[] = {c56, c48, c40, c32, c24, c16, c8, -1};
      int rshifts[] = {0, 8, 16, 24, 32, 40, 48, 56};
      for (int bi = 0; bi < 8; ++bi) {
        int piece = x;
        if (rshifts[bi] > 0) {
          int rs = ny_native_nir_emit_const(b, rshifts[bi]);
          if (rs < 0)
            return -1;
          piece = nyir_emit(
              &b->nyir,
              (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = rs});
          if (piece < 0)
            return -1;
          /*
           * Logical mask after SAR for high bytes
           */
          if (rshifts[bi] >= 32) {
            int m = ny_native_nir_emit_const(
                b, (int64_t)((1ULL << (64 - rshifts[bi])) - 1ULL));
            if (m < 0)
              return -1;
            piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_AND_I64,
                                                      .dst = -1,
                                                      .a = piece,
                                                      .b = m});
            if (piece < 0)
              return -1;
          }
        }
        piece = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = piece, .b = mff});
        if (piece < 0)
          return -1;
        if (shifts[bi] >= 0) {
          piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                    .dst = -1,
                                                    .a = piece,
                                                    .b = shifts[bi]});
          if (piece < 0)
            return -1;
        }
        if (acc < 0)
          acc = piece;
        else {
          acc = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64,
                                                  .dst = -1,
                                                  .a = acc,
                                                  .b = piece});
          if (acc < 0)
            return -1;
        }
      }
      return acc;
    }
    if (iname_len == 9 && memcmp(iname, "bswap.i32", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        ny_native_nir_fail(b, "native NYIR lower: bswap.i32 expects one value");
        return -1;
      }
      int x = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      const expr_t *src = e->as.call.args.data[1].val;
      if (src && src->kind == NY_E_LITERAL &&
          src->as.literal.kind == NY_LIT_INT) {
        uint32_t raw = (uint32_t)src->as.literal.as.i;
        uint32_t swapped = ((raw & 0xffu) << 24) | ((raw & 0xff00u) << 8) |
                           ((raw >> 8) & 0xff00u) | ((raw >> 24) & 0xffu);
        return ny_native_nir_emit_const(b, (int64_t)swapped);
      }
      /* Keep the result in the raw-i64 native ABI while limiting the input
       * to the intrinsic's 32-bit domain.  Arithmetic right shifts are safe
       * here because every shifted piece is immediately masked. */
      int c8 = ny_native_nir_emit_const(b, 8);
      int c16 = ny_native_nir_emit_const(b, 16);
      int c24 = ny_native_nir_emit_const(b, 24);
      int mff = ny_native_nir_emit_const(b, 0xff);
      int m32 = ny_native_nir_emit_const(b, 0xffffffffLL);
      if (c8 < 0 || c16 < 0 || c24 < 0 || mff < 0 || m32 < 0)
        return -1;
      int pieces[4] = {x, x, x, x};
      int shifts[4] = {c24, c16, c8, -1};
      int rshifts[4] = {0, 8, 16, 24};
      int acc = -1;
      for (int bi = 0; bi < 4; ++bi) {
        int piece = pieces[bi];
        if (rshifts[bi] > 0) {
          int rs = ny_native_nir_emit_const(b, rshifts[bi]);
          if (rs < 0)
            return -1;
          piece = nyir_emit(
              &b->nyir,
              (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = x, .b = rs});
          if (piece < 0)
            return -1;
        }
        piece = nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = piece, .b = mff});
        if (piece < 0)
          return -1;
        if (shifts[bi] >= 0) {
          piece = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                    .dst = -1,
                                                    .a = piece,
                                                    .b = shifts[bi]});
          if (piece < 0)
            return -1;
        }
        acc = acc < 0 ? piece
                      : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_OR_I64,
                                                          .dst = -1,
                                                          .a = acc,
                                                          .b = piece});
        if (acc < 0)
          return -1;
      }
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = acc, .b = m32});
    }
    if ((iname_len == 8 && memcmp(iname, "fshl.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "rotl.i64", 8) == 0)) {
      bool is_rotl = (iname_len == 8 && memcmp(iname, "rotl.i64", 8) == 0);
      size_t expected_args = is_rotl ? 2u : 3u;
      if (e->as.call.args.len != expected_args + 1) {
        ny_native_nir_fail(b, "native NYIR lower: %.*s argument count mismatch",
                           (int)iname_len, iname);
        return -1;
      }
      int a_val = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int b_val =
          is_rotl ? a_val
                  : ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      int sh_val = ny_native_nir_lower_expr(
          b, e->as.call.args.data[is_rotl ? 2 : 3].val);
      if (a_val < 0 || b_val < 0 || sh_val < 0)
        return -1;
      int c63 = ny_native_nir_emit_const(b, 63);
      int c64 = ny_native_nir_emit_const(b, 64);
      if (c63 < 0 || c64 < 0)
        return -1;
      int s = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = sh_val, .b = c63});
      int inv_s = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = c64, .b = sh_val});
      int rsh = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = inv_s, .b = c63});
      if (s < 0 || inv_s < 0 || rsh < 0)
        return -1;
      int left = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = a_val, .b = s});
      int right = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = b_val, .b = rsh});
      if (left < 0 || right < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = left, .b = right});
    }
    if ((iname_len == 8 && memcmp(iname, "fshr.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "rotr.i64", 8) == 0)) {
      bool is_rotr = (iname_len == 8 && memcmp(iname, "rotr.i64", 8) == 0);
      size_t expected_args = is_rotr ? 2u : 3u;
      if (e->as.call.args.len != expected_args + 1) {
        ny_native_nir_fail(b, "native NYIR lower: %.*s argument count mismatch",
                           (int)iname_len, iname);
        return -1;
      }
      int a_val = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      int b_val =
          is_rotr ? a_val
                  : ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
      int sh_val = ny_native_nir_lower_expr(
          b, e->as.call.args.data[is_rotr ? 2 : 3].val);
      if (a_val < 0 || b_val < 0 || sh_val < 0)
        return -1;
      int c63 = ny_native_nir_emit_const(b, 63);
      int c64 = ny_native_nir_emit_const(b, 64);
      if (c63 < 0 || c64 < 0)
        return -1;
      int s = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = sh_val, .b = c63});
      int inv_s = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = c64, .b = sh_val});
      int lsh = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = inv_s, .b = c63});
      if (s < 0 || inv_s < 0 || lsh < 0)
        return -1;
      int right = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = a_val, .b = s});
      int left = nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = b_val, .b = lsh});
      if (left < 0 || right < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = left, .b = right});
    }
    ny_native_nir_fail(b,
                       "native NYIR lower: intrinsic(\"%.*s\") is not "
                       "supported by the Nytrix-owned native backend; use "
                       "ordinary Nytrix operations or std.math.bin",
                       (int)iname_len, iname);
    return -1;
  }
  /*
   * NYIR integer values are raw i64s.  Keep typed print on the raw-i64
   * runtime entry point; rt_print_value is a dynamic NyValue ABI and must
   * only be used after an explicit box operation exists.
   */
  if (leaf_kind == NY_NATIVE_LEAF_ASSERT &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len == 2 && e->as.call.args.data[1].name)) {
      ny_native_nir_fail(b, "native NYIR lower: assert expects condition and "
                            "optional string message");
      return -1;
    }
    int condition = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int message = -1;
    if (condition < 0)
      return -1;
    if (e->as.call.args.len == 2) {
      const expr_t *message_expr = e->as.call.args.data[1].val;
      message = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (message >= 0 && !ny_native_nir_expr_is_cstr(b, message_expr)) {
        const char *format =
            ny_native_nir_expr_is_bool(b, message_expr)
                ? "rt_bool_to_cstr"
            : ny_native_nir_expr_is_f64(b, message_expr)
                ? "rt_f64_to_cstr_raw"
            : ny_native_nir_expr_is_f32(b, message_expr)
                ? "rt_f64_to_cstr_raw"
            : (message_expr &&
               ((message_expr->kind == NY_E_LITERAL &&
                 message_expr->as.literal.kind == NY_LIT_INT) ||
                message_expr->semantic.rep == NY_SEM_REP_RAW_INT))
                ? "rt_i64_to_cstr_raw"
                : "rt_any_to_cstr";
        if (ny_native_nir_expr_is_f32(b, message_expr))
          message = ny_native_nir_emit_f32_to_f64(b, message);
        if (message >= 0)
          message =
              ny_native_nir_emit_runtime_call(b, format, message, -1, -1, 1, 0);
      }
    } else {
      message = ny_native_nir_emit_const(b, 0);
    }
    if (message < 0)
      return -1;
    int result = ny_native_nir_emit_runtime_call(b, "rt_assert_cstr",
                                                 condition, message, -1, 2, 0);
    return result < 0 ? -1 : ny_native_nir_emit_const(b, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_PRINT &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len == 0) {
      ny_native_nir_fail(
          b,
          "native NYIR lower: print requires at least one positional argument");
      return -1;
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        ny_native_nir_fail(
            b, "native NYIR lower: print accepts positional arguments only");
        return -1;
      }
      const expr_t *arg = e->as.call.args.data[i].val;
      bool is_bool = ny_native_nir_expr_is_bool(b, arg);
      if (i > 0) {
        int space_ptr = ny_native_nir_emit_cstr_const(b, " ");
        if (space_ptr >= 0)
          (void)ny_native_nir_emit_runtime_call(b, "rt_print_cstr", space_ptr,
                                                -1, -1, 1, 0);
      }
      bool is_f64_arg = !is_bool && ny_native_nir_expr_is_f64(b, arg);
      bool is_f32_arg = !is_bool && ny_native_nir_expr_is_f32(b, arg);
      bool is_list_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                         ny_native_nir_expr_is_list(b, arg);
      if (!is_list_arg && arg && arg->kind == NY_E_IDENT &&
          arg->as.ident.name) {
        const ny_native_nir_local_t *arg_local =
            ny_native_nir_find_local(b, arg->as.ident.name);
        is_list_arg = arg_local && arg_local->is_list;
      }
      bool is_any_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                        !is_list_arg && !ny_native_nir_expr_is_cstr(b, arg) &&
                        ny_native_nir_expr_is_any(b, arg);
      bool is_raw_int_arg =
          !is_bool && !is_f64_arg && !is_f32_arg && !is_list_arg && arg &&
          arg->semantic.resolved && arg->semantic.rep == NY_SEM_REP_RAW_INT;
      if (getenv("NYDBG7") && arg && arg->kind == NY_E_MEMCALL)
        fprintf(stderr, "DBG printmc method=%s any=%d rawint=%d tgt_bytes=%d\n",
                arg->as.memcall.name ? arg->as.memcall.name : "-",
                (int)is_any_arg, (int)is_raw_int_arg,
                arg->as.memcall.target
                    ? (int)ny_native_nir_expr_is_bytes(b, arg->as.memcall.target)
                    : -1);
      if (!is_raw_int_arg && arg && arg->kind == NY_E_CALL &&
          arg->as.call.callee && arg->as.call.callee->kind == NY_E_IDENT &&
          arg->as.call.callee->as.ident.name) {
        const char *callee_name = arg->as.call.callee->as.ident.name;
        const stmt_t *value_fn =
            ny_native_nir_find_user_function(b, callee_name);
        if (!value_fn) {
          const expr_t *global =
              ny_native_nir_find_top_level_value(b, callee_name);
          ny_native_lambda_entry_t *lambda =
              global ? ny_native_lambda_find(global) : NULL;
          if (lambda)
            value_fn = lambda->fn;
        }
        is_raw_int_arg = value_fn &&
                         ny_native_type_name_is_int(
                             value_fn->as.fn.return_type);
      }
      if (!is_any_arg && arg && arg->kind == NY_E_IDENT && arg->as.ident.name) {
        const ny_native_nir_local_t *arg_local =
            ny_native_nir_find_local(b, arg->as.ident.name);
        is_any_arg = arg_local && arg_local->is_any;
      }
      int raw = ny_native_nir_lower_expr(b, arg);
      if (raw < 0)
        return -1;
      if (is_bool) {
        raw = ny_native_nir_emit_runtime_call(b, "rt_bool_to_cstr", raw,
                                              -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (is_f32_arg) {
        raw = ny_native_nir_emit_f32_to_f64(b, raw);
        if (raw < 0)
          return -1;
      } else if (is_list_arg) {
        /* Native lists are raw tbuf handles, not tagged heap lists.  Printing
         * them through the scalar i64 entry point exposes the allocation
         * address; materialize the representation-aware list text first. */
        raw = ny_native_nir_emit_runtime_call(b, "rt_tbuf_to_cstr", raw,
                                              -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (is_any_arg) {
        /* An `any`-classified argument carries the tagged dynamic ABI even
         * when inference also refined it to an integer: its storage slot is
         * tagged (is_any local / dynamic-return call), so decoding here is
         * the one correct render.  A genuinely raw integer local is never
         * is_any and still takes the raw branch below. */
        raw = ny_native_nir_emit_runtime_call(b, "rt_any_to_cstr", raw,
                                              -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (is_raw_int_arg) {
        raw = ny_native_nir_emit_runtime_call(b, "rt_i64_to_cstr_raw", raw,
                                              -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      }
      const char *print_sym = is_bool || ny_native_nir_expr_is_cstr(b, arg) ||
                                      is_list_arg || is_raw_int_arg || is_any_arg
                                  ? "rt_print_cstr"
                              : (is_f64_arg || is_f32_arg) ? "rt_print_f64_raw"
                                                           : "rt_print_i64_raw";
      if (ny_native_nir_emit_runtime_call(b, print_sym, raw, -1, -1, 1, 0) < 0)
        return -1;
    }
    if (nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CALL,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .imm = 0,
                                          .flags = NYIR_INST_F_EXTERN,
                                          .symbol = "rt_print_newline"}) < 0) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
    return ny_native_nir_emit_const(b, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_FLOAT) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: float requires one positional argument");
      return -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg);
    if (value < 0)
      return -1;
    if (ny_native_nir_expr_is_f64(b, arg))
      return value;
    if (ny_native_nir_expr_is_any(b, arg))
      return ny_native_nir_emit_runtime_call(b, "rt_any_to_f64", value,
                                             -1, -1, 1, NYIR_INST_F_RET_F64);
    return ny_native_nir_emit_i64_to_f64(b, value);
  }
  if (leaf_kind == NY_NATIVE_LEAF_ARGC &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 0) {
      ny_native_nir_fail(b, "native NYIR lower: %s takes no arguments", leaf);
      return -1;
    }
    /*
     * rt_argc returns the tagged VM integer; native NYIR integers are raw i64.
     */
    int tagged =
        ny_native_nir_emit_runtime_call(b, "rt_argc", -1, -1, -1, 0, 0);
    int one = tagged < 0 ? -1 : ny_native_nir_emit_const(b, 1);
    return one < 0 ? -1
                   : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (leaf && (strcmp(leaf, "__argv") == 0 || strcmp(leaf, "argv") == 0) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name)
      return ny_native_nir_fail(b, "native NYIR lower: %s takes one index",
                                leaf);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return index < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_argv_get",
                                                       index, -1, -1, 1, 0);
  }
  if (leaf && (strcmp(leaf, "__envc") == 0 || strcmp(leaf, "envc") == 0) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 0)
      return ny_native_nir_fail(b, "native NYIR lower: %s takes no arguments",
                                leaf);
    return ny_native_nir_emit_runtime_call(b, "rt_envc_raw", -1, -1, -1, 0,
                                           0);
  }
  if (leaf &&
      (strcmp(leaf, "__env_get") == 0 || strcmp(leaf, "env_get") == 0) &&
      !ny_native_nir_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name)
      return ny_native_nir_fail(
          b, "native NYIR lower: %s takes one positional argument", leaf);
    int key = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    return key < 0 ? -1
                   : ny_native_nir_emit_runtime_call(b, "rt_env_get",
                                                     key, -1, -1, 1, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_TICKS) {
    if (e->as.call.args.len != 0) {
      ny_native_nir_fail(b, "native NYIR lower: ticks takes no arguments");
      return -1;
    }
    return ny_native_nir_emit_runtime_call(b, "rt_ticks_ns", -1, -1, -1, 0, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_FLT_SQRT) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: __flt_sqrt requires one positional argument");
      return -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = ny_native_nir_lower_expr(b, arg);
    if (value < 0)
      return -1;
    if (!ny_native_nir_expr_is_f64(b, arg)) {
      value = ny_native_nir_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    return ny_native_nir_push_val(b, NYIR_SQRT_F64, value, -1, 0, NULL);
  }
  if (leaf_kind == NY_NATIVE_LEAF_ADDR && leaf && strcmp(leaf, "borrow") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name ||
        !e->as.call.args.data[0].val) {
      ny_native_nir_fail(b,
                         "native NYIR lower: borrow requires one expression");
      return -1;
    }
    /*
     * `&x` parses to borrow(x).  A borrow of a dereferenced pointer, of a
     * heap reference (list/str/any), or of a non-lvalue expression is the
     * identity and yields the reference itself.  A borrow of a scalar local
     * or global is the value's address and falls through to the shared
     * addr_of handling below.
     */
    const expr_t *target = e->as.call.args.data[0].val;
    if (target->kind == NY_E_DEREF)
      return ny_native_nir_lower_expr(b, target->as.deref.target);
    if (target->kind == NY_E_IDENT) {
      ny_native_nir_local_t *l =
          ny_native_nir_find_local(b, target->as.ident.name);
      if (l && (l->is_list || l->is_cstr || l->is_any))
        return ny_native_nir_lower_expr(b, target);
    } else {
      return ny_native_nir_lower_expr(b, target);
    }
  }
  if (leaf_kind == NY_NATIVE_LEAF_ADDR) {
    const expr_t *target = e->as.call.args.data[0].val;
    if (target->kind == NY_E_DEREF)
      return ny_native_nir_lower_expr(b, target->as.deref.target);
    if (target->kind != NY_E_IDENT) {
      ny_native_nir_fail(
          b,
          "native NYIR lower: %s supports local and dereferenced pointer "
          "lvalues, not expression kind %d at %s:%d in %s",
          leaf, (int)target->kind,
          target->tok.filename ? target->tok.filename : "<source>",
          target->tok.line,
          b->current_fn_name ? b->current_fn_name : "<unknown>");
      return -1;
    }
    const char *local_name = target->as.ident.name;
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, local_name);
    if (!l) {
      int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .imm = 0,
                                                .symbol = local_name});
      if (v < 0)
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return v;
    }
    return ny_native_nir_emit_addr_local(b, l->slot, local_name);
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_NEW) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: f64buf_new requires one positional length");
      return -1;
    }
    int count = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int width = ny_native_nir_emit_const(b, 8);
    if (count < 0 || width < 0)
      return -1;
    int out = ny_native_nir_emit_runtime_call(b, "rt_tbuf_new_raw", count,
                                              width, -1, 2, 0);
    int64_t const_count = 0;
    if (ny_native_nir_fold_top_level_int(b->prog, e->as.call.args.data[0].val,
                                         &const_count, 0) &&
        const_count > 0 && out >= 0)
      ny_native_nir_record_alloc_fact(b, out, const_count * 8);
    return out;
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_LOAD) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: f64buf_load requires buffer and index");
      return -1;
    }
    /*
     * Fin-typed index elision: skip bounds check when index is Fin<N>
     * and N <= buffer byte length (comptime-known).
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (data < 0 || index < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1
                            : ny_native_nir_push_val(b, NYIR_SHL_I64, index,
                                                     shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len))
      ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len);
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 ? -1 : ny_native_nir_emit_load_f64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_STORE) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(
          b,
          "native NYIR lower: f64buf_store requires buffer, index, and value");
      return -1;
    }
    /*
     * Fin-typed index elision.
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    if (data < 0 || index < 0 || value < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1
                            : ny_native_nir_push_val(b, NYIR_SHL_I64, index,
                                                     shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len))
      ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len);
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0
               ? -1
               : (ny_native_nir_emit_store_f64(b, addr, value) ? value : -1);
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_NEW) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: i64buf_new requires one positional length");
      return -1;
    }
    int64_t const_count = 0;
    /* Do not replace `i64buf_new` with a bare stack allocation.  The public
     * typed-buffer contract guarantees zero-initialized elements; an alloca
     * exposes indeterminate stack bytes and corrupts histogram/accumulator
     * workloads before their first store. */
    int count = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int width = ny_native_nir_emit_const(b, 8);
    if (count < 0 || width < 0)
      return -1;
    int out = ny_native_nir_emit_runtime_call(b, "rt_tbuf_new_raw", count,
                                              width, -1, 2, 0);
    if (const_count > 0 && out >= 0)
      ny_native_nir_record_alloc_fact(b, out, const_count * 8);
    return out;
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_LOAD) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(
          b, "native NYIR lower: i64buf_load requires buffer and index");
      return -1;
    }
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    if (data < 0 || index < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1
                            : ny_native_nir_push_val(b, NYIR_SHL_I64, index,
                                                     shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len) &&
        !ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len))
      return -1;
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_STORE) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(
          b,
          "native NYIR lower: i64buf_store requires buffer, index, and value");
      return -1;
    }
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int data = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    if (data < 0 || index < 0 || value < 0)
      return -1;
    int shift3 = ny_native_nir_emit_const(b, 3);
    int offset = shift3 < 0 ? -1
                            : ny_native_nir_push_val(b, NYIR_SHL_I64, index,
                                                     shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len) &&
        !ny_native_nir_emit_bounds_check(b, data, offset, buf_byte_len))
      return -1;
    int addr = ny_native_nir_emit_add_i64(b, data, offset);
    return addr < 0 || !ny_native_nir_emit_store_i64(b, addr, value) ? -1
                                                                     : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD8) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      ny_native_nir_fail(b, "load8 requires pointer and byte offset");
      return -1;
    }
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int effective = (addr < 0 || index < 0)
                        ? -1
                        : ny_native_nir_emit_add_i64(b, addr, index);
    return effective < 0 ? -1 : ny_native_nir_emit_load8(b, effective);
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE8) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      ny_native_nir_fail(b, "store8 requires pointer, value, and byte offset");
      return -1;
    }
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
    int index = ny_native_nir_lower_expr(b, e->as.call.args.data[2].val);
    int effective = (addr < 0 || index < 0)
                        ? -1
                        : ny_native_nir_emit_add_i64(b, addr, index);
    return effective < 0 || value < 0 ||
                   !ny_native_nir_emit_store8(b, effective, value)
               ? -1
               : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD32) {
    bool intrinsic = leaf && strcmp(leaf, "__load32_idx") == 0;
    if (e->as.call.args.len < (intrinsic ? 2u : 1u) ||
        e->as.call.args.len > 2 || e->as.call.args.data[0].name ||
        (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
      ny_native_nir_fail(
          b, "native NYIR lower: load32 requires pointer and optional offset");
      return -1;
    }
    int base = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int offset = e->as.call.args.len == 2
                     ? ny_native_nir_lower_expr(b, e->as.call.args.data[1].val)
                     : ny_native_nir_emit_const(b, 0);
    if (base < 0 || offset < 0)
      return -1;
    int addr = ny_native_nir_emit_add_i64(b, base, offset);
    int zero = ny_native_nir_emit_const(b, 0);
    int tagged = addr < 0 || zero < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_load32_idx", addr,
                                                       zero, -1, 2, 0);
    int one = tagged < 0 ? -1 : ny_native_nir_emit_const(b, 1);
    return one < 0 ? -1
                   : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD64 ||
      leaf_kind == NY_NATIVE_LEAF_LOAD64_IDX) {
    if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
      ny_native_nir_fail(b, "native NYIR lower: load64/load64_i/load64_h "
                            "require positional pointer and optional offset");
      return -1;
    }
    /*
     * Fin-typed offset elision: resolve buffer byte length from the ptr
     * argument, then check whether the byte-offset arg has a Fin type.
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (addr < 0)
      return -1;
    if (e->as.call.args.len > 1) {
      int off = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
      if (off < 0)
        return -1;
      if (!ny_native_nir_index_fin_bound_elision(b, e, 1, buf_byte_len))
        ny_native_nir_emit_bounds_check(b, addr, off, buf_byte_len);
      addr = ny_native_nir_emit_add_i64(b, addr, off);
      if (addr < 0)
        return -1;
    }
    return ny_native_nir_emit_load_i64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE64 ||
      leaf_kind == NY_NATIVE_LEAF_STORE64_H ||
      leaf_kind == NY_NATIVE_LEAF_STORE64_IDX) {
    /* Public store64_h(p, v, offset=0) keeps the ergonomic two-argument
     * form; only the underscored/index primitives require the explicit
     * pointer, offset, value ordering. */
    bool intrinsic_order = leaf && (strcmp(leaf, "__store64_h") == 0 ||
                                    strcmp(leaf, "__store64_idx") == 0);
    if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
        (intrinsic_order && e->as.call.args.len != 3) ||
        e->as.call.args.data[0].name || e->as.call.args.data[1].name ||
        (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
      ny_native_nir_fail(b, "native NYIR lower: store64_i/store64_h require "
                            "positional pointer, value, and optional offset");
      return -1;
    }
    size_t val_idx = intrinsic_order ? 2u : 1u;
    size_t off_idx = intrinsic_order ? 1u : 2u;
    /*
     * Fin-typed offset elision.
     */
    int64_t buf_byte_len = ny_native_nir_resolve_buf_byte_len(b, e);
    int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[val_idx].val);
    if (addr < 0 || value < 0)
      return -1;
    /* `store64_i` writes raw scalar bits. Large integer literals are
     * represented as boxed bigints by the language literal lowering; unwrap
     * that representation before writing the C/FFI memory slot. */
    const expr_t *value_expr = e->as.call.args.data[val_idx].val;
    /* store64 writes a raw slot.  Dynamic producers (callbacks, sequence
     * reads, and `any` locals) carry tagged integers at this boundary; decode
     * them once while preserving pointer/string/float handles. */
    if (value_expr && ny_native_nir_expr_is_any(b, value_expr)) {
      value = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", value,
                                              -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (leaf && strcmp(leaf, "store64_i") == 0 && value_expr &&
        value_expr->kind == NY_E_LITERAL &&
        value_expr->as.literal.kind == NY_LIT_INT &&
        (value_expr->as.literal.as.i >= (INT64_C(1) << 62) ||
         value_expr->as.literal.as.i <= -(INT64_C(1) << 62))) {
      value = ny_native_nir_emit_runtime_call(b, "rt_bigint_to_i64_raw", value,
                                              -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (e->as.call.args.len > off_idx) {
      int off = ny_native_nir_lower_expr(b, e->as.call.args.data[off_idx].val);
      if (off < 0)
        return -1;
      if (!ny_native_nir_index_fin_bound_elision(b, e, off_idx, buf_byte_len))
        ny_native_nir_emit_bounds_check(b, addr, off, buf_byte_len);
      addr = ny_native_nir_emit_add_i64(b, addr, off);
      if (addr < 0)
        return -1;
    }
    if (!ny_native_nir_emit_store_i64(b, addr, value))
      return -1;
    return ny_native_nir_emit_const(b, 0);
  }
  if (leaf && !ny_native_nir_user_defined_fn(b, name) &&
      (strcmp(leaf, "__layout_size") == 0 ||
       strcmp(leaf, "__layout_align") == 0 ||
       strcmp(leaf, "__layout_offset") == 0) &&
      ((strcmp(leaf, "__layout_offset") == 0 && e->as.call.args.len == 2) ||
       (strcmp(leaf, "__layout_offset") != 0 && e->as.call.args.len == 1)) &&
      e->as.call.args.data[0].val &&
      e->as.call.args.data[0].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[0].val->as.literal.kind == NY_LIT_STR &&
      (strcmp(leaf, "__layout_offset") != 0 ||
       (e->as.call.args.data[1].val &&
        e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR))) {
    const char *layout = e->as.call.args.data[0].val->as.literal.as.s.data;
    if (strcmp(leaf, "__layout_offset") == 0) {
      const char *field = e->as.call.args.data[1].val->as.literal.as.s.data;
      size_t offset = 0;
      if (layout && field &&
          ny_native_nir_ast_layout_query(b, layout, field, NULL, NULL, &offset))
        return ny_native_nir_emit_const(b, (int64_t)offset);
      ny_native_nir_fail(b, "native NYIR lower: unknown layout field '%s.%s'",
                         layout ? layout : "", field ? field : "");
      return -1;
    }
    size_t size = 0;
    size_t align = 0;
    if (!layout ||
        !ny_native_nir_ast_layout_query(b, layout, NULL, &size, &align, NULL)) {
      ny_native_nir_fail(b, "native NYIR lower: unknown layout '%s'",
                         layout ? layout : "");
      return -1;
    }
    return ny_native_nir_emit_const(
        b, (int64_t)(strcmp(leaf, "__layout_align") == 0 ? align : size));
  }
  if (leaf && strcmp(leaf, "store_layout") == 0 && e->as.call.args.len >= 3 &&
      e->as.call.args.data[1].val &&
      e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR) {
    const char *layout = e->as.call.args.data[1].val->as.literal.as.s.data;
    size_t fields = 0;
    const ny_layout_field_list *generic_fields = NULL;
    if (layout) {
      const stmt_t *decl = NULL;
      for (size_t j = 0; b->prog && j < b->prog->body.len && !decl; ++j)
        decl = ny_native_nir_find_layout_stmt(b->prog->body.data[j], layout);
      if (decl && (decl->kind == NY_S_LAYOUT || decl->kind == NY_S_STRUCT)) {
        generic_fields = decl->kind == NY_S_LAYOUT ? &decl->as.layout.fields
                                                   : &decl->as.struc.fields;
        fields = generic_fields->len;
      }
    }
    if (!generic_fields)
      fields = 0;
    if (!fields || e->as.call.args.len != fields + 2) {
      ny_native_nir_fail(b, "native NYIR lower: invalid store_layout '%s'",
                         layout ? layout : "");
      return -1;
    }
    int base = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (base < 0)
      return -1;
    for (size_t i = 0; i < fields; ++i) {
      int value = ny_native_nir_lower_expr(b, e->as.call.args.data[i + 2].val);
      int64_t raw_offset = 0;
      size_t field_size = 8;
      const char *field_type = NULL;
      if (generic_fields) {
        const layout_field_t *field = &generic_fields->data[i];
        size_t field_align = 0;
        size_t field_offset = 0;
        if (field->is_array || !field->name ||
            !ny_native_nir_ast_layout_query(b, layout, field->name, &field_size,
                                            &field_align, &field_offset)) {
          ny_native_nir_fail(
              b,
              "native NYIR lower: unsupported store_layout field %zu in '%s'",
              i, layout ? layout : "");
          return -1;
        }
        raw_offset = (int64_t)field_offset;
        field_type = field->type_name;
      }
      int offset = ny_native_nir_emit_const(b, raw_offset);
      if (value < 0 || offset < 0)
        return -1;
      bool wide = field_size >= 8;
      if (generic_fields && field_type &&
          ny_native_type_name_is_f64(field_type)) {
        int addr = ny_native_nir_emit_add_i64(b, base, offset);
        if (addr < 0 || !ny_native_nir_emit_store_f64(b, addr, value))
          return -1;
        continue;
      }
      if (generic_fields && field_type &&
          ny_native_type_name_is_f32(field_type)) {
        int f64 = ny_native_nir_expr_is_f32(b, e->as.call.args.data[i + 2].val)
                      ? ny_native_nir_emit_f32_to_f64(b, value)
                      : value;
        if (f64 < 0 ||
            ny_native_nir_emit_runtime_call(b, "rt_store32_f64", base,
                                            offset, f64, 3, 0) < 0)
          return -1;
        continue;
      }
      if (generic_fields && !wide && field_type &&
          !ny_native_type_name_is_str(field_type)) {
        int one = ny_native_nir_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = value,
                                                        .b = one});
        value = shifted < 0
                    ? -1
                    : ny_native_nir_emit_binop(b, NYIR_OR_I64, shifted, one);
        if (value < 0)
          return -1;
      }
      if (ny_native_nir_emit_runtime_call(
              b, wide ? "rt_store64_idx" : "rt_store32_idx", base, offset,
              value, 3, 0) < 0)
        return -1;
    }
    return ny_native_nir_emit_const(b, 0);
  }
  if (e->as.call.args.len > NYIR_CALL_MAX_ARGS) {
    ny_native_nir_fail(b,
                       "native NYIR lower: call exceeds the maximum supported "
                       "argument count (%d)",
                       NYIR_CALL_MAX_ARGS);
    return -1;
  }
  const char *saved_source_file = b->source_file;
  if (e->tok.filename)
    b->source_file = e->tok.filename;
  const stmt_t *callee_fn = direct_callee_fn
                                ? direct_callee_fn
                                : ny_native_nir_find_user_function(b, name);
  ny_native_nir_local_t *callable_local = NULL;
  if (e->as.call.callee->kind == NY_E_IDENT)
    callable_local =
        ny_native_nir_find_local(b, e->as.call.callee->as.ident.name);
  const expr_t *callable_global = e->as.call.callee->kind == NY_E_IDENT
                                      ? ny_native_nir_find_top_level_value(
                                            b, e->as.call.callee->as.ident.name)
                                      : NULL;
  /* A same-source function definition shadows a foreign top-level value.
   * Imported `#main`/self-test blocks register short bindings (such as `p`
   * or `q`) that are visible to user programs; without this the foreign
   * initializer drives the indirect-callable decision below and a user call
   * lowers to a dynamic load of unrelated module state. */
  if (callee_fn && callable_global && !callee_fn->as.fn.is_extern &&
      !callee_fn->as.fn.link_name && !ny_is_stdlib_tok(callee_fn->tok) &&
      !ny_native_nir_same_source_file(callable_global->tok.filename,
                                      b->source_file))
    callable_global = NULL;
  ny_native_lambda_entry_t *local_lambda =
      callable_local && callable_local->callable_expr
          ? ny_native_lambda_find(callable_local->callable_expr)
          : NULL;
  ny_native_lambda_entry_t *global_lambda =
      callable_global ? ny_native_lambda_find(callable_global) : NULL;
  if (!callee_fn && global_lambda && global_lambda->fn && global_lambda->name) {
    callee_fn = global_lambda->fn;
    name = global_lambda->name;
    leaf = ny_native_leaf_name(name);
  }
  bool known_raw_callable =
      (local_lambda && local_lambda->capture_count == 0) ||
      (global_lambda && global_lambda->capture_count == 0) ||
      (callable_local && callable_local->callable_name) ||
      (callable_global && callable_global->kind == NY_E_IDENT &&
       callable_global->as.ident.name &&
       ny_native_nir_find_user_function(b, callable_global->as.ident.name));
  bool indirect_callable =
      indirect_lambda ||
      (!known_raw_callable &&
       e->as.call.callee->semantic.rep == NY_SEM_REP_CLOSURE) ||
      (local_lambda && local_lambda->capture_count > 0) ||
      (!known_raw_callable && callable_global &&
       callable_global->semantic.rep == NY_SEM_REP_CLOSURE) ||
      /* A top-level value initialized by a function call is still a
       * callable closure, even when inference has not propagated the
       * closure representation through the initializer expression. */
      (!known_raw_callable && callable_global &&
       callable_global->kind == NY_E_CALL) ||
      (!known_raw_callable && callable_local && callable_global &&
       callable_global->kind == NY_E_CALL) ||
      /* Values loaded from a dynamic registry are callable at runtime even
       * though semantic analysis cannot attach a closure representation to
       * the local (`handler(...)`, `predicate(...)`). */
      (!known_raw_callable && callable_local && callable_local->is_any) ||
      /* `fnptr` parameters are callable values too; unlike `any`, their
       * local slot is not marked dynamic, so use the semantic closure fact
       * to select the trampoline path instead of treating the name as a
       * missing global function. */
      (!known_raw_callable && callable_local &&
       callable_local->semantic_rep == NY_SEM_REP_CLOSURE);
  /* The call token's source is needed only while resolving its callee and
   * globals.  Restore lexical source scope before lowering arguments or
   * taking the indirect-call early return; leaking an imported filename here
   * makes subsequent module aliases resolve against the callee's file. */
  b->source_file = saved_source_file;
  if (indirect_callable) {
    if (e->as.call.args.len > 2)
      return ny_native_nir_fail(
          b,
          "native NYIR: captured closure calls support at most two arguments");
    int callable = ny_native_nir_lower_expr(b, e->as.call.callee);
    if (callable < 0)
      return -1;
    int argv[4] = {-1, -1, -1, -1};
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name)
        return ny_native_nir_fail(
            b,
            "native NYIR: captured closure calls require positional arguments");
      const expr_t *arg_expr = e->as.call.args.data[i].val;
      bool indexed_any_arg = arg_expr && arg_expr->kind == NY_E_INDEX;
      bool saved_index_for_any_call = b->index_for_any_call;
      b->index_for_any_call = arg_expr && arg_expr->kind == NY_E_INDEX;
      argv[i] = ny_native_nir_lower_expr(b, arg_expr);
      b->index_for_any_call = saved_index_for_any_call;
      if (argv[i] < 0)
        return -1;
      /* Typed sequence accessors return the stored raw payload.  That
       * payload may be another list/form pointer, so it must cross an `any`
       * callback boundary unchanged; shifting it as a native integer turns
       * the pointer into an invalid value and makes nested syntax expansion
       * silently skip the form. */
      const expr_t *raw_sequence_target = NULL;
      const char *raw_sequence_method = NULL;
      if (arg_expr && arg_expr->kind == NY_E_MEMCALL) {
        raw_sequence_target = arg_expr->as.memcall.target;
        raw_sequence_method = arg_expr->as.memcall.name;
      } else if (arg_expr && arg_expr->kind == NY_E_CALL &&
                 arg_expr->as.call.callee &&
                 arg_expr->as.call.callee->kind == NY_E_MEMBER) {
        raw_sequence_target = arg_expr->as.call.callee->as.member.target;
        raw_sequence_method = arg_expr->as.call.callee->as.member.name;
      }
      bool raw_sequence_value =
          raw_sequence_target && raw_sequence_method &&
          (strcmp(raw_sequence_method, "get") == 0 ||
           strcmp(raw_sequence_method, "pop") == 0) &&
          (ny_native_nir_expr_is_list(b, raw_sequence_target) ||
           ny_native_nir_expr_is_bytes(b, raw_sequence_target) ||
           ny_native_nir_expr_is_range(b, raw_sequence_target));
      /* rt_callN consumes the legacy tagged value ABI; native scalar closure
       * bodies themselves receive the unboxed value after the trampoline.
       * A local whose initializer was a seq/dyn-list index read stores the
       * CANONICAL dynamic word (rt_tbuf_index_any_raw), so the raw-int
       * semantic guess must not shift-tag it a second time: `def v = xs[i]`
       * handed find_if's predicate tagged(10) as tagged(tagged(10)) and the
       * predicate compared 21 > 15. */
      const ny_native_nir_local_t *scalar_local =
          arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
              ? ny_native_nir_find_local(b, arg_expr->as.ident.name)
              : NULL;
      bool scalar_local_arg =
          scalar_local && !scalar_local->raw_dynamic_index &&
          scalar_local->semantic_rep == NY_SEM_REP_RAW_INT;
      if ((!ny_native_nir_expr_is_any(b, arg_expr) || scalar_local_arg) &&
          !ny_native_nir_expr_is_cstr(b, arg_expr) &&
          !ny_native_nir_expr_is_dict(b, arg_expr) &&
          !ny_native_nir_expr_is_list(b, arg_expr) &&
          !ny_native_nir_expr_is_bytes(b, arg_expr) &&
          !ny_native_nir_expr_is_range(b, arg_expr) && !raw_sequence_value &&
          !indexed_any_arg &&
          !ny_native_nir_expr_is_f64(b, arg_expr) &&
          !ny_native_nir_expr_is_f32(b, arg_expr)) {
        int one = ny_native_nir_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = argv[i],
                                                        .b = one});
        argv[i] = shifted < 0
                      ? -1
                      : ny_native_nir_emit_binop(b, NYIR_OR_I64, shifted, one);
      }
      if (argv[i] < 0)
        return -1;
    }
    const char *call_symbol = e->as.call.args.len == 0   ? "rt_call0"
                              : e->as.call.args.len == 1 ? "rt_call_any1"
                              : e->as.call.args.len == 2 ? "rt_call_any2"
                                                         : "rt_call2";
    int result = ny_native_nir_emit_runtime_call(
        b, call_symbol, callable, argv[0], argv[1],
        (int)e->as.call.args.len + 1, 0);
    if (result < 0)
      return -1;
    /* The callback trampoline returns the canonical tagged dynamic value.
     * Keep that representation through surrounding comparisons/arithmetic;
     * untagging here made an odd raw scalar look like a different integer at
     * the next any boundary. */
    return result;
  }
  /* A function-valued local is still a direct call when its initializer is a
   * non-capturing lambda.  Resolve it before ABI expansion so the generated
   * call uses the lambda's real signature and symbol. */
  if (!callee_fn && e->as.call.callee->kind == NY_E_IDENT) {
    ny_native_nir_local_t *local =
        ny_native_nir_find_local(b, e->as.call.callee->as.ident.name);
    if (local && local->callable_expr) {
      ny_native_lambda_entry_t *entry =
          ny_native_lambda_find(local->callable_expr);
      if (entry && entry->fn && entry->name) {
        callee_fn = entry->fn;
        name = entry->name;
        leaf = ny_native_leaf_name(name);
      }
    }
    if (!callee_fn && local && local->callable_name) {
      const stmt_t *aliased_fn =
          ny_native_nir_find_user_function(b, local->callable_name);
      if (aliased_fn) {
        callee_fn = aliased_fn;
        name = aliased_fn->as.fn.name;
        leaf = ny_native_leaf_name(name);
      }
    }
    /* Top-level named function aliases are materialized in the global data
     * table, but their immutable initializer still gives us the exact
     * callable signature.  Resolve that initializer before falling through
     * to an indirect/raw call, which NYIR intentionally does not model yet. */
    if (!callee_fn) {
      const expr_t *global = ny_native_nir_find_top_level_value(
          b, e->as.call.callee->as.ident.name);
      if (global && global->kind == NY_E_IDENT && global->as.ident.name) {
        const stmt_t *aliased_fn =
            ny_native_nir_find_user_function(b, global->as.ident.name);
        if (aliased_fn) {
          callee_fn = aliased_fn;
          name = aliased_fn->as.fn.name;
          leaf = ny_native_leaf_name(name);
        }
      }
    }
  }
  /* Expanded stdlib calls can preserve the provider module in an attached
   * method's canonical name (for example
   * `std.math.crypto.encoding.base.merge`) even though the receiver is a
   * native dictionary.  Resolve this representation before ordinary function
   * lookup; otherwise the valid dictionary operation is reported as a missing
   * function in the provider module. */
  if (leaf && strcmp(leaf, "merge") == 0 &&
      ((e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
        !e->as.call.args.data[1].name &&
        ny_native_nir_expr_is_dict(b, e->as.call.args.data[0].val)) ||
       (e->as.call.args.len == 1 && name &&
        strcmp(name, "std.math.crypto.encoding.base.merge") == 0 &&
        !e->as.call.args.data[0].name &&
        ny_native_nir_find_local(b, "base") != NULL))) {
    expr_t base_ident = {.kind = NY_E_IDENT, .tok = e->tok};
    base_ident.as.ident.name = "base";
    /* Canonical attached method calls carry the receiver as argument 0 and
     * the merge source as argument 1.  The one-argument compatibility form
     * carries only the source and uses the local `base` receiver above. */
    const expr_t *other_expr = e->as.call.args.len == 2
                                   ? e->as.call.args.data[1].val
                                   : e->as.call.args.data[0].val;
    int dict = e->as.call.args.len == 2
                   ? ny_native_nir_lower_expr(b, e->as.call.args.data[0].val)
                   : ny_native_nir_lower_expr(b, &base_ident);
    int other = ny_native_nir_lower_expr(b, other_expr);
    return dict < 0 || other < 0
               ? -1
               : ny_native_nir_emit_runtime_call(b, "rt_dict_merge_raw",
                                                 dict, other, -1, 2, 0);
  }
  /* The public std.os wrappers are interpreter-ABI functions (`any`), but
   * their only native operation is the scalar runtime sleep primitive.  A
   * native caller must not pass the wrapper's expanded value/length/tag
   * tuple to a NYIR function body: that body eventually reaches the tagged
   * runtime entry through the generic call gate.  Collapse the wrapper at
   * the native boundary and pass the raw scalar directly. */
  if (e->as.call.args.len == 1 && !e->as.call.args.data[0].name && leaf &&
      ((name && strncmp(name, "std.os.", 7) == 0) ||
       (callee_fn && callee_fn->as.fn.name &&
        strncmp(callee_fn->as.fn.name, "std.os.", 7) == 0)) &&
      (strcmp(leaf, "msleep") == 0 || strcmp(leaf, "sleep") == 0)) {
    int value = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    if (strcmp(leaf, "sleep") == 0) {
      int scale = ny_native_nir_emit_const(b, 1000);
      value = scale < 0 ? -1
                        : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_MUL_I64,
                                                            .dst = -1,
                                                            .a = value,
                                                            .b = scale});
    }
    return value < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_msleep_ms", value,
                                                       -1, -1, 1, 0);
  }
  if (leaf && strcmp(leaf, "load_layout") == 0 &&
      e->as.call.args.len == 3 &&
      e->as.call.args.data[1].val &&
      e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR &&
      e->as.call.args.data[2].val &&
      e->as.call.args.data[2].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[2].val->as.literal.kind == NY_LIT_STR) {
    const char *layout = e->as.call.args.data[1].val->as.literal.as.s.data;
    const char *field = e->as.call.args.data[2].val->as.literal.as.s.data;
    int64_t offset = -1;
    bool wide = false;
    size_t field_size = 4;
    size_t field_align = 0;
    size_t field_offset = 0;
    if (layout && field &&
        ny_native_nir_ast_layout_query(b, layout, field, &field_size,
                                       &field_align, &field_offset)) {
      offset = (int64_t)field_offset;
      wide = field_size >= 8;
    }
    if (offset < 0) {
      if (layout && field &&
          ny_native_nir_ast_layout_query(b, layout, field, &field_size,
                                         &field_align, &field_offset)) {
        offset = (int64_t)field_offset;
        wide = field_size >= 8;
      } else {
        ny_native_nir_fail(b, "native NYIR lower: unknown layout field '%s.%s'",
                           layout ? layout : "", field ? field : "");
        return -1;
      }
    }
    int base = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
    int off = ny_native_nir_emit_const(b, rt_tag_v(offset));
    if (base < 0 || off < 0)
      return -1;
    const char *load = wide              ? "rt_load64_idx"
                       : field_size == 1 ? "rt_load8_idx"
                       : field_size == 2 ? "rt_load16_idx"
                                         : "rt_load32_idx";
    int loaded = ny_native_nir_emit_runtime_call(b, load, base, off, -1, 2, 0);
    if (loaded < 0 || wide)
      return loaded;
    int one = ny_native_nir_emit_const(b, 1);
    if (one < 0)
      return -1;
    return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SAR_I64,
                                             .dst = -1,
                                             .a = loaded,
                                             .b = one});
  }
  const stmt_t *callee_ext_decl = NULL;
  for (size_t i = 0; b->prog && i < b->prog->body.len && !callee_ext_decl; ++i)
    callee_ext_decl = ny_native_nir_find_extern_decl_in_stmt(
        b->prog->body.data[i], name, b->options, 0);
  /*
   * Every non-extern callee returned by the program-wide finder is eligible
   * for native body collection, including functions nested in modules.  The
   * reachable-function pass owns that invariant; unresolved names are rejected
   * below before they can become undefined native symbols.  Builtin C
   * allocators and externs bypass this because they resolve by symbol.
   */
  bool runtime_builtin =
      ny_native_runtime_symbol_for_expr(name, leaf, e) != NULL;
  bool ffi_builtin = ny_native_ffi_symbol_name(name);
  bool declared_external =
      (b->externs && ny_extern_table_lookup(b->externs, name)) ||
      callee_ext_decl != NULL;
  /* User-defined `impl` types are nominal views over their runtime value.
   * Their one-argument constructor is an identity operation (for example
   * `SelfBox({"value": 5})`), so do not emit a fictitious function symbol. */
  if (!callee_fn && e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      ny_native_nir_find_impl_type(b, name))
    return ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
  if (!callee_fn && !runtime_builtin && !ffi_builtin && !declared_external &&
      ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE) {
    const char *keyword = ny_keyword_typo_suggestion(name);
    ny_native_nir_fail(
        b,
        "native NYIR lower: undefined symbol '%s'; no native body or supported "
        "external symbol at %s:%d (semantic call=%d callee=%s)%s%s%s",
        name, e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
        (int)e->semantic.member_call_kind,
        e->semantic.canonical_callee ? e->semantic.canonical_callee : "<none>",
        keyword ? "; did you mean keyword '" : "", keyword ? keyword : "",
        keyword ? "'?" : "");
    return -1;
  }
  call_arg_t normalized_args[NYIR_CALL_MAX_ARGS] = {0};
  call_arg_t normalized_defaults[NYIR_CALL_MAX_ARGS] = {0};
  call_arg_t *user_args = c ? c->args.data : NULL;
  size_t user_args_len = c ? c->args.len : 0;
  if (c) {
    bool has_named = false;
    for (size_t i = 0; i < c->args.len; ++i)
      has_named = has_named || c->args.data[i].name != NULL;
    if (has_named) {
      const ny_param_list *params = callee_fn ? &callee_fn->as.fn.params
                                    : callee_ext_decl
                                        ? &callee_ext_decl->as.ext.params
                                        : NULL;
      if (!params || params->len > NYIR_CALL_MAX_ARGS) {
        ny_native_nir_fail(
            b,
            "native NYIR lower: named call requires a bounded parameter list");
        return -1;
      }
      bool assigned[NYIR_CALL_MAX_ARGS] = {0};
      size_t next_pos = 0;
      for (size_t i = 0; i < c->args.len; ++i) {
        const call_arg_t *arg = &c->args.data[i];
        size_t param_idx = next_pos;
        if (arg->name) {
          param_idx = params->len;
          for (size_t j = 0; j < params->len; ++j) {
            if (params->data[j].name &&
                strcmp(params->data[j].name, arg->name) == 0) {
              param_idx = j;
              break;
            }
          }
          if (param_idx == params->len) {
            ny_native_nir_fail(
                b, "native NYIR lower: unknown named argument '%s'", arg->name);
            return -1;
          }
        } else {
          while (param_idx < params->len && assigned[param_idx])
            ++param_idx;
          if (param_idx == params->len) {
            ny_native_nir_fail(b, "native NYIR lower: too many call arguments");
            return -1;
          }
          next_pos = param_idx + 1;
        }
        if (assigned[param_idx]) {
          ny_native_nir_fail(b, "native NYIR lower: duplicate argument '%s'",
                             params->data[param_idx].name
                                 ? params->data[param_idx].name
                             : arg->name ? arg->name
                                         : "<positional>");
          return -1;
        }
        normalized_args[param_idx] = *arg;
        normalized_args[param_idx].name = NULL;
        assigned[param_idx] = true;
      }
      for (size_t i = 0; i < params->len; ++i) {
        if (assigned[i])
          continue;
        normalized_args[i].name = NULL;
        normalized_args[i].val = NULL;
        if (params->data[i].def) {
          normalized_defaults[i].val = params->data[i].def;
          normalized_args[i].val = normalized_defaults[i].val;
        }
      }
      user_args = normalized_args;
      user_args_len = params->len;
    }
  }
  /* Positional calls also need their trailing default expressions expanded.
   * Without this, `first_positive_int()` passed an uninitialized raw slot to
   * native code and could turn a requested one-frame run into an unbounded
   * loop. */
  if (callee_fn && c && c->args.len < callee_fn->as.fn.params.len) {
    bool has_named = false;
    for (size_t i = 0; i < c->args.len; ++i)
      has_named = has_named || c->args.data[i].name != NULL;
    if (!has_named) {
      if (callee_fn->as.fn.params.len > NYIR_CALL_MAX_ARGS)
        return ny_native_nir_fail(
            b, "native NYIR lower: defaulted call has too many parameters");
      for (size_t i = 0; i < callee_fn->as.fn.params.len; ++i) {
        normalized_args[i].name = NULL;
        normalized_args[i].val = i < c->args.len
                                     ? c->args.data[i].val
                                     : callee_fn->as.fn.params.data[i].def;
      }
      user_args = normalized_args;
      user_args_len = callee_fn->as.fn.params.len;
    }
  }
  const ny_extern_entry_t *ext =
      b->externs ? ny_extern_table_lookup(b->externs, name) : NULL;
  ny_extern_entry_t synthetic_ext;
  if (ext) {
    synthetic_ext = *ext;
  } else if (callee_ext_decl) {
    memset(&synthetic_ext, 0, sizeof(synthetic_ext));
    synthetic_ext.ny_name = callee_ext_decl->as.ext.name;
    synthetic_ext.c_symbol = callee_ext_decl->as.ext.link_name
                                 ? callee_ext_decl->as.ext.link_name
                                 : callee_ext_decl->as.ext.name;
    synthetic_ext.param_count = (unsigned)callee_ext_decl->as.ext.params.len;
  }
  if ((ext || callee_ext_decl) && synthetic_ext.ret_aggregate_size == 0) {
    const char *ret_type =
        callee_ext_decl ? callee_ext_decl->as.ext.return_type : NULL;
    size_t agg_size = 0, agg_align = 0;
    if (ret_type && ny_native_nir_ast_layout_query(b, ret_type, NULL, &agg_size,
                                                   &agg_align, NULL)) {
      synthetic_ext.ret_aggregate_size = agg_size;
      if (agg_size > 16) {
        synthetic_ext.ret_aggregate_classes[0] = NY_SYSV_AGG_MEMORY;
        synthetic_ext.ret_aggregate_classes[1] = NY_SYSV_AGG_NONE;
      } else if (agg_size > 0 && agg_size <= 8) {
        synthetic_ext.ret_aggregate_classes[0] = NY_SYSV_AGG_INTEGER;
        synthetic_ext.ret_aggregate_classes[1] = NY_SYSV_AGG_NONE;
      } else if (agg_size > 8 && agg_size <= 16) {
        synthetic_ext.ret_aggregate_classes[0] = NY_SYSV_AGG_INTEGER;
        synthetic_ext.ret_aggregate_classes[1] = NY_SYSV_AGG_INTEGER;
      }
    }
  }
  if (ext || callee_ext_decl)
    ext = &synthetic_ext;
  ny_builtin_alloc_kind_t builtin_kind = ny_builtin_alloc_kind(leaf);
  bool builtin_c_call = builtin_kind != NY_BUILTIN_ALLOC_NONE;
  bool user_defined_call = ny_native_nir_user_defined_fn(b, name);
  const char *runtime_symbol =
      ny_native_runtime_symbol_for_expr(name, leaf, e);
  if (user_defined_call)
    runtime_symbol = NULL;
  if (runtime_symbol &&
      strcmp(runtime_symbol, "rt_bigfloat_from_value_raw") == 0 &&
      e->as.call.args.len >= 1 &&
      ny_native_nir_expr_is_f64(b, e->as.call.args.data[0].val))
    runtime_symbol = "rt_bigfloat_from_f64_bits";
  bool runtime_c_call = runtime_symbol != NULL;
  bool declared_fn_call =
      callee_ext_decl != NULL || (callee_fn && callee_fn->as.fn.is_extern);
  bool ffi_c_call =
      !ext && !callee_fn && !callee_ext_decl && ny_native_ffi_symbol_name(name);
  bool is_c_call = ext != NULL || declared_fn_call || builtin_c_call ||
                   runtime_c_call || ffi_c_call;

  int args[NYIR_CALL_MAX_ARGS];
  size_t lowered_argc = 0;
  /* The raw dictionary bridge `rt_native_dict_set_nir_i64` deliberately takes
   * three raw i64 values. Do not append the legacy tagged-any
   * value/length/tag fields below: the bridge name is also visible to the
   * LLVM emitter, and mixing the two ABIs corrupts the following arguments.
   * Deciding by the RESOLVED runtime symbol (canonical callee first, spelling
   * only as a fallback) keeps aliases/attached methods on the raw ABI instead
   * of hardcoding a surface-name list. */
  bool raw_native_dict_set =
      runtime_c_call && runtime_symbol &&
      strcmp(runtime_symbol, "rt_native_dict_set_nir_i64") == 0;
  for (size_t i = 0; i < user_args_len; ++i) {
    const expr_t *arg_expr = user_args[i].val;
    bool arg_expr_f64 = ny_native_nir_expr_is_f64(b, arg_expr);
    bool arg_expr_f32 = ny_native_nir_expr_is_f32(b, arg_expr);
    int arg = arg_expr ? ny_native_nir_lower_expr(b, arg_expr)
                       : ny_native_nir_emit_const(b, 0);
    if (arg < 0)
      return -1;
    /* Runtime proof helpers require tagged strings, while native lowering
     * keeps literal C strings as raw pointers until a dynamic boundary. */
    bool proof_string_arg =
        runtime_symbol &&
        (strcmp(runtime_symbol, "rt_proof_cert_digest") == 0 ||
         strcmp(runtime_symbol, "rt_proof_cert_check") == 0) &&
        ((strcmp(runtime_symbol, "rt_proof_cert_digest") == 0 && i < 4) ||
         (strcmp(runtime_symbol, "rt_proof_cert_check") == 0 &&
          (i == 0 || i == 2 || i == 3 || i == 4)));
    if (proof_string_arg && ny_native_nir_expr_is_cstr(b, arg_expr))
      arg = ny_native_nir_emit_runtime_call(b, "rt_cstr_to_str", arg, -1,
                                            -1, 1, 0);
    const ny_param_list *callee_params = callee_fn ? &callee_fn->as.fn.params
                                         : callee_ext_decl
                                             ? &callee_ext_decl->as.ext.params
                                             : NULL;
    const ny_extern_entry_t *c_params =
        b->externs ? ny_extern_table_lookup(b->externs, name) : NULL;
    const char *param_type = callee_params && i < callee_params->len
                                 ? callee_params->data[i].type
                                 : NULL;
    const ny_expr_semantic_t *param_sem =
        callee_params && i < callee_params->len
            ? &callee_params->data[i].semantic
            : NULL;
    ny_sem_rep_t param_rep =
        param_sem && param_sem->resolved ? param_sem->rep : NY_SEM_REP_UNKNOWN;
    /* A declared bool parameter uses the raw predicate ABI (0/1), while
     * boolean literals lower to Ny's dynamic immediates (2/8). Normalize at
     * this call boundary so `if keep` does not treat false (2) as truthy. */
    if (param_type && strcmp(param_type, "bool") == 0 && arg_expr &&
        arg_expr->kind == NY_E_LITERAL &&
        arg_expr->as.literal.kind == NY_LIT_BOOL)
      arg = ny_native_nir_emit_const(b, arg_expr->as.literal.as.b ? 1 : 0);
    bool box_dynamic_argument =
        param_rep == NY_SEM_REP_TAGGED_DYNAMIC ||
        ny_native_type_name_is_any(param_type) ||
        (!param_type && param_rep == NY_SEM_REP_UNKNOWN);
    /* An unresolved untyped parameter used by a scalar operator is lowered
     * as a raw NYIR integer in the callee.  Match that body-derived ABI fact
     * at the call boundary instead of manufacturing tagged-any metadata. */
    if (box_dynamic_argument && callee_fn && !param_type &&
        ny_native_nir_untyped_param_is_raw(
            callee_fn, callee_params->data[i].name))
      box_dynamic_argument = false;
    /* The BigFloat f64 constructor is a typed native ABI entry point.  Its
     * first argument is an unboxed IEEE value (the lowering selects
     * rt_bigfloat_from_f64_bits for f64 expressions), not a dynamic float
     * handle.  Treating it as an `any` argument boxes the value first and
     * makes the callee reinterpret the handle bits as a double. */
    if (runtime_symbol &&
        strcmp(runtime_symbol, "rt_bigfloat_from_f64_bits") == 0 && i == 0)
      box_dynamic_argument = false;
    /* The exponent of the BigFloat integer-power bridge is already a raw
     * native integer at this call site.  When the intrinsic declaration is
     * untyped there is no parameter fact to suppress the generic dynamic
     * boxing pass; boxing here would tag an already-raw exponent a second
     * time (3 becomes 7) and compute a different power. */
    if (runtime_symbol &&
        strcmp(runtime_symbol, "rt_bigfloat_pow_int_raw") == 0 && i == 1)
      box_dynamic_argument = false;
    /*
     * __tbuf_index_any_raw's runtime entry (rt_tbuf_index_any_raw) takes the
     * raw index.  Boxing the loop counter here handed the reader tagged(0)=1
     * as the index, so every one-element list read inside mapcat panicked
     * with "index_read out of range" on the very first iteration.
     */
    if (box_dynamic_argument && runtime_symbol &&
        strcmp(runtime_symbol, "rt_tbuf_index_any_raw") == 0 && i == 1)
      box_dynamic_argument = false;

    /* Index reads default to the raw typed-slot ABI for arithmetic-heavy
     * lists.  A direct call whose formal is `any` is an explicit dynamic
     * boundary, however: re-lower through the descriptor-aware index path so
     * nested dictionaries/lists and scalar values retain their canonical
     * representation.  `.get(...)` has the equivalent handling below. */
    if (box_dynamic_argument && arg_expr && arg_expr->kind == NY_E_INDEX) {
      bool saved_index_for_any_call = b->index_for_any_call;
      b->index_for_any_call = true;
      arg = ny_native_nir_lower_expr(b, arg_expr);
      b->index_for_any_call = saved_index_for_any_call;
      if (arg < 0)
        return -1;
    }

    /* A typed list read is a raw slot payload.  When it is passed to an
     * `any` parameter, re-read it through the ABI-aware accessor so scalar
     * payloads are tagged exactly once while object/string handles remain
     * unchanged.  Calling the callee with the raw value makes a raw `1` look
     * like tagged integer zero (and a raw `0` like nil), which broke the
     * ring polynomial normalization path and other callback-style helpers. */
    const expr_t *raw_get_target = NULL;
    const expr_t *raw_get_index = NULL;
    if (box_dynamic_argument && arg_expr) {
      if (arg_expr->kind == NY_E_MEMCALL && arg_expr->as.memcall.target &&
          arg_expr->as.memcall.name &&
          strcmp(arg_expr->as.memcall.name, "get") == 0 &&
          arg_expr->as.memcall.args.len >= 1) {
        raw_get_target = arg_expr->as.memcall.target;
        raw_get_index = arg_expr->as.memcall.args.data[0].val;
      } else if (arg_expr->kind == NY_E_CALL && arg_expr->as.call.callee &&
                 arg_expr->as.call.callee->kind == NY_E_MEMBER &&
                 arg_expr->as.call.callee->as.member.target &&
                 arg_expr->as.call.callee->as.member.name &&
                 strcmp(arg_expr->as.call.callee->as.member.name, "get") == 0 &&
                 arg_expr->as.call.args.len >= 1) {
        raw_get_target = arg_expr->as.call.callee->as.member.target;
        raw_get_index = arg_expr->as.call.args.data[0].val;
      }
    }
    if (raw_get_target && raw_get_index &&
        ny_native_nir_expr_is_list(b, raw_get_target) &&
        !ny_native_nir_expr_is_dyn_list(b, raw_get_target)) {
      int raw_base = ny_native_nir_lower_expr(b, raw_get_target);
      int raw_index = ny_native_nir_lower_expr(b, raw_get_index);
      if (raw_base < 0 || raw_index < 0)
        return -1;
      /* The typed-list `.get` index is a raw native loop/index value here.
       * Use the raw-index entry point; the ABI wrapper decodes tagged
       * language integers and would turn raw odd indices (e.g. 1) into the
       * previous slot. */
      arg = ny_native_nir_emit_runtime_call(
          b, "rt_tbuf_index_any_raw", raw_base, raw_index, -1, 2, 0);
      if (arg < 0)
        return -1;
    }
    /* A direct call to a generated zero-capture lambda uses the ordinary
     * raw scalar ABI.  Its unannotated parameter has no declaration type,
     * but treating that absence as `any` tags the argument before raw lambda
     * arithmetic (`v * v` then multiplies VM encodings).  Callback adapters
     * retain responsibility for decoding dynamic values before this entry. */
    if (direct_zero_capture_lambda)
      box_dynamic_argument = false;
    /* Booleans are already immediate Ny values at an `any` boundary.  The
     * ordinary raw scalar lowering uses 0/1, which `type(any)` cannot
     * distinguish from an unknown native integer payload. */
    if (box_dynamic_argument && arg_expr &&
        arg_expr->kind == NY_E_LITERAL &&
        arg_expr->as.literal.kind == NY_LIT_BOOL)
      arg = ny_native_nir_emit_const(
          b, arg_expr->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE);
    /* BigInt runtime helpers take the opaque heap handle directly.  Their
     * legacy declarations are intentionally untyped, but applying the any
     * integer tag to a bigint pointer makes limb operations misclassify it as
     * a small integer (notably __bigint_clz on 2^63). */
    if (arg_expr && ny_native_nir_expr_is_bigint(b, arg_expr) &&
        ((runtime_symbol && strncmp(runtime_symbol, "rt_bigint_", 10) == 0) ||
         (name && strncmp(name, "__bigint_", 9) == 0)))
      box_dynamic_argument = false;
    /* Zero-capture lambdas use the same raw scalar callback ABI as named
     * functions.  The `rt_call_any*` adapters decode their dynamic
     * argument before invoking such a callback; marking it dynamic would
     * deliver the tagged argument to raw arithmetic a second time. */
    bool dynamic_fnptr_parameter =
        param_type && (strcmp(param_type, "fnptr") == 0 ||
                       strstr(param_type, "fnptr") != NULL);
    const expr_t *lambda_value_expr = NULL;
    if (arg_expr &&
        (arg_expr->kind == NY_E_LAMBDA || arg_expr->kind == NY_E_FN)) {
      lambda_value_expr = arg_expr;
    } else if (arg_expr && arg_expr->kind == NY_E_IDENT &&
               arg_expr->as.ident.name) {
      ny_native_nir_local_t *arg_local =
          ny_native_nir_find_local(b, arg_expr->as.ident.name);
      if (arg_local && arg_local->callable_expr)
        lambda_value_expr = arg_local->callable_expr;
    }
    /* A named function passed as a callback value has a raw code address but
     * may still need the tagged dynamic callback ABI.  The runtime's typed
     * adapter unboxes each argument to a scalar integer, which is only valid
     * when every parameter is a scalar numeric.  A non-scalar (or untyped)
     * parameter must be marked dynamic so the callback receives canonical
     * dynamic values.  The result side is independent: scalar results flow
     * through the raw adapter, while post-call boxing covers dynamic ones. */
    const stmt_t *named_callback_fn = NULL;
    if (dynamic_fnptr_parameter && !lambda_value_expr && arg_expr &&
        arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      const stmt_t *ref =
          ny_native_nir_find_user_function(b, arg_expr->as.ident.name);
      if (ref && ref->kind == NY_S_FUNC && !ref->as.fn.is_extern &&
          !ny_is_stdlib_tok(ref->tok)) {
        bool scalar_params = true;
        for (size_t pi = 0; pi < ref->as.fn.params.len; ++pi) {
          const param_t *cb_param = &ref->as.fn.params.data[pi];
          const char *pt = cb_param->type;
          /*
           * The compiled callee decides the ABI: an untyped parameter whose
           * semantics resolved to RAW_INT is a raw scalar register there,
           * exactly like a typed int.  Treating it as non-scalar here marked
           * the callable dynamic and rt_call_any1 forwarded the tagged word
           * (int 1 arrived as 3), so mapcat-style callbacks corrupted every
           * scalar leaf.
           */
          bool scalar = ny_native_type_name_is_int(pt) ||
                        ny_native_type_name_is_f64(pt) ||
                        ny_native_type_name_is_f32(pt) ||
                        (pt && strcmp(pt, "bool") == 0) ||
                        (!pt && cb_param->semantic.resolved &&
                         cb_param->semantic.rep == NY_SEM_REP_RAW_INT);
          if (!scalar) {
            scalar_params = false;
            break;
          }
        }
        if (!scalar_params)
          named_callback_fn = ref;
      }
    }
    if (dynamic_fnptr_parameter && named_callback_fn) {
      /* Same body-shape analysis as a lambda value: a boolean result keeps
       * the raw predicate ABI expected by filter-style loops. */
      bool returns_bool = false;
      stmt_t *fn_body = named_callback_fn->as.fn.body;
      expr_t *fn_result = NULL;
      if (fn_body && fn_body->kind == NY_S_EXPR) {
        fn_result = fn_body->as.expr.expr;
      } else if (fn_body && fn_body->kind == NY_S_BLOCK &&
                 fn_body->as.block.body.len > 0) {
        stmt_t *last =
            fn_body->as.block.body.data[fn_body->as.block.body.len - 1];
        if (last && last->kind == NY_S_EXPR)
          fn_result = last->as.expr.expr;
      }
      if (!named_callback_fn->as.fn.return_type ||
          strcmp(named_callback_fn->as.fn.return_type, "bool") == 0)
        returns_bool = fn_result && ny_native_nir_expr_is_bool(b, fn_result);
      arg = ny_native_nir_emit_runtime_call(
          b,
          returns_bool ? "rt_mark_dynamic_bool_callable"
                       : "rt_mark_dynamic_callable",
          arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
    }
    bool iter_callback_parameter =
        lambda_value_expr && callee_fn && callee_fn->kind == NY_S_FUNC &&
        callee_fn->as.fn.name && strstr(callee_fn->as.fn.name, "iter.");
    if ((dynamic_fnptr_parameter || iter_callback_parameter) &&
        !named_callback_fn && (lambda_value_expr || iter_callback_parameter)) {
      /* Inline/captured lambdas passed through fnptr use the dynamic callback
       * ABI. Mark the callable so rt_call_any1/2 forwards tagged
       * arguments instead of unboxing them as typed scalar parameters.
       * The result side keeps the raw 0/1 predicate ABI only when the
       * lambda body actually produces a boolean; the argument side must
       * follow the body's own parameter representation.  A body compiled
       * against raw-scalar parameters (semantics resolved to RAW_INT)
       * tolerates the dispatch unboxing, while a dynamic-parameter body
       * (rt_any_* helpers) consumes the canonical tagged word and marks
       * that fact with the tagged-args variant.  The former callee-name
       * heuristic unboxed integers for every filter/any/all callback and
       * corrupted predicates like `(v % 2) == 0` whose parameters stayed
       * dynamic ("filter list"). */
      bool predicate_callback = false;
      bool callback_params_raw = true;
      stmt_t *cb_body = lambda_value_expr ? lambda_value_expr->as.lambda.body
                                          : NULL;
      const ny_param_list *cb_params =
          lambda_value_expr ? &lambda_value_expr->as.lambda.params : NULL;
      for (size_t pi = 0; cb_params && pi < cb_params->len; ++pi) {
        const param_t *cb_param = &cb_params->data[pi];
        bool raw_scalar =
            cb_param->type
                ? ny_native_type_name_is_int(cb_param->type)
                : (cb_param->semantic.resolved &&
                   cb_param->semantic.rep == NY_SEM_REP_RAW_INT);
        if (!raw_scalar) {
          callback_params_raw = false;
          break;
        }
      }
      expr_t *cb_result = NULL;
      if (cb_body && cb_body->kind == NY_S_EXPR) {
        cb_result = cb_body->as.expr.expr;
      } else if (cb_body && cb_body->kind == NY_S_BLOCK &&
                 cb_body->as.block.body.len > 0) {
        stmt_t *last = cb_body->as.block.body.data[cb_body->as.block.body.len - 1];
        if (last && last->kind == NY_S_EXPR)
          cb_result = last->as.expr.expr;
      }
      predicate_callback = cb_result && ny_native_nir_expr_is_bool(b, cb_result);
      const char *mark_symbol =
          predicate_callback ? (callback_params_raw
                                    ? "rt_mark_dynamic_bool_callable"
                                    : "rt_mark_dynamic_bool_callable_tagged_args")
                             : "rt_mark_dynamic_callable";
      arg = ny_native_nir_emit_runtime_call(b, mark_symbol, arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
    }
    /* String builders are opaque native pointers.  Their first argument is
     * intentionally raw even when the public declaration is untyped: a
     * literal zero must remain the raw null pointer so the bridge can reject
     * it, rather than becoming Ny's tagged integer-zero immediate. */
    if (runtime_symbol && i == 0 &&
        (strcmp(runtime_symbol, "rt_str_builder_append") == 0 ||
         strcmp(runtime_symbol, "rt_str_builder_to_str") == 0 ||
         strcmp(runtime_symbol, "rt_str_builder_free") == 0))
      box_dynamic_argument = false;
    /* Builder payloads use the normal dynamic value ABI. String literals are
     * represented as raw C-string pointers while lowering, so materialize a
     * managed string before passing them to the runtime formatter. */
    if (runtime_symbol && i == 1 &&
        strcmp(runtime_symbol, "rt_str_builder_append") == 0 &&
        ny_native_nir_expr_is_cstr(b, arg_expr)) {
      arg = ny_native_nir_emit_runtime_call(b, "rt_cstr_to_str", arg, -1,
                                            -1, 1, 0);
      if (arg < 0)
        return -1;
      box_dynamic_argument = false;
    }
    bool raw_object_argument = ny_native_nir_expr_is_dict(b, arg_expr) ||
                               ny_native_nir_expr_is_list(b, arg_expr) ||
                               ny_native_nir_expr_is_range(b, arg_expr) ||
                               ny_native_nir_expr_is_bytes(b, arg_expr) ||
                               ny_native_nir_expr_is_bigint(b, arg_expr) ||
                               ny_native_nir_expr_is_cstr(b, arg_expr) ||
                               param_rep == NY_SEM_REP_OBJECT;
    /* Dynamic string parameters require a managed string handle. Raw literal
     * C-string pointers are valid native values, but match/assert helpers
     * consume the tagged string ABI and otherwise classify the pointer as an
     * unknown object. */
    if (box_dynamic_argument && ny_native_nir_expr_is_cstr(b, arg_expr) &&
        (ny_native_type_name_is_any(param_type) ||
         (!param_type && param_rep == NY_SEM_REP_TAGGED_DYNAMIC))) {
      arg = ny_native_nir_emit_runtime_call(b, "rt_cstr_to_str", arg, -1,
                                            -1, 1, 0);
      if (arg < 0)
        return -1;
      raw_object_argument = false;
    }
    bool raw_integer_argument = arg_expr && arg_expr->kind == NY_E_LITERAL &&
                                arg_expr->as.literal.kind == NY_LIT_INT &&
                                arg_expr->tok.kind != NY_T_NIL;
    if (arg_expr && arg_expr->semantic.resolved &&
        arg_expr->semantic.rep == NY_SEM_REP_RAW_INT)
      raw_integer_argument = true;
    /* Top-level scalar defs are raw NYIR storage too, but their identifier
     * expression often has no completed semantic fact after module
     * flattening.  Recover that fact from the defining integer expression so
     * an `any` boundary receives a real tagged value instead of a raw word
     * paired with an integer tag (which halves negative/odd values on read).
     */
    if (!raw_integer_argument && arg_expr &&
        arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      const ny_native_nir_local_t *arg_local =
          ny_native_nir_find_local(b, arg_expr->as.ident.name);
      if (arg_local && !arg_local->is_any &&
          ny_native_type_name_is_int(arg_local->type_name))
        raw_integer_argument = true;
      const expr_t *defined =
          ny_native_nir_find_top_level_value_in_source(
              b, arg_expr->as.ident.name, arg_expr->tok.filename);
      if (!defined)
        defined = ny_native_nir_find_top_level_value(b, arg_expr->as.ident.name);
      if (defined && defined->kind != NY_E_LITERAL /* bool handled above */ &&
          (defined->kind == NY_E_BINARY || defined->kind == NY_E_UNARY))
        raw_integer_argument = true;
      else if (defined && defined->kind == NY_E_LITERAL &&
               defined->as.literal.kind == NY_LIT_INT &&
               defined->tok.kind != NY_T_NIL)
        raw_integer_argument = true;
    }
    /* A value flowing into a concrete string/container/object parameter is a
     * pointer payload, even when its producer was semantically left dynamic.
     * Never apply tagged-integer boxing to these raw handles (notably a form
     * head passed as the `str name` parameter of macro expansion). */
    if (param_rep == NY_SEM_REP_STRING || param_rep == NY_SEM_REP_OBJECT ||
        param_rep == NY_SEM_REP_TYPED_BUFFER ||
        ny_native_type_name_is_str(param_type) ||
        ny_native_type_name_is_list(param_type) ||
        ny_native_type_name_is_bytes(param_type))
      raw_integer_argument = false;
    if (!raw_integer_argument && arg_expr &&
        (arg_expr->kind == NY_E_CALL || arg_expr->kind == NY_E_MEMCALL)) {
      const char *value_name =
          arg_expr->kind == NY_E_CALL
              ? (arg_expr->as.call.callee &&
                         arg_expr->as.call.callee->kind == NY_E_IDENT
                     ? arg_expr->as.call.callee->as.ident.name
                     : NULL)
              : arg_expr->as.memcall.name;
      const stmt_t *value_fn =
          value_name ? ny_native_nir_find_user_function(b, value_name) : NULL;
      /* A top-level function value (for example `def f = fn(int x) int {}`)
       * has no ordinary function declaration under the identifier `f`.  Its
       * generated lambda still carries the concrete return ABI, so resolve
       * that initializer before deciding whether an `any` argument needs
       * integer boxing. */
      if (!value_fn && value_name && arg_expr->kind == NY_E_CALL) {
        const expr_t *global =
            ny_native_nir_find_top_level_value(b, value_name);
        ny_native_lambda_entry_t *lambda =
            global ? ny_native_lambda_find(global) : NULL;
        if (lambda)
          value_fn = lambda->fn;
      }
      const char *value_type = value_fn ? value_fn->as.fn.return_type : NULL;
      /* A conservative `any.method(...)` semantic annotation must not hide
       * the concrete raw-integer return type of a resolved user method. */
    if (value_type && !ny_native_type_name_is_any(value_type) &&
          !ny_native_type_name_is_str(value_type) &&
          !ny_native_type_name_is_list(value_type) &&
          !ny_native_type_name_is_f64(value_type) &&
          !ny_native_type_name_is_f32(value_type))
        raw_integer_argument = true;
    }
    /* An explicitly declared `any` formal owns the dynamic ABI even when
     * body inference also proves scalar arithmetic. */
    if (ny_native_type_name_is_any(param_type))
      box_dynamic_argument = true;
    if (box_dynamic_argument && raw_integer_argument && !raw_object_argument) {
      int one = ny_native_nir_emit_const(b, 1);
      int shifted = one < 0
                        ? -1
                        : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                            .dst = -1,
                                                            .a = arg,
                                                            .b = one});
      arg = shifted < 0
                ? -1
                : ny_native_nir_emit_binop(b, NYIR_OR_I64, shifted, one);
      if (arg < 0)
        return -1;
    }
    /* A dynamically tagged caller may invoke an untyped function whose
     * semantic pass inferred a raw integer parameter from its arithmetic
     * body (the common `inner(x) { return x * 2 }` case).  Decode the direct
     * any local at that call boundary; otherwise small-call inlining copies
     * the tagged value into raw NYIR multiplication. */
    /* Only an untyped/integer parameter uses the raw scalar bridge here.
     * Nominal Nytrix values (poly_elem, zmod, user impl types, pointers, ...)
     * are also represented in an i64 NYIR slot, but that slot is already the
     * object handle.  Decoding it with any_to_i64 loses the handle and turns
     * a valid method receiver into zero (the ring `poly_elem.evaluate`
     * regression).  Keep the representation fact from the callee declaration
     * authoritative instead of inferring scalar-ness from the machine width.
     */
    bool raw_scalar_type =
        !param_type || ny_native_type_name_is_int(param_type) ||
        (param_type && strcmp(param_type, "bool") == 0) ||
        param_rep == NY_SEM_REP_RAW_INT;
    bool raw_scalar_parameter = callee_fn && !indirect_callable &&
                                !box_dynamic_argument &&
                                raw_scalar_type &&
                                !ny_native_type_name_is_list(param_type) &&
                                !ny_native_type_name_is_str(param_type) &&
                                !ny_native_type_name_is_f64(param_type) &&
                                !ny_native_type_name_is_f32(param_type);
    if (raw_scalar_parameter &&
        !(runtime_symbol && strcmp(runtime_symbol, "rt_proof_cert_check") == 0) &&
        arg_expr && !arg_expr_f64 && !arg_expr_f32 &&
        (ny_native_nir_expr_is_any(b, arg_expr) ||
         (arg_expr->kind == NY_E_IDENT &&
          ny_native_nir_find_local(b, arg_expr->as.ident.name) &&
          ny_native_nir_find_local(b, arg_expr->as.ident.name)->is_any))) {
      arg = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", arg, -1,
                                            -1, 1, 0);
    }
    if (box_dynamic_argument && arg_expr_f64) {
      /* Native f64 values live in an XMM/NYIR floating slot.  A dynamic
       * `any` parameter needs the normal heap-float handle, not bitcast bits
       * shifted through the integer tag encoding. */
      int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", arg,
                                                 -1, -1, 1, 0);
      arg = bits < 0 ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_flt_box_val",
                                                       bits, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
    }
    bool inferred_list =
        callee_fn && ny_native_nir_param_is_inferred_list(callee_fn, i);
    if ((param_rep == NY_SEM_REP_F32 ||
         ny_native_type_name_is_f32(param_type) ||
         (c_params && i < NY_C_MAX_PARAMS && c_params->param_f32[i])) &&
        !arg_expr_f32) {
      arg = arg_expr_f64 ? ny_native_nir_emit_f64_to_f32(b, arg)
                         : ny_native_nir_emit_i64_to_f32(b, arg);
    } else if ((param_rep == NY_SEM_REP_F64 ||
                ny_native_type_name_is_f64(param_type) ||
                (c_params && i < NY_C_MAX_PARAMS && c_params->param_f64[i])) &&
               !arg_expr_f64) {
      arg = arg_expr_f32 ? ny_native_nir_emit_f32_to_f64(b, arg)
                         : ny_native_nir_emit_i64_to_f64(b, arg);
    } else if ((param_rep == NY_SEM_REP_RAW_INT ||
                ny_native_type_name_is_int(param_type)) &&
               (arg_expr_f64 || arg_expr_f32)) {
      arg = arg_expr_f64 ? ny_native_nir_emit_f64_to_i64(b, arg)
                         : ny_native_nir_emit_f32_to_i64(b, arg);
    }
    if (arg < 0)
      return -1;
    if (lowered_argc >= NYIR_CALL_MAX_ARGS) {
      ny_native_nir_fail(b,
                         "native NYIR lower: expanded call exceeds the maximum "
                         "supported argument count (%d)",
                         NYIR_CALL_MAX_ARGS);
      return -1;
    }
    args[lowered_argc++] = arg;
    if (raw_native_dict_set || is_c_call)
      continue;
    bool callee_is_any =
        ny_native_type_name_is_any(param_type) ||
        (!param_type && (param_rep == NY_SEM_REP_TAGGED_DYNAMIC ||
                         param_rep == NY_SEM_REP_UNKNOWN));
    bool alias_raw_param =
        callee_fn && !param_type && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name && callee_fn->as.fn.name &&
        strcmp(e->as.call.callee->as.ident.name, callee_fn->as.fn.name) != 0 &&
        ny_native_nir_untyped_param_is_raw(callee_fn,
                                            callee_params->data[i].name);
    if (alias_raw_param)
      callee_is_any = false;
    bool callee_is_list =
        !callee_is_any &&
        (ny_native_type_name_is_list(param_type) || inferred_list ||
         (!param_type && param_rep == NY_SEM_REP_TYPED_BUFFER));
    if (callee_is_list) {
      int length = ny_native_nir_take_list_len_fact(b, arg);
      if (length < 0)
        length = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", arg,
                                                 -1, -1, 1, 0);
      if (length < 0) {
        ny_native_nir_fail(
            b,
            "native NYIR lower: list argument %zu to '%s' length query failed",
            i + 1, name);
        return -1;
      }
      if (lowered_argc >= NYIR_CALL_MAX_ARGS) {
        ny_native_nir_fail(b,
                           "native NYIR lower: expanded call exceeds the "
                           "maximum supported argument count (%d)",
                           NYIR_CALL_MAX_ARGS);
        return -1;
      }
      args[lowered_argc++] = length;
    } else if ((callee_is_any || param_rep == NY_SEM_REP_STRING ||
                ny_native_type_name_is_str(param_type)) &&
               !(name && (strcmp(name, "rt_proof_cert_digest") == 0 ||
                          strcmp(name, "rt_proof_cert_check") == 0 ||
                          strcmp(name, "__proof_cert_digest") == 0 ||
                          strcmp(name, "__proof_cert_check") == 0))) {
      int length =
          ny_native_nir_take_dyn_fact(b, arg, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
      int tag = ny_native_nir_take_dyn_fact(b, arg, NY_NATIVE_NIR_FACT_DYN_TAG);
      if (tag < 0 && arg_expr && arg_expr->kind == NY_E_IDENT &&
          arg_expr->as.ident.name) {
        ny_native_nir_local_t *source_local =
            ny_native_nir_find_local(b, arg_expr->as.ident.name);
        if (source_local && source_local->dyn_tag_slot >= 0)
          tag = ny_native_nir_load_local_value(b, source_local->dyn_tag_slot);
      }
      if (length < 0) {
        bool arg_local_canonical =
            arg_expr && arg_expr->kind == NY_E_IDENT &&
            arg_expr->as.ident.name &&
            ny_native_nir_find_local(b, arg_expr->as.ident.name) &&
            ny_native_nir_find_local(b, arg_expr->as.ident.name)
                ->raw_dynamic_index;
        if (ny_native_nir_expr_is_list(b, arg_expr) &&
            !(name && (strcmp(name, "set.sub") == 0 ||
                       strcmp(name, "std.core.set_mod.sub") == 0)))
        length = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", arg,
                                                   -1, -1, 1, 0);
        else if ((ny_native_nir_expr_is_any(b, arg_expr) || arg_local_canonical) &&
                 !(arg_expr && arg_expr->semantic.resolved &&
                   arg_expr->semantic.rep == NY_SEM_REP_RAW_INT && !arg_local_canonical))
          /*
           * A dynamic argument of unknown static length (an `any` local
           * holding a managed string) must carry its true length into the
           * callee: the seeded dyn_len slot feeds `str.len`, and a constant
           * zero here degenerated every `_match_at`-style loop into
           * "length 0" (split returned one empty piece per call).
           * Raw-integer arguments keep the constant zero: calling
           * rt_len on a packed scalar would misread it.  The any check
           * also covers module-level defs initialized from `any`-returning
           * calls: their use-site semantic refines to RAW_INT (globals
           * store raw payloads), which seeded decodeUTF8's `s.len` with 0
           * and made every scan bail before the first byte.
           * A local whose initializer was a seq/dyn-list index read holds
           * the canonical dynamic word even when the use-site semantic
           * claims RAW_INT; builder_append measured slen=0 for such chars
           * and _char_list_to_str returned the empty string.
           */
          length = ny_native_nir_emit_runtime_call(b, "rt_len", arg,
                                                   -1, -1, 1, 0);
        else if (!(arg_expr && arg_expr->kind == NY_E_LITERAL &&
                   arg_expr->as.literal.kind == NY_LIT_INT) &&
                 !(arg_expr && arg_expr->semantic.resolved &&
                   arg_expr->semantic.rep == NY_SEM_REP_RAW_INT))
          length = ny_native_nir_emit_runtime_call(b, "rt_len", arg,
                                                   -1, -1, 1, 0);
        else
          length = ny_native_nir_emit_const(b, 0);
      }
      if (tag < 0) {
        if (ny_expr_is_nil_literal(arg_expr)) {
          tag = ny_native_nir_emit_const(b, 0);
        } else if (ny_native_nir_expr_is_cstr(b, arg_expr)) {
          tag = ny_native_nir_emit_const(b, 121);
        } else if (arg_expr_f64 || arg_expr_f32) {
          tag = ny_native_nir_emit_const(b, TAG_FLOAT);
        } else if (arg_expr && arg_expr->kind == NY_E_LITERAL &&
                   arg_expr->as.literal.kind == NY_LIT_BOOL) {
          tag = ny_native_nir_emit_const(
              b, arg_expr->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE);
        } else if (arg_expr && arg_expr->kind == NY_E_LITERAL &&
                   arg_expr->as.literal.kind == NY_LIT_INT) {
          tag = ny_native_nir_emit_const(b, 3);
        } else if (ny_native_nir_expr_is_list(b, arg_expr)) {
          tag = ny_native_nir_emit_const(b, TAG_LIST);
        } else {
          tag = ny_native_nir_emit_runtime_call(b, "rt_value_tag", arg, -1,
                                                -1, 1, 0);
        }
      }
      if (lowered_argc + 1 >= NYIR_CALL_MAX_ARGS) {
        ny_native_nir_fail(b,
                           "native NYIR lower: expanded call exceeds the "
                           "maximum supported argument count (%d)",
                           NYIR_CALL_MAX_ARGS);
        return -1;
      }
      args[lowered_argc++] = length;
      args[lowered_argc++] = tag;
    }
  }
  if (callee_fn && ny_native_nir_fn_has_thread_attr(callee_fn)) {
    if (lowered_argc > 15) {
      ny_native_nir_fail(
          b, "native NYIR lower: @thread call supports up to 15 ABI arguments");
      return -1;
    }
    if (ny_native_type_name_is_f64(callee_fn->as.fn.return_type) ||
        ny_native_type_name_is_f32(callee_fn->as.fn.return_type)) {
      ny_native_nir_fail(b, "native NYIR lower: @thread functions must return "
                            "an i64-compatible value");
      return -1;
    }
    char callback_name[512];
    const char *fn_name = callee_fn->as.fn.name;
    int callback_len = snprintf(callback_name, sizeof(callback_name),
                                "ny_fn_%s", fn_name ? fn_name : name);
    if (callback_len < 0 || (size_t)callback_len >= sizeof(callback_name)) {
      ny_native_nir_fail(
          b, "native NYIR lower: @thread callback symbol is too long");
      return -1;
    }
    /* This is a code symbol, not a pooled C string.  Keep a stable copy for
     * the NYIR/object lifetime; strtab_intern would turn it into .Lnystr.N
     * and the trampoline would jump into string data. */
    char *callback_symbol = malloc((size_t)callback_len + 1);
    if (callback_symbol)
      memcpy(callback_symbol, callback_name, (size_t)callback_len + 1);
    int callback =
        callback_symbol
            ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .symbol = callback_symbol})
            : -1;
    /* The runtime call helper consumes a Ny integer for argc. */
    /* Mark this packed argc as a raw-code callback.  The low bits retain the
     * VM integer encoding; the sign bit is outside the legal 0..15 argc
     * range and lets the runtime bypass callable-tag heuristics. */
    int argc_value = ny_native_nir_emit_const(
        b, (int64_t)(INT64_MIN | (int64_t)((lowered_argc << 1) | 1)));
    int argv = ny_native_nir_emit_const(b, 0);
    if (lowered_argc > 0) {
      argv =
          nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ALLOCA,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .imm = (int64_t)lowered_argc * 8});
      if (argv >= 0) {
        for (size_t i = 0; i < lowered_argc; ++i) {
          int address = argv;
          if (i > 0) {
            int offset = ny_native_nir_emit_const(b, (int64_t)i * 8);
            address =
                offset < 0 ? -1 : ny_native_nir_emit_add_i64(b, argv, offset);
          }
          if (address < 0 ||
              !ny_native_nir_emit_store_i64(b, address, args[i])) {
            argv = -1;
            break;
          }
        }
      }
    }
    if (callback < 0 || argc_value < 0 || argv < 0)
      return -1;
    if (b->thread_detach_stmt_call) {
      if (ny_native_nir_emit_runtime_call(b, "rt_thread_launch_call", callback,
                                          argc_value, argv, 3, 0) < 0)
        return -1;
      return ny_native_nir_emit_const(b, 0);
    }
    int handle = ny_native_nir_emit_runtime_call(
        b, "rt_thread_spawn_call", callback, argc_value, argv, 3, 0);
    if (handle < 0)
      return -1;
    return ny_native_nir_emit_runtime_call(b, "rt_thread_join", handle, -1, -1,
                                           1, 0);
  }
  bool has_aggregate_return = ext && ext->ret_aggregate_size > 0;
  bool has_sret = has_aggregate_return &&
                  ext->ret_aggregate_classes[0] == NY_SYSV_AGG_MEMORY;
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_INTEGER &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_SSE &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F32 &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F64 &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HVA_V128) {
    ny_native_nir_fail(b, "native NYIR lower: aggregate return class is not "
                          "represented for the selected ABI");
    return -1;
  }
  if (ext) {
    for (unsigned i = 0; i < ext->param_count && i < e->as.call.args.len; ++i) {
      if (ext->arg_aggregate_sizes[i] > 0 &&
          NYIR_ARG_AGG_SIZE(ext->arg_aggregate_sizes[i]) <= 16 &&
          (NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
               NY_SYSV_AGG_UNSUPPORTED ||
           NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
               NY_SYSV_AGG_NONE)) {
        ny_native_nir_fail(b, "native NYIR lower: register aggregate argument "
                              "is not represented for the selected ABI");
        return -1;
      }
    }
  }
  int aggregate_ret_ptr = -1;
  if (has_aggregate_return) {
    /* Aggregate results escape the call and are owned by the caller.  Stack
     * alloca storage made `free(result)` corrupt the allocator; use the same
     * heap ownership contract as ordinary C aggregate wrappers. */
    int aggregate_size =
        ny_native_nir_emit_const(b, (int64_t)ext->ret_aggregate_size);
    aggregate_ret_ptr =
        aggregate_size < 0
            ? -1
            : ny_native_nir_emit_runtime_call(b, "rt_zalloc_raw",
                                              aggregate_size, -1, -1, 1, 0);
    if (aggregate_ret_ptr < 0) {
      ny_native_nir_fail(
          b, "native NYIR lower: aggregate return allocation failed");
      return -1;
    }
  }

  uint32_t *arg_sizes = NULL;
  if (ext && ext->param_count > 0) {
    bool has_byval = false;
    for (unsigned i = 0; i < ext->param_count; ++i) {
      if (ext->arg_aggregate_sizes[i] > 0)
        has_byval = true;
    }
    if (has_byval) {
      size_t total_args = e->as.call.args.len + (has_sret ? 1 : 0);
      arg_sizes = (uint32_t *)calloc(total_args, sizeof(*arg_sizes));
      if (!arg_sizes) {
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      for (unsigned i = 0; i < e->as.call.args.len && i < ext->param_count;
           ++i) {
        arg_sizes[i + (has_sret ? 1 : 0)] = ext->arg_aggregate_sizes[i];
      }
    }
  }

  size_t original_argc = lowered_argc;
  size_t argc = original_argc + (has_sret ? 1 : 0);
  if (has_sret) {
    if (argc > NYIR_CALL_MAX_ARGS) {
      free(arg_sizes);
      ny_native_nir_fail(
          b, "native NYIR lower: call exceeds maximum args with sret");
      return -1;
    }
    for (int i = (int)original_argc - 1; i >= 0; --i) {
      args[i + 1] = args[i];
    }
    args[0] = aggregate_ret_ptr;
  }

  const char *decl_symbol = callee_ext_decl ? callee_ext_decl->as.ext.link_name
                            : callee_fn     ? callee_fn->as.fn.link_name
                                            : NULL;
  const char *symbol =
      ext ? ext->c_symbol
          : (declared_fn_call && decl_symbol
                 ? decl_symbol
                 : (builtin_c_call
                        ? NULL
                        : (runtime_symbol ? runtime_symbol
                                          : (callee_fn && callee_fn->as.fn.name
                                                 ? callee_fn->as.fn.name
                                                 : name))));
  if (!ext && builtin_c_call) {
    switch (builtin_kind) {
    /* Native allocations receive raw i64 byte counts.  `rt_malloc` accepts
     * the interpreter's tagged count ABI, which halves every odd count and
     * corrupts the heap on writes past that truncated allocation. */
    case NY_BUILTIN_ALLOC_MALLOC:
      symbol = "rt_zalloc_raw";
      break;
    case NY_BUILTIN_ALLOC_CALLOC:
      symbol = "rt_zalloc_raw";
      break;
    case NY_BUILTIN_ALLOC_REALLOC:
      symbol = "rt_realloc_raw";
      break;
    case NY_BUILTIN_ALLOC_FREE:
      symbol = "rt_zfree_raw";
      break;
    case NY_BUILTIN_ALLOC_NONE:
      break;
    }
  }
  unsigned flags = (ext || declared_fn_call || builtin_c_call ||
                    runtime_c_call || ffi_c_call)
                       ? NYIR_INST_F_EXTERN
                       : 0;
  if (has_sret)
    flags |= NYIR_INST_F_SRET;
  const char *return_type = callee_fn ? callee_fn->as.fn.return_type
                            : callee_ext_decl
                                ? callee_ext_decl->as.ext.return_type
                                : NULL;
  if ((ext && ext->ret_f64) ||
      (symbol && strcmp(symbol, "rt_bigfloat_to_f64_raw") == 0) ||
      ny_native_type_name_is_f64(return_type)) {
    flags |= NYIR_INST_F_RET_F64;
  } else if ((ext && ext->ret_f32) || ny_native_type_name_is_f32(return_type)) {
    flags |= NYIR_INST_F_RET_F32;
  } else if (ny_native_nir_expr_is_f64(b, e)) {
    flags |= NYIR_INST_F_RET_F64;
  }
  if (has_aggregate_return && !has_sret) {
    if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ||
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)
      flags |= NYIR_INST_F_RET_F64;
    else if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32)
      flags |= NYIR_INST_F_RET_F32;
  }
  int *extra = NULL;
  if (argc > 6) {
    size_t extra_len = argc - 6;
    extra = (int *)malloc(extra_len * sizeof(*extra));
    if (!extra) {
      free(arg_sizes);
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
    memcpy(extra, &args[6], extra_len * sizeof(*extra));
  }
  int v = nyir_emit(&b->nyir,
                    (nyir_inst_t){.op = NYIR_CALL,
                                  .dst = -1,
                                  .a = argc > 0 ? args[0] : -1,
                                  .b = argc > 1 ? args[1] : -1,
                                  .c = argc > 2 ? args[2] : -1,
                                  .d = argc > 3 ? args[3] : -1,
                                  .e = argc > 4 ? args[4] : -1,
                                  .f = argc > 5 ? args[5] : -1,
                                  .imm = (int64_t)argc,
                                  .flags = flags,
                                  .symbol = symbol,
                                  .extra_args = extra,
                                  .extra_args_len = argc > 6 ? argc - 6 : 0,
                                  .arg_sizes = arg_sizes});
  if (v < 0) {
    free(extra);
    free(arg_sizes);
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return -1;
  }
  /* File/module functions without an explicit return annotation are exposed
   * through the dynamic import ABI. Their native scalar return is raw, while
   * the importing expression is consumed as `any`; box that boundary once so
   * `helper_val() == 123` does not interpret raw 123 as tagged 61. */
  /* A dynamic arithmetic expression already returns the canonical tagged
   * value from its `any` runtime helper (for example `a + b` lowers through
   * rt_any_add).  Only box an untyped module result when its body
   * visibly returns a raw scalar leaf; tagging the arithmetic result again
   * turns 7 into 15/31 and breaks imported helper calls. */
  bool module_raw_scalar_leaf = false;
  if (callee_fn && !return_type && callee_fn->as.fn.body &&
      callee_fn->as.fn.body->kind == NY_S_BLOCK &&
      callee_fn->as.fn.body->as.block.body.len > 0) {
    const stmt_t *tail =
        callee_fn->as.fn.body->as.block.body
            .data[callee_fn->as.fn.body->as.block.body.len - 1];
    const expr_t *returned = NULL;
    if (tail && tail->kind == NY_S_EXPR)
      returned = tail->as.expr.expr;
    else if (tail && tail->kind == NY_S_RETURN)
      returned = tail->as.ret.value;
    /* A literal leaf is a raw scalar that still needs boxing.  An ident leaf
     * is only raw when it provably binds a raw integer on the callee side:
     * an untyped parameter already carries the tagged dynamic
     * representation (tagging it again shifts the value, 21 becomes 43),
     * while globals keep the raw representation.  Caller-scope facts cannot
     * answer this; the name belongs to the callee. */
    bool ident_raw = false;
    if (returned && returned->kind == NY_E_IDENT && returned->as.ident.name) {
      const char *rname = returned->as.ident.name;
      bool resolved = false;
      for (size_t pi = 0; pi < callee_fn->as.fn.params.len; ++pi) {
        if (callee_fn->as.fn.params.data[pi].name &&
            strcmp(callee_fn->as.fn.params.data[pi].name, rname) == 0) {
          resolved = true;
          ident_raw =
              ny_native_type_name_is_int(callee_fn->as.fn.params.data[pi].type);
          break;
        }
      }
      if (!resolved && callee_fn->as.fn.body &&
          callee_fn->as.fn.body->kind == NY_S_BLOCK) {
        const expr_t *last_init = NULL;
        for (size_t si = 0; si < callee_fn->as.fn.body->as.block.body.len;
             ++si) {
          const stmt_t *bstmt = callee_fn->as.fn.body->as.block.body.data[si];
          if (!bstmt || bstmt->kind != NY_S_VAR)
            continue;
          for (size_t ni = 0;
               ni < bstmt->as.var.names.len && ni < bstmt->as.var.exprs.len;
               ++ni) {
            if (bstmt->as.var.names.data[ni] &&
                strcmp(bstmt->as.var.names.data[ni], rname) == 0) {
              resolved = true;
              last_init = bstmt->as.var.exprs.data[ni];
            }
          }
        }
        if (resolved)
          ident_raw = last_init &&
                      ((last_init->kind == NY_E_LITERAL &&
                        last_init->as.literal.kind == NY_LIT_INT &&
                        last_init->tok.kind != NY_T_NIL) ||
                       last_init->kind == NY_E_MATCH);
      }
      if (!resolved)
        ident_raw = true;
    }
    bool literal_raw =
        returned && returned->kind == NY_E_LITERAL &&
        returned->as.literal.kind == NY_LIT_INT && returned->tok.kind != NY_T_NIL;
    /* The semantic return fact is authoritative when inference proved a raw
     * scalar.  Such a module function is a normal integer call even though it
     * has no source annotation; tagging it merely because the caller's AST is
     * dynamic changes `helper() == 123` into a comparison against Ny's encoded
     * integer.  Fall back to body-shape detection only when inference did not
     * resolve the representation. */
    bool semantic_raw =
        (callee_fn->as.fn.return_semantic.resolved &&
         callee_fn->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT) ||
        literal_raw;
    bool body_raw_scalar = false;
    if (returned && returned->kind == NY_E_BINARY) {
      for (size_t pi = 0; pi < callee_fn->as.fn.params.len; ++pi) {
        const char *pname = callee_fn->as.fn.params.data[pi].name;
        if (pname && ny_native_nir_param_has_scalar_op_expr(returned, pname)) {
          body_raw_scalar = true;
          break;
        }
      }
    }
    module_raw_scalar_leaf =
        !semantic_raw &&
        (literal_raw || body_raw_scalar ||
         (returned && returned->kind == NY_E_IDENT && ident_raw));
  }
  bool callee_name_alias =
      callee_fn && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name && callee_fn->as.fn.name &&
      strcmp(e->as.call.callee->as.ident.name, callee_fn->as.fn.name) != 0;
  (void)callee_name_alias;
  /*
   * Return-ABI invariant: an untyped callee guarantees a tagged dynamic
   * return (its return boundary boxes raw scalar producers), so the legacy
   * alias box was removed — it double-tagged.  Consumers classify such calls
   * via expr_is_any (callee return fact) and decode themselves; a consumer
   * that is declared raw (`def int n = f(...)`) decodes at its store
   * boundary in the variable lowering instead.
   */
  /* Preserve the dynamic tag for the common identity/pass-through shape.
   * Any-return functions expose only their value in the native return
   * register, so without this fact an inlined `identity(0)` loses the
   * distinction between tagged integer zero (tag 3) and nil (tag 0). */
  if (callee_fn && callee_fn->as.fn.return_type &&
      strcmp(callee_fn->as.fn.return_type, "any") == 0 &&
      callee_fn->as.fn.body && callee_fn->as.fn.body->kind == NY_S_BLOCK &&
      callee_fn->as.fn.body->as.block.body.len > 0) {
    const stmt_t *tail =
        callee_fn->as.fn.body->as.block.body
            .data[callee_fn->as.fn.body->as.block.body.len - 1];
    const expr_t *returned = NULL;
    if (tail && tail->kind == NY_S_EXPR)
      returned = tail->as.expr.expr;
    else if (tail && tail->kind == NY_S_RETURN)
      returned = tail->as.ret.value;
    if (returned && returned->kind == NY_E_IDENT && returned->as.ident.name) {
      for (size_t i = 0; i < callee_fn->as.fn.params.len && i < user_args_len;
           ++i) {
        const char *param_name = callee_fn->as.fn.params.data[i].name;
        if (!param_name || strcmp(param_name, returned->as.ident.name) != 0)
          continue;
        const expr_t *source = user_args[i].val;
        int64_t static_tag = -1;
        if (ny_expr_is_nil_literal(source))
          static_tag = 0;
        else if (source && source->kind == NY_E_LITERAL &&
                 source->as.literal.kind == NY_LIT_INT)
          static_tag = 3;
        else if (ny_native_nir_expr_is_cstr(b, source))
          static_tag = 121;
        else if (source && (ny_native_nir_expr_is_f64(b, source) ||
                            ny_native_nir_expr_is_f32(b, source)))
          static_tag = TAG_FLOAT;
        if (static_tag >= 0) {
          int tag = ny_native_nir_emit_const(b, static_tag);
          if (tag < 0 || !ny_native_nir_record_dyn_fact(
                             b, v, NY_NATIVE_NIR_FACT_DYN_TAG, tag))
            return -1;
        }
        break;
      }
    }
  }
  if (builtin_c_call &&
      !ny_native_nir_record_alloc_fact(
          b, v, ny_native_nir_literal_allocation_size(leaf, e))) {
    return -1;
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HVA_V128) {
    unsigned elem_count = ext->ret_aggregate_size / 16u;
    if (elem_count < 1 || elem_count > 4 ||
        elem_count * 16u != ext->ret_aggregate_size) {
      ny_native_nir_fail(
          b, "native NYIR lower: invalid AAPCS64 HVA return layout");
      return -1;
    }
    for (unsigned i = 0; i < elem_count; ++i) {
      int captured = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .c = -1,
                                                       .imm = 10 + (int64_t)i});
      int off = i ? ny_native_nir_emit_const(b, (int64_t)i * 16) : -1;
      int addr = i ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                   : aggregate_ret_ptr;
      if (captured < 0 || addr < 0 ||
          nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_VEC4_STORE_I64,
                                            .dst = -1,
                                            .a = addr,
                                            .b = captured,
                                            .c = -1}) < 0)
        return -1;
    }
    return aggregate_ret_ptr;
  }
  if (has_aggregate_return && !has_sret &&
      (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32 ||
       ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)) {
    bool f32_hfa = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32;
    unsigned elem_size = f32_hfa ? 4u : 8u;
    unsigned elem_count = ext->ret_aggregate_size / elem_size;
    if (elem_count < 1 || elem_count > 4 ||
        elem_count * elem_size != ext->ret_aggregate_size) {
      ny_native_nir_fail(
          b, "native NYIR lower: invalid AAPCS64 HFA return layout");
      return -1;
    }
    static const int f64_selectors[4] = {2, 3, 8, 9};
    int captured[4] = {-1, -1, -1, -1};
    for (unsigned i = 0; i < elem_count; ++i) {
      captured[i] =
          nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .imm = f32_hfa ? 4 + (int64_t)i
                                                           : f64_selectors[i]});
      if (captured[i] < 0)
        return -1;
    }
    if (!f32_hfa) {
      for (unsigned i = 0; i < elem_count; ++i) {
        int off = i ? ny_native_nir_emit_const(b, (int64_t)i * 8) : -1;
        int addr = i ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                     : aggregate_ret_ptr;
        if (addr < 0 || !ny_native_nir_emit_store_i64(b, addr, captured[i]))
          return -1;
      }
    } else {
      int shift32 = ny_native_nir_emit_const(b, 32);
      if (shift32 < 0)
        return -1;
      for (unsigned pair = 0; pair * 2 < elem_count; ++pair) {
        unsigned lo_idx = pair * 2;
        int packed = captured[lo_idx];
        if (lo_idx + 1 < elem_count) {
          int shifted =
              nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64,
                                                .dst = -1,
                                                .a = captured[lo_idx + 1],
                                                .b = shift32});
          packed = shifted < 0 ? -1
                               : nyir_emit(&b->nyir,
                                           (nyir_inst_t){.op = NYIR_OR_I64,
                                                         .dst = -1,
                                                         .a = captured[lo_idx],
                                                         .b = shifted});
        }
        int off = pair ? ny_native_nir_emit_const(b, (int64_t)pair * 8) : -1;
        int addr = pair ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off)
                        : aggregate_ret_ptr;
        if (packed < 0 || addr < 0 ||
            !ny_native_nir_emit_store_i64(b, addr, packed))
          return -1;
      }
    }
    return aggregate_ret_ptr;
  }
  int primary_ret = v;
  int second_ret = -1;
  bool packed_integer_pair =
      has_aggregate_return && !has_sret && ext->ret_aggregate_size <= 8 &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER &&
      ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER;
  bool capture_second_integer_first =
      has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE &&
      ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER;
  if (capture_second_integer_first) {
    second_ret = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .c = -1,
                                                   .imm = 1});
    if (second_ret < 0) {
      ny_native_nir_fail(
          b, "native NYIR lower: secondary return register capture failed");
      return -1;
    }
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE) {
    primary_ret = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                                    .dst = -1,
                                                    .a = -1,
                                                    .b = -1,
                                                    .c = -1,
                                                    .imm = 2});
    if (primary_ret < 0) {
      ny_native_nir_fail(
          b, "native NYIR lower: primary return register capture failed");
      return -1;
    }
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[1] != NY_SYSV_AGG_NONE &&
      !capture_second_integer_first && !packed_integer_pair) {
    int selector = -1;
    if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER)
      /* A two-integer SysV result uses RAX then RDX.  A scalar/SSE first
       * result still leaves the integer member in the secondary register. */
      selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER ? 1 : 1;
    else if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_SSE)
      selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ? 3 : 2;
    if (selector < 0) {
      ny_native_nir_fail(b, "native NYIR lower: secondary aggregate return "
                            "class is not represented for the selected ABI");
      return -1;
    }
    second_ret = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CAPTURE_RET,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .c = -1,
                                                   .imm = selector});
    if (second_ret < 0) {
      ny_native_nir_fail(b,
                         "native NYIR lower: return register capture failed");
      return -1;
    }
  }
  if (has_aggregate_return && !has_sret) {
    if ((second_ret >= 0 || packed_integer_pair) &&
        ext->ret_aggregate_size <= 8 &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER &&
        ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER) {
      int mask = ny_native_nir_emit_const(b, UINT64_C(0xffffffff));
      int shift = ny_native_nir_emit_const(b, 32);
      int lo = mask < 0 ? -1 : nyir_emit(&b->nyir, (nyir_inst_t){
          .op = NYIR_AND_I64, .dst = -1, .a = primary_ret, .b = mask});
      int high_raw = packed_integer_pair
                         ? nyir_emit(&b->nyir, (nyir_inst_t){
                               .op = NYIR_SAR_I64, .dst = -1,
                               .a = primary_ret, .b = shift})
                         : second_ret;
      int hi = (high_raw < 0 || mask < 0) ? -1 : nyir_emit(&b->nyir, (nyir_inst_t){
          .op = NYIR_AND_I64, .dst = -1, .a = high_raw, .b = mask});
      int shifted = (hi < 0 || shift < 0) ? -1 : nyir_emit(&b->nyir, (nyir_inst_t){
          .op = NYIR_SHL_I64, .dst = -1, .a = hi, .b = shift});
      int packed = (lo < 0 || shifted < 0) ? -1 : nyir_emit(&b->nyir, (nyir_inst_t){
          .op = NYIR_OR_I64, .dst = -1, .a = lo, .b = shifted});
      if (packed < 0 || !ny_native_nir_emit_store_i64(b, aggregate_ret_ptr, packed))
        return -1;
    } else if (!ny_native_nir_emit_store_i64(b, aggregate_ret_ptr, primary_ret))
      return -1;
    if (second_ret >= 0 && !(ext->ret_aggregate_size <= 8 &&
                             ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER &&
                             ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER)) {
      int off = ny_native_nir_emit_const(b, 8);
      int addr =
          off >= 0 ? ny_native_nir_emit_add_i64(b, aggregate_ret_ptr, off) : -1;
      if (addr < 0 || !ny_native_nir_emit_store_i64(b, addr, second_ret))
        return -1;
    }
  }
  if (return_type && strcmp(return_type, "bigint") == 0) {
    int bigint_tag = ny_native_nir_emit_const(b, TAG_BIGINT);
    if (bigint_tag < 0 || !ny_native_nir_record_dyn_fact(
                              b, v, NY_NATIVE_NIR_FACT_DYN_TAG, bigint_tag))
      return -1;
  }
  /*
   * A list-returning call's length is only known at runtime, and the fact
   * table maps a register to a compile-time constant.  Recording the length
   * register's id as the payload made a later `.len` on a reused local slot
   * return that register id as the length itself (mapcat's `m` became 2 or
   * 4 for one-element lists), so the fact must simply not exist here.
   */
  if (leaf && strcmp(leaf, "_ensure_windows") == 0) {
    int meta_off = ny_native_nir_emit_const(b, -16);
    int meta = meta_off < 0 ? -1 : ny_native_nir_emit_add_i64(b, v, meta_off);
    int length = meta < 0 ? -1 : ny_native_nir_emit_load_i64(b, meta);
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, v, length))
      return -1;
  }
  return has_aggregate_return ? aggregate_ret_ptr : v;
}
static bool ny_native_nir_qualified_expr(const expr_t *e, char *out,
                                         size_t out_len) {
  if (!e || !out || out_len == 0)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int n = snprintf(out, out_len, "%s", e->as.ident.name);
    return n >= 0 && (size_t)n < out_len;
  }
  if (e->kind == NY_E_MEMBER && e->as.member.target && e->as.member.name) {
    char prefix[512];
    if (!ny_native_nir_qualified_expr(e->as.member.target, prefix,
                                      sizeof(prefix)))
      return false;
    int n = snprintf(out, out_len, "%s.%s", prefix, e->as.member.name);
    return n >= 0 && (size_t)n < out_len;
  }
  return false;
}
