static const char *ny_native_fn_module(ny_native_fn_index_t *index,
                                       const program_t *prog, const stmt_t *fn);
static const char *ny_native_emitted_fn_name(ny_native_fn_index_t *index,
                                             const program_t *prog,
                                             const ny_options *opt,
                                             const stmt_t *fn);
static const char *nyr_global_symbol(const nyr_builder_t *b, const char *name);
static bool nyr_qualified_expr(const expr_t *e, char *out, size_t out_len);

typedef struct {
  const char *name;
  int64_t value;
} ny_native_proof_binding_t;

static inline bool nyr_size_fits_i64(size_t n) {
  return n <= (size_t)INT64_MAX;
}

static inline bool nyr_size_fits_tagged_i64(size_t n) {
  return n <= ((size_t)INT64_MAX >> 1);
}

/* NY_SEM_REP_RAW_INT covers the 128-bit spellings too, but no scalar call or
 * callback ABI in this lowering moves a 16-byte integer, so i128/u128 stay
 * non-raw even when a resolved semantic fact says RAW_INT. */
static bool ny_native_type_name_is_128bit_int(const char *name) {
  const char *leaf = ny_generic_type_leaf(name);
  return leaf && (strcmp(leaf, "i128") == 0 || strcmp(leaf, "u128") == 0);
}

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
static const char *nyr_early_leaf(const expr_t *e) {
  if (e && e->semantic.member_call_kind != NY_SEM_CALL_NONE &&
      e->semantic.canonical_callee) {
    const char *cleaf = ny_name_leaf(e->semantic.canonical_callee);
    if (cleaf && *cleaf)
      return cleaf;
  }
  return ny_native_call_leaf(e);
}

/* Canonical `__mark_dynamic_callable*` -> runtime marker mapping. The
 * suffix flags decide the callable-word encoding; every consumer reads the
 * record instead of re-matching spellings. */
static const char *nyr_dynamic_marker_symbol(const char *leaf,
                                             bool *tagged_args,
                                             bool *raw_result) {
  if (tagged_args)
    *tagged_args = false;
  if (raw_result)
    *raw_result = false;
  if (!leaf)
    return NULL;
  const char *symbol = NULL;
  bool tagged = false;
  bool raw = false;
  if (strcmp(leaf, "__mark_dynamic_callable_tagged_args_raw_result") == 0) {
    symbol = "rt_mark_dynamic_callable_tagged_word_args_raw_result";
    tagged = true;
    raw = true;
  } else if (strcmp(leaf, "__mark_dynamic_callable_tagged_args") == 0) {
    symbol = "rt_mark_dynamic_callable_tagged_word_args";
    tagged = true;
  } else if (strcmp(leaf, "__mark_dynamic_bool_callable_tagged_args") == 0) {
    symbol = "rt_mark_dynamic_bool_callable_tagged_word_args";
    tagged = true;
  } else if (strcmp(leaf, "__mark_dynamic_callable_raw_result") == 0) {
    symbol = "rt_mark_dynamic_callable_word_args_raw_result";
    raw = true;
  } else if (strcmp(leaf, "__mark_dynamic_callable") == 0) {
    symbol = "rt_mark_dynamic_callable_word_args";
  } else if (strcmp(leaf, "__mark_dynamic_bool_callable") == 0) {
    symbol = "rt_mark_dynamic_bool_callable_word_args";
  }
  if (!symbol)
    return NULL;
  if (tagged_args)
    *tagged_args = tagged;
  if (raw_result)
    *raw_result = raw;
  return symbol;
}

/* These language-level conversions deliberately produce the machine integer
 * representation.  Their declarations remain dynamically typed for source
 * compatibility, so the ordinary return-type metadata cannot be used to
 * decide whether a caller must decode the result. */
static bool nyr_expr_is_raw_integer_conversion(nyr_builder_t *b,
                                               const expr_t *e) {
  if (!b || !e || e->kind != NY_E_CALL || !e->as.call.callee)
    return false;
  const char *leaf = nyr_early_leaf(e);
  if (leaf && e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name &&
      (strcmp(leaf, "bshr") == 0 || strcmp(leaf, "__shr") == 0))
    return true;
  if (e->as.call.args.len != 1 || e->as.call.args.data[0].name)
    return false;
  if (leaf && strcmp(leaf, "int") == 0 && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      e->semantic.canonical_callee_stmt->as.fn.return_type &&
      ny_native_type_name_is_int(
          e->semantic.canonical_callee_stmt->as.fn.return_type))
    return true;
  if (!leaf || nyr_user_defined_fn(b, leaf))
    return false;
  return strcmp(leaf, "int") == 0 || strcmp(leaf, "to_int") == 0 ||
         strcmp(leaf, "__untag") == 0 || strcmp(leaf, "__any_to_i64") == 0 ||
         strcmp(leaf, "any_to_i64") == 0 ||
         strcmp(leaf, "__bigint_to_int") == 0;
}

static bool nyr_leaf_in(const char *leaf, const char *const *set, size_t n) {
  if (!leaf)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (strcmp(leaf, set[i]) == 0)
      return true;
  return false;
}

/* A nested primitive bitwise call produces a raw NYIR word even though its
 * source declaration is `any`.  Follow that producer ABI through nested
 * calls before deciding whether the outer call needs the tagged rt_or path. */
static bool nyr_raw_bitwise_value(nyr_builder_t *b, const expr_t *e,
                                  unsigned depth) {
  if (!e || depth > 16)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_nearest(b, e->as.ident.name);
    if (local && !local->is_any && local->semantic_rep == NY_SEM_REP_RAW_INT)
      return true;
    const stmt_t *decl =
        e->tok.filename ? nyr_find_top_level_var_in_source_in_prog(
                              b, b->prog, e->as.ident.name, e->tok.filename)
                        : NULL;
    if (!decl)
      decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (decl && decl->kind == NY_S_VAR) {
      for (size_t i = 0; i < decl->as.var.names.len; ++i) {
        if (decl->as.var.names.data[i] &&
            strcmp(decl->as.var.names.data[i], e->as.ident.name) == 0 &&
            i < decl->as.var.exprs.len && decl->as.var.exprs.data[i])
          return nyr_raw_bitwise_value(b, decl->as.var.exprs.data[i],
                                       depth + 1);
      }
    }
    const expr_t *global = e->tok.filename
                               ? nyr_find_top_level_value_in_source(
                                     b, e->as.ident.name, e->tok.filename)
                               : NULL;
    if (!global)
      global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e && nyr_raw_bitwise_value(b, global, depth + 1);
  }
  if (e->kind == NY_E_BINARY && nyr_is_bitwise_operator(e->as.binary.op))
    return nyr_raw_bitwise_value(b, e->as.binary.left, depth + 1) &&
           nyr_raw_bitwise_value(b, e->as.binary.right, depth + 1);
  if (e->kind != NY_E_CALL || e->as.call.args.len != 2 ||
      e->as.call.args.data[0].name || e->as.call.args.data[1].name)
    return false;
  const char *leaf = nyr_early_leaf(e);
  if (!nyr_leaf_in(leaf,
                   (const char *const[]){"__or", "bor", "__and", "band",
                                         "__xor", "bxor", "__shl", "bshl",
                                         "__shr", "bshr"},
                   10))
    return false;
  return nyr_raw_bitwise_value(b, e->as.call.args.data[0].val, depth + 1) &&
         nyr_raw_bitwise_value(b, e->as.call.args.data[1].val, depth + 1);
}

/*
 * Unlike nyr_raw_bitwise_value(), this requires the expression itself (or
 * an immutable global alias) to originate at a compatibility bitwise
 * operation. Integer literals are valid operands, but are not bitwise
 * results on their own.
 */
static bool nyr_raw_bitwise_expression(nyr_builder_t *b, const expr_t *e,
                                       unsigned depth) {
  if (!e || depth > 16)
    return false;
  if (e->kind == NY_E_BINARY)
    return nyr_is_bitwise_operator(e->as.binary.op) &&
           nyr_raw_bitwise_value(b, e, depth + 1);
  if (e->kind == NY_E_CALL) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name)
      return false;
    const char *leaf = nyr_early_leaf(e);
    if (!nyr_leaf_in(leaf,
                     (const char *const[]){"__or", "bor", "__and", "band",
                                           "__xor", "bxor", "__shl", "bshl",
                                           "__shr", "bshr"},
                     10))
      return false;
    return nyr_raw_bitwise_value(b, e, depth + 1);
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const expr_t *global = e->tok.filename
                               ? nyr_find_top_level_value_in_source(
                                     b, e->as.ident.name, e->tok.filename)
                               : NULL;
    if (!global)
      global = nyr_find_top_level_value(b, e->as.ident.name);
    return global && global != e &&
           nyr_raw_bitwise_expression(b, global, depth + 1);
  }
  return false;
}

/* Return the ABI fact established by semantic typing for an omitted scalar
 * parameter.  This is deliberately declaration-based: call lowering and
 * function lowering must consult the same resolved fact instead of guessing
 * from the call spelling or from the expression at one call site. */
static bool nyr_untyped_param_is_raw(const stmt_t *fn, const char *param) {
  if (!fn || fn->kind != NY_S_FUNC || !param)
    return false;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const param_t *p = &fn->as.fn.params.data[i];
    if (!p->name || strcmp(p->name, param) != 0 || p->type ||
        !p->semantic.resolved)
      continue;
    return p->semantic.rep == NY_SEM_REP_RAW_INT;
  }
  return false;
}

/*
 * Whether a canonical proof-type string genuinely references an identifier:
 * substring matching is unsound here (a parameter named `n` textually occurs
 * in "binary"), and a same-named unrelated top-level binding must not
 * suppress the rejection (it is shadowed at the call site, so it cannot
 * discharge an obligation over the caller's parameter).
 */
static bool ny_proof_type_names_ident(const char *proof_type,
                                      const char *name) {
  if (!proof_type || !name || !*name)
    return false;
  size_t nlen = strlen(name);
  for (const char *p = proof_type; (p = strstr(p, name)) != NULL; p++) {
    bool left_ok = (p == proof_type);
    if (!left_ok) {
      char c = p[-1];
      left_ok = !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_');
    }
    char next = p[nlen];
    bool right_ok = !next || (!((next >= 'a' && next <= 'z') ||
                                (next >= 'A' && next <= 'Z') ||
                                (next >= '0' && next <= '9')) &&
                              next != '_');
    if (left_ok && right_ok)
      return true;
  }
  return false;
}

/* Resolve the representation of a source expression at a dynamic boundary.
 * Top-level constant identifiers are lowered from raw storage, but their
 * identifier node can retain an unresolved/dynamic semantic fact after module
 * flattening. Keep this query tied to declaration and local ABI facts; do not
 * infer representation from the value's low bits. */
static bool ny_proof_condition_touches_runtime(const nyr_builder_t *b,
                                               const expr_t *e, unsigned depth,
                                               bool *out) {
  if (!e || !out || depth > 64)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, e->as.ident.name);
    if (!local)
      local = nyr_find_local_nearest((nyr_builder_t *)b, e->as.ident.name);
    const stmt_t *decl = nyr_find_top_level_var_stmt(b, e->as.ident.name);
    if (local) {
      *out = true;
      return false;
    }
    if (decl && decl->kind == NY_S_VAR && decl->as.var.is_mut) {
      *out = true;
      return false;
    }
    return false;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    return ny_proof_condition_touches_runtime(b, e->as.unary.right, depth + 1,
                                              out);
  case NY_E_BINARY:
    return ny_proof_condition_touches_runtime(b, e->as.binary.left, depth + 1,
                                              out) ||
           ny_proof_condition_touches_runtime(b, e->as.binary.right, depth + 1,
                                              out);
  case NY_E_LOGICAL:
    return ny_proof_condition_touches_runtime(b, e->as.logical.left, depth + 1,
                                              out) ||
           ny_proof_condition_touches_runtime(b, e->as.logical.right, depth + 1,
                                              out);
  case NY_E_TERNARY:
    return ny_proof_condition_touches_runtime(b, e->as.ternary.cond, depth + 1,
                                              out) ||
           ny_proof_condition_touches_runtime(b, e->as.ternary.true_expr,
                                              depth + 1, out) ||
           ny_proof_condition_touches_runtime(b, e->as.ternary.false_expr,
                                              depth + 1, out);
  case NY_E_INDEX:
    return ny_proof_condition_touches_runtime(b, e->as.index.target, depth + 1,
                                              out) ||
           ny_proof_condition_touches_runtime(b, e->as.index.start, depth + 1,
                                              out);
  case NY_E_CALL:
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (ny_proof_condition_touches_runtime(b, e->as.call.args.data[i].val,
                                             depth + 1, out))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (ny_proof_condition_touches_runtime(b, e->as.memcall.target, depth + 1,
                                           out))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (ny_proof_condition_touches_runtime(b, e->as.memcall.args.data[i].val,
                                             depth + 1, out))
        return true;
    return false;
  case NY_E_MEMBER:
    return ny_proof_condition_touches_runtime(b, e->as.member.target, depth + 1,
                                              out);
  default:
    return false;
  }
}

static bool nyr_expr_has_raw_int_abi(const nyr_builder_t *b, const expr_t *e,
                                     unsigned depth) {
  if (!b || !e || depth > 32)
    return false;
  /* An element read of a whole-program proven elem-8 int list returns the
   * raw i64 slot payload; this representation contract outranks the stale
   * semantic annotation attached while the list was an untyped `[]`. */
  if (e->kind == NY_E_INDEX && e->as.index.target &&
      e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name &&
      nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name))
    return true;
  /* A bigint expression is a handle even when its syntax is an arithmetic
   * binary expression.  Do this check before the raw-int semantic fallback:
   * otherwise a handle produced by `bigint(1) << 62` is boxed a second time
   * as if its address were an integer at an `any` boundary. */
  if (nyr_expr_is_bigint((nyr_builder_t *)b, e))
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return true;
  if (e->kind == NY_E_UNARY || e->kind == NY_E_BINARY) {
    int64_t folded = 0;
    if (nyr_fold_top_level_int(b, NULL, e, &folded, depth + 1))
      return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "^") == 0)
    return false;
  /* A value received through an explicit `any` parameter is already in the
   * canonical dynamic ABI.  Flow inference may refine one use of that value
   * to RAW_INT, but that refinement does not rewrite the function boundary;
   * boxing it again would turn dynamic strings/handles into integer payloads.
   * Check the storage contract before expression-level semantic facts. */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    nyr_local_t *local = nyr_find_local_ctx(
        (nyr_builder_t *)b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!local)
      local = nyr_find_local_nearest((nyr_builder_t *)b, e->as.ident.name);
    if (local && local->is_any)
      return false;
  }
  /* Runtime call results follow the registered ABI result representation,
   * even when imported semantic facts still call a raw scalar result
   * `tagged_dynamic`.  This keeps raw bridges such as bigint-to-i64 from
   * reaching expanded any calls without being boxed. */
  if (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL) {
    const char *leaf =
        e->kind == NY_E_CALL ? ny_native_call_leaf(e) : e->as.memcall.name;

    /* This internal intrinsic has a native raw-i64 lowering even though its
     * public runtime entry returns a VM-tagged integer.  The emitter below
     * uses rt_f64_to_i64/rt_any_to_i64 directly, so classify the expression
     * by that emitted ABI rather than the public runtime declaration. */
    if (leaf && strcmp(leaf, "__flt_to_int") == 0 && e->kind == NY_E_CALL &&
        e->as.call.args.len == 1 && !e->as.call.args.data[0].name)
      return true;
    /* Precedes the record lookup below: `bshr` resolves to no runtime
     * symbol and `__shr` resolves to the tagged rt_shr entry, so the ABI
     * record cannot classify these compat intrinsics even though the bshr
     * arm emits the raw rt_bshr_raw row. */
    if (leaf && (strcmp(leaf, "bshr") == 0 || strcmp(leaf, "__shr") == 0) &&
        e->kind == NY_E_CALL && e->as.call.args.len == 2)
      return true;
    const char *symbol =
        leaf ? ny_native_runtime_symbol_for_expr(leaf, leaf, e) : NULL;
    const ny_native_runtime_abi_decl_t *decl =
        symbol ? ny_native_runtime_abi_decl(symbol) : NULL;
    /* RAW_WORD and RAW_INT both describe an untagged i64 result; the bigint
     * bridge (rt_bigint_to_i64_raw) uses the RAW_INT spelling and its result
     * must box at dynamic boundaries or even raw words decode as tagged
     * smallints and halve. */
    if (ny_native_runtime_abi_result_is_raw_word(decl) ||
        ny_native_runtime_abi_result_is_raw_int(decl))
      return true;
    /* A global initialized from a source function call is stored in the
     * producer's scalar ABI.  The global identifier may have no semantic
     * representation after module flattening, so recover the declaration
     * fact from the canonical callee instead of treating the call result as
     * an opaque dynamic word. */
    const stmt_t *callee = e->semantic.canonical_callee_stmt;
    if (!callee && e->semantic.canonical_callee)
      callee = nyr_find_user_function((nyr_builder_t *)b,
                                      e->semantic.canonical_callee);
    if (!callee && e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name)
      callee = nyr_find_user_function((nyr_builder_t *)b,
                                      e->as.call.callee->as.ident.name);
    if (callee && callee->kind == NY_S_FUNC) {
      /* An explicit scalar return annotation owns the call ABI.  HM may
       * retain a broader TAGGED_DYNAMIC fact from a compatibility intrinsic
       * in the function body (for example bshr), but the function boundary
       * is still a raw `int` result.  A resolved RAW_INT fact decides when
       * it is coherent with the annotation, which classifies spellings the
       * name check misses (char/handle/Fin); i128/u128 stay excluded. */
      if (callee->as.fn.return_semantic.resolved &&
          callee->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT &&
          ny_sem_rep_coherent_with_type(callee->as.fn.return_type,
                                        callee->as.fn.return_semantic.rep) &&
          !ny_native_type_name_is_128bit_int(callee->as.fn.return_type))
        return true;
      if (callee->as.fn.return_type)
        return ny_native_type_name_is_int(callee->as.fn.return_type);
      if (callee->as.fn.return_semantic.resolved)
        return callee->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT;
    }
    /* Imported calls can retain only the canonical return type rather than a
     * local function declaration node.  The shared type query follows that
     * canonical callee and is authoritative for this scalar ABI boundary. */
    if (ny_native_type_name_is_int(nyr_expr_type_name(b, e)))
      return true;
  }
  if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_RAW_INT)
    return true;
  if (e->semantic.resolved &&
      (e->semantic.rep == NY_SEM_REP_STRING ||
       e->semantic.rep == NY_SEM_REP_F32 || e->semantic.rep == NY_SEM_REP_F64 ||
       e->semantic.rep == NY_SEM_REP_OBJECT ||
       e->semantic.rep == NY_SEM_REP_TYPED_BUFFER ||
       e->semantic.rep == NY_SEM_REP_POINTER ||
       e->semantic.rep == NY_SEM_REP_CLOSURE ||
       e->semantic.rep == NY_SEM_REP_AGGREGATE))
    return false;
  if (e->kind == NY_E_UNARY)
    return nyr_expr_has_raw_int_abi(b, e->as.unary.right, depth + 1);
  /* A value-producing ternary keeps the ABI of its two value arms.  Without
   * this case, raw integer arms are stored into a merge local as i64-raw but
   * an `any` call boundary sees the expression as unknown and skips the
   * required box_i64 (for example the 0/1 option passed to FFI call3). */
  if (e->kind == NY_E_TERNARY)
    return nyr_expr_has_raw_int_abi(b, e->as.ternary.true_expr, depth + 1) &&
           nyr_expr_has_raw_int_abi(b, e->as.ternary.false_expr, depth + 1);
  if (e->kind == NY_E_BINARY) {
    const char *op = e->as.binary.op;
    bool numeric_op =
        op && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") ||
               !strcmp(op, "/") || !strcmp(op, "%") || !strcmp(op, "&") ||
               !strcmp(op, "|") || !strcmp(op, "^") || !strcmp(op, "<<") ||
               !strcmp(op, ">>"));
    if (!numeric_op)
      return false;
    return nyr_expr_has_raw_int_abi(b, e->as.binary.left, depth + 1) &&
           nyr_expr_has_raw_int_abi(b, e->as.binary.right, depth + 1);
  }
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  nyr_local_t *local = nyr_find_local_ctx((nyr_builder_t *)b, e->as.ident.name,
                                          e->as.ident.syntax_ctx);
  if (!local)
    local = nyr_find_local_nearest((nyr_builder_t *)b, e->as.ident.name);
  if (local && !local->is_any &&
      (local->semantic_rep == NY_SEM_REP_RAW_INT ||
       ny_native_type_name_is_int(local->type_name)))
    return true;
  /* A visible local shadows the same-named top-level def, exactly as in
   * fold_top_level_int's guard above: an untyped parameter or a
   * call-result local with no representation fact is a dynamic word, not
   * the global's raw initializer. Falling through to the name-keyed table
   * here made `mut x = 5` preceding `fn sq(x)` lower `x * x` as raw mul
   * over the boxed argument, so `sq(7) == 49` compared false. */
  if (local)
    return false;
  const expr_t *def =
      nyr_find_top_level_value_in_source(b, e->as.ident.name, e->tok.filename);
  if (!def)
    def = nyr_find_top_level_value(b, e->as.ident.name);
  if (!def || def == e)
    return false;
  if (def->semantic.resolved && def->semantic.rep == NY_SEM_REP_RAW_INT)
    return true;
  int64_t folded = 0;
  return nyr_fold_top_level_int(b, NULL, def, &folded, depth + 1) ||
         nyr_expr_has_raw_int_abi(b, def, depth + 1);
}

/* A dynamic sequence index is integral even when contextual numeric
 * inference widens its arithmetic expression to f32/f64, as in
 * `0.0 + values.get(0 * 4 + row, 0.0)`. Inspect source operations without
 * trusting that widened result annotation; explicit float literals,
 * division, and dynamic locals remain non-integer keys. */
static bool nyr_expr_is_integer_index(const nyr_builder_t *b, const expr_t *e,
                                      unsigned depth) {
  if (!b || !e || depth > 32)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_local_ctx(
        (nyr_builder_t *)b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!local)
      local = nyr_find_local_nearest((nyr_builder_t *)b, e->as.ident.name);
    if (local)
      return !local->is_any && (local->semantic_rep == NY_SEM_REP_RAW_INT ||
                                ny_native_type_name_is_int(local->type_name));
    return e->semantic.resolved && e->semantic.rep == NY_SEM_REP_RAW_INT;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "-") == 0)
    return nyr_expr_is_integer_index(b, e->as.unary.right, depth + 1);
  if (e->kind != NY_E_BINARY || !e->as.binary.op)
    return false;
  const char *op = e->as.binary.op;
  bool integral_op = !strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*") ||
                     !strcmp(op, "%") || !strcmp(op, "&") || !strcmp(op, "|") ||
                     !strcmp(op, "^") || !strcmp(op, "<<") || !strcmp(op, ">>");
  return integral_op &&
         nyr_expr_is_integer_index(b, e->as.binary.left, depth + 1) &&
         nyr_expr_is_integer_index(b, e->as.binary.right, depth + 1);
}

static int nyr_box_raw_dynamic_value(nyr_builder_t *b, int value) {
  return value < 0 ? -1
                   : nyr_emit_runtime_call(b, "rt_box_i64", value, -1, -1, 1,
                                           NYIR_SCALAR_I64_TAGGED);
}

/* Dictionary get bridges return the fallback word unchanged.  When the
 * source fallback is a raw integer expression, tag it before crossing that
 * dynamic result boundary so later consumers do not halve it as an encoded
 * Ny integer. */
static int nyr_box_dict_fallback(nyr_builder_t *b, int fallback,
                                 const expr_t *fallback_expr) {
  if (!b || fallback < 0 || !fallback_expr || nyr_expr_is_any(b, fallback_expr))
    return fallback;
  bool int_literal = fallback_expr->kind == NY_E_LITERAL &&
                     fallback_expr->as.literal.kind == NY_LIT_INT &&
                     fallback_expr->tok.kind != NY_T_NIL;
  if (int_literal || nyr_expr_has_raw_int_abi(b, fallback_expr, 0))
    return nyr_box_raw_dynamic_value(b, fallback);
  return fallback;
}

static bool nyr_mapped_proof_holds(const char *proof_type, const char *param,
                                   int64_t value) {
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

/* Inline the overwhelmingly common append-to-capacity path for scalar lists.
 * A native scalar list is a typed-buffer data pointer with the canonical
 * 32-byte header immediately before the data: count @ -24, elem_size @ -16,
 * capacity @ -8.  The slow runtime helper is still authoritative for the
 * initial descriptor-to-i64 compaction and every reallocation; once capacity
 * exists, appending an i64 needs only three header loads, one element store,
 * and one count store. */
static int nyr_emit_scalar_list_append_fast(nyr_builder_t *b, int list,
                                            int value, int known_count) {
  if (!b || list < 0 || value < 0)
    return -1;
  int neg24 = nyr_emit_const(b, -24);
  int neg8 = nyr_emit_const(b, -8);
  int one = nyr_emit_const(b, 1);
  if (neg24 < 0 || neg8 < 0 || one < 0)
    return -1;

  /* Keep the two header-address ADDs parallel (same base, independent) so
   * superscalar CPUs can issue them together. Deriving one from the other
   * serializes the chain and measured slower. */
  int count_addr = nyr_emit_add_i64(b, list, neg24);
  int cap_addr = nyr_emit_add_i64(b, list, neg8);
  if (count_addr < 0 || cap_addr < 0)
    return -1;
  /* The caller only selects this helper for a proven scalar list.  Reuse the
   * lowering-time length fact when available instead of re-reading count from
   * the header on every append; the header store below remains authoritative
   * for runtime consumers. */
  int count = known_count >= 0 ? known_count : nyr_emit_load_i64(b, count_addr);
  int capacity = nyr_emit_load_i64(b, cap_addr);
  if (count < 0 || capacity < 0)
    return -1;

  int can_fast = nyr_emit_cmp_i64(b, NYIR_CMP_LT, count, capacity);
  int result_slot = nyr_temp_slot(b);
  if (can_fast < 0 || result_slot < 0)
    return -1;

  int fast_label = b->next_label++;
  int slow_label = b->next_label++;
  int end_label = b->next_label++;
  if (!nyr_emit_br_if(b, can_fast, fast_label) || !nyr_emit_br(b, slow_label) ||
      !nyr_emit_label(b, fast_label))
    return -1;

  /* Strength-reduced byte offset: count*8 == count<<3. Emitting SHL
   * directly lets both LLVM and native encoders use shift/LEA instead of
   * a full imul on every append. */
  int shl_three = nyr_emit_const(b, 3);
  int byte_off = shl_three < 0
                     ? -1
                     : nyr_push_val(b, NYIR_SHL_I64, count, shl_three, 0, NULL);
  int dst = byte_off < 0 ? -1 : nyr_emit_add_i64(b, list, byte_off);
  int next_count = nyr_emit_add_i64(b, count, one);
  if (dst < 0 || next_count < 0 || !nyr_emit_store_i64(b, dst, value) ||
      !nyr_emit_store_i64(b, count_addr, next_count) ||
      !nyr_store_local_value(b, result_slot, list) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, slow_label))
    return -1;

  int slow =
      nyr_emit_runtime_call(b, "rt_tbuf_append_i64_raw", list, value, -1, 2, 0);
  if (slow < 0 || !nyr_store_local_value(b, result_slot, slow) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, end_label))
    return -1;
  return nyr_load_local_value(b, result_slot);
}

/* Untyped lists that are currently compact integer buffers can use the same
 * spare-capacity store as typed scalar lists.  The header guard is required:
 * an empty `[]` starts as a descriptor buffer and the first append must go
 * through the runtime helper, which converts it to elem-8 storage.  The slow
 * arm also remains responsible for growth and any descriptor representation.
 */
static int nyr_emit_dynamic_i64_append_fast(nyr_builder_t *b, int list,
                                            int value, int known_count) {
  if (!b || list < 0 || value < 0)
    return -1;
  int neg24 = nyr_emit_const(b, -24);
  int neg16 = nyr_emit_const(b, -16);
  int neg8 = nyr_emit_const(b, -8);
  int eight = nyr_emit_const(b, 8);
  int zero = nyr_emit_const(b, 0);
  int one = nyr_emit_const(b, 1);
  if (neg24 < 0 || neg16 < 0 || neg8 < 0 || eight < 0 || zero < 0 || one < 0)
    return -1;

  /* Keep header-address ADDs parallel (same base). Chained derivation
   * measured slower on superscalar CPUs. */
  int count_addr = nyr_emit_add_i64(b, list, neg24);
  int elem_size_addr = nyr_emit_add_i64(b, list, neg16);
  int capacity_addr = nyr_emit_add_i64(b, list, neg8);
  int count = known_count >= 0
                  ? known_count
                  : (count_addr < 0 ? -1 : nyr_emit_load_i64(b, count_addr));
  int elem_size =
      elem_size_addr < 0 ? -1 : nyr_emit_load_i64(b, elem_size_addr);
  int capacity = capacity_addr < 0 ? -1 : nyr_emit_load_i64(b, capacity_addr);
  int result_slot = nyr_temp_slot(b);
  if (count < 0 || elem_size < 0 || capacity < 0 || result_slot < 0)
    return -1;

  int count_label = b->next_label++;
  int capacity_label = b->next_label++;
  int fast_label = b->next_label++;
  int slow_label = b->next_label++;
  int end_label = b->next_label++;
  /* Keep each comparison adjacent to its branch. Native encoders can branch
   * on flags instead of materializing and spilling three boolean values and
   * two eager ANDs on every append. All representation guards still apply.
   * When the lowering-time length fact proves count>=0, skip the redundant
   * non-negative guard entirely (saves 1 CMP+BR per append in hot loops). */
  int compact = nyr_emit_cmp_i64(b, NYIR_CMP_EQ, elem_size, eight);
  if (compact < 0)
    return -1;
  if (known_count >= 0) {
    if (!nyr_emit_br_if(b, compact, capacity_label) ||
        !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, capacity_label))
      return -1;
  } else {
    if (!nyr_emit_br_if(b, compact, count_label) ||
        !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, count_label))
      return -1;
    int nonnegative = nyr_emit_cmp_i64(b, NYIR_CMP_GE, count, zero);
    if (nonnegative < 0 || !nyr_emit_br_if(b, nonnegative, capacity_label) ||
        !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, capacity_label))
      return -1;
  }
  int has_capacity = nyr_emit_cmp_i64(b, NYIR_CMP_LT, count, capacity);
  if (has_capacity < 0 || !nyr_emit_br_if(b, has_capacity, fast_label) ||
      !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, fast_label))
    return -1;

  int shl_three_dyn = nyr_emit_const(b, 3);
  int byte_off = shl_three_dyn < 0 ? -1
                                   : nyr_push_val(b, NYIR_SHL_I64, count,
                                                  shl_three_dyn, 0, NULL);
  int dst = byte_off < 0 ? -1 : nyr_emit_add_i64(b, list, byte_off);
  int next_count = nyr_emit_add_i64(b, count, one);
  if (dst < 0 || next_count < 0 || !nyr_emit_store_i64(b, dst, value) ||
      !nyr_emit_store_i64(b, count_addr, next_count) ||
      !nyr_store_local_value(b, result_slot, list) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, slow_label))
    return -1;

  int slow =
      nyr_emit_runtime_call(b, "rt_tbuf_append_i64_raw", list, value, -1, 2, 0);
  if (slow < 0 || !nyr_store_local_value(b, result_slot, slow) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, end_label))
    return -1;
  return nyr_load_local_value(b, result_slot);
}

/*
 * Inline elem-8 read for receivers whose whole-program proof already pins
 * the element layout (declared `list<int>` globals, promoted append-built
 * int lists).  The runtime accessor re-validates handle provenance, header
 * magic, and stride on every element; here the proof replaces that per-read
 * gauntlet with a header count load, the shared negative-index wrap, one
 * bounds check, and the element load.  The header layout (count at -24,
 * elements at data + i*8) is identical for compact and descriptor buffers,
 * and an empty promoted buffer fails the bounds check exactly like the
 * runtime accessor's count test.
 */
static int nyr_emit_proven_elem8_read(nyr_builder_t *b, int base, int idx,
                                      bool canonical_payload) {
  if (!b || base < 0 || idx < 0)
    return -1;
  int neg24 = nyr_emit_const(b, -24);
  int eight = nyr_emit_const(b, 8);
  int zero = nyr_emit_const(b, 0);
  if (neg24 < 0 || eight < 0 || zero < 0)
    return -1;
  int check_label = b->next_label++;
  int fast_label = b->next_label++;
  int slow_label = b->next_label++;
  int end_label = b->next_label++;
  int direct = canonical_payload
                   ? base
                   : nyr_emit_runtime_call(b, "rt_tbuf_direct_elem8_ptr_raw",
                                           base, -1, -1, 1, 0);
  int nonnull =
      direct < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_NE, direct, zero);
  if (nonnull < 0 || !nyr_emit_br_if(b, nonnull, check_label) ||
      !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, check_label))
    return -1;
  int count_addr = nyr_emit_add_i64(b, direct, neg24);
  int count = count_addr < 0 ? -1 : nyr_emit_load_i64(b, count_addr);
  if (count < 0)
    return -1;
  int nonnegative = nyr_emit_cmp_i64(b, NYIR_CMP_GE, idx, zero);
  int below_count = nyr_emit_cmp_i64(b, NYIR_CMP_LT, idx, count);
  int in_bounds =
      nonnegative < 0 || below_count < 0
          ? -1
          : nyr_emit_binop(b, NYIR_AND_I64, nonnegative, below_count);
  if (in_bounds < 0 || !nyr_emit_br_if(b, in_bounds, fast_label) ||
      !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, fast_label))
    return -1;
  int off8 = nyr_emit_binop(b, NYIR_MUL_I64, idx, eight);
  int addr = off8 < 0 ? -1 : nyr_emit_add_i64(b, direct, off8);
  if (addr < 0)
    return -1;
  int fast_result = nyr_emit_load_i64(b, addr);
  if (fast_result < 0 || !nyr_emit_br(b, end_label) ||
      !nyr_emit_label(b, slow_label))
    return -1;
  int slow_result = nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base,
                                          idx, -1, 2, NYIR_SCALAR_I64_RAW);
  if (slow_result < 0 || !nyr_emit_br(b, end_label) ||
      !nyr_emit_label(b, end_label))
    return -1;
  nyr_phi_incoming_t *incoming = malloc(2 * sizeof(*incoming));
  if (!incoming)
    return -1;
  incoming[0] = (nyr_phi_incoming_t){.predecessor_label = fast_label,
                                      .value = fast_result};
  incoming[1] = (nyr_phi_incoming_t){.predecessor_label = slow_label,
                                      .value = slow_result};
  int result =
      nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_PHI,
                                        .dst = -1,
                                        .a = -1,
                                        .b = -1,
                                        .c = -1,
                                        .d = -1,
                                        .e = -1,
                                        .f = -1,
                                        .phi_incoming = incoming,
                                        .phi_incoming_len = 2,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
  if (result < 0) {
    free(incoming);
    return -1;
  }
  return result;
}

/*
 * Store an integer directly into a whole-program-proven append-built elem-8
 * buffer when the normalized index is already inside its live length. Keep
 * the checked runtime setter as the slow arm: it owns null, capacity-growth,
 * and out-of-range behavior. The proof at the call site must establish that
 * this global is an unescaped integer tbuf, and the value must be raw i64.
 */
static int nyr_emit_proven_elem8_write(nyr_builder_t *b, int base, int index,
                                       int value) {
  if (!b || base < 0 || index < 0 || value < 0)
    return -1;
  int zero = nyr_emit_const(b, 0);
  int neg24 = nyr_emit_const(b, -24);
  int eight = nyr_emit_const(b, 8);
  if (zero < 0 || neg24 < 0 || eight < 0)
    return -1;
  int check_label = b->next_label++;
  int fast_label = b->next_label++;
  int slow_label = b->next_label++;
  int end_label = b->next_label++;
  int nonnull = nyr_emit_cmp_i64(b, NYIR_CMP_NE, base, zero);
  if (nonnull < 0 || !nyr_emit_br_if(b, nonnull, check_label) ||
      !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, check_label))
    return -1;
  int count_addr = nyr_emit_add_i64(b, base, neg24);
  int count = count_addr < 0 ? -1 : nyr_emit_load_i64(b, count_addr);
  int is_negative =
      count < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_LT, index, zero);
  int wrapped =
      is_negative < 0 || count < 0 ? -1 : nyr_emit_add_i64(b, index, count);
  int adjusted = wrapped < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                                         .dst = -1,
                                                         .a = is_negative,
                                                         .b = wrapped,
                                                         .c = index});
  int nonnegative =
      adjusted < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_GE, adjusted, zero);
  int below_count = adjusted < 0 || count < 0
                        ? -1
                        : nyr_emit_cmp_i64(b, NYIR_CMP_LT, adjusted, count);
  int in_bounds =
      nonnegative < 0 || below_count < 0
          ? -1
          : nyr_emit_binop(b, NYIR_AND_I64, nonnegative, below_count);
  if (in_bounds < 0 || !nyr_emit_br_if(b, in_bounds, fast_label) ||
      !nyr_emit_br(b, slow_label) || !nyr_emit_label(b, fast_label))
    return -1;
  int offset = nyr_emit_binop(b, NYIR_MUL_I64, adjusted, eight);
  int address = offset < 0 ? -1 : nyr_emit_add_i64(b, base, offset);
  if (address < 0 || !nyr_emit_store_i64(b, address, value) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, slow_label))
    return -1;
  int slow_result =
      nyr_emit_runtime_call(b, "rt_tbuf_set_i64_raw", base, index, value, 3, 0);
  if (slow_result < 0 || !nyr_emit_br(b, end_label) ||
      !nyr_emit_label(b, end_label))
    return -1;
  nyr_phi_incoming_t *incoming = malloc(2 * sizeof(*incoming));
  if (!incoming)
    return -1;
  incoming[0] =
      (nyr_phi_incoming_t){.predecessor_label = fast_label, .value = base};
  incoming[1] = (nyr_phi_incoming_t){.predecessor_label = slow_label,
                                      .value = slow_result};
  int result = nyr_ir_emit(
      &b->nyir, (nyr_inst_t){.op = NYIR_PHI,
                              .dst = -1,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .phi_incoming = incoming,
                              .phi_incoming_len = 2,
                              .scalar_class = nyr_value_scalar_class(b, base)});
  if (result < 0) {
    free(incoming);
    return -1;
  }
  return result;
}

/* Call arguments have their own ABI.  A numeric f64 expression can contain
 * an indexed buffer access, but the access index is still an integer even
 * while the caller is lowering its f64 result.  Keep that context from
 * leaking into index arithmetic (`row * width + col`). */
static int nyr_lower_integer_expr(nyr_builder_t *b, const expr_t *e) {
  if (!b)
    return -1;
  unsigned saved_f64_depth = b->f64_context_depth;
  b->f64_context_depth = 0;
  int value = nyr_lower_expr(b, e);
  b->f64_context_depth = saved_f64_depth;
  return value;
}

static int nyr_lower_f64_arg(nyr_builder_t *b, const expr_t *e) {
  if (!b || !e)
    return -1;
  /* A conditional has no single representation until both arms have been
   * normalized.  Decoding the merged machine word is incorrect when an arm
   * is a tagged dynamic read: the join would reinterpret either a raw f64
   * bit-pattern or a tagged value according to whichever branch happened to
   * be selected. */
  if (e->kind == NY_E_TERNARY && e->as.ternary.cond &&
      e->as.ternary.true_expr && e->as.ternary.false_expr) {
    if (e->as.ternary.cond->kind == NY_E_LITERAL) {
      bool choose_true = false;
      if (e->as.ternary.cond->as.literal.kind == NY_LIT_BOOL)
        choose_true = e->as.ternary.cond->as.literal.as.b;
      else if (e->as.ternary.cond->as.literal.kind == NY_LIT_INT)
        choose_true = e->as.ternary.cond->as.literal.as.i != 0;
      else
        goto lower_f64_as_value;
      return nyr_lower_f64_arg(b, choose_true ? e->as.ternary.true_expr
                                              : e->as.ternary.false_expr);
    }
    int result_slot = nyr_temp_slot(b);
    int true_label = b->next_label++;
    int false_label = b->next_label++;
    int end_label = b->next_label++;
    int cond = nyr_lower_expr(b, e->as.ternary.cond);
    if (cond < 0 || !nyr_emit_br_if(b, cond, true_label) ||
        !nyr_emit_br(b, false_label) || !nyr_emit_label(b, true_label))
      return -1;
    int value = nyr_lower_f64_arg(b, e->as.ternary.true_expr);
    if (value < 0 || !nyr_store_local_value(b, result_slot, value) ||
        !nyr_emit_br(b, end_label) || !nyr_emit_label(b, false_label))
      return -1;
    value = nyr_lower_f64_arg(b, e->as.ternary.false_expr);
    if (value < 0 || !nyr_store_local_value(b, result_slot, value) ||
        !nyr_emit_label(b, end_label))
      return -1;
    return nyr_load_local_value(b, result_slot);
  }
lower_f64_as_value:
  {
    bool is_f64 = nyr_expr_is_f64(b, e);
    bool is_f32 = nyr_expr_is_f32(b, e);
    bool is_any = nyr_expr_is_any(b, e);
    /*
     * Operator dispatch is the representation authority for overloaded
     * arithmetic.  When the selected operator declares an f64 return (for
     * example vec3 * vec3), the lowered call already yields machine f64;
     * converting it as i64 would reinterpret the IEEE bits as an integer.
     * Mark it f64 here so the value is used directly.
     */
    if (!is_f64 && !is_f32 && e->kind == NY_E_BINARY && e->as.binary.op) {
      const stmt_t *op_stmt = nyr_find_operator(b, e);
      if (op_stmt && op_stmt->as.oper.return_type &&
          ny_native_type_name_is_f64(op_stmt->as.oper.return_type)) {
        is_f64 = true;
        is_any = false;
      }
    }
    /* Lowering a dynamic `get` while an f64 consumer is active already asks
     * the accessor to decode its canonical boxed result.  Mark that contextual
     * result as f64 here so the generic post-lowering conversion does not
     * decode the resulting machine value a second time. */
    if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len >= 2 &&
        e->as.memcall.target &&
        (nyr_expr_is_any(b, e->as.memcall.target) ||
         nyr_expr_is_dyn_list(b, e->as.memcall.target)) &&
        nyr_expr_is_f64_depth(b, e->as.memcall.args.data[1].val, 0)) {
      is_f64 = true;
      is_any = false;
    }
    if (is_f64 && !is_any && e->kind == NY_E_IDENT && e->as.ident.name) {
      const nyr_local_t *local = nyr_find_local_nearest(b, e->as.ident.name);
      if (local && local->is_f64) {
        int addr = nyr_emit_addr_local(b, local->slot, e->as.ident.name);
        return addr < 0 ? -1 : nyr_emit_load_f64(b, addr);
      }
    }
    unsigned saved_f64_depth = b->f64_context_depth;
    b->f64_context_depth++;
    int value = (is_f64 || is_f32 || is_any) ? nyr_lower_expr(b, e)
                                             : nyr_lower_integer_expr(b, e);
    b->f64_context_depth = saved_f64_depth;
    if (value < 0)
      return -1;
    /* Contextual float inference can describe a boxed arithmetic result as
     * f64. The produced carrier decides whether it needs dynamic decoding. */
    nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
    if (value_class == NYIR_SCALAR_F64)
      return value;
    if (value_class == NYIR_SCALAR_F32)
      return nyr_emit_f32_to_f64(b, value);
    if (value_class == NYIR_SCALAR_I64_TAGGED)
      return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    if (is_f64)
      return value;
    if (is_f32)
      return nyr_emit_f32_to_f64(b, value);
    if (is_any)
      return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    /*
     * The AST-level type can say int while the register is f64: context
     * propagation computes integer sub-expressions in the f64 domain inside
     * float-returning functions (`1.0 / float((ij*(ij+1)) >> 1 ...)`).  The
     * register class is authoritative — reinterpreting f64 bits as an i64
     * here turned the divisor into garbage (spectral's ZeroDivisionError).
     */
    if (nyr_value_is_f64(b, value, 0))
      return value;
    return nyr_emit_i64_to_f64(b, value);
  }
}

/* Lower an integer conversion with its destination representation in mind.
 * A conditional expression must normalize each arm before the arms are
 * joined: decoding only the merged value is unsafe because one arm may
 * already be raw while the other still carries the tagged dynamic ABI. */
static int nyr_lower_int_value(nyr_builder_t *builder,
                               const expr_t *value_expr) {
  if (!value_expr)
    return -1;
  /* The physical local carrier wins over a stale contextual float fact.
   * This occurs when an `any` parameter (for example a slice bound) receives
   * an integer assignment and HM annotates a later `int(local)` from that
   * assignment as f64. The slot still contains a tagged Ny word and must be
   * decoded through the dynamic conversion before integer arithmetic. */
  if (value_expr->kind == NY_E_IDENT && value_expr->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local_nearest(builder, value_expr->as.ident.name);
    if (local &&
        (local->is_any ||
         (local->type_name && ny_native_type_name_is_any(local->type_name)))) {
      int value = nyr_lower_expr(builder, value_expr);
      return value < 0 ? -1
                       : nyr_emit_runtime_call(builder, "rt_any_to_i64", value,
                                               -1, -1, 1, 0);
    }
  }
  if (nyr_expr_is_f64_depth(builder, value_expr, 0)) {
    int value = nyr_lower_f64_arg(builder, value_expr);
    return value < 0 ? -1 : nyr_emit_f64_to_i64(builder, value);
  }
  if (nyr_expr_is_f32_depth(builder, value_expr, 0)) {
    int value = nyr_lower_expr(builder, value_expr);
    return value < 0 ? -1 : nyr_emit_f32_to_i64(builder, value);
  }
  if (value_expr->kind == NY_E_TERNARY && value_expr->as.ternary.cond &&
      value_expr->as.ternary.true_expr && value_expr->as.ternary.false_expr) {
    if (value_expr->as.ternary.cond->kind == NY_E_LITERAL) {
      bool choose_true = false;
      if (value_expr->as.ternary.cond->as.literal.kind == NY_LIT_BOOL)
        choose_true = value_expr->as.ternary.cond->as.literal.as.b;
      else if (value_expr->as.ternary.cond->as.literal.kind == NY_LIT_INT)
        choose_true = value_expr->as.ternary.cond->as.literal.as.i != 0;
      else
        goto lower_as_value;
      return nyr_lower_int_value(
          builder, choose_true ? value_expr->as.ternary.true_expr
                               : value_expr->as.ternary.false_expr);
    }
    int result_slot = nyr_temp_slot(builder);
    int true_label = builder->next_label++;
    int false_label = builder->next_label++;
    int end_label = builder->next_label++;
    int cond = nyr_lower_expr(builder, value_expr->as.ternary.cond);
    if (cond < 0 || !nyr_emit_br_if(builder, cond, true_label) ||
        !nyr_emit_br(builder, false_label) ||
        !nyr_emit_label(builder, true_label))
      return -1;
    int value = nyr_lower_int_value(builder, value_expr->as.ternary.true_expr);
    if (value < 0 || !nyr_store_local_value(builder, result_slot, value) ||
        !nyr_emit_br(builder, end_label) ||
        !nyr_emit_label(builder, false_label))
      return -1;
    value = nyr_lower_int_value(builder, value_expr->as.ternary.false_expr);
    if (value < 0 || !nyr_store_local_value(builder, result_slot, value) ||
        !nyr_emit_label(builder, end_label))
      return -1;
    return nyr_load_local_value(builder, result_slot);
  }

lower_as_value:
  {
    /* Index operands are integer ABI values even when the surrounding
     * consumer is lowering an f64/string-producing expression.  Clear the
     * inherited floating context while lowering the ordinary integer path;
     * otherwise `i + j` is promoted to f64 and later bitcast into a bogus
     * sequence index. */
    int value = nyr_lower_integer_expr(builder, value_expr);
    if (value < 0)
      return -1;
    /* A lowering producer can prove a raw i64 even when imported constant
     * expressions retain a stale `any` semantic annotation.  Trust the
     * physical register class at this boundary; untagging raw 127 here would
     * silently turn a capability mask into 63. */
    if (nyr_value_scalar_class(builder, value) == NYIR_SCALAR_I64_RAW)
      return value;
    if (nyr_value_scalar_class(builder, value) == NYIR_SCALAR_I64_TAGGED)
      return nyr_emit_runtime_call(builder, "rt_any_to_i64", value, -1, -1, 1,
                                   NYIR_SCALAR_I64_RAW);
    /* Descriptor-backed sequence reads carry the canonical dynamic word even
     * when inference has attached a scalar fallback type to the expression.
     * Normalize that word at the first typed-integer boundary; otherwise a
     * byte load stores the tagged payload (97 -> 195) and corrupts every
     * algorithm that consumes a dynamic byte/list as an int. */
    bool canonical_dynamic =
        nyr_expr_is_canonical_dynamic_value(builder, value_expr);
    if (!builder->value_for_any_call &&
        (nyr_expr_is_any(builder, value_expr) || canonical_dynamic) &&
        !nyr_expr_is_raw_integer_conversion(builder, value_expr) &&
        !nyr_expr_has_raw_int_abi(builder, value_expr, 0) &&
        !nyr_expr_is_raw_int(value_expr) &&
        !nyr_expr_is_raw_dynamic_read(builder, value_expr))
      value =
          nyr_emit_runtime_call(builder, "rt_any_to_i64", value, -1, -1, 1, 0);
    return value;
  }
}

static bool nyr_expr_may_dynamic_value(nyr_builder_t *b, const expr_t *e,
                                       unsigned depth) {
  if (!e || depth > 8)
    return false;
  if (nyr_expr_is_any(b, e))
    return true;
  if (e->kind == NY_E_TERNARY)
    return nyr_expr_may_dynamic_value(b, e->as.ternary.true_expr, depth + 1) ||
           nyr_expr_may_dynamic_value(b, e->as.ternary.false_expr, depth + 1);
  const expr_t *target = NULL;
  const char *method = NULL;
  if (e->kind == NY_E_MEMCALL) {
    target = e->as.memcall.target;
    method = e->as.memcall.name;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_MEMBER) {
    target = e->as.call.callee->as.member.target;
    method = e->as.call.callee->as.member.name;
  }
  if (target && method &&
      (strcmp(method, "get") == 0 || strcmp(method, "pop") == 0) &&
      (nyr_expr_is_dict(b, target) || nyr_expr_is_any(b, target)))
    return true;
  if (e->kind == NY_E_INDEX && e->as.index.target) {
    if (nyr_expr_is_dict(b, e->as.index.target))
      return true;
    if (nyr_expr_is_dyn_list(b, e->as.index.target))
      return true;
    /* A constant scalar list is lowered as raw elem-8 storage.  Its index
     * result is therefore a raw integer even if the surrounding inference
     * conservatively marked the list expression dynamic. */
    const expr_t *literal_target =
        nyr_resolve_list_literal(b, e->as.index.target, 0);
    if (literal_target && literal_target->kind == NY_E_LIST &&
        !nyr_expr_is_dyn_list(b, literal_target))
      return false;
    if (nyr_expr_is_any(b, e->as.index.target)) {
      /* A nested list read with a proven scalar element type still returns a
       * raw slot through the typed accessor.  Do not format that slot as an
       * already-tagged any value. */
      const char *target_type = nyr_expr_type_name(b, e->as.index.target);
      if (target_type && strncmp(target_type, "list<", 5) == 0) {
        const char *element = target_type + 5;
        if (strncmp(element, "any>", 4) != 0 &&
            strncmp(element, "list<", 5) != 0 &&
            strncmp(element, "dict<", 5) != 0 &&
            strncmp(element, "tuple>", 6) != 0)
          return false;
      }
      return true;
    }
  }
  return false;
}

/* Recover the concrete callback behind a call that produces a callable.  A
 * `fnptr` annotation intentionally describes only the callable carrier, not
 * its signature, so the ordinary expression semantic cache cannot determine
 * the result ABI of `make(1)(2)`.  When the producer has an implicit returned
 * lambda, its AST still contains the exact generated function entry and
 * return semantic.  Keep this lookup structural and bounded; it applies to
 * every function-valued producer rather than to a particular library or
 * source spelling. */
static const expr_t *
nyr_find_callable_var_init(const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || !*name || depth > 32)
    return NULL;
  if (s->kind == NY_S_VAR) {
    for (size_t i = s->as.var.names.len; i > 0; --i) {
      size_t n = i - 1;
      if (s->as.var.names.data[n] &&
          strcmp(s->as.var.names.data[n], name) == 0 && n < s->as.var.exprs.len)
        return s->as.var.exprs.data[n];
    }
    return NULL;
  }
  if (s->kind == NY_S_BLOCK || s->kind == NY_S_MODULE) {
    const ny_stmt_list *body =
        s->kind == NY_S_BLOCK ? &s->as.block.body : &s->as.module.body;
    for (size_t i = body->len; i > 0; --i) {
      const expr_t *found =
          nyr_find_callable_var_init(body->data[i - 1], name, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const expr_t *nyr_callable_tail(const stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC || !fn->as.fn.body)
    return NULL;
  const stmt_t *body = fn->as.fn.body;
  if (body->kind == NY_S_EXPR)
    return body->as.expr.expr;
  if (body->kind != NY_S_BLOCK || body->as.block.body.len == 0)
    return NULL;
  const stmt_t *last = body->as.block.body.data[body->as.block.body.len - 1];
  if (!last)
    return NULL;
  if (last->kind == NY_S_EXPR)
    return last->as.expr.expr;
  if (last->kind == NY_S_RETURN)
    return last->as.ret.value;
  return NULL;
}

static bool nyr_callable_result_abi_matches(const stmt_t *left,
                                            const stmt_t *right) {
  if (!left || left->kind != NY_S_FUNC || !right || right->kind != NY_S_FUNC)
    return false;
  const char *left_type = left->as.fn.return_type;
  const char *right_type = right->as.fn.return_type;
  if ((left_type == NULL) != (right_type == NULL) ||
      (left_type && strcmp(left_type, right_type) != 0))
    return false;
  const ny_expr_semantic_t *left_slot = &left->as.fn.return_semantic;
  const ny_expr_semantic_t *right_slot = &right->as.fn.return_semantic;
  return left_slot->resolved == right_slot->resolved &&
         (!left_slot->resolved || left_slot->rep == right_slot->rep);
}

static const stmt_t *nyr_callable_result_fn(nyr_builder_t *b, const expr_t *e,
                                            unsigned depth) {
  if (!b || !e || depth > 8)
    return NULL;
  if (e->kind == NY_E_LAMBDA || e->kind == NY_E_FN) {
    ny_native_lambda_entry_t *entry =
        ny_native_lambda_find(ny_native_lambda_cache_for(b), e);
    return entry ? entry->fn : NULL;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    nyr_local_t *local = nyr_find_expr_local(b, e);
    if (local) {
      if (local->callable_expr)
        return nyr_callable_result_fn(b, local->callable_expr, depth + 1);
      if (local->callable_name)
        return nyr_find_user_function(b, local->callable_name);
      return NULL;
    }
    const stmt_t *named_function = nyr_find_user_function(b, e->as.ident.name);
    if (named_function)
      return named_function;
    const expr_t *global = nyr_find_top_level_value(b, e->as.ident.name);
    if (global && global != e)
      return nyr_callable_result_fn(b, global, depth + 1);
    return NULL;
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "get") == 0 && e->as.memcall.args.len >= 1 &&
      e->as.memcall.args.len <= 2 && e->as.memcall.target) {
    const expr_t *key_expr = e->as.memcall.args.data[0].val;
    const char *key = key_expr && key_expr->kind == NY_E_LITERAL &&
                              key_expr->as.literal.kind == NY_LIT_STR
                          ? key_expr->as.literal.as.s.data
                          : NULL;
    const expr_t *dict = nyr_resolve_dict_literal(b, e->as.memcall.target, 0);
    if (!dict && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name && b->current_fn_name) {
      const stmt_t *caller = nyr_find_user_function(b, b->current_fn_name);
      const expr_t *init = caller ? nyr_find_callable_var_init(
                                        caller->as.fn.body,
                                        e->as.memcall.target->as.ident.name, 0)
                                  : NULL;
      dict = init ? nyr_resolve_dict_literal(b, init, 0) : NULL;
      if (!dict && init && init->kind == NY_E_CALL && init->as.call.callee &&
          init->as.call.callee->kind == NY_E_IDENT &&
          init->as.call.callee->as.ident.name) {
        const stmt_t *producer =
            nyr_find_user_function(b, init->as.call.callee->as.ident.name);
        const expr_t *tail = nyr_callable_tail(producer);
        dict = tail ? nyr_resolve_dict_literal(b, tail, 0) : NULL;
      }
    }
    if (!dict)
      dict = e->as.memcall.target;
    if (dict->kind != NY_E_DICT)
      return NULL;
    const stmt_t *result_fn = NULL;
    for (size_t i = 0; i < dict->as.dict.pairs.len; ++i) {
      const dict_pair_t *pair = &dict->as.dict.pairs.data[i];
      if (!pair->key || pair->key->kind != NY_E_LITERAL ||
          pair->key->as.literal.kind != NY_LIT_STR ||
          !pair->key->as.literal.as.s.data ||
          (key && strcmp(pair->key->as.literal.as.s.data, key) != 0))
        continue;
      const stmt_t *candidate =
          nyr_callable_result_fn(b, pair->value, depth + 1);
      if (!candidate ||
          (result_fn && !nyr_callable_result_abi_matches(result_fn, candidate)))
        return NULL;
      if (!result_fn)
        result_fn = candidate;
    }
    if (!key && e->as.memcall.args.len == 2) {
      const call_arg_t *fallback_arg = &e->as.memcall.args.data[1];
      const stmt_t *fallback_fn =
          fallback_arg->val
              ? nyr_callable_result_fn(b, fallback_arg->val, depth + 1)
              : NULL;
      if (!fallback_fn || (result_fn && !nyr_callable_result_abi_matches(
                                            result_fn, fallback_fn)))
        return NULL;
      if (!result_fn)
        result_fn = fallback_fn;
    }
    return result_fn;
  }
  if (e->kind != NY_E_CALL || !e->as.call.callee)
    return NULL;
  const expr_t *callee = e->as.call.callee;
  const stmt_t *producer = NULL;
  if (callee->kind == NY_E_IDENT && callee->as.ident.name) {
    nyr_local_t *local = nyr_find_expr_local(b, callee);
    if (local) {
      if (local->callable_name) {
        producer = nyr_find_user_function(b, local->callable_name);
      } else if (local->callable_expr &&
                 (local->callable_expr->kind == NY_E_FN ||
                  local->callable_expr->kind == NY_E_LAMBDA)) {
        ny_native_lambda_entry_t *entry = ny_native_lambda_find(
            ny_native_lambda_cache_for(b), local->callable_expr);
        producer = entry ? entry->fn : NULL;
      }
    } else {
      producer = nyr_find_user_function(b, callee->as.ident.name);
    }
  }
  if (!producer)
    return NULL;
  const expr_t *tail = nyr_callable_tail(producer);
  if (!tail)
    return NULL;
  if (tail->kind == NY_E_IDENT && tail->as.ident.name) {
    const expr_t *init = nyr_find_callable_var_init(producer->as.fn.body,
                                                    tail->as.ident.name, 0);
    return init ? nyr_callable_result_fn(b, init, depth + 1) : NULL;
  }
  return nyr_callable_result_fn(b, tail, depth + 1);
}

/* Return only a directly initialized immutable module literal. Contract
 * evaluation may use this without observing runtime state; computed or
 * mutable globals deliberately remain unknown. A local or parameter with the
 * same spelling also prevents substitution. */
static expr_t *nyr_proof_global_literal(nyr_builder_t *b, const char *name) {
  if (!b || !b->prog || !name || !*name)
    return NULL;
  const stmt_t *caller =
      b->current_fn_name ? nyr_find_user_function(b, b->current_fn_name) : NULL;
  if (caller && caller->kind == NY_S_FUNC) {
    for (size_t i = 0; i < caller->as.fn.params.len; ++i)
      if (caller->as.fn.params.data[i].name &&
          strcmp(caller->as.fn.params.data[i].name, name) == 0)
        return NULL;
    if (nyr_find_callable_var_init(caller->as.fn.body, name, 0))
      return NULL;
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (!s || s->kind != NY_S_VAR || !s->as.var.is_decl || s->as.var.is_mut)
      continue;
    for (size_t j = 0; j < s->as.var.names.len; ++j) {
      if (!s->as.var.names.data[j] ||
          strcmp(s->as.var.names.data[j], name) != 0)
        continue;
      if (j >= s->as.var.exprs.len || !s->as.var.exprs.data[j] ||
          s->as.var.exprs.data[j]->kind != NY_E_LITERAL)
        return NULL;
      return s->as.var.exprs.data[j];
    }
  }
  return NULL;
}

static int nyr_lower_call(nyr_builder_t *b, const expr_t *e) {
  /* A statically known dictionary receiver can use the typed stdlib reader.
   * Keep it ahead of the global and local raw lookup shortcuts, which lose
   * the dynamic tag on fields loaded from layouts and generated adapters. */
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name &&
      strcmp(e->as.call.callee->as.member.name, "get") == 0 &&
      (e->as.call.args.len == 1 || e->as.call.args.len == 2) &&
      nyr_expr_is_dict(b, e->as.call.callee->as.member.target)) {
    const expr_t *key_expr = e->as.call.args.data[0].val;
    const expr_t *fallback_expr = e->as.call.args.data[1].val;
    const expr_t *decimal_key_expr = NULL;
    if (e->as.call.args.len == 2 && key_expr && key_expr->kind == NY_E_CALL &&
        fallback_expr && fallback_expr->kind == NY_E_LITERAL &&
        fallback_expr->as.literal.kind == NY_LIT_INT &&
        key_expr->as.call.args.len == 1 &&
        !key_expr->as.call.args.data[0].name && nyr_early_leaf(key_expr) &&
        strcmp(nyr_early_leaf(key_expr), "to_str") == 0 &&
        !nyr_expr_is_any(b, key_expr->as.call.args.data[0].val) &&
        nyr_expr_has_raw_int_abi(b, key_expr->as.call.args.data[0].val, 0) &&
        !nyr_expr_is_bigint(b, key_expr->as.call.args.data[0].val)) {
      const expr_t *inner_callee = key_expr->as.call.callee;
      const char *inner_name = inner_callee && inner_callee->kind == NY_E_IDENT
                                   ? inner_callee->as.ident.name
                                   : NULL;
      if (inner_name && strcmp(inner_name, "to_str") == 0 &&
          !nyr_user_defined_fn(b, inner_name))
        decimal_key_expr = key_expr->as.call.args.data[0].val;
    }
    if (decimal_key_expr) {
      int target_v = nyr_lower_expr(b, e->as.call.callee->as.member.target);
      int key_v = nyr_lower_expr(b, decimal_key_expr);
      int fallback = nyr_lower_expr(b, fallback_expr);
      if (target_v < 0 || key_v < 0 || fallback < 0)
        return -1;
      if (!nyr_expr_is_any(b, fallback_expr) &&
          !nyr_expr_is_cstr(b, fallback_expr) &&
          !nyr_expr_is_list(b, fallback_expr) &&
          !nyr_expr_is_dict(b, fallback_expr) &&
          !nyr_expr_is_bool(b, fallback_expr) &&
          fallback_expr->kind != NY_E_FN &&
          fallback_expr->kind != NY_E_LAMBDA &&
          fallback_expr->semantic.rep != NY_SEM_REP_CLOSURE &&
          !ny_expr_is_nil_literal(fallback_expr)) {
        fallback = nyr_emit_runtime_call(b, "rt_tag", fallback, -1, -1, 1, 0);
        if (fallback < 0)
          return -1;
      } else if (nyr_expr_is_bool(b, fallback_expr)) {
        fallback = nyr_box_bool(b, fallback);
        if (fallback < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_dict_get_i64_str_raw", target_v,
                                   key_v, fallback, 3, NYIR_SCALAR_I64_TAGGED);
    }
    call_arg_t direct_args[3] = {
        {.name = NULL, .val = e->as.call.callee->as.member.target},
        {.name = NULL, .val = e->as.call.args.data[0].val},
        {.name = NULL,
         .val = e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL},
    };
    expr_t direct_callee = {.kind = NY_E_IDENT, .tok = e->tok};
    direct_callee.as.ident.name = "core.dict_mod.dict_get";
    expr_t direct = *e;
    direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
    direct.semantic.canonical_callee = NULL;
    direct.semantic.canonical_callee_stmt = NULL;
    direct.as.call.callee = &direct_callee;
    direct.as.call.args.data = direct_args;
    direct.as.call.args.len = e->as.call.args.len + 1;
    direct.as.call.args.cap = direct.as.call.args.len;
    return nyr_lower_call(b, &direct);
  }
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_SPLICE) {
    const expr_t *callee = e->as.call.callee;
    while (callee && callee->kind == NY_E_SPLICE)
      callee = callee->as.splice.expr;
    expr_t direct = *e;
    direct.as.call.callee = (expr_t *)callee;
    return nyr_lower_call(b, &direct);
  }
  /*
   * Semantic completion can canonicalize an explicitly qualified attached
   * call (`core.len(xs)`) to its implementation name and materialize the
   * module expression as an implicit receiver.  The module path is not the
   * method's receiver; remove only that exact, verified namespace prefix
   * before checking the implementation's source arity.
   */
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name && e->as.call.args.len >= 2 &&
      e->as.call.args.data[0].val && !e->as.call.args.data[0].name) {
    const char *name = e->as.call.callee->as.ident.name;
    const char *any_owner = strstr(name, ".any.");
    const stmt_t *resolved = nyr_find_user_function(b, name);
    char receiver_name[512];
    const expr_t *receiver = e->as.call.args.data[0].val;
    if (any_owner && any_owner != name && resolved &&
        ny_is_stdlib_tok(resolved->tok) &&
        nyr_qualified_expr(receiver, receiver_name, sizeof(receiver_name))) {
      size_t receiver_len = strlen(receiver_name);
      const expr_t *root = receiver;
      while (root && root->kind == NY_E_MEMBER)
        root = root->as.member.target;
      const expr_t *bound_receiver_value =
          nyr_find_top_level_value(b, receiver_name);
      if (receiver_len == (size_t)(any_owner - name) &&
          strncmp(name, receiver_name, receiver_len) == 0 && root &&
          root->kind == NY_E_IDENT && root->as.ident.name &&
          !nyr_find_local(b, root->as.ident.name) && !bound_receiver_value) {
        expr_t direct = *e;
        direct.as.call.args.data++;
        direct.as.call.args.len--;
        direct.as.call.args.cap = direct.as.call.args.len;
        return nyr_lower_call(b, &direct);
      }
    }
  }
  /* Resolve an explicit namespace through its import binding before
   * treating the member-shaped syntax as a runtime receiver. */
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER) {
    char qualified[512];
    const expr_t *root = e->as.call.callee;
    while (root && root->kind == NY_E_MEMBER)
      root = root->as.member.target;
    bool shadowed_namespace = root && root->kind == NY_E_IDENT &&
                              nyr_find_local(b, root->as.ident.name);
    bool have_qualified =
        nyr_qualified_expr(e->as.call.callee, qualified, sizeof(qualified));
    if (have_qualified && !shadowed_namespace && root &&
        root->kind == NY_E_IDENT && root->as.ident.name) {
      const char *dot = strchr(qualified, '.');
      if (dot && dot > qualified) {
        nyr_builder_t probe = *b;
        probe.source_file = NULL;
        probe.locals = NULL;
        probe.local_count = probe.local_cap = 0;
        const char *module = nyr_resolve_use_alias(&probe, root->as.ident.name);
        if (module) {
          char canonical[512];
          int n = snprintf(canonical, sizeof(canonical), "%s%s", module, dot);
          if (n > 0 && (size_t)n < sizeof(canonical))
            memcpy(qualified, canonical, (size_t)n + 1);
        }
      }
    }
    if (!shadowed_namespace && have_qualified &&
        nyr_find_user_function(b, qualified)) {
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = qualified;
      expr_t direct = *e;
      direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
      direct.semantic.canonical_callee = NULL;
      direct.semantic.canonical_callee_stmt = NULL;
      direct.as.call.callee = &callee;
      return nyr_lower_call(b, &direct);
    }
  }
  /* An eligible @async_effects call constructs a stackless task implicitly.
   * Reuse the explicit `async f(args)` lowering so callback ABI metadata and
   * argument lifetime handling stay identical across both spellings. */
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    const stmt_t *async_fn =
        nyr_find_user_function(b, e->as.call.callee->as.ident.name);
    if (async_fn && async_fn->kind == NY_S_FUNC &&
        async_fn->as.fn.attr_async_effects_lowered) {
      expr_t task = {.kind = NY_E_UNARY, .tok = e->tok};
      task.as.unary.op = "async";
      task.as.unary.right = (expr_t *)e;
      return nyr_lower_expr(b, &task);
    }
  }
  const bool callback_word_abi =
      !b->options || b->options->native_backend != NY_NATIVE_BACKEND_LLVM;
  /* `range` is variadic at the language level, while the old AST ABI models
   * its packed argument list as an `any` aggregate.  NYIR has no aggregate
   * varargs representation, so lower the three supported forms directly to
   * the raw range allocator. */
  const char *early_leaf = ny_native_call_leaf(e);
  if (!early_leaf)
    early_leaf = "";
  /* Semantic-first view shared by the raw-vs-dynamic intrinsic families:
   * __shl, bitwise primitives, ptr_add, and range. The canonical callee
   * leaf wins over the surface spelling so aliases/attached sugar keep the
   * raw path; spelling remains the fallback for unresolved nodes. */
  const char *canon_leaf = nyr_early_leaf(e);
  if (!canon_leaf)
    canon_leaf = "";

  /* Source declarations own their calls even when their leaf spelling is
   * also an intrinsic. Apply this before every early intrinsic family. */
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
      nyr_user_defined_fn(b, e->as.call.callee->as.ident.name))
    goto ordinary_call;
  /* A source-level callback marker is shared with the LLVM callback bridge,
   * whose dynamic formals use value/length/tag triples. Native NYIR and
   * machine-code functions use one word per source argument, so translate
   * these public markers here and carry the native callback ABI explicitly.
   */
  const char *native_callback_marker =
      nyr_dynamic_marker_symbol(early_leaf, NULL, NULL);
  if (native_callback_marker && callback_word_abi) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR: dynamic callback marker "
                         "expects one positional argument"),
             -1;
    }
    int callable = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return callable < 0 ? -1
                        : nyr_emit_runtime_call(b, native_callback_marker,
                                                callable, -1, -1, 1, 0);
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-call", "%s args=%zu begin",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           e->as.call.args.len);
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name && e->as.call.args.len == 2 &&
      strcmp(early_leaf, "_set_remove") == 0 &&
      nyr_expr_is_set(b, e->as.call.args.data[0].val)) {
    int set_value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int key =
        set_value < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (set_value < 0 || key < 0)
      return -1;
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event("native-abi",
                             "function=%s call=_set_remove receiver=set "
                             "decision=use-raw-set-remove-runtime-entry",
                             b->current_fn_name ? b->current_fn_name
                                                : "<entry>");
    return nyr_emit_runtime_call(b, "rt_set_remove_raw", set_value, key, -1, 2,
                                 0);
  }
  /* Some alias-qualified module calls arrive here as unresolved surface
   * members (`set_mod.add`) even though semantic analysis has proved the
   * receiver is the set module. Preserve the set handle's native pointer ABI
   * across this call instead of routing it through the stdlib `any` wrapper,
   * which tags the pointer and makes `_set_add` reject it as a non-set. */
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name &&
      strcmp(e->as.call.callee->as.member.name, "add") == 0 &&
      e->as.call.callee->as.member.target &&
      e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
      e->as.call.callee->as.member.target->as.ident.name &&
      e->as.call.args.len == 2 &&
      nyr_expr_is_set(b, e->as.call.args.data[0].val)) {
    const char *module = nyr_resolve_use_alias(
        b, e->as.call.callee->as.member.target->as.ident.name);
    if (module && (strcmp(module, "core.set_mod") == 0 ||
                   strcmp(module, "core.set") == 0)) {
      int set_value = nyr_lower_expr(b, e->as.call.args.data[0].val);
      int key =
          set_value < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (set_value < 0 || key < 0)
        return -1;
      if (nyr_value_scalar_class(b, set_value) == NYIR_SCALAR_I64_TAGGED)
        set_value = nyr_emit_runtime_call(b, "rt_untag", set_value, -1, -1, 1,
                                          NYIR_SCALAR_TYPED_BUFFER);
      if (set_value < 0)
        return -1;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s receiver=%s call=add decision=preserve-set-handle",
            b->current_fn_name ? b->current_fn_name : "<entry>", module);
      return nyr_emit_runtime_call(b, "rt_set_add_raw", set_value, key, -1, 2,
                                   0);
    }
  }
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name &&
      (strcmp(e->as.call.callee->as.member.name, "sub") == 0 ||
       strcmp(e->as.call.callee->as.member.name, "remove") == 0 ||
       strcmp(e->as.call.callee->as.member.name, "delete") == 0) &&
      e->as.call.callee->as.member.target && e->as.call.args.len == 1 &&
      nyr_expr_is_set(b, e->as.call.callee->as.member.target)) {
    int set_value = nyr_lower_expr(b, e->as.call.callee->as.member.target);
    int key =
        set_value < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (set_value < 0 || key < 0)
      return -1;
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event("native-abi",
                             "function=%s call=%s receiver=set "
                             "decision=use-raw-set-remove-runtime-entry",
                             b->current_fn_name ? b->current_fn_name
                                                : "<entry>",
                             e->as.call.callee->as.member.name);
    return nyr_emit_runtime_call(b, "rt_set_remove_raw", set_value, key, -1, 2,
                                 0);
  }
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name && e->as.call.args.len == 2 &&
      nyr_expr_is_set(b, e->as.call.args.data[0].val)) {
    const char *spelling = e->as.call.callee->as.ident.name;
    const char *dot = strrchr(spelling, '.');
    if (dot && strcmp(dot + 1, "add") == 0 && dot > spelling &&
        (size_t)(dot - spelling) < 512) {
      char alias[512];
      size_t alias_len = (size_t)(dot - spelling);
      memcpy(alias, spelling, alias_len);
      alias[alias_len] = '\0';
      const char *module = nyr_resolve_use_alias(b, alias);
      if ((module && (strcmp(module, "core.set_mod") == 0 ||
                      strcmp(module, "core.set") == 0)) ||
          strcmp(alias, "core.set_mod") == 0 ||
          strcmp(alias, "core.set") == 0) {
        int set_value = nyr_lower_expr(b, e->as.call.args.data[0].val);
        int key =
            set_value < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
        if (set_value < 0 || key < 0)
          return -1;
        if (nyr_value_scalar_class(b, set_value) == NYIR_SCALAR_I64_TAGGED)
          set_value = nyr_emit_runtime_call(b, "rt_untag", set_value, -1, -1, 1,
                                            NYIR_SCALAR_TYPED_BUFFER);
        if (set_value < 0)
          return -1;
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "native-abi",
              "function=%s receiver=%s call=add decision=preserve-set-handle",
              b->current_fn_name ? b->current_fn_name : "<entry>",
              module ? module : alias);
        return nyr_emit_runtime_call(b, "rt_set_add_raw", set_value, key, -1, 2,
                                     0);
      }
    }
  }
  /* BigInt-domain values may use either a heap BigInt handle or the tagged
   * small-int representation after arithmetic demotion.  Do not fold
   * bigint(x) to an identity from static type alone: the stdlib conversion
   * is the normalization boundary that distinguishes those representations.
   */
  /* The public __shl intrinsic is tagged in the interpreter ABI, but native
   * NYIR arguments are raw scalar integers. Route it through the raw adapter
   * so overflow promotes to BigInt instead of wrapping the machine shift. */
  if (canon_leaf && strcmp(canon_leaf, "__shl") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int left = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int right = left < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (left < 0 || right < 0)
      return -1;
    return nyr_emit_runtime_call(b, "rt_shl_raw", left, right, -1, 2, 0);
  }
  /* __shr/bshr has a tagged interpreter entry, but native operands are raw
   * integers.  Keep the shift count raw as well; otherwise an odd count is
   * decoded as a tagged immediate and every other logical shift is wrong. */
  if (canon_leaf &&
      (strcmp(canon_leaf, "__shr") == 0 || strcmp(canon_leaf, "bshr") == 0 ||
       strcmp(early_leaf, "bshr") == 0) &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int left = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int right = left < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (left < 0 || right < 0)
      return -1;
    return nyr_emit_runtime_call(b, "rt_bshr_raw", left, right, -1, 2, 0);
  }
  /* Primitive bitwise helpers are tagged in the interpreter, while native
   * scalar callers (flags, masks, ABI fields) carry raw i64 values. Preserve
   * the raw domain when both operands are statically scalar; routing these
   * through rt_or would shift each value as if it were a tagged integer and
   * silently turn masks such as 1|2 into 5. Dynamic operands keep the normal
   * call path below. */
  if (canon_leaf && e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name &&
      nyr_leaf_in(canon_leaf,
                  (const char *const[]){
                      "__or",
                      "bor",
                      "__and",
                      "band",
                      "__xor",
                      "bxor",
                  },
                  6)) {
    const expr_t *left_expr = e->as.call.args.data[0].val;
    const expr_t *right_expr = e->as.call.args.data[1].val;
    bool dynamic = nyr_expr_is_any(b, e) || nyr_expr_is_any(b, left_expr) ||
                   nyr_expr_is_any(b, right_expr);
    if (dynamic && nyr_raw_bitwise_value(b, left_expr, 0) &&
        nyr_raw_bitwise_value(b, right_expr, 0))
      dynamic = false;
    /* Primitive wrappers are untyped, but a function with an explicit raw
     * integer result (feature masks, flags, ABI words) establishes the
     * scalar domain for its local bitwise accumulation. */
    if (dynamic && b->current_fn_name) {
      const stmt_t *owner = nyr_find_user_function(b, b->current_fn_name);
      if (owner && owner->as.fn.return_type &&
          !ny_native_type_name_is_any(owner->as.fn.return_type) &&
          !ny_native_type_name_is_list(owner->as.fn.return_type) &&
          !ny_native_type_name_is_f64(owner->as.fn.return_type) &&
          !ny_native_type_name_is_f32(owner->as.fn.return_type))
        dynamic = false;
    }
    if (!dynamic) {
      int left = nyr_lower_expr(b, left_expr);
      int right = left < 0 ? -1 : nyr_lower_expr(b, right_expr);
      if (left < 0 || right < 0)
        return -1;
      int op =
          (strcmp(canon_leaf, "__or") == 0 || strcmp(canon_leaf, "bor") == 0)
              ? NYIR_OR_I64
              : ((strcmp(canon_leaf, "__and") == 0 ||
                  strcmp(canon_leaf, "band") == 0)
                     ? NYIR_AND_I64
                     : NYIR_XOR_I64);
      int result = nyr_emit_binop(b, op, left, right);
      if (result >= 0 && b->nyir.len > 0 &&
          b->nyir.data[b->nyir.len - 1].dst == result)
        b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
      return result;
    }
  }
  if (canon_leaf && strcmp(canon_leaf, "ptr_add") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int ptr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int off = ptr < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
    return off < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_ptr_add_i64", ptr, off, -1, 2, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "range") == 0 ||
       strcmp(canon_leaf, "range_new_raw") == 0) &&
      e->as.call.args.len >= 1 && e->as.call.args.len <= 3) {
    bool named = false;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      named = named || e->as.call.args.data[i].name != NULL;
    if (!named) {
      int start = nyr_emit_const(b, 0);
      int stop = -1;
      int step = nyr_emit_const(b, 1);
      if (e->as.call.args.len == 1) {
        stop = nyr_lower_expr(b, e->as.call.args.data[0].val);
      } else {
        start = nyr_lower_expr(b, e->as.call.args.data[0].val);
        stop = nyr_lower_expr(b, e->as.call.args.data[1].val);
        if (e->as.call.args.len == 3)
          step = nyr_lower_expr(b, e->as.call.args.data[2].val);
      }
      if (start < 0 || stop < 0 || step < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_range_new_raw", start, stop, step, 3,
                                   0);
    }
  }
  if (early_leaf && strcmp(early_leaf, "unwrap") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int v = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (v < 0)
      return -1;
    /* `unwrap` is declared to return `any`. Keep the Result payload's
     * canonical tagged representation for dynamic consumers; the raw helper
     * converts tagged scalar payloads to their native representation. Keep
     * its NYIR class aligned with the resolved payload: strings and handles
     * pass through as words, while floats need the dedicated f64 conversion.
     */
    if (nyr_expr_is_any(b, e) &&
        (!e->semantic.resolved || e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC))
      return nyr_emit_runtime_call(b, "rt_unwrap", v, -1, -1, 1,
                                   NYIR_SCALAR_I64_TAGGED);
    ny_sem_rep_t payload_rep =
        e->semantic.resolved ? e->semantic.rep : NY_SEM_REP_UNRESOLVED;
    if (payload_rep == NY_SEM_REP_UNRESOLVED ||
        payload_rep == NY_SEM_REP_TAGGED_DYNAMIC) {
      const char *payload_type = nyr_expr_type_name(b, e);
      payload_rep = nyr_rep_for_type(b, payload_type);
    }
    nyr_scalar_class_t payload_class =
        ny_native_scalar_class_from_sem_rep(payload_rep);
    if (payload_class == NYIR_SCALAR_F64 || payload_class == NYIR_SCALAR_F32) {
      int tagged = nyr_emit_runtime_call(b, "rt_unwrap", v, -1, -1, 1,
                                         NYIR_SCALAR_I64_TAGGED);
      int f64 = tagged < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_any_to_f64", tagged,
                                                   -1, -1, 1, NYIR_SCALAR_F64);
      if (f64 < 0 || payload_class == NYIR_SCALAR_F64)
        return f64;
      return nyr_emit_f64_to_f32(b, f64);
    }
    if (payload_class == NYIR_SCALAR_UNKNOWN ||
        payload_class == NYIR_SCALAR_I64_TAGGED)
      return nyr_emit_runtime_call(b, "rt_unwrap", v, -1, -1, 1,
                                   NYIR_SCALAR_I64_TAGGED);
    return nyr_emit_runtime_call(b, "rt_result_unwrap_raw", v, -1, -1, 1,
                                 payload_class);
  }
  if (early_leaf && strcmp(early_leaf, "unwrap_or") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    const expr_t *fallback = e->as.call.args.data[1].val;
    int v = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int def_val = nyr_lower_expr(b, fallback);
    if (v < 0 || def_val < 0)
      return -1;
    /* The source declaration returns any. The raw helper is only suitable
     * when inference proves a scalar result; an untyped fallback must retain
     * its canonical tag (raw zero otherwise becomes nil in an any local). */
    if (nyr_expr_is_any(b, e)) {
      if (!nyr_expr_is_any(b, fallback)) {
        if (nyr_expr_is_bool(b, fallback)) {
          def_val = nyr_box_bool(b, def_val);
        } else if (nyr_expr_is_f64(b, fallback) ||
                   nyr_expr_is_f32(b, fallback)) {
          int f64 = nyr_expr_is_f32(b, fallback)
                        ? nyr_emit_f32_to_f64(b, def_val)
                        : def_val;
          int bits = f64 < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                     -1, 1, 0);
          def_val = bits < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                     -1, -1, 1, 0);
        } else if (nyr_expr_is_cstr(b, fallback)) {
          def_val =
              nyr_emit_runtime_call(b, "rt_cstr_to_str", def_val, -1, -1, 1, 0);
        } else if (!nyr_expr_is_list(b, fallback) &&
                   !nyr_expr_is_dict(b, fallback) &&
                   !nyr_expr_is_bytes(b, fallback) &&
                   !nyr_expr_is_tuple(b, fallback)) {
          def_val = nyr_box_raw_dynamic_value(b, def_val);
        }
      }
      return def_val < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_result_unwrap_or_any",
                                                 v, def_val, -1, 2, 0);
    }
    return nyr_emit_runtime_call(b, "rt_result_unwrap_or_raw", v, def_val, -1,
                                 2, 0);
  }
  const char *result_leaf =
      early_leaf && (strcmp(early_leaf, "__result_ok") == 0 ||
                     strcmp(early_leaf, "__result_err") == 0)
          ? early_leaf
          : canon_leaf;
  if (result_leaf &&
      (strcmp(result_leaf, "__result_ok") == 0 ||
       strcmp(result_leaf, "__result_err") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *payload_expr = e->as.call.args.data[0].val;
    int payload = nyr_lower_expr(b, payload_expr);
    if (payload < 0)
      return -1;
    /* Result constructors consume the language-value ABI.  Native NYIR
     * scalar expressions are raw, so normalize only those values here; a
     * dynamic value or managed object already carries its runtime form. */
    if (!nyr_expr_is_any(b, payload_expr)) {
      if (nyr_expr_is_f64(b, payload_expr)) {
        int bits =
            nyr_emit_runtime_call(b, "rt_f64_bits", payload, -1, -1, 1, 0);
        payload = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
      } else if (nyr_expr_is_f32(b, payload_expr)) {
        int f64 = nyr_emit_f32_to_f64(b, payload);
        int bits = f64 < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                   -1, 1, 0);
        payload = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
      } else if (nyr_expr_is_bool(b, payload_expr)) {
        payload = nyr_box_bool(b, payload);
      } else if (!nyr_expr_is_cstr(b, payload_expr) &&
                 !nyr_expr_is_list(b, payload_expr) &&
                 !nyr_expr_is_dict(b, payload_expr) &&
                 !nyr_expr_is_bytes(b, payload_expr) &&
                 !nyr_expr_is_tuple(b, payload_expr)) {
        payload = nyr_emit_runtime_call(b, "rt_tag", payload, -1, -1, 1, 0);
      }
    }
    if (payload < 0)
      return -1;
    return nyr_emit_runtime_call(b,
                                 strcmp(result_leaf, "__result_ok") == 0
                                     ? "rt_result_ok"
                                     : "rt_result_err",
                                 payload, -1, -1, 1, 0);
  }
  if (early_leaf && strcmp(early_leaf, "clone") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    /* clone(any) is representation-polymorphic. Keep dispatch at the
     * runtime boundary so overload lookup cannot select a container ABI from
     * an unrelated attached method with the same leaf name. */
    return nyr_emit_runtime_call(b, "__clone_raw", value, -1, -1, 1, 0);
  }
  /* These core helpers still have a legacy heap-list implementation in
   * source for the interpreter.  Native lists are tbufs, so route the
   * implementation-sensitive operations through the ABI bridge even when the
   * formal parameter is `any`. */
  if (early_leaf && strcmp(early_leaf, "_clone_list") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int list = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return list < 0 ? -1
                    : nyr_emit_runtime_call(b, "rt_tbuf_clone_raw", list, -1,
                                            -1, 1, 0);
  }
  if (early_leaf && strcmp(early_leaf, "clear") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0
               ? -1
               : nyr_emit_runtime_call(b, "__clear_raw", value, -1, -1, 1, 0);
  }
  if (early_leaf &&
      (strcmp(early_leaf, "swap_items") == 0 ||
       strcmp(early_leaf, "swap") == 0) &&
      e->as.call.args.len == 3 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name && !e->as.call.args.data[2].name) {
    int list = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int left = list < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
    int right = left < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[2].val);
    return right < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_tbuf_swap", list, left,
                                             right, 3, 0);
  }
  /* Native list buffers are mutable.  Preserve the language-level clear
   * operation's in-place semantics; cloning here leaves the caller's list
   * unchanged and makes the following observation of the list surprising. */
  if (early_leaf && strcmp(early_leaf, "clear") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_list(b, e->as.call.args.data[0].val)) {
    int list = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return list < 0 ? -1
                    : nyr_emit_runtime_call(b, "rt_tbuf_clear_raw", list, -1,
                                            -1, 1, 0);
  }
  /* `count` is the sequence capability's length operation.  Its stdlib
   * implementation dispatches through the same `.len` surface, so use the
   * native length bridge for lists, strings, bytes, ranges, and dynamic
   * sequence parameters alike. */
  if (early_leaf && strcmp(early_leaf, "count") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    if (nyr_user_defined_fn(b, e->as.call.callee->as.ident.name))
      goto ordinary_call;
    int sequence = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return sequence < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_sequence_len_raw",
                                                sequence, -1, -1, 1, 0);
  }
  if (early_leaf && strcmp(early_leaf, "len") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      !nyr_user_defined_fn(b, e->as.call.callee->as.ident.name)) {
    int sequence = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (sequence < 0)
      return -1;
    /* NYIR scalar results are raw here.  Tagging this length made
     * len(list) return 2*n+1 while list.len returned n. */
    return nyr_emit_runtime_call(b, "rt_len_strict", sequence, -1, -1, 1, 0);
  }
  /* core.sort/sorted dispatch through the boxed list ABI in the VM, but
   * native literals and list locals are raw typed buffers.  Use the runtime's
   * tbuf-aware sorter and clone only for the non-mutating `sorted` surface.
   */
  if (early_leaf &&
      (strcmp(early_leaf, "sort") == 0 || strcmp(early_leaf, "sorted") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_list(b, e->as.call.args.data[0].val)) {
    int list = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (list < 0)
      return -1;
    if (strcmp(early_leaf, "sorted") == 0)
      list = nyr_emit_runtime_call(b, "rt_tbuf_clone_raw", list, -1, -1, 1, 0);
    return list < 0
               ? -1
               : nyr_emit_runtime_call(b, "__sort_list", list, -1, -1, 1, 0);
  }
  if (early_leaf &&
      (strcmp(early_leaf, "sort") == 0 || strcmp(early_leaf, "sorted") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_range(b, e->as.call.args.data[0].val)) {
    int range = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int list = range < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_range_values_raw",
                                                 range, -1, -1, 1, 0);
    return list < 0
               ? -1
               : nyr_emit_runtime_call(b, "__sort_list", list, -1, -1, 1, 0);
  }
  /* The legacy `any` ABI expands each operand to value/length/tag triples.
   * NYIR keeps the native value in one raw slot, so structural equality only
   * needs the two value positions. */
  if (early_leaf && strcmp(early_leaf, "eq") == 0 &&
      (e->as.call.args.len == 2 || e->as.call.args.len == 6)) {
    size_t rhs_index = e->as.call.args.len == 6 ? 3u : 1u;
    int left = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int right = nyr_lower_expr(b, e->as.call.args.data[rhs_index].val);
    if (ny_trace_enabled("NY_TRACE_ABI"))
      ny_trace_line("ABI_EQ",
                    "%s structural eq() fast path: left_cstr=%d "
                    "right_cstr=%d",
                    b->current_fn_name ? b->current_fn_name : "?",
                    nyr_expr_is_cstr(b, e->as.call.args.data[0].val),
                    nyr_expr_is_cstr(b, e->as.call.args.data[rhs_index].val));
    if (left >= 0 &&
        nyr_expr_has_raw_int_abi(b, e->as.call.args.data[0].val, 0))
      left = nyr_box_raw_dynamic_value(b, left);
    else if (left >= 0 && nyr_expr_is_cstr(b, e->as.call.args.data[0].val)) {
      /* A cstr-classified operand holds a raw C-string pointer (a
       * str-declared function's return crosses that way).  rt_any_eq only
       * recognizes managed strings, so a raw pointer would fall through to
       * the scalar pointer compare and report false for equal strings.
       * rt_cstr_to_str is idempotent for values already managed. */
      int conv = nyr_emit_runtime_call(b, "rt_cstr_to_str", left, -1, -1, 1, 0);
      if (conv < 0)
        return -1;
      left = conv;
    }
    if (right >= 0 &&
        nyr_expr_has_raw_int_abi(b, e->as.call.args.data[rhs_index].val, 0))
      right = nyr_box_raw_dynamic_value(b, right);
    else if (right >= 0 &&
             nyr_expr_is_cstr(b, e->as.call.args.data[rhs_index].val)) {
      int conv =
          nyr_emit_runtime_call(b, "rt_cstr_to_str", right, -1, -1, 1, 0);
      if (conv < 0)
        return -1;
      right = conv;
    }
    return left < 0 || right < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_any_eq", left, right, -1, 2, 0);
  }
  /* A top-level value can shadow an imported module alias just like a local.
   * Check the source-local table first because merged programs can contain
   * same-named globals from unrelated modules. */
  const char *module_alias = NULL;
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.target &&
      e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
      e->as.call.callee->as.member.target->as.ident.name) {
    const char *alias = e->as.call.callee->as.member.target->as.ident.name;
    const expr_t *global_value =
        e->tok.filename
            ? nyr_find_top_level_value_in_source(b, alias, e->tok.filename)
            : nyr_find_top_level_value(b, alias);
    bool local_value = nyr_find_local(b, alias) != NULL;
    /* Import aliases are also represented in the merged top-level value
     * table.  When semantic resolution selected a declaration from an
     * imported module, that module binding must win over its synthetic value
     * entry.  An explicit source local still shadows it. */
    bool imported_semantic_target =
        e->semantic.canonical_callee_stmt &&
        e->semantic.canonical_callee_stmt->kind == NY_S_FUNC;
    const stmt_t *source_value = e->tok.filename
                                     ? nyr_find_top_level_var_in_source_in_prog(
                                           b, b->prog, alias, e->tok.filename)
                                     : NULL;
    bool explicit_global_value = source_value != NULL;
    if (!e->tok.filename && global_value && !imported_semantic_target)
      explicit_global_value = nyr_find_top_level_var_stmt(b, alias) != NULL;
    bool bound_value = local_value || explicit_global_value;
    if (!bound_value)
      module_alias = nyr_resolve_use_alias(b, alias);
    if (!module_alias && !bound_value) {
      nyr_builder_t probe = *b;
      probe.source_file = NULL;
      probe.locals = NULL;
      probe.local_count = probe.local_cap = 0;
      module_alias = nyr_resolve_use_alias(&probe, alias);
    }
  }
  /* A qualified module call (`core_ref.get(x)`) is represented by the parser
   * as a member-shaped callee, but semantic attached-method resolution may
   * also annotate it as `core.any.get`.  The alias is a namespace, not a
   * runtime receiver: canonicalize it before the attached-call path can
   * materialize `core_ref` as an ELF data symbol. */
  if (e && e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.target &&
      e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
      e->as.call.callee->as.member.target->as.ident.name &&
      e->as.call.callee->as.member.name) {
    const char *module = module_alias;
    if (module) {
      char canonical[512];
      int n = snprintf(canonical, sizeof(canonical), "%s.%s", module,
                       e->as.call.callee->as.member.name);
      if (n <= 0 || (size_t)n >= sizeof(canonical)) {
        return nyr_fail(b, "native NYIR lower: module call name is too long"),
               -1;
      }
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = canonical;
      expr_t direct = *e;
      direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
      direct.semantic.canonical_callee = NULL;
      direct.semantic.canonical_callee_stmt = NULL;
      direct.as.call.callee = &callee;
      return nyr_lower_call(b, &direct);
    }
  }
  if (e->as.call.callee &&
      (e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED ||
       e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT)) {
    const char *canonical = e->semantic.canonical_callee;
    /* Type validation selected the exact declaration. Preserve its canonical
     * identity through lowering instead of resolving owner/name from syntax a
     * second time; that second lookup can choose a shadowed or re-exported
     * declaration with the same surface member name. */
    const stmt_t *canonical_decl = e->semantic.canonical_callee_stmt;
    if (canonical && canonical_decl && canonical_decl->kind == NY_S_FUNC &&
        ny_is_stdlib_tok(canonical_decl->tok)) {
      /* Import aliases and attached-method sugar can leave the semantic
       * callee spelled with the caller's alias (for example
       * `str.utf8_slice`) even though type resolution selected a declaration
       * owned by `core.str`.  Use the selected declaration's name so the
       * emitted call binds to its definition rather than an undefined alias
       * symbol. */
      if (canonical_decl->as.fn.name && strchr(canonical_decl->as.fn.name, '.'))
        canonical = canonical_decl->as.fn.name;
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
            return nyr_fail(b, "native NYIR lower: semantic "
                               "receiver name is too long"),
                   -1;
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
       * reference (for example `it.map` where `it` aliases `core.iter`).
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
        const expr_t *global_value = receiver->tok.filename
                                         ? nyr_find_top_level_value_in_source(
                                               b, rcv, receiver->tok.filename)
                                         : nyr_find_top_level_value(b, rcv);
        /* Import aliases may also be materialized in the top-level value
         * table as module registries.  The alias fact is authoritative: a
         * qualified import call is a namespace call even when that registry
         * happens to be discoverable as a value. */
        const bool bound_value =
            nyr_find_local(b, rcv) != NULL || global_value != NULL;
        namespace_receiver =
            (!bound_value && nyr_resolve_use_alias(b, rcv) != NULL) ||
            (!nyr_find_local(b, rcv) && !nyr_find_user_function(b, rcv) &&
             !global_value);
      }
      if (namespace_receiver) {
        const char *namespace_leaf = strrchr(canonical, '.');
        namespace_leaf = namespace_leaf ? namespace_leaf + 1 : canonical;
        if (namespace_leaf && strcmp(namespace_leaf, "add") == 0 &&
            e->as.call.args.len == 2 &&
            nyr_expr_is_set(b, e->as.call.args.data[0].val)) {
          int set_value = nyr_lower_expr(b, e->as.call.args.data[0].val);
          int key = nyr_lower_expr(b, e->as.call.args.data[1].val);
          if (set_value < 0 || key < 0)
            return -1;
          if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
            ny_dbg_heuristic_event(
                "native-abi",
                "function=%s call=%s receiver=set "
                "decision=use-raw-set-add-runtime-entry",
                b->current_fn_name ? b->current_fn_name : "<entry>", canonical);
          return nyr_emit_runtime_call(b, "rt_set_add_raw", set_value, key, -1,
                                       2, 0);
        }
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
        return nyr_lower_expr(b, &direct);
      }
      const char *canonical_leaf = strrchr(canonical, '.');
      canonical_leaf = canonical_leaf ? canonical_leaf + 1 : canonical;
      if (strcmp(canonical_leaf, "pop") == 0 && e->as.call.args.len == 0 &&
          (!canonical_decl || ny_is_stdlib_tok(canonical_decl->tok)) &&
          nyr_expr_is_list(b, receiver)) {
        int list = nyr_lower_expr(b, receiver);
        int out = list < 0
                      ? -1
                      : nyr_emit_runtime_call(b, "rt_tbuf_pop_any", list, -1,
                                              -1, 1, NYIR_SCALAR_I64_TAGGED);
        if (out < 0)
          return -1;
        int length = nyr_emit_known_list_pop_len(b, list);
        if (length < 0)
          length =
              nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1, -1, 1, 0);
        if (length < 0 || !nyr_record_list_len_fact(b, list, length))
          return -1;
        nyr_local_t *local = receiver->kind == NY_E_IDENT
                                 ? nyr_find_local(b, receiver->as.ident.name)
                                 : NULL;
        if (local) {
          if (local->list_len_slot < 0)
            local->list_len_slot = b->next_local_slot++;
          if (!nyr_store_local_value(b, local->list_len_slot, length))
            return -1;
        }
        return out;
      }
      if (strcmp(canonical_leaf, "add") == 0 && e->as.call.args.len == 1 &&
          nyr_expr_is_set(b, receiver)) {
        int set = nyr_lower_expr(b, receiver);
        int key = nyr_lower_expr(b, e->as.call.args.data[0].val);
        return set < 0 || key < 0 ? -1
                                  : nyr_emit_runtime_call(b, "rt_set_add_raw",
                                                          set, key, -1, 2, 0);
      }
      if (strcmp(canonical_leaf, "clear") == 0 && e->as.call.args.len == 0 &&
          nyr_expr_is_list(b, receiver)) {
        int list = nyr_lower_expr(b, receiver);
        return list < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_tbuf_clear_raw", list,
                                                -1, -1, 1, 0);
      }
      if ((strcmp(canonical_leaf, "delete") == 0 ||
           strcmp(canonical_leaf, "remove") == 0) &&
          e->as.call.args.len == 1 && nyr_expr_is_dict(b, receiver)) {
        int dict = nyr_lower_expr(b, receiver);
        int key = nyr_lower_dict_key(b, e->as.call.args.data[0].val);
        if (dict < 0 || key < 0)
          return -1;
        return nyr_emit_runtime_call(
            b,
            nyr_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_dict_delete_str_raw"
                : "rt_dict_delete_raw",
            dict, key, -1, 2, 0);
      }
      if (strcmp(canonical_leaf, "set") == 0 && e->as.call.args.len == 2 &&
          nyr_expr_is_dict(b, receiver)) {
        int dict = nyr_lower_expr(b, receiver);
        const expr_t *key_expr = e->as.call.args.data[0].val;
        const char *set_symbol = nyr_dict_set_symbol(b, key_expr);
        int key = strcmp(set_symbol, "rt_dict_set_i64_str_raw") == 0
                      ? nyr_lower_expr(b, key_expr->as.call.args.data[0].val)
                      : nyr_lower_dict_key(b, key_expr);
        int value = nyr_lower_expr(b, e->as.call.args.data[1].val);
        if (dict < 0 || key < 0 || value < 0)
          return -1;
        return nyr_emit_runtime_call(b, set_symbol, dict, key, value, 3, 0);
      }
      if ((strcmp(canonical_leaf, "get") == 0 ||
           strcmp(canonical_leaf, "dict_get") == 0) &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        bool receiver_is_dict = nyr_expr_is_dict(b, receiver);
        bool receiver_is_list =
            nyr_expr_is_list(b, receiver) || nyr_expr_is_dyn_list(b, receiver);
        if (!receiver_is_list && receiver && receiver->kind == NY_E_IDENT &&
            receiver->as.ident.name) {
          const nyr_local_t *receiver_local =
              nyr_find_local(b, receiver->as.ident.name);
          receiver_is_list = receiver_local && (receiver_local->is_list ||
                                                receiver_local->is_dyn_list);
        }
        const call_arg_t *key_arg = &e->as.call.args.data[0];
        const call_arg_t *fallback_arg =
            e->as.call.args.len == 2 ? &e->as.call.args.data[1] : NULL;
        for (size_t i = 0; i < e->as.call.args.len; ++i) {
          const call_arg_t *arg = &e->as.call.args.data[i];
          if (!arg->name)
            continue;
          if (strcmp(arg->name, "key") == 0)
            key_arg = arg;
          else if (strcmp(arg->name, "default") == 0)
            fallback_arg = arg;
        }
        if (!receiver_is_list && receiver && receiver->kind == NY_E_IDENT &&
            receiver->as.ident.name &&
            !nyr_find_local(b, receiver->as.ident.name)) {
          const expr_t *global =
              nyr_find_top_level_value(b, receiver->as.ident.name);
          receiver_is_list =
              global && global != receiver && nyr_expr_is_list(b, global);
        }
        if (receiver &&
            (receiver->kind == NY_E_CALL || receiver->kind == NY_E_MEMCALL))
          receiver_is_dict = false;
        int dict = nyr_lower_expr(b, receiver);
        int key = receiver_is_dict ? nyr_lower_dict_key(b, key_arg->val)
                                   : nyr_lower_expr(b, key_arg->val);
        if (!receiver_is_dict && !receiver_is_list &&
            !nyr_expr_is_any(b, key_arg->val) &&
            !nyr_expr_is_cstr(b, key_arg->val)) {
          int one = nyr_emit_const(b, 1);
          int shifted =
              one < 0 ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = key,
                                                          .b = one});
          key = shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
        }
        int fallback = fallback_arg ? nyr_lower_expr(b, fallback_arg->val)
                                    : nyr_emit_const(b, 0);
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        /* Dictionary lookup exposes an `any` result. Its missing-key value
         * must use the same dynamic representation as values read from a
         * native dictionary slot; native scalar expressions are otherwise
         * passed in their raw register form (e.g. raw 7 would be decoded as
         * tagged integer 3 by the caller). */
        const expr_t *fallback_expr = fallback_arg ? fallback_arg->val : NULL;
        bool fallback_is_int_literal =
            fallback_expr && fallback_expr->kind == NY_E_LITERAL &&
            fallback_expr->as.literal.kind == NY_LIT_INT &&
            fallback_expr->tok.kind != NY_T_NIL;
        nyr_scalar_class_t fallback_class =
            nyr_value_scalar_class(b, fallback);
        bool fallback_is_float = nyr_expr_is_f64(b, fallback_expr) ||
                                 nyr_expr_is_f32(b, fallback_expr) ||
                                 fallback_class == NYIR_SCALAR_F64 ||
                                 fallback_class == NYIR_SCALAR_F32;
        if ((receiver_is_dict || nyr_expr_is_any(b, receiver)) &&
            fallback_expr &&
            (!nyr_expr_is_any(b, fallback_expr) ||
             nyr_expr_has_raw_int_abi(b, fallback_expr, 0) ||
             fallback_is_int_literal || fallback_is_float)) {
          if (nyr_expr_is_bool(b, fallback_expr)) {
            fallback = nyr_box_bool(b, fallback);
          } else if (fallback_is_float) {
            int f64 = nyr_expr_is_f32(b, fallback_expr) ||
                              fallback_class == NYIR_SCALAR_F32
                          ? nyr_emit_f32_to_f64(b, fallback)
                          : fallback;
            int bits = f64 < 0 ? -1
                               : nyr_emit_runtime_call(b, "rt_f64_bits", f64,
                                                       -1, -1, 1, 0);
            fallback = bits < 0 ? -1
                                : nyr_emit_runtime_call(b, "rt_flt_box_val",
                                                        bits, -1, -1, 1, 0);
          } else if (nyr_expr_is_cstr(b, fallback_expr)) {
            fallback = nyr_emit_runtime_call(b, "rt_cstr_to_str", fallback, -1,
                                             -1, 1, 0);
          } else if (!nyr_expr_is_list(b, fallback_expr) &&
                     !nyr_expr_is_dict(b, fallback_expr) &&
                     !nyr_expr_is_bytes(b, fallback_expr) &&
                     !nyr_expr_is_tuple(b, fallback_expr) &&
                     !ny_expr_is_nil_literal(fallback_expr)) {
            fallback = nyr_box_raw_dynamic_value(b, fallback);
          }
        }
        if (fallback < 0)
          return -1;
        if (receiver_is_list) {
          const char *result_type = nyr_expr_type_name(b, e);
          bool dynamic_result =
              (result_type && ny_native_type_name_is_any(result_type)) ||
              (e->semantic.resolved &&
               e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC);
          bool dynamic_get =
              nyr_expr_is_dyn_list(b, receiver) || dynamic_result;
          if (dynamic_get) {
            /*
             * `get` takes a dynamic key, while rt_value_get_index_raw takes
             * an untagged integer index.  This specialization used to send
             * the source-level `any` key (or a raw integer register) to the
             * raw-index bridge, bypassing core.reflect.get's type check and
             * integer unboxing.  Keep the dynamic key contract here; the
             * generic accessor handles typed buffers and managed sequences.
             */
            nyr_scalar_class_t key_class = nyr_value_scalar_class(b, key);
            if (key_class == NYIR_SCALAR_I64_RAW) {
              key = nyr_box_raw_dynamic_value(b, key);
            } else if (key_class == NYIR_SCALAR_F32 ||
                       key_class == NYIR_SCALAR_F64) {
              int f64 = key_class == NYIR_SCALAR_F32
                            ? nyr_emit_f32_to_f64(b, key)
                            : key;
              int bits = f64 < 0 ? -1
                                 : nyr_emit_runtime_call(b, "rt_f64_bits", f64,
                                                         -1, -1, 1, 0);
              key = bits < 0
                        ? -1
                        : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                                -1, 1, NYIR_SCALAR_I64_TAGGED);
            }
            if (key < 0)
              return -1;
            if (!fallback_arg ||
                (fallback_arg->val && !nyr_expr_is_any(b, fallback_arg->val))) {
              if (fallback_arg && nyr_expr_is_bool(b, fallback_arg->val)) {
                fallback = nyr_box_bool(b, fallback);
              } else if (fallback_is_float) {
                int f64 = fallback_class == NYIR_SCALAR_F32 ||
                                  nyr_expr_is_f32(b, fallback_arg->val)
                              ? nyr_emit_f32_to_f64(b, fallback)
                              : fallback;
                int bits = f64 < 0 ? -1
                                   : nyr_emit_runtime_call(b, "rt_f64_bits",
                                                           f64, -1, -1, 1, 0);
                fallback =
                    bits < 0
                        ? -1
                        : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                                -1, 1, NYIR_SCALAR_I64_TAGGED);
              } else if (fallback_arg &&
                         nyr_expr_is_cstr(b, fallback_arg->val)) {
                fallback =
                    nyr_emit_runtime_call(b, "rt_cstr_to_str", fallback, -1, -1,
                                          1, NYIR_SCALAR_I64_TAGGED);
              } else if (!fallback_arg ||
                         (!nyr_expr_is_list(b, fallback_arg->val) &&
                          !nyr_expr_is_dict(b, fallback_arg->val) &&
                          !nyr_expr_is_bytes(b, fallback_arg->val) &&
                          !nyr_expr_is_tuple(b, fallback_arg->val) &&
                          !ny_expr_is_nil_literal(fallback_arg->val))) {
                fallback = nyr_box_raw_dynamic_value(b, fallback);
              }
              if (fallback < 0)
                return -1;
            }
            return nyr_emit_runtime_call(b, "rt_value_get_tagged", dict, key,
                                         fallback, 3, NYIR_SCALAR_I64_TAGGED);
          }
          if (!dynamic_get && fallback_arg &&
              (nyr_expr_is_f64(b, fallback_arg->val) ||
               nyr_expr_is_f32(b, fallback_arg->val))) {
            int as_f64 = nyr_expr_is_f32(b, fallback_arg->val)
                             ? nyr_emit_f32_to_f64(b, fallback)
                             : fallback;
            fallback = as_f64 < 0
                           ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64, -1,
                                                   -1, 1, NYIR_SCALAR_I64_RAW);
            if (fallback < 0)
              return -1;
          }
          /*
           * Dynamic get fallbacks have already crossed into the tagged ABI.
           * The raw tbuf helper cannot accept that fallback; the shared
           * index bridge normalizes tagged integers and preserves other
           * dynamic values before reading the runtime-selected layout.
           */
          return nyr_emit_runtime_call(b, "rt_tbuf_get", dict, key, fallback, 3,
                                       NYIR_SCALAR_UNKNOWN);
        }
        if (!receiver_is_dict) {
          int got = nyr_emit_runtime_call(b, "rt_value_get_tagged", dict, key,
                                          fallback, 3, NYIR_SCALAR_I64_TAGGED);
          if (got >= 0 && nyr_expr_is_cstr(b, e))
            return nyr_emit_runtime_call(b, "rt_any_to_cstr", got, -1, -1, 1,
                                         0);
          if (got >= 0 && b->f64_context_depth != 0)
            return nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                         NYIR_SCALAR_F64);
          /* An integer literal default types the expression as a raw scalar;
           * the canonical accessor returns the tagged word, so decode here
           * (v.get(0, 0) > 2 otherwise compares tagged 3 > 2 and every
           * nested list survives the filter). */
          if (!b->value_for_any_call && got >= 0 && e->as.call.args.len == 2 &&
              e->as.call.args.data[1].val &&
              e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
              e->as.call.args.data[1].val->as.literal.kind == NY_LIT_INT &&
              e->as.call.args.data[1].val->tok.kind != NY_T_NIL)
            return nyr_emit_runtime_call(b, "rt_any_to_i64", got, -1, -1, 1, 0);
          return got;
        }
        int got = nyr_emit_runtime_call(
            b, nyr_dict_get_symbol(b, e->as.call.args.data[0].val), dict, key,
            fallback, 3, NYIR_SCALAR_I64_TAGGED);
        if (got >= 0 && nyr_expr_is_cstr(b, e))
          return nyr_emit_runtime_call(b, "rt_any_to_cstr", got, -1, -1, 1, 0);
        return got >= 0 && b->f64_context_depth != 0
                   ? nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                           NYIR_SCALAR_F64)
                   : got;
      }
      call_arg_t args[NYIR_CALL_MAX_ARGS] = {{.val = receiver}};
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        args[i + 1] = e->as.call.args.data[i];
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = canonical;
      expr_t direct = {.kind = NY_E_CALL, .tok = e->tok};
      direct.semantic = e->semantic;
      direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
      direct.as.call.callee = &callee;
      direct.as.call.args.data = args;
      direct.as.call.args.len = direct.as.call.args.cap =
          e->as.call.args.len + 1;
      return nyr_lower_expr(b, &direct);
    }
  }
ordinary_call:
  if (!e->as.call.callee) {
    return nyr_fail(b, "native NYIR lower: only direct calls are supported"),
           -1;
  }
  const expr_call_t *c = &e->as.call;
  const stmt_t *direct_callee_fn = NULL;
  ny_native_lambda_entry_t *indirect_lambda = NULL;
  bool direct_zero_capture_lambda = false;
  const char *name = NULL;
  const expr_t *callee_expr = e->as.call.callee;
  /* Sequence.get returns `any`, so the type pass cannot mark its result as a
   * closure even when the receiver is a known list/tuple literal. Preserve
   * the indirect-call ABI for that direct accessor-call shape. */
  bool indirect_sequence_get =
      callee_expr->kind == NY_E_MEMCALL && callee_expr->as.memcall.name &&
      strcmp(callee_expr->as.memcall.name, "get") == 0 &&
      callee_expr->as.memcall.args.len >= 1 &&
      callee_expr->as.memcall.args.len <= 2 &&
      !callee_expr->as.memcall.args.data[0].name &&
      (callee_expr->as.memcall.args.len == 1 ||
       !callee_expr->as.memcall.args.data[1].name ||
       strcmp(callee_expr->as.memcall.args.data[1].name, "default") == 0) &&
      (nyr_expr_is_list(b, callee_expr->as.memcall.target) ||
       nyr_expr_is_tuple(b, callee_expr->as.memcall.target));
  bool indirect_sequence_index =
      callee_expr->kind == NY_E_INDEX && !callee_expr->as.index.stop &&
      !callee_expr->as.index.step && callee_expr->as.index.target &&
      (nyr_expr_is_list(b, callee_expr->as.index.target) ||
       nyr_expr_is_tuple(b, callee_expr->as.index.target));
  bool indirect_dictionary_get =
      callee_expr->kind == NY_E_MEMCALL && callee_expr->as.memcall.name &&
      strcmp(callee_expr->as.memcall.name, "get") == 0 &&
      callee_expr->as.memcall.args.len >= 1 &&
      callee_expr->as.memcall.args.len <= 2 &&
      !callee_expr->as.memcall.args.data[0].name &&
      (callee_expr->as.memcall.args.len == 1 ||
       !callee_expr->as.memcall.args.data[1].name ||
       strcmp(callee_expr->as.memcall.args.data[1].name, "default") == 0) &&
      (nyr_expr_is_dict(b, callee_expr->as.memcall.target) ||
       (callee_expr->as.memcall.target &&
        callee_expr->as.memcall.target->kind == NY_E_IDENT &&
        callee_expr->as.memcall.target->as.ident.name &&
        nyr_find_local(b, callee_expr->as.memcall.target->as.ident.name) &&
        nyr_find_local(b, callee_expr->as.memcall.target->as.ident.name)
            ->is_dict));
  if (e->as.call.callee->kind == NY_E_IDENT) {
    name = e->as.call.callee->as.ident.name;
  } else if (e->as.call.callee->kind == NY_E_FN ||
             e->as.call.callee->kind == NY_E_LAMBDA) {
    ny_native_lambda_entry_t *entry =
        ny_native_lambda_find(ny_native_lambda_cache_for(b), e->as.call.callee);
    if (!entry || !entry->fn || !entry->name) {
      return nyr_fail(b, "native NYIR lower: closure was not collected"), -1;
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
  } else if (e->as.call.callee->semantic.resolved &&
             e->as.call.callee->semantic.rep == NY_SEM_REP_CLOSURE) {
    /* A call expression can itself be the callee of another call
     * (`make_adder(10)(5)`).  It is a normal closure value, so let the
     * indirect-call ABI below lower the expression and invoke its result.
     * Rejecting every non-identifier here made higher-order calls depend on
     * whether the closure happened to be assigned to a local first. */
    /* Keep the surface name non-null for the generic intrinsic checks below;
     * they intentionally query it while deciding whether a builtin spelling
     * is shadowed by a user function.  The empty name still forces the
     * indirect-call branch because no direct function can resolve to it. */
    name = "";
  } else if (indirect_sequence_get) {
    name = "";
  } else if (indirect_sequence_index) {
    name = "";
  } else if (indirect_dictionary_get) {
    name = "";
  } else if (e->as.call.callee->kind == NY_E_MEMBER &&
             e->as.call.callee->as.member.name) {
    const char *receiver_type =
        nyr_expr_type_name(b, e->as.call.callee->as.member.target);
    return nyr_fail(b,
                    "unresolved member call '%s.%s': no callable "
                    "method was resolved for "
                    "the receiver type",
                    receiver_type ? receiver_type : "<unknown>",
                    e->as.call.callee->as.member.name),
           -1;
  } else {
    return nyr_fail(b, "native NYIR lower: only direct calls are supported"),
           -1;
  }
  /* HM records the exact declaration selected for an unqualified call.
   * Preserve that module identity here: leaf lookup is ambiguous when sibling
   * modules define the same generated helper. A runtime callable local still
   * takes precedence, as it does for ordinary source calls. */
  if (name && e->as.call.callee->kind == NY_E_IDENT &&
      !nyr_find_local(b, name) && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      e->semantic.canonical_callee &&
      !ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok))
    name = e->semantic.canonical_callee;
  /* A later same-file declaration cannot capture an earlier call. Preserve
   * the semantic import binding only for that forward-shadowing case; normal
   * stdlib resolution still goes through the reachable-body resolver. */
  if (name && e->as.call.callee->kind == NY_E_IDENT &&
      !nyr_find_local(b, name) && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      e->semantic.canonical_callee &&
      ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok)) {
    const stmt_t *same_name = nyr_find_user_function(b, name);
    if (same_name && same_name->tok.filename && e->tok.filename &&
        nyr_same_source_file(same_name->tok.filename, e->tok.filename) &&
        same_name->tok.line > e->tok.line)
      name = e->semantic.canonical_callee;
  }
  /* Canonicalize module-alias calls before any ABI or reachability decision.
   * Some expanded stdlib functions retain the spelling `core_ref.eq` even
   * though their `use ... as core_ref` declaration is owned by the module
   * currently being lowered.  Emitting that spelling would create an
   * unresolved ELF symbol instead of calling `core.reflect.eq`. */
  char canonical_name[512];
  if (name) {
    const char *name_dot = strchr(name, '.');
    if (name_dot && name_dot > name) {
      size_t alias_len = (size_t)(name_dot - name);
      if (alias_len < 256) {
        char alias[256];
        memcpy(alias, name, alias_len);
        alias[alias_len] = '\0';
        const char *module = nyr_resolve_use_alias(b, alias);
        if (!module) {
          /* Expanded stdlib nodes can carry the provider's source filename
           * while the alias declaration belongs to the importing module. */
          nyr_builder_t probe = *b;
          probe.source_file = NULL;
          probe.locals = NULL;
          probe.local_count = probe.local_cap = 0;
          module = nyr_resolve_use_alias(&probe, alias);
        }
        if (module) {
          /*
           * The spelling may already be fully qualified (`core.str.strip`
           * from an attached-method rewrite): prepending the alias target
           * again would double it (`core.str.str.strip`), a symbol no
           * definition site emits. Only qualify bare-alias spellings.
           */
          size_t mlen = strlen(module);
          if (strncmp(name, module, mlen) != 0 || name[mlen] != '.') {
            int n = snprintf(canonical_name, sizeof(canonical_name), "%s%s",
                             module, name_dot);
            if (n > 0 && (size_t)n < sizeof(canonical_name))
              name = canonical_name;
          }
        }
      }
    }
  }
  const char *leaf = ny_name_leaf(name);
  if (!leaf)
    leaf = "";
  ny_native_leaf_kind_t leaf_kind = ny_native_leaf_kind(leaf);
  const char *dot = name ? strrchr(name, '.') : NULL;

  if (canon_leaf && strcmp(canon_leaf, "sub") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    if (nyr_expr_is_list(b, target) && !nyr_expr_is_any(b, target)) {
      int set = nyr_lower_expr(b, target);
      int key = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (set < 0 || key < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_set_remove_raw", set, key, -1, 2, 0);
    }
  }

  /* `set(dict, key, value)` is the generic reflective setter.  Once the
   * receiver is known to be a native dictionary, keep the call on the same
   * dictionary bridge as `dict.set(...)`; the generic any adapter otherwise
   * loses scalar tags on integer/bool payloads. */
  if (name && leaf && strcmp(leaf, "set") == 0 && !ny_is_stdlib_tok(e->tok) &&
      e->as.call.args.len == 3 &&
      nyr_expr_is_dict(b, e->as.call.args.data[0].val)) {
    int dict = nyr_lower_expr(b, e->as.call.args.data[0].val);
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const char *set_symbol = nyr_dict_set_symbol(b, key_expr);
    int key = strcmp(set_symbol, "rt_dict_set_i64_str_raw") == 0
                  ? nyr_lower_expr(b, key_expr->as.call.args.data[0].val)
                  : nyr_lower_dict_key(b, key_expr);
    int value = nyr_lower_expr(b, e->as.call.args.data[2].val);
    const expr_t *value_expr = e->as.call.args.data[2].val;
    bool is_bool_val = nyr_expr_is_bool(b, value_expr);
    const nyr_local_t *value_local = NULL;
    if (value_expr && value_expr->kind == NY_E_IDENT &&
        value_expr->as.ident.name) {
      value_local = nyr_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_bool)
        is_bool_val = true;
    }
    if (is_bool_val) {
      value = nyr_box_bool(b, value);
    } else if ((value_expr && nyr_expr_is_f64(b, value_expr)) ||
               nyr_value_is_f64(b, value, 0)) {
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, 0);
    } else if (value_expr && value_expr->kind == NY_E_LITERAL &&
               value_expr->as.literal.kind == NY_LIT_INT &&
               value_expr->tok.kind != NY_T_NIL) {
      value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    } else if (value_local && value_local->semantic_rep == NY_SEM_REP_RAW_INT) {
      /* Generic reflective setters store dynamic values.  A typed native
       * integer local (notably a socket fd) is raw in NYIR and must be boxed
       * before it enters the dictionary; otherwise a later `.get` sees the
       * untagged word as a false/nil immediate and silently changes the fd.
       */
      value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    } else if (!is_bool_val && (!value_local || !value_local->is_any) &&
               value_expr &&
               ny_native_type_name_is_int(nyr_expr_type_name(b, value_expr))) {
      value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    }
    if (dict < 0 || key < 0 || value < 0)
      return -1;
    return nyr_emit_runtime_call(
        b,
        strcmp(set_symbol, "rt_dict_set_i64_str_raw") == 0
            ? set_symbol
            : (nyr_expr_is_cstr(b, key_expr) ? "rt_native_dict_set_str_compact"
                                             : "rt_native_dict_set_nir_i64"),
        dict, key, value, 3, 0);
  }

  /* A proof parameter may depend on a call-site value.  The unified native
   * path must not accept a witness proved for a different value, or inherit a
   * caller's unknown parameter range.  The HM proof shape is preserved on the
   * source function parameter, so reject the unsound cases before emitting a
   * direct call. */
  if (name && !dot && b->current_fn_name) {
    const stmt_t *callee_fn = nyr_find_user_function(b, name);
    const stmt_t *caller_fn = nyr_find_user_function(b, b->current_fn_name);
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
          bool holds = nyr_mapped_proof_holds(proof_type, mapped,
                                              actual->as.literal.as.i);
          if (!holds) {
            return nyr_fail(b, "mapped value ranges do not prove it"), -1;
          }
        }
      }
      for (size_t ci = 0; caller_fn && ci < caller_fn->as.fn.params.len; ++ci) {
        const char *unknown = caller_fn->as.fn.params.data[ci].name;
        if (!unknown || !ny_proof_type_names_ident(proof_type, unknown))
          continue;
        return nyr_fail(b, "mapped value ranges do not prove it"), -1;
      }
    }
  }
  /* Native NYIR keeps scalar integers unboxed, so raw integer zero is a
   * legitimate value and must not be confused with the nil immediate by the
   * generic `__is_nil` runtime bridge.  Resolve statically-known integers
   * before lowering the call; dynamic values still use the runtime predicate.
   */
  if (canon_leaf &&
      (strcmp(canon_leaf, "__is_nil") == 0 ||
       strcmp(canon_leaf, "is_nil") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (arg && arg->tok.kind == NY_T_NIL)
      return nyr_emit_const(b, NY_IMM_TRUE);
    if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name) {
      nyr_local_t *local = nyr_find_local(b, arg->as.ident.name);
      if (local && local->is_any && local->dyn_tag_slot >= 0) {
        int tag = nyr_load_local_value(b, local->dyn_tag_slot);
        int nil_tag = nyr_emit_const(b, 0);
        if (tag < 0 || nil_tag < 0)
          return -1;
        return nyr_emit_cmp_i64(b, NYIR_CMP_EQ, tag, nil_tag);
      }
    }
    int64_t known = 0;
    if (arg &&
        ((arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT) ||
         (arg->semantic.resolved && arg->semantic.rep == NY_SEM_REP_RAW_INT) ||
         nyr_eval_proof_i64(b, arg, 0, &known)))
      return nyr_emit_const(b, NY_IMM_FALSE);
    /* Nullable method/function results are already in the pointer/nil ABI;
     * avoid routing them through the generic any predicate, which can treat
     * a raw zero return as a tagged integer. */
    if (arg && (arg->kind == NY_E_CALL || arg->kind == NY_E_MEMCALL)) {
      int value = nyr_lower_expr(b, arg);
      int nilv = value < 0 ? -1 : nyr_emit_const(b, NY_IMM_NIL);
      return value < 0 || nilv < 0
                 ? -1
                 : nyr_emit_cmp_i64(b, NYIR_CMP_EQ, value, nilv);
    }
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__is_int") == 0 ||
       strcmp(canon_leaf, "is_int") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    nyr_local_t *arg_local = NULL;
    if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name)
      arg_local = nyr_find_local(b, arg->as.ident.name);
    if (arg && arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT)
      return nyr_emit_const(b, arg->tok.kind == NY_T_NIL ? 0 : 1);
    if (nyr_expr_has_raw_int_abi(b, arg, 0))
      return nyr_emit_const(b, 1);
    /* HM can leave a narrowed scalar fact on an explicitly dynamic local,
     * even though the slot still contains its tagged runtime carrier. Keep
     * the predicate dynamic for that slot so its actual tag is inspected. */
    if (arg && !(arg_local && arg_local->is_any) && arg->semantic.resolved &&
        arg->semantic.rep != NY_SEM_REP_TAGGED_DYNAMIC &&
        arg->semantic.rep != NY_SEM_REP_UNRESOLVED)
      return nyr_emit_const(b, arg->semantic.rep == NY_SEM_REP_RAW_INT ? 1 : 0);
    if (arg_local) {
      nyr_local_t *local = arg_local;
      /* A tbuf/any-producing expression can retain a companion tag slot even
       * when semantic inference narrowed the value local to an untyped
       * scalar. Prefer that authoritative tag over probing the value itself:
       * raw native odd integers and tagged Ny integers share the same bit
       * shape. */
      if (local && local->dyn_tag_slot >= 0) {
        int tag = nyr_load_local_value(b, local->dyn_tag_slot);
        int tag1 = nyr_emit_const(b, 1);
        int tag3 = nyr_emit_const(b, 3);
        if (tag < 0 || tag1 < 0 || tag3 < 0)
          return -1;
        int eq1 = nyr_emit_cmp_i64(b, NYIR_CMP_EQ, tag, tag1);
        int eq3 = nyr_emit_cmp_i64(b, NYIR_CMP_EQ, tag, tag3);
        if (eq1 < 0 || eq3 < 0)
          return -1;
        return nyr_emit_binop(b, NYIR_OR_I64, eq1, eq3);
      }
      if (local && !local->is_any &&
          local->semantic_rep != NY_SEM_REP_UNRESOLVED)
        return nyr_emit_const(b, local->semantic_rep == NY_SEM_REP_RAW_INT ? 1
                                                                           : 0);
    }
    int value = nyr_lower_expr(b, arg);
    return value < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_native_is_int", value, -1,
                                             -1, 1, 0);
  }

  /* Wide literals are materialized as BigInt handles by literal lowering.
   * Keep the source proof at this predicate boundary: local type inference
   * may still label their binding `int`, which would send the handle through
   * a tagged-value predicate and incorrectly report false. */
  if (canon_leaf &&
      (strcmp(canon_leaf, "__is_bigint") == 0 ||
       strcmp(canon_leaf, "is_bigint") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (nyr_expr_is_bigint(b, arg))
      return nyr_emit_const(b, 1);
    if (nyr_expr_has_raw_int_abi(b, arg, 0))
      return nyr_emit_const(b, 0);
  }

  /* Keep the native BigInt formatter at the representation boundary.  The
   * stdlib body re-enters `bigint()` and tag predicates, which is ambiguous
   * for lazily-materialized wide literals after they have passed through an
   * `any`-annotated local.  A known BigInt expression can use the runtime's
   * handle-aware C-string bridge directly. */
  if (leaf && strcmp(leaf, "bigint_to_str") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (nyr_expr_is_bigint(b, arg) ||
        (nyr_expr_type_name(b, arg) &&
         ny_type_is(nyr_expr_type_name(b, arg), "bigint"))) {
      int value = nyr_lower_expr(b, arg);
      return value < 0
                 ? -1
                 : nyr_emit_runtime_call(b, "rt_bigint_to_cstr_raw", value, -1,
                                         -1, 1, NYIR_SCALAR_STRING);
    }
  }

  /* Literal lowering already materializes wide integers as BigInt handles.
   * Bypass the stdlib `bigint(any)` wrapper for that proven source case: its
   * generic-any parameter can reclassify the handle after type/predicate
   * queries, then pass a corrupted value back out. */
  if (canon_leaf && strcmp(canon_leaf, "bigint") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    bool known_wide_literal = arg && arg->kind == NY_E_LITERAL &&
                              arg->as.literal.kind == NY_LIT_INT &&
                              nyr_expr_is_bigint(b, arg);
    if (!known_wide_literal && arg && arg->kind == NY_E_IDENT &&
        arg->as.ident.name) {
      const expr_t *global = nyr_find_top_level_value(b, arg->as.ident.name);
      known_wide_literal = global && global->kind == NY_E_LITERAL &&
                           global->as.literal.kind == NY_LIT_INT &&
                           nyr_expr_is_bigint(b, global);
    }
    if (known_wide_literal)
      return nyr_lower_expr(b, arg);
  }

  if (canon_leaf &&
      (strcmp(canon_leaf, "__is_ptr") == 0 ||
       strcmp(canon_leaf, "is_ptr") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg = e->as.call.args.data[0].val;
    if (arg && arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT)
      return nyr_emit_const(b, 0);
    int value = nyr_lower_expr(b, arg);
    return value < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_value_is_ptr", value, -1,
                                             -1, 1, 0);
  }

  /* Built-in container predicates must inspect both managed objects and the
   * compact native dict/tbuf layouts.  Lowering their stdlib wrappers through
   * the expanded `any` ABI loses the raw pointer payload (and makes a native
   * `{}` look like nil), so use the native tag bridge directly. */
  /* Use the canonical semantic callee for container predicates.  Aliases and
   * attached method sugar may retain a different surface leaf; falling back
   * to that spelling here routed equivalent calls through the generic any
   * ABI and lost native-buffer tags. */
  if (canon_leaf && e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      (strcmp(canon_leaf, "is_dict") == 0 ||
       strcmp(canon_leaf, "is_list") == 0 ||
       strcmp(canon_leaf, "is_tuple") == 0 ||
       strcmp(canon_leaf, "is_set") == 0 ||
       strcmp(canon_leaf, "is_range") == 0 ||
       strcmp(canon_leaf, "is_bytes") == 0)) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int tag =
        nyr_emit_const(b, strcmp(canon_leaf, "is_dict") == 0    ? TAG_DICT
                          : strcmp(canon_leaf, "is_list") == 0  ? TAG_LIST
                          : strcmp(canon_leaf, "is_tuple") == 0 ? TAG_TUPLE
                          : strcmp(canon_leaf, "is_set") == 0   ? TAG_SET
                          : strcmp(canon_leaf, "is_range") == 0 ? TAG_RANGE
                                                                : TAG_BYTES);
    int result = value < 0 || tag < 0
                     ? -1
                     : nyr_emit_runtime_call(b, "rt_native_has_tag", value, tag,
                                             -1, 2, 0);
    return result;
  }

  /* Proof builtins are compile-time operations. Keeping them in the generic
   * call path makes the NYIR->LLVM backend look for runtime bodies such as
   * `assert_compile_range`, which do not exist by design. Resolve the exact
   * forms here and leave genuinely dynamic obligations as compile-time no-ops
   * (the typing/proof pipeline remains the authority for flow-sensitive
   * checks). */
  if (!nyr_user_defined_fn(b, name) &&
      ny_native_leaf_kind(canon_leaf) == NY_NATIVE_LEAF_RANGE_PROVEN) {
    bool assertion = strcmp(canon_leaf, "assert_compile_range") == 0;
    if (e->as.call.args.len < 3 || (assertion && e->as.call.args.len > 4) ||
        (!assertion && e->as.call.args.len != 3)) {
      return nyr_fail(b, "native NYIR: %s expects value, min, max%s",
                      canon_leaf, assertion ? ", and optional message" : ""),
             -1;
    }
    int64_t value = 0, lo = 0, hi = 0;
    bool exact =
        nyr_eval_proof_i64(b, e->as.call.args.data[0].val, 0, &value) &&
        nyr_eval_proof_i64(b, e->as.call.args.data[1].val, 0, &lo) &&
        nyr_eval_proof_i64(b, e->as.call.args.data[2].val, 0, &hi);
    bool proven = exact && lo <= hi && value >= lo && value <= hi;
    if (assertion && exact && !proven) {
      const expr_t *msg =
          e->as.call.args.len == 4 ? e->as.call.args.data[3].val : NULL;
      if (msg && msg->kind == NY_E_LITERAL &&
          msg->as.literal.kind == NY_LIT_STR) {
        return nyr_fail(b, "%s", msg->as.literal.as.s.data), -1;
      }
      return nyr_fail(b,
                      "compile-time range assertion failed (value=%lld, "
                      "required=[%lld,%lld])",
                      (long long)value, (long long)lo, (long long)hi),
             -1;
    }
    return nyr_emit_const(b, assertion ? 0
                                       : (proven ? NY_IMM_TRUE : NY_IMM_FALSE));
  }
  if (!nyr_user_defined_fn(b, name) &&
      ny_native_leaf_kind(canon_leaf) == NY_NATIVE_LEAF_INDEX_PROVEN) {
    bool assertion = strcmp(canon_leaf, "assert_compile_index") == 0;
    if (e->as.call.args.len < 2 || (assertion && e->as.call.args.len > 3) ||
        (!assertion && e->as.call.args.len != 2)) {
      return nyr_fail(b, "native NYIR: %s expects container, index%s",
                      canon_leaf, assertion ? ", and optional message" : ""),
             -1;
    }
    int64_t index = 0;
    int64_t length = -1;
    bool exact_index =
        nyr_eval_proof_i64(b, e->as.call.args.data[1].val, 0, &index);
    const expr_t *container = e->as.call.args.data[0].val;
    if (container &&
        (container->kind == NY_E_LIST || container->kind == NY_E_TUPLE) &&
        nyr_size_fits_i64(container->as.list_like.len))
      length = (int64_t)container->as.list_like.len;
    else if (container && container->kind == NY_E_IDENT &&
             container->as.ident.name) {
      const expr_t *init =
          nyr_find_top_level_value(b, container->as.ident.name);
      if (init && init != container &&
          (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE) &&
          nyr_size_fits_i64(init->as.list_like.len))
        length = (int64_t)init->as.list_like.len;
    }
    bool proven = exact_index && length > 0 && index >= 0 && index < length;
    if (assertion && exact_index && length >= 0 && !proven) {
      const expr_t *msg =
          e->as.call.args.len == 3 ? e->as.call.args.data[2].val : NULL;
      if (msg && msg->kind == NY_E_LITERAL &&
          msg->as.literal.kind == NY_LIT_STR) {
        return nyr_fail(b, "%s", msg->as.literal.as.s.data), -1;
      }
      return nyr_fail(b,
                      "compile-time index assertion failed (index=%lld, "
                      "length=%lld)",
                      (long long)index, (long long)length),
             -1;
    }
    return nyr_emit_const(b, assertion ? 0
                                       : (proven ? NY_IMM_TRUE : NY_IMM_FALSE));
  }

  if (canon_leaf &&
      (strcmp(canon_leaf, "repr") == 0 || strcmp(canon_leaf, "to_str") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_list(b, e->as.call.args.data[0].val)) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_tbuf_to_cstr", value, -1,
                                             -1, 1, 0);
  }

  /* The public free `get` helper is implemented through reflect for the VM,
   * but known native containers already have raw, three-argument accessors.
   * Avoid sending a raw list through reflect's expanded `any` ABI. */
  if (canon_leaf &&
      (strcmp(canon_leaf, "get") == 0 || strcmp(canon_leaf, "dict_get") == 0) &&
      !nyr_user_defined_fn(b, name) &&
      (e->as.call.args.len == 2 || e->as.call.args.len == 3) &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      (e->as.call.args.len == 2 || !e->as.call.args.data[2].name)) {
    const expr_t *target = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const expr_t *decimal_key_expr = NULL;
    if (key_expr && key_expr->kind == NY_E_CALL &&
        key_expr->as.call.args.len == 1 &&
        !key_expr->as.call.args.data[0].name && nyr_early_leaf(key_expr) &&
        strcmp(nyr_early_leaf(key_expr), "to_str") == 0 &&
        !nyr_expr_is_any(b, key_expr->as.call.args.data[0].val) &&
        nyr_expr_has_raw_int_abi(b, key_expr->as.call.args.data[0].val, 0) &&
        !nyr_expr_is_bigint(b, key_expr->as.call.args.data[0].val)) {
      const expr_t *inner_callee = key_expr->as.call.callee;
      const char *inner_name = inner_callee && inner_callee->kind == NY_E_IDENT
                                   ? inner_callee->as.ident.name
                                   : NULL;
      if (inner_name && strcmp(inner_name, "to_str") == 0 &&
          !nyr_user_defined_fn(b, inner_name))
        decimal_key_expr = key_expr->as.call.args.data[0].val;
    }
    int target_v = nyr_lower_expr(b, target);
    int key_v =
        nyr_lower_expr(b, decimal_key_expr ? decimal_key_expr : key_expr);
    bool dynamic_target =
        !nyr_expr_is_bytes(b, target) && !nyr_expr_is_range(b, target) &&
        !nyr_expr_is_list(b, target) && !nyr_expr_is_dyn_list(b, target) &&
        !nyr_expr_is_dict(b, target);
    bool key_is_lit_int = key_expr && key_expr->kind == NY_E_LITERAL &&
                          key_expr->as.literal.kind == NY_LIT_INT &&
                          key_expr->tok.kind != NY_T_NIL;
    bool key_is_raw_int =
        key_is_lit_int ||
        (key_expr && key_expr->semantic.rep == NY_SEM_REP_RAW_INT) ||
        nyr_expr_is_integer_index(b, key_expr, 0);
    const char *key_type = nyr_expr_type_name(b, key_expr);
    key_is_raw_int =
        key_is_raw_int || (key_type && strcmp(key_type, "int") == 0);
    if (!key_is_raw_int && key_expr && key_expr->kind == NY_E_IDENT) {
      nyr_local_t *kl = nyr_find_local(b, key_expr->as.ident.name);
      if (kl && kl->semantic_rep == NY_SEM_REP_RAW_INT)
        key_is_raw_int = true;
    }
    bool dynamic_access = dynamic_target || nyr_expr_is_dict(b, target);
    /* The generic get builtin has an `any` result contract even when this
     * call site can prove the receiver is a scalar list. Its raw literal
     * fallback must therefore be boxed before the runtime accessor returns
     * it through that dynamic ABI. */
    bool dynamic_value_fallback = true;
    if (!decimal_key_expr && dynamic_access && key_is_raw_int &&
        !nyr_expr_is_any(b, key_expr) && !nyr_expr_is_cstr(b, key_expr)) {
      key_v = nyr_emit_runtime_call(b, "rt_box_i64", key_v, -1, -1, 1, 0);
    }
    int fallback = e->as.call.args.len == 3
                       ? nyr_lower_expr(b, e->as.call.args.data[2].val)
                       : nyr_emit_const(b, 0);
    const expr_t *fallback_expr =
        e->as.call.args.len == 3 ? e->as.call.args.data[2].val : NULL;
    const nyr_local_t *fallback_local =
        fallback_expr && fallback_expr->kind == NY_E_IDENT &&
                fallback_expr->as.ident.name
            ? nyr_find_local_nearest(b, fallback_expr->as.ident.name)
            : NULL;
    bool fallback_is_closure =
        fallback_expr &&
        (fallback_expr->kind == NY_E_FN || fallback_expr->kind == NY_E_LAMBDA ||
         fallback_expr->semantic.rep == NY_SEM_REP_CLOSURE ||
         (fallback_expr->kind == NY_E_IDENT && fallback_expr->as.ident.name &&
          ((!fallback_local &&
            nyr_find_user_function(b, fallback_expr->as.ident.name)) ||
           (fallback_local &&
            fallback_local->semantic_rep == NY_SEM_REP_CLOSURE))));
    if (dynamic_value_fallback && fallback_is_closure) {
      /* Preserve callable identity across rt_tbuf_get_any's dynamic fallback
       * boxing. The later indirect-call marker is idempotent. */
      fallback = nyr_emit_runtime_call(
          b, "rt_mark_dynamic_callable_tagged_args", fallback, -1, -1, 1, 0);
      if (fallback < 0)
        return -1;
    }
    if (dynamic_value_fallback && e->as.call.args.len == 3 &&
        !nyr_expr_is_any(b, e->as.call.args.data[2].val) &&
        !nyr_expr_is_cstr(b, e->as.call.args.data[2].val) &&
        !nyr_expr_is_list(b, e->as.call.args.data[2].val) &&
        !nyr_expr_is_dict(b, e->as.call.args.data[2].val) &&
        !nyr_expr_is_bool(b, e->as.call.args.data[2].val) &&
        e->as.call.args.data[2].val->kind != NY_E_FN &&
        e->as.call.args.data[2].val->kind != NY_E_LAMBDA &&
        e->as.call.args.data[2].val->semantic.rep != NY_SEM_REP_CLOSURE &&
        /* Nil is already the canonical dynamic word (NY_IMM_NIL 0).
         * rt_tag(0) re-encodes it as tagged integer zero, so a missed
         * lookup returned an int instead of the caller's nil
         * (dict_get(TYPE_GROUPS, name, nil) -> 0 broke every group
         * check in core.compiler.syntax.type). */
        !ny_expr_is_nil_literal(e->as.call.args.data[2].val)) {
      fallback = nyr_emit_runtime_call(b, "rt_tag", fallback, -1, -1, 1, 0);
      if (fallback < 0)
        return -1;
    }
    if (dynamic_value_fallback && e->as.call.args.len == 3 &&
        nyr_expr_is_bool(b, e->as.call.args.data[2].val)) {
      fallback = nyr_box_bool(b, fallback);
      if (fallback < 0)
        return -1;
    }
    if (target_v < 0 || key_v < 0 || fallback < 0)
      return -1;
    if (decimal_key_expr && nyr_expr_is_dict(b, target))
      return nyr_emit_runtime_call(b, "rt_dict_get_i64_str_raw", target_v,
                                   key_v, fallback, 3, NYIR_SCALAR_I64_TAGGED);
    if (nyr_expr_is_bytes(b, target))
      return nyr_emit_runtime_call(b, "rt_bytes_get_raw", target_v, key_v,
                                   fallback, 3, 0);
    if (nyr_expr_is_cstr(b, target)) {
      /*
       * String `.get` shares the raw-index sequence ABI.  The generic tagged
       * accessor returns the C-string byte pointer for a character, while
       * language strings are dynamic string values; use the raw-index bridge
       * to normalize negative indices and box the character consistently.
       */
      nyr_scalar_class_t key_class = nyr_value_scalar_class(b, key_v);
      if (key_class == NYIR_SCALAR_I64_TAGGED) {
        key_v = nyr_emit_runtime_call(b, "rt_any_to_i64", key_v, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
        if (key_v < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_value_get_index_raw", target_v, key_v,
                                   fallback, 3, NYIR_SCALAR_I64_TAGGED);
    }
    if (nyr_expr_is_range(b, target))
      return nyr_emit_runtime_call(b, "rt_value_get_tagged", target_v, key_v,
                                   fallback, 3, NYIR_SCALAR_I64_TAGGED);
    if (nyr_expr_is_dict(b, target) && !nyr_expr_is_any(b, key_expr)) {
      const char *dict_get = nyr_dict_get_symbol(b, key_expr);
      if (strcmp(dict_get, "rt_dict_get_str_raw") == 0)
        return nyr_emit_runtime_call(b, dict_get, target_v, key_v, fallback, 3,
                                     NYIR_SCALAR_I64_TAGGED);
    }
    if ((target->kind == NY_E_IDENT || target->kind == NY_E_LIST ||
         target->kind == NY_E_TUPLE) &&
        (nyr_expr_is_list(b, target) || nyr_expr_is_dyn_list(b, target))) {
      /* Native sequence buffers use a raw-index bridge. Keep integer keys
       * unboxed for that ABI; tagged and floating keys are normalized below. */
      nyr_scalar_class_t key_class = nyr_value_scalar_class(b, key_v);
      if (key_class == NYIR_SCALAR_F32 || key_class == NYIR_SCALAR_F64) {
        int f64 = key_class == NYIR_SCALAR_F32 ? nyr_emit_f32_to_f64(b, key_v)
                                               : key_v;
        int bits = f64 < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", f64, -1,
                                                   -1, 1, 0);
        key_v = bits < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                                 -1, 1, NYIR_SCALAR_I64_TAGGED);
      }
      if (key_v < 0)
        return -1;
      key_class = nyr_value_scalar_class(b, key_v);
      if (key_class == NYIR_SCALAR_I64_RAW) {
        int got =
            nyr_emit_runtime_call(b, "rt_value_get_index_raw", target_v, key_v,
                                  fallback, 3, NYIR_SCALAR_I64_TAGGED);
        if (got >= 0 && nyr_expr_is_f64(b, e)) {
          return nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                       NYIR_SCALAR_F64);
        }
        return got;
      }
      if (key_class == NYIR_SCALAR_I64_TAGGED) {
        key_v = nyr_emit_runtime_call(b, "rt_any_to_i64", key_v, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
        if (key_v < 0)
          return -1;
        int got =
            nyr_emit_runtime_call(b, "rt_value_get_index_raw", target_v, key_v,
                                  fallback, 3, NYIR_SCALAR_I64_TAGGED);
        if (got >= 0 && nyr_expr_is_f64(b, e)) {
          return nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                       NYIR_SCALAR_F64);
        }
        return got;
      }
      int got = nyr_emit_runtime_call(b, "rt_value_get_tagged", target_v, key_v,
                                      fallback, 3, NYIR_SCALAR_I64_TAGGED);
      if (got >= 0 && nyr_expr_is_f64(b, e)) {
        return nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                     NYIR_SCALAR_F64);
      }
      return got;
    }
    /* Dynamic results (for example `dict.get("items").get(0)`) are
     * intentionally dispatched by the runtime instead of being guessed as
     * dictionaries or scalar lists at compile time. */
    const char *get_runtime = "rt_value_get_tagged";
    int got = nyr_emit_runtime_call(b, get_runtime, target_v, key_v, fallback,
                                    3, NYIR_SCALAR_I64_TAGGED);
    if (got >= 0 && nyr_expr_is_cstr(b, e))
      return nyr_emit_runtime_call(b, "rt_any_to_cstr", got, -1, -1, 1, 0);
    if (got >= 0 && b->f64_context_depth != 0)
      return nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    if (got >= 0 && nyr_expr_is_f64(b, e)) {
      return nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    }
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
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int size = nyr_emit_const(b, 16);
    int base =
        size < 0 ? -1 : nyr_emit_runtime_call(b, "malloc", size, -1, -1, 1, 0);
    if (base < 0)
      return -1;
    for (size_t i = 0; i < 2; ++i) {
      int value = nyr_lower_expr(b, e->as.call.args.data[i].val);
      int offset = nyr_emit_const(b, rt_tag_v((int64_t)(i * 8)));
      if (value < 0 || offset < 0 ||
          nyr_emit_runtime_call(b, "rt_store64_idx", base, offset, value, 3,
                                0) < 0)
        return -1;
    }
    return base;
  }
  if (c_layout && e->as.call.args.len == c_layout->field_count &&
      c_layout->field_count > 0) {
    if (!nyr_size_fits_i64(c_layout->size)) {
      return nyr_fail(b, "native NYIR lower: C aggregate size overflows i64"),
             -1;
    }
    int size = nyr_emit_const(b, (int64_t)c_layout->size);
    int base =
        size < 0 ? -1 : nyr_emit_runtime_call(b, "malloc", size, -1, -1, 1, 0);
    if (base < 0)
      return -1;
    for (size_t i = 0; i < c_layout->field_count; ++i) {
      const ny_native_c_layout_field_t *field = &c_layout->fields[i];
      if (e->as.call.args.data[i].name || field->size == 0 || field->size > 8) {
        return nyr_fail(b, "native NYIR lower: unsupported C "
                           "aggregate constructor field"),
               -1;
      }
      int value = nyr_lower_expr(b, e->as.call.args.data[i].val);
      if (!nyr_size_fits_tagged_i64(field->offset)) {
        return nyr_fail(b, "native NYIR lower: C aggregate field "
                           "offset overflows tagged i64"),
               -1;
      }
      int offset = nyr_emit_const(b, rt_tag_v((int64_t)field->offset));
      if (value < 0 || offset < 0)
        return -1;
      /* The indexed runtime stores consume VM-tagged scalar values. Native
       * NYIR integers are raw, so tag narrow C scalar fields before crossing
       * this ABI boundary; pointer-sized fields remain raw handles. */
      if (field->size <= 4) {
        int one = nyr_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = value,
                                                        .b = one});
        int tagged = shifted < 0
                         ? -1
                         : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_OR_I64,
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
      if (nyr_emit_runtime_call(b, store, base, offset, value, 3, 0) < 0)
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
  if (nyr_find_enum_member(b, name, &adt_enum, &adt_item, &adt_tag) &&
      adt_item && adt_item->fields.len > 0 && !nyr_user_defined_fn(b, name)) {
    (void)adt_enum;
    size_t field_count = adt_item->fields.len;
    if (e->as.call.args.len != field_count) {
      bool positional = true;
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        positional = positional && !e->as.call.args.data[i].name;
      if (e->as.call.args.len > field_count && positional) {
        nyr_fail(b, "too many positional fields for ADT variant '%s'",
                 name ? name : "");
      } else if (e->as.call.args.len < field_count) {
        const char *missing = adt_item->fields.data[e->as.call.args.len].name;
        nyr_fail(b, "missing field '%s' for ADT variant '%s'",
                 missing ? missing : "", name ? name : "");
      } else {
        nyr_fail(b,
                 "native NYIR lower: ADT constructor '%s' expects %zu fields",
                 name ? name : "", field_count);
      }
      return -1;
    }
    /* The runtime helper accepts the ordinary tagged scalar ABI.  Encode
     * both metadata arguments explicitly; raw odd values are otherwise
     * indistinguishable from tagged integers (notably one-field variants and
     * odd variant tags). */
    if (!nyr_size_fits_tagged_i64(field_count)) {
      return nyr_fail(
                 b, "native NYIR lower: ADT field count overflows tagged i64"),
             -1;
    }
    int nfields = nyr_emit_const(b, rt_tag_v((int64_t)field_count));
    int tag = nyr_emit_const(b, rt_tag_v(adt_tag));
    int object =
        nfields < 0 || tag < 0
            ? -1
            : nyr_emit_runtime_call(b, "rt_adt_alloc", nfields, tag, -1, 2, 0);
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
          return nyr_fail(b, "unknown field '%s' for ADT variant '%s'",
                          arg->name ? arg->name : "", name ? name : ""),
                 -1;
        }
      }
      int value = nyr_lower_expr(b, arg->val);
      int offset = nyr_emit_const(b, (int64_t)(i * 8));
      int address = value < 0 || offset < 0
                        ? -1
                        : nyr_emit_binop(b, NYIR_ADD_I64, object, offset);
      size_t before_store = b->nyir.len;
      (void)nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                              .a = address,
                                              .c = value,
                                              .dst = -1});
      if (address < 0 || b->nyir.len == before_store) {
        return nyr_fail(
                   b, "native NYIR lower: ADT constructor field store failed"),
               -1;
      }
    }
    return object;
  }
  /* Comptime templates may interpolate a quoted tag argument as an
   * identifier (`runtime_tag_raw(${tag})`).  In the interpreter that value is
   * still a string; treating it as an ordinary native identifier creates a
   * bogus relocation such as `sym set`.  Fold the closed built-in tag set at
   * the call boundary, just as we already fold literal tag calls. */
  const stmt_t *tag_callee = e->semantic.canonical_callee_stmt;
  const stmt_t *tag_fn = name ? nyr_find_user_function(b, name) : NULL;
  bool tag_builtin = (!tag_fn || ny_is_stdlib_tok(tag_fn->tok)) ||
                     (tag_callee && tag_callee->kind == NY_S_FUNC &&
                      ny_is_stdlib_tok(tag_callee->tok));
  if (canon_leaf &&
      (strcmp(canon_leaf, "runtime_tag_raw") == 0 ||
       strcmp(canon_leaf, "__runtime_tag") == 0) &&
      tag_builtin && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name && e->as.call.args.data[0].val &&
      ((e->as.call.args.data[0].val->kind == NY_E_IDENT &&
        e->as.call.args.data[0].val->as.ident.name) ||
       (e->as.call.args.data[0].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[0].val->as.literal.kind == NY_LIT_STR))) {
    const expr_t *tag_expr = e->as.call.args.data[0].val;
    const char *tag_name = tag_expr->kind == NY_E_IDENT
                               ? tag_expr->as.ident.name
                               : tag_expr->as.literal.as.s.data;
    size_t tag_len = tag_expr->kind == NY_E_IDENT
                         ? strlen(tag_name)
                         : tag_expr->as.literal.as.s.len;
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
      int64_t raw = rt_runtime_tag_raw_name(tag_name, tag_len);
      return nyr_emit_const(b, raw);
    }
  }
  if (canon_leaf && strcmp(canon_leaf, "__flt_unbox_val") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_f64(b, e->as.call.args.data[0].val)) {
    int value = nyr_lower_f64_arg(b, e->as.call.args.data[0].val);
    return value < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__complex_new_bits") == 0 ||
       strcmp(canon_leaf, "__complex_new") == 0) &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    int re = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int im = nyr_lower_expr(b, e->as.call.args.data[1].val);
    int out = re < 0 || im < 0
                  ? -1
                  : nyr_emit_runtime_call(
                        b,
                        strcmp(canon_leaf, "__complex_new_bits") == 0
                            ? "rt_complex_new_bits"
                            : "rt_complex_new",
                        re, im, -1, 2, 0);
    int tag = out < 0 ? -1 : nyr_emit_const(b, TAG_COMPLEX);
    return out < 0 || tag < 0 ||
                   !nyr_record_dyn_fact(b, out, NYR_FACT_DYN_TAG, tag)
               ? -1
               : out;
  }
  if (canon_leaf && strcmp(canon_leaf, "type") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = nyr_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    bool known_bigint = nyr_expr_is_bigint(b, arg_expr);
    const nyr_local_t *type_local =
        arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
            ? nyr_find_local(b, arg_expr->as.ident.name)
            : NULL;
    const expr_t *type_global =
        arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
            ? nyr_find_top_level_value(b, arg_expr->as.ident.name)
            : NULL;
    bool root_global_bigint =
        type_local && type_local->syntax_ctx == 0 &&
        (type_local->is_any ||
         ny_native_type_name_is_any(type_local->type_name)) &&
        type_global && type_global != arg_expr &&
        type_global->kind != NY_E_IDENT && nyr_expr_is_bigint(b, type_global);
    if (ny_trace_enabled("NY_TRACE_ABI"))
      ny_trace_line(
          "TYPE",
          "function=%s value=v%d expr-kind=%d name=%s bigint=%s "
          "root-global-bigint=%s raw-sem=%s raw-abi=%s local=%s/%s/ctx%u "
          "global=%d/%s",
          b->current_fn_name ? b->current_fn_name : "<entry>", value,
          arg_expr ? (int)arg_expr->kind : -1,
          arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
              ? arg_expr->as.ident.name
              : "-",
          known_bigint ? "yes" : "no", root_global_bigint ? "yes" : "no",
          arg_expr && arg_expr->semantic.resolved &&
                  arg_expr->semantic.rep == NY_SEM_REP_RAW_INT
              ? "yes"
              : "no",
          nyr_expr_has_raw_int_abi(b, arg_expr, 0) ? "yes" : "no",
          type_local ? (type_local->is_bigint ? "bigint" : "other") : "-",
          type_local && type_local->type_name ? type_local->type_name : "-",
          type_local ? type_local->syntax_ctx : 0,
          type_global ? (int)type_global->kind : -1,
          type_global && nyr_expr_is_bigint(b, type_global) ? "bigint"
                                                            : "other");
    bool known_math_f64 = arg_expr && arg_expr->kind == NY_E_CALL &&
                          ny_native_call_leaf(arg_expr) &&
                          (strcmp(ny_native_call_leaf(arg_expr), "sin") == 0 ||
                           strcmp(ny_native_call_leaf(arg_expr), "cos") == 0 ||
                           strcmp(ny_native_call_leaf(arg_expr), "sqrt") == 0);
    if (nyr_expr_is_bool(b, arg_expr))
      return nyr_emit_cstr_const(b, "bool");
    /* Heap-backed BigInt values may have odd handles and some source
     * metadata still describes their originating literal as an int. Their
     * producer/local ABI fact must outrank that stale raw-int semantic. */
    if (known_bigint || root_global_bigint)
      return nyr_emit_cstr_const(b, "bigint");
    /* `type` is representation aware in native lowering. Integer arithmetic
     * intentionally produces a raw i64, so sending that word to the dynamic
     * rt_type_name bridge reports `unknown`. The producer fact (or the
     * resolved scalar ABI carried by its local/declaration) proves the
     * language-level result is an int; return the corresponding type name
     * directly without boxing and immediately decoding it again. Keep this
     * after the boolean case because predicates also use raw i64 registers.
     */
    if ((arg_expr && arg_expr->semantic.resolved &&
         arg_expr->semantic.rep == NY_SEM_REP_RAW_INT) ||
        (arg_expr && arg_expr->kind == NY_E_BINARY &&
         b->last_arith_expr == arg_expr && b->last_arith_raw) ||
        nyr_expr_has_raw_int_abi(b, arg_expr, 0))
      return nyr_emit_cstr_const(b, "int");
    /*
     * Ranges use the generic object representation in some top-level
     * bindings, which also makes the conservative dict predicate true.
     * Their concrete producer is still known, so check it before the broad
     * container predicates below.
     */
    if (nyr_expr_is_range(b, arg_expr))
      return nyr_emit_cstr_const(b, "range");
    if (nyr_expr_is_bytes(b, arg_expr))
      return nyr_emit_cstr_const(b, "bytes");
    if (nyr_expr_is_set(b, arg_expr))
      return nyr_emit_cstr_const(b, "set");
    if (nyr_expr_is_tuple(b, arg_expr))
      return nyr_emit_cstr_const(b, "tuple");
    if (nyr_expr_is_list(b, arg_expr))
      return nyr_emit_cstr_const(b, "list");
    if (nyr_expr_is_dict(b, arg_expr))
      return nyr_emit_cstr_const(b, "dict");
    if (known_math_f64 || nyr_expr_is_f64(b, arg_expr) ||
        nyr_expr_is_f32(b, arg_expr))
      return nyr_emit_cstr_const(b, "float");
    if (nyr_expr_is_cstr(b, arg_expr))
      return nyr_emit_cstr_const(b, "str");
    if (arg_expr && arg_expr->semantic.resolved &&
        arg_expr->semantic.rep == NY_SEM_REP_CLOSURE)
      return nyr_emit_cstr_const(b, "ptr");
    if (arg_expr && arg_expr->kind == NY_E_IDENT) {
      nyr_local_t *local = nyr_find_local(b, arg_expr->as.ident.name);
      if (local && (local->callable_expr || local->callable_name))
        return nyr_emit_cstr_const(b, "ptr");
      if (local && local->dyn_tag_slot >= 0) {
        int tag = nyr_load_local_value(b, local->dyn_tag_slot);
        if (tag >= 0)
          return nyr_emit_runtime_call(b, "rt_type_name_tagged", value, tag, -1,
                                       2, 0);
      }
    }
    return nyr_emit_runtime_call(b, "rt_type_name", value, -1, -1, 1, 0);
  }
  /* Ownership wrappers are semantic no-ops at the native ABI boundary. */
  if (canon_leaf && strcmp(canon_leaf, "own") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name)
    return nyr_lower_expr(b, e->as.call.args.data[0].val);
  if (canon_leaf && strcmp(canon_leaf, "forget") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    if (nyr_lower_expr(b, e->as.call.args.data[0].val) < 0)
      return -1;
    return nyr_emit_const(b, 1);
  }
  /* Native NYIR is the typed/raw ABI: from_int is an interpreter boxing
   * boundary, so preserve the raw integer when it is used by native code. */
  if (canon_leaf && strcmp(canon_leaf, "from_int") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (value >= 0 &&
        nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED)
      return nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1,
                                   NYIR_SCALAR_I64_RAW);
    return value;
  }
  if (canon_leaf && strcmp(canon_leaf, "atoi") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_atoi", value, -1, -1, 1, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "atof") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return value < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_atof", value, -1, -1, 1,
                                             NYIR_SCALAR_F64);
  }
  /* C `getenv` returns a borrowed C string.  Materialize it through the
   * runtime's native bridge so the result is a managed Nytrix string and can
   * safely cross the typed/dynamic boundary. */
  if (canon_leaf && strcmp(canon_leaf, "getenv") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int key = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return key < 0 ? -1
                   : nyr_emit_runtime_call(b, "rt_env_get", key, -1, -1, 1, 0);
  }
  /* The portable runtime wrappers return VM-tagged integers.  Native NYIR
   * keeps scalar integers raw, so unwrap these zero-argument POSIX queries at
   * the boundary instead of linking an ABI-incompatible libc declaration. */
  if (canon_leaf && !nyr_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "getpid") == 0 ||
       strcmp(canon_leaf, "getppid") == 0 ||
       strcmp(canon_leaf, "getuid") == 0 ||
       strcmp(canon_leaf, "getgid") == 0) &&
      e->as.call.args.len == 0) {
    const char *runtime = strcmp(canon_leaf, "getpid") == 0    ? "rt_getpid"
                          : strcmp(canon_leaf, "getppid") == 0 ? "rt_getppid"
                          : strcmp(canon_leaf, "getuid") == 0  ? "rt_getuid"
                                                               : "rt_getgid";
    int tagged = nyr_emit_runtime_call(b, runtime, -1, -1, -1, 0, 0);
    int one = tagged < 0 ? -1 : nyr_emit_const(b, 1);
    return one < 0 ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (canon_leaf && strcmp(canon_leaf, "getlogin") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 0) {
    return nyr_emit_runtime_call(b, "rt_getlogin", -1, -1, -1, 0, 0);
  }
  /* `gettimeofday` is a libc call with an out-parameter.  Keep it behind a
   * native bridge so the NYIR call uses raw pointers and the host's exact C
   * prototype.  The source-level `&local_tv` is represented as borrow(tv);
   * a timeval constructor already returns the allocated C object, so pass
   * that value rather than the address of its scalar local slot. */
  if (canon_leaf && strcmp(canon_leaf, "gettimeofday") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    const expr_t *tv_expr = e->as.call.args.data[0].val;
    if (tv_expr && tv_expr->kind == NY_E_CALL && tv_expr->as.call.callee &&
        tv_expr->as.call.callee->kind == NY_E_IDENT &&
        tv_expr->as.call.callee->as.ident.name &&
        strcmp(ny_name_leaf(tv_expr->as.call.callee->as.ident.name),
               "borrow") == 0 &&
        tv_expr->as.call.args.len == 1 && !tv_expr->as.call.args.data[0].name)
      tv_expr = tv_expr->as.call.args.data[0].val;
    int tv = tv_expr ? nyr_lower_expr(b, tv_expr) : -1;
    int tz = nyr_lower_expr(b, e->as.call.args.data[1].val);
    return tv < 0 || tz < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_gettimeofday", tv, tz, -1, 2, 0);
  }
  /* Float memory helpers box values for the interpreter.  NYIR keeps native
   * floats unboxed, so lower them directly to the ABI-safe native helpers. */
  const stmt_t *float_memory_decl = e->semantic.canonical_callee_stmt;
  bool builtin_float_memory_helper =
      !nyr_user_defined_fn(b, name) ||
      (float_memory_decl && float_memory_decl->kind == NY_S_FUNC &&
       ny_is_stdlib_tok(float_memory_decl->tok));
  if (canon_leaf && builtin_float_memory_helper &&
      (strcmp(canon_leaf, "load32_f32") == 0 ||
       strcmp(canon_leaf, "store32_f32") == 0 ||
       strcmp(canon_leaf, "load64_f64") == 0 ||
       strcmp(canon_leaf, "store64_f64") == 0)) {
    bool store = canon_leaf[0] == 's';
    size_t min_args = store ? 2u : 1u;
    size_t max_args = store ? 3u : 2u;
    if (e->as.call.args.len < min_args || e->as.call.args.len > max_args) {
      return nyr_fail(b, "native NYIR lower: %s argument count mismatch",
                      canon_leaf),
             -1;
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        return nyr_fail(b, "native NYIR lower: %s expects positional arguments",
                        canon_leaf),
               -1;
      }
    }
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int idx = e->as.call.args.len == (store ? 3u : 2u)
                  ? nyr_lower_expr(b, e->as.call.args.data[store ? 2 : 1].val)
                  : nyr_emit_const(b, 0);
    if (addr < 0 || idx < 0)
      return -1;
    /* The f32 runtime bridge consumes the tagged indexed-memory ABI; the
     * source-level offset is a raw native integer.  Tag it at this boundary,
     * including non-constant offsets, so odd byte offsets remain unchanged.
     */
    int runtime_idx = idx;
    if (strcmp(canon_leaf, "load32_f32") == 0 ||
        strcmp(canon_leaf, "store32_f32") == 0)
      runtime_idx = nyr_emit_runtime_call(b, "rt_tag", idx, -1, -1, 1, 0);
    if (runtime_idx < 0)
      return -1;
    if (!store) {
      if (strcmp(canon_leaf, "load32_f32") == 0)
        return nyr_emit_runtime_call(b, "rt_load32_f64", addr, runtime_idx, -1,
                                     2, NYIR_SCALAR_F64);
      int effective = nyr_emit_add_i64(b, addr, idx);
      return effective < 0 ? -1 : nyr_emit_load_f64(b, effective);
    }
    int value = nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (value < 0)
      return -1;
    nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
    const expr_t *value_expr = e->as.call.args.data[1].val;
    if (value_class != NYIR_SCALAR_F64 && value_expr &&
        value_expr->kind == NY_E_IDENT && value_expr->as.ident.name) {
      /* Inferred local metadata can lag behind its physical slot when a
       * mutable declaration has a stale nominal type. If every reaching
       * store emitted so far is native F64, reload using that proven slot
       * class rather than decoding the generic LOAD_LOCAL as tagged. */
      const nyr_local_t *value_local =
          nyr_find_local_nearest(b, value_expr->as.ident.name);
      bool saw_store = false;
      bool all_f64_stores = value_local != NULL;
      for (size_t si = 0; value_local && si < b->nyir.len; ++si) {
        const nyr_inst_t *store_inst = &b->nyir.data[si];
        if (store_inst->op != NYIR_STORE_LOCAL ||
            store_inst->imm != value_local->slot)
          continue;
        saw_store = true;
        if (nyr_value_scalar_class(b, store_inst->a) != NYIR_SCALAR_F64) {
          all_f64_stores = false;
          break;
        }
      }
      if (saw_store && all_f64_stores) {
        int address = nyr_emit_addr_local(b, value_local->slot,
                                          value_expr->as.ident.name);
        value = address < 0 ? -1 : nyr_emit_load_f64(b, address);
        if (value < 0)
          return -1;
        value_class = NYIR_SCALAR_F64;
      }
    }
    if (value_class != NYIR_SCALAR_F64) {
      value = value_class == NYIR_SCALAR_F32 ? nyr_emit_f32_to_f64(b, value)
              : value_class == NYIR_SCALAR_I64_TAGGED
                  ? nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                          NYIR_SCALAR_F64)
                  : nyr_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    if (strcmp(canon_leaf, "store32_f32") == 0)
      return nyr_emit_runtime_call(b, "rt_store32_f64", addr, runtime_idx,
                                   value, 3, 0);
    int effective = nyr_emit_add_i64(b, addr, idx);
    return effective < 0 || !nyr_emit_store_f64(b, effective, value) ? -1
                                                                     : value;
  }
  /* Terminal primitives return VM-tagged integers from the shared runtime. */
  if (canon_leaf && !nyr_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "__tty_size") == 0 ||
       strcmp(canon_leaf, "__tty_raw") == 0 ||
       strcmp(canon_leaf, "__tty_sane_fd") == 0 ||
       strcmp(canon_leaf, "__tty_isatty") == 0 ||
       strcmp(canon_leaf, "__tty_pending") == 0)) {
    size_t expected = strcmp(canon_leaf, "__tty_pending") == 0 ? 0 : 1;
    if (e->as.call.args.len != expected) {
      return nyr_fail(b, "native NYIR lower: %s expects %zu argument(s)",
                      canon_leaf, expected),
             -1;
    }
    int arg = -1;
    if (expected) {
      if (e->as.call.args.data[0].name) {
        return nyr_fail(b,
                        "native NYIR lower: %s expects a positional argument",
                        canon_leaf),
               -1;
      }
      arg = nyr_lower_expr(b, e->as.call.args.data[0].val);
      if (arg < 0)
        return -1;
    }
    if (strcmp(canon_leaf, "__tty_isatty") == 0) {
      /* `rt_tty_isatty` is the canonical manifest entry and consumes the
       * tagged interpreter ABI.  Native callers hold the descriptor as a raw
       * scalar, so make the representation change explicit at this boundary
       * instead of calling an unregistered private raw helper. */
      int tagged_arg = nyr_emit_runtime_call(b, "rt_tag", arg, -1, -1, 1, 0);
      if (tagged_arg < 0)
        return -1;
      int tagged =
          nyr_emit_runtime_call(b, "rt_tty_isatty", tagged_arg, -1, -1, 1, 0);
      if (tagged < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_untag", tagged, -1, -1, 1, 0);
    }
    int tagged = nyr_emit_runtime_call(b, leaf, arg, -1, -1, (int)expected, 0);
    if (tagged < 0)
      return -1;
    int one = nyr_emit_const(b, 1);
    return one < 0 ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  /* Native integers are already unboxed; floating inputs need a cast and
   * dynamic/any inputs need unboxing. */
  if (canon_leaf &&
      (strcmp(canon_leaf, "float") == 0 ||
       strcmp(canon_leaf, "to_float") == 0 || strcmp(canon_leaf, "f64") == 0) &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = nyr_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    /*
     * A range induction variable (and raw arithmetic over it) keeps its
     * untagged integer ABI even when the surrounding source expression has a
     * conservative dynamic fact. Convert that scalar directly; passing it to
     * the tagged-value decoder would treat raw 1 as an invalid Ny word.
     */
    if (nyr_expr_has_raw_int_abi(b, arg_expr, 0))
      return nyr_emit_i64_to_f64(b, value);
    /* A dynamic accessor can have an f64 HM result because its fallback is a
     * float, while the actual NYIR value is still a tagged float.  Resolve
     * that physical representation before trusting the inferred type. */
    if (nyr_expr_is_canonical_dynamic_value(b, arg_expr) ||
        nyr_expr_is_any(b, arg_expr))
      return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    if (nyr_expr_is_f32(b, arg_expr))
      return nyr_emit_f32_to_f64(b, value);
    if (nyr_expr_is_f64(b, arg_expr))
      return value;
    /*
     * The AST-level type can say int while the register holds f64: context
     * propagation computes integer sub-expressions in the f64 domain
     * inside float-returning functions.  The register class is
     * authoritative — i64.to.f64 of an f64 register reinterpreted the IEEE
     * bits and spectral's divisor became garbage.
     */
    if (nyr_value_is_f64(b, value, 0))
      return value;
    return nyr_emit_i64_to_f64(b, value);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "int") == 0 || strcmp(canon_leaf, "to_int") == 0) &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    /* BigInt is an opaque handle in the native ABI.  It must be converted
     * through the BigInt narrowing contract; returning the handle itself
     * happens to look like a valid i64 and makes `int(Z(3))` compare unequal
     * while silently leaking the pointer into scalar code. */
    if (nyr_expr_is_bigint(b, arg_expr) ||
        (nyr_expr_type_name(b, arg_expr) &&
         strcmp(nyr_expr_type_name(b, arg_expr), "bigint") == 0)) {
      int bigint = nyr_lower_expr(b, arg_expr);
      if (bigint < 0)
        return -1;
      /*
       * A BigInt-typed expression may already have been narrowed to a raw
       * i64 by its producer (notably INT64_MIN). The conversion helper takes
       * a tagged value, so calling it on that raw register treats the integer
       * bits as a heap pointer. The raw value is already the requested result.
       */
      if (nyr_value_scalar_class(b, bigint) == NYIR_SCALAR_I64_RAW)
        return bigint;
      return nyr_emit_runtime_call(b, "rt_bigint_to_i64_raw", bigint, -1, -1, 1,
                                   0);
    }
    /* A native string is a C-string pointer, not an integer payload.  Route
     * the conversion through the shared parser so invalid text produces the
     * language-level zero result instead of exposing the pointer address. */
    if (nyr_expr_is_cstr(b, arg_expr)) {
      int string = nyr_lower_expr(b, arg_expr);
      return string < 0
                 ? -1
                 : nyr_emit_runtime_call(b, "rt_atoi", string, -1, -1, 1, 0);
    }
    int arg = nyr_lower_int_value(b, arg_expr);
    if (arg < 0)
      return -1;
    return arg;
  }
  /* `__flt_to_int` is an internal typed conversion, unlike the public
   * runtime entry which returns a VM-tagged integer.  Native code keeps ints
   * unboxed, so using the tagged runtime result here turns values such as
   * 36000 into 72001 and corrupts buffer sizes and sample counts. */
  if (canon_leaf && strcmp(canon_leaf, "__flt_to_int") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int arg = nyr_lower_expr(b, arg_expr);
    if (arg < 0)
      return -1;
    if (nyr_expr_is_f64_depth(b, arg_expr, 0))
      return nyr_emit_f64_to_i64(b, arg);
    if (nyr_expr_is_f32_depth(b, arg_expr, 0))
      return nyr_emit_f32_to_i64(b, arg);
    return nyr_emit_runtime_call(b, "rt_any_to_i64", arg, -1, -1, 1, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "delete") == 0 ||
       strcmp(canon_leaf, "remove") == 0) &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    bool target_is_set = nyr_expr_is_set(b, target_expr);
    bool target_is_dict = nyr_expr_is_dict(b, target_expr);
    /* Unknown receivers may contain either collection kind. Keep their
     * runtime layout dispatch in one checked helper instead of assuming dict.
     */
    if (!target_is_set && !target_is_dict) {
      int target = nyr_lower_expr(b, target_expr);
      int key = nyr_lower_dict_key(b, e->as.call.args.data[1].val);
      if (target < 0 || key < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_any_collection_remove_raw", target,
                                   key, -1, 2, 0);
    }
    if (target_is_set || target_is_dict) {
      int target = nyr_lower_expr(b, target_expr);
      int key = target_is_set
                    ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                    : nyr_lower_dict_key(b, e->as.call.args.data[1].val);
      if (target < 0 || key < 0)
        return -1;
      if (target_is_set)
        return nyr_emit_runtime_call(b, "rt_set_remove_raw", target, key, -1, 2,
                                     0);
      return nyr_emit_runtime_call(
          b,
          nyr_expr_is_cstr(b, e->as.call.args.data[1].val)
              ? "rt_dict_delete_str_raw"
              : "rt_dict_delete_raw",
          target, key, -1, 2, 0);
    }
  }
  if (canon_leaf && strcmp(canon_leaf, "set") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    bool target_is_bytes = nyr_expr_is_bytes(b, target_expr);
    bool target_is_dict = nyr_expr_is_dict(b, target_expr);
    bool target_is_list =
        nyr_expr_is_list(b, target_expr) ||
        (nyr_expr_is_dyn_list(b, target_expr) && !target_is_dict);
    if (!target_is_dict && target_expr && target_expr->kind == NY_E_IDENT &&
        target_expr->as.ident.name) {
      const nyr_local_t *target_local =
          nyr_find_local_nearest(b, target_expr->as.ident.name);
      target_is_bytes =
          target_is_bytes || (target_local && target_local->is_bytes);
      if (target_local && target_local->is_any)
        target_is_list = false;
      else
        target_is_list =
            target_is_list || (target_local && target_local->is_list);
    }
    target_is_list = target_is_list && !target_is_bytes;
    int dict = nyr_lower_expr(b, target_expr);
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const char *set_symbol = nyr_dict_set_symbol(b, key_expr);
    int key = target_is_list || target_is_bytes ? nyr_lower_expr(b, key_expr)
              : strcmp(set_symbol, "rt_dict_set_i64_str_raw") == 0
                  ? nyr_lower_expr(b, key_expr->as.call.args.data[0].val)
                  : nyr_lower_dict_key(b, key_expr);
    const expr_t *value_expr = e->as.call.args.data[2].val;
    int value = nyr_lower_expr(b, value_expr);
    bool is_bool_val = nyr_expr_is_bool(b, value_expr);
    const nyr_local_t *value_local = NULL;
    if (value_expr && value_expr->kind == NY_E_IDENT &&
        value_expr->as.ident.name) {
      value_local = nyr_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_bool)
        is_bool_val = true;
    }
    if (!target_is_bytes && !target_is_list && is_bool_val) {
      value = nyr_box_bool(b, value);
    } else {
      bool raw_dict_integer =
          value_expr && !is_bool_val &&
          ((value_expr->kind == NY_E_LITERAL &&
            value_expr->as.literal.kind == NY_LIT_INT &&
            value_expr->tok.kind != NY_T_NIL) ||
           (value_expr->semantic.resolved &&
            value_expr->semantic.rep == NY_SEM_REP_RAW_INT));
      if (value_expr && !is_bool_val && value_expr->kind == NY_E_BINARY &&
          !nyr_expr_is_f64(b, value_expr) && !nyr_expr_is_f32(b, value_expr) &&
          !nyr_expr_is_bool(b, value_expr) && !nyr_expr_is_any(b, value_expr))
        raw_dict_integer = true;
      if (value_local && !is_bool_val &&
          value_local->semantic_rep == NY_SEM_REP_RAW_INT)
        raw_dict_integer = true;
      if (!target_is_bytes && !target_is_list && raw_dict_integer) {
        value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
    }
    if (target_is_list && value_expr && value_expr->kind == NY_E_IDENT) {
      if (value_local && value_local->is_any)
        value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
    }
    if (!target_is_bytes && !target_is_list && nyr_expr_is_f64(b, value_expr)) {
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (dict < 0 || key < 0 || value < 0)
      return -1;
    if (target_is_list && nyr_expr_is_f64(b, value_expr)) {
      value = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
      if (value < 0)
        return -1;
      int updated = nyr_emit_runtime_call(b, "rt_tbuf_set_f64_bits", dict, key,
                                          value, 3, 0);
      if (updated < 0)
        return -1;
      /* The float setter may promote compact elem-8 storage to descriptor
       * storage and returns the new handle.  Keep the owning binding current
       * so subsequent compiler-generated direct loads observe the promoted
       * representation rather than stale compact slots. */
      if (target_expr->kind == NY_E_IDENT && target_expr->as.ident.name) {
        nyr_local_t *local =
            nyr_find_local_nearest(b, target_expr->as.ident.name);
        if (local) {
          local->is_dyn_list = true;
          if (!nyr_store_local_value(b, local->slot, updated))
            return -1;
        } else {
          const char *symbol = nyr_global_symbol(b, target_expr->as.ident.name);
          if (symbol) {
            int address =
                nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .imm = 0,
                                                  .symbol = symbol});
            if (address < 0 || !nyr_emit_store_i64(b, address, updated))
              return -1;
          }
        }
      }
      return updated;
    }
    bool literal_string_key =
        e->as.call.args.data[1].val &&
        e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind != NY_LIT_INT &&
        e->as.call.args.data[1].val->as.literal.kind != NY_LIT_FLOAT &&
        e->as.call.args.data[1].val->as.literal.kind != NY_LIT_BOOL &&
        e->as.call.args.data[1].val->tok.kind != NY_T_NIL;
    return nyr_emit_runtime_call(
        b,
        target_is_bytes  ? "rt_bytes_set_raw"
        : target_is_list ? "rt_tbuf_set_i64_raw"
        : strcmp(set_symbol, "rt_dict_set_i64_str_raw") == 0 ? set_symbol
        : (literal_string_key ||
           nyr_expr_is_cstr(b, e->as.call.args.data[1].val))
            ? "rt_native_dict_set_str_compact"
            : "rt_value_set_tagged",
        dict, key, value, 3, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "values") == 0 &&
      (e->as.call.args.len == 1 || e->as.call.args.len == 3) &&
      !e->as.call.args.data[0].name &&
      (e->as.call.args.len == 1 ||
       (!e->as.call.args.data[1].name && !e->as.call.args.data[2].name)) &&
      nyr_expr_is_range(b, e->as.call.args.data[0].val)) {
    int range = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return range < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_range_values_raw", range,
                                             -1, -1, 1, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "items") == 0 ||
       strcmp(canon_leaf, "dict_items") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_dict(b, e->as.call.args.data[0].val)) {
    int dict = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return dict < 0 ? -1
                    : nyr_emit_runtime_call(b, "rt_dict_items_raw", dict, -1,
                                            -1, 1, 0);
  }
  /* The generic core keys/values wrappers accept `any` and may be
   * lowered through an imported module's dynamic ABI.  A proven dictionary
   * has a direct descriptor-producing bridge; use it so whole-program
   * lowering cannot lose the receiver's dictionary representation. */
  if (canon_leaf &&
      (strcmp(canon_leaf, "keys") == 0 ||
       strcmp(canon_leaf, "dict_keys") == 0 ||
       strcmp(canon_leaf, "values") == 0 ||
       strcmp(canon_leaf, "dict_values") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_dict(b, e->as.call.args.data[0].val)) {
    int dict = nyr_lower_expr(b, e->as.call.args.data[0].val);
    const char *runtime = (strcmp(canon_leaf, "keys") == 0 ||
                           strcmp(canon_leaf, "dict_keys") == 0)
                              ? "rt_dict_keys_raw"
                              : "rt_dict_values_raw";
    return dict < 0 ? -1
                    : nyr_emit_runtime_call(b, runtime, dict, -1, -1, 1, 0);
  }
  bool is_thread_call =
      canon_leaf && (strcmp(canon_leaf, "thread_spawn_call") == 0 ||
                     strcmp(canon_leaf, "thread_launch_call") == 0);
  const stmt_t *thread_wrapper_fn =
      is_thread_call && name ? nyr_find_user_function(b, name) : NULL;
  if (is_thread_call && !nyr_user_defined_fn(b, name) &&
      !nyr_find_imported_function(b, name) &&
      !(thread_wrapper_fn && ny_is_stdlib_tok(thread_wrapper_fn->tok))) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      return nyr_fail(
                 b, "native NYIR lower: %s expects callback and argument list",
                 leaf),
             -1;
    }
    const expr_t *callback_expr = e->as.call.args.data[0].val;
    const stmt_t *target_fn = NULL;
    if (callback_expr && callback_expr->kind == NY_E_IDENT) {
      nyr_local_t *callback_local = nyr_find_expr_local(b, callback_expr);
      target_fn =
          callback_local
              ? (callback_local->callable_name
                     ? nyr_find_user_function(b, callback_local->callable_name)
                     : NULL)
              : nyr_find_user_function(b, callback_expr->as.ident.name);
    }
    int callback = nyr_lower_expr(b, callback_expr);
    int list = nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (callback < 0 || list < 0 || !target_fn) {
      return nyr_fail(
                 b, "native NYIR lower: %s requires a direct function callback",
                 leaf),
             -1;
    }
    size_t param_count = target_fn->as.fn.params.len;
    int known_count = nyr_peek_list_len_fact(b, list);
    if (param_count > 15 ||
        (param_count > 0 &&
         (known_count < 0 || (size_t)known_count < param_count))) {
      return nyr_fail(b,
                      "native NYIR lower: %s needs a statically "
                      "sized argument list",
                      leaf),
             -1;
    }
    int argv_values[NYIR_CALL_MAX_ARGS];
    size_t argc = 0;
    for (size_t i = 0; i < param_count; ++i) {
      int index = nyr_emit_const(b, (int64_t)i);
      int value = index < 0
                      ? -1
                      : nyr_emit_runtime_call(b, "rt_tbuf_get", list, index,
                                              nyr_emit_const(b, 0), 3, 0);
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
        int want_dyn = nyr_emit_const(b, dynamic ? 1 : 0);
        int elems = want_dyn < 0
                        ? -1
                        : nyr_emit_runtime_call(b, "rt_tbuf_dyn_elem", list,
                                                index, want_dyn, 3, 0);
        if (elems < 0)
          return -1;
        value = elems;
      }
      argv_values[argc++] = value;
      if (dynamic) {
        int tag = index < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_tbuf_tag", list,
                                                    index, -1, 2, 0);
        int zero = nyr_emit_const(b, 0);
        if (tag < 0 || zero < 0 || argc + 1 >= NYIR_CALL_MAX_ARGS)
          return -1;
        argv_values[argc++] = zero;
        argv_values[argc++] = tag;
      }
    }
    int argc_value = nyr_emit_const(b, (int64_t)argc);
    int raw_fn = nyr_emit_const(b, rt_tag_v(1));
    int tagged_result =
        nyr_emit_const(b, rt_tag_v(nyr_fn_result_kind(b, target_fn)));
    int stack = nyr_ir_emit(
        &b->nyir,
        (nyr_inst_t){.op = NYIR_ALLOCA, .dst = -1, .imm = (int64_t)argc * 8});
    if (argc_value < 0 || raw_fn < 0 || tagged_result < 0 || stack < 0)
      return -1;
    for (size_t i = 0; i < argc; ++i) {
      int address = stack;
      if (i) {
        int offset = nyr_emit_const(b, (int64_t)i * 8);
        address = offset < 0 ? -1 : nyr_emit_add_i64(b, stack, offset);
      }
      if (address < 0 || !nyr_emit_store_i64(b, address, argv_values[i]))
        return -1;
    }
    int result = nyr_emit_runtime_call4(
        b,
        strcmp(canon_leaf, "thread_launch_call") == 0
            ? "rt_thread_launch_call_abi"
            : "rt_thread_spawn_call_abi",
        callback, argc_value, stack, raw_fn, tagged_result, 5, 0);
    if (result < 0)
      return -1;
    return strcmp(canon_leaf, "thread_launch_call") == 0 ? nyr_emit_const(b, 0)
                                                         : result;
  }
  if (canon_leaf && strcmp(canon_leaf, "thread_spawn") == 0 &&
      !nyr_user_defined_fn(b, name)) {
    if ((e->as.call.args.len != 1 && e->as.call.args.len != 2) ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len == 2 && e->as.call.args.data[1].name)) {
      return nyr_fail(b, "native NYIR lower: thread_spawn expects callback "
                         "and optional argument"),
             -1;
    }
    const expr_t *callback_expr = e->as.call.args.data[0].val;
    const stmt_t *target_fn = NULL;
    if (callback_expr && callback_expr->kind == NY_E_IDENT) {
      nyr_local_t *callback_local = nyr_find_expr_local(b, callback_expr);
      target_fn =
          callback_local
              ? (callback_local->callable_name
                     ? nyr_find_user_function(b, callback_local->callable_name)
                     : NULL)
              : nyr_find_user_function(b, callback_expr->as.ident.name);
    }
    int callback = nyr_lower_expr(b, callback_expr);
    if (callback < 0)
      return -1;
    /* Pack the callback's expanded native ABI when its target is known.  A
     * `fn(any)` receives value/length/tag, just like an ordinary native call;
     * passing only the value made thread_spawn callbacks read bogus mutexes
     * and list elements. */
    if (target_fn && target_fn->as.fn.params.len == 1) {
      int logical = e->as.call.args.len == 2
                        ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                        : nyr_emit_const(b, 0);
      if (logical < 0)
        return -1;
      const param_t *param = &target_fn->as.fn.params.data[0];
      /* A statically known one-pointer worker has the plain native ABI used
       * by rt_thread_spawn_raw. Avoid allocating and copying a one-element
       * dynamic callback argument vector for this common worker shape. */
      if (ny_native_type_name_is_ptr(param->type))
        return nyr_emit_runtime_call(b, "rt_thread_spawn_raw", callback,
                                     logical, -1, 2, 0);
      bool list_param = (param->semantic.resolved &&
                         param->semantic.rep == NY_SEM_REP_TYPED_BUFFER) ||
                        ny_native_type_name_is_list(param->type);
      bool dynamic = (param->semantic.resolved &&
                      (param->semantic.rep == NY_SEM_REP_STRING ||
                       param->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)) ||
                     ny_native_type_name_is_str(param->type) ||
                     ny_native_type_name_is_any(param->type);
      int argc = 1;
      int argv[3] = {logical, -1, -1};
      if (list_param) {
        int length =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", logical, -1, -1, 1, 0);
        if (length < 0)
          return -1;
        argv[1] = length;
        argc = 2;
      } else if (dynamic) {
        int length = nyr_expr_is_cstr(b, e->as.call.args.len == 2
                                             ? e->as.call.args.data[1].val
                                             : NULL)
                         ? nyr_emit_const(b, 0)
                         : nyr_emit_runtime_call(b, "rt_tbuf_len_raw", logical,
                                                 -1, -1, 1, 0);
        int64_t tag_value = (e->as.call.args.len == 2 &&
                             nyr_expr_is_cstr(b, e->as.call.args.data[1].val))
                                ? 121
                            : (e->as.call.args.len == 2 &&
                               nyr_expr_is_list(b, e->as.call.args.data[1].val))
                                ? TAG_LIST
                                : 3;
        int tag = nyr_emit_const(b, tag_value);
        if (length < 0 || tag < 0)
          return -1;
        argv[1] = length;
        argv[2] = tag;
        argc = 3;
      }
      int argc_value = nyr_emit_const(b, (int64_t)argc);
      int raw_fn = nyr_emit_const(b, rt_tag_v(1));
      int tagged_result =
          nyr_emit_const(b, rt_tag_v(nyr_fn_result_kind(b, target_fn)));
      int stack = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ALLOCA,
                                                    .dst = -1,
                                                    .imm = (int64_t)argc * 8});
      if (argc_value < 0 || raw_fn < 0 || tagged_result < 0 || stack < 0)
        return -1;
      for (int i = 0; i < argc; ++i) {
        int address = stack;
        if (i) {
          int offset = nyr_emit_const(b, (int64_t)i * 8);
          address = offset < 0 ? -1 : nyr_emit_add_i64(b, stack, offset);
        }
        if (address < 0 || !nyr_emit_store_i64(b, address, argv[i]))
          return -1;
      }
      return nyr_emit_runtime_call4(b, "rt_thread_spawn_call_abi", callback,
                                    argc_value, stack, raw_fn, tagged_result, 5,
                                    0);
    }
    int argument = e->as.call.args.len == 2
                       ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                       : nyr_emit_const(b, 0);
    return argument < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_thread_spawn_raw",
                                                callback, argument, -1, 2, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__load_item") == 0 ||
       strcmp(canon_leaf, "__load_item_fast") == 0 ||
       strcmp(canon_leaf, "__load_item_any") == 0) &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int target = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int index = nyr_lower_int_value(b, e->as.call.args.data[1].val);
    if (target < 0 || index < 0)
      return -1;
    bool dynamic_load =
        strcmp(canon_leaf, "__load_item_any") == 0 || b->value_for_any_call;
    if (dynamic_load) {
      /* rt_load_item_any has the public tagged-index ABI; NYIR indices are
       * raw scalars.  Tag exactly at this boundary so odd positions do not
       * decode as the preceding slot (1 must remain index 1). */
      index = nyr_emit_runtime_call(b, "rt_tag", index, -1, -1, 1, 0);
      if (index < 0)
        return -1;
    }
    return nyr_emit_runtime_call(
        b, dynamic_load ? "rt_load_item_any" : "rt_load_item", target, index,
        -1, 2, 0);
  }
  if (canon_leaf &&
      (strcmp(canon_leaf, "__store_item") == 0 ||
       strcmp(canon_leaf, "__store_item_fast") == 0) &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const expr_t *value_expr = e->as.call.args.data[2].val;
    int target = nyr_lower_expr(b, target_expr);
    int key = nyr_lower_expr(b, key_expr);
    int value = nyr_lower_expr(b, value_expr);
    if (target < 0 || key < 0 || value < 0)
      return -1;
    if (nyr_expr_is_f64(b, value_expr) || nyr_expr_is_f32(b, value_expr)) {
      if (nyr_expr_is_f32(b, value_expr))
        value = nyr_emit_f32_to_f64(b, value);
      int bits = value < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_f64_bits", value, -1,
                                                   -1, 1, 0);
      value = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, 0);
      if (value < 0)
        return -1;
    } else if (nyr_expr_is_bool(b, value_expr)) {
      value = nyr_box_bool(b, value);
      if (value < 0)
        return -1;
    }
    /* The runtime helper owns the list representation boundary.  In
     * particular, dynamic booleans must promote compact elem-8 lists before
     * storing so their provenance is not lost. */
    /* Both private store primitives are lowered from NYIR's unboxed scalar
     * ABI.  Sending __store_item through the tagged public helper made an odd
     * raw index (1) look like tagged zero and wrote the wrong slot. */
    /* The raw integer ABI cannot identify a pointer from the payload's tag
     * bits: values such as 246 collide with NY_NATIVE_IS.  Select the scalar
     * store from the source representation so integer bytes never trigger a
     * dynamic-list promotion.  Keep the representation-aware helper for
     * pointers, strings, and other genuinely dynamic elements. */
    const char *store_runtime = nyr_expr_is_raw_scalar(b, value_expr, 0)
                                    ? "rt_tbuf_set_i64_raw"
                                    : "rt_store_item_fast_raw";
    if (nyr_emit_runtime_call(b, store_runtime, target, key, value, 3, 0) < 0)
      return -1;
    /* This intrinsic can materialize an element in reserved capacity, just
     * like indexed assignment.  Refresh any local sequence-length fact from
     * the runtime header so subsequent reads do not retain the pre-store
     * empty length (list(n) starts empty with capacity n). */
    nyr_local_t *stored_list =
        target_expr && target_expr->kind == NY_E_IDENT
            ? nyr_find_local_nearest(b, target_expr->as.ident.name)
            : NULL;
    if (stored_list &&
        (stored_list->is_list || stored_list->list_len_slot >= 0)) {
      int updated_len =
          nyr_emit_runtime_call(b, "rt_tbuf_len_raw", target, -1, -1, 1, 0);
      if (updated_len < 0)
        return -1;
      if (stored_list->list_len_slot < 0)
        stored_list->list_len_slot = b->next_local_slot++;
      if (!nyr_store_local_value(b, stored_list->list_len_slot, updated_len) ||
          !nyr_record_list_len_fact(b, target, updated_len))
        return -1;
    }
    return value;
  }
  if (canon_leaf && strcmp(canon_leaf, "__list_sum_int_range") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    int target = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int start = nyr_lower_expr(b, e->as.call.args.data[1].val);
    int stop = nyr_lower_expr(b, e->as.call.args.data[2].val);
    if (target < 0 || start < 0 || stop < 0)
      return -1;
    int result = nyr_emit_runtime_call(b, "rt_list_sum_int_range", target,
                                       start, stop, 3, 0);
    if (result < 0)
      return -1;
    /* The shared helper retains the boxed legacy return ABI.  Its semantic
     * declaration is scalar, so native callers decode that value exactly
     * once at this boundary. */
    return nyr_emit_runtime_call(b, "rt_any_to_i64", result, -1, -1, 1, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "set_idx") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 3 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      !e->as.call.args.data[2].name) {
    const expr_t *target_expr = e->as.call.args.data[0].val;
    const expr_t *key_expr = e->as.call.args.data[1].val;
    const expr_t *value_expr = e->as.call.args.data[2].val;
    const expr_t *literal_target = nyr_resolve_list_literal(b, target_expr, 0);
    bool global_dynamic_sequence = false;
    bool typed_global_int_list = false;
    const char *global_symbol = NULL;
    bool descriptor = literal_target ? nyr_expr_is_dyn_list(b, literal_target)
                                     : nyr_expr_is_dyn_list(b, target_expr);
    /* A mutable typed list loses its literal initializer, so the generic
     * `any` fallback above can incorrectly select descriptor stride 24.
     * Preserve the declaration's concrete element layout for indexed stores.
     */
    if (target_expr->kind == NY_E_IDENT) {
      nyr_local_t *local =
          nyr_find_local_nearest(b, target_expr->as.ident.name);
      if (local && local->is_list)
        descriptor = local->is_dyn_list;
      if (!local) {
        const expr_t *global_init =
            nyr_find_top_level_value(b, target_expr->as.ident.name);
        if (global_init && nyr_expr_is_list(b, global_init)) {
          descriptor = nyr_expr_is_dyn_list(b, global_init);
          /* A top-level mut proven by the whole-program scan to hold only
           * int appends keeps elem-8 storage; its indexed stores are raw
           * i64 writes, not tagged dynamic boundary crossings. */
          if (descriptor &&
              nyr_top_mut_is_append_int_list(b, target_expr->as.ident.name))
            descriptor = false;
          global_dynamic_sequence = descriptor;
          global_symbol = nyr_global_symbol(b, target_expr->as.ident.name);
        }
      }
      /* A top-level `list<int>` declaration fixes the element ABI even when
       * its initializer is the generic `list(size)` allocator.  Do not let
       * the initializer's dynamic fallback select descriptor stores: other
       * functions may already have compiled direct elem-8 accesses to this
       * shared buffer. */
      if (!local && b->prog) {
        const stmt_t *typed_decl =
            target_expr->tok.filename
                ? nyr_find_top_level_var_in_source_in_prog(
                      b, b->prog, target_expr->as.ident.name,
                      target_expr->tok.filename)
                : NULL;
        if (!typed_decl)
          typed_decl =
              nyr_find_top_level_var_stmt(b, target_expr->as.ident.name);
        if (typed_decl && typed_decl->kind == NY_S_VAR) {
          for (size_t ti = 0; ti < typed_decl->as.var.names.len; ++ti) {
            if (typed_decl->as.var.names.data[ti] &&
                strcmp(typed_decl->as.var.names.data[ti],
                       target_expr->as.ident.name) == 0 &&
                ti < typed_decl->as.var.types.len &&
                typed_decl->as.var.types.data[ti] &&
                strcmp(typed_decl->as.var.types.data[ti], "list<int>") == 0) {
              typed_global_int_list = true;
              break;
            }
          }
        }
        if (typed_global_int_list) {
          descriptor = false;
          global_dynamic_sequence = false;
        }
      }
    }
    bool literal_string_key = key_expr && key_expr->kind == NY_E_LITERAL &&
                              key_expr->as.literal.kind != NY_LIT_INT &&
                              key_expr->as.literal.kind != NY_LIT_FLOAT &&
                              key_expr->as.literal.kind != NY_LIT_BOOL &&
                              key_expr->tok.kind != NY_T_NIL;
    bool string_key = literal_string_key || nyr_expr_is_cstr(b, key_expr);
    bool raw_dict_integer_key =
        key_expr && !string_key && !nyr_expr_is_any(b, key_expr) &&
        ((key_expr->kind == NY_E_LITERAL &&
          key_expr->as.literal.kind == NY_LIT_INT &&
          key_expr->tok.kind != NY_T_NIL) ||
         (key_expr->semantic.resolved &&
          key_expr->semantic.rep == NY_SEM_REP_RAW_INT));
    bool target_is_dict = nyr_expr_is_dict(b, target_expr);
    if (!target_is_dict && target_expr && target_expr->kind == NY_E_IDENT &&
        target_expr->as.ident.name) {
      nyr_local_t *local =
          nyr_find_local_nearest(b, target_expr->as.ident.name);
      target_is_dict = target_is_dict || (local && local->is_dict);
    }
    if (string_key)
      target_is_dict = true;
    int target = nyr_lower_expr(b, target_expr);
    int key = target_is_dict ? nyr_lower_dict_key(b, key_expr)
                             : nyr_lower_expr(b, key_expr);
    int value = nyr_lower_expr(b, value_expr);
    /* The public set_idx/set helpers accept `any`; their receiver can be nil,
     * a dict, or any supported sequence.  Do not specialize an any formal to
     * the tbuf setter here: that both skips receiver validation and reads the
     * tbuf header after a failed nil write.  Keep all three arguments in the
     * canonical dynamic ABI for the representation-aware runtime bridge. */
    if (target >= 0 && key >= 0 && value >= 0 &&
        nyr_expr_is_any(b, target_expr) && !e->as.call.args.data[0].name &&
        !e->as.call.args.data[1].name && !e->as.call.args.data[2].name) {
      /* The generic dynamic setter is still a mutation of its receiver.
       * Detach a shared local sequence before taking this early-return path,
       * just as the specialized tbuf setter does below. */
      if (target_expr->kind == NY_E_IDENT && target_expr->as.ident.name) {
        nyr_local_t *local =
            nyr_find_local_nearest(b, target_expr->as.ident.name);
        if (local && local->is_list && local->alias_count > 1) {
          int copy = nyr_emit_runtime_call(b, "rt_tbuf_clone_raw", target, -1,
                                           -1, 1, NYIR_SCALAR_TYPED_BUFFER);
          if (copy < 0 || !nyr_store_local_value(b, local->slot, copy))
            return -1;
          target = copy;
          local->alias_count = 1;
        }
      }
      /* rt_value_set_tagged is the generic dynamic-value boundary: its key
       * and item arguments must use tagged scalar values even when this
       * function body has raw NYIR integers.  Passing the raw odd index 1
       * made the runtime untag it to 0, and raw item 99 was observed as 49.
       * Preserve already-dynamic values, booleans, and heap handles. */
      if (nyr_value_scalar_class(b, key) == NYIR_SCALAR_I64_RAW) {
        key = nyr_emit_runtime_call(b, "rt_tag", key, -1, -1, 1, 0);
        if (key < 0)
          return -1;
      }
      if (!nyr_expr_is_bool(b, value_expr) &&
          (nyr_expr_has_raw_int_abi(b, value_expr, 0) ||
           nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_RAW)) {
        value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      /* The tagged setter consumes a boxed dynamic scalar.  Typed f64
       * expressions stay in the FP register class until this boundary, so
       * preserve their bits and box them instead of passing an FP value in a
       * GP/tagged argument slot. */
      if (nyr_expr_is_f64(b, value_expr) ||
          nyr_value_scalar_class(b, value) == NYIR_SCALAR_F64) {
        int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
        value = bits < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                                 -1, 1, 0);
        if (value < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_value_set_tagged", target, key, value,
                                   3, 0);
    }
    bool cloned_for_cow = false;
    nyr_local_t *cow_local = NULL;
    if (!target_is_dict && target_expr && target_expr->kind == NY_E_IDENT &&
        target_expr->as.ident.name) {
      nyr_local_t *local =
          nyr_find_local_nearest(b, target_expr->as.ident.name);
      cow_local = local;
      if (local && local->is_list && local->alias_count > 1) {
        int copy = target < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_tbuf_clone_raw",
                                                      target, -1, -1, 1,
                                                      NYIR_SCALAR_TYPED_BUFFER);
        if (copy < 0 || !nyr_store_local_value(b, local->slot, copy))
          return -1;
        target = copy;
        local->alias_count = 1;
        cloned_for_cow = true;
      }
      if (!local) {
        const expr_t *global_init =
            nyr_find_top_level_value(b, target_expr->as.ident.name);
        bool global_list = global_init && nyr_expr_is_list(b, global_init);
        const stmt_t *global_decl = NULL;
        /*
         * Prefer the declaration from the store site's own file: the merged
         * program also contains stdlib `#main` self-test bindings, and an
         * unfiltered first match can return e.g. stdlib's immutable
         * `def xs = [1, 2, 3]` for a user's `mut xs`, forcing
         * copy-on-write cloning on every indexed store (linear loop becomes
         * O(n^2); ~90us/store measured).  Fall back to the unfiltered lookup
         * for genuinely cross-file globals.
         */
        if (b->prog && target_expr->tok.filename)
          for (size_t root = 0; root < b->prog->body.len && !global_decl;
               ++root)
            global_decl = nyr_find_top_level_var_in_source_stmt(
                b->prog->body.data[root], target_expr->as.ident.name,
                target_expr->tok.filename, 0);
        if (!global_decl)
          global_decl =
              nyr_find_top_level_var_stmt(b, target_expr->as.ident.name);
        bool global_mutable = global_decl && global_decl->kind == NY_S_VAR &&
                              global_decl->as.var.is_mut;
        const char *symbol = global_list ? global_symbol : NULL;
        if (!symbol && global_list)
          symbol = nyr_global_symbol(b, target_expr->as.ident.name);
        /* A mutable module binding is the owner of its buffer.  Cloning it
         * for every indexed write turns a linear loop into an O(n²) allocator
         * and is exactly what made vector/cmov-sort consume gigabytes.  Copy
         * only immutable globals that are being materialized for a mutation;
         * mutable globals already have the language's live-storage contract.
         */
        bool global_has_alias =
            symbol && nyr_global_alias_count(b, target_expr->as.ident.name) > 0;
        if (symbol && (!global_mutable || global_has_alias)) {
          int copy =
              target < 0
                  ? -1
                  : nyr_emit_runtime_call(b, "rt_tbuf_clone_raw", target, -1,
                                          -1, 1, NYIR_SCALAR_TYPED_BUFFER);
          int addr = copy < 0 ? -1
                              : nyr_ir_emit(&b->nyir,
                                          (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                        .dst = -1,
                                                        .a = -1,
                                                        .b = -1,
                                                        .imm = 0,
                                                        .symbol = symbol});
          if (copy < 0 || addr < 0 || !nyr_emit_store_i64(b, addr, copy))
            return -1;
          target = copy;
        }
      }
    }
    /*
     * Whole-program proven top-level elem-8 integer buffers have no local
     * slot from which the local-list store diamond can learn their element
     * stride.  Keep their checked runtime setter for negative/out-of-range
     * stores, but make the overwhelmingly common in-range write a direct
     * raw store.  The shape scan rejects escaped or heterogeneously written
     * globals, and the helper repeats the length check before touching data.
     */
    bool target_is_global_ident =
        target_expr && target_expr->kind == NY_E_IDENT &&
        target_expr->as.ident.name &&
        !nyr_find_local_nearest(b, target_expr->as.ident.name);
    bool source_proven_int_buffer =
        target_is_global_ident &&
        (typed_global_int_list ||
         nyr_top_mut_is_append_int_list(b, target_expr->as.ident.name));
    bool raw_key_abi = nyr_expr_has_raw_int_abi(b, key_expr, 0);
    bool raw_value_abi = nyr_expr_has_raw_int_abi(b, value_expr, 0);
    bool value_noninteger_abi = nyr_expr_is_bool(b, value_expr) ||
                                nyr_expr_is_f64(b, value_expr) ||
                                nyr_expr_is_f32(b, value_expr);
    if (cloned_for_cow && raw_key_abi && raw_value_abi &&
        !value_noninteger_abi) {
      /*
       * A cloned typed buffer must stay a raw tbuf through the store. The
       * tagged setter is polymorphic: its typed-buffer branch returns a raw
       * pointer, while its legacy-list branch returns a tagged value. Its
       * dynamic ABI therefore cannot safely be stored back into this typed
       * local. Use the raw setter for the proven integer case.
       */
      int out = nyr_emit_runtime_call(b, "rt_tbuf_set_i64_raw", target, key,
                                      value, 3, NYIR_SCALAR_TYPED_BUFFER);
      if (out < 0 || !cow_local ||
          !nyr_store_local_value(b, cow_local->slot, out))
        return -1;
      return out;
    }
    bool proven_global_int_buffer = !target_is_dict && !descriptor &&
                                    source_proven_int_buffer && raw_key_abi &&
                                    raw_value_abi && !value_noninteger_abi;
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && target_is_global_ident)
      ny_dbg_heuristic_event(
          "NATIVE_LIST_STORE",
          "elem8-store symbol=%s dict=%s descriptor=%s source-int-proof=%s "
          "raw-key=%s raw-value=%s value-abi={bool:%s,f64:%s,f32:%s,any:%s} "
          "decision=%s",
          target_expr->as.ident.name, target_is_dict ? "yes" : "no",
          descriptor ? "yes" : "no", source_proven_int_buffer ? "yes" : "no",
          raw_key_abi ? "yes" : "no", raw_value_abi ? "yes" : "no",
          nyr_expr_is_bool(b, value_expr) ? "yes" : "no",
          nyr_expr_is_f64(b, value_expr) ? "yes" : "no",
          nyr_expr_is_f32(b, value_expr) ? "yes" : "no",
          nyr_expr_is_any(b, value_expr) ? "yes" : "no",
          proven_global_int_buffer ? "inline-raw" : "generic-guarded");
    if (proven_global_int_buffer)
      return nyr_emit_proven_elem8_write(b, target, key, value);
    bool value_unboxed_for_store = false;
    if (!target_is_dict && !descriptor && value_expr &&
        value_expr->kind == NY_E_IDENT) {
      const nyr_local_t *value_local =
          nyr_find_local(b, value_expr->as.ident.name);
      if (value_local && value_local->is_any) {
        value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
        value_unboxed_for_store = value >= 0;
      }
    }
    /* An unparameterized list has compact raw slots but exposes canonical
     * dynamic values at its indexing boundary.  Route writes through the
     * tagged setter so integer values are unboxed for elem-8 storage and
     * heterogeneous values promote the buffer to descriptor storage. */
    bool canonical_value = nyr_expr_is_canonical_dynamic_value(b, value_expr);
    if (!target_is_dict && target_expr && target_expr->kind == NY_E_IDENT &&
        target_expr->as.ident.name) {
      nyr_local_t *local =
          nyr_find_local_nearest(b, target_expr->as.ident.name);
      bool dynamic_sequence_target =
          local && (nyr_local_is_dynamic_sequence(local) || cloned_for_cow);
      if (!local && global_dynamic_sequence)
        dynamic_sequence_target = true;
      if (dynamic_sequence_target) {
        /* Arithmetic over a dynamic sequence is lowered through raw i64
         * helpers after each indexed operand crosses `rt_any_to_i64`.  The
         * semantic resolver can still label the whole binary expression as
         * tagged-dynamic because its operands came from an any boundary.
         * Preserve the actual producer representation here: feeding that raw
         * result directly to rt_tbuf_set_tagged makes 3 look like tagged 1,
         * which corrupts every dynamic-list update (vector, heapsort, and
         * insertion sort). */
        bool raw_dynamic_scalar =
            value_expr && value_expr->kind == NY_E_BINARY &&
            ((b->last_arith_expr == value_expr && b->last_arith_raw) ||
             nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_RAW) &&
            !nyr_expr_is_f64(b, value_expr) &&
            !nyr_expr_is_f32(b, value_expr) && !nyr_expr_is_bool(b, value_expr);
        /*
         * A statically typed integer read from a list is already an unboxed
         * NYIR i64 even when the index expression's semantic ABI is tagged.
         * Dynamic-sequence stores cross through rt_tbuf_set_tagged, so box
         * that raw payload exactly once.  Without this, values such as
         * `perm1[i + 1]` were interpreted as tagged words and permutation
         * updates silently wrote zero (fannkuch).
         */
        const char *value_type =
            value_expr ? nyr_expr_type_name(b, value_expr) : NULL;
        if (value_expr && value_expr->kind == NY_E_INDEX && value_type &&
            ny_native_type_name_is_int(value_type))
          raw_dynamic_scalar = true;
        /* A dynamic modulo's own lowering decodes its helper result to a
         * raw i64 (see the rt_any_mod boundary in arithmetic lowering),
         * even though the semantic resolver labels the whole expression
         * any/tagged-dynamic.  Treating it as canonical here sent the raw
         * remainder to rt_tbuf_set_tagged, which unboxed it a second
         * time: odd values stored as (v-1)/2 and even values stored as
         * themselves, silently corrupting every graph filled through
         * `xs[i] = v % m`. */
        bool raw_dynamic_mod_rhs =
            value_expr && value_expr->kind == NY_E_BINARY &&
            value_expr->as.binary.op &&
            strcmp(value_expr->as.binary.op, "%") == 0 &&
            (nyr_expr_is_any(b, value_expr->as.binary.left) ||
             nyr_expr_is_any(b, value_expr->as.binary.right)) &&
            !nyr_expr_is_f64(b, value_expr) && !nyr_expr_is_f32(b, value_expr);
        if (raw_dynamic_mod_rhs)
          raw_dynamic_scalar = true;
        /* A typed function call is also a raw scalar producer in NYIR.  The
         * dynamic-list setter consumes the canonical tagged ABI, so calls
         * such as `words[i] = be32(buf, off)` must be boxed exactly once. */
        if (value_expr &&
            (value_expr->kind == NY_E_CALL ||
             value_expr->kind == NY_E_MEMCALL) &&
            (nyr_expr_is_raw_int(value_expr) ||
             (nyr_expr_type_name(b, value_expr) &&
              strcmp(nyr_expr_type_name(b, value_expr), "int") == 0)))
          raw_dynamic_scalar = true;
        bool raw_dynamic_local = value_unboxed_for_store;
        if (!raw_dynamic_local && value_expr &&
            value_expr->kind == NY_E_IDENT && value_expr->as.ident.name) {
          const nyr_local_t *value_local =
              nyr_find_local_nearest(b, value_expr->as.ident.name);
          raw_dynamic_local = value_local && !value_local->is_any &&
                              value_local->semantic_rep == NY_SEM_REP_RAW_INT;
        }
        nyr_scalar_class_t stored_value_class =
            nyr_value_scalar_class(b, value);
        bool is_f64 = nyr_expr_is_f64(b, value_expr) ||
                      stored_value_class == NYIR_SCALAR_F64;
        bool is_f32 = !is_f64 && (nyr_expr_is_f32(b, value_expr) ||
                                  stored_value_class == NYIR_SCALAR_F32);
        if (is_f64 || is_f32) {
          if (is_f32)
            value = nyr_emit_f32_to_f64(b, value);
          int bits = value < 0 ? -1
                               : nyr_emit_runtime_call(b, "rt_f64_bits", value,
                                                       -1, -1, 1, 0);
          value = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
        } else if (nyr_expr_is_bool(b, value_expr)) {
          value = nyr_box_bool(b, value);
        } else if ((!canonical_value || raw_dynamic_scalar ||
                    raw_dynamic_local) &&
                   (!nyr_expr_is_any(b, value_expr) || raw_dynamic_scalar ||
                    raw_dynamic_local) &&
                   /* Dynamic descriptor slots carry raw scalar payloads,
                    * but container/object values are already runtime
                    * handles.  Applying rt_tag to a nested list/dict turns
                    * its pointer into an integer and destroys the value at
                    * the generic sequence boundary. */
                   !nyr_expr_is_cstr(b, value_expr) &&
                   !nyr_expr_is_bytes(b, value_expr) &&
                   !nyr_expr_is_range(b, value_expr) &&
                   !nyr_expr_is_list(b, value_expr) &&
                   !nyr_expr_is_dict(b, value_expr)) {
          value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
        }
        if (value < 0)
          return -1;
        int out = nyr_emit_runtime_call(b, "rt_tbuf_set_tagged", target, key,
                                        value, 3, 0);
        if (out < 0)
          return -1;
        if (local) {
          if (!nyr_store_local_value(b, local->slot, out))
            return -1;
        } else if (global_symbol) {
          int addr =
              nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .imm = 0,
                                                .symbol = global_symbol});
          if (addr < 0 || !nyr_emit_store_i64(b, addr, out))
            return -1;
        }
        int neg24 = nyr_emit_const(b, -24);
        int len_addr = neg24 < 0 ? -1 : nyr_emit_add_i64(b, out, neg24);
        int updated_len = len_addr < 0 ? -1 : nyr_emit_load_i64(b, len_addr);
        if (updated_len < 0)
          return -1;
        if (local && local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        if (local &&
            !nyr_store_local_value(b, local->list_len_slot, updated_len))
          return -1;
        return out;
      }
    }
    int width = nyr_emit_const(b, descriptor ? 24 : 8);
    if (target < 0 || key < 0 || value < 0 || width < 0)
      return -1;
    if (target_is_dict) {
      /* Indexed dictionary stores cross the dynamic ABI. */
      bool raw_dict_integer_item = false;
      bool is_bool_val = nyr_expr_is_bool(b, value_expr);
      const nyr_local_t *value_local = NULL;
      if (value_expr && value_expr->kind == NY_E_IDENT &&
          value_expr->as.ident.name) {
        value_local = nyr_find_local(b, value_expr->as.ident.name);
        if (value_local && value_local->is_bool)
          is_bool_val = true;
      }
      if (is_bool_val) {
        value = nyr_box_bool(b, value);
        if (value < 0)
          return -1;
      } else {
        bool raw_dict_integer =
            value_expr && ((value_expr->kind == NY_E_LITERAL &&
                            value_expr->as.literal.kind == NY_LIT_INT &&
                            value_expr->tok.kind != NY_T_NIL) ||
                           (value_expr->semantic.resolved &&
                            value_expr->semantic.rep == NY_SEM_REP_RAW_INT));
        if (value_expr && value_expr->kind == NY_E_BINARY &&
            !nyr_expr_is_f64(b, value_expr) &&
            !nyr_expr_is_f32(b, value_expr) &&
            !nyr_expr_is_bool(b, value_expr) && !nyr_expr_is_any(b, value_expr))
          raw_dict_integer = true;
        if (value_expr && value_expr->kind == NY_E_BINARY &&
            !nyr_expr_is_f64(b, value_expr) &&
            !nyr_expr_is_f32(b, value_expr) &&
            !nyr_expr_is_bool(b, value_expr)) {
          /* Mixed scalar arithmetic decodes a dynamic dictionary/get read
           * against a raw peer at the arithmetic boundary and keeps a raw
           * NYIR result.  The read still advertises `any`, so the generic
           * expr_is_any check above misses it; without tagging here the
           * stored raw word is re-halved on the next dynamic read. */
          const expr_t *bl = value_expr->as.binary.left;
          const expr_t *br = value_expr->as.binary.right;
          bool l_dyn_read = nyr_expr_is_dict_read(b, bl);
          bool r_dyn_read = nyr_expr_is_dict_read(b, br);
          bool l_raw_peer =
              bl && !nyr_expr_is_any(b, bl) && !nyr_expr_is_cstr(b, bl) &&
              ((bl->kind == NY_E_LITERAL && bl->as.literal.kind == NY_LIT_INT &&
                bl->tok.kind != NY_T_NIL) ||
               (bl->semantic.resolved &&
                bl->semantic.rep == NY_SEM_REP_RAW_INT) ||
               bl->kind == NY_E_BINARY);
          bool r_raw_peer =
              br && !nyr_expr_is_any(b, br) && !nyr_expr_is_cstr(b, br) &&
              ((br->kind == NY_E_LITERAL && br->as.literal.kind == NY_LIT_INT &&
                br->tok.kind != NY_T_NIL) ||
               (br->semantic.resolved &&
                br->semantic.rep == NY_SEM_REP_RAW_INT) ||
               br->kind == NY_E_BINARY);
          if ((l_dyn_read && r_raw_peer) || (r_dyn_read && l_raw_peer))
            raw_dict_integer = true;
        }
        if (value_local && value_local->semantic_rep == NY_SEM_REP_RAW_INT)
          raw_dict_integer = true;
        if (raw_dict_integer) {
          if (string_key) {
            value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
            if (value < 0)
              return -1;
          } else {
            raw_dict_integer_item = true;
          }
        }
      }
      /* Dictionary slots are dynamic values.  Indexed stores of typed f64
       * components must carry a boxed float handle, not the raw IEEE bits;
       * vector constructors otherwise read every coordinate as zero. */
      if (value_expr && (nyr_expr_is_f64(b, value_expr) ||
                         nyr_value_scalar_class(b, value) == NYIR_SCALAR_F64)) {
        int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
        value = bits < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                                 -1, 1, 0);
        if (value < 0)
          return -1;
      }
      const char *dict_set_symbol = string_key
                                        ? "rt_native_dict_set_str_compact"
                                        : "rt_native_dict_set_nir_i64";
      /* The raw-int setter consumes the compiler-proven unboxed value and
       * performs its dynamic tag conversion in the runtime. */
      if (raw_dict_integer_item)
        dict_set_symbol = raw_dict_integer_key
                              ? "rt_native_dict_set_nir_i64_raw_int_key"
                              : "rt_native_dict_set_nir_i64_raw_int";
      return nyr_emit_runtime_call(b, dict_set_symbol, target, key, value, 3,
                                   0);
    }
    /* Indexed assignment may fill reserved capacity in list(n) even when
     * the logical length is still zero.  The scalar fast path's length fact
     * cannot prove that operation, while the runtime tbuf bridge has the
     * authoritative capacity and preserves descriptor metadata. */
    if (target_expr->kind == NY_E_IDENT) {
      nyr_local_t *local =
          nyr_find_local_nearest(b, target_expr->as.ident.name);
      bool untyped_list_store = local && !local->is_list && local->is_any &&
                                local->type_name &&
                                strcmp(local->type_name, "list") == 0;
      if (local && (local->is_list || untyped_list_store)) {
        bool typed_i64_list = local->is_list && !local->is_any &&
                              local->type_name &&
                              strcmp(local->type_name, "list<int>") == 0;
        int neg24 = nyr_emit_const(b, -24);
        int neg16 = nyr_emit_const(b, -16);
        int eight = nyr_emit_const(b, 8);
        int len_addr = neg24 < 0 ? -1 : nyr_emit_add_i64(b, target, neg24);
        int count = len_addr < 0 ? -1 : nyr_emit_load_i64(b, len_addr);
        int elem_sz = -1;
        if (!typed_i64_list) {
          int esz_addr = neg16 < 0 ? -1 : nyr_emit_add_i64(b, target, neg16);
          elem_sz = esz_addr < 0 ? -1 : nyr_emit_load_i64(b, esz_addr);
        }
        int zero = nyr_emit_const(b, 0);
        bool value_is_f64 = nyr_expr_is_f64(b, value_expr);
        /* An f64 write changes a compact list's element representation.  It
         * must use the promoting runtime setter even when the slot is in
         * bounds; a raw store leaves later dynamic reads seeing integer bits.
         */
        if (zero >= 0 && count >= 0 && (typed_i64_list || elem_sz >= 0) &&
            (typed_i64_list || eight >= 0)) {
          int slow_lab = b->next_label++;
          int end_lab = b->next_label++;
          if (slow_lab >= 0 && end_lab >= 0) {
            bool is_f64 = value_is_f64;
            if (is_f64) {
              if (!nyr_emit_br(b, slow_lab))
                return -1;
            } else {
              int negative = nyr_emit_cmp_i64(b, NYIR_CMP_LT, key, zero);
              int past_end = nyr_emit_cmp_i64(b, NYIR_CMP_GE, key, count);
              if (negative < 0 || past_end < 0 ||
                  !nyr_emit_cmp_br_if(b, negative, slow_lab) ||
                  !nyr_emit_cmp_br_if(b, past_end, slow_lab))
                return -1;
              if (!typed_i64_list) {
                int wrong_size =
                    nyr_emit_cmp_i64(b, NYIR_CMP_NE, elem_sz, eight);
                if (wrong_size < 0 ||
                    !nyr_emit_cmp_br_if(b, wrong_size, slow_lab))
                  return -1;
              }
            }

            int width8 = nyr_emit_const(b, 8);
            int off = width8 < 0
                          ? -1
                          : nyr_push_val(b, NYIR_MUL_I64, key, width8, 0, NULL);
            int addr = off < 0 ? -1 : nyr_emit_add_i64(b, target, off);
            if (addr < 0)
              return -1;
            if (is_f64) {
              if (!nyr_emit_store_f64(b, addr, value))
                return -1;
            } else {
              if (!nyr_emit_store_i64(b, addr, value))
                return -1;
            }
            if (!nyr_emit_br(b, end_lab))
              return -1;

            if (!nyr_emit_label(b, slow_lab))
              return -1;
            int updated_target = target;
            if (is_f64) {
              int bits =
                  nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
              if (bits < 0)
                return -1;
              updated_target = nyr_emit_runtime_call(b, "rt_tbuf_set_f64_bits",
                                                     target, key, bits, 3, 0);
              if (updated_target < 0)
                return -1;
            } else {
              updated_target = nyr_emit_runtime_call(b, "rt_tbuf_set_i64_raw",
                                                     target, key, value, 3, 0);
              if (updated_target < 0)
                return -1;
            }
            if (!nyr_store_local_value(b, local->slot, updated_target))
              return -1;
            /* The runtime slow path may extend an empty/reserved list
             * (list(n)[i] = v).  Keep the local length SSA slot synchronized
             * with the authoritative tbuf header; otherwise later indexed
             * reads can reuse the stale zero-length fact and trap even though
             * the store materialized the element successfully. */
            /* The list header is already proven readable here.  Reload the
             * authoritative count directly after the slow store instead of
             * crossing the runtime boundary merely to read hdr->count. */
            int updated_len = nyr_emit_runtime_call(
                b, "rt_tbuf_len_raw", updated_target, -1, -1, 1, 0);
            if (updated_len < 0)
              return -1;
            if (local->list_len_slot < 0)
              local->list_len_slot = b->next_local_slot++;
            if (!nyr_store_local_value(b, local->list_len_slot, updated_len))
              return -1;
            if (!nyr_emit_br(b, end_lab))
              return -1;

            if (!nyr_emit_label(b, end_lab))
              return -1;
            return target;
          }
        }
      }
      if (local && local->is_list) {
        int out = -1;
        if (nyr_expr_is_f64(b, value_expr)) {
          value = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
          if (value < 0)
            return -1;
          out = nyr_emit_runtime_call(b, "rt_tbuf_set_f64_bits", target, key,
                                      value, 3, 0);
        } else {
          out = nyr_emit_runtime_call(b, "rt_tbuf_set_i64_raw", target, key,
                                      value, 3, 0);
        }
        if (out < 0)
          return -1;
        if (!nyr_store_local_value(b, local->slot, out))
          return -1;
        int updated_len =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", out, -1, -1, 1, 0);
        if (updated_len < 0)
          return -1;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        if (!nyr_store_local_value(b, local->list_len_slot, updated_len))
          return -1;
        return out;
      }
    }
    /* Indexed assignment through another index expression is a real nested
     * container boundary.  It cannot use the scalar-width fallback below:
     * the selected inner sequence may be descriptor-backed even when the
     * outer sequence is statically typed.  Keep f64 metadata through the
     * runtime setter so a later chained read sees the value as a float. */
    if (target_expr && target_expr->kind == NY_E_INDEX) {
      if (nyr_expr_is_f64(b, value_expr)) {
        int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
        if (bits < 0)
          return -1;
        return nyr_emit_runtime_call(b, "rt_tbuf_set_f64_bits", target, key,
                                     bits, 3, 0);
      }
      /* The indexed target itself is a dynamic sequence boundary.  A scalar
       * local copied out of that sequence is raw after its declaration load
       * (the function-formal path explicitly calls rt_any_to_i64), while the
       * setter below consumes a canonical tagged value.  Box that producer
       * exactly once; otherwise heap swaps write odd raw integers as tagged
       * payloads and turn later reads into bogus pointers/functions. */
      bool raw_dynamic_value =
          value_expr && value_expr->kind == NY_E_BINARY &&
          !nyr_expr_is_f64(b, value_expr) && !nyr_expr_is_f32(b, value_expr) &&
          !nyr_expr_is_bool(b, value_expr) && !nyr_expr_is_any(b, value_expr);
      raw_dynamic_value = raw_dynamic_value || value_unboxed_for_store;
      if (raw_dynamic_value) {
        value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_tbuf_set_tagged", target, key, value,
                                   3, 0);
    }
    /* An unproven view (notably `canvas.get(2)`) may be either a scalar or
     * descriptor tbuf.  Let the runtime inspect its header instead of
     * materializing a guessed stride and potentially overwriting metadata. */
    if (!target_expr || target_expr->kind != NY_E_IDENT ||
        !nyr_find_local(b, target_expr->as.ident.name) ||
        !nyr_find_local(b, target_expr->as.ident.name)->is_list) {
      bool is_f64 = nyr_expr_is_f64(b, value_expr);
      if (is_f64) {
        value = nyr_emit_runtime_call(b, "rt_f64_to_i64", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_tbuf_set_i64_raw", target, key, value,
                                   3, 0);
    }
    int length = nyr_peek_list_len_fact(b, target);
    int byte_len = nyr_peek_alloc_fact(b, target);
    int dynamic_byte_len =
        length < 0 ? -1 : nyr_push_val(b, NYIR_MUL_I64, length, width, 0, NULL);
    int offset = nyr_push_val(b, NYIR_MUL_I64, key, width, 0, NULL);
    if (offset < 0 || !nyr_emit_bounds_check_value(b, target, offset,
                                                   dynamic_byte_len, byte_len))
      return -1;
    int address = nyr_emit_add_i64(b, target, offset);
    bool is_f64 = nyr_expr_is_f64(b, value_expr);
    if (address < 0)
      return -1;
    if (is_f64) {
      if (!nyr_emit_store_f64(b, address, value))
        return -1;
    } else {
      if (!nyr_emit_store_i64(b, address, value))
        return -1;
    }
    if (descriptor) {
      int o8 = nyr_emit_const(b, 8);
      int o16 = nyr_emit_const(b, 16);
      int a8 = o8 < 0 ? -1 : nyr_emit_add_i64(b, address, o8);
      int a16 = o16 < 0 ? -1 : nyr_emit_add_i64(b, address, o16);
      int len_val =
          nyr_expr_is_cstr(b, value_expr)
              ? nyr_emit_runtime_call(b, "rt_cstr_len", value, -1, -1, 1, 0)
              : nyr_emit_const(b, 0);
      int tag_val = nyr_emit_const(
          b, is_f64 ? TAG_FLOAT : (nyr_expr_is_cstr(b, value_expr) ? 121 : 3));
      if (a8 < 0 || a16 < 0 || len_val < 0 || tag_val < 0 ||
          !nyr_emit_store_i64(b, a8, len_val) ||
          !nyr_emit_store_i64(b, a16, tag_val))
        return -1;
    }
    return target;
  }
  /*
   * core.reflect._raw_len is a tiny layout helper used by repr/bytes
   * paths.  Keep it in NYIR instead of emitting a call to its stdlib body:
   * the native reachable-function collector intentionally omits internal
   * reflection helpers, while the helper's contract is exactly the managed
   * string/bytes header length at payload - 16.
   */
  if (canon_leaf && strcmp(canon_leaf, "_raw_len") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *obj_expr = e->as.call.args.data[0].val;
    int obj = nyr_lower_expr(b, obj_expr);
    bool is_list = nyr_expr_is_list(b, obj_expr);
    if (!is_list && obj_expr->kind == NY_E_IDENT) {
      const nyr_local_t *local = nyr_find_local(b, obj_expr->as.ident.name);
      if (local && local->is_list && !local->is_any && !local->is_dyn_list)
        is_list = true;
    }
    if (!is_list && obj_expr->kind == NY_E_IDENT) {
      const expr_t *resolved =
          nyr_find_top_level_value(b, obj_expr->as.ident.name);
      if (resolved && resolved != obj_expr)
        is_list = nyr_expr_is_list(b, resolved);
    }
    if (is_list) {
      int known_len = nyr_peek_list_len_fact(b, obj);
      if (known_len >= 0)
        return known_len;
      if (nyr_value_scalar_class(b, obj) == NYIR_SCALAR_I64_TAGGED)
        return nyr_emit_runtime_call(b, "rt_len", obj, -1, -1, 1, 0);
      return nyr_emit_runtime_call(b, "rt_tbuf_len_raw", obj, -1, -1, 1, 0);
    }
    return nyr_emit_runtime_call(b, "rt_len", obj, -1, -1, 1, 0);
  }
  /*
   * `append(list, value)` is the public core spelling.  The semantic
   * resolver may canonicalize it to core.reflect.append, but native
   * lowering must use the raw tbuf representation directly; otherwise the
   * omitted stdlib helper leaves an unresolved ny_fn symbol.
   */
  bool canonical_stdlib_callee =
      e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
      ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok);
  if (canon_leaf && strcmp(canon_leaf, "append") == 0 &&
      (!nyr_user_defined_fn(b, name) || canonical_stdlib_callee) &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    const expr_t *value_expr = e->as.call.args.data[1].val;
    int list = nyr_lower_expr(b, target);
    bool value_is_string = nyr_expr_is_cstr(b, value_expr);
    bool value_is_async = value_expr && value_expr->kind == NY_E_UNARY &&
                          value_expr->as.unary.op &&
                          strcmp(value_expr->as.unary.op, "async") == 0;
    nyr_local_t *target_local =
        target && e->as.call.args.data[0].val->kind == NY_E_IDENT
            ? nyr_find_local(b, e->as.call.args.data[0].val->as.ident.name)
            : NULL;
    /* Attached list methods are value-producing operations: an alias kept
     * before `xs = xs.append(v)` must retain the old sequence. Generic
     * collection insertion (for example `add(xs, v)`) remains in-place. */
    bool value_member =
        e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED &&
        nyr_expr_is_list(b, target);
    bool aliased_target =
        target_local && target_local->is_list && target_local->alias_count > 1;
    if (target && target->kind == NY_E_IDENT && target->as.ident.name) {
      const expr_t *global_init =
          nyr_find_top_level_value(b, target->as.ident.name);
      bool is_global_alias = global_init && global_init->kind == NY_E_IDENT &&
                             nyr_expr_is_list(b, target);
      aliased_target = aliased_target || is_global_alias ||
                       (!target_local &&
                        nyr_global_alias_count(b, target->as.ident.name) > 0);
    }
    if (value_member || aliased_target) {
      list = nyr_emit_runtime_call(b, "rt_tbuf_clone_raw", list, -1, -1, 1, 0);
      if (list < 0)
        return -1;
      if (target_local)
        target_local->alias_count = 1;
    }
    bool declared_scalar_list =
        target_local && target_local->is_list && !target_local->is_any &&
        target_local->type_name &&
        !ny_native_type_name_is_dyn_list(target_local->type_name);
    bool scalar_list =
        (declared_scalar_list || nyr_expr_is_list(b, target)) &&
        !nyr_expr_is_dyn_list(b, target) && !nyr_expr_is_list(b, value_expr) &&
        !nyr_expr_is_dict(b, value_expr) && !nyr_expr_is_bytes(b, value_expr) &&
        !nyr_expr_is_cstr(b, value_expr) && !nyr_expr_is_any(b, value_expr) &&
        value_expr->kind != NY_E_CALL && value_expr->kind != NY_E_MEMCALL &&
        !value_is_async;
    bool value_is_f64 = nyr_expr_is_f64(b, value_expr);
    bool value_is_f32 = nyr_expr_is_f32(b, value_expr);
    if (getenv("NYTRIX_APPEND_DEBUG") && value_expr &&
        value_expr->kind == NY_E_LITERAL &&
        value_expr->as.literal.kind == NY_LIT_FLOAT) {
      fprintf(stderr,
              "[append-debug] float-lit hint=%d scalar_list=%d declared=%d "
              "vf64=%d vf32=%d\n",
              (int)value_expr->as.literal.hint, (int)scalar_list,
              (int)declared_scalar_list, (int)value_is_f64, (int)value_is_f32);
    }
    /*
     * Inferred (undeclared) lists have no proven element flavor: storing
     * f64/f32 IEEE bits through the scalar raw path leaves words that
     * dynamic readers cannot decode (float appends printed as garbage).
     * Keep the scalar raw path for declared typed lists only; inferred
     * destinations take the tagged path below, which boxes floats.
     */
    if (!declared_scalar_list && (value_is_f64 || value_is_f32))
      scalar_list = false;
    bool dynamic_append =
        !scalar_list &&
        (((nyr_expr_is_any(b, value_expr) || value_expr->kind == NY_E_CALL ||
           value_expr->kind == NY_E_MEMCALL) &&
          !nyr_expr_is_raw_dynamic_read(b, value_expr)) ||
         nyr_expr_is_dict(b, value_expr) || nyr_expr_is_list(b, value_expr) ||
         nyr_expr_is_bytes(b, value_expr) || value_is_async || value_is_f64 ||
         value_is_f32);
    const nyr_local_t *append_value_local =
        value_expr && value_expr->kind == NY_E_IDENT
            ? nyr_find_local(b, value_expr->as.ident.name)
            : NULL;
    bool append_value_is_raw_integer =
        value_expr &&
        ((value_expr->kind == NY_E_LITERAL &&
          value_expr->as.literal.kind == NY_LIT_INT &&
          value_expr->tok.kind != NY_T_NIL) ||
         (value_expr->semantic.resolved &&
          value_expr->semantic.rep == NY_SEM_REP_RAW_INT) ||
         (append_value_local &&
          append_value_local->semantic_rep == NY_SEM_REP_RAW_INT));
    bool use_tagged_append =
        (((nyr_expr_is_any(b, value_expr) || value_expr->kind == NY_E_CALL ||
           value_expr->kind == NY_E_MEMCALL) &&
          !nyr_expr_is_raw_dynamic_read(b, value_expr)) ||
         nyr_expr_is_dict(b, value_expr) || nyr_expr_is_list(b, value_expr) ||
         nyr_expr_is_bytes(b, value_expr) || value_is_async ||
         (!scalar_list && (value_is_f64 || value_is_f32)));
    bool saved_value_for_any_call = b->value_for_any_call;
    if (use_tagged_append)
      b->value_for_any_call = true;
    int value = nyr_lower_expr(b, value_expr);
    b->value_for_any_call = saved_value_for_any_call;
    if (list < 0 || value < 0)
      return -1;
    if (value_is_string) {
      value = nyr_emit_runtime_call(b, "rt_cstr_to_str", value, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    /* Typed scalar buffers store native words.  f64 expressions use an LLVM
     * double register, so preserve their IEEE payload before the raw append
     * ABI; passing the double register as an i64 argument produces an invalid
     * call boundary and corrupts later elements. */
    if (scalar_list && value_is_f64) {
      value = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (use_tagged_append && value_expr->kind == NY_E_LITERAL &&
        value_expr->as.literal.kind == NY_LIT_INT &&
        value_expr->tok.kind != NY_T_NIL) {
      value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    /*
     * An indexed read of a dynamically classified sequence crosses the
     * accessor boundary as a canonical tagged value (rt_tbuf_index_any_raw).
     * The raw append ABIs store the machine word unchanged, so the tagged
     * encoding would land in the elem-8 slot and double every later
     * consumer's value (2*v+1 stored where v belongs).  The defining
     * instruction is authoritative after lowering: decode exactly once
     * before a raw/scalar append, mirroring the print boundary's fact.
     */
    if (!use_tagged_append && value_expr->kind == NY_E_INDEX) {
      for (size_t ni = b->nyir.len; ni > 0; --ni) {
        const nyr_inst_t *def = &b->nyir.data[ni - 1];
        if (def->dst != value)
          continue;
        /* Producer ABI record: classify by the rt_tbuf_index_any_raw record
         * row (NO_DYNAMIC_BOXING | RESULT_TAGGED_VALUE); the spelling check
         * stays as the fallback for symbols the record does not declare. */
        if (def->op == NYIR_CALL && def->symbol &&
            (ny_native_runtime_abi_decl(def->symbol) ==
                 ny_native_runtime_abi_decl("rt_tbuf_index_any_raw") ||
             strcmp(def->symbol, "rt_tbuf_index_any_raw") == 0)) {
          value =
              nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        break;
      }
    }
    /* f64 NYIR values are IEEE bits, while descriptor slots use the tagged
     * dynamic ABI. Box them before append_any stores the value and tag. */
    if (dynamic_append && (value_is_f64 || value_is_f32)) {
      int f64v = value;
      if (value_is_f32) {
        f64v = nyr_emit_f32_to_f64(b, value);
        if (f64v < 0)
          return -1;
      }
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", f64v, -1, -1, 1, 0);
      value = bits < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1,
                                               -1, 1, 0);
      if (value < 0)
        return -1;
    }
    int out = -1;
    if (!nyr_expr_is_list(b, target)) {
      /*
       * The raw typed-buffer append entry points assume their receiver is
       * already a list. An `any`/inferred receiver must preserve the public
       * append contract and reject non-lists rather than returning a bogus
       * handle or writing through it. rt_append validates the receiver and
       * consumes a tagged value, so encode a proven raw integer here.
       */
      if (!use_tagged_append && append_value_is_raw_integer) {
        value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      out = nyr_emit_runtime_call(b, "rt_append", list, value, -1, 2, 0);
    } else {
      bool append_i64 = !scalar_list && !use_tagged_append &&
                        !value_is_string && append_value_is_raw_integer &&
                        !nyr_expr_is_raw_dynamic_read(b, value_expr);
      /* The inline native append lowering currently loses the live header
       * count when multiple appends are adjacent in one block. Keep the
       * checked runtime helper as the default until the generated-store path
       * preserves that memory dependency. The opt-in exists for focused
       * benchmarking while fixing the fast path. */
      const char *append_fast_env =
          getenv("NYTRIX_ENABLE_NATIVE_LIST_APPEND_FAST");
      bool disable_append_fast =
          !append_fast_env || strcmp(append_fast_env, "1") != 0;
      out = disable_append_fast && (scalar_list || append_i64)
                ? nyr_emit_runtime_call(b, "rt_tbuf_append_i64_raw", list,
                                        value, -1, 2, 0)
            : scalar_list ? nyr_emit_scalar_list_append_fast(
                                b, list, value, nyr_peek_list_len_fact(b, list))
            : append_i64
                ? nyr_emit_dynamic_i64_append_fast(
                      b, list, value,
                      nyr_peek_list_len_fact(b, list) >= 0
                          ? nyr_peek_list_len_fact(b, list)
                          : (target_local && target_local->list_len_slot >= 0
                                 ? nyr_load_local_value(
                                       b, target_local->list_len_slot)
                                 : -1))
                : nyr_emit_runtime_call(
                      b,
                      use_tagged_append ? "rt_tbuf_append_tagged"
                                        : "rt_tbuf_append_raw",
                      list, value, nyr_emit_const(b, value_is_string ? 1 : 0),
                      3, 0);
    }
    int length = nyr_emit_known_list_append_len(b, list, out);
    if (out < 0 || length < 0 || !nyr_record_list_len_fact(b, out, length))
      return -1;
    /* append(list, value) mutates its first argument in place. Keep a local
     * list's companion length synchronized at the mutation site, where the
     * post-append count is known even when fast and reallocating arms merge
     * through a temporary slot. */
    if (e->as.call.args.len >= 1 && e->as.call.args.data[0].val &&
        e->as.call.args.data[0].val->kind == NY_E_IDENT &&
        e->as.call.args.data[0].val->as.ident.name) {
      nyr_local_t *mutated_list =
          nyr_find_local_nearest(b, e->as.call.args.data[0].val->as.ident.name);
      if (mutated_list &&
          (mutated_list->is_list || mutated_list->list_len_slot >= 0)) {
        if (mutated_list->list_len_slot < 0)
          mutated_list->list_len_slot = b->next_local_slot++;
        if (!nyr_store_local_value(b, mutated_list->list_len_slot, length))
          return -1;
      }
    }
    /* The generic append contract permits a compact list to cross into the
     * descriptor representation.  Preserve that representation transition
     * on the receiver for subsequent indexed reads. */
    if (!scalar_list && target && target->kind == NY_E_IDENT &&
        target->as.ident.name) {
      nyr_local_t *updated_target_local =
          nyr_find_local(b, target->as.ident.name);
      if (updated_target_local && updated_target_local->is_list)
        updated_target_local->is_dyn_list = true;
    }
    /* Lowering has just proved and emitted the elem-8 append ABI.  Persist
     * that proof on an identifier receiver so a following indexed loop does
     * not rediscover a fictitious descriptor alternative on every access.
     * This update intentionally happens here (after call selection), because
     * assignment classification runs before lowering the RHS and therefore
     * cannot know which append ABI was actually chosen. */
    if (scalar_list && target && target->kind == NY_E_IDENT &&
        target->as.ident.name) {
      nyr_local_t *updated_target_local =
          nyr_find_local(b, target->as.ident.name);
      if (updated_target_local) {
        updated_target_local->is_list = true;
        updated_target_local->is_dyn_list = false;
        updated_target_local->is_any = false;
        updated_target_local->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
      }
    }
    return out;
  }
  /* The native list-length setter consumes a raw count.  Do not route its
   * second argument through the generic dynamic-call boxing rule: 3 must stay
   * 3, not become the tagged integer 7 in the tbuf header. */
  if (canon_leaf && strcmp(canon_leaf, "__list_set_len") == 0 &&
      e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
      !e->as.call.args.data[1].name) {
    const expr_t *list_expr = e->as.call.args.data[0].val;
    int list = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int length = nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (list < 0 || length < 0)
      return -1;
    int result =
        nyr_emit_runtime_call(b, "rt_list_set_len_raw", list, length, -1, 2, 0);
    if (result < 0)
      return -1;
    /* Keep the local length fact in sync with the raw runtime setter.  A
     * pre-sized list starts with logical length zero; without this update,
     * later typed reads in the same function still bounds-check against zero
     * even though the runtime header now exposes the requested slots. */
    nyr_local_t *updated_list =
        list_expr && list_expr->kind == NY_E_IDENT
            ? nyr_find_local_nearest(b, list_expr->as.ident.name)
            : NULL;
    if (updated_list) {
      updated_list->is_list = true;
      updated_list->is_dyn_list = false;
      updated_list->is_any = false;
      updated_list->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
      if (updated_list->list_len_slot < 0)
        updated_list->list_len_slot = b->next_local_slot++;
      if (!nyr_store_local_value(b, updated_list->list_len_slot, length) ||
          !nyr_record_list_len_fact(b, list, length))
        return -1;
    }
    return result;
  }
  /* A statically known set keeps a raw managed handle in native NYIR.  The
   * generic source `add(any, any)` wrapper expects interpreter-tagged values,
   * so routing this case through that wrapper loses the set representation
   * and silently skips insertion.  Preserve the set ABI at this boundary;
   * scalar and list calls still use their resolved source implementations. */
  if (canon_leaf && strcmp(canon_leaf, "add") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 2 &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
      nyr_expr_is_set(b, e->as.call.args.data[0].val)) {
    int set = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int key = set < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[1].val);
    return set < 0 || key < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_set_add_raw", set, key, -1, 2, 0);
  }
  /* Attached set membership must use the receiver expression directly.
   * Flattening `values.contains(k)` into a generic dotted call loses the
   * top-level set fact: the global load is then decoded as an integer and
   * the raw key is boxed, so neither operand matches the set runtime ABI. */
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.target &&
      e->as.call.callee->as.member.name &&
      (strcmp(e->as.call.callee->as.member.name, "contains") == 0 ||
       strcmp(e->as.call.callee->as.member.name, "has") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_expr_is_set(b, e->as.call.callee->as.member.target)) {
    int set = nyr_lower_expr(b, e->as.call.callee->as.member.target);
    int key = set < 0 ? -1 : nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (key >= 0 && (nyr_expr_is_f64(b, e->as.call.args.data[0].val) ||
                     nyr_value_is_f64(b, key, 0) ||
                     nyr_value_scalar_class(b, key) == NYIR_SCALAR_F64)) {
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", key, -1, -1, 1, 0);
      key = bits < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                             1, 0);
    }
    return set < 0 || key < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_contains_raw", set,
                                                      key, -1, 2, 0);
  }
  if (canon_leaf && !nyr_user_defined_fn(b, name) &&
      strcmp(canon_leaf, "contains") == 0 && e->as.call.args.len == 2 &&
      (!strchr(name, '.') || nyr_expr_is_set(b, e->as.call.args.data[0].val)) &&
      !e->as.call.args.data[0].name && !e->as.call.args.data[1].name) {
    int container = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int item = nyr_lower_expr(b, e->as.call.args.data[1].val);
    if (item >= 0 && (nyr_expr_is_f64(b, e->as.call.args.data[1].val) ||
                      nyr_value_is_f64(b, item, 0) ||
                      nyr_value_scalar_class(b, item) == NYIR_SCALAR_F64)) {
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", item, -1, -1, 1, 0);
      item = bits < 0 ? -1
                      : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                              1, 0);
    }
    return container < 0 || item < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_contains_raw", container, item,
                                       -1, 2, 0);
  }
  /*
   * Free `len(list)`: the semantic resolver qualifies it to core.len,
   * whose polymorphic body dispatches on interpreter runtime tags and
   * returns 0 for a native tbuf handle.  When the argument is known to be
   * a list, lower it directly to the tbuf length (recorded fact or header
   * read) instead of calling through the stdlib body.
   */
  if (canon_leaf && strcmp(canon_leaf, "len") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    const expr_t *target = e->as.call.args.data[0].val;
    bool is_list =
        (nyr_expr_is_list(b, target) || nyr_expr_is_dyn_list(b, target)) &&
        !nyr_expr_is_any(b, target) && !nyr_expr_is_bytes(b, target);
    if (!is_list && target->kind == NY_E_IDENT) {
      const nyr_local_t *local = nyr_find_local(b, target->as.ident.name);
      if (local && !local->is_any)
        is_list = local->is_list;
      if (!is_list) {
        const expr_t *resolved =
            nyr_find_top_level_value(b, target->as.ident.name);
        if (resolved && resolved != target)
          is_list = nyr_expr_is_list(b, resolved);
      }
    }
    int obj = nyr_lower_expr(b, target);
    if (obj < 0)
      return -1;
    if (is_list) {
      int known_len = nyr_peek_list_len_fact(b, obj);
      if (known_len >= 0)
        return known_len;
      return nyr_emit_runtime_call(b, "rt_tbuf_len_raw", obj, -1, -1, 1, 0);
    }
    return nyr_emit_runtime_call(b, "rt_len", obj, -1, -1, 1, 0);
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
      expr_t dotted_target = {.kind = NY_E_IDENT, .tok = e->tok};
      dotted_target.as.ident.name = local_name;
      if (nyr_expr_is_set(b, &dotted_target) &&
          (strcmp(dot + 1, "len") == 0 || strcmp(dot + 1, "contains") == 0 ||
           strcmp(dot + 1, "has") == 0)) {
        int set = nyr_lower_expr(b, &dotted_target);
        if (set < 0)
          return -1;
        if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0)
          return nyr_emit_runtime_call(b, "rt_len", set, -1, -1, 1, 0);
        if (e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
          int key = nyr_lower_expr(b, e->as.call.args.data[0].val);
          if (key >= 0 && (nyr_expr_is_f64(b, e->as.call.args.data[0].val) ||
                           nyr_value_is_f64(b, key, 0) ||
                           nyr_value_scalar_class(b, key) == NYIR_SCALAR_F64)) {
            int bits =
                nyr_emit_runtime_call(b, "rt_f64_bits", key, -1, -1, 1, 0);
            key = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
          }
          return key < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_contains_raw", set, key,
                                                 -1, 2, 0);
        }
      }
      nyr_local_t *canonical_local = nyr_find_local(b, local_name);
      if (!canonical_local) {
        for (size_t li = b->local_count; li > 0; --li) {
          nyr_local_t *candidate = &b->locals[li - 1];
          const char *module = nyr_resolve_use_alias(b, candidate->name);
          if (module && strcmp(module, base_name) == 0) {
            canonical_local = candidate;
            local_name = candidate->name;
            break;
          }
        }
      }
      if (canonical_local &&
          (strcmp(dot + 1, "len") == 0 || strcmp(dot + 1, "contains") == 0 ||
           strcmp(dot + 1, "has") == 0)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = local_name;
        if (nyr_expr_is_set(b, &target)) {
          int set = nyr_lower_expr(b, &target);
          if (set < 0)
            return -1;
          if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0)
            return nyr_emit_runtime_call(b, "rt_len", set, -1, -1, 1, 0);
          if (e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
            int key = nyr_lower_expr(b, e->as.call.args.data[0].val);
            if (key >= 0 &&
                (nyr_expr_is_f64(b, e->as.call.args.data[0].val) ||
                 nyr_value_is_f64(b, key, 0) ||
                 nyr_value_scalar_class(b, key) == NYIR_SCALAR_F64)) {
              int bits =
                  nyr_emit_runtime_call(b, "rt_f64_bits", key, -1, -1, 1, 0);
              key = bits < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                     -1, -1, 1, 0);
            }
            return key < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_contains_raw", set,
                                                   key, -1, 2, 0);
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
        int list = nyr_lower_expr(b, &target);
        int index = nyr_lower_expr(b, e->as.call.args.data[0].val);
        int fallback = e->as.call.args.len == 2
                           ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                           : nyr_emit_const(b, 0);
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
        int got =
            (strcmp(get_symbol, "rt_tbuf_index_any_raw") == 0)
                ? nyr_emit_runtime_call(b, get_symbol, list, index, -1, 2, 0)
                : nyr_emit_runtime_call(b, get_symbol, list, index, fallback, 3,
                                        0);
        bool wants_f64 = !canonical_local->is_dyn_list &&
                         !canonical_local->is_any && e->as.call.args.len == 2 &&
                         nyr_expr_is_f64(b, e->as.call.args.data[1].val);
        return wants_f64 ? nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1,
                                                 -1, 1, NYIR_SCALAR_F64)
                         : got;
      }
      if (canonical_local && canonical_local->type_name &&
          strcmp(canonical_local->type_name, "set") == 0) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = local_name;
        int set = nyr_lower_expr(b, &target);
        if (set < 0)
          return -1;
        if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0)
          return nyr_emit_runtime_call(b, "rt_len", set, -1, -1, 1, 0);
        if ((strcmp(dot + 1, "contains") == 0 || strcmp(dot + 1, "has") == 0) &&
            e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
          int key = nyr_lower_expr(b, e->as.call.args.data[0].val);
          if (key >= 0 && (nyr_expr_is_f64(b, e->as.call.args.data[0].val) ||
                           nyr_value_is_f64(b, key, 0) ||
                           nyr_value_scalar_class(b, key) == NYIR_SCALAR_F64)) {
            int bits =
                nyr_emit_runtime_call(b, "rt_f64_bits", key, -1, -1, 1, 0);
            key = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
          }
          return key < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_contains_raw", set, key,
                                                 -1, 2, 0);
        }
      }
      const expr_t *base_expr = nyr_find_top_level_value(b, base_name);
      const char *global_name = nyr_globaltab_name(base_name);
      if (!global_name && b->current_fn_name) {
        const char *fn_dot = strrchr(b->current_fn_name, '.');
        if (fn_dot && fn_dot[1]) {
          char qualified[512];
          int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                           (int)(fn_dot - b->current_fn_name),
                           b->current_fn_name, base_name);
          if (n > 0 && (size_t)n < sizeof(qualified))
            global_name = nyr_globaltab_name(qualified);
        }
      }
      if (global_name && strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        if (base_expr && nyr_expr_is_list(b, base_expr)) {
          int list = nyr_lower_expr(b, base_expr);
          int index = nyr_lower_expr(b, e->as.call.args.data[0].val);
          int fallback = e->as.call.args.len == 2
                             ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                             : nyr_emit_const(b, 0);
          if (list < 0 || index < 0 || fallback < 0)
            return -1;
          bool dynamic_list = nyr_expr_is_dyn_list(b, base_expr);
          return nyr_emit_runtime_call(
              b, dynamic_list ? "rt_tbuf_get_any" : "rt_tbuf_get", list, index,
              fallback, 3,
              dynamic_list ? NYIR_SCALAR_I64_TAGGED : NYIR_SCALAR_UNKNOWN);
        }
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = nyr_lower_expr(b, &target);
        int key = nyr_lower_dict_key(b, e->as.call.args.data[0].val);
        int fallback = e->as.call.args.len == 2
                           ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                           : nyr_emit_const(b, 0);
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        if (nyr_expr_is_cstr(b, e->as.call.args.data[0].val))
          fallback = nyr_box_dict_fallback(
              b, fallback,
              e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL);
        if (fallback < 0)
          return -1;
        return nyr_emit_runtime_call(
            b,
            nyr_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_dict_get_str_raw"
                : "rt_value_get_tagged",
            dict, key, fallback, 3, NYIR_SCALAR_I64_TAGGED);
      }
      if (global_name && strcmp(dot + 1, "set") == 0 &&
          e->as.call.args.len == 2) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = nyr_lower_expr(b, &target);
        int key = nyr_lower_dict_key(b, e->as.call.args.data[0].val);
        int value = nyr_lower_expr(b, e->as.call.args.data[1].val);
        if (dict < 0 || key < 0 || value < 0)
          return -1;
        return nyr_emit_runtime_call(
            b,
            nyr_expr_is_cstr(b, e->as.call.args.data[0].val)
                ? "rt_native_dict_set_str_compact"
                : "rt_native_dict_set_nir_i64",
            dict, key, value, 3, 0);
      }
      if (base_expr && nyr_expr_is_list(b, base_expr)) {
        if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0) {
          int list = nyr_lower_expr(b, base_expr);
          return list < 0 ? -1
                          : nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list,
                                                  -1, -1, 1, 0);
        }
        if (strcmp(dot + 1, "get") == 0 &&
            (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
          int list = nyr_lower_expr(b, base_expr);
          int index = nyr_lower_expr(b, e->as.call.args.data[0].val);
          int fallback = e->as.call.args.len == 2
                             ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                             : nyr_emit_const(b, 0);
          if (list < 0 || index < 0 || fallback < 0)
            return -1;
          bool dynamic_list = nyr_expr_is_dyn_list(b, base_expr);
          return nyr_emit_runtime_call(
              b, dynamic_list ? "rt_tbuf_get_any" : "rt_tbuf_get", list, index,
              fallback, 3,
              dynamic_list ? NYIR_SCALAR_I64_TAGGED : NYIR_SCALAR_UNKNOWN);
        }
      }
      if (base_expr && nyr_expr_is_dict(b, base_expr) &&
          strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        call_arg_t direct_args[3] = {
            {.name = NULL, .val = (expr_t *)base_expr},
            {.name = NULL, .val = e->as.call.args.data[0].val},
            {.name = NULL,
             .val =
                 e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL},
        };
        expr_t direct_callee = {.kind = NY_E_IDENT, .tok = e->tok};
        direct_callee.as.ident.name = "core.dict_mod.dict_get";
        expr_t direct = *e;
        direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
        direct.semantic.canonical_callee = NULL;
        direct.semantic.canonical_callee_stmt = NULL;
        direct.as.call.callee = &direct_callee;
        direct.as.call.args.data = direct_args;
        direct.as.call.args.len = e->as.call.args.len + 1;
        direct.as.call.args.cap = direct.as.call.args.len;
        return nyr_lower_call(b, &direct);
      }
      if (base_expr) {
        int dict = nyr_lower_expr(b, base_expr);
        if (dict < 0)
          return -1;
        if (strcmp(dot + 1, "len") == 0 && e->as.call.args.len == 0)
          return nyr_emit_runtime_call(b, "rt_len", dict, -1, -1, 1, 0);
        if ((strcmp(dot + 1, "has") == 0 || strcmp(dot + 1, "contains") == 0 ||
             strcmp(dot + 1, "exists") == 0) &&
            e->as.call.args.len == 1) {
          int key = nyr_lower_dict_key(b, e->as.call.args.data[0].val);
          return key < 0 ? -1
                         : nyr_emit_runtime_call(
                               b,
                               nyr_expr_is_cstr(b, e->as.call.args.data[0].val)
                                   ? "rt_dict_has_str_raw"
                                   : "rt_dict_has_raw",
                               dict, key, -1, 2, NYIR_SCALAR_I64_RAW);
        }
        if (strcmp(dot + 1, "get") == 0 &&
            (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
          /* A def-bound dictionary can be changed through an alias; its
           * initializer does not establish the value of a later read. */
          int key = nyr_lower_dict_key(b, e->as.call.args.data[0].val);
          int fallback = e->as.call.args.len == 2
                             ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                             : nyr_emit_const(b, 0);
          if (nyr_expr_is_cstr(b, e->as.call.args.data[0].val))
            fallback = nyr_box_dict_fallback(
                b, fallback,
                e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL);
          return key < 0 || fallback < 0
                     ? -1
                     : nyr_emit_runtime_call(
                           b,
                           nyr_expr_is_cstr(b, e->as.call.args.data[0].val)
                               ? "rt_dict_get_str_raw"
                               : "rt_value_get_tagged",
                           dict, key, fallback, 3, NYIR_SCALAR_I64_TAGGED);
        }
      }
      nyr_local_t *local = nyr_find_local_nearest(b, base_name);
      if (!local && local_name != base_name)
        local = nyr_find_local(b, local_name);
      if (local && local->is_any && strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        const expr_t *receiver = e->as.call.callee->as.member.target;
        if (nyr_expr_is_dyn_list(b, receiver) || local->is_dyn_list) {
          int list = nyr_lower_expr(b, receiver);
          int index = nyr_lower_expr(b, e->as.call.args.data[0].val);
          int fallback = e->as.call.args.len == 2
                             ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                             : nyr_emit_const(b, 0);
          if (list < 0 || index < 0 || fallback < 0)
            return -1;
          if (nyr_value_scalar_class(b, index) == NYIR_SCALAR_I64_TAGGED) {
            index = nyr_emit_runtime_call(b, "rt_any_to_i64", index, -1, -1, 1,
                                          NYIR_SCALAR_I64_RAW);
            if (index < 0)
              return -1;
          }
          return nyr_emit_runtime_call(b, "rt_tbuf_get_any", list, index,
                                       fallback, 3, NYIR_SCALAR_I64_TAGGED);
        }
        call_arg_t direct_args[3] = {
            {.name = NULL, .val = (expr_t *)receiver},
            {.name = NULL, .val = e->as.call.args.data[0].val},
            {.name = NULL,
             .val =
                 e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL},
        };
        expr_t direct_callee = {.kind = NY_E_IDENT, .tok = e->tok};
        direct_callee.as.ident.name = "core.get";
        expr_t direct = *e;
        direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
        direct.semantic.canonical_callee = NULL;
        direct.semantic.canonical_callee_stmt = NULL;
        direct.as.call.callee = &direct_callee;
        direct.as.call.args.data = direct_args;
        direct.as.call.args.len = e->as.call.args.len + 1;
        direct.as.call.args.cap = direct.as.call.args.len;
        return nyr_lower_call(b, &direct);
      }
      if (local && (local->is_list || !local->is_dyn_list) &&
          strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = nyr_lower_expr(b, &target);
        int index = nyr_lower_expr(b, e->as.call.args.data[0].val);
        int fallback = e->as.call.args.len == 2
                           ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                           : nyr_emit_const(b, 0);
        if (list < 0 || index < 0 || fallback < 0)
          return -1;
        int value = nyr_emit_runtime_call(b, "rt_tbuf_get", list, index,
                                          fallback, 3, 0);
        int tag = value < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_tbuf_tag", list,
                                                    index, -1, 2, 0);
        if (value < 0 || tag < 0 ||
            !nyr_record_dyn_fact(b, value, NYR_FACT_DYN_TAG, tag))
          return -1;
        return value;
      }
      if (strcmp(dot + 1, "get") == 0 &&
          (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int dict = nyr_lower_expr(b, &target);
        if (dict >= 0) {
          int key = nyr_lower_dict_key(b, e->as.call.args.data[0].val);
          int fallback = e->as.call.args.len == 2
                             ? nyr_lower_expr(b, e->as.call.args.data[1].val)
                             : nyr_emit_const(b, 0);
          if (key < 0 || fallback < 0)
            return -1;
          if (nyr_expr_is_cstr(b, e->as.call.args.data[0].val))
            fallback = nyr_box_dict_fallback(
                b, fallback,
                e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL);
          if (fallback < 0)
            return -1;
          return nyr_emit_runtime_call(
              b,
              nyr_expr_is_cstr(b, e->as.call.args.data[0].val)
                  ? "rt_dict_get_str_raw"
                  : "rt_value_get_tagged",
              dict, key, fallback, 3, NYIR_SCALAR_I64_TAGGED);
        }
      }
      if (strcmp(dot + 1, "merge") == 0 && e->as.call.args.len == 1) {
        int dict =
            local ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = local->slot})
                  : -1;
        if (dict >= 0) {
          int other = nyr_lower_expr(b, e->as.call.args.data[0].val);
          if (other < 0)
            return -1;
          return nyr_emit_runtime_call(b, "rt_dict_merge_raw", dict, other, -1,
                                       2, nyr_value_scalar_class(b, dict));
        }
      }
    }
  }
  /* Keep dictionary `.get()` on the stdlib implementation.  The inline
   * `rt_dict_get_str_raw` shortcut loses the tagged result representation
   * when a local typed dictionary returns a nested object, so callers see a
   * `ptr` wrapper instead of the object. */
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
      e->as.call.callee->as.member.name &&
      strcmp(e->as.call.callee->as.member.name, "get") == 0 &&
      (e->as.call.args.len == 1 || e->as.call.args.len == 2) &&
      nyr_expr_is_dict(b, e->as.call.callee->as.member.target)) {
    call_arg_t direct_args[3] = {
        {.name = NULL, .val = e->as.call.callee->as.member.target},
        {.name = NULL, .val = e->as.call.args.data[0].val},
        {.name = NULL,
         .val = e->as.call.args.len == 2 ? e->as.call.args.data[1].val : NULL},
    };
    expr_t direct_callee = {.kind = NY_E_IDENT, .tok = e->tok};
    direct_callee.as.ident.name = "core.dict_mod.dict_get";
    expr_t direct = *e;
    direct.semantic.member_call_kind = NY_SEM_CALL_NONE;
    direct.semantic.canonical_callee = NULL;
    direct.semantic.canonical_callee_stmt = NULL;
    direct.as.call.callee = &direct_callee;
    direct.as.call.args.data = direct_args;
    direct.as.call.args.len = e->as.call.args.len + 1;
    direct.as.call.args.cap = direct.as.call.args.len;
    return nyr_lower_call(b, &direct);
  }
  if (dot && dot > name && strcmp(dot + 1, "append") == 0 &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    size_t base_len = (size_t)(dot - name);
    if (base_len < 256) {
      char base_name[256];
      memcpy(base_name, name, base_len);
      base_name[base_len] = '\0';
      nyr_local_t *local = nyr_find_local(b, base_name);
      if (local) {
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base_name;
        int list = nyr_lower_expr(b, &target);
        const expr_t *append_value = e->as.call.args.data[0].val;
        bool value_is_string = nyr_expr_is_cstr(b, append_value);
        bool value_is_f64 = nyr_expr_is_f64(b, append_value);
        bool value_is_async = append_value->kind == NY_E_UNARY &&
                              append_value->as.unary.op &&
                              strcmp(append_value->as.unary.op, "async") == 0;
        bool declared_scalar_list =
            local->is_list && !local->is_any && local->type_name &&
            !ny_native_type_name_is_dyn_list(local->type_name);
        bool structured_append = nyr_expr_is_dict(b, append_value) ||
                                 nyr_expr_is_list(b, append_value) ||
                                 nyr_expr_is_bytes(b, append_value) ||
                                 value_is_async;
        declared_scalar_list = declared_scalar_list && !structured_append;
        bool dynamic_list =
            (local->is_dyn_list && !declared_scalar_list) || structured_append;
        bool saved_value_for_any_call = b->value_for_any_call;
        if (dynamic_list)
          b->value_for_any_call = true;
        int value = nyr_lower_expr(b, append_value);
        b->value_for_any_call = saved_value_for_any_call;
        if (list < 0 || value < 0)
          return -1;
        if (dynamic_list && value_is_f64) {
          int bits =
              nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
          value = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        if (!dynamic_list && value_is_f64) {
          value = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        int out = dynamic_list
                      ? nyr_emit_runtime_call(
                            b, "rt_tbuf_append_tagged", list, value,
                            nyr_emit_const(b, value_is_string ? 1 : 0), 3, 0)
                      : nyr_emit_scalar_list_append_fast(
                            b, list, value, nyr_peek_list_len_fact(b, list));
        int length = nyr_emit_known_list_append_len(b, list, out);
        if (out < 0 || length < 0 || !nyr_record_list_len_fact(b, out, length))
          return -1;
        local->is_list = true;
        if (dynamic_list)
          local->is_dyn_list = true;
        if (declared_scalar_list)
          local->is_dyn_list = false;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        return out;
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
      int list = nyr_lower_expr(b, &target);
      if (list < 0)
        return -1;
      const char *pop_symbol = nyr_expr_is_dyn_list(b, &target)
                                   ? "rt_tbuf_pop_any"
                                   : "rt_tbuf_pop_raw";
      int out = nyr_emit_runtime_call(b, pop_symbol, list, -1, -1, 1, 0);
      if (out < 0)
        return -1;
      int length = nyr_emit_known_list_pop_len(b, list);
      if (length < 0)
        length =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1, -1, 1, 0);
      nyr_local_t *local = nyr_find_local(b, base_name);
      if (length < 0 || !nyr_record_list_len_fact(b, list, length))
        return -1;
      if (local) {
        local->is_list = true;
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        if (!nyr_store_local_value(b, local->list_len_slot, length))
          return -1;
      }
      return out;
    }
  }

  if (canon_leaf && strcmp(canon_leaf, "dict") == 0 &&
      !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len > 1 ||
        (e->as.call.args.len == 1 && e->as.call.args.data[0].name)) {
      return nyr_fail(b,
                      "native NYIR lower: dict accepts an optional capacity"),
             -1;
    }
    int capacity = e->as.call.args.len == 1
                       ? nyr_lower_expr(b, e->as.call.args.data[0].val)
                       : nyr_emit_const(b, 8);
    return capacity < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_dict_new_raw", capacity,
                                                -1, -1, 1, 0);
  }
  /* `set()` / `set(int capacity)` allocates an empty set directly.  Any
   * other single argument (a list or tuple literal, a variable) is the
   * set-from-collection constructor and must fall through to the standard
   * library definition; treating a list word as a capacity corrupts the
   * allocation and the later .len panics on a nil set. */
  if (canon_leaf && strcmp(canon_leaf, "set") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name &&
      (nyr_expr_is_list(b, e->as.call.args.data[0].val) ||
       nyr_expr_is_tuple(b, e->as.call.args.data[0].val))) {
    int sequence = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return sequence < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_set_from_sequence",
                                                sequence, -1, -1, 1, 0);
  }
  if (canon_leaf && strcmp(canon_leaf, "set") == 0 &&
      !nyr_user_defined_fn(b, name) &&
      (e->as.call.args.len == 0 ||
       (e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
        e->as.call.args.data[0].val &&
        e->as.call.args.data[0].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[0].val->as.literal.kind == NY_LIT_INT &&
        e->as.call.args.data[0].val->tok.kind != NY_T_NIL))) {
    int capacity =
        e->as.call.args.len == 1
            ? nyr_emit_const(b, e->as.call.args.data[0].val->as.literal.as.i)
            : nyr_emit_const(b, 8);
    return capacity < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_set_new", capacity, -1, -1, 1, 0);
  }

  if (!nyr_user_defined_fn(b, name) &&
      (strcmp(name, "core.str.replace") == 0 ||
       strcmp(name, "core.replace") == 0 || strcmp(leaf, "str_replace") == 0)) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      return nyr_fail(b, "native NYIR lower: str_replace expects three values"),
             -1;
    }
    int args[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
      const expr_t *arg_expr = e->as.call.args.data[i].val;
      args[i] = nyr_lower_expr(b, arg_expr);
      if (args[i] < 0)
        return -1;
      const char *arg_type = nyr_expr_type_name(b, arg_expr);
      bool managed_string = ny_native_type_name_is_str(arg_type);
      if (!nyr_expr_is_cstr(b, arg_expr) && !managed_string) {
        args[i] = nyr_emit_runtime_call(b,
                                        nyr_expr_is_any(b, arg_expr)
                                            ? "rt_any_to_cstr"
                                            : "rt_i64_to_cstr_raw",
                                        args[i], -1, -1, 1, 0);
        if (args[i] < 0)
          return -1;
      }
    }
    int out = nyr_emit_runtime_call(b, "rt_cstr_replace", args[0], args[1],
                                    args[2], 3, NYIR_SCALAR_I64_TAGGED);
    int length =
        out < 0 ? -1
                : nyr_emit_runtime_call(b, "rt_cstr_len", out, -1, -1, 1, 0);
    int tag = length < 0 ? -1 : nyr_emit_const(b, 121);
    if (out < 0 || length < 0 || tag < 0 ||
        !nyr_record_dyn_fact(b, out, NYR_FACT_DYN_STR_LEN, length) ||
        !nyr_record_dyn_fact(b, out, NYR_FACT_DYN_TAG, tag))
      return -1;
    return out;
  }
  /*
   * core.str is `@inline fn str(any v) str { to_str(v) }`, so routing it
   * through the same formatter dispatch is semantically identical — and
   * avoids compiling the stdlib body, which would hit the any-parameter ABI
   * path.  A user-defined (non-stdlib) `str` skips this via the guard below.
   */
  if ((strcmp(leaf, "str") == 0 || strcmp(leaf, "to_str") == 0 ||
       strcmp(leaf, "__to_str") == 0) &&
      (!nyr_user_defined_fn(b, name) || strcmp(leaf, "to_str") == 0 ||
       strcmp(leaf, "__to_str") == 0)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b,
                      "native NYIR lower: to_str expects one positional value"),
             -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    if (nyr_expr_is_immutable_nil(b, arg_expr, 0))
      return nyr_emit_cstr_const(b, "nil");
    int arg = nyr_lower_expr(b, arg_expr);
    if (arg < 0)
      return -1;
    /* The loaded register class is stronger than a stale contextual type on
     * the AST node. A reused local can carry an old numeric annotation even
     * though its current reaching value is already a native C-string pointer;
     * passing that pointer through the f64 formatter corrupts the value and
     * fails the runtime-call ABI verifier. */
    nyr_scalar_class_t actual_arg_class = nyr_value_scalar_class(b, arg);
    if (actual_arg_class == NYIR_SCALAR_STRING)
      return arg;
    if (nyr_expr_is_cstr(b, arg_expr))
      return arg;
    /*
     * Route by the argument's actual kind: bools and floats must not go
     * through rt_i64_to_cstr_raw (which would print their raw bits as an
     * integer).  f32 is widened to f64 first; everything else falls through
     * to the tagged-any or i64 formatter.
     */
    if (nyr_expr_is_bool(b, arg_expr))
      return nyr_emit_runtime_call(b, "rt_bool_to_cstr", arg, -1, -1, 1, 0);
    if (nyr_expr_is_bigfloat(b, arg_expr))
      return nyr_emit_runtime_call(b, "rt_bigfloat_to_str", arg, -1, -1, 1, 0);
    /* BigInt handles take precedence over conservative numeric inference;
     * operator expressions can otherwise be mislabeled as f64.  This native
     * formatter receives the typed/raw ABI value, so pass the opaque handle
     * to the handle-aware bridge rather than pretending it is a canonical
     * tagged-any word. */
    if (nyr_expr_is_bigint(b, arg_expr))
      return nyr_emit_runtime_call(b, "rt_bigint_to_cstr_raw", arg, -1, -1, 1,
                                   0);
    /* Native integer operators can retain a conservative `any` semantic
     * annotation even when their operand proof and emitted ABI are raw. Route
     * those values to the raw formatter before consulting the source-level
     * dynamic category; otherwise raw zero is interpreted as tagged nil. */
    if (nyr_expr_is_raw_scalar(b, arg_expr, 0))
      return nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", arg, -1, -1, 1, 0);
    const char *arg_type = nyr_expr_type_name(b, arg_expr);
    if (arg_type && strcmp(arg_type, "bigint") == 0)
      return nyr_emit_runtime_call(b, "rt_bigint_to_cstr_raw", arg, -1, -1, 1,
                                   0);
    if (nyr_expr_is_f32(b, arg_expr)) {
      int f64 = nyr_emit_f32_to_f64(b, arg);
      if (f64 < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", f64, -1, -1, 1, 0);
    }
    /* Semantic inference can conservatively call a dynamic call result
     * "number" while the actual NYIR producer is a tagged word. Dispatch on
     * the lowered register class at this boundary, otherwise the formatter
     * receives a tagged integer/handle as raw IEEE bits. */
    nyr_scalar_class_t lowered_arg_class = actual_arg_class;
    if (lowered_arg_class == NYIR_SCALAR_I64_TAGGED)
      return nyr_emit_runtime_call(b, "rt_any_to_cstr", arg, -1, -1, 1, 0);
    if (nyr_expr_is_f64(b, arg_expr) || lowered_arg_class == NYIR_SCALAR_F64)
      return nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", arg, -1, -1, 1, 0);
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
      return nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", arg, -1, -1, 1, 0);
    return nyr_emit_runtime_call(b,
                                 nyr_expr_may_dynamic_value(b, arg_expr, 0)
                                     ? "rt_any_to_cstr"
                                     : "rt_i64_to_cstr_raw",
                                 arg, -1, -1, 1, 0);
  }
  /*
   * Whether a builtin name resolves to any declaration (user function,
   * import, local, or top-level binding).  Mirrors the unresolved-bridge
   * rule in comparison lowering: an unresolved name keeps the raw machine
   * convention on both sides, so tagging a raw builtin result would compare
   * tagged against raw (clamp(15, 0, 10) == 10 failed).  Resolved dynamic
   * callees keep the tagged contract instead.
   */
  bool builtin_callee_resolved = false;
  {
    const char *cname = ny_native_call_leaf(e);
    if (cname && *cname &&
        (nyr_user_defined_fn(b, cname) ||
         nyr_find_imported_function(b, cname) || nyr_find_local(b, cname) ||
         nyr_find_top_level_value(b, cname)))
      builtin_callee_resolved = true;
  }
  /* The exact callee identity decides whether a user definition shadows the
   * builtin. A leaf-only lookup sees unrelated imported `abs` functions and
   * incorrectly disables this lane-polymorphic intrinsic. */
  if (!nyr_user_defined_fn(b, name) && canon_leaf &&
      strcmp(canon_leaf, "abs") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: abs expects one value"), -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    int value = nyr_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
    bool value_f32 = value_class == NYIR_SCALAR_F32;
    bool value_f64 = value_class == NYIR_SCALAR_F64;
    bool use_f64 = nyr_expr_is_f64(b, arg_expr) || value_f32 || value_f64;
    const char *value_class_name =
        value_class == NYIR_SCALAR_I64_RAW      ? "i64-raw"
        : value_class == NYIR_SCALAR_I64_TAGGED ? "i64-tagged"
        : value_class == NYIR_SCALAR_F32        ? "f32"
        : value_class == NYIR_SCALAR_F64        ? "f64"
        : value_class == NYIR_SCALAR_STRING     ? "string"
        : value_class == NYIR_SCALAR_POINTER    ? "pointer"
        : value_class == NYIR_SCALAR_OBJECT     ? "object"
                                                : "unknown";
    const char *normalization =
        value_f64                                          ? "preserve-f64"
        : value_f32                                        ? "f32-to-f64"
        : use_f64 && value_class == NYIR_SCALAR_I64_TAGGED ? "tagged-to-f64"
        : use_f64                                          ? "i64-to-f64"
        : value_class == NYIR_SCALAR_I64_TAGGED            ? "tagged-to-i64"
                                                           : "preserve-i64";
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "native-arithmetic",
          "function=%s intrinsic=abs decision=%s producer_class=%s "
          "ast_f64=%d "
          "ast_any=%d normalization=%s",
          b->current_fn_name ? b->current_fn_name : "<entry>",
          use_f64 ? "f64" : "i64", value_class_name,
          (int)nyr_expr_is_f64(b, arg_expr), (int)nyr_expr_is_any(b, arg_expr),
          normalization);
    /*
     * Same normalization as min/max below: the raw helpers consume
     * untagged words, so a dynamically-typed operand must go through the
     * any bridge first (abs of a tagged value otherwise compares raw
     * bits).
     */
    if (nyr_expr_is_any(b, arg_expr) && !value_f32 && !value_f64) {
      value = nyr_emit_runtime_call(
          b, use_f64 ? "rt_any_to_f64" : "rt_any_to_i64", value, -1, -1, 1,
          use_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW);
      if (value < 0)
        return -1;
      value_class = use_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW;
    }
    if (use_f64 && value_class != NYIR_SCALAR_F64) {
      value = value_class == NYIR_SCALAR_F32 ? nyr_emit_f32_to_f64(b, value)
              : value_class == NYIR_SCALAR_I64_TAGGED
                  ? nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                          NYIR_SCALAR_F64)
                  : nyr_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    int zero = use_f64 ? nyr_emit_const_f64(b, 0.0) : nyr_emit_const(b, 0);
    int neg = use_f64 ? nyr_push_val(b, NYIR_SUB_F64, zero, value, 0, NULL)
                      : nyr_push_val(b, NYIR_SUB_I64, zero, value, 0, NULL);
    if (zero < 0 || neg < 0)
      return -1;
    int abs_result = nyr_emit_runtime_call(
        b, use_f64 ? "rt_f64_max" : "rt_i64_max", value, neg, -1, 2,
        use_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW);
    if (abs_result < 0)
      return -1;
    /*
     * Same producer contract as min/max below: an `any`-typed call site
     * observes tagged values, so re-tag the raw helper result (abs(0 - 7)
     * printed 3: raw 7 rendered as tagged).  The f64 lane keeps raw IEEE
     * bits like the min/max f64 lane does.
     */
    /*
     * Tag only for resolved declarations: an unresolved builtin fallback
     * (no import) keeps the raw convention that ==/print apply to it;
     * tagging it made clamp(15, 0, 10) == 10 compare tagged against raw.
     */
    /*
     * Tag only when semantic resolution established the any-contract.
     * Unresolved builtin fallbacks (no import) keep the raw convention
     * that ==/print apply to them; tagging those made clamp(15, 0, 10)
     * == 10 compare tagged against raw.
     */
    if (!use_f64 && nyr_expr_is_any(b, e))
      return nyr_emit_runtime_call(b, "rt_tag", abs_result, -1, -1, 1, 0);
    return abs_result;
  }
  if (!nyr_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "clamp") == 0 || strcmp(canon_leaf, "lerp") == 0)) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      return nyr_fail(b, "native NYIR lower: %s expects three values", leaf),
             -1;
    }
    const expr_t *a_expr = e->as.call.args.data[0].val;
    const expr_t *b_expr = e->as.call.args.data[1].val;
    const expr_t *c_expr = e->as.call.args.data[2].val;
    int a = nyr_lower_expr(b, a_expr);
    int b_arg = nyr_lower_expr(b, b_expr);
    int c_arg = nyr_lower_expr(b, c_expr);
    if (a < 0 || b_arg < 0 || c_arg < 0)
      return -1;
    bool use_f64 = nyr_expr_is_f64(b, a_expr) || nyr_expr_is_f64(b, b_expr) ||
                   nyr_expr_is_f64(b, c_expr);
    /*
     * Normalize dynamically-typed operands before the raw helpers, as in
     * min/max below: clamp/lerp of tagged values otherwise compute on raw
     * bits (clamp(m, 0, 10) with m = max(1, 0) yielded 3).
     */
    bool a_any = nyr_expr_is_any(b, a_expr);
    bool b_any = nyr_expr_is_any(b, b_expr);
    bool c_any = nyr_expr_is_any(b, c_expr);
    if (use_f64) {
      if (a_any) {
        a = nyr_emit_runtime_call(b, "rt_any_to_f64", a, -1, -1, 1,
                                  NYIR_SCALAR_F64);
      } else if (!nyr_expr_is_f64(b, a_expr)) {
        a = nyr_emit_i64_to_f64(b, a);
      }
      if (b_any) {
        b_arg = nyr_emit_runtime_call(b, "rt_any_to_f64", b_arg, -1, -1, 1,
                                      NYIR_SCALAR_F64);
      } else if (!nyr_expr_is_f64(b, b_expr)) {
        b_arg = nyr_emit_i64_to_f64(b, b_arg);
      }
      if (c_any) {
        c_arg = nyr_emit_runtime_call(b, "rt_any_to_f64", c_arg, -1, -1, 1,
                                      NYIR_SCALAR_F64);
      } else if (!nyr_expr_is_f64(b, c_expr)) {
        c_arg = nyr_emit_i64_to_f64(b, c_arg);
      }
      if (a < 0 || b_arg < 0 || c_arg < 0)
        return -1;
      if (strcmp(canon_leaf, "lerp") == 0) {
        int diff = nyr_push_val(b, NYIR_SUB_F64, b_arg, a, 0, NULL);
        int scaled =
            diff < 0 ? -1 : nyr_push_val(b, NYIR_MUL_F64, diff, c_arg, 0, NULL);
        return scaled < 0 ? -1
                          : nyr_push_val(b, NYIR_ADD_F64, a, scaled, 0, NULL);
      }
      int lo = nyr_emit_runtime_call(b, "rt_f64_max", a, b_arg, -1, 2,
                                     NYIR_SCALAR_F64);
      return lo < 0 ? -1
                    : nyr_emit_runtime_call(b, "rt_f64_min", lo, c_arg, -1, 2,
                                            NYIR_SCALAR_F64);
    }
    /*
     * Integer clamp/lerp consume raw words: normalize dynamic operands
     * first (same tagged-bits hazard as min/max above).
     */
    if (a_any) {
      a = nyr_emit_runtime_call(b, "rt_any_to_i64", a, -1, -1, 1, 0);
    }
    if (b_any) {
      b_arg = nyr_emit_runtime_call(b, "rt_any_to_i64", b_arg, -1, -1, 1, 0);
    }
    if (c_any) {
      c_arg = nyr_emit_runtime_call(b, "rt_any_to_i64", c_arg, -1, -1, 1, 0);
    }
    if (a < 0 || b_arg < 0 || c_arg < 0)
      return -1;
    if (strcmp(canon_leaf, "lerp") == 0) {
      int diff = nyr_push_val(b, NYIR_SUB_I64, b_arg, a, 0, NULL);
      int scaled =
          diff < 0 ? -1 : nyr_push_val(b, NYIR_MUL_I64, diff, c_arg, 0, NULL);
      int lerp_result =
          scaled < 0 ? -1 : nyr_push_val(b, NYIR_ADD_I64, a, scaled, 0, NULL);
      if (lerp_result < 0)
        return -1;
      if (nyr_expr_is_any(b, e) && builtin_callee_resolved)
        return nyr_emit_runtime_call(b, "rt_tag", lerp_result, -1, -1, 1, 0);
      return lerp_result;
    }
    int lo = nyr_emit_runtime_call(b, "rt_i64_max", a, b_arg, -1, 2,
                                   NYIR_SCALAR_I64_RAW);
    if (lo < 0)
      return -1;
    int clamp_result = nyr_emit_runtime_call(b, "rt_i64_min", lo, c_arg, -1, 2,
                                             NYIR_SCALAR_I64_RAW);
    if (clamp_result < 0)
      return -1;
    if (nyr_expr_is_any(b, e) && builtin_callee_resolved)
      return nyr_emit_runtime_call(b, "rt_tag", clamp_result, -1, -1, 1, 0);
    /* The generic `number` result annotation does not erase the concrete
     * integer lane selected from these operands. Keep the call-site fact in
     * step with the helper result so consumers such as print use the integer
     * representation instead of reinterpreting it as f64 bits. */
    ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
    ((expr_t *)e)->semantic.resolved = true;
    return clamp_result;
  }
  if (!nyr_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "min") == 0 || strcmp(canon_leaf, "max") == 0)) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      return nyr_fail(b, "native NYIR lower: min/max expects two values"), -1;
    }
    const expr_t *left_expr = e->as.call.args.data[0].val;
    const expr_t *right_expr = e->as.call.args.data[1].val;
    bool use_f64 =
        nyr_expr_is_f64(b, left_expr) || nyr_expr_is_f64(b, right_expr);
    int left = nyr_lower_expr(b, left_expr);
    int right = nyr_lower_expr(b, right_expr);
    if (left < 0 || right < 0)
      return -1;
    nyr_scalar_class_t left_class = nyr_value_scalar_class(b, left);
    nyr_scalar_class_t right_class = nyr_value_scalar_class(b, right);
    /* Contextual inference can call an integer-looking expression `number`
     * even when lowering emitted machine-float arithmetic. The runtime helper
     * ABI must follow the actual values, or rt_i64_min/max consumes f64 bits
     * as an integer argument. */
    use_f64 = use_f64 || left_class == NYIR_SCALAR_F64 ||
              left_class == NYIR_SCALAR_F32 || right_class == NYIR_SCALAR_F64 ||
              right_class == NYIR_SCALAR_F32;
    /*
     * The raw helpers consume untagged words, but dynamically-typed
     * operands (call results, untyped locals) arrive tagged: feeding them
     * raw made min(max(1, 0), 100000000) evaluate to 3 (tagged 1 is 3).
     * Normalize per the operand's declared representation instead of the
     * `rt_` spelling: dynamic sides through the any bridges, statically
     * known sides as before.
     */
    bool left_any = nyr_expr_is_any(b, left_expr);
    bool right_any = nyr_expr_is_any(b, right_expr);
    if (use_f64) {
      if (left_class == NYIR_SCALAR_F64) {
        /* Already in the helper's machine-f64 ABI. */
      } else if (left_class == NYIR_SCALAR_F32) {
        left = nyr_emit_f32_to_f64(b, left);
      } else if (left_class == NYIR_SCALAR_I64_TAGGED || left_any) {
        left = nyr_emit_runtime_call(b, "rt_any_to_f64", left, -1, -1, 1,
                                     NYIR_SCALAR_F64);
      } else {
        left = nyr_emit_i64_to_f64(b, left);
      }
      if (right_class == NYIR_SCALAR_F64) {
        /* Already in the helper's machine-f64 ABI. */
      } else if (right_class == NYIR_SCALAR_F32) {
        right = nyr_emit_f32_to_f64(b, right);
      } else if (right_class == NYIR_SCALAR_I64_TAGGED || right_any) {
        right = nyr_emit_runtime_call(b, "rt_any_to_f64", right, -1, -1, 1,
                                      NYIR_SCALAR_F64);
      } else {
        right = nyr_emit_i64_to_f64(b, right);
      }
      if (left < 0 || right < 0)
        return -1;
      return nyr_emit_runtime_call(
          b, strcmp(canon_leaf, "min") == 0 ? "rt_f64_min" : "rt_f64_max", left,
          right, -1, 2, NYIR_SCALAR_F64);
    }
    if (left_any) {
      left = nyr_emit_runtime_call(b, "rt_any_to_i64", left, -1, -1, 1, 0);
    }
    if (right_any) {
      right = nyr_emit_runtime_call(b, "rt_any_to_i64", right, -1, -1, 1, 0);
    }
    if (left < 0 || right < 0)
      return -1;
    int result = nyr_emit_runtime_call(
        b, strcmp(canon_leaf, "min") == 0 ? "rt_i64_min" : "rt_i64_max", left,
        right, -1, 2, NYIR_SCALAR_I64_RAW);
    if (result < 0)
      return -1;
    /* The source contract of min/max is `any`; the raw helper is only an
     * implementation detail.  Restore the canonical dynamic ABI whenever
     * the expression is dynamic, including the std root facade where the
     * imported declaration is not found by the local name lookup. */
    if (nyr_expr_is_any(b, e))
      return nyr_emit_runtime_call(b, "rt_tag", result, -1, -1, 1, 0);
    ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
    ((expr_t *)e)->semantic.resolved = true;
    return result;
  }
  if (canon_leaf && strcmp(canon_leaf, "int") == 0 &&
      !nyr_user_defined_fn(b, name) && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name &&
      nyr_expr_is_f64(b, e->as.call.args.data[0].val)) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    int result = nyr_emit_runtime_call(b, "rt_f64_to_i64", value, -1, -1, 1,
                                       NYIR_SCALAR_I64_RAW);
    if (result >= 0) {
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
      ((expr_t *)e)->semantic.resolved = true;
    }
    return result;
  }
  if (leaf && !nyr_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "__flt_sin") == 0 ||
       strcmp(canon_leaf, "__flt_cos") == 0)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b,
                      "native NYIR lower: %s requires one positional argument",
                      leaf),
             -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = nyr_lower_f64_arg(b, arg);
    if (value < 0)
      return -1;
    int result = -1;
    if (b->options && b->options->native_backend == NY_NATIVE_BACKEND_X86_64)
      result = nyr_push_val(
          b, strcmp(canon_leaf, "__flt_sin") == 0 ? NYIR_SIN_F64 : NYIR_COS_F64,
          value, -1, 0, NULL);
    else
      result = nyr_emit_runtime_call(
          b, strcmp(canon_leaf, "__flt_sin") == 0 ? "rt_sin_f64" : "rt_cos_f64",
          value, -1, -1, 1, NYIR_SCALAR_F64);
    if (result >= 0) {
      /* Native-only lowering can bypass the type pass that records these
       * builtin return types. Publish the f64 result fact at the call site so
       * a local initialized from the intrinsic remains unboxed downstream. */
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_F64;
      ((expr_t *)e)->semantic.resolved = true;
    }
    return result;
  }
  /* C math headers do not consistently expose the return representation to
   * the lightweight FFI parser.  Normalize the common scalar entry points to
   * the runtime f64 ABI before generic external-call lowering; otherwise the
   * callee returns in XMM0 while the caller reads an integer register. */
  if (leaf && !nyr_user_defined_fn(b, name) &&
      (strcmp(canon_leaf, "sin") == 0 || strcmp(canon_leaf, "cos") == 0 ||
       strcmp(canon_leaf, "sqrt") == 0) &&
      e->as.call.args.len == 1 && !e->as.call.args.data[0].name) {
    int value = nyr_lower_f64_arg(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    if (strcmp(canon_leaf, "sqrt") == 0)
      return nyr_push_val(b, NYIR_SQRT_F64, value, -1, 0, NULL);
    const char *runtime =
        strcmp(canon_leaf, "sin") == 0 ? "rt_sin_f64" : "rt_cos_f64";
    return nyr_emit_runtime_call(b, runtime, value, -1, -1, 1, NYIR_SCALAR_F64);
  }
  if (canon_leaf && !nyr_user_defined_fn(b, name)) {
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
        return nyr_fail(b, "native NYIR lower: %s expects %d positional values",
                        canon_leaf, c_argc),
               -1;
      }
      int c_args[3] = {-1, -1, -1};
      for (int i = 0; i < c_argc; ++i) {
        if (e->as.call.args.data[i].name) {
          return nyr_fail(b, "native NYIR lower: %s expects positional values",
                          canon_leaf),
                 -1;
        }
        c_args[i] = nyr_lower_expr(b, e->as.call.args.data[i].val);
        if (c_args[i] < 0)
          return -1;
      }
      /* The native runtime-call table contains the raw ABI bridge, not an
       * unresolved libc function pointer.  In particular, emitting the
       * surface spelling `memcmp` here leaves a null target in LLVM-free
       * native images when the call is reached through an imported module. */
      const char *runtime_memory_symbol =
          strcmp(canon_leaf, "memcmp") == 0 ? "rt_memcmp_raw" : c_symbol;
      return nyr_emit_runtime_call(b, runtime_memory_symbol, c_args[0],
                                   c_args[1], c_args[2], c_argc, 0);
    }
  }

  nyr_local_t *named_local = nyr_find_local(b, name);
  const expr_t *named_global = name ? nyr_find_top_level_value(b, name) : NULL;
  bool named_callable_alias =
      (named_local &&
       (named_local->callable_expr || named_local->callable_name)) ||
      (named_global && named_global->kind == NY_E_IDENT &&
       named_global->as.ident.name &&
       nyr_find_user_function(b, named_global->as.ident.name));
  if (name && named_local && !named_callable_alias &&
      !nyr_user_defined_fn(b, name)) {
    /* An untyped callable parameter is a raw closure/function value in the
     * native ABI.  Preserve the indirect call instead of treating the local
     * as an ordinary value binding (the old zero result made thunk() a
     * no-op). rt_call0 understands both raw code pointers and heap closures.
     */
    if ((named_local->is_any ||
         named_local->semantic_rep == NY_SEM_REP_CLOSURE) &&
        e->as.call.args.len == 0) {
      int callable = nyr_lower_expr(b, e->as.call.callee);
      int result = callable < 0 ? -1
                                : nyr_emit_runtime_call(b, "rt_call0", callable,
                                                        -1, -1, 1, 0);
      if (getenv("NY_DEBUG_FNPTR"))
        fprintf(stderr, "[call0] name=%s rep=%d res=%d sem=%d\n", name,
                (int)named_local->semantic_rep, result, (int)e->semantic.rep);
      /* rt_call0 crosses the dynamic callback ABI and returns a canonical
       * tagged value. Restore the statically inferred scalar representation
       * here just as the general indirect-call path does below. */
      if (result >= 0 && e->semantic.resolved &&
          e->semantic.rep == NY_SEM_REP_RAW_INT)
        result =
            nyr_emit_runtime_call(b, "rt_any_to_i64", result, -1, -1, 1, 0);
      else if (result >= 0 && e->semantic.resolved &&
               e->semantic.rep == NY_SEM_REP_F64)
        result = nyr_emit_runtime_call(b, "rt_any_to_f64", result, -1, -1, 1,
                                       NYIR_SCALAR_F64);
      return result;
    }
    /* Non-zero-argument callable locals continue into the general indirect
     * closure path below; returning zero here silently erased `f(x)` calls in
     * functions with an untyped/fnptr parameter. */
    if (!(named_local->is_any ||
          named_local->semantic_rep == NY_SEM_REP_CLOSURE ||
          (named_global && named_global->kind == NY_E_CALL)))
      return nyr_emit_const(b, 0);
  }
  if (!nyr_user_defined_fn(b, name) && canon_leaf &&
      strcmp(canon_leaf, "panic") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(
                 b, "native NYIR lower: panic expects one positional argument"),
             -1;
    }
    int message = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int tagged = message < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_alloc_string",
                                                     message, -1, -1, 1, 0);
    if (tagged < 0 ||
        nyr_emit_runtime_call(b, "rt_panic", tagged, -1, -1, 1, 0) < 0)
      return -1;
    return nyr_emit_const(b, 0);
  }
  ny_native_leaf_kind_t proof_kind = ny_native_leaf_kind(canon_leaf);
  if (!nyr_user_defined_fn(b, name) &&
      (proof_kind == NY_NATIVE_LEAF_PROVE ||
       proof_kind == NY_NATIVE_LEAF_STATIC_ASSERT ||
       proof_kind == NY_NATIVE_LEAF_ASSERT_COMPILE)) {
    if (e->as.call.args.len == 0) {
      return nyr_fail_at(b, e->tok,
                         "native NYIR: %s expects a compile-time condition",
                         canon_leaf),
             -1;
    }
    const expr_t *condition = e->as.call.args.data[0].val;
    if (condition && ny_proof_has_wide_integer((expr_t *)condition)) {
      return nyr_fail_at(b, condition->tok,
                         "%s: integer literal is outside the signed-i64 proof "
                         "fragment",
                         canon_leaf),
             -1;
    }
    const char *strat = NULL;
    if (e->as.call.args.len >= 2 && e->as.call.args.data[1].val &&
        e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR &&
        e->as.call.args.data[1].val->as.literal.as.s.data) {
      strat = e->as.call.args.data[1].val->as.literal.as.s.data;
    }
    char policy_reason[NY_PROOF_POLICY_EXPLAIN_MAX];
    if (strat && !ny_proof_policy_validate_text(strat, policy_reason,
                                                sizeof(policy_reason))) {
      return nyr_fail_at(b, e->as.call.args.data[1].val->tok,
                         "%s: malformed proof strategy: %s", canon_leaf,
                         policy_reason),
             -1;
    }
    int64_t proof_value = 0;
    bool exact_proof_value =
        condition && nyr_eval_proof_i64(b, condition, 0, &proof_value);
    if (!exact_proof_value && proof_kind == NY_NATIVE_LEAF_PROVE)
      exact_proof_value =
          nyr_eval_proof_lemma_call(b, condition, 0, &proof_value, NULL);
    /* A lemma invocation carrying an ordinary runtime call is not a
     * compile-time witness. Keep the native proof path aligned with the
     * legacy checker instead of hashing the unresolved proposition as if it
     * were already established. */
    if (proof_kind == NY_NATIVE_LEAF_PROVE && condition &&
        condition->kind == NY_E_CALL && condition->as.call.callee &&
        condition->as.call.args.len > 0) {
      for (size_t ai = 0; ai < condition->as.call.args.len; ++ai) {
        const expr_t *arg = condition->as.call.args.data[ai].val;
        if (arg && (arg->kind == NY_E_CALL || arg->kind == NY_E_MEMCALL)) {
          return nyr_fail_at(b, condition->tok,
                             "lemma could not prove the supplied "
                             "arguments: arguments must "
                             "be compile-time values or proven ranges"),
                 -1;
        }
      }
    }
    if (strat && strstr(strat, "by_fold") && !strstr(strat, "by_range") &&
        !exact_proof_value) {
      return nyr_fail_at(b, condition ? condition->tok : e->tok,
                         "%s: obligation failed under strategy '%s'",
                         canon_leaf, strat),
             -1;
    }
    if (exact_proof_value && proof_value == 0) {
      const char *message = proof_kind == NY_NATIVE_LEAF_PROVE
                                ? "proof obligation failed"
                                : "static assertion failed";
      if (strat)
        message = strat;
      return nyr_fail_at(b, condition ? condition->tok : e->tok, "%s", message),
             -1;
    }
    if (proof_kind == NY_NATIVE_LEAF_PROVE && !exact_proof_value && condition) {
      /*
       * A proof witness must be compile-time exact. A condition that cannot
       * be evaluated here can never be discharged: minting a digest for it
       * would certify unproven (possibly false) claims, e.g. deep recursion
       * past evaluator limits or comptime blocks that exhausted fuel and
       * fell back to runtime lowering. Fail closed with a located
       * diagnostic. This sits after the specific checks above so lemma
       * arguments, strategy violations, and refuted obligations keep their
       * own messages.
       */
      return nyr_fail_at(b, condition->tok,
                         "prove condition must be known at compile time"),
             -1;
    }
    /* A proof witness is represented by the same canonical proposition digest
     * used by the legacy backend.  This keeps proof_matches meaningful in
     * native code while static_assert/assert_compile remain erased. */
    if (proof_kind == NY_NATIVE_LEAF_PROVE && e->as.call.args.len >= 1 &&
        e->as.call.args.data[0].val) {
      char *proof_type =
          ny_proof_type_from_expr((expr_t *)e->as.call.args.data[0].val);
      if (proof_type) {
        uint64_t digest = ny_hash64_cstr(proof_type);
        free(proof_type);
        return nyr_emit_const(b, (int64_t)digest);
      }
    }
    if (ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_ASSERT_COMPILE &&
        e->as.call.args.len >= 1 && e->as.call.args.data[0].val &&
        e->as.call.args.data[0].val->kind == NY_E_CALL) {
      const expr_t *nested = e->as.call.args.data[0].val;
      const char *nested_leaf = ny_native_call_leaf(nested);
      if (ny_native_leaf_kind(nested_leaf) == NY_NATIVE_LEAF_RANGE_PROVEN &&
          nested->as.call.args.len == 3) {
        int64_t value = 0, lo = 0, hi = 0;
        bool exact =
            nyr_eval_proof_i64(b, nested->as.call.args.data[0].val, 0,
                               &value) &&
            nyr_eval_proof_i64(b, nested->as.call.args.data[1].val, 0, &lo) &&
            nyr_eval_proof_i64(b, nested->as.call.args.data[2].val, 0, &hi);
        if (exact && (lo > hi || value < lo || value > hi)) {
          return nyr_fail(b, "native NYIR: assert_compile failed for "
                             "range_proven"),
                 -1;
        }
      }
    }
    return nyr_emit_const(b, 0);
  }
  if (!nyr_user_defined_fn(b, name) &&
      ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_PROOF_MATCHES) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      return nyr_fail(b, "native NYIR: proof_matches expects "
                         "witness and proposition"),
             -1;
    }
    const expr_t *witness = e->as.call.args.data[0].val;
    const expr_t *init =
        witness && witness->kind == NY_E_IDENT
            ? nyr_find_top_level_value(b, witness->as.ident.name)
            : NULL;
    uint64_t actual = 0;
    bool have_actual = false;
    if (init && init->kind == NY_E_CALL && init->as.call.args.len >= 1 &&
        init->as.call.args.data[0].val) {
      const char *wleaf = ny_native_call_leaf(init);
      if (ny_native_leaf_kind(wleaf) == NY_NATIVE_LEAF_PROVE) {
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
      if (ny_native_leaf_kind(wleaf) == NY_NATIVE_LEAF_PROVE) {
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
    return nyr_emit_const(b, matches ? NY_IMM_TRUE : NY_IMM_FALSE);
  }
  if (leaf && strcmp(leaf, "__tagof") == 0 && !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: __tagof expects one value"), -1;
    }
    const expr_t *arg_expr = e->as.call.args.data[0].val;
    if (arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      nyr_local_t *local = nyr_find_local_nearest(b, arg_expr->as.ident.name);
      /* An `any` parameter carries the authoritative runtime tag in its
       * companion ABI slot.  The identifier's semantic annotation may have
       * been resolved in the caller's lexical environment (notably when a
       * top-level `a` shadows a parameter named `a`), so it cannot override
       * the local ABI here. */
      if (local && local->is_any && local->dyn_tag_slot >= 0) {
        int tag = nyr_load_local_value(b, local->dyn_tag_slot);
        if (tag >= 0)
          return tag;
      }
    }
    bool known_container =
        nyr_expr_is_list(b, arg_expr) || nyr_expr_is_dict(b, arg_expr) ||
        nyr_expr_is_set(b, arg_expr) || nyr_expr_is_tuple(b, arg_expr) ||
        nyr_expr_is_range(b, arg_expr) || nyr_expr_is_bytes(b, arg_expr);
    if (!known_container && nyr_expr_has_raw_int_abi(b, arg_expr, 0))
      return nyr_emit_const(b, 1);
    if (arg_expr && arg_expr->semantic.resolved && !known_container) {
      if (arg_expr->semantic.rep == NY_SEM_REP_RAW_INT)
        return nyr_emit_const(b, 1);
      if (arg_expr->semantic.rep == NY_SEM_REP_F64 ||
          arg_expr->semantic.rep == NY_SEM_REP_F32)
        return nyr_emit_const(b, TAG_FLOAT);
      if (arg_expr->semantic.rep == NY_SEM_REP_STRING)
        return nyr_emit_const(b, 121);
      if (arg_expr->semantic.rep == NY_SEM_REP_CLOSURE)
        return nyr_emit_const(b, 122);
    }
    int value = nyr_lower_expr(b, arg_expr);
    if (value < 0)
      return -1;
    int tag = nyr_peek_dyn_fact(b, value, NYR_FACT_DYN_TAG);
    if (tag >= 0)
      return tag;
    if (nyr_expr_is_cstr(b, arg_expr))
      return nyr_emit_const(b, 121);
    if (nyr_expr_is_f64(b, arg_expr) || nyr_expr_is_f32(b, arg_expr))
      return nyr_emit_const(b, TAG_FLOAT);
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
    return nyr_emit_runtime_call(b, tag_symbol, value, -1, -1, 1, 0);
  }
  if (leaf && strcmp(leaf, "_big_is_rt") == 0 && e->as.call.args.len == 1 &&
      !e->as.call.args.data[0].name) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    /* BigInt handles can lose generic `any` provenance after loop/call joins,
     * but their concrete runtime object tag is stable.  The old has-tag fast
     * path rejected such handles before it reached the heap tag check. */
    int actual = nyr_emit_runtime_call(b, "rt_value_tag", value, -1, -1, 1, 0);
    int want = nyr_emit_const(b, TAG_BIGINT);
    return actual < 0 || want < 0
               ? -1
               : nyr_emit_cmp_i64(b, NYIR_CMP_EQ, actual, want);
  }
  if (leaf_kind == NY_NATIVE_LEAF_IS_STR && !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: is_str expects one value"), -1;
    }
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    /* This is a language predicate, not an internal raw classifier.  The
     * static tag shortcut returned raw 0/1 for a bool-valued function, which
     * made a true result falsy at the dynamic boundary.  Keep representation
     * ownership in the runtime predicate and preserve the canonical Ny bool.
     */
    return nyr_emit_runtime_call(b, "rt_is_str_obj", value, -1, -1, 1, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_INTRINSIC) {
    /* Lower the supported intrinsic subset through NYIR operations or typed
     * runtime bridges, without routing the native path through LLVM. Unknown
     * names still fail explicitly. */
    if (e->as.call.args.len < 1 || !e->as.call.args.data[0].val ||
        e->as.call.args.data[0].val->kind != NY_E_LITERAL ||
        e->as.call.args.data[0].val->as.literal.kind != NY_LIT_STR) {
      return nyr_fail(b, "native NYIR lower: intrinsic(...) first argument "
                         "must be a string literal name"),
             -1;
    }
    const char *iname = e->as.call.args.data[0].val->as.literal.as.s.data;
    size_t iname_len = e->as.call.args.data[0].val->as.literal.as.s.len;
    if (!iname || iname_len == 0) {
      return nyr_fail(b, "native NYIR lower: empty intrinsic name"), -1;
    }
    if (iname_len == 9 && memcmp(iname, "ctpop.i64", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        return nyr_fail(b, "native NYIR lower: ctpop.i64 expects one value"),
               -1;
      }
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /* Preserve this as one typed operation so each target can use its
       * native population-count instruction or the runtime's portable path. */
      return nyr_emit_runtime_call(b, "rt_simmd_popcnt64_i64", x, -1, -1, 1, 0);
    }
    if ((iname_len == 8 && memcmp(iname, "cttz.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0)) {
      int is_ctlz = (iname_len == 8 && memcmp(iname, "ctlz.i64", 8) == 0);
      /* Lower both intrinsics through the scalar runtime bridge. */
      if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
          e->as.call.args.data[1].name) {
        return nyr_fail(b, "native NYIR lower: cttz/ctlz.i64 expects a value"),
               -1;
      }
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      int zero_undef = e->as.call.args.len == 3
                           ? nyr_lower_expr(b, e->as.call.args.data[2].val)
                           : nyr_emit_const(b, 0);
      if (zero_undef < 0)
        return -1;
      /* Evaluate the optional zero-undef operand for source-level effects;
       * the runtime bridge itself uses the defined zero result (64). */
      (void)zero_undef;
      int raw_result = nyr_emit_runtime_call(
          b, is_ctlz ? "rt_simmd_clz64_i64" : "rt_simmd_ctz64_i64", x, -1, -1,
          1, 0);
      return raw_result;
    }
    if ((iname_len == 8 && memcmp(iname, "umax.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "umin.i64", 8) == 0)) {
      /* Compare as uint64_t while retaining the signed NYIR comparison:
       * flipping bit 63 maps unsigned ordering onto signed ordering. */
      if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
          e->as.call.args.data[2].name) {
        return nyr_fail(b,
                        "native NYIR lower: umax/umin.i64 expects two values"),
               -1;
      }
      bool is_max = iname[1] == 'm' && iname[2] == 'a';
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      int y = nyr_lower_expr(b, e->as.call.args.data[2].val);
      if (x < 0 || y < 0)
        return -1;
      int signbit = nyr_emit_const(b, INT64_MIN);
      if (signbit < 0)
        return -1;
      int ux = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = x, .b = signbit});
      int uy = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = y, .b = signbit});
      if (ux < 0 || uy < 0)
        return -1;
      int cond = nyr_ir_emit(
          &b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                  .dst = -1,
                                  .a = ux,
                                  .b = uy,
                                  .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
      int one = nyr_emit_const(b, 1);
      if (cond < 0 || one < 0)
        return -1;
      int nc = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = cond, .b = one});
      if (nc < 0)
        return -1;
      int t0 = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = cond, .b = x});
      int t1 = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
      if (t0 < 0 || t1 < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
    }
    if ((iname_len == 8 && memcmp(iname, "smax.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "smin.i64", 8) == 0)) {
      if (e->as.call.args.len != 3 || e->as.call.args.data[1].name ||
          e->as.call.args.data[2].name) {
        return nyr_fail(b,
                        "native NYIR lower: smax/smin.i64 expects two values"),
               -1;
      }
      bool is_max = iname_len >= 4 && iname[1] == 'm' && iname[2] == 'a';
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      int y = nyr_lower_expr(b, e->as.call.args.data[2].val);
      if (x < 0 || y < 0)
        return -1;
      /*
       * result = (x > y) ? x : y  via arithmetic select:
       * c = (x > y); c*x + (1-c)*y
       */
      int cond = nyr_ir_emit(
          &b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                  .dst = -1,
                                  .a = x,
                                  .b = y,
                                  .cmp = is_max ? NYIR_CMP_GT : NYIR_CMP_LT});
      int one = nyr_emit_const(b, 1);
      if (cond < 0 || one < 0)
        return -1;
      int nc = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = cond, .b = one});
      if (nc < 0)
        return -1;
      int t0 = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = cond, .b = x});
      int t1 = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = nc, .b = y});
      if (t0 < 0 || t1 < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = t0, .b = t1});
    }
    if (iname_len == 14 && memcmp(iname, "bitreverse.i64", 14) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        return nyr_fail(b,
                        "native NYIR lower: bitreverse.i64 expects one value"),
               -1;
      }
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      /*
       * Portable bit reverse via parallel SWAR.
       */
      int c1 = nyr_emit_const(b, (int64_t)0x5555555555555555LL);
      int c2 = nyr_emit_const(b, (int64_t)0x3333333333333333LL);
      int c4 = nyr_emit_const(b, (int64_t)0x0f0f0f0f0f0f0f0fLL);
      int c8 = nyr_emit_const(b, (int64_t)0x00ff00ff00ff00ffLL);
      int c16 = nyr_emit_const(b, (int64_t)0x0000ffff0000ffffLL);
      int one = nyr_emit_const(b, 1);
      int two = nyr_emit_const(b, 2);
      int four = nyr_emit_const(b, 4);
      int eight = nyr_emit_const(b, 8);
      int sixteen = nyr_emit_const(b, 16);
      int thirtytwo = nyr_emit_const(b, 32);
      if (c1 < 0 || c2 < 0 || c4 < 0 || c8 < 0 || c16 < 0 || one < 0 ||
          two < 0 || four < 0 || eight < 0 || sixteen < 0 || thirtytwo < 0)
        return -1;
      /*
       * x = ((x >> 1) & c1) | ((x & c1) << 1) etc. Use SAR+mask for >>
       */
      int m63 = nyr_emit_const(b, (int64_t)0x7fffffffffffffffLL);
      int m62 = nyr_emit_const(b, (int64_t)0x3fffffffffffffffLL);
      int m60 = nyr_emit_const(b, (int64_t)0x0fffffffffffffffLL);
      int m56 = nyr_emit_const(b, (int64_t)0x00ffffffffffffffLL);
      int m48 = nyr_emit_const(b, (int64_t)0x0000ffffffffffffLL);
      int m32 = nyr_emit_const(b, (int64_t)0x00000000ffffffffLL);
      if (m63 < 0 || m62 < 0 || m60 < 0 || m56 < 0 || m48 < 0 || m32 < 0)
        return -1;
#define NY_BREV_STEP(xin, sh, msk, cm)                                         \
  do {                                                                         \
    int _r = nyr_ir_emit(                                                        \
        &b->nyir,                                                              \
        (nyr_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = (xin), .b = (sh)});  \
    if (_r < 0)                                                                \
      return -1;                                                               \
    _r = nyr_ir_emit(                                                            \
        &b->nyir,                                                              \
        (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = _r, .b = (msk)});    \
    if (_r < 0)                                                                \
      return -1;                                                               \
    _r = nyr_ir_emit(                                                            \
        &b->nyir,                                                              \
        (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = _r, .b = (cm)});     \
    if (_r < 0)                                                                \
      return -1;                                                               \
    int _l = nyr_ir_emit(                                                        \
        &b->nyir,                                                              \
        (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = (xin), .b = (cm)});  \
    if (_l < 0)                                                                \
      return -1;                                                               \
    _l = nyr_ir_emit(                                                            \
        &b->nyir,                                                              \
        (nyr_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = _l, .b = (sh)});     \
    if (_l < 0)                                                                \
      return -1;                                                               \
    (xin) = nyr_ir_emit(                                                         \
        &b->nyir,                                                              \
        (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = _r, .b = _l});        \
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
        int r = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                  .dst = -1,
                                                  .a = x,
                                                  .b = thirtytwo});
        if (r < 0)
          return -1;
        r = nyr_ir_emit(
            &b->nyir,
            (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = r, .b = m32});
        if (r < 0)
          return -1;
        int l = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                  .dst = -1,
                                                  .a = x,
                                                  .b = thirtytwo});
        if (l < 0)
          return -1;
        x = nyr_ir_emit(
            &b->nyir,
            (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = r, .b = l});
        if (x < 0)
          return -1;
      }
#undef NY_BREV_STEP
      return x;
    }
    if (iname_len == 7 && memcmp(iname, "abs.i64", 7) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        return nyr_fail(b, "native NYIR lower: abs.i64 expects one value"), -1;
      }
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      int zero = nyr_emit_const(b, 0);
      int sixtythree = nyr_emit_const(b, 63);
      if (zero < 0 || sixtythree < 0)
        return -1;
      /*
       * abs via (x ^ (x>>63)) - (x>>63) arithmetic.
       */
      int s = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                .dst = -1,
                                                .a = x,
                                                .b = sixtythree});
      if (s < 0)
        return -1;
      int y = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = x, .b = s});
      if (y < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = y, .b = s});
    }
    if (iname_len == 9 && memcmp(iname, "bswap.i64", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        return nyr_fail(b, "native NYIR lower: bswap.i64 expects one value"),
               -1;
      }
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      return nyr_ir_emit(&b->nyir,
                       (nyr_inst_t){.op = NYIR_BSWAP_I64, .dst = -1, .a = x});
    }
    if (iname_len == 9 && memcmp(iname, "bswap.i32", 9) == 0) {
      if (e->as.call.args.len != 2 || e->as.call.args.data[1].name) {
        return nyr_fail(b, "native NYIR lower: bswap.i32 expects one value"),
               -1;
      }
      int x = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (x < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir, (nyr_inst_t){.op = NYIR_BSWAP32_I64, .dst = -1, .a = x});
    }
    if ((iname_len == 8 && memcmp(iname, "fshl.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "rotl.i64", 8) == 0)) {
      bool is_rotl = (iname_len == 8 && memcmp(iname, "rotl.i64", 8) == 0);
      size_t expected_args = is_rotl ? 2u : 3u;
      if (e->as.call.args.len != expected_args + 1) {
        return nyr_fail(b, "native NYIR lower: %.*s argument count mismatch",
                        (int)iname_len, iname),
               -1;
      }
      int a_val = nyr_lower_expr(b, e->as.call.args.data[1].val);
      int b_val =
          is_rotl ? a_val : nyr_lower_expr(b, e->as.call.args.data[2].val);
      int sh_val = nyr_lower_expr(b, e->as.call.args.data[is_rotl ? 2 : 3].val);
      if (a_val < 0 || b_val < 0 || sh_val < 0)
        return -1;
      int c0 = nyr_emit_const(b, 0);
      int c63 = nyr_emit_const(b, 63);
      int c64 = nyr_emit_const(b, 64);
      if (c0 < 0 || c63 < 0 || c64 < 0)
        return -1;
      int sh = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = sh_val, .b = c63});
      int inv = sh < 0 ? -1
                       : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SUB_I64,
                                                           .dst = -1,
                                                           .a = c64,
                                                           .b = sh});
      int rsh = inv < 0 ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_AND_I64,
                                                            .dst = -1,
                                                            .a = inv,
                                                            .b = c63});
      int nz = sh < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_NE, sh, c0);
      if (sh < 0 || inv < 0 || rsh < 0 || nz < 0)
        return -1;
      int left = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = a_val, .b = sh});
      int sar = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = b_val, .b = rsh});
      int sign = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = b_val, .b = c63});
      int fill = sign < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                         .dst = -1,
                                                         .a = sign,
                                                         .b = sh});
      int logical = (sar < 0 || fill < 0)
                        ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                            .dst = -1,
                                                            .a = sar,
                                                            .b = fill});
      int right = logical < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                          .dst = -1,
                                                          .a = logical,
                                                          .b = nz});
      if (left < 0 || right < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = left, .b = right});
    }
    if ((iname_len == 8 && memcmp(iname, "fshr.i64", 8) == 0) ||
        (iname_len == 8 && memcmp(iname, "rotr.i64", 8) == 0)) {
      bool is_rotr = (iname_len == 8 && memcmp(iname, "rotr.i64", 8) == 0);
      size_t expected_args = is_rotr ? 2u : 3u;
      if (e->as.call.args.len != expected_args + 1) {
        return nyr_fail(b, "native NYIR lower: %.*s argument count mismatch",
                        (int)iname_len, iname),
               -1;
      }
      int a_val = nyr_lower_expr(b, e->as.call.args.data[1].val);
      int b_val =
          is_rotr ? a_val : nyr_lower_expr(b, e->as.call.args.data[2].val);
      int sh_val = nyr_lower_expr(b, e->as.call.args.data[is_rotr ? 2 : 3].val);
      if (a_val < 0 || b_val < 0 || sh_val < 0)
        return -1;
      int c0 = nyr_emit_const(b, 0);
      int c63 = nyr_emit_const(b, 63);
      int c64 = nyr_emit_const(b, 64);
      if (c0 < 0 || c63 < 0 || c64 < 0)
        return -1;
      int sh = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_AND_I64, .dst = -1, .a = sh_val, .b = c63});
      int inv = sh < 0 ? -1
                       : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SUB_I64,
                                                           .dst = -1,
                                                           .a = c64,
                                                           .b = sh});
      int lsh = inv < 0 ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_AND_I64,
                                                            .dst = -1,
                                                            .a = inv,
                                                            .b = c63});
      int nz = sh < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_NE, sh, c0);
      if (sh < 0 || inv < 0 || lsh < 0 || nz < 0)
        return -1;
      int sar = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = b_val, .b = sh});
      int sign = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SAR_I64, .dst = -1, .a = b_val, .b = c63});
      int fill0 = sign < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = sign,
                                                          .b = lsh});
      int fill = fill0 < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                         .dst = -1,
                                                         .a = fill0,
                                                         .b = nz});
      int right = (sar < 0 || fill < 0)
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                          .dst = -1,
                                                          .a = sar,
                                                          .b = fill});
      int left0 = nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SHL_I64, .dst = -1, .a = a_val, .b = lsh});
      int left = left0 < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                         .dst = -1,
                                                         .a = left0,
                                                         .b = nz});
      if (left < 0 || right < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_OR_I64, .dst = -1, .a = left, .b = right});
    }
    return nyr_fail(b,
                    "native NYIR lower: intrinsic(\"%.*s\") is not "
                    "supported by the Nytrix-owned native backend; use "
                    "ordinary Nytrix operations or core.math.bin",
                    (int)iname_len, iname),
           -1;
  }
  /*
   * NYIR integer values are raw i64s.  Keep typed print on the raw-i64
   * runtime entry point; rt_print_value is a dynamic NyValue ABI and must
   * only be used after an explicit box operation exists.
   */
  if (leaf_kind == NY_NATIVE_LEAF_ASSERT && !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len == 2 && e->as.call.args.data[1].name)) {
      return nyr_fail(b, "native NYIR lower: assert expects condition and "
                         "optional string message"),
             -1;
    }
    int condition = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int message = -1;
    if (condition < 0)
      return -1;
    if (e->as.call.args.len == 2) {
      const expr_t *message_expr = e->as.call.args.data[1].val;
      message = nyr_lower_expr(b, e->as.call.args.data[1].val);
      if (message >= 0 && !nyr_expr_is_cstr(b, message_expr)) {
        const char *format =
            nyr_expr_is_bool(b, message_expr)  ? "rt_bool_to_cstr"
            : nyr_expr_is_f64(b, message_expr) ? "rt_f64_to_cstr_raw"
            : nyr_expr_is_f32(b, message_expr) ? "rt_f64_to_cstr_raw"
            : (message_expr &&
               ((message_expr->kind == NY_E_LITERAL &&
                 message_expr->as.literal.kind == NY_LIT_INT) ||
                message_expr->semantic.rep == NY_SEM_REP_RAW_INT))
                ? "rt_i64_to_cstr_raw"
                : "rt_any_to_cstr";
        if (nyr_expr_is_f32(b, message_expr))
          message = nyr_emit_f32_to_f64(b, message);
        if (message >= 0)
          message = nyr_emit_runtime_call(b, format, message, -1, -1, 1, 0);
      }
    } else {
      message = nyr_emit_const(b, 0);
    }
    if (message < 0)
      return -1;
    int result = nyr_emit_runtime_call(b, "rt_assert_cstr", condition, message,
                                       -1, 2, 0);
    return result < 0 ? -1 : nyr_emit_const(b, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_PRINT && !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len == 0) {
      return nyr_fail(b, "native NYIR lower: print requires at least "
                         "one positional argument"),
             -1;
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        return nyr_fail(b, "native NYIR lower: print accepts "
                           "positional arguments only"),
               -1;
      }
      const expr_t *arg = e->as.call.args.data[i].val;
      bool is_bool = nyr_expr_is_bool(b, arg);
      if (i > 0) {
        int space_ptr = nyr_emit_cstr_const(b, " ");
        if (space_ptr >= 0)
          (void)nyr_emit_runtime_call(b, "rt_print_cstr", space_ptr, -1, -1, 1,
                                      0);
      }
      bool is_f64_arg = !is_bool && nyr_expr_is_f64(b, arg);
      bool is_f32_arg = !is_bool && nyr_expr_is_f32(b, arg);
      /* A bare list/any index is a dynamic scalar/container boundary, not a
       * floating-point value merely because semantic inference propagated a
       * contextual float rep from the unknown element. */
      if (arg && arg->kind == NY_E_INDEX && arg->as.index.target &&
          arg->as.index.target->kind == NY_E_IDENT &&
          arg->as.index.target->as.ident.name) {
        const nyr_local_t *sequence =
            nyr_find_local(b, arg->as.index.target->as.ident.name);
        const char *decl = sequence ? sequence->type_name : NULL;
        bool explicit_float = decl && (strstr(decl, "<f32>") != NULL ||
                                       strstr(decl, "<f64>") != NULL);
        if (sequence && (sequence->is_list || sequence->is_any) &&
            !explicit_float) {
          is_f64_arg = false;
          is_f32_arg = false;
        }
      }
      if (arg && arg->kind == NY_E_BINARY) {
        const expr_t *parts[2] = {arg->as.binary.left, arg->as.binary.right};
        for (size_t pi = 0; pi < 2; ++pi) {
          const expr_t *part = parts[pi];
          if (!part || part->kind != NY_E_INDEX || !part->as.index.target ||
              part->as.index.target->kind != NY_E_IDENT ||
              !part->as.index.target->as.ident.name)
            continue;
          const nyr_local_t *sequence =
              nyr_find_local(b, part->as.index.target->as.ident.name);
          const char *decl = sequence ? sequence->type_name : NULL;
          bool explicit_float = decl && (strstr(decl, "<f32>") != NULL ||
                                         strstr(decl, "<f64>") != NULL);
          if (sequence && (sequence->is_list || sequence->is_any) &&
              !explicit_float) {
            is_f64_arg = false;
            is_f32_arg = false;
            break;
          }
        }
      }
      bool is_list_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                         !nyr_expr_is_cstr(b, arg) && nyr_expr_is_list(b, arg);
      /*
       * Tuples share the native sequence buffer ABI with lists but carry
       * the tuple flag; without their own branch they fall through to the
       * raw-integer printer and render as pointer words.  Materialize them
       * through the same representation-aware formatter (which prints
       * tuple brackets) instead.
       */
      bool is_tuple_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                          !is_list_arg && !nyr_expr_is_cstr(b, arg) &&
                          nyr_expr_is_tuple(b, arg);
      bool is_bigint_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                           !is_list_arg && !is_tuple_arg &&
                           (nyr_expr_is_bigint(b, arg) ||
                            (nyr_expr_type_name(b, arg) &&
                             ny_type_is(nyr_expr_type_name(b, arg), "bigint")));
      if (!is_list_arg && arg && arg->kind == NY_E_IDENT &&
          arg->as.ident.name) {
        const nyr_local_t *arg_local = nyr_find_local(b, arg->as.ident.name);
        is_list_arg = arg_local && arg_local->is_list;
      }
      bool is_any_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                        !is_list_arg && !is_tuple_arg && !is_bigint_arg &&
                        !nyr_expr_is_cstr(b, arg) && nyr_expr_is_any(b, arg);
      if (!is_any_arg && arg && arg->kind == NY_E_INDEX &&
          arg->as.index.target && arg->as.index.target->kind == NY_E_IDENT &&
          arg->as.index.target->as.ident.name) {
        const nyr_local_t *sequence =
            nyr_find_local(b, arg->as.index.target->as.ident.name);
        bool dynamic_sequence_target = nyr_local_is_dynamic_sequence(sequence);
        if (!sequence) {
          const expr_t *global_init =
              nyr_find_top_level_value(b, arg->as.index.target->as.ident.name);
          dynamic_sequence_target = global_init &&
                                    nyr_expr_is_list(b, global_init) &&
                                    nyr_expr_is_dyn_list(b, global_init);
        }
        if (dynamic_sequence_target)
          is_any_arg = true;
      }
      bool is_raw_int_arg = !is_bool && !is_f64_arg && !is_f32_arg &&
                            !is_list_arg && arg && arg->semantic.resolved &&
                            arg->semantic.rep == NY_SEM_REP_RAW_INT;
      if (!is_bool && !is_f64_arg && !is_f32_arg && !is_list_arg &&
          nyr_raw_bitwise_value(b, arg, 0)) {
        is_raw_int_arg = true;
        is_any_arg = false;
      }
      if (!is_raw_int_arg && arg && arg->kind == NY_E_INDEX) {
        const expr_t *literal_target =
            nyr_resolve_list_literal(b, arg->as.index.target, 0);
        const char *value_type = nyr_expr_type_name(b, arg);
        bool dynamic_sequence_target = false;
        if (arg->as.index.target && arg->as.index.target->kind == NY_E_IDENT &&
            arg->as.index.target->as.ident.name) {
          const nyr_local_t *sequence =
              nyr_find_local(b, arg->as.index.target->as.ident.name);
          dynamic_sequence_target = nyr_local_is_dynamic_sequence(sequence);
        }
        bool scalar_index =
            (literal_target && literal_target->kind == NY_E_LIST &&
             !nyr_expr_is_dyn_list(b, literal_target) &&
             !dynamic_sequence_target) ||
            (value_type && ny_native_type_name_is_int(value_type));
        if (scalar_index) {
          is_any_arg = false;
          is_raw_int_arg = true;
        }
      }
      if (!is_raw_int_arg && arg && arg->kind == NY_E_CALL &&
          arg->as.call.callee && arg->as.call.callee->kind == NY_E_IDENT &&
          arg->as.call.callee->as.ident.name) {
        const char *callee_name = arg->as.call.callee->as.ident.name;
        const stmt_t *value_fn = nyr_find_user_function(b, callee_name);
        if (!value_fn) {
          const expr_t *global = nyr_find_top_level_value(b, callee_name);
          ny_native_lambda_entry_t *lambda =
              global
                  ? ny_native_lambda_find(ny_native_lambda_cache_for(b), global)
                  : NULL;
          if (lambda)
            value_fn = lambda->fn;
        }
        is_raw_int_arg =
            value_fn && ny_native_type_name_is_int(value_fn->as.fn.return_type);
      }
      if (!is_any_arg && arg && arg->kind == NY_E_IDENT && arg->as.ident.name) {
        const nyr_local_t *arg_local = nyr_find_local(b, arg->as.ident.name);
        is_any_arg = arg_local && arg_local->is_any;
      }
      /* Formatting is an any boundary.  A dynamic sequence may use raw
       * elem-8 slots for arithmetic, but an indexed value passed to print
       * must be materialized through the tagged accessor before
       * rt_any_to_cstr sees it.  Keep this context local to the expression;
       * typed scalar lists continue to use their proven raw ABI. */
      bool index_any_boundary = arg && arg->kind == NY_E_INDEX && is_any_arg;
      bool saved_index_for_any_call = b->index_for_any_call;
      if (index_any_boundary)
        b->index_for_any_call = true;
      int raw = nyr_lower_expr(b, arg);
      b->index_for_any_call = saved_index_for_any_call;
      if (raw < 0)
        return -1;
      /*
       * Direct-call specialization can refine an otherwise dynamic function
       * result to RAW_INT and insert its one required unbox while lowering
       * the expression.  The source annotation may still say `any`, but the
       * value at this point is already raw; rendering it through the dynamic
       * string helper would halve tagged-looking integers (for example,
       * `print(add(1, 2))` could print 1 instead of 3).
       */
      for (size_t ni = b->nyir.len; ni > 0; --ni) {
        const nyr_inst_t *def = &b->nyir.data[ni - 1];
        if (def->dst != raw)
          continue;
        /* Producer ABI record: the conversion bridge's row carries
         * RESULT_RAW_INT, so classify by record row; the spelling check
         * stays as the fallback for symbols the record does not declare. */
        if (def->op == NYIR_CALL && def->symbol &&
            (ny_native_runtime_abi_decl(def->symbol) ==
                 ny_native_runtime_abi_decl("rt_any_to_i64") ||
             strcmp(def->symbol, "rt_any_to_i64") == 0)) {
          is_any_arg = false;
          is_raw_int_arg = true;
        }
        /* The ABI record is authoritative for any recorded producer: a
         * raw-int result row (rt_f64_to_i64 under `__flt_to_int`, and the
         * other typed conversion bridges) marks an already-decoded scalar
         * even when the source classifier kept the node in the dynamic lane.
         * Rendering that word through rt_any_to_cstr decodes it as a tagged
         * value (raw 480 renders as nil). */
        else if (def->op == NYIR_CALL && def->symbol && is_any_arg &&
                 !is_bool && !is_f64_arg && !is_f32_arg && !is_list_arg) {
          const ny_native_runtime_abi_decl_t *producer_row =
              ny_native_runtime_abi_decl(def->symbol);
          if (producer_row &&
              ny_native_runtime_abi_result_is_raw_int(producer_row)) {
            is_any_arg = false;
            is_raw_int_arg = true;
          }
        }
        /*
         * The bigint→int bridge returns a raw i64 by contract. Its result
         * can exceed the tagged-smallint range and is frequently even, so
         * rendering it through the dynamic helper decodes the raw word as a
         * tagged smallint and halves it (SMALL_MIN - 1 lost its low bit).
         * Its record row (RESULT_RAW_INT/RAW_WORD family) decides; the
         * spelling remains the fallback for unrecorded symbols.
         */
        if (def->op == NYIR_CALL && def->symbol &&
            (ny_native_runtime_abi_decl(def->symbol) ==
                 ny_native_runtime_abi_decl("rt_bigint_to_i64_raw") ||
             strcmp(def->symbol, "rt_bigint_to_i64_raw") == 0)) {
          is_any_arg = false;
          is_raw_int_arg = true;
        }
        /*
         * A typed elem-8 list read keeps its proven raw accessor even when
         * the surrounding classifier conservatively called the argument
         * `any` (notably a top-level `mut list<int>` initialized from an
         * empty literal).  The raw slot payload must render through the
         * integer formatter; rt_any_to_cstr would decode it as a tagged
         * word (raw 0 renders as nil, raw 2 as false). The record row
         * (NO_DYNAMIC_BOXING) decides; the spelling remains the fallback
         * for unrecorded symbols.
         */
        if (def->op == NYIR_CALL && def->symbol &&
            (ny_native_runtime_abi_decl(def->symbol) ==
                 ny_native_runtime_abi_decl("rt_tbuf_index_read_raw") ||
             strcmp(def->symbol, "rt_tbuf_index_read_raw") == 0)) {
          is_any_arg = false;
          is_raw_int_arg = true;
        }
        break;
      }
      /*
       * The emitter fact again: a dynamic integer binary reaches print as a
       * raw i64 even though inference classifies the node as any, so
       * rt_any_to_cstr would render the payload word (<ptr>/<fn>) instead of
       * the number.  Render through the raw entry when the fact says raw.
       */
      bool arg_arith_raw = arg && arg->kind == NY_E_BINARY &&
                           b->last_arith_expr == arg && b->last_arith_raw;
      /* Set when the argument was already materialized into a C string by
       * the render below; the printer must then take the string entry point.
       * Printing that pointer through rt_print_i64_raw exposes the string's
       * address as the checksum (fasta/revcomp `sum * 1000000 + N`). */
      bool arg_rendered_cstr =
          nyr_value_scalar_class(b, raw) == NYIR_SCALAR_STRING;
      if (arg_rendered_cstr) {
        is_any_arg = false;
        is_raw_int_arg = false;
        is_list_arg = false;
        is_tuple_arg = false;
        is_f64_arg = false;
        is_f32_arg = false;
        is_bool = false;
      }
      if (is_bigint_arg) {
        raw = nyr_emit_runtime_call(b, "rt_bigint_to_cstr_raw", raw, -1, -1, 1,
                                    0);
        if (raw < 0)
          return -1;
      }
      /* The source classifier can miss a dispatched member/operator call
       * whose selected implementation has an f64 return ABI.  The defining
       * NYIR instruction is authoritative after lowering; use its scalar
       * class to keep print on the floating ABI instead of rendering IEEE
       * bits as an integer. */
      if (!is_bool && !is_f64_arg && !is_f32_arg) {
        for (size_t ni = b->nyir.len; ni > 0; --ni) {
          const nyr_inst_t *def = &b->nyir.data[ni - 1];
          if (def->dst != raw)
            continue;
          if (def->scalar_class == NYIR_SCALAR_F64)
            is_f64_arg = true;
          else if (def->scalar_class == NYIR_SCALAR_F32)
            is_f32_arg = true;
          break;
        }
        if (is_f64_arg || is_f32_arg) {
          is_any_arg = false;
          is_list_arg = false;
          is_raw_int_arg = false;
        }
      }
      if (is_bool) {
        raw = nyr_emit_runtime_call(b, "rt_bool_to_cstr", raw, -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (is_f32_arg) {
        raw = nyr_emit_f32_to_f64(b, raw);
        if (raw < 0)
          return -1;
      } else if (is_list_arg || is_tuple_arg) {
        /* Native lists are raw tbuf handles, not tagged heap lists.  Printing
         * them through the scalar i64 entry point exposes the allocation
         * address; materialize the representation-aware list text first.
         * Tuples share the buffer ABI (with the tuple flag selecting
         * parentheses inside the formatter). */
        raw = nyr_emit_runtime_call(b, "rt_tbuf_to_cstr", raw, -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (arg_arith_raw && !is_f64_arg && !is_f32_arg) {
        raw = nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", raw, -1, -1, 1, 0);
        if (raw < 0)
          return -1;
        arg_rendered_cstr = true;
      } else if (is_any_arg) {
        /* An `any`-classified argument carries the tagged dynamic ABI even
         * when inference also refined it to an integer: its storage slot is
         * tagged (is_any local / dynamic-return call), so decoding here is
         * the one correct render.  A genuinely raw integer local is never
         * is_any and still takes the raw branch below. */
        raw = nyr_emit_runtime_call(b, "rt_any_to_cstr", raw, -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      } else if (is_raw_int_arg) {
        raw = nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", raw, -1, -1, 1, 0);
        if (raw < 0)
          return -1;
      }
      const char *print_sym = is_bool || is_bigint_arg ||
                                      nyr_expr_is_cstr(b, arg) || is_list_arg ||
                                      is_tuple_arg || is_raw_int_arg ||
                                      is_any_arg || arg_rendered_cstr
                                  ? "rt_print_cstr"
                              : (is_f64_arg || is_f32_arg) ? "rt_print_f64_raw"
                                                           : "rt_print_i64_raw";
      if (nyr_emit_runtime_call(b, print_sym, raw, -1, -1, 1, 0) < 0)
        return -1;
    }
    if (nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CALL,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .c = -1,
                                          .imm = 0,
                                          .flags = NYIR_INST_F_EXTERN,
                                          .symbol = "rt_print_newline"}) < 0) {
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    return nyr_emit_const(b, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_EPRINT && !nyr_user_defined_fn(b, name)) {
    /* Keep eprint's varargs ABI at the call site.  The stdlib implementation
     * receives a synthesized list, which the shared NYIR lowerer cannot
     * currently build for variadic functions.  Format each original argument
     * through the same scalar conversion helpers as print, then write the
     * resulting C string to stderr. */
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name)
        return nyr_fail(b, "native NYIR lower: eprint accepts positional "
                           "arguments only"),
               -1;
      const expr_t *arg = e->as.call.args.data[i].val;
      if (i > 0) {
        int space = nyr_emit_cstr_const(b, " ");
        int len = space < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_cstr_len", space, -1,
                                                    -1, 1, NYIR_SCALAR_I64_RAW);
        if (space < 0 || len < 0 ||
            nyr_emit_runtime_call4(b, "rt_write_off_raw", nyr_emit_const(b, 2),
                                   space, len, nyr_emit_const(b, 0), -1, 4,
                                   NYIR_SCALAR_I64_RAW) < 0)
          return -1;
      }
      bool is_bool = nyr_expr_is_bool(b, arg);
      bool is_f64 = !is_bool && nyr_expr_is_f64(b, arg);
      bool is_f32 = !is_bool && nyr_expr_is_f32(b, arg);
      bool is_any = !is_bool && !is_f64 && !is_f32 &&
                    !nyr_expr_is_cstr(b, arg) && nyr_expr_is_any(b, arg);
      bool is_bigint = !is_bool && !is_f64 && !is_f32 && !is_any &&
                       !nyr_expr_is_cstr(b, arg) &&
                       (nyr_expr_is_bigint(b, arg) ||
                        (nyr_expr_type_name(b, arg) &&
                         ny_type_is(nyr_expr_type_name(b, arg), "bigint")));
      int value = nyr_lower_expr(b, arg);
      if (value < 0)
        return -1;
      if (is_bool)
        value = nyr_emit_runtime_call(b, "rt_bool_to_cstr", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      else if (is_f32) {
        value = nyr_emit_f32_to_f64(b, value);
        if (value >= 0)
          value = nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", value, -1, -1,
                                        1, NYIR_SCALAR_I64_RAW);
      } else if (is_f64)
        value = nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      else if (is_bigint)
        value = nyr_emit_runtime_call(b, "rt_bigint_to_cstr_raw", value, -1, -1,
                                      1, NYIR_SCALAR_I64_RAW);
      else if (is_any)
        value = nyr_emit_runtime_call(b, "rt_any_to_cstr", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      else if (!nyr_expr_is_cstr(b, arg))
        value = nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      if (value < 0)
        return -1;
      int len = nyr_emit_runtime_call(b, "rt_cstr_len", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      if (len < 0 ||
          nyr_emit_runtime_call4(b, "rt_write_off_raw", nyr_emit_const(b, 2),
                                 value, len, nyr_emit_const(b, 0), -1, 4,
                                 NYIR_SCALAR_I64_RAW) < 0)
        return -1;
    }
    int newline = nyr_emit_cstr_const(b, "\n");
    int newline_len = newline < 0
                          ? -1
                          : nyr_emit_runtime_call(b, "rt_cstr_len", newline, -1,
                                                  -1, 1, NYIR_SCALAR_I64_RAW);
    if (newline < 0 || newline_len < 0 ||
        nyr_emit_runtime_call4(b, "rt_write_off_raw", nyr_emit_const(b, 2),
                               newline, newline_len, nyr_emit_const(b, 0), -1,
                               4, NYIR_SCALAR_I64_RAW) < 0)
      return -1;
    return nyr_emit_const(b, 0);
  }
  /* The float() cast must claim priority before the generic branches: an
   * integer-typed argument lowered inside a float-returning function is
   * computed in the f64 domain, and only this intrinsic knows its result
   * register is already machine f64. */
  if (leaf_kind == NY_NATIVE_LEAF_FLOAT && !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(
                 b,
                 "native NYIR lower: float requires one positional argument"),
             -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    return nyr_lower_f64_arg(b, arg);
  }
  if (leaf_kind == NY_NATIVE_LEAF_ARGC && !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 0) {
      return nyr_fail(b, "native NYIR lower: %s takes no arguments", leaf), -1;
    }
    /*
     * rt_argc returns the tagged VM integer; native NYIR integers are raw
     * i64.
     */
    int tagged = nyr_emit_runtime_call(b, "rt_argc", -1, -1, -1, 0, 0);
    int one = tagged < 0 ? -1 : nyr_emit_const(b, 1);
    return one < 0 ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (leaf && (strcmp(leaf, "__argv") == 0 || strcmp(leaf, "argv") == 0) &&
      !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: %s takes one index", leaf), -1;
    }
    int index = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return index < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_argv_get", index, -1, -1, 1, 0);
  }
  if (leaf && (strcmp(leaf, "__envc") == 0 || strcmp(leaf, "envc") == 0) &&
      !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 0) {
      return nyr_fail(b, "native NYIR lower: %s takes no arguments", leaf), -1;
    }
    return nyr_emit_runtime_call(b, "rt_envc_raw", -1, -1, -1, 0, 0);
  }
  if (leaf &&
      (strcmp(leaf, "__env_get") == 0 || strcmp(leaf, "env_get") == 0) &&
      !nyr_user_defined_fn(b, name)) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: %s takes one positional argument",
                      leaf),
             -1;
    }
    int key = nyr_lower_expr(b, e->as.call.args.data[0].val);
    return key < 0 ? -1
                   : nyr_emit_runtime_call(b, "rt_env_get", key, -1, -1, 1, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_TICKS) {
    if (e->as.call.args.len != 0) {
      return nyr_fail(b, "native NYIR lower: ticks takes no arguments"), -1;
    }
    /* rt_ticks_ns returns the tagged encoding; raw subtraction of two
     * tagged words doubled every self-timed measurement.  The raw
     * entrypoint returns plain monotonic nanoseconds so `t1 - t0` in user
     * code is the true elapsed time. */
    return nyr_emit_runtime_call(b, "rt_monotonic_ns_raw", -1, -1, -1, 0, 0);
  }
  if (leaf_kind == NY_NATIVE_LEAF_FLT_SQRT) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: __flt_sqrt requires one "
                         "positional argument"),
             -1;
    }
    const expr_t *arg = e->as.call.args.data[0].val;
    int value = nyr_lower_expr(b, arg);
    if (value < 0)
      return -1;
    /*
     * The register class outranks the AST type here too: loop-carried f64
     * locals feed sqrt through registers the AST classifier calls int
     * (their HM type lagged the f64 context), and i64.to.f64 of an f64
     * register reinterprets the IEEE bits — n-body's sqrt returned 4.7e18.
     */
    if (!nyr_expr_is_f64(b, arg) && !nyr_value_is_f64(b, value, 0)) {
      value = nyr_emit_i64_to_f64(b, value);
      if (value < 0)
        return -1;
    }
    return nyr_push_val(b, NYIR_SQRT_F64, value, -1, 0, NULL);
  }
  if (leaf_kind == NY_NATIVE_LEAF_ADDR && leaf && strcmp(leaf, "borrow") == 0) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name ||
        !e->as.call.args.data[0].val) {
      return nyr_fail(b, "native NYIR lower: borrow requires one expression"),
             -1;
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
      return nyr_lower_expr(b, target->as.deref.target);
    if (target->kind == NY_E_IDENT) {
      nyr_local_t *l = nyr_find_local(b, target->as.ident.name);
      if (l && (l->is_list || l->is_cstr || l->is_any))
        return nyr_lower_expr(b, target);
    } else {
      return nyr_lower_expr(b, target);
    }
  }
  if (leaf_kind == NY_NATIVE_LEAF_ADDR) {
    const expr_t *target = e->as.call.args.data[0].val;
    if (target->kind == NY_E_DEREF)
      return nyr_lower_expr(b, target->as.deref.target);
    if (target->kind != NY_E_IDENT) {
      return nyr_fail(b,
                      "native NYIR lower: %s supports local and dereferenced "
                      "pointer "
                      "lvalues, not expression kind %d at %s:%d in %s",
                      leaf, (int)target->kind,
                      target->tok.filename ? target->tok.filename : "<source>",
                      target->tok.line,
                      b->current_fn_name ? b->current_fn_name : "<unknown>"),
             -1;
    }
    const char *local_name = target->as.ident.name;
    nyr_local_t *l = nyr_find_local(b, local_name);
    if (!l) {
      int v = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .imm = 0,
                                                .symbol = local_name});
      if (v < 0)
        nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
      return v;
    }
    return nyr_emit_addr_local(b, l->slot, local_name);
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_NEW) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: f64buf_new requires one "
                         "positional length"),
             -1;
    }
    int count = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int width = nyr_emit_const(b, 8);
    if (count < 0 || width < 0)
      return -1;
    int out = nyr_emit_runtime_call(b, "rt_tbuf_new_typed_raw", count, width,
                                    -1, 2, 0);
    int64_t const_count = 0;
    if (nyr_fold_top_level_int(b, NULL, e->as.call.args.data[0].val,
                               &const_count, 0) &&
        const_count > 0 && out >= 0)
      nyr_record_alloc_fact(b, out, const_count * 8);
    return out;
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_LOAD) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      return nyr_fail(
                 b, "native NYIR lower: f64buf_load requires buffer and index"),
             -1;
    }
    /*
     * Fin-typed index elision: skip bounds check when index is Fin<N>
     * and N <= buffer byte length (comptime-known).
     */
    int64_t buf_byte_len = nyr_resolve_buf_byte_len(b, e);
    int data = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int index = nyr_lower_int_value(b, e->as.call.args.data[1].val);
    if (data < 0 || index < 0)
      return -1;
    int shift3 = nyr_emit_const(b, 3);
    int offset =
        shift3 < 0 ? -1 : nyr_push_val(b, NYIR_SHL_I64, index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!nyr_index_fin_bound_elision(b, e, 1, buf_byte_len))
      nyr_emit_bounds_check(b, data, offset, buf_byte_len);
    int addr = nyr_emit_add_i64(b, data, offset);
    if (addr >= 0 && b->nyir.len > 0 &&
        b->nyir.data[b->nyir.len - 1].dst == addr)
      b->nyir.data[b->nyir.len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
    return addr < 0 ? -1 : nyr_emit_load_f64(b, addr);
  }
  if (leaf_kind == NY_NATIVE_LEAF_F64BUF_STORE) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      return nyr_fail(b, "native NYIR lower: f64buf_store requires "
                         "buffer, index, and value"),
             -1;
    }
    /*
     * Fin-typed index elision.
     */
    int64_t buf_byte_len = nyr_resolve_buf_byte_len(b, e);
    int data = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int index = nyr_lower_int_value(b, e->as.call.args.data[1].val);
    int value = nyr_lower_expr(b, e->as.call.args.data[2].val);
    if (data < 0 || index < 0 || value < 0)
      return -1;
    int shift3 = nyr_emit_const(b, 3);
    int offset =
        shift3 < 0 ? -1 : nyr_push_val(b, NYIR_SHL_I64, index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!nyr_index_fin_bound_elision(b, e, 1, buf_byte_len))
      nyr_emit_bounds_check(b, data, offset, buf_byte_len);
    int addr = nyr_emit_add_i64(b, data, offset);
    if (addr >= 0 && b->nyir.len > 0 &&
        b->nyir.data[b->nyir.len - 1].dst == addr)
      b->nyir.data[b->nyir.len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
    return addr < 0 ? -1 : (nyr_emit_store_f64(b, addr, value) ? data : -1);
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_NEW) {
    if (e->as.call.args.len != 1 || e->as.call.args.data[0].name) {
      return nyr_fail(b, "native NYIR lower: i64buf_new requires one "
                         "positional length"),
             -1;
    }
    /* Do not replace `i64buf_new` with a bare stack allocation.  The public
     * typed-buffer contract guarantees zero-initialized elements; an alloca
     * exposes indeterminate stack bytes and corrupts histogram/accumulator
     * workloads before their first store. */
    int count = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int width = nyr_emit_const(b, 8);
    if (count < 0 || width < 0)
      return -1;
    int out = nyr_emit_runtime_call(b, "rt_tbuf_new_typed_raw", count, width,
                                    -1, 2, 0);
    /* Fold the length arg so bounds-check elision can fire on literal-count
     * buffers like i64buf_new(64) / zeros(64), matching f64buf_new. */
    int64_t const_count = 0;
    if (nyr_fold_top_level_int(b, NULL, e->as.call.args.data[0].val,
                               &const_count, 0) &&
        const_count > 0 && out >= 0)
      nyr_record_alloc_fact(b, out, const_count * 8);
    return out;
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_LOAD) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      return nyr_fail(
                 b, "native NYIR lower: i64buf_load requires buffer and index"),
             -1;
    }
    int64_t buf_byte_len = nyr_resolve_buf_byte_len(b, e);
    int data = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int index = nyr_lower_int_value(b, e->as.call.args.data[1].val);
    if (data < 0 || index < 0)
      return -1;
    int shift3 = nyr_emit_const(b, 3);
    int offset =
        shift3 < 0 ? -1 : nyr_push_val(b, NYIR_SHL_I64, index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!nyr_index_fin_bound_elision(b, e, 1, buf_byte_len) &&
        !nyr_emit_bounds_check(b, data, offset, buf_byte_len))
      return -1;
    int addr = nyr_emit_add_i64(b, data, offset);
    if (addr >= 0 && b->nyir.len > 0 &&
        b->nyir.data[b->nyir.len - 1].dst == addr)
      b->nyir.data[b->nyir.len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
    int loaded = addr < 0 ? -1 : nyr_emit_load_i64(b, addr);
    /*
     * i64buf_load is the typed-buffer integer boundary: unlike a dynamic
     * list read, its payload is stored as a raw machine i64.  The generic
     * load emitter has no source-level context, so leaving its class
     * UNKNOWN lets later semantic fallback stamp it TAGGED (for example
     * `def y = i64buf_load(right, x)` in splay).  A subsequent typed-buffer
     * index then scales the tagged value as an address, corrupting links.
     * Stamp the producer here, at the point where the storage contract is
     * known; this also keeps integer consumers such as rotr on the raw ABI.
     */
    if (loaded >= 0 && b->nyir.len > 0) {
      nyr_inst_t *load = &b->nyir.data[b->nyir.len - 1];
      if (load->dst == loaded && load->op == NYIR_LOAD_I64)
        load->scalar_class = NYIR_SCALAR_I64_RAW;
    }
    return loaded;
  }
  if (leaf_kind == NY_NATIVE_LEAF_I64BUF_STORE) {
    if (e->as.call.args.len != 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name || e->as.call.args.data[2].name) {
      return nyr_fail(b, "native NYIR lower: i64buf_store requires "
                         "buffer, index, and value"),
             -1;
    }
    int64_t buf_byte_len = nyr_resolve_buf_byte_len(b, e);
    int data = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int index = nyr_lower_int_value(b, e->as.call.args.data[1].val);
    int value = nyr_lower_expr(b, e->as.call.args.data[2].val);
    if (data < 0 || index < 0 || value < 0)
      return -1;
    /*
     * The store ABI writes the machine word unchanged.  A value argument
     * carrying the tagged dynamic ABI must therefore be decoded exactly once
     * here: tagged list-element reads and tagged bitwise runtime results
     * (rt_and/rt_xor/...) otherwise persist 2*v+1 into the slot, and every
     * later raw load doubles the stored value (the pbkdf2 checksum
     * divergence).  The defining instruction is authoritative after
     * lowering, mirroring the print and append boundaries.
     */
    {
      const char *tagged_ops[] = {"rt_tag",     "rt_and",     "rt_or",
                                  "rt_xor",     "rt_shl",     "rt_shr",
                                  "rt_any_add", "rt_any_sub", NULL};
      for (size_t ni = b->nyir.len; ni > 0; --ni) {
        const nyr_inst_t *def = &b->nyir.data[ni - 1];
        if (def->dst != value)
          continue;
        if (def->op == NYIR_CALL && def->symbol) {
          for (const char **op = tagged_ops; *op; ++op) {
            if (strcmp(def->symbol, *op) == 0) {
              int decoded = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1,
                                                  -1, 1, 0);
              if (decoded < 0)
                return -1;
              value = decoded;
            }
          }
        }
        break;
      }
    }
    int shift3 = nyr_emit_const(b, 3);
    int offset =
        shift3 < 0 ? -1 : nyr_push_val(b, NYIR_SHL_I64, index, shift3, 0, NULL);
    if (offset < 0)
      return -1;
    if (!nyr_index_fin_bound_elision(b, e, 1, buf_byte_len) &&
        !nyr_emit_bounds_check(b, data, offset, buf_byte_len))
      return -1;
    int addr = nyr_emit_add_i64(b, data, offset);
    if (addr >= 0 && b->nyir.len > 0 &&
        b->nyir.data[b->nyir.len - 1].dst == addr)
      b->nyir.data[b->nyir.len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
    return addr < 0 || !nyr_emit_store_i64(b, addr, value) ? -1 : data;
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD8) {
    if (e->as.call.args.len != 2 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name) {
      return nyr_fail(b, "load8 requires pointer and byte offset"), -1;
    }
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int index = nyr_lower_int_value(b, e->as.call.args.data[1].val);
    int effective =
        (addr < 0 || index < 0) ? -1 : nyr_emit_add_i64(b, addr, index);
    return effective < 0 ? -1 : nyr_emit_load8(b, effective);
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD16) {
    bool intrinsic = leaf && strcmp(leaf, "__load16_idx") == 0;
    if (e->as.call.args.len < (intrinsic ? 2u : 1u) ||
        e->as.call.args.len > 2 || e->as.call.args.data[0].name ||
        (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
      return nyr_fail(b, "load16 requires pointer and optional byte offset"),
             -1;
    }
    int base = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int offset = e->as.call.args.len == 2
                     ? nyr_lower_int_value(b, e->as.call.args.data[1].val)
                     : nyr_emit_const(b, 0);
    if (base < 0 || offset < 0)
      return -1;
    /* Normalize a dynamic integer carrier before adding it to a raw pointer. */
    if (nyr_value_scalar_class(b, offset) == NYIR_SCALAR_I64_TAGGED) {
      offset = nyr_emit_runtime_call(b, "rt_any_to_i64", offset, -1, -1, 1,
                                     NYIR_SCALAR_I64_RAW);
      if (offset < 0)
        return -1;
    }
    int addr = nyr_emit_add_i64(b, base, offset);
    int zero = nyr_emit_const(b, 0);
    int tagged =
        addr < 0 || zero < 0
            ? -1
            : nyr_emit_runtime_call(b, "rt_load16_idx", addr, zero, -1, 2, 0);
    int one = tagged < 0 ? -1 : nyr_emit_const(b, 1);
    return one < 0 ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE8) {
    bool intrinsic = leaf && strcmp(leaf, "__store8_idx") == 0;
    if (e->as.call.args.len < (intrinsic ? 3u : 2u) ||
        e->as.call.args.len > 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name ||
        (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
      return nyr_fail(
                 b, "store8 requires pointer, value, and optional byte offset"),
             -1;
    }
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    size_t value_index = intrinsic ? 2u : 1u;
    size_t index_index = intrinsic ? 1u : 2u;
    int value = nyr_lower_int_value(b, e->as.call.args.data[value_index].val);
    int index =
        e->as.call.args.len > index_index
            ? nyr_lower_int_value(b, e->as.call.args.data[index_index].val)
            : nyr_emit_const(b, 0);
    int effective =
        (addr < 0 || index < 0) ? -1 : nyr_emit_add_i64(b, addr, index);
    return effective < 0 || value < 0 || !nyr_emit_store8(b, effective, value)
               ? -1
               : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE16) {
    bool intrinsic = leaf && strcmp(leaf, "__store16_idx") == 0;
    if (e->as.call.args.len < (intrinsic ? 3u : 2u) ||
        e->as.call.args.len > 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name ||
        (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
      return nyr_fail(
                 b,
                 "store16 requires pointer, value, and optional byte offset"),
             -1;
    }
    size_t value_index = intrinsic ? 2u : 1u;
    size_t index_index = intrinsic ? 1u : 2u;
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int value = nyr_lower_int_value(b, e->as.call.args.data[value_index].val);
    int index =
        e->as.call.args.len > index_index
            ? nyr_lower_int_value(b, e->as.call.args.data[index_index].val)
            : nyr_emit_const(b, 0);
    if (addr < 0 || value < 0 || index < 0)
      return -1;
    int effective = nyr_emit_add_i64(b, addr, index);
    int one = nyr_emit_const(b, 1);
    int shifted = one < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = value,
                                                          .b = one});
    int tagged =
        shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
    int zero = nyr_emit_const(b, 0);
    return effective < 0 || tagged < 0 || zero < 0 ||
                   nyr_emit_runtime_call(b, "rt_store16_idx", effective, zero,
                                         tagged, 3, 0) < 0
               ? -1
               : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_STORE32) {
    bool intrinsic = leaf && strcmp(leaf, "__store32_idx") == 0;
    if (e->as.call.args.len < (intrinsic ? 3u : 2u) ||
        e->as.call.args.len > 3 || e->as.call.args.data[0].name ||
        e->as.call.args.data[1].name ||
        (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
      return nyr_fail(
                 b,
                 "store32 requires pointer, value, and optional byte offset"),
             -1;
    }
    size_t value_index = intrinsic ? 2u : 1u;
    size_t index_index = intrinsic ? 1u : 2u;
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int value = nyr_lower_int_value(b, e->as.call.args.data[value_index].val);
    int index =
        e->as.call.args.len > index_index
            ? nyr_lower_int_value(b, e->as.call.args.data[index_index].val)
            : nyr_emit_const(b, 0);
    if (addr < 0 || value < 0 || index < 0)
      return -1;
    int effective = nyr_emit_add_i64(b, addr, index);
    int one = nyr_emit_const(b, 1);
    int shifted = one < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = value,
                                                          .b = one});
    int tagged =
        shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
    int zero = nyr_emit_const(b, 0);
    return effective < 0 || tagged < 0 || zero < 0 ||
                   nyr_emit_runtime_call(b, "rt_store32_idx", effective, zero,
                                         tagged, 3, 0) < 0
               ? -1
               : value;
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD32) {
    bool intrinsic = leaf && strcmp(leaf, "__load32_idx") == 0;
    if (e->as.call.args.len < (intrinsic ? 2u : 1u) ||
        e->as.call.args.len > 2 || e->as.call.args.data[0].name ||
        (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
      return nyr_fail(b, "native NYIR lower: load32 requires pointer "
                         "and optional offset"),
             -1;
    }
    int base = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int offset = e->as.call.args.len == 2
                     ? nyr_lower_int_value(b, e->as.call.args.data[1].val)
                     : nyr_emit_const(b, 0);
    if (base < 0 || offset < 0)
      return -1;
    /* Keep the memory address in the raw integer domain even when the
     * source offset is an `any` arithmetic expression. */
    if (nyr_value_scalar_class(b, offset) == NYIR_SCALAR_I64_TAGGED) {
      offset = nyr_emit_runtime_call(b, "rt_any_to_i64", offset, -1, -1, 1,
                                     NYIR_SCALAR_I64_RAW);
      if (offset < 0)
        return -1;
    }
    int addr = nyr_emit_add_i64(b, base, offset);
    int zero = nyr_emit_const(b, 0);
    int tagged =
        addr < 0 || zero < 0
            ? -1
            : nyr_emit_runtime_call(b, "rt_load32_idx", addr, zero, -1, 2, 0);
    int one = tagged < 0 ? -1 : nyr_emit_const(b, 1);
    return one < 0 ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                       .dst = -1,
                                                       .a = tagged,
                                                       .b = one});
  }
  if (leaf_kind == NY_NATIVE_LEAF_LOAD64 ||
      leaf_kind == NY_NATIVE_LEAF_LOAD64_IDX) {
    if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
        e->as.call.args.data[0].name ||
        (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
      return nyr_fail(b, "native NYIR lower: load64/load64_i/load64_h "
                         "require positional pointer and optional offset"),
             -1;
    }
    /*
     * Fin-typed offset elision: resolve buffer byte length from the ptr
     * argument, then check whether the byte-offset arg has a Fin type.
     */
    int64_t buf_byte_len = nyr_resolve_buf_byte_len(b, e);
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (addr < 0)
      return -1;
    /*
     * A dynamic address (list/dict element, any local) reaches this
     * intrinsic as a tagged word — the raw store would then dereference the
     * tag bits (the remote-socket server thread's store64 through an
     * args.get slot). Proven raw pointers skip the decode.
     */
    if (nyr_expr_is_any(b, e->as.call.args.data[0].val)) {
      int decoded =
          nyr_emit_runtime_call(b, "rt_any_to_i64", addr, -1, -1, 1, 0);
      if (decoded < 0)
        return -1;
      addr = decoded;
    }
    if (e->as.call.args.len > 1) {
      int off = nyr_lower_int_value(b, e->as.call.args.data[1].val);
      if (off < 0)
        return -1;
      if (!nyr_index_fin_bound_elision(b, e, 1, buf_byte_len))
        nyr_emit_bounds_check(b, addr, off, buf_byte_len);
      addr = nyr_emit_add_i64(b, addr, off);
      if (addr < 0)
        return -1;
    }
    int loaded = nyr_emit_load_i64(b, addr);
    if (loaded >= 0 && b->nyir.len > 0 && leaf) {
      nyr_inst_t *load = &b->nyir.data[b->nyir.len - 1];
      if (load->dst == loaded) {
        if (strcmp(leaf, "__load64_idx") == 0 ||
            strcmp(leaf, "load64_i") == 0 || strcmp(leaf, "load64") == 0)
          load->scalar_class = NYIR_SCALAR_I64_RAW;
        else if (strcmp(leaf, "__load64_tagged_idx") == 0)
          load->scalar_class = NYIR_SCALAR_I64_TAGGED;
      }
    }
    return loaded;
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
      return nyr_fail(b, "native NYIR lower: store64_i/store64_h require "
                         "positional pointer, value, and optional offset"),
             -1;
    }
    size_t val_idx = intrinsic_order ? 2u : 1u;
    size_t off_idx = intrinsic_order ? 1u : 2u;
    /*
     * Fin-typed offset elision.
     */
    int64_t buf_byte_len = nyr_resolve_buf_byte_len(b, e);
    int addr = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int value = nyr_lower_expr(b, e->as.call.args.data[val_idx].val);
    if (addr < 0 || value < 0)
      return -1;
    /*
     * A dynamic address (list/dict element, any local) reaches the store as
     * a tagged word — decoding it keeps the raw store from dereferencing tag
     * bits (the remote-socket server thread crashed storing through an
     * args.get slot). Proven raw pointers skip the decode.
     */
    if (nyr_expr_is_any(b, e->as.call.args.data[0].val)) {
      int decoded =
          nyr_emit_runtime_call(b, "rt_any_to_i64", addr, -1, -1, 1, 0);
      if (decoded < 0)
        return -1;
      addr = decoded;
    }
    /* `store64_i` writes raw scalar bits. Large integer literals are
     * represented as boxed bigints by the language literal lowering; unwrap
     * that representation before writing the C/FFI memory slot. */
    const expr_t *value_expr = e->as.call.args.data[val_idx].val;
    /* store64 writes a raw slot.  Dynamic producers (callbacks, sequence
     * reads, and `any` locals) carry tagged integers at this boundary; decode
     * them once while preserving pointer/string/float handles. */
    if (value_expr && nyr_expr_is_any(b, value_expr)) {
      value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (leaf && strcmp(leaf, "store64_i") == 0 && value_expr &&
        value_expr->kind == NY_E_LITERAL &&
        value_expr->as.literal.kind == NY_LIT_INT &&
        (value_expr->as.literal.as.i >= (INT64_C(1) << 62) ||
         value_expr->as.literal.as.i <= -(INT64_C(1) << 62)) &&
        !nyr_value_is_untagged_i64_call(b, value)) {
      value =
          nyr_emit_runtime_call(b, "rt_bigint_to_i64_raw", value, -1, -1, 1, 0);
      if (value < 0)
        return -1;
    }
    if (e->as.call.args.len > off_idx) {
      int off = nyr_lower_int_value(b, e->as.call.args.data[off_idx].val);
      if (off < 0)
        return -1;
      if (!nyr_index_fin_bound_elision(b, e, off_idx, buf_byte_len))
        nyr_emit_bounds_check(b, addr, off, buf_byte_len);
      addr = nyr_emit_add_i64(b, addr, off);
      if (addr < 0)
        return -1;
    }
    if (!nyr_emit_store_i64(b, addr, value))
      return -1;
    return nyr_emit_const(b, 0);
  }
  ny_native_leaf_kind_t layout_kind = ny_native_leaf_kind(leaf);
  if (layout_kind != NY_NATIVE_LEAF_NONE && !nyr_user_defined_fn(b, name) &&
      (layout_kind == NY_NATIVE_LEAF_LAYOUT_SIZE ||
       layout_kind == NY_NATIVE_LEAF_LAYOUT_ALIGN ||
       layout_kind == NY_NATIVE_LEAF_LAYOUT_OFFSET) &&
      ((layout_kind == NY_NATIVE_LEAF_LAYOUT_OFFSET &&
        e->as.call.args.len == 2) ||
       (layout_kind != NY_NATIVE_LEAF_LAYOUT_OFFSET &&
        e->as.call.args.len == 1)) &&
      e->as.call.args.data[0].val &&
      e->as.call.args.data[0].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[0].val->as.literal.kind == NY_LIT_STR &&
      (layout_kind != NY_NATIVE_LEAF_LAYOUT_OFFSET ||
       (e->as.call.args.data[1].val &&
        e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR))) {
    const char *layout = e->as.call.args.data[0].val->as.literal.as.s.data;
    if (layout_kind == NY_NATIVE_LEAF_LAYOUT_OFFSET) {
      const char *field = e->as.call.args.data[1].val->as.literal.as.s.data;
      size_t offset = 0;
      if (layout && field &&
          nyr_ast_layout_query(b, layout, field, NULL, NULL, &offset)) {
        if (!nyr_size_fits_i64(offset)) {
          return nyr_fail(b, "native NYIR lower: layout offset overflows i64"),
                 -1;
        }
        return nyr_emit_const(b, (int64_t)offset);
      }
      return nyr_fail(b, "native NYIR lower: unknown layout field '%s.%s'",
                      layout ? layout : "", field ? field : ""),
             -1;
    }
    size_t size = 0;
    size_t align = 0;
    if (!layout ||
        !nyr_ast_layout_query(b, layout, NULL, &size, &align, NULL)) {
      return nyr_fail(b, "native NYIR lower: unknown layout '%s'",
                      layout ? layout : ""),
             -1;
    }
    size_t layout_value =
        layout_kind == NY_NATIVE_LEAF_LAYOUT_ALIGN ? align : size;
    if (!nyr_size_fits_i64(layout_value)) {
      return nyr_fail(b, "native NYIR lower: layout value overflows i64"), -1;
    }
    return nyr_emit_const(b, (int64_t)layout_value);
  }
  bool store_one_layout_field =
      leaf && ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_LAYOUT_FIELD_STORE;
  if (leaf &&
      (ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_LAYOUT_STORE ||
       store_one_layout_field) &&
      e->as.call.args.len >= 3 && e->as.call.args.data[1].val &&
      e->as.call.args.data[1].val->kind == NY_E_LITERAL &&
      e->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR) {
    const char *layout = e->as.call.args.data[1].val->as.literal.as.s.data;
    size_t fields = 0;
    size_t selected_field = SIZE_MAX;
    const ny_layout_field_list *generic_fields = NULL;
    if (layout) {
      const stmt_t *decl = nyr_find_layout_in_prog(b, b->prog, layout);
      if (decl && (decl->kind == NY_S_LAYOUT || decl->kind == NY_S_STRUCT)) {
        generic_fields = decl->kind == NY_S_LAYOUT ? &decl->as.layout.fields
                                                   : &decl->as.struc.fields;
        fields = generic_fields->len;
      }
    }
    if (!generic_fields)
      fields = 0;
    if (store_one_layout_field && e->as.call.args.len == 4 && generic_fields &&
        e->as.call.args.data[2].val &&
        e->as.call.args.data[2].val->kind == NY_E_LITERAL &&
        e->as.call.args.data[2].val->as.literal.kind == NY_LIT_STR) {
      const char *field_name =
          e->as.call.args.data[2].val->as.literal.as.s.data;
      for (size_t i = 0; i < fields; ++i) {
        if (generic_fields->data[i].name && field_name &&
            strcmp(generic_fields->data[i].name, field_name) == 0) {
          selected_field = i;
          break;
        }
      }
    }
    if (!fields || (store_one_layout_field
                        ? e->as.call.args.len != 4 || selected_field == SIZE_MAX
                        : e->as.call.args.len != fields + 2)) {
      return nyr_fail(b, "native NYIR lower: invalid layout store '%s'",
                      layout ? layout : ""),
             -1;
    }
    int base = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (base < 0)
      return -1;
    for (size_t i = 0; i < fields; ++i) {
      if (store_one_layout_field && i != selected_field)
        continue;
      const expr_t *field_expr =
          e->as.call.args.data[store_one_layout_field ? 3 : i + 2].val;
      int64_t raw_offset = 0;
      size_t field_size = 8;
      const char *field_type = NULL;
      if (generic_fields) {
        const layout_field_t *field = &generic_fields->data[i];
        size_t field_align = 0;
        size_t field_offset = 0;
        if (field->is_array || !field->name ||
            !nyr_ast_layout_query(b, layout, field->name, &field_size,
                                  &field_align, &field_offset)) {
          return nyr_fail(b,
                          "native NYIR lower: unsupported "
                          "store_layout field %zu in '%s'",
                          i, layout ? layout : ""),
                 -1;
        }
        if (!nyr_size_fits_i64(field_offset)) {
          return nyr_fail(b, "native NYIR lower: store_layout field "
                             "offset overflows i64"),
                 -1;
        }
        raw_offset = (int64_t)field_offset;
        field_type = field->type_name;
      }
      /* Lower a floating layout field in its destination ABI from the
       * beginning.  In particular, a generated shape conversion is a
       * conditional dynamic read; lowering it as i64 and converting the
       * merged word afterward cannot distinguish a tagged arm from an IEEE
       * payload. */
      int value = (field_type && ny_native_type_name_is_f64(field_type))
                      ? nyr_lower_f64_arg(b, field_expr)
                      : nyr_lower_expr(b, field_expr);
      int offset = nyr_emit_const(b, raw_offset);
      int indexed_offset = nyr_emit_const(b, rt_tag_v(raw_offset));
      if (value < 0 || offset < 0 || indexed_offset < 0)
        return -1;
      bool wide = field_size >= 8;
      if (generic_fields && field_type &&
          ny_native_type_name_is_f64(field_type)) {
        int addr = nyr_emit_add_i64(b, base, offset);
        int f64 = value;
        bool field_any = nyr_expr_is_any(b, field_expr);
        bool field_f64 = nyr_expr_is_f64(b, field_expr);
        bool field_f32 = nyr_expr_is_f32(b, field_expr);
        const nyr_local_t *field_local = NULL;
        if (field_expr && field_expr->kind == NY_E_IDENT &&
            field_expr->as.ident.name) {
          field_local = nyr_find_local_nearest(b, field_expr->as.ident.name);
          /* The local record describes the physical slot that was actually
           * emitted.  It outranks stale expression semantics, especially for
           * generated shape locals whose dynamic source was narrowed to an
           * f64 layout field. */
          if (field_local && field_local->is_any) {
            field_any = true;
            field_f64 = false;
            field_f32 = false;
          } else if (field_local && field_local->is_f64) {
            field_any = false;
            field_f64 = true;
            field_f32 = false;
          } else if (field_local && field_local->is_f32) {
            field_any = false;
            field_f64 = false;
            field_f32 = true;
          }
        }
        if (field_f32) {
          f64 = nyr_emit_f32_to_f64(b, value);
        } else if (field_any) {
          f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                      NYIR_SCALAR_F64);
        } else if (!field_f64) {
          f64 = nyr_emit_i64_to_f64(b, value);
        }
        if (addr < 0 || f64 < 0 || !nyr_emit_store_f64(b, addr, f64))
          return -1;
        continue;
      }
      if (generic_fields && field_type &&
          ny_native_type_name_is_f32(field_type)) {
        int f64 = nyr_expr_is_f32(b, field_expr) ? nyr_emit_f32_to_f64(b, value)
                  : nyr_expr_is_any(b, field_expr)
                      ? nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1,
                                              1, NYIR_SCALAR_F64)
                      : value;
        if (f64 < 0 || nyr_emit_runtime_call(b, "rt_store32_f64", base,
                                             indexed_offset, f64, 3, 0) < 0)
          return -1;
        continue;
      }
      if (generic_fields && field_type &&
          !ny_native_type_name_is_f64(field_type) &&
          !ny_native_type_name_is_f32(field_type) &&
          strcmp(field_type, "bool") != 0 && nyr_expr_is_any(b, field_expr)) {
        value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      if (generic_fields && field_type && strcmp(field_type, "bool") == 0) {
        /* Layout bool fields are one raw byte.  The source bool ABI may be a
         * tagged immediate (2/8) or an internal raw 0/1 value; normalize both
         * forms without guessing from the low bit. */
        int zero = nyr_emit_const(b, 0);
        int false_imm = nyr_emit_const(b, NY_IMM_FALSE);
        int is_zero = (zero < 0 || value < 0)
                          ? -1
                          : nyr_emit_cmp_i64(b, NYIR_CMP_EQ, value, zero);
        int is_false = (false_imm < 0 || value < 0)
                           ? -1
                           : nyr_emit_cmp_i64(b, NYIR_CMP_EQ, value, false_imm);
        int falsy = (is_zero < 0 || is_false < 0)
                        ? -1
                        : nyr_emit_binop(b, NYIR_OR_I64, is_zero, is_false);
        int raw_slot = b->next_local_slot++;
        int false_label = b->next_label++;
        int end_label = b->next_label++;
        if (falsy < 0 || raw_slot < 0 || !nyr_emit_br_if(b, falsy, false_label))
          return -1;
        int one = nyr_emit_const(b, 1);
        if (one < 0 || !nyr_store_local_value(b, raw_slot, one) ||
            !nyr_emit_br(b, end_label) || !nyr_emit_label(b, false_label) ||
            !nyr_store_local_value(b, raw_slot, zero) ||
            !nyr_emit_label(b, end_label))
          return -1;
        int raw_bool = nyr_load_local_value(b, raw_slot);
        int addr = raw_bool < 0 ? -1 : nyr_emit_add_i64(b, base, offset);
        if (addr < 0 || !nyr_emit_store8(b, addr, raw_bool))
          return -1;
        continue;
      }
      if (generic_fields && !wide && field_type &&
          !ny_native_type_name_is_str(field_type)) {
        int one = nyr_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = value,
                                                        .b = one});
        value = shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
        if (value < 0)
          return -1;
      }
      /* Preserve the declared field width.  Using the 32-bit helper for every
       * narrow field overwrites adjacent packed fields and makes the result
       * depend on declaration order. */
      const char *store = wide              ? "rt_store64_idx"
                          : field_size == 1 ? "rt_store8_idx"
                          : field_size == 2 ? "rt_store16_idx"
                                            : "rt_store32_idx";
      if (nyr_emit_runtime_call(b, store, base, indexed_offset, value, 3, 0) <
          0)
        return -1;
    }
    return nyr_emit_const(b, 0);
  }
  if (e->as.call.args.len > NYIR_CALL_MAX_ARGS) {
    return nyr_fail(b,
                    "native NYIR lower: call exceeds the maximum supported "
                    "argument count (%d)",
                    NYIR_CALL_MAX_ARGS),
           -1;
  }
  const char *saved_source_file = b->source_file;
  if (e->tok.filename)
    b->source_file = e->tok.filename;
  const stmt_t *semantic_callee_fn =
      e->semantic.canonical_callee_stmt &&
              e->semantic.canonical_callee_stmt->kind == NY_S_FUNC
          ? e->semantic.canonical_callee_stmt
          : NULL;
  size_t call_argc = e->as.call.args.len;
  bool direct_callee_accepts =
      nyr_native_user_fn_accepts_arity(direct_callee_fn, call_argc);
  bool semantic_callee_accepts =
      nyr_native_user_fn_accepts_arity(semantic_callee_fn, call_argc);
  const stmt_t *callee_fn =
      direct_callee_accepts
          ? direct_callee_fn
          : (semantic_callee_accepts
                 ? semantic_callee_fn
                 : (e->as.call.callee->kind == NY_E_IDENT
                        ? nyr_find_user_function_for_arity(b, name, call_argc)
                        : nyr_find_user_function(b, name)));
  if (callee_fn && name && e->tok.filename && callee_fn->tok.filename &&
      nyr_same_source_file(callee_fn->tok.filename, e->tok.filename) &&
      callee_fn->tok.line > e->tok.line &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *surface_name = e->as.call.callee->as.ident.name;
    const stmt_t *imported = nyr_find_imported_function(b, surface_name);
    if (nyr_native_user_fn_accepts_arity(imported, e->as.call.args.len)) {
      callee_fn = imported;
      name = imported->as.fn.name;
    }
  }
  /* A semantic annotation can already be qualified with an unrelated
   * same-leaf declaration in a flattened stdlib source. Honor the caller's
   * explicit unqualified import when it accepts this call's arity. */
  if (e->as.call.callee->kind == NY_E_IDENT) {
    const char *surface_name = e->as.call.callee->as.ident.name;
    const stmt_t *imported = nyr_find_imported_function(b, surface_name);
    const char *callee_module =
        callee_fn
            ? ny_native_fn_module(ny_native_fn_index_for(b), b->prog, callee_fn)
            : NULL;
    const char *imported_module =
        imported
            ? ny_native_fn_module(ny_native_fn_index_for(b), b->prog, imported)
            : NULL;
    bool same_source_user_fn =
        callee_fn && !ny_is_stdlib_tok(callee_fn->tok) && e->tok.filename &&
        callee_fn->tok.filename &&
        nyr_same_source_file(callee_fn->tok.filename, e->tok.filename);
    if (!same_source_user_fn &&
        nyr_native_user_fn_accepts_arity(imported, e->as.call.args.len) &&
        (!callee_fn || !callee_module || !imported_module ||
         strcmp(callee_module, imported_module) != 0)) {
      callee_fn = imported;
      name = imported->as.fn.name;
    }
  }
  /* An unqualified call inside a module is first resolved in that module's
   * lexical namespace.  Leaf-only lookup is intentionally ambiguous when a
   * package exports the same helper from multiple modules (for example
   * `core.os.is_dir` and `core.os.fs.is_dir`), but that ambiguity must not
   * erase a valid sibling call.  Keep the resolved declaration's canonical
   * name so the emitted call and reachability graph use the same target. */
  char scoped_call_name[512];
  char scope_module_buf[512];
  if (name && !strchr(name, '.') && b->current_fn_name) {
    const char *scope_module = b->module_name;
    const char *scope_end = NULL;
    if (!scope_module || !*scope_module) {
      scope_end = strrchr(b->current_fn_name, '.');
      if (scope_end && scope_end != b->current_fn_name) {
        size_t module_len = (size_t)(scope_end - b->current_fn_name);
        if (module_len < sizeof(scope_module_buf)) {
          memcpy(scope_module_buf, b->current_fn_name, module_len);
          scope_module_buf[module_len] = '\0';
          scope_module = scope_module_buf;
        }
      }
    }
    if (scope_module && *scope_module) {
      int n = snprintf(scoped_call_name, sizeof(scoped_call_name), "%s.%s",
                       scope_module, name);
      if (n > 0 && (size_t)n < sizeof(scoped_call_name)) {
        const stmt_t *scoped = nyr_find_user_function_for_arity(
            b, scoped_call_name, e->as.call.args.len);
        if (!scoped)
          scoped = nyr_find_user_function(b, scoped_call_name);
        const char *scoped_module =
            scoped ? ny_native_fn_module(ny_native_fn_index_for(b), b->prog,
                                         scoped)
                   : NULL;
        const char *callee_module =
            callee_fn ? ny_native_fn_module(ny_native_fn_index_for(b), b->prog,
                                            callee_fn)
                      : NULL;
        /* Semantic call annotations may carry a leaf-only binding from a
         * different module when stdlib modules share one generated source
         * file. A declaration in the caller's own module is the lexical
         * binding and must win in that case. */
        if (scoped &&
            nyr_native_user_fn_accepts_arity(scoped, e->as.call.args.len) &&
            (!callee_fn || (scoped_module && callee_module &&
                            strcmp(scoped_module, callee_module) != 0))) {
          callee_fn = scoped;
          name = scoped->as.fn.name ? scoped->as.fn.name : scoped_call_name;
        }
      }
    }
  }
  nyr_local_t *callable_local = NULL;
  if (e->as.call.callee->kind == NY_E_IDENT)
    callable_local = nyr_find_local(b, e->as.call.callee->as.ident.name);
  const expr_t *callable_global =
      e->as.call.callee->kind == NY_E_IDENT
          ? nyr_find_top_level_value(b, e->as.call.callee->as.ident.name)
          : NULL;
  /* A same-source function definition shadows a foreign top-level value.
   * Imported `#main`/self-test blocks register short bindings (such as `p`
   * or `q`) that are visible to user programs; without this the foreign
   * initializer drives the indirect-callable decision below and a user call
   * lowers to a dynamic load of unrelated module state. */
  if (callee_fn && callable_global && !callee_fn->as.fn.is_extern &&
      !callee_fn->as.fn.link_name &&
      !nyr_same_source_file(callable_global->tok.filename, b->source_file))
    callable_global = NULL;
  ny_native_lambda_entry_t *local_lambda =
      callable_local && callable_local->callable_expr
          ? ny_native_lambda_find(ny_native_lambda_cache_for(b),
                                  callable_local->callable_expr)
          : NULL;
  ny_native_lambda_entry_t *global_lambda =
      callable_global ? ny_native_lambda_find(ny_native_lambda_cache_for(b),
                                              callable_global)
                      : NULL;
  if (!callee_fn && global_lambda && global_lambda->fn && global_lambda->name) {
    callee_fn = global_lambda->fn;
    name = global_lambda->name;
    leaf = ny_name_leaf(name);
  }
  bool known_raw_callable =
      (callable_local && callable_local->callable_name) ||
      (callable_global && callable_global->kind == NY_E_IDENT &&
       callable_global->as.ident.name &&
       nyr_find_user_function(b, callable_global->as.ident.name));
  bool indirect_callable =
      indirect_lambda || indirect_sequence_get || indirect_sequence_index ||
      indirect_dictionary_get ||
      (!callee_fn && !nyr_user_defined_fn(b, name) && !known_raw_callable &&
       e->as.call.callee->semantic.rep == NY_SEM_REP_CLOSURE) ||
      (local_lambda && local_lambda->capture_count > 0) ||
      (!callee_fn && !nyr_user_defined_fn(b, name) && !known_raw_callable &&
       callable_global &&
       callable_global->semantic.rep == NY_SEM_REP_CLOSURE) ||
      /* A top-level value initialized by a function call is still a
       * callable closure, even when inference has not propagated the
       * closure representation through the initializer expression. */
      (!callee_fn && !known_raw_callable && callable_global &&
       callable_global->kind == NY_E_CALL) ||
      (!callee_fn && !known_raw_callable && callable_local && callable_global &&
       callable_global->kind == NY_E_CALL) ||
      /* Lambda entry points use the expanded callback ABI even without a
       * captured environment.  Directly calling a zero-capture lambda with
       * raw source arguments skipped its value/length/tag normalization and
       * made tagged lambda parameters decode the range-loop raw integer. */
      local_lambda || global_lambda ||
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
  if (getenv("NY_DBG_CALL"))
    fprintf(stderr, "[call-entry] arity=%zu indirect=%d name=%s\n",
            e->as.call.args.len, (int)indirect_callable, name ? name : "?");
  if (indirect_callable) {
    /*
     * Dynamic-callback adapters exist through arity fifteen
     * (rt_call_any1..15 with the matching ABI table rows); the fail below
     * must return -1, not nyr_fail's bool: inside this
     * int-returning lowerer a plain `nyr_fail(...)` yields
     * 0 — a VALID SSA value — so the failed call silently degraded to
     * "result is register 0" (the >=5-arg garbage).
     */
    if (e->as.call.args.len > 15) {
      nyr_fail(b,
               "native NYIR: dynamic calls support at most fifteen arguments");
      return -1;
      return -1;
    }
    int callable = -1;
    callable = nyr_lower_expr(b, callee_expr);
    if (callable < 0)
      return -1;
    /* Generated lambda entry points use the callback ABI even when a
     * zero-capture lambda is invoked directly.  Mark the callable so
     * rt_call_any1/2 forwards the canonical tagged value instead of
     * unboxing it for a raw C callback; the lambda body itself performs the
     * typed/raw conversion selected by its parameter representation. */
    /* Every value reaching this branch is an indirect/captured callable, so
     * its invocation crosses the dynamic callback ABI.  Marking only syntax
     * lambdas missed callable parameters after generic functions were
     * specialized/inlined (for example compose(f, g, x)); those generated
     * lambdas then received an unboxed scalar while their body used rt_any_*.
     * The marker is idempotent and also preserves closure objects. */
    const stmt_t *returned_callable =
        nyr_callable_result_fn(b, e->as.call.callee, 0);
    /* A dynamic `.get(index, fallback)` call may return either an element or
     * its fallback. The fallback's declared signature supplies the result
     * ABI when HM has erased the returned callable to `any`; use that fact to
     * keep raw scalar callback results from being decoded as tagged values.
     */
    if (!returned_callable && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_MEMCALL &&
        e->as.call.callee->as.memcall.args.len == 2) {
      const call_arg_t *fallback_arg =
          &e->as.call.callee->as.memcall.args.data[1];
      const expr_t *fallback_expr = fallback_arg->val;
      if (fallback_expr && fallback_expr->kind == NY_E_IDENT &&
          fallback_expr->as.ident.name)
        returned_callable =
            nyr_find_user_function(b, fallback_expr->as.ident.name);
    }
    /* Same scalar contract as nyr_expr_has_raw_int_abi: a resolved
     * RAW_INT fact decides when it is coherent with the annotation, and the
     * 128-bit spellings keep their non-raw ABI. */
    bool raw_result = false;
    if (returned_callable) {
      if (returned_callable->as.fn.return_semantic.resolved &&
          returned_callable->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT &&
          ny_sem_rep_coherent_with_type(
              returned_callable->as.fn.return_type,
              returned_callable->as.fn.return_semantic.rep) &&
          !ny_native_type_name_is_128bit_int(
              returned_callable->as.fn.return_type))
        raw_result = true;
      else if (returned_callable->as.fn.return_type)
        raw_result =
            ny_native_type_name_is_int(returned_callable->as.fn.return_type);
    }
    /* An erased lookup result can hide the selected function declaration
     * from the local callable resolver. When the call itself has a resolved
     * raw-int result, carry that ABI fact across the dynamic callback bridge
     * so odd results are boxed before the caller decodes them. */
    if (!raw_result && e->semantic.resolved &&
        e->semantic.rep == NY_SEM_REP_RAW_INT &&
        ny_sem_rep_coherent_with_type(e->semantic.resolved_type_name,
                                      e->semantic.rep) &&
        !ny_native_type_name_is_128bit_int(e->semantic.resolved_type_name))
      raw_result = true;
    /*
     * A named function value whose parameters all lower as raw scalars
     * keeps the plain raw ABI through the whole arity range: the
     * rt_call1..4 bridges untag scalar arguments themselves and normalize
     * the raw result.  Marking such a body TAGGED_ARGS (the any-trampoline
     * contract) made a typed `int` body read the tagged word — the
     * raw_result+int contract gap (21 via a raw-marked callback computed
     * 86 = 2 * tagged(21)).  The plain raw-result marker keeps result
     * normalization (boxing odd raw scalars) while letting the bridges
     * untag.
     */
    bool all_raw_callable = false;
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      const stmt_t *callee_fn_value =
          nyr_find_user_function(b, e->as.call.callee->as.ident.name);
      all_raw_callable =
          callee_fn_value && !callee_fn_value->as.fn.is_variadic &&
          callee_fn_value->as.fn.params.len == e->as.call.args.len;
      for (size_t pi = 0;
           all_raw_callable && pi < callee_fn_value->as.fn.params.len; ++pi) {
        const param_t *pp = &callee_fn_value->as.fn.params.data[pi];
        bool param_raw_int =
            pp->type ? ny_native_type_name_is_int(pp->type)
                     : nyr_untyped_param_is_raw(callee_fn_value, pp->name);
        /* A resolved RAW_INT fact also qualifies when it agrees with the
         * annotation; 128-bit spellings keep their non-raw ABI. */
        if (!param_raw_int && pp->semantic.resolved &&
            pp->semantic.rep == NY_SEM_REP_RAW_INT &&
            ny_sem_rep_coherent_with_type(pp->type, pp->semantic.rep) &&
            !ny_native_type_name_is_128bit_int(pp->type))
          param_raw_int = true;
        if (!param_raw_int)
          all_raw_callable = false;
      }
    }
    if (all_raw_callable && e->as.call.args.len <= 4) {
      callable = nyr_emit_runtime_call(
          b,
          callback_word_abi
              ? (raw_result ? "rt_mark_dynamic_callable_word_args_raw_result"
                            : "rt_mark_dynamic_callable_word_args")
              : (raw_result ? "rt_mark_dynamic_callable_raw_result"
                            : "rt_mark_dynamic_callable"),
          callable, -1, -1, 1, 0);
      if (callable < 0)
        return -1;
      int raw_argv[4] = {-1, -1, -1, -1};
      for (size_t i = 0; i < e->as.call.args.len; ++i) {
        raw_argv[i] = nyr_lower_expr(b, e->as.call.args.data[i].val);
        if (raw_argv[i] < 0)
          return -1;
      }
      const char *raw_bridge = e->as.call.args.len == 1   ? "rt_call1"
                               : e->as.call.args.len == 2 ? "rt_call2"
                               : e->as.call.args.len == 3 ? "rt_call3"
                                                          : "rt_call4";
      int raw_call_result =
          e->as.call.args.len == 1
              ? nyr_emit_runtime_call(b, raw_bridge, callable, raw_argv[0], -1,
                                      2, 0)
              : nyr_emit_runtime_call4(b, raw_bridge, callable, raw_argv[0],
                                       raw_argv[1], raw_argv[2], raw_argv[3],
                                       (int)e->as.call.args.len + 1, 0);
      if (raw_call_result < 0)
        return -1;
      return raw_call_result;
    }
    bool callable_local_has_abi =
        callable_local && callable_local->semantic_rep == NY_SEM_REP_CLOSURE;
    /* A known closure already carries its callback ABI beside the closure
     * object. Sending it through a generic callable marker can reinterpret
     * the object as a raw code pointer at a returned-fnptr boundary, dropping
     * the captured environment. Keep the value intact; rt_call_anyN reads
     * the closure flag record directly. */
    if (!callable_local_has_abi) {
      const char *callable_marker =
          callback_word_abi
              ? (raw_result ? "rt_mark_dynamic_callable_word_args_raw_result"
                            : "rt_mark_dynamic_callable_word_args")
              : (raw_result ? "rt_mark_dynamic_callable_tagged_args_raw_result"
                            : "rt_mark_dynamic_callable_tagged_args");
      callable =
          nyr_emit_runtime_call(b, callable_marker, callable, -1, -1, 1, 0);
      if (callable < 0)
        return -1;
    }
    int argv[15];
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        return nyr_fail(b, "native NYIR: captured closure calls "
                           "require positional arguments"),
               -1;
      }
      const expr_t *arg_expr = e->as.call.args.data[i].val;
      bool saved_index_for_any_call = b->index_for_any_call;
      b->index_for_any_call = arg_expr && arg_expr->kind == NY_E_INDEX;
      argv[i] = nyr_lower_expr(b, arg_expr);
      b->index_for_any_call = saved_index_for_any_call;
      if (argv[i] < 0)
        return -1;
      nyr_scalar_class_t physical_class = nyr_value_scalar_class(b, argv[i]);
      if (physical_class == NYIR_SCALAR_F64 ||
          physical_class == NYIR_SCALAR_F32) {
        int floating = physical_class == NYIR_SCALAR_F32
                           ? nyr_emit_f32_to_f64(b, argv[i])
                           : argv[i];
        int bits = floating < 0 ? -1
                                : nyr_emit_runtime_call(b, "rt_f64_bits",
                                                        floating, -1, -1, 1, 0);
        argv[i] = bits < 0
                      ? -1
                      : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                              1, NYIR_SCALAR_I64_TAGGED);
        if (argv[i] < 0)
          return -1;
        continue;
      }
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
      bool raw_sequence_value = raw_sequence_target && raw_sequence_method &&
                                (strcmp(raw_sequence_method, "get") == 0 ||
                                 strcmp(raw_sequence_method, "pop") == 0) &&
                                (nyr_expr_is_list(b, raw_sequence_target) ||
                                 nyr_expr_is_bytes(b, raw_sequence_target) ||
                                 nyr_expr_is_range(b, raw_sequence_target));
      /* rt_callN consumes the legacy tagged value ABI; native scalar closure
       * bodies themselves receive the unboxed value after the trampoline.
       * A local whose initializer was a seq/dyn-list index read stores the
       * CANONICAL dynamic word (rt_tbuf_index_any_raw), so the raw-int
       * semantic guess must not shift-tag it a second time: `def v = xs[i]`
       * handed find_if's predicate tagged(10) as tagged(tagged(10)) and the
       * predicate compared 21 > 15. */
      const nyr_local_t *scalar_local =
          arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
              ? nyr_find_local(b, arg_expr->as.ident.name)
              : NULL;
      bool scalar_local_arg = scalar_local &&
                              !scalar_local->raw_dynamic_index &&
                              scalar_local->semantic_rep == NY_SEM_REP_RAW_INT;
      bool callable_value_arg =
          arg_expr &&
          (arg_expr->kind == NY_E_LAMBDA || arg_expr->kind == NY_E_FN ||
           (arg_expr->kind != NY_E_IDENT && arg_expr->semantic.resolved &&
            arg_expr->semantic.rep == NY_SEM_REP_CLOSURE));
      if (arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
        const nyr_local_t *callable_arg_local =
            nyr_find_expr_local(b, arg_expr);
        callable_value_arg =
            callable_arg_local
                ? (callable_arg_local->callable_expr ||
                   callable_arg_local->callable_name ||
                   callable_arg_local->semantic_rep == NY_SEM_REP_CLOSURE)
                : (arg_expr->semantic.resolved &&
                   arg_expr->semantic.rep == NY_SEM_REP_CLOSURE) ||
                      nyr_find_user_function(b, arg_expr->as.ident.name) !=
                          NULL;
      }
      bool canonical_tagged_argument = physical_class == NYIR_SCALAR_I64_TAGGED;
      if (((!nyr_expr_is_any(b, arg_expr) && !canonical_tagged_argument) ||
           scalar_local_arg) &&
          !callable_value_arg && !nyr_expr_is_cstr(b, arg_expr) &&
          !nyr_expr_is_dict(b, arg_expr) && !nyr_expr_is_list(b, arg_expr) &&
          !nyr_expr_is_bytes(b, arg_expr) && !nyr_expr_is_range(b, arg_expr) &&
          !raw_sequence_value && !nyr_expr_is_f64(b, arg_expr) &&
          !nyr_expr_is_f32(b, arg_expr)) {
        int one = nyr_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = argv[i],
                                                        .b = one});
        argv[i] =
            shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
      }
      if (argv[i] < 0)
        return -1;
    }
    const ny_native_runtime_callback_abi_decl_t *callback_abi =
        ny_native_runtime_callback_abi_decl_for_arity(e->as.call.args.len);
    if (!callback_abi || callback_abi->source_arity != e->as.call.args.len ||
        !(callback_abi->callable_repr & NY_NATIVE_ABI_VALUE_CALLBACK_MARKED) ||
        callback_abi->callable_ownership != NY_NATIVE_ABI_BORROWED ||
        callback_abi->environment_ownership != NY_NATIVE_ABI_BORROWED ||
        callback_abi->result_ownership != NY_NATIVE_ABI_RESULT_ABI ||
        (callback_abi->source_arity > 0 &&
         (!(callback_abi->arg_repr & NY_NATIVE_ABI_VALUE_CALLBACK_MARKED) ||
          callback_abi->arg_ownership != NY_NATIVE_ABI_BORROWED)) ||
        !(callback_abi->result_repr & NY_NATIVE_ABI_VALUE_CALLBACK_MARKED)) {
      return nyr_fail(b, "native NYIR: callback ABI declaration "
                         "missing or inconsistent"),
             -1;
    }
    const char *call_symbol = callback_abi->symbol;
    if (e->as.call.args.len >= 5) {
      /*
       * Higher-arity dynamic callbacks dispatch through the
       * rt_call_any5..rt_call_any15 declaration rows; the trampoline word
       * count is source args + the callable slot.
       */
      int argv_full[17];
      argv_full[0] = callable;
      for (size_t ai = 0; ai < e->as.call.args.len && ai < 16; ++ai)
        argv_full[ai + 1] = argv[ai];
      int rn = nyr_emit_runtime_call_argv(b, call_symbol, argv_full,
                                          (int)e->as.call.args.len + 1,
                                          NYIR_SCALAR_I64_TAGGED);
      if (rn < 0)
        return -1;
      /* The any-trampolines return the canonical tagged dynamic value. */
      if (raw_result)
        rn = nyr_emit_runtime_call(b, "rt_any_to_i64", rn, -1, -1, 1,
                                   NYIR_SCALAR_I64_RAW);
      else if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F64)
        rn = nyr_emit_runtime_call(b, "rt_any_to_f64", rn, -1, -1, 1,
                                   NYIR_SCALAR_F64);
      if (rn < 0)
        return -1;
      return rn;
    }
    if (e->as.call.args.len >= 3) {
      int r4 = nyr_emit_runtime_call4(
          b, call_symbol, callable, argv[0], argv[1], argv[2], argv[3],
          (int)e->as.call.args.len + 1, NYIR_SCALAR_I64_TAGGED);
      if (r4 < 0)
        return -1;
      /* The any-trampolines return the canonical tagged dynamic value. */
      if (raw_result)
        r4 = nyr_emit_runtime_call(b, "rt_any_to_i64", r4, -1, -1, 1,
                                   NYIR_SCALAR_I64_RAW);
      else if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F64)
        r4 = nyr_emit_runtime_call(b, "rt_any_to_f64", r4, -1, -1, 1,
                                   NYIR_SCALAR_F64);
      if (r4 < 0)
        return -1;
      return r4;
    }
    int result = nyr_emit_runtime_call(b, call_symbol, callable, argv[0],
                                       argv[1], (int)e->as.call.args.len + 1,
                                       NYIR_SCALAR_I64_TAGGED);
    if (result < 0)
      return -1;
    /* Indirect calls cross the dynamic callback ABI, whose result is a
     * canonical tagged value.  The enclosing expression still has its own
     * statically selected representation; restore it at this boundary so a
     * scalar result used directly (rather than first stored in a local) is
     * handled exactly like the same call through a local binding. */
    bool indirect_any_callee = false;
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name) {
      /* An untyped callable parameter (for example `fn apply(f, v) { f(v) }`)
       * is itself an any/closure local: HM can specialize the call site to
       * RAW_INT while the runtime dispatch still returns the canonical tagged
       * word, so `apply(addu, 41) == 42` compared tagged 42 (85) against 42.
       */
      const nyr_local_t *callee_local =
          nyr_find_local(b, e->as.call.callee->as.ident.name);
      indirect_any_callee =
          callee_local &&
          (callee_local->is_any || callee_local->callable_expr ||
           callee_local->semantic_rep == NY_SEM_REP_CLOSURE);
    }
    if (raw_result ||
        (((e->as.call.callee && e->as.call.callee->kind == NY_E_CALL) ||
          indirect_any_callee) &&
         e->semantic.resolved && e->semantic.rep == NY_SEM_REP_RAW_INT))
      result = nyr_emit_runtime_call(b, "rt_any_to_i64", result, -1, -1, 1, 0);
    else if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F64)
      result = nyr_emit_runtime_call(b, "rt_any_to_f64", result, -1, -1, 1,
                                     NYIR_SCALAR_F64);
    return result;
  }
  /* A function-valued local is still a direct call when its initializer is a
   * non-capturing lambda.  Resolve it before ABI expansion so the generated
   * call uses the lambda's real signature and symbol. */
  if (!callee_fn && e->as.call.callee->kind == NY_E_IDENT) {
    nyr_local_t *local = nyr_find_local(b, e->as.call.callee->as.ident.name);
    if (local && local->callable_expr) {
      ny_native_lambda_entry_t *entry = ny_native_lambda_find(
          ny_native_lambda_cache_for(b), local->callable_expr);
      if (entry && entry->fn && entry->name) {
        callee_fn = entry->fn;
        name = entry->name;
        leaf = ny_name_leaf(name);
      }
    }
    if (!callee_fn && local && local->callable_name) {
      const stmt_t *aliased_fn =
          nyr_find_user_function(b, local->callable_name);
      if (aliased_fn) {
        callee_fn = aliased_fn;
        name = aliased_fn->as.fn.name;
        leaf = ny_name_leaf(name);
      }
    }
    /* Top-level named function aliases are materialized in the global data
     * table, but their immutable initializer still gives us the exact
     * callable signature.  Resolve that initializer before falling through
     * to an indirect/raw call, which NYIR intentionally does not model yet.
     */
    if (!callee_fn) {
      const expr_t *global =
          nyr_find_top_level_value(b, e->as.call.callee->as.ident.name);
      if (global && global->kind == NY_E_IDENT && global->as.ident.name) {
        const stmt_t *aliased_fn =
            nyr_find_user_function(b, global->as.ident.name);
        if (aliased_fn) {
          callee_fn = aliased_fn;
          name = aliased_fn->as.fn.name;
          leaf = ny_name_leaf(name);
        }
      }
    }
  }
  /* Expanded stdlib calls can preserve the provider module in an attached
   * method's canonical name (for example
   * `core.math.crypto.encoding.base.merge`) even though the receiver is a
   * native dictionary.  Resolve this representation before ordinary function
   * lookup; otherwise the valid dictionary operation is reported as a missing
   * function in the provider module. */
  if (leaf && strcmp(leaf, "merge") == 0 &&
      ((e->as.call.args.len == 2 && !e->as.call.args.data[0].name &&
        !e->as.call.args.data[1].name &&
        nyr_expr_is_dict(b, e->as.call.args.data[0].val)) ||
       (e->as.call.args.len == 1 && name &&
        strcmp(name, "core.math.crypto.encoding.base.merge") == 0 &&
        !e->as.call.args.data[0].name && nyr_find_local(b, "base") != NULL))) {
    expr_t base_ident = {.kind = NY_E_IDENT, .tok = e->tok};
    base_ident.as.ident.name = "base";
    /* Canonical attached method calls carry the receiver as argument 0 and
     * the merge source as argument 1.  The one-argument compatibility form
     * carries only the source and uses the local `base` receiver above. */
    const expr_t *other_expr = e->as.call.args.len == 2
                                   ? e->as.call.args.data[1].val
                                   : e->as.call.args.data[0].val;
    int dict = e->as.call.args.len == 2
                   ? nyr_lower_expr(b, e->as.call.args.data[0].val)
                   : nyr_lower_expr(b, &base_ident);
    int other = nyr_lower_expr(b, other_expr);
    return dict < 0 || other < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_dict_merge_raw", dict, other, -1,
                                       2, nyr_value_scalar_class(b, dict));
  }
  /* The public core.os wrappers are interpreter-ABI functions (`any`), but
   * their only native operation is the scalar runtime sleep primitive.  A
   * native caller must not pass the wrapper's expanded value/length/tag
   * tuple to a NYIR function body: that body eventually reaches the tagged
   * runtime entry through the generic call gate.  Collapse the wrapper at
   * the native boundary and pass the raw scalar directly. */
  if (e->as.call.args.len == 1 && !e->as.call.args.data[0].name && leaf &&
      (canonical_stdlib_callee ||
       (callee_fn && ny_is_stdlib_tok(callee_fn->tok))) &&
      (strcmp(leaf, "msleep") == 0 || strcmp(leaf, "sleep") == 0)) {
    int value = nyr_lower_expr(b, e->as.call.args.data[0].val);
    if (value < 0)
      return -1;
    if (strcmp(leaf, "sleep") == 0) {
      int scale = nyr_emit_const(b, 1000);
      value = scale < 0 ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                            .dst = -1,
                                                            .a = value,
                                                            .b = scale});
    }
    return value < 0
               ? -1
               : nyr_emit_runtime_call(b, "rt_msleep_ms", value, -1, -1, 1, 0);
  }
  if (leaf && ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_LAYOUT_LOAD &&
      e->as.call.args.len == 3 && e->as.call.args.data[1].val &&
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
    if (!layout || !field ||
        !nyr_ast_layout_query(b, layout, field, &field_size, &field_align,
                              &field_offset)) {
      return nyr_fail(b, "native NYIR lower: unknown layout field '%s.%s'",
                      layout ? layout : "", field ? field : ""),
             -1;
    }
    if (!nyr_size_fits_tagged_i64(field_offset)) {
      return nyr_fail(b, "native NYIR lower: load_layout field "
                         "offset overflows tagged i64"),
             -1;
    }
    offset = (int64_t)field_offset;
    wide = field_size >= 8;
    const char *field_type = NULL;
    const stmt_t *layout_stmt = nyr_find_layout_in_prog(b, b->prog, layout);
    if (layout_stmt && (layout_stmt->kind == NY_S_LAYOUT ||
                        layout_stmt->kind == NY_S_STRUCT)) {
      const ny_layout_field_list *layout_fields =
          layout_stmt->kind == NY_S_LAYOUT ? &layout_stmt->as.layout.fields
                                           : &layout_stmt->as.struc.fields;
      for (size_t fi = 0; fi < layout_fields->len; ++fi) {
        if (layout_fields->data[fi].name &&
            strcmp(layout_fields->data[fi].name, field) == 0) {
          field_type = layout_fields->data[fi].type_name;
          break;
        }
      }
    }
    bool load_result_f64 = field_type && ny_native_type_name_is_f64(field_type);
    if (!load_result_f64 && b->current_fn_name) {
      const stmt_t *owner_fn = nyr_find_user_function(b, b->current_fn_name);
      load_result_f64 = owner_fn && owner_fn->as.fn.return_type &&
                        ny_native_type_name_is_f64(owner_fn->as.fn.return_type);
    }
    int base = nyr_lower_expr(b, e->as.call.args.data[0].val);
    int off = nyr_emit_const(b, rt_tag_v(offset));
    if (base < 0 || off < 0)
      return -1;
    const char *load = wide              ? "rt_load64_idx"
                       : field_size == 1 ? "rt_load8_idx"
                       : field_size == 2 ? "rt_load16_idx"
                                         : "rt_load32_idx";
    int loaded = nyr_emit_runtime_call(b, load, base, off, -1, 2, 0);
    if (loaded < 0)
      return loaded;
    if (wide) {
      if (load_result_f64) {
        int boxed =
            nyr_emit_runtime_call(b, "rt_flt_box_val", loaded, -1, -1, 1, 0);
        return boxed < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_any_to_f64", boxed, -1,
                                                 -1, 1, NYIR_SCALAR_F64);
      }
      /* A typed integer-returning accessor uses the raw scalar function ABI;
       * only a generic load_layout expression crosses as a boxed `any`. */
      if (field_type && ny_native_type_name_is_int(field_type) &&
          b->return_type && ny_native_type_name_is_int(b->return_type))
        return loaded;
      if (field_type && ny_native_type_name_is_int(field_type))
        return nyr_emit_runtime_call(b, "rt_box_i64", loaded, -1, -1, 1,
                                     NYIR_SCALAR_I64_TAGGED);
      return loaded;
    }
    int one = nyr_emit_const(b, 1);
    if (one < 0)
      return -1;
    int raw =
        nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                          .dst = -1,
                                          .a = loaded,
                                          .b = one,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
    if (raw < 0)
      return -1;
    if (field_type && ny_native_type_name_is_f32(field_type)) {
      int tagged_bits = nyr_emit_runtime_call(b, "rt_tag", raw, -1, -1, 1,
                                              NYIR_SCALAR_I64_TAGGED);
      int boxed =
          tagged_bits < 0
              ? -1
              : nyr_emit_runtime_call(b, "rt_flt_box_val32", tagged_bits, -1,
                                      -1, 1, NYIR_SCALAR_I64_TAGGED);
      if (boxed < 0)
        return -1;
      if (b->return_type && ny_native_type_name_is_f32(b->return_type)) {
        int f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", boxed, -1, -1, 1,
                                        NYIR_SCALAR_F64);
        return f64 < 0 ? -1 : nyr_emit_f64_to_f32(b, f64);
      }
      if (load_result_f64)
        return nyr_emit_runtime_call(b, "rt_any_to_f64", boxed, -1, -1, 1,
                                     NYIR_SCALAR_F64);
      return boxed;
    }
    if (field_type && strcmp(field_type, "bool") == 0) {
      int bool_zero = nyr_emit_const(b, 0);
      int is_false =
          bool_zero < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_EQ, raw, bool_zero);
      int false_value = nyr_emit_const(b, NY_IMM_FALSE);
      int true_value = nyr_emit_const(b, NY_IMM_TRUE);
      int bool_slot = b->next_local_slot++;
      int false_label = b->next_label++;
      int bool_end_label = b->next_label++;
      if (is_false < 0 || false_value < 0 || true_value < 0 || bool_slot < 0 ||
          !nyr_emit_br_if(b, is_false, false_label) ||
          !nyr_store_local_value(b, bool_slot, true_value) ||
          !nyr_emit_br(b, bool_end_label) || !nyr_emit_label(b, false_label) ||
          !nyr_store_local_value(b, bool_slot, false_value) ||
          !nyr_emit_label(b, bool_end_label))
        return -1;
      return nyr_load_local_value(b, bool_slot);
    }
    /* load32_idx exposes an unsigned machine word.  A signed i32 layout
     * field must sign-extend after untagging; otherwise -1 becomes
     * 4294967295 at the typed accessor boundary. */
    bool signed32 = field_type &&
                    (!strcmp(field_type, "i32") || !strcmp(field_type, "s32"));
    if (!signed32)
      return nyr_emit_runtime_call(b, "rt_tag", raw, -1, -1, 1,
                                   NYIR_SCALAR_I64_TAGGED);
    int sign_bit = nyr_emit_const(b, INT64_C(1) << 31);
    int width = nyr_emit_const(b, INT64_C(1) << 32);
    int zero = nyr_emit_const(b, 0);
    int high =
        sign_bit < 0 ? -1 : nyr_emit_binop(b, NYIR_AND_I64, raw, sign_bit);
    int has_sign = (high < 0 || zero < 0)
                       ? -1
                       : nyr_emit_cmp_i64(b, NYIR_CMP_NE, high, zero);
    int result_slot = b->next_local_slot++;
    int negative_label = b->next_label++;
    int end_label = b->next_label++;
    if (has_sign < 0 || width < 0 || result_slot < 0 ||
        !nyr_emit_br_if(b, has_sign, negative_label) ||
        !nyr_store_local_value(b, result_slot, raw) ||
        !nyr_emit_br(b, end_label) || !nyr_emit_label(b, negative_label))
      return -1;
    int extended = nyr_emit_binop(b, NYIR_SUB_I64, raw, width);
    if (extended < 0 || !nyr_store_local_value(b, result_slot, extended) ||
        !nyr_emit_label(b, end_label))
      return -1;
    int result = nyr_load_local_value(b, result_slot);
    if (result < 0)
      return -1;
    /* Generated layout accessors have a declared scalar return ABI (for
     * example `fn TypeTextureInfo_load_index(ptr) i32`). Keep the decoded
     * value raw for that ABI. Generic `load_layout` expressions still return
     * a dynamic value and need the tagged bridge below. */
    if (b->return_type && ny_native_type_name_is_int(b->return_type))
      return result;
    int tagged = nyr_emit_runtime_call(b, "rt_tag", result, -1, -1, 1,
                                       NYIR_SCALAR_I64_TAGGED);
    if (tagged < 0)
      return -1;
    return tagged;
  }
  const stmt_t *callee_ext_decl =
      nyr_find_extern_decl_in_prog(b, b->prog, name, b->options);
  /*
   * Every non-extern callee returned by the program-wide finder is eligible
   * for native body collection, including functions nested in modules.  The
   * reachable-function pass owns that invariant; unresolved names are
   * rejected below before they can become undefined native symbols.  Builtin
   * C allocators and externs bypass this because they resolve by symbol.
   */
  bool runtime_builtin =
      ny_native_runtime_symbol_for_expr(name, leaf, e) != NULL;
  bool ffi_builtin = ny_native_ffi_symbol_name(name);
  bool declared_external =
      (b->externs && ny_extern_table_lookup(b->externs, name)) ||
      callee_ext_decl != NULL;
  /* A source struct/layout is a constructor declaration, not a function
   * symbol.  Resolve it before the unresolved-call diagnostic so the native
   * lowering block below gets a chance to materialize the record. */
  const stmt_t *struct_layout_ctor = NULL;
  if (!callee_fn && name && b->prog) {
    struct_layout_ctor = nyr_find_layout_in_prog(b, b->prog, name);
  }
  /* User-defined `impl` types are nominal views over their runtime value.
   * Their one-argument constructor is an identity operation (for example
   * `SelfBox({"value": 5})`).  An overloaded attached method may have won the
   * surface-name lookup first, so only keep that method when its formal type
   * actually accepts the argument's proven nominal type.  This preserves the
   * language's constructor identity without turning opaque handles into raw
   * integers at a later `any` boundary. */
  if (!callee_fn && e->as.call.args.len == 1 && !e->as.call.args.data[0].name &&
      nyr_find_impl_type(b, name)) {
    const expr_t *value = e->as.call.args.data[0].val;
    const char *value_type = nyr_expr_type_name(b, value);
    const char *formal_type = callee_fn && callee_fn->as.fn.params.len > 0
                                  ? callee_fn->as.fn.params.data[0].type
                                  : NULL;
    /* The identity is only valid when the constructor's RETURN type aliases
     * its param type — a true nominal view of the same runtime value.  A
     * conversion whose return type differs (int -> bigint: raw tagged word
     * vs heap object) must lower as a real call, or the caller receives the
     * unconverted raw word and every consumer halves odd values (the
     * bigint xor-zero regression). */
    const char *ctor_return = callee_fn ? callee_fn->as.fn.return_type : NULL;
    if (!callee_fn || !value_type || !formal_type ||
        !ny_native_type_alias_eq(value_type, formal_type) ||
        (ctor_return && !ny_native_type_alias_eq(value_type, ctor_return)))
      return nyr_lower_expr(b, value);
  }
  /* Refinement type membership predicate (e.g. Even(6) or Even(x)). */
  const stmt_t *refine_stmt = NULL;
  if (!callee_fn && name && b->prog && e->as.call.args.len == 1)
    refine_stmt = nyr_find_refine_in_prog(b, b->prog, name);
  if (refine_stmt && e->as.call.args.len == 1) {
    expr_t *arg_expr = e->as.call.args.data[0].val;
    ny_ct_fast_val_t fval = ny_ct_fast_none();
    if (ny_try_eval_comptime_expr_fast(NULL, (expr_t *)e, &fval, 0) &&
        fval.kind == NY_CT_FAST_BOOL) {
      return nyr_emit_const(b, fval.b ? 1 : 0);
    }
    expr_t *inst = ny_expr_subst(NULL, refine_stmt->as.refine.predicate,
                                 refine_stmt->as.refine.param, arg_expr);
    if (inst) {
      if (ny_try_eval_comptime_expr_fast(NULL, inst, &fval, 0) &&
          fval.kind == NY_CT_FAST_BOOL) {
        return nyr_emit_const(b, fval.b ? 1 : 0);
      }
      return nyr_lower_expr(b, inst);
    }
  }
  if (!callee_fn && !struct_layout_ctor && !runtime_builtin && !ffi_builtin &&
      !declared_external &&
      ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE) {
    const char *keyword = ny_keyword_typo_suggestion(name);
    return nyr_fail(
               b,
               "native NYIR lower: undefined symbol '%s'; no native body or "
               "supported "
               "external symbol at %s:%d (semantic call=%d callee=%s)%s%s%s",
               name, e->tok.filename ? e->tok.filename : "<source>",
               e->tok.line, (int)e->semantic.member_call_kind,
               e->semantic.canonical_callee ? e->semantic.canonical_callee
                                            : "<none>",
               keyword ? "; did you mean keyword '" : "",
               keyword ? keyword : "", keyword ? "'?" : ""),
           -1;
  }
  call_arg_t normalized_args[NYIR_CALL_MAX_ARGS] = {0};
  call_arg_t normalized_defaults[NYIR_CALL_MAX_ARGS] = {0};
  bool normalized_default_args[NYIR_CALL_MAX_ARGS] = {0};
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
        return nyr_fail(b, "native NYIR lower: named call requires a "
                           "bounded parameter list"),
               -1;
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
            return nyr_fail(b, "native NYIR lower: unknown named argument '%s'",
                            arg->name),
                   -1;
          }
        } else {
          while (param_idx < params->len && assigned[param_idx])
            ++param_idx;
          if (param_idx == params->len) {
            return nyr_fail(b, "native NYIR lower: too many call arguments"),
                   -1;
          }
          next_pos = param_idx + 1;
        }
        if (assigned[param_idx]) {
          return nyr_fail(b, "native NYIR lower: duplicate argument '%s'",
                          params->data[param_idx].name
                              ? params->data[param_idx].name
                          : arg->name ? arg->name
                                      : "<positional>"),
                 -1;
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
          normalized_default_args[i] = true;
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
      size_t min_required = 0;
      for (size_t p = 0; p < callee_fn->as.fn.params.len; ++p) {
        if (!callee_fn->as.fn.params.data[p].def)
          min_required = p + 1;
      }
      if (c->args.len < min_required && !callee_fn->as.fn.is_variadic) {
        return nyr_fail(b,
                        "native NYIR lower: call to '%s' expects at "
                        "least %zu argument(s), got %zu",
                        name ? name : "<anonymous>", min_required, c->args.len),
               -1;
      }
      if (callee_fn->as.fn.params.len > NYIR_CALL_MAX_ARGS) {
        return nyr_fail(b, "native NYIR lower: defaulted call has "
                           "too many parameters"),
               -1;
      }
      /* The trailing rest parameter of a variadic function is fed by the
       * caller's variadic tail, never by a default expression.  Expanding a
       * default slot for it materialized a NULL expression that the
       * variadic-tbuf builder then lowered ("missing expression" on every
       * zero-argument call like XInitThreads()). */
      size_t expand_params = callee_fn->as.fn.params.len;
      if (callee_fn->as.fn.is_variadic && expand_params > 0)
        expand_params -= 1;
      for (size_t i = 0; i < expand_params; ++i) {
        normalized_args[i].name = NULL;
        if (i < c->args.len)
          normalized_args[i].val = c->args.data[i].val;
        else {
          normalized_args[i].val = callee_fn->as.fn.params.data[i].def;
          normalized_default_args[i] = normalized_args[i].val != NULL;
        }
      }
      user_args = normalized_args;
      user_args_len = expand_params;
    }
  }
  if (callee_fn && user_args == normalized_args) {
    /*
     * Parameter defaults are evaluated in formal order. Later defaults may
     * refer to earlier formals, so substitute the normalized earlier
     * arguments before lowering a default at the call site. Keep the clones
     * in the function builder's scratch arena; the expressions are consumed
     * immediately and must not leak through the process-wide NULL arena.
     */
    for (size_t i = 0; i < user_args_len && i < callee_fn->as.fn.params.len;
         ++i) {
      if (!normalized_default_args[i] || !user_args[i].val)
        continue;
      expr_t *default_expr = user_args[i].val;
      for (size_t j = 0; j < i; ++j) {
        const char *formal = callee_fn->as.fn.params.data[j].name;
        if (formal && user_args[j].val)
          default_expr = ny_expr_subst(&b->default_arg_arena, default_expr,
                                       formal, user_args[j].val);
      }
      user_args[i].val = default_expr;
    }
  }
  if (callee_fn && callee_fn->as.fn.requires_cond && user_args) {
    expr_t *req = callee_fn->as.fn.requires_cond;
    for (size_t p_idx = 0;
         p_idx < callee_fn->as.fn.params.len && p_idx < user_args_len;
         ++p_idx) {
      if (callee_fn->as.fn.params.data[p_idx].name && user_args[p_idx].val) {
        req = ny_expr_subst(NULL, req, callee_fn->as.fn.params.data[p_idx].name,
                            user_args[p_idx].val);
        if (user_args[p_idx].val->kind == NY_E_IDENT &&
            user_args[p_idx].val->as.ident.name) {
          expr_t *literal =
              nyr_proof_global_literal(b, user_args[p_idx].val->as.ident.name);
          if (literal)
            req = ny_expr_subst(NULL, req, user_args[p_idx].val->as.ident.name,
                                literal);
        }
      }
    }
    ny_ct_fast_val_t fval = ny_ct_fast_none();
    bool proved = e->semantic.requires_proven_by_caller;
    if (!proved && !e->semantic.requires_check_custom_operator &&
        ny_try_eval_comptime_expr_fast(NULL, req, &fval, 0)) {
      if (fval.kind == NY_CT_FAST_BOOL)
        proved = fval.b;
      else if (fval.kind == NY_CT_FAST_INT)
        proved = (fval.i != 0);
    }
    if (!proved) {
      ny_diag_error(e->tok,
                    "precondition obligation failed: 'requires' condition is "
                    "false or unproved for native lowering");
      /*
       * The native lowerer does not carry the caller's typing/proof context,
       * so an unknown condition cannot be discharged here. Fail closed rather
       * than emitting a call whose contract may be false.
       */
      nyr_fail(b, "precondition obligation failed: 'requires' condition "
                  "is false or unproved for native lowering");
      return -1;
    }
  }
  if (callee_fn && callee_fn->as.fn.decreases_expr && user_args &&
      b->current_fn_name && callee_fn->as.fn.name &&
      strcmp(b->current_fn_name, callee_fn->as.fn.name) == 0) {
    bool descends = false;
    if (user_args_len > 0 && user_args[0].val) {
      expr_t *arg0 = (expr_t *)user_args[0].val;
      if (arg0->kind == NY_E_IDENT) {
        if (callee_fn->as.fn.decreases_expr->kind == NY_E_IDENT &&
            arg0->as.ident.name &&
            callee_fn->as.fn.decreases_expr->as.ident.name &&
            strcmp(arg0->as.ident.name,
                   callee_fn->as.fn.decreases_expr->as.ident.name) == 0) {
          descends = false;
        } else {
          descends = true;
        }
      } else if (arg0->kind == NY_E_BINARY && arg0->as.binary.op &&
                 strcmp(arg0->as.binary.op, "-") == 0) {
        descends = true;
      }
    }
    if (!descends) {
      ny_diag_error(e->tok,
                    "termination check failed: decreases(...) metric did not "
                    "strictly descend at recursive call to '%s'",
                    callee_fn->as.fn.name ? callee_fn->as.fn.name : "<anon>");
      return nyr_fail(b,
                      "termination check failed: decreases(...) metric did not "
                      "strictly descend at recursive call to '%s'",
                      callee_fn->as.fn.name ? callee_fn->as.fn.name : "<anon>"),
             -1;
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
    if (ret_type &&
        nyr_ast_layout_query(b, ret_type, NULL, &agg_size, &agg_align, NULL)) {
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

  /*
   * A Nytrix `struct`/`layout` declaration is a value constructor even
   * though it is not represented by a function declaration.  The legacy
   * LLVM path materializes the declared layout here; native NYIR must do the
   * same instead of falling through to an `ADDR_SYMBOL` for the type name.
   * Keep this driven by the AST layout and its field types so every declared
   * record gets the same ABI, including user-defined layouts.
   */
  const stmt_t *layout_ctor = NULL;
  if (!callee_fn && name && b->prog)
    layout_ctor = nyr_find_layout_in_prog(b, b->prog, name);
  if (!callee_fn && layout_ctor &&
      (layout_ctor->kind == NY_S_STRUCT || layout_ctor->kind == NY_S_LAYOUT)) {
    const ny_layout_field_list *fields = layout_ctor->kind == NY_S_STRUCT
                                             ? &layout_ctor->as.struc.fields
                                             : &layout_ctor->as.layout.fields;
    if (e->as.call.args.len != fields->len) {
      return nyr_fail(b,
                      "native layout constructor '%s' expects %zu field(s), "
                      "got %zu",
                      name, fields->len, e->as.call.args.len),
             -1;
    }
    size_t size = 0, align = 0;
    if (!nyr_ast_layout_query(b, name, NULL, &size, &align, NULL) ||
        size == 0) {
      return nyr_fail(b, "native layout constructor '%s' has no layout", name),
             -1;
    }
    (void)align;
    if (!nyr_size_fits_i64(size)) {
      return nyr_fail(b, "native layout constructor '%s' size overflows i64",
                      name),
             -1;
    }
    int size_v = nyr_emit_const(b, (int64_t)size);
    int object = size_v < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_malloc_i64", size_v,
                                                    -1, -1, 1, 0);
    if (object < 0)
      return -1;
    for (size_t fi = 0; fi < fields->len; ++fi) {
      const layout_field_t *field = &fields->data[fi];
      if (!field->name || field->is_array) {
        return nyr_fail(b,
                        "native layout constructor '%s' has unsupported "
                        "field %zu",
                        name, fi),
               -1;
      }
      const call_arg_t *source = &e->as.call.args.data[fi];
      if (source->name && strcmp(source->name, field->name) != 0) {
        return nyr_fail(b,
                        "native layout constructor '%s' requires positional "
                        "fields or matching field '%s'",
                        name, field->name),
               -1;
      }
      int value = nyr_lower_expr(b, source->val);
      size_t field_size = 0, field_align = 0, field_offset = 0;
      if (value < 0 || !nyr_ast_layout_query(b, name, field->name, &field_size,
                                             &field_align, &field_offset)) {
        return nyr_fail(b,
                        "native layout constructor '%s.%s' cannot resolve "
                        "field layout",
                        name, field->name),
               -1;
      }
      if (!nyr_size_fits_i64(field_offset)) {
        return nyr_fail(
                   b, "native layout constructor '%s.%s' offset overflows i64",
                   name, field->name),
               -1;
      }
      int offset = nyr_emit_const(b, (int64_t)field_offset);
      int address = offset < 0 ? -1 : nyr_emit_add_i64(b, object, offset);
      if (address < 0)
        return -1;
      size_t primitive_size = 0, primitive_align = 0;
      if (!nyr_primitive_layout(field->type_name, &primitive_size,
                                &primitive_align)) {
        int bytes = nyr_emit_const(b, (int64_t)field_size);
        if (bytes < 0 ||
            nyr_emit_runtime_call(b, "rt_memcpy_raw", address, value, bytes, 3,
                                  NYIR_SCALAR_I64_RAW) < 0)
          return -1;
      } else if (ny_native_type_name_is_f64(field->type_name)) {
        if (!nyr_emit_store_f64(b, address, value))
          return -1;
      } else if (ny_native_type_name_is_f32(field->type_name)) {
        int f64 = nyr_expr_is_f32(b, source->val)
                      ? nyr_emit_f32_to_f64(b, value)
                      : value;
        if (f64 < 0 || nyr_emit_runtime_call(b, "rt_store32_f64", object,
                                             offset, f64, 3, 0) < 0)
          return -1;
      } else if (field_size <= 4) {
        /* Narrow layout fields use the tagged indexed-memory ABI. */
        int one = nyr_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = value,
                                                        .b = one});
        int tagged = shifted < 0
                         ? -1
                         : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_OR_I64,
                                                             .dst = -1,
                                                             .a = shifted,
                                                             .b = one});
        if (tagged < 0 ||
            nyr_emit_runtime_call(b,
                                  field_size == 1   ? "rt_store8_idx"
                                  : field_size == 2 ? "rt_store16_idx"
                                                    : "rt_store32_idx",
                                  object, offset, tagged, 3, 0) < 0)
          return -1;
      } else if (!nyr_emit_store_i64(b, address, value)) {
        return -1;
      }
    }
    return object;
  }
  ny_builtin_alloc_kind_t builtin_kind = ny_builtin_alloc_kind(leaf);
  bool builtin_c_call = builtin_kind != NY_BUILTIN_ALLOC_NONE;
  char call_module_buf[512];
  const char *call_module = b->module_name;
  if ((!call_module || !*call_module) && b->current_fn_name) {
    const char *module_end = strrchr(b->current_fn_name, '.');
    if (module_end && module_end != b->current_fn_name) {
      size_t module_len = (size_t)(module_end - b->current_fn_name);
      if (module_len < sizeof(call_module_buf)) {
        memcpy(call_module_buf, b->current_fn_name, module_len);
        call_module_buf[module_len] = '\0';
        call_module = call_module_buf;
      }
    }
  }
  const char *callee_module =
      callee_fn
          ? ny_native_fn_module(ny_native_fn_index_for(b), b->prog, callee_fn)
          : NULL;
  bool same_module_callee =
      callee_fn && callee_fn->kind == NY_S_FUNC &&
      !callee_fn->as.fn.is_extern && !callee_fn->as.fn.link_name &&
      call_module && callee_module && strcmp(call_module, callee_module) == 0;
  bool user_defined_call = same_module_callee || nyr_user_defined_fn(b, name);
  const char *runtime_symbol = ny_native_runtime_symbol_for_expr(name, leaf, e);
  /* Keep `__list_len` representation-polymorphic for dynamic values, while
   * preserving the typed-buffer fast path when the source argument has a
   * proven static list ABI. */
  if (b->options && b->options->native_only && leaf &&
      strcmp(leaf, "__list_len") == 0 && e->as.call.args.len == 1 &&
      e->as.call.args.data[0].val) {
    const expr_t *len_arg = e->as.call.args.data[0].val;
    bool typed_list =
        nyr_expr_is_list(b, len_arg) && !nyr_expr_is_any(b, len_arg);
    if (!typed_list && len_arg->kind == NY_E_IDENT && len_arg->as.ident.name) {
      const nyr_local_t *local = nyr_find_local(b, len_arg->as.ident.name);
      typed_list = local && local->is_list && !local->is_any;
    }
    runtime_symbol = typed_list ? "rt_tbuf_len_raw" : "rt_len";
  }
  const stmt_t *runtime_fn = name ? nyr_find_user_function(b, name) : NULL;
  bool stdlib_runtime_wrapper =
      !same_module_callee &&
      ((runtime_fn && ny_is_stdlib_tok(runtime_fn->tok)) ||
       (e->semantic.canonical_callee_stmt &&
        ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok)));
  /* A stdlib wrapper is part of the compiler/runtime ABI, not an ordinary
   * user function merely because expansion left a function declaration in
   * the AST.  Preserve the canonical runtime binding for every such wrapper;
   * otherwise its raw result is sent through the user-call return ABI and is
   * silently reinterpreted as a dynamic value. */
  if (user_defined_call && !stdlib_runtime_wrapper && runtime_symbol &&
      ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "native-abi",
        "function=%s call=%s decision=suppress-runtime-alias "
        "reason=non-stdlib-source fn_module_id=%llu fn_features=0x%x "
        "canonical_module_id=%llu canonical_features=0x%x",
        b->current_fn_name ? b->current_fn_name : "<entry>",
        name ? name : "<indirect>",
        (unsigned long long)(runtime_fn ? runtime_fn->tok.module_id : 0),
        runtime_fn ? runtime_fn->tok.module_features : 0,
        (unsigned long long)(e->semantic.canonical_callee_stmt
                                 ? e->semantic.canonical_callee_stmt->tok
                                       .module_id
                                 : 0),
        e->semantic.canonical_callee_stmt
            ? e->semantic.canonical_callee_stmt->tok.module_features
            : 0);
  if (user_defined_call && !stdlib_runtime_wrapper)
    runtime_symbol = NULL;
  const ny_native_runtime_overload_abi_decl_t *runtime_overload_abi =
      runtime_symbol ? ny_native_runtime_overload_abi_decl(runtime_symbol)
                     : NULL;
  if (runtime_overload_abi && runtime_overload_abi->f64_symbol &&
      e->as.call.args.len >= 1 &&
      nyr_expr_is_f64(b, e->as.call.args.data[0].val))
    runtime_symbol = runtime_overload_abi->f64_symbol;
  if (runtime_overload_abi && runtime_overload_abi->tagged_value_symbol &&
      e->as.call.args.len >= 1 &&
      nyr_expr_is_any(b, e->as.call.args.data[0].val))
    runtime_symbol = runtime_overload_abi->tagged_value_symbol;
  const rt_ffi_callback_abi_decl_t *ffi_callback_abi =
      runtime_symbol ? rt_ffi_callback_abi_decl(runtime_symbol) : NULL;
  if (ffi_callback_abi) {
    if (user_args_len != (size_t)ffi_callback_abi->source_arity + 1 ||
        ffi_callback_abi->callable_repr == RT_FFI_CALLBACK_REPR_UNSPECIFIED ||
        ffi_callback_abi->callable_ownership ==
            RT_FFI_CALLBACK_OWNERSHIP_UNSPECIFIED ||
        ffi_callback_abi->result_repr == RT_FFI_CALLBACK_REPR_UNSPECIFIED ||
        ffi_callback_abi->result_ownership ==
            RT_FFI_CALLBACK_OWNERSHIP_UNSPECIFIED)
      return nyr_fail(b,
                      "native NYIR lower: invalid FFI callback ABI "
                      "declaration for '%s'",
                      runtime_symbol),
             -1;
    for (size_t i = 0; i < ffi_callback_abi->source_arity; ++i) {
      if (ffi_callback_abi->arg_repr[i] == RT_FFI_CALLBACK_REPR_UNSPECIFIED ||
          ffi_callback_abi->arg_ownership[i] ==
              RT_FFI_CALLBACK_OWNERSHIP_UNSPECIFIED)
        return nyr_fail(b,
                        "native NYIR lower: incomplete FFI callback "
                        "argument %zu ABI for '%s'",
                        i + 1, runtime_symbol),
               -1;
    }
  }
  bool runtime_c_call = runtime_symbol != NULL;
  bool declared_fn_call =
      callee_ext_decl != NULL || (callee_fn && callee_fn->as.fn.is_extern);
  bool ffi_c_call =
      !ext && !callee_fn && !callee_ext_decl && ny_native_ffi_symbol_name(name);
  bool is_c_call = ext != NULL || declared_fn_call || builtin_c_call ||
                   runtime_c_call || ffi_c_call;
  bool has_aggregate_return = ext && ext->ret_aggregate_size > 0;
  bool has_sret = has_aggregate_return &&
                  ext->ret_aggregate_classes[0] == NY_SYSV_AGG_MEMORY;

  int args[NYIR_CALL_MAX_ARGS];
  size_t lowered_argc = 0;
  /* These dictionary bridges have a declared flat-word ABI with explicit
   * mixed source representations. Keep them out of the legacy expanded
   * value/length/tag call path, and reject stale descriptors that no longer
   * describe the borrowed, receiver-alias result contract. */
  const ny_native_runtime_value_abi_decl_t *value_abi =
      runtime_c_call ? ny_native_runtime_value_abi_decl(runtime_symbol) : NULL;
  const ny_native_runtime_arg_abi_decl_t *runtime_arg_abi =
      runtime_c_call ? ny_native_runtime_arg_abi_decl(runtime_symbol) : NULL;
  bool raw_native_dict_set = value_abi != NULL;
  if (value_abi) {
    if (user_args_len != value_abi->arity || value_abi->arity != 3 ||
        value_abi->result_repr != NY_NATIVE_ABI_VALUE_ALIAS_ARG0 ||
        value_abi->result_ownership != NY_NATIVE_ABI_BORROWED_ALIAS) {
      return nyr_fail(b,
                      "native NYIR lower: runtime ABI descriptor "
                      "for '%s' does not "
                      "match its flat dictionary bridge contract",
                      runtime_symbol ? runtime_symbol : "<unknown>"),
             -1;
    }
    for (size_t ri = 0; ri < NY_NATIVE_ABI_DICT_RECEIVER_COUNT; ++ri) {
      for (size_t ai = 0; ai < value_abi->arity; ++ai) {
        if (!value_abi->arg_repr[ri][ai] ||
            value_abi->arg_ownership[ai] != NY_NATIVE_ABI_BORROWED) {
          return nyr_fail(b,
                          "native NYIR lower: runtime ABI descriptor "
                          "for '%s' has an "
                          "incomplete argument contract for receiver "
                          "%zu at index %zu",
                          runtime_symbol, ri, ai),
                 -1;
        }
      }
    }
  }
  bool variadic_call = callee_fn && callee_fn->as.fn.is_variadic &&
                       callee_fn->as.fn.params.len > 0;
  size_t variadic_index = variadic_call ? callee_fn->as.fn.params.len - 1 : 0;
  size_t lowered_user_args = variadic_call ? variadic_index + 1 : user_args_len;
  if (callee_fn && !callee_fn->as.fn.is_variadic) {
    size_t min_required = 0;
    for (size_t p = 0; p < callee_fn->as.fn.params.len; ++p) {
      if (!callee_fn->as.fn.params.data[p].def)
        min_required = p + 1;
    }
    if (user_args_len < min_required) {
      return nyr_fail(b,
                      "native NYIR lower: call to '%s' expects at "
                      "least %zu argument(s), got %zu",
                      name ? name : "<anonymous>", min_required, user_args_len),
             -1;
    }
    if (user_args_len > callee_fn->as.fn.params.len) {
      return nyr_fail(b,
                      "native NYIR lower: call to '%s' expects %zu "
                      "argument(s), got %zu (caller=%s at %s:%d)",
                      name ? name : "<anonymous>", callee_fn->as.fn.params.len,
                      user_args_len,
                      b->current_fn_name ? b->current_fn_name : "<entry>",
                      e->tok.filename ? e->tok.filename : "<source>",
                      e->tok.line),
             -1;
    }
  } else if (callee_ext_decl && !callee_ext_decl->as.ext.is_variadic) {
    if (user_args_len != callee_ext_decl->as.ext.params.len) {
      return nyr_fail(b,
                      "native NYIR lower: extern call to '%s' "
                      "expects %zu argument(s), got %zu",
                      name ? name : "<extern>",
                      callee_ext_decl->as.ext.params.len, user_args_len),
             -1;
    }
  }
  /* Per-argument formal scalar ABI facts recorded below. */
  uint64_t c_arg_scalar_classes[NYIR_CALL_MAX_ARGS / 16] = {0};
  uint64_t c_arg_ownership[NYIR_CALL_MAX_ARGS / 32] = {0};
  uint64_t c_arg_contracts[NYIR_CALL_MAX_ARGS / 32] = {0};
  for (size_t i = 0; i < lowered_user_args; ++i) {
    const expr_t *arg_expr =
        variadic_call && i == variadic_index ? NULL : user_args[i].val;
    bool lowering_default = normalized_default_args[i];
    bool default_raw_constant = false;
    int64_t default_value = 0;
    if (lowering_default && arg_expr && callee_fn) {
      const char *default_module =
          ny_native_fn_module(ny_native_fn_index_for(b), b->prog, callee_fn);
      default_raw_constant = nyr_fold_top_level_int_in_module(
          b, NULL, default_module, arg_expr, &default_value, 0);
    }
    bool arg_expr_f64 = nyr_expr_is_f64(b, arg_expr) ||
                        (arg_expr && arg_expr->kind == NY_E_LITERAL &&
                         arg_expr->as.literal.kind == NY_LIT_FLOAT);
    bool arg_expr_f32 = nyr_expr_is_f32(b, arg_expr);
    const nyr_local_t *arg_local =
        arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
            ? nyr_find_local_nearest(b, arg_expr->as.ident.name)
            : NULL;
    /* Dynamic sequence accessors already produce the canonical tagged/object
     * word.  Do not let a numeric fallback on `.get(i, fallback)` select the
     * machine-float boxing path. */
    if (nyr_expr_is_canonical_dynamic_value(b, arg_expr)) {
      arg_expr_f64 = false;
      arg_expr_f32 = false;
    }
    /* Identifier nodes can retain a conservative `any` semantic fact even
     * after local lowering has fixed their physical slot to f64.  The local
     * ABI is authoritative at this call boundary; otherwise a raw IEEE word
     * is forwarded to an `any` parameter without boxing. */
    if (arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      if (arg_local && arg_local->is_any) {
        /* The physical local representation is authoritative.  A dynamic
         * lookup may carry a numeric fallback in its syntax, but the value
         * in an any slot is already a canonical Ny value.  Reclassifying it
         * as raw f64 here would box its pointer bits a second time. */
        arg_expr_f64 = false;
        arg_expr_f32 = false;
      } else if (arg_local && arg_local->is_f64) {
        arg_expr_f64 = true;
      } else if (arg_local && arg_local->is_f32) {
        arg_expr_f32 = true;
      }
      /* A concrete local declaration fixes the carrier for the lifetime of
       * its slot. Contextual HM facts can still label an int, string, or
       * pointer use as numeric (notably loop-carried locals in core.str);
       * do not synthesize float boundary conversions from that stale guess.
       */
      if (arg_local && arg_local->type_name &&
          !ny_native_type_name_is_any(arg_local->type_name)) {
        ny_sem_rep_t local_rep = nyr_rep_for_type(b, arg_local->type_name);
        /* The recorded type name can disagree with the resolved float rep
         * (e.g. a `1f32` literal inferring the generic f64 float-word name).
         * The semantic rep drives the store/load ABI stamps, so it is the
         * authoritative carrier at this boundary; trusting the name alone
         * forwarded an f32 register through an f64 formal (rt_f64_bits). */
        if (arg_local->semantic_rep == NY_SEM_REP_F32 ||
            arg_local->semantic_rep == NY_SEM_REP_F64)
          local_rep = arg_local->semantic_rep;
        if (local_rep != NY_SEM_REP_UNRESOLVED &&
            local_rep != NY_SEM_REP_TAGGED_DYNAMIC) {
          arg_expr_f64 = local_rep == NY_SEM_REP_F64;
          arg_expr_f32 = local_rep == NY_SEM_REP_F32;
        }
      }
    }
    const ny_param_list *callee_params = callee_fn ? &callee_fn->as.fn.params
                                         : callee_ext_decl
                                             ? &callee_ext_decl->as.ext.params
                                             : NULL;
    const char *param_type = callee_params && i < callee_params->len
                                 ? callee_params->data[i].type
                                 : NULL;
    const ny_expr_semantic_t *param_sem =
        callee_params && i < callee_params->len
            ? &callee_params->data[i].semantic
            : NULL;
    nyr_param_abi_t param_abi = {0};
    bool has_param_abi =
        callee_fn && nyr_param_abi(b, callee_fn, i, &param_abi);
    bool try_dynamic_parameter = has_param_abi && param_abi.try_dynamic;
    bool param_inferred_list = has_param_abi && param_abi.inferred_list;
    bool dynamic_sequence = has_param_abi && param_abi.dynamic_sequence;
    ny_sem_rep_t param_rep = has_param_abi ? param_abi.rep
                                           : (param_sem && param_sem->resolved
                                                  ? param_sem->rep
                                                  : NY_SEM_REP_UNRESOLVED);
    const ny_extern_entry_t *c_params =
        b->externs ? ny_extern_table_lookup(b->externs, name) : NULL;
    bool expected_f64 =
        param_rep == NY_SEM_REP_F64 || param_rep == NY_SEM_REP_F32 ||
        ny_native_type_name_is_f64(param_type) ||
        ny_native_type_name_is_f32(param_type) ||
        (c_params && i < NY_C_MAX_PARAMS && c_params->param_f64[i]);
    ny_sem_rep_t formal_rep =
        param_rep != NY_SEM_REP_UNRESOLVED
            ? param_rep
            : (param_type ? nyr_rep_for_type(b, param_type)
                          : NY_SEM_REP_UNRESOLVED);
    /* Explicit source declarations own the external call ABI even when HM
     * specializes the body from its default expression. In particular an
     * `any` formal with a numeric default must remain tagged at call sites.
     * A resolved fact coherent with the declared spelling is the formal's
     * ABI contract and must not be clobbered by a spelling alone. */
    bool formal_sem_authoritative =
        param_sem && param_sem->resolved &&
        ny_sem_rep_coherent_with_type(param_type, param_sem->rep);
    if (ny_native_type_name_is_any(param_type) && !formal_sem_authoritative) {
      param_rep = NY_SEM_REP_TAGGED_DYNAMIC;
      formal_rep = NY_SEM_REP_TAGGED_DYNAMIC;
    }
    /* A declared list parameter has the typed-buffer carrier when its ABI is
     * not deliberately dynamic. HM may retain a tagged fallback fact for a
     * generic list body; letting that fallback select the call boundary
     * leaves the tagged producer in place even though the expanded call ABI
     * (including its length slot) is typed-buffer. */
    if (!try_dynamic_parameter && !dynamic_sequence &&
        ny_native_type_name_is_list(param_type) && !formal_sem_authoritative) {
      param_rep = NY_SEM_REP_TYPED_BUFFER;
      formal_rep = NY_SEM_REP_TYPED_BUFFER;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu decision=typed-buffer-formal "
            "source=list-type reason=non-dynamic-list-parameter",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            name ? name : "<indirect>", i + 1);
    }
    if (c_params && i < NY_C_MAX_PARAMS && c_params->param_f32[i])
      formal_rep = NY_SEM_REP_F32;
    else if (c_params && i < NY_C_MAX_PARAMS && c_params->param_f64[i])
      formal_rep = NY_SEM_REP_F64;
    const char *list_elem_type =
        param_type && ny_native_type_name_is_list(param_type)
            ? strchr(param_type, '<')
            : NULL;
    bool expected_list_f64 = false;
    if (list_elem_type) {
      const char *elem = list_elem_type + 1;
      while (*elem == ' ' || *elem == '\t')
        ++elem;
      expected_list_f64 = strncmp(elem, "f64", 3) == 0 &&
                          (elem[3] == '>' || isspace((unsigned char)elem[3]));
    }
    int arg = -1;
    bool untyped_dynamic_scalar =
        !param_type && !param_inferred_list && !dynamic_sequence &&
        param_rep != NY_SEM_REP_F64 && param_rep != NY_SEM_REP_F32 &&
        param_rep != NY_SEM_REP_STRING && param_rep != NY_SEM_REP_TYPED_BUFFER;
    bool value_for_any_call =
        param_rep == NY_SEM_REP_TAGGED_DYNAMIC ||
        ny_native_type_name_is_any(param_type) ||
        (!param_type && param_rep == NY_SEM_REP_UNRESOLVED) ||
        untyped_dynamic_scalar;
    if (variadic_call && i == variadic_index) {
      size_t count =
          user_args_len > variadic_index ? user_args_len - variadic_index : 0;
      if (!nyr_size_fits_i64(count)) {
        return nyr_fail(b, "native NYIR lower: variadic argument "
                           "count overflows i64"),
               -1;
      }
      int n = nyr_emit_const(b, (int64_t)count);
      int width = nyr_emit_const(b, 24);
      arg = n < 0 || width < 0 ? -1
                               : nyr_emit_runtime_call(b, "rt_tbuf_new_raw", n,
                                                       width, -1, 2, 0);
      for (size_t j = 0; arg >= 0 && j < count; ++j) {
        const expr_t *value_expr = user_args[variadic_index + j].val;
        int value = nyr_lower_expr(b, value_expr);
        int index = nyr_emit_const(b, (int64_t)j);
        if (value < 0 || index < 0)
          return -1;
        if (nyr_expr_is_f64(b, value_expr) || nyr_expr_is_f32(b, value_expr)) {
          int bits =
              nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
          if (bits < 0 || nyr_emit_runtime_call(b, "rt_tbuf_set_f64_bits", arg,
                                                index, bits, 3, 0) < 0)
            return -1;
        } else {
          bool raw_int =
              value_expr && ((value_expr->semantic.resolved &&
                              value_expr->semantic.rep == NY_SEM_REP_RAW_INT) ||
                             (value_expr->kind == NY_E_LITERAL &&
                              value_expr->as.literal.kind == NY_LIT_INT));
          if (raw_int) {
            int one = nyr_emit_const(b, 1);
            int shifted =
                one < 0 ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                            .dst = -1,
                                                            .a = value,
                                                            .b = one});
            value =
                shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
            if (value < 0)
              return -1;
          }
          if (nyr_emit_runtime_call(b, "rt_tbuf_set_tagged", arg, index, value,
                                    3, 0) < 0)
            return -1;
        }
      }
    } else {
      /* A surrounding floating expression must not determine the ABI of a
       * nested call argument.  Lower unresolved arguments in the formal's
       * representation; resolved semantic facts always win.  This prevents
       * e.g. `x * f(x, n - 1)` from turning an inferred integer `n - 1` into
       * an f64 subtraction merely because the outer product is f64. */
      unsigned saved_f64_depth = b->f64_context_depth;
      bool saved_list_elem_f64 = b->current_list_elem_f64;
      bool argument_needs_f64_context =
          expected_f64 &&
          (!arg_expr || !arg_expr->semantic.resolved ||
           (arg_expr->semantic.rep != NY_SEM_REP_RAW_INT &&
            arg_expr->semantic.rep != NY_SEM_REP_TAGGED_DYNAMIC));
      b->f64_context_depth = argument_needs_f64_context ? 1u : 0u;
      b->current_list_elem_f64 = expected_list_f64;
      bool saved_value_for_any_call = b->value_for_any_call;
      b->value_for_any_call = value_for_any_call;
      /* Default expressions belong to the callee's lexical module.  They are
       * stored on the callee signature but lowered at the call site; using
       * the caller module here makes imported defaults resolve through the
       * wrong namespace (and can silently drop a module constant's bits).
       * Explicit arguments remain in the caller context. */
      const char *saved_module_name = b->module_name;
      if (lowering_default)
        b->module_name =
            ny_native_fn_module(ny_native_fn_index_for(b), b->prog, callee_fn);
      bool default_raw_formal = lowering_default && default_raw_constant &&
                                (param_rep == NY_SEM_REP_RAW_INT ||
                                 ny_native_type_name_is_int(param_type));
      if (default_raw_formal) {
        arg = nyr_emit_const(b, default_value);
        if (arg >= 0 && b->nyir.len > 0) {
          nyr_inst_t *constant = &b->nyir.data[b->nyir.len - 1];
          if (constant->op == NYIR_CONST_I64 && constant->dst == arg)
            constant->scalar_class = NYIR_SCALAR_I64_RAW;
        }
      } else {
        arg = arg_expr ? nyr_lower_expr(b, arg_expr) : nyr_emit_const(b, 0);
      }
      b->module_name = saved_module_name;
      b->value_for_any_call = saved_value_for_any_call;
      b->f64_context_depth = saved_f64_depth;
      b->current_list_elem_f64 = saved_list_elem_f64;
    }
    if (arg < 0)
      return -1;
    /* The formal and the concrete source local agree on its storage ABI even
     * when contextual HM inference has attached a stale numeric class to the
     * identifier. Preserve that fact on the actual LOAD_LOCAL producer so
     * optimization and the verifier see the same pointer/string/integer
     * carrier used by the source binding. */
    if (arg_local && arg_expr && arg_expr->kind == NY_E_IDENT) {
      /* The local's storage fact is stronger than a stale/inferred surface
       * type. In particular, an explicitly floating local can be loaded into
       * an F64 NYIR slot even when HM retained `any` on its identifier. Do
       * not then feed that native register to rt_any_to_f64 at a typed
       * runtime boundary. */
      nyr_scalar_class_t source_class = arg_local->is_f64 ? NYIR_SCALAR_F64
                                         : arg_local->is_f32
                                             ? NYIR_SCALAR_F32
                                             : NYIR_SCALAR_UNKNOWN;
      if (source_class == NYIR_SCALAR_UNKNOWN && arg_local->type_name &&
          !ny_native_type_name_is_any(arg_local->type_name)) {
        ny_sem_rep_t source_rep = nyr_rep_for_type(b, arg_local->type_name);
        source_class = ny_native_scalar_class_from_sem_rep(source_rep);
      }
      if (source_class != NYIR_SCALAR_UNKNOWN) {
        for (size_t d = b->nyir.len; d > 0; --d) {
          nyr_inst_t *producer = &b->nyir.data[d - 1];
          if (producer->dst != arg)
            continue;
          if (producer->op == NYIR_LOAD_LOCAL &&
              producer->imm == arg_local->slot)
            producer->scalar_class = source_class;
          break;
        }
      }
    }
    nyr_scalar_class_t producer_class = nyr_value_scalar_class(b, arg);
    if (ny_native_runtime_arg_abi_cstr_early(runtime_arg_abi, i)) {
      arg = nyr_emit_runtime_call(b, "rt_cstr_to_str", arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
    }
    if (param_type && arg_expr) {
      if (!nyr_check_refine_obligation(b, e->tok, param_type,
                                       (expr_t *)arg_expr))
        return -1;
    }
    /* A declared bool parameter uses the raw predicate ABI (0/1). Boolean
     * expressions can carry either raw predicate bits or Ny's dynamic
     * immediates, and their conservative NYIR class may say TAGGED even
     * when the comparison actually produced raw 0/1. Normalize from the
     * boolean value before integer ABI reconciliation can decode that 1 as
     * tagged integer zero. */
    if (param_type && strcmp(param_type, "bool") == 0 && arg_expr &&
        nyr_expr_is_bool(b, arg_expr)) {
      int zero = nyr_emit_const(b, 0);
      int false_value = nyr_emit_const(b, NY_IMM_FALSE);
      int nonzero = zero < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_NE, arg, zero);
      int not_false = false_value < 0
                          ? -1
                          : nyr_emit_cmp_i64(b, NYIR_CMP_NE, arg, false_value);
      arg = nonzero < 0 || not_false < 0
                ? -1
                : nyr_ir_emit(&b->nyir,
                            (nyr_inst_t){.op = NYIR_AND_I64,
                                          .dst = -1,
                                          .a = nonzero,
                                          .b = not_false,
                                          .scalar_class = NYIR_SCALAR_I64_RAW});
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_RAW;
    }
    bool box_dynamic_argument =
        dynamic_sequence || param_rep == NY_SEM_REP_TAGGED_DYNAMIC ||
        ny_native_type_name_is_any(param_type) ||
        (!param_type && param_rep == NY_SEM_REP_UNRESOLVED) ||
        untyped_dynamic_scalar;
    /* The BigFloat f64 overload takes IEEE bits in an integer word.  The
     * source declaration is intentionally dynamic, so its generic argument
     * path would box the f64 as a tagged heap float and pass an
     * ABI-mismatched value to rt_bigfloat_from_f64_bits. */
    if (runtime_symbol && strcmp(runtime_symbol, "rt_bigfloat_from_f64") == 0 &&
        i == 0 && arg_expr_f64)
      box_dynamic_argument = false;
    /* C declarations use the platform ABI: integer and pointer arguments are
     * raw machine words.  They have no Ny tagged-value parameter boundary,
     * even when this AST call has no recovered parameter type and the
     * language-level fallback above would otherwise box its arguments.
     * Runtime helpers are handled separately by their explicit ABI metadata.
     */
    if ((ext || callee_ext_decl || builtin_c_call || ffi_c_call) &&
        !runtime_c_call)
      box_dynamic_argument = false;
    if (ny_native_runtime_symbol_arg_is_tagged(runtime_symbol, i))
      box_dynamic_argument = true;
    else if (ny_native_runtime_symbol_skips_dynamic_boxing(runtime_symbol))
      box_dynamic_argument = false;
    /* An unresolved untyped parameter used by a scalar operator is lowered
     * as a raw NYIR integer in the callee.  Match that body-derived ABI fact
     * at the call boundary instead of manufacturing tagged-any metadata. */
    if (box_dynamic_argument && !try_dynamic_parameter && callee_fn &&
        !direct_zero_capture_lambda && !param_type &&
        param_rep != NY_SEM_REP_TAGGED_DYNAMIC &&
        nyr_untyped_param_is_raw(callee_fn, callee_params->data[i].name))
      box_dynamic_argument = false;
    /* Float literals are native f64 values.  If the callee's parameter type
     * is dynamic or was lost while flattening an imported module, preserve
     * the float object ABI instead of passing raw IEEE bits as an integer. */
    if (arg_expr_f64 && value_for_any_call && formal_rep != NY_SEM_REP_F64 &&
        formal_rep != NY_SEM_REP_F32 &&
        !ny_native_runtime_symbol_skips_dynamic_boxing(runtime_symbol) &&
        param_rep != NY_SEM_REP_F64 && param_rep != NY_SEM_REP_F32)
      box_dynamic_argument = true;
    /* Index reads default to the raw typed-slot ABI for arithmetic-heavy
     * lists.  A direct call whose formal is `any` is an explicit dynamic
     * boundary, however: re-lower through the descriptor-aware index path so
     * nested dictionaries/lists and scalar values retain their canonical
     * representation.  `.get(...)` has the equivalent handling below. */
    if (box_dynamic_argument && arg_expr && arg_expr->kind == NY_E_INDEX) {
      bool saved_index_for_any_call = b->index_for_any_call;
      b->index_for_any_call = true;
      arg = nyr_lower_expr(b, arg_expr);
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
        nyr_expr_is_list(b, raw_get_target) &&
        !nyr_expr_is_dyn_list(b, raw_get_target)) {
      int raw_base = nyr_lower_expr(b, raw_get_target);
      int raw_index = nyr_lower_expr(b, raw_get_index);
      if (raw_base < 0 || raw_index < 0)
        return -1;
      /* The typed-list `.get` index is a raw native loop/index value here.
       * Use the raw-index entry point; the ABI wrapper decodes tagged
       * language integers and would turn raw odd indices (e.g. 1) into the
       * previous slot. */
      /* rt_tbuf_index_any (checked) returns the canonical dynamic word —
       * the raw-index entry point returned pooled raw words that downstream
         consumers misclassified (61 -> 30, 24 -> nil) — but it decodes
         tagged indices, so an odd raw index (1) must be tagged first or it
         decodes to the previous slot.  rt_tag round-trips through the
         decode exactly for every integer, including negatives and zero. */
      int tagged_index =
          nyr_emit_runtime_call(b, "rt_tag", raw_index, -1, -1, 1, 0);
      if (tagged_index < 0)
        return -1;
      arg = nyr_emit_runtime_call(b, "rt_tbuf_index_any", raw_base,
                                  tagged_index, -1, 2, 0);
      if (arg < 0)
        return -1;
    }
    /* Generated lambda entry points decode concrete scalar parameters from
     * the canonical tagged callback ABI in stmt.h. A zero-capture
     * lambda can still be resolved as a direct call when stored in a local
     * (`def f = fn(int x) int { ... }; f(41)`).  Passing the ordinary raw
     * scalar ABI there makes odd inputs look pre-tagged and halves them in
     * rt_any_to_i64.  Preserve the tagged entry contract for explicit integer
     * lambda formals; untyped/raw callback cases keep their existing adapter
     * selection below. */
    bool direct_generated_lambda_int =
        callee_fn && callee_fn->as.fn.name &&
        strncmp(callee_fn->as.fn.name, "__ny_lambda_", 12) == 0 && param_type &&
        ny_native_type_name_is_int(param_type);
    if (direct_generated_lambda_int)
      box_dynamic_argument = true;
    /* Generated lambdas use the callback entry ABI even when they have no
     * captures.  In particular, an inferred untyped parameter is consumed by
     * rt_any_* operations and therefore needs a canonical tagged argument;
     * the address being directly callable does not change that contract. */
    else if (direct_zero_capture_lambda && !param_type)
      box_dynamic_argument = true;
    /* Booleans are already immediate Ny values at an `any` boundary.  The
     * ordinary raw scalar lowering uses 0/1, which `type(any)` cannot
     * distinguish from an unknown native integer payload. */
    if (box_dynamic_argument && arg_expr && arg_expr->kind == NY_E_LITERAL &&
        arg_expr->as.literal.kind == NY_LIT_BOOL)
      arg = nyr_emit_const(b, arg_expr->as.literal.as.b ? NY_IMM_TRUE
                                                        : NY_IMM_FALSE);
    /* Boolean expressions use the raw predicate ABI while being evaluated,
     * but an `any` formal owns the canonical boxed-boolean ABI.  A generic
     * integer box would encode raw false (0) as tagged integer zero (1), so
     * normalize every boolean producer at this boundary, not just literals.
     */
    if (box_dynamic_argument && arg_expr && nyr_expr_is_bool(b, arg_expr)) {
      arg = nyr_box_bool(b, arg);
      box_dynamic_argument = false;
      if (arg < 0)
        return -1;
    }
    /* BigInt runtime helpers take the opaque heap handle directly.  Their
     * legacy declarations are intentionally untyped, but applying the any
     * integer tag to a bigint pointer makes limb operations misclassify it as
     * a small integer (notably __bigint_clz on 2^63). */
    const ny_native_runtime_abi_decl_t *runtime_abi_decl =
        ny_native_runtime_abi_decl(runtime_symbol);
    if (arg_expr && nyr_expr_is_bigint(b, arg_expr) &&
        ny_native_runtime_abi_accepts_bigint_values(runtime_abi_decl))
      box_dynamic_argument = false;
    /* Zero-capture lambdas use the same raw scalar callback ABI as named
     * functions.  The `rt_call_any*` adapters decode their dynamic
     * argument before invoking such a callback; marking it dynamic would
     * deliver the tagged argument to raw arithmetic a second time. */
    bool dynamic_fnptr_parameter =
        param_type && (strcmp(param_type, "fnptr") == 0 ||
                       strstr(param_type, "fnptr") != NULL);
    bool callback_value_parameter =
        dynamic_fnptr_parameter ||
        (param_type && ny_native_type_name_is_any(param_type));
    bool inferred_callback_value_parameter =
        !param_type && callee_fn && callee_fn->kind == NY_S_FUNC &&
        i < callee_fn->as.fn.params.len &&
        !callee_fn->as.fn.params.data[i].type;
    const expr_t *lambda_value_expr = NULL;
    if (arg_expr &&
        (arg_expr->kind == NY_E_LAMBDA || arg_expr->kind == NY_E_FN)) {
      lambda_value_expr = arg_expr;
    } else if (arg_expr && arg_expr->kind == NY_E_IDENT &&
               arg_expr->as.ident.name) {
      nyr_local_t *lambda_binding_local =
          nyr_find_local(b, arg_expr->as.ident.name);
      if (lambda_binding_local && lambda_binding_local->callable_expr)
        lambda_value_expr = lambda_binding_local->callable_expr;
    }
    /* A named function passed through an `any` callback slot must carry its
     * actual parameter ABI with the function value.  This includes the
     * all-scalar case: the callback may be stored in a registry and invoked
     * later, after the static call site has disappeared.  Recording the raw
     * contract here prevents a later dynamic-call adapter from mistaking a
     * tagged integer for the scalar that the function body expects. */
    const stmt_t *named_callback_fn = NULL;
    if ((callback_value_parameter || inferred_callback_value_parameter) &&
        !lambda_value_expr && arg_expr && arg_expr->kind == NY_E_IDENT &&
        arg_expr->as.ident.name) {
      /* Resolve the identifier in the current lexical environment before
       * consulting the global function table.  A local such as `value` must
       * shadow an unrelated function with the same spelling; otherwise the
       * callback ABI marker is applied to an ordinary value and corrupts
       * pointers/floats at the dynamic boundary. */
      const nyr_local_t *named_function_local =
          nyr_find_local(b, arg_expr->as.ident.name);
      bool local_is_callable =
          named_function_local &&
          (named_function_local->callable_expr ||
           named_function_local->callable_name ||
           named_function_local->semantic_rep == NY_SEM_REP_CLOSURE);
      const stmt_t *ref =
          !named_function_local || local_is_callable
              ? nyr_find_user_function(b, arg_expr->as.ident.name)
              : NULL;
      if (getenv("NY_DEBUG_FNPTR"))
        fprintf(
            stderr, "[cbmark] arg=%s local=%p ref=%p param_type=%s cbvp=%d\n",
            arg_expr->as.ident.name, (void *)named_function_local, (void *)ref,
            param_type ? param_type : "(null)", (int)callback_value_parameter);
      if (ref && ref->kind == NY_S_FUNC && !ref->as.fn.is_extern &&
          !ny_is_stdlib_tok(ref->tok)) {
        named_callback_fn = ref;
      }
    }
    if ((callback_value_parameter || inferred_callback_value_parameter) &&
        named_callback_fn) {
      /* Same body-shape analysis as a lambda value: a boolean result keeps
       * the raw predicate ABI expected by filter-style loops. */
      bool returns_bool = false;
      /* Same scalar contract as nyr_expr_has_raw_int_abi: a
       * resolved RAW_INT fact decides when it is coherent with the
       * annotation; 128-bit spellings keep their non-raw ABI. */
      bool raw_scalar_result =
          (named_callback_fn->as.fn.return_semantic.resolved &&
           named_callback_fn->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT &&
           ny_sem_rep_coherent_with_type(
               named_callback_fn->as.fn.return_type,
               named_callback_fn->as.fn.return_semantic.rep) &&
           !ny_native_type_name_is_128bit_int(
               named_callback_fn->as.fn.return_type)) ||
          (named_callback_fn->as.fn.return_type &&
           ny_native_type_name_is_int(named_callback_fn->as.fn.return_type));
      expr_t *fn_result = (expr_t *)nyr_callable_tail(named_callback_fn);
      const char *inferred_return =
          fn_result ? nyr_expr_type_name(b, fn_result) : NULL;
      if (!named_callback_fn->as.fn.return_type && inferred_return &&
          ny_native_type_name_is_int(inferred_return) &&
          !nyr_expr_is_bigint(b, fn_result) &&
          !nyr_expr_is_bigfloat(b, fn_result))
        raw_scalar_result = true;
      if (!named_callback_fn->as.fn.return_type ||
          strcmp(named_callback_fn->as.fn.return_type, "bool") == 0)
        returns_bool = fn_result && nyr_expr_is_bool(b, fn_result);
      if (returns_bool)
        raw_scalar_result = false;
      bool callback_params_raw = true;
      uint64_t callback_dynamic_arg_mask = 0;
      bool callback_params_fnptr_only = named_callback_fn->as.fn.params.len > 0;
      for (size_t pi = 0; pi < named_callback_fn->as.fn.params.len; ++pi) {
        const param_t *cb_param = &named_callback_fn->as.fn.params.data[pi];
        callback_params_fnptr_only &=
            cb_param->type && strstr(cb_param->type, "fnptr") != NULL;
        bool try_dynamic = nyr_param_is_try_dynamic(b, named_callback_fn, pi);
        bool inferred_list =
            !try_dynamic && nyr_param_is_inferred_list(named_callback_fn, pi);
        bool callback_dynamic_sequence =
            !try_dynamic &&
            nyr_param_uses_dynamic_sequence_abi(named_callback_fn, pi);
        ny_sem_rep_t rep =
            try_dynamic ? NY_SEM_REP_TAGGED_DYNAMIC
                        : (cb_param->semantic.resolved ? cb_param->semantic.rep
                                                       : NY_SEM_REP_UNRESOLVED);
        /* Match the callee's real parameter ABI, not just its HM scalar
         * classification.  Untyped sequence/list-surface parameters use a
         * raw value/handle in slot zero (plus length metadata), so callback
         * dispatch must unbox integer elements before entering them.  Only
         * the true any/seq dynamic ABI consumes the canonical tagged word. */
        bool dynamic_value_abi =
            try_dynamic || callback_dynamic_sequence ||
            ny_native_type_name_is_any(cb_param->type) ||
            (!cb_param->type && (!inferred_list || callback_dynamic_sequence) &&
             (rep == NY_SEM_REP_TAGGED_DYNAMIC ||
              rep == NY_SEM_REP_UNRESOLVED) &&
             !nyr_untyped_param_is_raw(named_callback_fn, cb_param->name));
        if (dynamic_value_abi) {
          callback_params_raw = false;
          if (pi < 64)
            callback_dynamic_arg_mask |= UINT64_C(1) << pi;
        }
      }
      const char *mark_symbol =
          returns_bool
              ? (callback_params_fnptr_only && callback_word_abi
                     ? "rt_mark_dynamic_bool_callable_tagged_word_args"
                 : (callback_params_raw && callback_word_abi)
                     ? "rt_mark_dynamic_bool_callable_word_args"
                 : callback_params_raw
                     ? "rt_mark_dynamic_bool_callable"
                     : "rt_mark_dynamic_bool_callable_tagged_args")
              : (raw_scalar_result
                     ? (callback_params_fnptr_only && callback_word_abi
                            ? "rt_mark_dynamic_callable_tagged_word_args_raw_"
                              "result"
                        : (callback_params_raw && callback_word_abi)
                            ? "rt_mark_dynamic_callable_word_args_raw_result"
                        : callback_params_raw
                            ? "rt_mark_dynamic_callable_raw_result"
                            : "rt_mark_dynamic_callable_tagged_args_raw_"
                              "result")
                     : (callback_params_fnptr_only && callback_word_abi
                            ? "rt_mark_dynamic_callable_tagged_word_args"
                        : (callback_params_raw && callback_word_abi)
                            ? "rt_mark_dynamic_callable_word_args"
                        : callback_params_raw
                            ? "rt_mark_dynamic_callable"
                            : "rt_mark_dynamic_callable_tagged_args"));
      arg = nyr_emit_runtime_call(b, mark_symbol, arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
      size_t callback_arity = named_callback_fn->as.fn.params.len;
      uint64_t all_dynamic_mask =
          callback_arity >= 64
              ? UINT64_MAX
              : (callback_arity ? (UINT64_C(1) << callback_arity) - 1 : 0);
      if (callback_dynamic_arg_mask != 0 &&
          callback_dynamic_arg_mask != all_dynamic_mask) {
        int mask_value = nyr_emit_const(b, (int64_t)callback_dynamic_arg_mask);
        arg =
            mask_value < 0
                ? -1
                : nyr_emit_runtime_call(b, "rt_mark_dynamic_callable_arg_mask",
                                        arg, mask_value, -1, 2, 0);
        if (arg < 0)
          return -1;
      }
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
      stmt_t *cb_body =
          lambda_value_expr ? lambda_value_expr->as.lambda.body : NULL;
      const ny_param_list *cb_params =
          lambda_value_expr ? &lambda_value_expr->as.lambda.params : NULL;
      bool callback_params_fnptr_only = cb_params && cb_params->len > 0;
      for (size_t pi = 0; cb_params && pi < cb_params->len; ++pi) {
        const param_t *cb_param = &cb_params->data[pi];
        callback_params_fnptr_only &=
            cb_param->type && strstr(cb_param->type, "fnptr") != NULL;
        /* Generated lambda bodies read parameters through the closure
         * prologue, which untags typed ints itself (`fn(i32 op)` observed
         * value>>1 when the trampoline also untagged).  Only an explicitly
         * raw-annotated untyped param keeps the raw mark. */
        bool raw_scalar = !cb_param->type && cb_param->semantic.resolved &&
                          cb_param->semantic.rep == NY_SEM_REP_RAW_INT;
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
        stmt_t *last =
            cb_body->as.block.body.data[cb_body->as.block.body.len - 1];
        if (last && last->kind == NY_S_EXPR)
          cb_result = last->as.expr.expr;
      }
      predicate_callback = cb_result && nyr_expr_is_bool(b, cb_result);
      /* The collected lambda stmt carries the resolved return fact; consult
       * it before falling back to the annotation spelling.  A resolved
       * RAW_INT fact opts into the raw-result adapter when it is coherent
       * with the annotation, and 128-bit spellings stay non-raw. */
      const ny_native_lambda_entry_t *materialized_lambda =
          ny_native_lambda_find(ny_native_lambda_cache_for(b),
                                lambda_value_expr);
      const char *lambda_return_name = lambda_value_expr->as.lambda.return_type;
      bool raw_scalar_result =
          lambda_return_name && ny_native_type_name_is_int(lambda_return_name);
      if (!raw_scalar_result && materialized_lambda &&
          materialized_lambda->fn &&
          materialized_lambda->fn->kind == NY_S_FUNC &&
          materialized_lambda->fn->as.fn.return_semantic.resolved &&
          materialized_lambda->fn->as.fn.return_semantic.rep ==
              NY_SEM_REP_RAW_INT &&
          ny_sem_rep_coherent_with_type(
              lambda_return_name,
              materialized_lambda->fn->as.fn.return_semantic.rep) &&
          !ny_native_type_name_is_128bit_int(lambda_return_name))
        raw_scalar_result = true;
      /* An unannotated lambda has the dynamic NyValue result ABI.  Its
       * body may be scalar arithmetic, but that does not make the returned
       * word raw: the generated lambda still returns the canonical tagged
       * value consumed by the callback edge.  Only an explicit integer
       * result annotation opts into the raw-result adapter. */
      const char *mark_symbol =
          predicate_callback
              ? (callback_params_fnptr_only && callback_word_abi
                     ? "rt_mark_dynamic_bool_callable_tagged_word_args"
                 : (callback_params_raw && callback_word_abi)
                     ? "rt_mark_dynamic_bool_callable_word_args"
                 : callback_params_raw
                     ? "rt_mark_dynamic_bool_callable"
                     : "rt_mark_dynamic_bool_callable_tagged_args")
              : (raw_scalar_result
                     ? (callback_params_fnptr_only && callback_word_abi
                            ? "rt_mark_dynamic_callable_tagged_word_args_raw_"
                              "result"
                        : (callback_params_raw && callback_word_abi)
                            ? "rt_mark_dynamic_callable_word_args_raw_result"
                        : callback_params_raw
                            ? "rt_mark_dynamic_callable_raw_result"
                            : "rt_mark_dynamic_callable_tagged_args_raw_"
                              "result")
                     : (callback_params_fnptr_only && callback_word_abi
                            ? "rt_mark_dynamic_callable_tagged_word_args"
                        : (callback_params_raw && callback_word_abi)
                            ? "rt_mark_dynamic_callable_word_args"
                        : callback_params_raw
                            ? "rt_mark_dynamic_callable"
                            : "rt_mark_dynamic_callable_tagged_args"));
      arg = nyr_emit_runtime_call(b, mark_symbol, arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
    }
    const char *arg_nominal_type =
        arg_expr && arg_expr->kind == NY_E_IDENT &&
                arg_expr->semantic.resolved &&
                arg_expr->semantic.rep == NY_SEM_REP_RAW_INT
            ? "int"
        : arg_expr ? nyr_expr_type_name(b, arg_expr)
                   : NULL;
    bool arg_is_nominal_object = false;
    if (arg_nominal_type && *arg_nominal_type &&
        !ny_native_type_name_is_any(arg_nominal_type) &&
        !ny_native_type_name_is_int(arg_nominal_type) &&
        !ny_native_type_name_is_f64(arg_nominal_type) &&
        !ny_native_type_name_is_f32(arg_nominal_type) &&
        !ny_native_type_name_is_str(arg_nominal_type) &&
        !ny_native_type_name_is_list(arg_nominal_type) &&
        !ny_native_type_name_is_ptr(arg_nominal_type)) {
      const stmt_t *nominal = NULL;
      if (b->prog)
        nominal = nyr_find_layout_in_prog(b, b->prog, arg_nominal_type);
      arg_is_nominal_object = nominal && nominal->kind == NY_S_STRUCT;
    }
    if (!arg_is_nominal_object && arg_expr) {
      const expr_t *producer = arg_expr;
      if (producer->kind == NY_E_IDENT && producer->as.ident.name) {
        const expr_t *defined = nyr_find_top_level_value_in_source(
            b, producer->as.ident.name, producer->tok.filename);
        if (!defined)
          defined = nyr_find_top_level_value(b, producer->as.ident.name);
        if (defined && defined != producer)
          producer = defined;
      }
      const char *producer_name = NULL;
      if (producer->kind == NY_E_CALL && producer->as.call.callee &&
          producer->as.call.callee->kind == NY_E_IDENT)
        producer_name = producer->as.call.callee->as.ident.name;
      if (producer_name && b->prog) {
        const stmt_t *nominal =
            nyr_find_layout_in_prog(b, b->prog, producer_name);
        arg_is_nominal_object = nominal && nominal->kind == NY_S_STRUCT;
      }
    }
    bool raw_object_argument =
        nyr_expr_is_dict(b, arg_expr) || arg_is_nominal_object ||
        nyr_expr_is_list(b, arg_expr) || nyr_expr_is_set(b, arg_expr) ||
        nyr_expr_is_tuple(b, arg_expr) || nyr_expr_is_range(b, arg_expr) ||
        nyr_expr_is_bytes(b, arg_expr) || nyr_expr_is_bigint(b, arg_expr) ||
        nyr_expr_is_cstr(b, arg_expr) || nyr_expr_is_ptr(b, arg_expr) ||
        param_rep == NY_SEM_REP_OBJECT || param_rep == NY_SEM_REP_AGGREGATE ||
        param_rep == NY_SEM_REP_POINTER;
    /* Nominal handle types are opaque even when a flattened imported
     * declaration leaves the expression's semantic representation as a raw
     * integer.  Preserve the declared bigint ABI before the generic scalar
     * boundary recovery below. */
    if (arg_nominal_type && ny_type_is(arg_nominal_type, "bigint"))
      raw_object_argument = true;
    /* An index of an unparameterized sequence is not a proven integer.  Its
     * element may be a string, handle, or scalar depending on the runtime
     * value, even when HM conservatively recorded RAW_INT for the expression.
     * Preserve that value as an object/dynamic word at the next any boundary
     * instead of shifting a pointer as though it were an integer. */
    if (arg_expr && arg_expr->kind == NY_E_INDEX && arg_expr->as.index.target) {
      const expr_t *sequence = arg_expr->as.index.target;
      bool polymorphic_sequence = false;
      if (sequence->kind == NY_E_IDENT && sequence->as.ident.name) {
        const nyr_local_t *sequence_local =
            nyr_find_local(b, sequence->as.ident.name);
        polymorphic_sequence = sequence_local && sequence_local->is_list &&
                               !sequence_local->type_name;
      } else if (sequence->kind == NY_E_CALL ||
                 sequence->kind == NY_E_MEMCALL) {
        const stmt_t *sequence_fn =
            sequence->semantic.canonical_callee_stmt &&
                    sequence->semantic.canonical_callee_stmt->kind == NY_S_FUNC
                ? sequence->semantic.canonical_callee_stmt
                : NULL;
        const char *sequence_type =
            sequence_fn ? sequence_fn->as.fn.return_type : NULL;
        polymorphic_sequence =
            (sequence_type && strcmp(sequence_type, "list") == 0) ||
            nyr_expr_is_list(b, sequence) || nyr_expr_is_dyn_list(b, sequence);
      }
      if (polymorphic_sequence)
        raw_object_argument = true;
      /* Calls returning an unparameterized sequence may lose the concrete
       * element type during module flattening.  The index lowering has
       * already selected the representation-aware accessor, so preserve its
       * canonical result at this boundary even when the stale semantic fact
       * says RAW_INT. */
      if (sequence->kind == NY_E_CALL || sequence->kind == NY_E_MEMCALL)
        raw_object_argument = true;
    }
    /* Preserve resolved object representations even when a qualified call
     * lost its source-level constructor spelling during module flattening.
     * These values are handles, never raw integers to be tagged. */
    if (arg_expr && arg_expr->semantic.resolved &&
        (arg_expr->semantic.rep == NY_SEM_REP_OBJECT ||
         arg_expr->semantic.rep == NY_SEM_REP_AGGREGATE ||
         arg_expr->semantic.rep == NY_SEM_REP_POINTER ||
         arg_expr->semantic.rep == NY_SEM_REP_TYPED_BUFFER ||
         arg_expr->semantic.rep == NY_SEM_REP_STRING))
      raw_object_argument = true;
    /* Function values are opaque code pointers.  They may be passed through
     * an `any` registry (macros, hooks, callbacks), but must never be shifted
     * as if they were raw integer payloads. */
    if (arg_expr && arg_expr->kind == NY_E_IDENT &&
        nyr_find_user_function(b, arg_expr->as.ident.name))
      raw_object_argument = true;
    /* Dynamic string parameters require a managed string handle. Raw literal
     * C-string pointers are valid native values, but match/assert helpers
     * consume the tagged string ABI and otherwise classify the pointer as an
     * unknown object. */
    if (box_dynamic_argument && nyr_expr_is_cstr(b, arg_expr) &&
        (dynamic_sequence || ny_native_type_name_is_any(param_type) ||
         (!param_type && param_rep == NY_SEM_REP_TAGGED_DYNAMIC))) {
      arg = nyr_emit_runtime_call(b, "rt_cstr_to_str", arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
      /* The conversion returns an object handle, not an integer payload.
       * In particular a string-concatenation initializer is a binary AST
       * expression but must never trigger the raw-integer boxing below. */
      raw_object_argument = true;
    }
    bool raw_integer_argument = arg_expr && arg_expr->kind == NY_E_LITERAL &&
                                arg_expr->as.literal.kind == NY_LIT_INT &&
                                arg_expr->tok.kind != NY_T_NIL;
    /* A raw memory read returns the machine word stored in the slot.  Its
     * semantic declaration may still be `any`, or inference may describe it
     * as RAW_INT, but neither fact authorizes integer boxing: the word may be
     * a string/container handle.  Preserve the read's representation fact
     * until the dynamic ABI derives its length and tag below. */
    bool raw_dynamic_read = nyr_expr_is_raw_dynamic_read(b, arg_expr);
    if (nyr_expr_has_raw_int_abi(b, arg_expr, 0))
      raw_integer_argument = true;
    if (arg_expr && arg_expr->semantic.resolved &&
        arg_expr->semantic.rep == NY_SEM_REP_RAW_INT)
      raw_integer_argument = true;
    /* Top-level scalar defs are raw NYIR storage too, but their identifier
     * expression often has no completed semantic fact after module
     * flattening.  Recover that fact from the defining integer expression so
     * an `any` boundary receives a real tagged value instead of a raw word
     * paired with an integer tag (which halves negative/odd values on read).
     */
    if (!raw_integer_argument && arg_expr && arg_expr->kind == NY_E_IDENT &&
        arg_expr->as.ident.name) {
      const nyr_local_t *raw_int_local =
          nyr_find_local(b, arg_expr->as.ident.name);
      /* An is_any local already stores the tagged dynamic word; re-boxing
       * it double-tags (Z(n) -> nt_bigint(n) turned 12345 into 24691). */
      if (raw_int_local && !raw_int_local->is_any &&
          !raw_int_local->raw_dynamic_index &&
          (ny_native_type_name_is_int(raw_int_local->type_name) ||
           raw_int_local->semantic_rep == NY_SEM_REP_RAW_INT ||
           (!raw_int_local->scalar_class_mixed &&
            raw_int_local->scalar_class == NYIR_SCALAR_I64_RAW)))
        raw_integer_argument = true;
      const expr_t *defined = nyr_find_top_level_value_in_source(
          b, arg_expr->as.ident.name, arg_expr->tok.filename);
      if (!defined)
        defined = nyr_find_top_level_value(b, arg_expr->as.ident.name);
      if (defined && defined->kind != NY_E_LITERAL /* bool handled above */ &&
          (defined->kind == NY_E_BINARY || defined->kind == NY_E_UNARY)) {
        if (nyr_expr_is_bigint(b, defined))
          raw_object_argument = true;
        else
          raw_integer_argument = true;
      } else if (defined && defined->kind == NY_E_LITERAL &&
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
        param_rep == NY_SEM_REP_POINTER ||
        ny_native_type_name_is_str(param_type) ||
        ny_native_type_name_is_list(param_type) ||
        ny_native_type_name_is_bytes(param_type) ||
        ny_native_type_name_is_ptr(param_type))
      raw_integer_argument = false;
    /* The producer representation wins over stale/incomplete semantic
     * metadata at a dynamic boundary.  A native f64 expression may still
     * carry RAW_INT from an earlier unresolved type pass; shifting its IEEE
     * bits as an integer creates a different floating value. */
    if (arg_expr_f64 || arg_expr_f32)
      raw_integer_argument = false;
    if (raw_dynamic_read)
      raw_integer_argument = false;
    if (raw_object_argument)
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
          value_name ? nyr_find_user_function(b, value_name) : NULL;
      /* A top-level function value (for example `def f = fn(int x) int {}`)
       * has no ordinary function declaration under the identifier `f`.  Its
       * generated lambda still carries the concrete return ABI, so resolve
       * that initializer before deciding whether an `any` argument needs
       * integer boxing. */
      if (!value_fn && value_name && arg_expr->kind == NY_E_CALL) {
        const expr_t *global = nyr_find_top_level_value(b, value_name);
        ny_native_lambda_entry_t *lambda =
            global
                ? ny_native_lambda_find(ny_native_lambda_cache_for(b), global)
                : NULL;
        if (lambda)
          value_fn = lambda->fn;
      }
      const char *value_type = value_fn ? value_fn->as.fn.return_type : NULL;
      /* A resolved producer type owns the ABI at a dynamic boundary.  The
       * old fallback treated every return type not listed here as a raw
       * integer, which included opaque handles such as bigint and user impl
       * values.  Only an explicitly integer return (or its semantic raw-int
       * fact) may be tagged as a native scalar; all object-like returns stay
       * as their handle payload. */
      if (value_fn && value_fn->as.fn.return_semantic.resolved) {
        raw_integer_argument =
            value_fn->as.fn.return_semantic.rep == NY_SEM_REP_RAW_INT;
      } else if (value_type && ny_native_type_name_is_int(value_type)) {
        raw_integer_argument = true;
      } else if (value_type && (ny_native_type_name_is_str(value_type) ||
                                ny_native_type_name_is_list(value_type) ||
                                ny_native_type_name_is_f64(value_type) ||
                                ny_native_type_name_is_f32(value_type) ||
                                ny_native_type_name_is_ptr(value_type) ||
                                ny_native_type_name_is_bytes(value_type) ||
                                ny_native_type_name_is_any(value_type))) {
        raw_integer_argument = false;
      }
    }
    if (!raw_dynamic_read && !arg_expr_f64 && !arg_expr_f32 &&
        !raw_object_argument && nyr_expr_has_raw_int_abi(b, arg_expr, 0))
      raw_integer_argument = true;
    /* The arithmetic emitter records the physical result ABI.  A top-level
     * constant reference can hide raw-int provenance from the recursive
     * expression classifier, but a direct raw arithmetic argument still
     * needs one tag when passed to an `any` formal. */
    if (arg_expr && b->last_arith_expr == arg_expr && b->last_arith_raw &&
        !raw_dynamic_read && !raw_object_argument && !arg_expr_f64 &&
        !arg_expr_f32)
      raw_integer_argument = true;
    bool direct_bigint_argument = arg_expr && nyr_expr_is_bigint(b, arg_expr);
    if (direct_bigint_argument) {
      raw_object_argument = true;
      raw_integer_argument = false;
    }
    /* Descriptor-backed reads and dynamic accessors already return a
     * canonical Ny value.  A stale RAW_INT fact on the index expression
     * must not tag a nested object pointer when forwarding it to `any`. */
    bool explicit_raw_call_result =
        arg_expr &&
        (arg_expr->kind == NY_E_CALL || arg_expr->kind == NY_E_MEMCALL) &&
        nyr_expr_has_raw_int_abi(b, arg_expr, 0);
    bool explicit_raw_ternary = arg_expr && arg_expr->kind == NY_E_TERNARY &&
                                nyr_expr_has_raw_int_abi(b, arg_expr, 0);
    if (box_dynamic_argument && arg_expr &&
        nyr_expr_is_canonical_dynamic_value(b, arg_expr) &&
        !explicit_raw_call_result && !explicit_raw_ternary &&
        !nyr_expr_is_raw_integer_conversion(b, arg_expr)) {
      raw_object_argument = true;
      raw_integer_argument = false;
      /* Dynamic sequence accessors return canonical Ny words, but a stale
       * RAW_INT semantic fact can leave their register class raw. Preserve
       * the bits and record the dynamic carrier before final call-array
       * reconciliation; rt_box_i64 would turn an ADT/list handle into an
       * integer and discard its heap tag. */
      if (arg >= 0 && nyr_value_scalar_class(b, arg) == NYIR_SCALAR_I64_RAW) {
        arg = nyr_ir_emit(&b->nyir,
                        (nyr_inst_t){.op = NYIR_COPY,
                                      .dst = -1,
                                      .a = arg,
                                      .b = -1,
                                      .scalar_class = NYIR_SCALAR_I64_TAGGED});
        if (arg < 0)
          return -1;
        producer_class = NYIR_SCALAR_I64_TAGGED;
      }
    }
    /* An explicitly declared `any` formal owns the dynamic ABI even when
     * body inference also proves scalar arithmetic. */
    if (ny_native_type_name_is_any(param_type))
      box_dynamic_argument = true;
    /* The bigint-to-int bridge returns a raw i64 even when its compatibility
     * declaration makes the expression look like a canonical object. */
    if (box_dynamic_argument &&
        nyr_expr_is_raw_integer_conversion(b, arg_expr)) {
      raw_object_argument = false;
      raw_integer_argument = true;
    }
    /* The boolean boundary above already produced a canonical Ny immediate.
     * Do not let the later raw-integer recovery for an explicit `any` formal
     * reinterpret that immediate as a small integer and box it again. */
    if (arg_expr && nyr_expr_is_bool(b, arg_expr)) {
      raw_integer_argument = false;
      box_dynamic_argument = false;
    }
    /* An is_any local already stores the tagged dynamic word; re-boxing it
     * double-tags (Z(n) -> nt_bigint(n) turned 12345 into 24691) even when
     * the expression-level semantic still records RAW_INT. */
    bool arg_local_is_any = false;
    if (arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      const nyr_local_t *al = nyr_find_local(b, arg_expr->as.ident.name);
      arg_local_is_any = al && al->is_any &&
                         nyr_value_scalar_class(b, arg) != NYIR_SCALAR_I64_RAW;
    }
    /* Inferred locals can have no source type or semantic rep even when all
     * stores are raw integers. The contextual `any` load then carries a
     * stale tagged class, so the generic class-based conversion below would
     * mistake raw payload bits for an already boxed value. Recover the
     * storage fact from the binder, stamp an explicit raw copy, and box once
     * before expanding the any ABI's value/length/tag slots. */
    if (box_dynamic_argument && raw_integer_argument && arg_local &&
        !arg_local_is_any && !arg_local->scalar_class_mixed &&
        arg_local->scalar_class == NYIR_SCALAR_I64_RAW && arg >= 0) {
      int raw = nyr_ir_emit(&b->nyir,
                          (nyr_inst_t){.op = NYIR_COPY,
                                        .dst = -1,
                                        .a = arg,
                                        .b = -1,
                                        .scalar_class = NYIR_SCALAR_I64_RAW});
      arg = raw < 0 ? -1
                    : nyr_emit_runtime_call(b, "rt_box_i64", raw, -1, -1, 1,
                                            NYIR_SCALAR_I64_TAGGED);
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_TAGGED;
    }
    /* Defer scalar boxing until formal_class is resolved below. Doing it here
     * too gives the final physical-ABI reconciliation a raw/unknown producer
     * and boxes the same value a second time (42 becomes 85 at `any` calls).
     */
    /* rt_long is the generic dynamic-normalization boundary: it decodes
     * immediate integers (is_int -> untag), so a proven raw-int receiver
     * must take the raw scalar bridge instead (`123.long` became 61 —
     * rt_long halved the raw word).  Anything already dynamic (any local,
     * string, bigint handle, container) keeps rt_long and passes through
     * unchanged. */
    if (runtime_overload_abi && runtime_overload_abi->raw_i64_symbol &&
        i == 0 && arg >= 0 && arg_expr && !arg_expr_f64 && !arg_expr_f32 &&
        !nyr_expr_is_any(b, arg_expr) && !nyr_expr_is_cstr(b, arg_expr) &&
        !nyr_expr_is_bigint(b, arg_expr) && !nyr_expr_is_list(b, arg_expr) &&
        !nyr_expr_is_dict(b, arg_expr) && !nyr_expr_is_bool(b, arg_expr) &&
        (arg_expr->semantic.resolved
             ? arg_expr->semantic.rep == NY_SEM_REP_RAW_INT
             : true))
      runtime_symbol = runtime_overload_abi->raw_i64_symbol;
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
    bool raw_scalar_type = !param_type ||
                           ny_native_type_name_is_int(param_type) ||
                           (param_type && strcmp(param_type, "bool") == 0) ||
                           param_rep == NY_SEM_REP_RAW_INT;
    bool raw_scalar_parameter = callee_fn && !indirect_callable &&
                                !box_dynamic_argument && raw_scalar_type &&
                                !raw_object_argument &&
                                !ny_native_type_name_is_list(param_type) &&
                                !ny_native_type_name_is_str(param_type) &&
                                !ny_native_type_name_is_f64(param_type) &&
                                !ny_native_type_name_is_f32(param_type);
    /*
     * Imported module constants retain their initializer AST at the call
     * site.  Recover that pure integer value before dynamic-expression
     * heuristics can decode its raw constant payload as though it were a
     * tagged Ny integer (for example, a folded capability mask 127 becoming
     * 63 at an i32 formal).
     */
    bool folded_module_int_argument = false;
    if (raw_scalar_parameter && arg_expr && !arg_local) {
      const char *constant_module =
          b->module_name && b->module_name[0]
              ? b->module_name
              : nyr_module_for_initializer_expr(b->prog, arg_expr);
      int64_t folded_value = 0;
      if (nyr_fold_top_level_int_in_module(b, NULL, constant_module, arg_expr,
                                           &folded_value, 0)) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "native-constant",
              "function=%s argument=%zu expression-kind=%d value=%lld "
              "action=materialize-constant",
              b->current_fn_name ? b->current_fn_name : "<entry>", i + 1,
              (int)arg_expr->kind, (long long)folded_value);
        arg = nyr_ir_emit(&b->nyir,
                        (nyr_inst_t){.op = NYIR_CONST_I64,
                                      .dst = -1,
                                      .a = -1,
                                      .b = -1,
                                      .imm = folded_value,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
        if (arg < 0)
          return -1;
        producer_class = NYIR_SCALAR_I64_RAW;
        folded_module_int_argument = true;
      }
    }
    /* A value narrowed from an explicit `any` parameter still occupies the
     * dynamic/tagged ABI slot even when the source type checker proves the
     * branch is an integer (for example `if is_int(x) {
     * bigint_from_int(x) }`).  The refinement changes what the callee may
     * accept; it does not rewrite the caller's physical word.  Decode that
     * word exactly at the typed-scalar boundary. */
    bool dynamic_source_scalar = false;
    if (param_type && ny_native_type_name_is_int(param_type) && arg_expr &&
        arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name) {
      const nyr_local_t *source = nyr_find_local(b, arg_expr->as.ident.name);
      /* A dynamic refinement may carry a shadow tag slot after the value has
       * already been normalized to a raw scalar.  The slot alone does not
       * prove this particular load is tagged; decoding a raw odd integer a
       * second time halves it (e.g. nested helper `inner(a)` for a=5). */
      dynamic_source_scalar =
          source && source->dyn_tag_slot >= 0 && arg >= 0 &&
          nyr_value_scalar_class(b, arg) == NYIR_SCALAR_I64_TAGGED;
    }
    if (dynamic_source_scalar &&
        !ny_native_runtime_arg_abi_preserve_tagged(runtime_arg_abi, i) &&
        arg_expr && !arg_expr_f64 && !arg_expr_f32) {
      arg = nyr_emit_runtime_call(b, "rt_any_to_i64", arg, -1, -1, 1, 0);
      if (arg < 0)
        return -1;
    }
    if (raw_scalar_parameter && !dynamic_source_scalar &&
        !folded_module_int_argument &&
        !ny_native_runtime_arg_abi_preserve_tagged(runtime_arg_abi, i) &&
        arg_expr && !arg_expr_f64 && !arg_expr_f32 &&
        nyr_value_scalar_class(b, arg) != NYIR_SCALAR_I64_RAW &&
        ((!default_raw_constant && nyr_expr_is_any(b, arg_expr) &&
          (arg_local_is_any || !nyr_expr_is_raw_int(arg_expr))) ||
         (arg_expr->kind == NY_E_IDENT &&
          nyr_find_local(b, arg_expr->as.ident.name) &&
          nyr_find_local(b, arg_expr->as.ident.name)->is_any))) {
      arg = nyr_emit_runtime_call(b, "rt_any_to_i64", arg, -1, -1, 1, 0);
    }
    /*
     * The AST-level float flags can miss a float producer stamped by the
     * lowering itself (a member load re-boxed through rt_any_to_f64).  The
     * register class is authoritative: an f64-classified word must cross a
     * dynamic boundary as a heap-float handle, never as raw IEEE bits.
     */
    bool arg_reg_f32 =
        !arg_expr_f64 && !arg_expr_f32 && nyr_value_is_f32(b, arg, 0);
    bool arg_reg_f64 =
        !arg_expr_f64 && !arg_expr_f32 && nyr_value_is_f64(b, arg, 0);
    if (box_dynamic_argument &&
        nyr_value_scalar_class(b, arg) != NYIR_SCALAR_I64_TAGGED &&
        (arg_expr_f64 || arg_expr_f32 || arg_reg_f32 || arg_reg_f64)) {
      /* Native f32/f64 values live in floating NYIR slots. A dynamic `any`
       * parameter needs the normal heap-float handle, not the raw IEEE word
       * shifted through integer tagging. Promote f32 before boxing so both
       * paths share the f64 runtime representation. */
      int as_f64 =
          (arg_expr_f32 || arg_reg_f32) ? nyr_emit_f32_to_f64(b, arg) : arg;
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      arg = bits < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                             1, 0);
      if (arg < 0)
        return -1;
    }
    bool inferred_list = param_inferred_list;
    nyr_scalar_class_t scalar_arg_class = nyr_value_scalar_class(b, arg);
    if ((param_rep == NY_SEM_REP_F32 ||
         ny_native_type_name_is_f32(param_type) ||
         (c_params && i < NY_C_MAX_PARAMS && c_params->param_f32[i])) &&
        scalar_arg_class != NYIR_SCALAR_F32) {
      if (scalar_arg_class == NYIR_SCALAR_I64_TAGGED) {
        int as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", arg, -1, -1, 1,
                                           NYIR_SCALAR_F64);
        arg = as_f64 < 0 ? -1 : nyr_emit_f64_to_f32(b, as_f64);
      } else if (scalar_arg_class == NYIR_SCALAR_F64) {
        arg = nyr_emit_f64_to_f32(b, arg);
      } else {
        arg = nyr_emit_i64_to_f32(b, arg);
      }
      producer_class = NYIR_SCALAR_F32;
    } else if (param_rep == NY_SEM_REP_F64 ||
               ny_native_type_name_is_f64(param_type) ||
               (c_params && i < NY_C_MAX_PARAMS && c_params->param_f64[i])) {
      /*
       * Convert only when the operand is not already f64.  Float literals
       * lower as f64 words (whole or fractional), so converting a
       * whole-number literal reinterprets its IEEE bits as an integer:
       * fhalf(5.0) became 4.6e18 and printed 2.30866e+18 for 5.0/2.0 on
       * the VM.  Plain integers still convert numerically below.
       */
      if (scalar_arg_class != NYIR_SCALAR_F64) {
        if (scalar_arg_class == NYIR_SCALAR_I64_TAGGED) {
          arg = nyr_emit_runtime_call(b, "rt_any_to_f64", arg, -1, -1, 1,
                                      NYIR_SCALAR_F64);
        } else if (arg_expr_f32 || scalar_arg_class == NYIR_SCALAR_F32) {
          arg = nyr_emit_f32_to_f64(b, arg);
        } else {
          arg = nyr_emit_i64_to_f64(b, arg);
        }
        producer_class = NYIR_SCALAR_F64;
      }
    } else if ((param_rep == NY_SEM_REP_RAW_INT ||
                ny_native_type_name_is_int(param_type)) &&
               !ny_native_runtime_arg_abi_preserve_tagged(runtime_arg_abi, i)) {
      nyr_scalar_class_t actual_class = nyr_value_scalar_class(b, arg);
      int64_t raw_constant = 0;
      bool folded_raw_constant =
          !arg_local && arg_expr &&
          nyr_fold_top_level_int_in_module(b, NULL, b->module_name, arg_expr,
                                           &raw_constant, 0);
      /* Integer constants from module scope are emitted as raw NYIR words.
       * A conservative tagged fact on the identifier must not send that
       * payload through rt_any_to_i64 (the raw constant 1 would decode to
       * zero). Re-materialize the proven constant with its authoritative
       * scalar class at this typed-int boundary. */
      if (actual_class == NYIR_SCALAR_I64_TAGGED && folded_raw_constant) {
        arg = nyr_emit_const(b, raw_constant);
        if (arg < 0)
          return -1;
        actual_class = NYIR_SCALAR_I64_RAW;
        producer_class = NYIR_SCALAR_I64_RAW;
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "native-abi",
              "function=%s arg=%zu action=preserve-constant-as-raw-int "
              "value=%lld reason=module-initializer-folded",
              b->current_fn_name ? b->current_fn_name : "<entry>", i + 1,
              (long long)raw_constant);
      }
      /* The load expression's contextual class can be stale when HM
       * narrows an `any` local at this use.  Its slot still carries the
       * established tagged representation (for example a `load64` result
       * stored in an inferred dynamic local); decode according to that
       * storage fact instead of trusting the load's raw-int stamp. */
      bool arg_local_tagged =
          arg_local &&
          (arg_local->is_any ||
           ny_native_type_name_is_any(arg_local->type_name)) &&
          actual_class != NYIR_SCALAR_I64_RAW;
      if (actual_class == NYIR_SCALAR_I64_TAGGED || arg_local_tagged) {
        arg = nyr_emit_runtime_call(b, "rt_any_to_i64", arg, -1, -1, 1,
                                    NYIR_SCALAR_I64_RAW);
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && arg_local_tagged)
          ny_dbg_heuristic_event(
              "native-abi",
              "function=%s arg=%zu action=decode-any-local-to-i64 "
              "reason=local-slot-is-tagged formal=int",
              b->current_fn_name ? b->current_fn_name : "<entry>", i + 1);
      } else if (actual_class == NYIR_SCALAR_F64 ||
                 (actual_class == NYIR_SCALAR_UNKNOWN && arg_expr_f64)) {
        arg = nyr_emit_f64_to_i64(b, arg);
      } else if (actual_class == NYIR_SCALAR_F32 ||
                 (actual_class == NYIR_SCALAR_UNKNOWN && arg_expr_f32)) {
        arg = nyr_emit_f32_to_i64(b, arg);
      }
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_RAW;
    }
    /*
     * A declared str formal consumes a C-string pointer.  A dynamic
     * (tagged-class) argument must cross through the value-to-cstr bridge,
     * or the arg register keeps its tagged class, the callee's string
     * formal disagrees at the verify layer, and a real tagged word would
     * be dereferenced as a raw pointer at run time.
     */
    if (param_type && ny_native_type_name_is_str(param_type)) {
      nyr_scalar_class_t arg_class = nyr_value_scalar_class(b, arg);
      if (arg_class == NYIR_SCALAR_I64_TAGGED) {
        arg = nyr_emit_runtime_call(b, "rt_any_to_cstr", arg, -1, -1, 1, 0);
        producer_class = NYIR_SCALAR_STRING;
      } else if (arg_class == NYIR_SCALAR_F64 || arg_class == NYIR_SCALAR_F32) {
        /*
         * An untyped local the checker typed str while the lowering
         * computed it in the f64 domain: cross through the number-to-string
         * bridge, or the f64 register is dereferenced as a C pointer
         * (dgemm's repr path, float-cmp-oracle's builder append).
         */
        int as_f64 =
            arg_class == NYIR_SCALAR_F32 ? nyr_emit_f32_to_f64(b, arg) : arg;
        arg = as_f64 < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw",
                                                 as_f64, -1, -1, 1, 0);
        producer_class = NYIR_SCALAR_STRING;
      }
    }
    /* Dynamic values share a machine-word carrier with the runtime handles
     * consumed by object, buffer, closure, and pointer formals.  Establish
     * the formal's checked/unboxed ABI at the call boundary instead of
     * letting an any-local's tagged class leak into the callee.  The runtime
     * conversion preserves non-integer handles and decodes tagged integer
     * payloads; the verifier then sees the exact formal contract. */
    bool tagged_dynamic_local = arg_local_is_any;
    if ((!is_c_call || param_rep == NY_SEM_REP_TYPED_BUFFER) && arg >= 0 &&
        (nyr_value_scalar_class(b, arg) == NYIR_SCALAR_I64_TAGGED ||
         tagged_dynamic_local) &&
        (param_rep == NY_SEM_REP_OBJECT || param_rep == NY_SEM_REP_AGGREGATE ||
         param_rep == NY_SEM_REP_TYPED_BUFFER ||
         param_rep == NY_SEM_REP_POINTER || param_rep == NY_SEM_REP_CLOSURE)) {
      nyr_scalar_class_t target_class =
          ny_native_scalar_class_from_sem_rep(param_rep);
      arg = nyr_emit_runtime_call(b, "rt_any_to_i64", arg, -1, -1, 1,
                                  target_class);
      if (arg < 0)
        return -1;
      producer_class = target_class;
      if (tagged_dynamic_local && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu "
            "action=decode-dynamic-local "
            "reason=local-slot-is-tagged formal-contract",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol ? runtime_symbol : "native", i + 1);
    }
    /* Ny nominal impl values can be dictionary-backed object handles even
     * though their nominal type maps to aggregate. If a same-typed source and
     * formal disagree only between those one-word managed-handle classes,
     * preserve the bits and stamp the formal class. C by-value structs never
     * enter this bridge: they are C calls or carry aggregate on both sides.
     */
    const char *actual_nominal_type =
        arg_expr && arg_expr->semantic.resolved_type_name
            ? arg_expr->semantic.resolved_type_name
        : arg_expr ? nyr_expr_type_name(b, arg_expr)
                   : NULL;
    bool same_nominal_type =
        actual_nominal_type && param_type &&
        (strcmp(actual_nominal_type, param_type) == 0 ||
         (actual_nominal_type[0] == '?' &&
          strcmp(actual_nominal_type + 1, param_type) == 0));
    nyr_scalar_class_t actual_handle_class =
        arg >= 0 ? nyr_value_scalar_class(b, arg) : NYIR_SCALAR_UNKNOWN;
    bool nominal_handle_class = actual_handle_class == NYIR_SCALAR_OBJECT ||
                                actual_handle_class == NYIR_SCALAR_AGGREGATE;
    bool nominal_formal_class =
        param_rep == NY_SEM_REP_OBJECT || param_rep == NY_SEM_REP_AGGREGATE;
    if (!is_c_call && arg >= 0 && nominal_handle_class &&
        nominal_formal_class && same_nominal_type &&
        actual_handle_class != ny_native_scalar_class_from_sem_rep(param_rep)) {
      nyr_scalar_class_t target_class =
          ny_native_scalar_class_from_sem_rep(param_rep);
      arg = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_COPY,
                                              .dst = -1,
                                              .a = arg,
                                              .b = -1,
                                              .scalar_class = target_class});
      if (arg < 0)
        return -1;
      producer_class = target_class;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s arg=%zu action=preserve-managed-nominal-handle "
            "source=%s target=%s reason=same-nominal-type",
            b->current_fn_name ? b->current_fn_name : "<entry>", i + 1,
            nyr_scalar_class_name(actual_handle_class),
            nyr_scalar_class_name(target_class));
    }
    /* Managed aggregate handles and opaque pointers can cross an `any` ABI
     * boundary by preserving their machine word. C by-value structs stay on
     * the aggregate ABI path above and are never reclassified here. The COPY
     * records the representation boundary for NYIR/verifiers without
     * changing the payload bits. */
    nyr_scalar_class_t dynamic_source_class =
        arg >= 0 ? nyr_value_scalar_class(b, arg) : NYIR_SCALAR_UNKNOWN;
    bool semantic_aggregate_handle =
        arg_expr && arg_expr->semantic.resolved &&
        arg_expr->semantic.rep == NY_SEM_REP_AGGREGATE &&
        dynamic_source_class == NYIR_SCALAR_UNKNOWN;
    bool bigint_word_formal = param_type && ny_type_is(param_type, "bigint");
    bool legacy_handle_tag_bridge =
        !arg_is_nominal_object &&
        (box_dynamic_argument || bigint_word_formal) &&
        (dynamic_source_class == NYIR_SCALAR_AGGREGATE ||
         semantic_aggregate_handle ||
         dynamic_source_class == NYIR_SCALAR_POINTER ||
         (bigint_word_formal && dynamic_source_class == NYIR_SCALAR_UNKNOWN));
    if (arg >= 0 && legacy_handle_tag_bridge) {
      arg = nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = arg,
                                    .b = -1,
                                    .scalar_class = NYIR_SCALAR_I64_TAGGED});
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_TAGGED;
    }
    if (arg < 0)
      return -1;
    if (ny_trace_enabled("NY_TRACE_ABI")) {
      const char *class_name =
          producer_class == NYIR_SCALAR_I64_RAW        ? "i64-raw"
          : producer_class == NYIR_SCALAR_I64_TAGGED   ? "i64-tagged"
          : producer_class == NYIR_SCALAR_F32          ? "f32"
          : producer_class == NYIR_SCALAR_F64          ? "f64"
          : producer_class == NYIR_SCALAR_STRING       ? "string"
          : producer_class == NYIR_SCALAR_TYPED_BUFFER ? "typed-buffer"
          : producer_class == NYIR_SCALAR_POINTER      ? "pointer"
          : producer_class == NYIR_SCALAR_OBJECT       ? "object"
                                                       : "unknown";
      const char *source_rep = arg_expr && arg_expr->semantic.resolved
                                   ? ny_sem_rep_name(arg_expr->semantic.rep)
                                   : "unresolved";
      const char *formal_rep_name =
          formal_rep == NY_SEM_REP_F32              ? "f32"
          : formal_rep == NY_SEM_REP_F64            ? "f64"
          : formal_rep == NY_SEM_REP_RAW_INT        ? "raw-int"
          : formal_rep == NY_SEM_REP_TAGGED_DYNAMIC ? "tagged-dynamic"
          : formal_rep == NY_SEM_REP_OBJECT         ? "object"
          : formal_rep == NY_SEM_REP_POINTER        ? "pointer"
          : formal_rep == NY_SEM_REP_TYPED_BUFFER   ? "typed-buffer"
          : formal_rep == NY_SEM_REP_STRING         ? "string"
          : formal_rep == NY_SEM_REP_AGGREGATE      ? "aggregate"
                                                    : "unresolved";
      const char *action = box_dynamic_argument ? "box-any"
                           : ny_native_type_name_is_f32(param_type) ||
                                   param_rep == NY_SEM_REP_F32
                               ? "convert-to-f32"
                           : ny_native_type_name_is_f64(param_type) ||
                                   param_rep == NY_SEM_REP_F64
                               ? "convert-to-f64"
                           : ny_native_type_name_is_int(param_type) ||
                                   param_rep == NY_SEM_REP_RAW_INT
                               ? "convert-to-int"
                           : raw_object_argument ? "preserve-handle"
                                                 : "preserve-native";
      ny_trace_line(
          "ABI",
          "[abi-arg] %s arg=%zu source=%s actual=%s formal=%s/%s action=%s",
          b->current_fn_name ? b->current_fn_name : "?", i + 1, source_rep,
          class_name, param_type ? param_type : "untyped", formal_rep_name,
          action);
    }
    /*
     * A bool-typed argument crossing into a dynamically typed parameter
     * carries the comparison lowering's raw 0/1, which dynamic truthiness
     * reads as the tagged integer 0 (`true` became falsy inside the callee
     * and Rule 110's `if c || r` never fired for live cells).  Box to the
     * canonical NY_IMM_TRUE/NY_IMM_FALSE encoding; statically-typed bool
     * parameters keep the raw ABI and extern calls keep their C ABI.
     */
    if (!is_c_call && arg_expr && nyr_expr_is_bool(b, arg_expr) &&
        (!param_type || ny_native_type_name_is_any(param_type) ||
         param_rep == NY_SEM_REP_TAGGED_DYNAMIC ||
         param_rep == NY_SEM_REP_UNRESOLVED)) {
      if (arg_expr->kind == NY_E_LITERAL &&
          arg_expr->as.literal.kind == NY_LIT_BOOL) {
        arg = nyr_emit_const(b, arg_expr->as.literal.as.b ? NY_IMM_TRUE
                                                          : NY_IMM_FALSE);
      } else {
        /* Scan-free box: correct for raw 0/1 and boxed 2/8 alike (a
           backward defining-instruction scan misreads reused registers). */
        int got = nyr_emit_cmp_i64(b, NYIR_CMP_NE, arg, nyr_emit_const(b, 0));
        int not_false = nyr_emit_cmp_i64(b, NYIR_CMP_NE, arg,
                                         nyr_emit_const(b, NY_IMM_FALSE));
        if (got < 0 || not_false < 0)
          return -1;
        int truthy = nyr_emit_binop(b, NYIR_AND_I64, got, not_false);
        int six = nyr_emit_const(b, 6);
        int mul = truthy < 0 || six < 0
                      ? -1
                      : nyr_emit_binop(b, NYIR_MUL_I64, truthy, six);
        arg = mul < 0 ? -1
                      : nyr_emit_binop(b, NYIR_ADD_I64, mul,
                                       nyr_emit_const(b, NY_IMM_FALSE));
        if (arg < 0)
          return -1;
      }
    }
    if (lowered_argc >= NYIR_CALL_MAX_ARGS) {
      return nyr_fail(b,
                      "native NYIR lower: expanded call exceeds the maximum "
                      "supported argument count (%d)",
                      NYIR_CALL_MAX_ARGS),
             -1;
    }
    nyr_scalar_class_t formal_class =
        ny_native_scalar_class_from_sem_rep(formal_rep);
    /* Length is representation-polymorphic: rt_len accepts a tagged dynamic
     * list, a typed-buffer handle, a string, or a dictionary in the same
     * machine-word slot. The source `len` overload's semantic formal can be
     * aggregate even when an indexed list element reaches this call tagged.
     * Preserve the actual carrier for this declared polymorphic bridge. */
    if (runtime_symbol && (strcmp(runtime_symbol, "rt_len") == 0 ||
                           strcmp(runtime_symbol, "rt_len_strict") == 0))
      formal_class = nyr_value_scalar_class(b, arg);
    const ny_native_runtime_abi_decl_t *runtime_call_abi =
        runtime_c_call ? ny_native_runtime_abi_decl(runtime_symbol) : NULL;
    /* The BigInt nominal type accepts both tagged small integers and tagged
     * heap handles. Its source representation stays specialized in the
     * callee, while this physical argument slot uses the common tagged
     * one-word carrier. */
    const char *arg_static_type =
        arg_expr ? nyr_expr_type_name(b, arg_expr) : NULL;
    /* The mutex bridges accept raw native addresses as well as tagged
     * handles. A statically raw integer passed through a typed i64 buffer is
     * already the address word; boxing and untagging it at this boundary is
     * redundant. Keep dynamic any handles on their tagged ABI below. */
    bool raw_mutex_handle =
        runtime_call_abi &&
        (runtime_call_abi->flags & NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING) &&
        runtime_symbol &&
        (!strcmp(runtime_symbol, "rt_mutex_lock64") ||
         !strcmp(runtime_symbol, "rt_mutex_unlock64")) &&
        (ny_native_type_name_is_int(arg_static_type) ||
         (arg_local && ny_native_type_name_is_int(arg_local->type_name)));
    if (ny_native_runtime_arg_abi_preserve_tagged(runtime_arg_abi, i))
      formal_class = NYIR_SCALAR_I64_TAGGED;
    else if (param_type && ny_type_is(param_type, "bigint"))
      formal_class = NYIR_SCALAR_I64_TAGGED;
    /* Explicit `any` owns the tagged-word ABI even when body inference has
     * refined a use of that parameter to raw integer arithmetic.  The call
     * expansion below already appends its dynamic length/tag lanes; keeping
     * the inferred RAW_INT class here left raw ternary payloads in that
     * tagged value lane (curl's CURLOPT_FOLLOWLOCATION is one example). */
    else if (ny_native_type_name_is_any(param_type) && !raw_mutex_handle)
      formal_class = NYIR_SCALAR_I64_TAGGED;
    else if (param_type && ny_native_type_name_is_str(param_type))
      formal_class = NYIR_SCALAR_STRING;
    else if (param_type && ny_native_type_name_is_int(param_type) &&
             param_rep != NY_SEM_REP_F64 && param_rep != NY_SEM_REP_F32)
      formal_class = NYIR_SCALAR_I64_RAW;
    else if (ny_native_runtime_abi_arg_is_tagged(runtime_call_abi, i))
      formal_class = NYIR_SCALAR_I64_TAGGED;
    else if (ny_native_runtime_abi_accepts_bigint_values(runtime_call_abi))
      formal_class = NYIR_SCALAR_I64_TAGGED;
    else if (ny_native_runtime_arg_abi_is_pointer(runtime_arg_abi, i))
      formal_class = NYIR_SCALAR_POINTER;
    else if (runtime_arg_abi && i < 16 &&
             (runtime_arg_abi->object_arg_mask & (uint16_t)(1u << (unsigned)i)))
      formal_class = NYIR_SCALAR_OBJECT;
    else if (runtime_arg_abi && i < 16 &&
             (runtime_arg_abi->typed_buffer_arg_mask &
              (uint16_t)(1u << (unsigned)i)))
      formal_class = NYIR_SCALAR_TYPED_BUFFER;
    else if (ny_native_runtime_abi_arg_is_f64(runtime_call_abi, i))
      formal_class = NYIR_SCALAR_F64;
    else if (!box_dynamic_argument &&
             (ny_native_type_name_is_str(arg_static_type) ||
              (arg_local && ny_native_type_name_is_str(arg_local->type_name))))
      formal_class = NYIR_SCALAR_STRING;
    else if (!box_dynamic_argument && formal_rep != NY_SEM_REP_F64 &&
             formal_rep != NY_SEM_REP_F32 &&
             (ny_native_type_name_is_int(arg_static_type) ||
              (arg_local && ny_native_type_name_is_int(arg_local->type_name))))
      formal_class = NYIR_SCALAR_I64_RAW;
    else if (runtime_call_abi &&
             (runtime_call_abi->flags &
              NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING) &&
             !(runtime_call_abi->flags & NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES)) {
      /* Raw runtime entries with no per-argument override consume machine
       * words. Leaving the default as the source expression's tagged class
       * let values such as `.len` bypass the required any_to_i64 boundary. */
      formal_class = NYIR_SCALAR_I64_RAW;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu decision=formal-raw-i64 "
            "reason=no-dynamic-boxing-runtime-contract",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol ? runtime_symbol : "native", i + 1);
    }
    /* The NYIR call verifier models the dictionary bridge's value lane as a
     * tagged Ny word. Keep that transport contract even though the runtime
     * descriptor also accepts raw native integers for direct C callers. */
    nyr_scalar_class_t dictionary_value_class = nyr_value_scalar_class(b, arg);
    bool dictionary_value_is_raw =
        dictionary_value_class == NYIR_SCALAR_I64_RAW ||
        (dictionary_value_class == NYIR_SCALAR_UNKNOWN &&
         producer_class != NYIR_SCALAR_I64_TAGGED &&
         ((arg_local && !arg_local->is_any &&
           (arg_local->semantic_rep == NY_SEM_REP_RAW_INT ||
            ny_native_type_name_is_int(arg_local->type_name))) ||
          (arg_expr &&
           ny_native_type_name_is_int(nyr_expr_type_name(b, arg_expr)))));
    if (value_abi && i == 2 && dictionary_value_is_raw) {
      arg = nyr_emit_runtime_call(b, "rt_box_i64", arg, -1, -1, 1,
                                  NYIR_SCALAR_I64_TAGGED);
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_TAGGED;
      formal_class = NYIR_SCALAR_I64_TAGGED;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=3 descriptor=dynamic-dict-value "
            "action=box-for-nyir-transport",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol ? runtime_symbol : "native");
    }
    /* Bytes values share the physical string-pointer payload, but may flow
     * through a tagged any-shaped local slot. Preserve the handle bits and
     * carry the statically proven bytes/string ABI across this boundary. */
    if (formal_class == NYIR_SCALAR_STRING && arg_expr &&
        nyr_expr_is_bytes(b, arg_expr) &&
        nyr_value_scalar_class(b, arg) == NYIR_SCALAR_I64_TAGGED) {
      arg = nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = arg,
                                    .b = -1,
                                    .scalar_class = NYIR_SCALAR_STRING});
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_STRING;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu "
            "decision=preserve-tagged-bytes-payload-as-string formal=string",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol ? runtime_symbol : "native", i + 1);
    }
    /* A str local can be written by a helper whose result annotation is
     * tagged even though its concrete string contract is known. Stamp an
     * explicit identity conversion so the call and its verifier agree on
     * the pointer payload class. */
    if (formal_class == NYIR_SCALAR_STRING && arg_expr &&
        arg_expr->kind == NY_E_IDENT && arg_local && arg_local->is_cstr) {
      arg = nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = arg,
                                    .b = -1,
                                    .scalar_class = NYIR_SCALAR_STRING});
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_STRING;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu source=str-local decision=str-copy",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol ? runtime_symbol : "native", i + 1);
    }
    /* A typed list local may flow through a generic clone helper whose
     * result is conservatively marked as an object handle. The source-level
     * list fact makes this one-word handle conversion safe and explicit. */
    if (formal_class == NYIR_SCALAR_TYPED_BUFFER && arg_expr &&
        nyr_expr_is_list(b, arg_expr) &&
        nyr_value_scalar_class(b, arg) == NYIR_SCALAR_OBJECT) {
      arg = nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){.op = NYIR_COPY,
                                    .dst = -1,
                                    .a = arg,
                                    .b = -1,
                                    .scalar_class = NYIR_SCALAR_TYPED_BUFFER});
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_TYPED_BUFFER;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu object-handle→typed-buffer "
            "via=list-source-fact",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol
                ? runtime_symbol
                : (callee_fn && callee_fn->as.fn.name ? callee_fn->as.fn.name
                                                      : "<indirect>"),
            i + 1);
    }
    if (ny_trace_enabled("NY_TRACE_ABI"))
      ny_trace_line(
          "ABI",
          "[abi-contract] %s arg=%zu source=%s actual=%s formal=%s via=%s",
          b->current_fn_name ? b->current_fn_name : "?", i + 1,
          runtime_symbol
              ? runtime_symbol
              : (callee_fn && callee_fn->as.fn.name ? callee_fn->as.fn.name
                                                    : "native"),
          nyr_scalar_class_name(nyr_value_scalar_class(b, arg)),
          nyr_scalar_class_name(formal_class),
          runtime_arg_abi    ? "runtime-arg-table"
          : runtime_call_abi ? "runtime-abi-table"
                             : "semantic-formal");

    /* Enforce the selected one-word dynamic ABI from the producer's final
     * class. BigInt is an immediate-or-handle representation: it can be a
     * tagged small integer or an opaque heap handle. Its conservative scalar
     * class can be stale (for example, a large literal handle may have flowed
     * through an integer-shaped temporary), so never reinterpret its payload
     * based only on that class. */
    nyr_scalar_class_t final_arg_class = nyr_value_scalar_class(b, arg);
    nyr_scalar_class_t tagged_source_class = final_arg_class;
    /*
     * Physical producer facts win over stale semantic annotations. An
     * inferred parameter can be semantically tagged while its actual local
     * slot was normalized to raw i64; re-decoding that raw word here halves
     * odd values (nested `inner(a)` with a=5 became 2). Consult source-level
     * dynamic facts only when lowering has no class for the produced value.
     */
    bool arg_is_tagged_dynamic =
        final_arg_class == NYIR_SCALAR_I64_TAGGED ||
        producer_class == NYIR_SCALAR_I64_TAGGED ||
        (final_arg_class == NYIR_SCALAR_UNKNOWN && arg_expr &&
         ((arg_expr->semantic.resolved &&
           arg_expr->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC) ||
          nyr_expr_is_canonical_dynamic_value(b, arg_expr)));
    if (is_c_call && formal_class == NYIR_SCALAR_POINTER &&
        arg_is_tagged_dynamic) {
      /*
       * `any` stores native pointers in the tagged-word encoding. Native Ny
       * calls decode that carrier before entering a pointer formal; C calls
       * must do the same before the raw pointer crosses the external ABI.
       * Preserving the tagged bits here passes an invalid, usually odd,
       * address to the C callee.
       */
      arg = nyr_emit_runtime_call(b, "rt_untag", arg, -1, -1, 1,
                                  NYIR_SCALAR_POINTER);
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_POINTER;
      final_arg_class = NYIR_SCALAR_POINTER;
      arg_is_tagged_dynamic = false;
    } else if (formal_class == NYIR_SCALAR_I64_TAGGED &&
               producer_class == NYIR_SCALAR_I64_TAGGED &&
               final_arg_class == NYIR_SCALAR_UNKNOWN &&
               !raw_integer_argument) {
      /* Earlier dynamic-list expansion can already box this raw scalar while
       * the producer map still reports unknown. Only that ambiguous case may
       * preserve the bits. A known raw integer is not already boxed and must
       * take the conversion below; treating every non-tagged class as an
       * expanded value let raw arguments cross `any` formals unboxed (for
       * example `_str_slice` forwarding `stop` to `core.slice`). */
      int tagged = nyr_ir_emit(
          &b->nyir, (nyr_inst_t){.op = NYIR_COPY,
                                  .dst = -1,
                                  .a = arg,
                                  .b = -1,
                                  .scalar_class = NYIR_SCALAR_I64_TAGGED});
      if (tagged < 0) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      arg = tagged;
    } else if (formal_class == NYIR_SCALAR_I64_TAGGED &&
               (final_arg_class == NYIR_SCALAR_I64_RAW ||
                (final_arg_class == NYIR_SCALAR_UNKNOWN &&
                 raw_integer_argument) ||
                (final_arg_class == NYIR_SCALAR_I64_TAGGED &&
                 raw_integer_argument && arg_expr &&
                 b->last_arith_expr == arg_expr && b->last_arith_raw) ||
                (final_arg_class == NYIR_SCALAR_I64_TAGGED &&
                 explicit_raw_ternary))) {
      /* An arithmetic emitter can be lowered in a dynamic call context, so
       * its temporary may inherit TAGGED even though the concrete producer
       * is a raw integer instruction. The emitter's expression-scoped ABI
       * result is authoritative here; stamp a raw copy before the one box. */
      if (final_arg_class == NYIR_SCALAR_I64_TAGGED && raw_integer_argument &&
          arg_expr && b->last_arith_expr == arg_expr && b->last_arith_raw) {
        arg = nyr_ir_emit(&b->nyir,
                        (nyr_inst_t){.op = NYIR_COPY,
                                      .dst = -1,
                                      .a = arg,
                                      .b = -1,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
        if (arg < 0)
          return -1;
      }
      if (arg_expr && nyr_expr_is_bool(b, arg_expr))
        arg = nyr_box_bool(b, arg);
      else
        arg = nyr_emit_runtime_call(b, "rt_box_i64", arg, -1, -1, 1,
                                    NYIR_SCALAR_I64_TAGGED);
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_TAGGED;
    } else if (formal_class == NYIR_SCALAR_STRING && arg_is_tagged_dynamic) {
      arg = nyr_emit_runtime_call(b, "rt_any_to_cstr", arg, -1, -1, 1,
                                  NYIR_SCALAR_STRING);
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_STRING;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu source=tagged_dynamic formal=str "
            "action=decode-cstr",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol
                ? runtime_symbol
                : (callee_fn && callee_fn->as.fn.name ? callee_fn->as.fn.name
                                                      : "<indirect>"),
            i + 1);
    } else if (formal_class == NYIR_SCALAR_I64_RAW && arg_is_tagged_dynamic) {
      /* A tagged dynamic value passed to a raw integer formal must be
       * decoded at this boundary. Do not let the callee perform integer
       * arithmetic on the tagged encoding or silently relabel the word. */
      arg = nyr_emit_runtime_call(b, "rt_any_to_i64", arg, -1, -1, 1,
                                  NYIR_SCALAR_I64_RAW);
      if (arg < 0)
        return -1;
      producer_class = NYIR_SCALAR_I64_RAW;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu action=decode-tagged-to-raw-i64 "
            "reason=formal-contract",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol ? runtime_symbol : "native", i + 1);
    } else if (formal_class == NYIR_SCALAR_AGGREGATE &&
               ((arg_static_type && nyr_rep_for_type(b, arg_static_type) ==
                                        NY_SEM_REP_AGGREGATE) ||
                (arg_local && arg_local->type_name &&
                 nyr_rep_for_type(b, arg_local->type_name) ==
                     NY_SEM_REP_AGGREGATE) ||
                (arg_expr && arg_expr->semantic.resolved &&
                 arg_expr->semantic.rep == NY_SEM_REP_AGGREGATE)) &&
               (final_arg_class == NYIR_SCALAR_I64_RAW ||
                final_arg_class == NYIR_SCALAR_I64_TAGGED)) {
      /* Generic Result unwraps can expose a nominal aggregate payload through
       * the raw one-word helper ABI. When both the source type and formal
       * agree that this word is an aggregate handle, carry that class across
       * the call boundary without changing its bits. */
      arg = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_COPY,
                                              .dst = -1,
                                              .a = arg,
                                              .b = -1,
                                              .scalar_class = formal_class});
      if (arg < 0)
        return -1;
      producer_class = formal_class;
    } else if ((formal_class == NYIR_SCALAR_POINTER ||
                formal_class == NYIR_SCALAR_OBJECT ||
                formal_class == NYIR_SCALAR_TYPED_BUFFER ||
                formal_class == NYIR_SCALAR_CLOSURE) &&
               arg_is_tagged_dynamic) {
      /* Dynamic handles and their concrete native formals share one word.
       * Preserve its bits: integer unboxing can corrupt odd pointer values.
       */
      arg = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_COPY,
                                              .dst = -1,
                                              .a = arg,
                                              .b = -1,
                                              .scalar_class = formal_class});
      if (arg < 0)
        return -1;
      producer_class = formal_class;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s call=%s arg=%zu "
            "action=preserve-canonical-dynamic-handle source=%s formal=%s "
            "reason=explicit-handle-class-boundary",
            b->current_fn_name ? b->current_fn_name : "<entry>",
            runtime_symbol
                ? runtime_symbol
                : (callee_fn && callee_fn->as.fn.name ? callee_fn->as.fn.name
                                                      : "<indirect>"),
            i + 1, nyr_scalar_class_name(tagged_source_class),
            nyr_scalar_class_name(formal_class));
    }
    nyr_call_arg_class_set(c_arg_scalar_classes,
                            lowered_argc + (has_sret ? 1u : 0u), formal_class);
    const nyr_inst_t *arg_value_def = nyr_reg_def_inst(b, arg);
    uint8_t arg_value_ownership =
        arg_value_def && arg_value_def->semantic_ownership != NY_SEM_OWN_UNKNOWN
            ? arg_value_def->semantic_ownership
        : arg_expr && arg_expr->semantic.resolved
            ? (uint8_t)arg_expr->semantic.ownership
            : NY_SEM_OWN_UNKNOWN;
    if (arg_value_ownership <= NY_SEM_OWN_SHARED)
      nyr_call_arg_ownership_set_packed(c_arg_ownership,
                                         lowered_argc + (has_sret ? 1u : 0u),
                                         arg_value_ownership);
    if (callee_fn && i < callee_fn->as.fn.params.len) {
      const char *formal_name = callee_fn->as.fn.params.data[i].name;
      uint8_t formal_contract = NYIR_ARG_CONTRACT_UNKNOWN;
      bool formal_borrows = false;
      bool formal_consumes = false;
      if (formal_name) {
        for (size_t k = 0; k < callee_fn->as.fn.attr_borrows.len; ++k)
          if (callee_fn->as.fn.attr_borrows.data[k] &&
              strcmp(callee_fn->as.fn.attr_borrows.data[k], formal_name) == 0)
            formal_borrows = true;
        for (size_t k = 0; k < callee_fn->as.fn.attr_consumes.len; ++k)
          if (callee_fn->as.fn.attr_consumes.data[k] &&
              strcmp(callee_fn->as.fn.attr_consumes.data[k], formal_name) == 0)
            formal_consumes = true;
        if (callee_fn->as.fn.attr_returns_borrow &&
            strcmp(callee_fn->as.fn.attr_returns_borrow, formal_name) == 0)
          formal_contract = NYIR_ARG_CONTRACT_RETURNS_BORROW;
        else if (formal_consumes)
          formal_contract = NYIR_ARG_CONTRACT_CONSUMES;
        else if (formal_borrows)
          formal_contract = NYIR_ARG_CONTRACT_BORROWS;
      }
      nyr_call_arg_contract_set_packed(c_arg_contracts,
                                        lowered_argc + (has_sret ? 1u : 0u),
                                        formal_contract);
    } else if (value_abi && i < value_abi->arity) {
      /* Intrinsic formal ownership comes from its canonical ABI declaration.
       * Keep this independent from the source expression's actual ownership.
       */
      uint8_t formal_contract =
          ny_native_runtime_value_formal_contract(value_abi, i);
      nyr_call_arg_contract_set_packed(c_arg_contracts,
                                        lowered_argc + (has_sret ? 1u : 0u),
                                        formal_contract);
    } else if (runtime_arg_abi) {
      /* General runtime intrinsics carry explicit per-source-argument
       * ownership contracts alongside their representation policies. */
      uint8_t formal_contract =
          ny_native_runtime_arg_formal_contract(runtime_arg_abi, i);
      nyr_call_arg_contract_set_packed(c_arg_contracts,
                                        lowered_argc + (has_sret ? 1u : 0u),
                                        formal_contract);
    }
    args[lowered_argc++] = arg;
    if (raw_native_dict_set || is_c_call)
      continue;
    bool callee_is_any =
        dynamic_sequence || ny_native_type_name_is_any(param_type) ||
        (!param_type && (param_rep == NY_SEM_REP_TAGGED_DYNAMIC ||
                         param_rep == NY_SEM_REP_UNRESOLVED)) ||
        untyped_dynamic_scalar;
    if (ny_native_type_name_is_int(param_type) ||
        param_rep == NY_SEM_REP_RAW_INT)
      callee_is_any = false;
    if (callee_fn && !try_dynamic_parameter && !param_type &&
        nyr_untyped_param_is_raw(callee_fn, callee_params->data[i].name))
      callee_is_any = false;
    /* The final parameter of a variadic function is represented as a native
     * sequence by the callee. Its absent source type must not fall through
     * the generic `any` ABI, which would append a third tag slot and shift
     * every following register. */
    if (variadic_call && i == variadic_index)
      callee_is_any = false;
    bool alias_raw_param =
        callee_fn && !try_dynamic_parameter && !param_type &&
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name && callee_fn->as.fn.name &&
        strcmp(e->as.call.callee->as.ident.name, callee_fn->as.fn.name) != 0 &&
        nyr_untyped_param_is_raw(callee_fn, callee_params->data[i].name);
    if (alias_raw_param)
      callee_is_any = false;
    if (callee_is_any && lowered_argc > 0) {
      nyr_call_arg_class_set(c_arg_scalar_classes,
                              lowered_argc - 1 + (has_sret ? 1u : 0u),
                              NYIR_SCALAR_I64_TAGGED);
    }
    bool callee_is_list =
        !callee_is_any && !dynamic_sequence &&
        (ny_native_type_name_is_list(param_type) ||
         (!param_type && inferred_list) ||
         (!param_type && param_rep == NY_SEM_REP_TYPED_BUFFER) ||
         (variadic_call && i == variadic_index));
    if (callee_is_list) {
      int length = nyr_take_list_len_fact(b, arg);
      if (length < 0)
        length = nyr_emit_runtime_call(b, "rt_tbuf_len_raw", arg, -1, -1, 1, 0);
      if (length < 0) {
        return nyr_fail(b,
                        "native NYIR lower: list argument %zu to "
                        "'%s' length query failed",
                        i + 1, name),
               -1;
      }
      if (lowered_argc >= NYIR_CALL_MAX_ARGS) {
        return nyr_fail(b,
                        "native NYIR lower: expanded call exceeds the "
                        "maximum supported argument count (%d)",
                        NYIR_CALL_MAX_ARGS),
               -1;
      }
      nyr_call_arg_class_set(c_arg_scalar_classes,
                              lowered_argc + (has_sret ? 1u : 0u),
                              NYIR_SCALAR_I64_RAW);
      args[lowered_argc++] = length;
    } else if ((callee_is_any || param_rep == NY_SEM_REP_STRING ||
                ny_native_type_name_is_str(param_type)) &&
               /* A declared `bytes` formal is a single-slot handle in the
                * callee ABI record (nyr_param_abi counts extra
                * slots from type names only): its STRING semantic rep must
                * not append len/tag words the signature never allocated.
                * The handle is self-contained; the body reads length through
                * the bytes runtime bridges. */
               !ny_native_type_name_is_bytes(param_type) &&
               !ny_native_runtime_arg_abi_single_slot(runtime_arg_abi, i)) {
      const nyr_local_t *expanded_arg_local =
          arg_expr && arg_expr->kind == NY_E_IDENT && arg_expr->as.ident.name
              ? nyr_find_local_nearest(b, arg_expr->as.ident.name)
              : NULL;
      bool source_string_local =
          expanded_arg_local &&
          (expanded_arg_local->is_cstr ||
           ny_native_type_name_is_str(expanded_arg_local->type_name));
      if (source_string_local &&
          nyr_value_scalar_class(b, arg) == NYIR_SCALAR_I64_TAGGED) {
        int decoded = nyr_emit_runtime_call(b, "rt_any_to_cstr", arg, -1, -1, 1,
                                            NYIR_SCALAR_STRING);
        if (decoded < 0)
          return -1;
        arg = decoded;
        args[lowered_argc - 1] = decoded;
        producer_class = NYIR_SCALAR_STRING;
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "native-abi",
              "function=%s call=%s arg=%zu source=tagged-string local=%s "
              "action=decode-before-expanded-string-abi",
              b->current_fn_name ? b->current_fn_name : "<entry>",
              runtime_symbol
                  ? runtime_symbol
                  : (callee_fn && callee_fn->as.fn.name ? callee_fn->as.fn.name
                                                        : "<indirect>"),
              i + 1,
              expanded_arg_local->name ? expanded_arg_local->name
                                       : "<unnamed>");
      }
      /* Keep the scalar raw until the final formal-class reconciliation
       * below. Boxing before expanding an any argument duplicated that
       * conversion when the expanded value lane was normalized a second time.
       */
      int length = nyr_take_dyn_fact(b, arg, NYR_FACT_DYN_STR_LEN);
      int tag = nyr_take_dyn_fact(b, arg, NYR_FACT_DYN_TAG);
      /* The argument's BigInt ABI proof already preserved its heap handle
       * above. Numeric objects have no sequence length and a fixed runtime
       * tag, so expanded any formals need no runtime metadata probes. */
      if (direct_bigint_argument) {
        if (length < 0)
          length = nyr_emit_const(b, 0);
        if (tag < 0)
          tag = nyr_emit_const(b, TAG_BIGINT);
      }
      if (tag < 0 && arg_expr && arg_expr->kind == NY_E_IDENT &&
          arg_expr->as.ident.name) {
        nyr_local_t *source_local = nyr_find_local(b, arg_expr->as.ident.name);
        if (source_local && source_local->dyn_tag_slot >= 0)
          tag = nyr_load_local_value(b, source_local->dyn_tag_slot);
      }
      if (length < 0) {
        bool arg_local_canonical =
            arg_expr && arg_expr->kind == NY_E_IDENT &&
            arg_expr->as.ident.name &&
            nyr_find_local(b, arg_expr->as.ident.name) &&
            nyr_find_local(b, arg_expr->as.ident.name)->raw_dynamic_index;
        if (nyr_expr_is_cstr(b, arg_expr))
          length = nyr_emit_runtime_call(b, "rt_cstr_len", arg, -1, -1, 1, 0);
        else if (nyr_expr_is_list(b, arg_expr) &&
                 !(name && (strcmp(name, "set.sub") == 0 ||
                            strcmp(name, "core.set_mod.sub") == 0)))
          length =
              nyr_emit_runtime_call(b, "rt_tbuf_len_raw", arg, -1, -1, 1, 0);
        else if ((nyr_expr_is_any(b, arg_expr) || arg_local_canonical) &&
                 !raw_integer_argument &&
                 !(arg_expr && arg_expr->semantic.resolved &&
                   arg_expr->semantic.rep == NY_SEM_REP_RAW_INT &&
                   !arg_local_canonical))
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
          length = nyr_emit_runtime_call(b, "rt_len", arg, -1, -1, 1, 0);
        else if (!(arg_expr && arg_expr->kind == NY_E_LITERAL &&
                   arg_expr->as.literal.kind == NY_LIT_INT) &&
                 !raw_integer_argument &&
                 !(arg_expr && arg_expr->semantic.resolved &&
                   arg_expr->semantic.rep == NY_SEM_REP_RAW_INT) &&
                 !nyr_expr_is_ptr(b, arg_expr))
          length = nyr_emit_runtime_call(b, "rt_len", arg, -1, -1, 1, 0);
        else
          length = nyr_emit_const(b, 0);
      }
      if (tag < 0) {
        if (ny_expr_is_nil_literal(arg_expr)) {
          tag = nyr_emit_const(b, 0);
        } else if (nyr_expr_is_cstr(b, arg_expr)) {
          tag = nyr_emit_const(b, 121);
        } else if (arg_expr_f64 || arg_expr_f32) {
          tag = nyr_emit_const(b, TAG_FLOAT);
        } else if (arg_expr && arg_expr->kind == NY_E_LITERAL &&
                   arg_expr->as.literal.kind == NY_LIT_BOOL) {
          tag = nyr_emit_const(b, arg_expr->as.literal.as.b ? NY_IMM_TRUE
                                                            : NY_IMM_FALSE);
        } else if (arg_expr && arg_expr->kind == NY_E_LITERAL &&
                   arg_expr->as.literal.kind == NY_LIT_INT) {
          tag = nyr_emit_const(b, 3);
        } else if (raw_integer_argument) {
          tag = nyr_emit_const(b, 3);
        } else if (nyr_expr_is_list(b, arg_expr)) {
          tag = nyr_emit_const(b, TAG_LIST);
        } else if (raw_dynamic_read) {
          /* The payload is already unboxed.  rt_value_tag would interpret an
           * odd raw integer as a tagged integer and make the callee unbox it
           * a second time; raw_word_tag distinguishes that from real object
           * handles without guessing from the low bit. */
          tag = nyr_emit_runtime_call(b, "rt_raw_word_tag", arg, -1, -1, 1, 0);
        } else {
          tag = nyr_emit_runtime_call(b, "rt_value_tag", arg, -1, -1, 1, 0);
        }
      }
      if (lowered_argc + 1 >= NYIR_CALL_MAX_ARGS) {
        return nyr_fail(b,
                        "native NYIR lower: expanded call exceeds the "
                        "maximum supported argument count (%d)",
                        NYIR_CALL_MAX_ARGS),
               -1;
      }
      nyr_call_arg_class_set(c_arg_scalar_classes,
                              lowered_argc + (has_sret ? 1u : 0u),
                              NYIR_SCALAR_I64_RAW);
      args[lowered_argc++] = length;
      nyr_call_arg_class_set(c_arg_scalar_classes,
                              lowered_argc + (has_sret ? 1u : 0u),
                              NYIR_SCALAR_I64_RAW);
      args[lowered_argc++] = tag;
    }
  }
  if (callee_fn && nyr_fn_has_thread_attr(callee_fn)) {
    if (lowered_argc > 15) {
      const char *source_file =
          e->tok.filename ? e->tok.filename
                          : (b->source_file ? b->source_file : "<unknown>");
      return nyr_fail(
                 b, "%s:%d:%d: @thread call '%s' supports up to 15 arguments",
                 source_file, e->tok.real_line ? e->tok.real_line : e->tok.line,
                 e->tok.col,
                 callee_fn->as.fn.name ? callee_fn->as.fn.name : name),
             -1;
    }
    if (ny_native_type_name_is_f64(callee_fn->as.fn.return_type) ||
        ny_native_type_name_is_f32(callee_fn->as.fn.return_type) ||
        (callee_fn->as.fn.return_semantic.resolved &&
         (callee_fn->as.fn.return_semantic.rep == NY_SEM_REP_F64 ||
          callee_fn->as.fn.return_semantic.rep == NY_SEM_REP_F32))) {
      return nyr_fail(b, "native NYIR lower: @thread functions must return "
                         "an i64-compatible value"),
             -1;
    }
    char callback_name[512];
    const char *fn_name = callee_fn->as.fn.name;
    int callback_len = snprintf(callback_name, sizeof(callback_name),
                                "ny_fn_%s", fn_name ? fn_name : name);
    if (callback_len < 0 || (size_t)callback_len >= sizeof(callback_name)) {
      return nyr_fail(b,
                      "native NYIR lower: @thread callback symbol is too long"),
             -1;
    }
    /* This is a code symbol, not a pooled C string.  Keep a stable copy for
     * the NYIR/object lifetime; strtab_intern would turn it into .Lnystr.N
     * and the trampoline would jump into string data. */
    char *callback_symbol = malloc((size_t)callback_len + 1);
    if (!callback_symbol) {
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    memcpy(callback_symbol, callback_name, (size_t)callback_len + 1);
    int callback =
        nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                          .dst = -1,
                                          .a = -1,
                                          .b = -1,
                                          .symbol = callback_symbol});
    if (callback < 0) {
      free(callback_symbol);
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    /* Keep the count and callback representation in separate ABI values.
     * rt_thread_prepare_call_args validates argc as a raw scalar count (the
     * list-form thread_spawn_call lowering below also emits it raw); a tagged
     * count reached the runtime doubled and every call with eight or more
     * arguments was rejected, so the join crashed on the -1 handle. */
    int argc_value = nyr_emit_const(b, (int64_t)lowered_argc);
    int raw_fn = nyr_emit_const(b, rt_tag_v(1));
    int tagged_result =
        nyr_emit_const(b, rt_tag_v(nyr_fn_result_kind(b, callee_fn)));
    int argv = nyr_emit_const(b, 0);
    if (lowered_argc > 0) {
      argv =
          nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ALLOCA,
                                            .dst = -1,
                                            .a = -1,
                                            .b = -1,
                                            .c = -1,
                                            .imm = (int64_t)lowered_argc * 8});
      if (argv >= 0) {
        for (size_t i = 0; i < lowered_argc; ++i) {
          int address = argv;
          if (i > 0) {
            int offset = nyr_emit_const(b, (int64_t)i * 8);
            address = offset < 0 ? -1 : nyr_emit_add_i64(b, argv, offset);
          }
          if (address < 0 || !nyr_emit_store_i64(b, address, args[i])) {
            argv = -1;
            break;
          }
        }
      }
    }
    if (argc_value < 0 || raw_fn < 0 || tagged_result < 0 || argv < 0)
      return -1;
    if (b->thread_detach_stmt_call) {
      if (nyr_emit_runtime_call4(b, "rt_thread_launch_call_abi", callback,
                                 argc_value, argv, raw_fn, tagged_result, 5,
                                 0) < 0)
        return -1;
      return nyr_emit_const(b, 0);
    }
    int handle =
        nyr_emit_runtime_call4(b, "rt_thread_spawn_call_abi", callback,
                               argc_value, argv, raw_fn, tagged_result, 5, 0);
    if (handle < 0)
      return -1;
    int joined =
        nyr_emit_runtime_call(b, "rt_thread_join", handle, -1, -1, 1, 0);
    if (joined < 0)
      return -1;
    /*
     * The join returns the value per the CALLEE's result kind (tagged when
     * the body's return fact is dynamic), while HM can stamp this call site
     * RAW_INT from the specialized argument types.  Restore the caller's
     * representation exactly once, matching the other call boundaries.
     */
    int result_kind = nyr_fn_result_kind(b, callee_fn);
    if (!nyr_expr_is_bool(b, e) &&
        (result_kind == 0 ||
         (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_RAW_INT &&
          result_kind == 1))) {
      int raw = nyr_emit_runtime_call(b, "rt_any_to_i64", joined, -1, -1, 1, 0);
      if (raw < 0)
        return -1;
      joined = raw;
    }
    return joined;
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_INTEGER &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_SSE &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F32 &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HFA_F64 &&
      ext->ret_aggregate_classes[0] != NY_SYSV_AGG_HVA_V128) {
    return nyr_fail(b, "native NYIR lower: aggregate return class is not "
                       "represented for the selected ABI"),
           -1;
  }
  if (ext) {
    for (unsigned i = 0; i < ext->param_count && i < e->as.call.args.len; ++i) {
      if (ext->arg_aggregate_sizes[i] > 0 &&
          NYIR_ARG_AGG_SIZE(ext->arg_aggregate_sizes[i]) <= 16 &&
          (NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
               NY_SYSV_AGG_UNSUPPORTED ||
           NYIR_ARG_AGG_CLASS(ext->arg_aggregate_sizes[i], 0) ==
               NY_SYSV_AGG_NONE)) {
        return nyr_fail(b, "native NYIR lower: register aggregate argument "
                           "is not represented for the selected ABI"),
               -1;
      }
    }
  }
  int aggregate_ret_ptr = -1;
  if (has_aggregate_return) {
    /* Aggregate results escape the call and are owned by the caller.  Stack
     * alloca storage made `free(result)` corrupt the allocator; use the same
     * heap ownership contract as ordinary C aggregate wrappers. */
    if ((uint64_t)ext->ret_aggregate_size > (uint64_t)INT64_MAX) {
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    int aggregate_size = nyr_emit_const(b, (int64_t)ext->ret_aggregate_size);
    aggregate_ret_ptr =
        aggregate_size < 0
            ? -1
            : nyr_emit_runtime_call(b, "rt_zalloc_raw", aggregate_size, -1, -1,
                                    1, 0);
    if (aggregate_ret_ptr < 0) {
      return nyr_fail(b,
                      "native NYIR lower: aggregate return allocation failed"),
             -1;
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
      if (has_sret && e->as.call.args.len == SIZE_MAX) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      size_t total_args = e->as.call.args.len + (has_sret ? 1u : 0u);
      if (total_args > SIZE_MAX / sizeof(*arg_sizes)) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      if (total_args > 0) {
        arg_sizes = (uint32_t *)calloc(total_args, sizeof(*arg_sizes));
        if (!arg_sizes) {
          return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
        }
      }
      for (unsigned i = 0; i < e->as.call.args.len && i < ext->param_count;
           ++i) {
        arg_sizes[i + (has_sret ? 1 : 0)] = ext->arg_aggregate_sizes[i];
      }
    }
  }

  size_t original_argc = lowered_argc;
  if (has_sret && original_argc == SIZE_MAX) {
    free(arg_sizes);
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  }
  size_t argc = original_argc + (has_sret ? 1u : 0u);
  if (has_sret) {
    if (argc > NYIR_CALL_MAX_ARGS) {
      free(arg_sizes);
      return nyr_fail(b,
                      "native NYIR lower: call exceeds maximum args with sret"),
             -1;
    }
    for (int i = (int)original_argc - 1; i >= 0; --i) {
      args[i + 1] = args[i];
    }
    args[0] = aggregate_ret_ptr;
    nyr_call_arg_class_set(c_arg_scalar_classes, 0, NYIR_SCALAR_POINTER);
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
  /* Core declarations are compiled into the shared bitcode with their
   * module path prepended to the source declaration name.  Keep the call
   * reference under that same name so cache linking retains the definition
   * instead of pruning it as unused. */
  if (!ext && !runtime_symbol && !decl_symbol && callee_fn &&
      callee_fn->kind == NY_S_FUNC && ny_is_stdlib_tok(callee_fn->tok) &&
      callee_fn->as.fn.name) {
    const char *module =
        ny_native_fn_module(ny_native_fn_index_for(b), b->prog, callee_fn);
    const char *fn_name = callee_fn->as.fn.name;
    size_t module_len = module ? strlen(module) : 0;
    if (module && *module &&
        !(strncmp(fn_name, module, module_len) == 0 &&
          fn_name[module_len] == '.')) {
      char qualified[1024];
      int qn = snprintf(qualified, sizeof(qualified), "%s.%s", module, fn_name);
      if (qn > 0 && (size_t)qn < sizeof(qualified))
        symbol = nyr_builder_own_string(b, qualified);
    }
  }
  /* Functions nested in a Ny module are emitted under the module-qualified
   * native name. A named import can retain the leaf spelling in `symbol`
   * even though semantic resolution selected the module-local definition;
   * emit the same qualified symbol as the definition so re-exported calls
   * do not bind to an unresolved leaf. */
  if (!ext && !runtime_symbol && callee_fn && callee_fn->kind == NY_S_FUNC &&
      !ny_is_stdlib_tok(callee_fn->tok) && callee_fn->as.fn.name &&
      e->semantic.canonical_callee) {
    const char *module =
        ny_native_fn_module(ny_native_fn_index_for(b), b->prog, callee_fn);
    if (module && *module) {
      char qualified[1024];
      int qn = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                        callee_fn->as.fn.name);
      if (qn > 0 && (size_t)qn < sizeof(qualified) &&
          strcmp(qualified, e->semantic.canonical_callee) == 0 &&
          (strcmp(symbol, callee_fn->as.fn.name) == 0 ||
           strcmp(symbol, e->semantic.canonical_callee) == 0)) {
        char native_name[1056];
        int nn =
            snprintf(native_name, sizeof(native_name), "ny_fn_%s", qualified);
        if (nn > 0 && (size_t)nn < sizeof(native_name))
          symbol = nyr_builder_own_string(b, native_name);
      }
    }
  }
  /* Module artifacts qualify definition names. Calls must use that same
   * declaration identity, including the owner segment of attached methods. */
  if (!ext && !runtime_symbol && !decl_symbol && callee_fn && b->options &&
      b->options->emit_module && *b->options->emit_module)
    symbol = nyr_builder_own_string(
        b, ny_native_emitted_fn_name(ny_native_fn_index_for(b), b->prog,
                                     b->options, callee_fn));
  /* Trace return intrinsics have a tagged interpreter entry and a raw native
   * scalar ABI. A generated declaration's link_name otherwise wins above
   * the runtime alias and calls the tagged helper with a raw NYIR argument.
   */
  if (leaf && strcmp(leaf, "__trace_ret_void") == 0)
    symbol = "rt_trace_ret_void_raw";
  else if (leaf && strcmp(leaf, "__trace_ret_i64") == 0)
    symbol = "rt_trace_ret_i64_raw";
  else if (leaf && strcmp(leaf, "__trace_ret_u64") == 0)
    symbol = "rt_trace_ret_u64_raw";
  /*
   * The overload-dispatch record: which declaration (or runtime intrinsic)
   * this call site actually binds to, after semantic canonicalization,
   * module qualification, and runtime-symbol aliasing all had their say.
   * This is the heuristic the checker's "operator/overload" decisions
   * funnel into; everything downstream inherits this ABI.
   */
  if (ny_trace_enabled("NY_TRACE_OPRESOLVE") && symbol && name &&
      strcmp(symbol, name) != 0)
    ny_trace_line("CALL_BIND", "%s: %s -> %s%s",
                  b->current_fn_name ? b->current_fn_name : "?", name, symbol,
                  callee_fn && callee_fn->as.fn.name &&
                          strcmp(callee_fn->as.fn.name, symbol) == 0
                      ? ""
                      : " (intrinsic)");
  /* Dynamic argument expansion can replace the source value lane after its
   * initial formal reconciliation. Normalize the final physical argument
   * array once, immediately before call emission, so a raw load cannot reach
   * a tagged formal through a later expansion or local-slot rewrite. */
  for (size_t i = 0; i < argc; ++i) {
    nyr_scalar_class_t formal =
        (nyr_scalar_class_t)((c_arg_scalar_classes[i / 16] >> ((i % 16) * 4)) &
                              0xfu);
    if (args[i] < 0)
      continue;
    nyr_scalar_class_t actual = nyr_value_scalar_class(b, args[i]);
    if (formal == NYIR_SCALAR_I64_TAGGED && actual == NYIR_SCALAR_I64_RAW) {
      int tagged = nyr_emit_runtime_call(b, "rt_box_i64", args[i], -1, -1, 1,
                                         NYIR_SCALAR_I64_TAGGED);
      if (tagged < 0)
        return -1;
      args[i] = tagged;
    } else if (formal == NYIR_SCALAR_I64_RAW &&
               actual == NYIR_SCALAR_I64_TAGGED) {
      int raw = nyr_emit_runtime_call(b, "rt_any_to_i64", args[i], -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      if (raw < 0)
        return -1;
      args[i] = raw;
    }
  }
  if (ny_trace_enabled("NY_TRACE_ABI")) {
    for (size_t i = 0; i < argc; ++i) {
      nyr_scalar_class_t actual = nyr_value_scalar_class(b, args[i]);
      nyr_scalar_class_t formal =
          (nyr_scalar_class_t)((c_arg_scalar_classes[i / 16] >>
                                 ((i % 16) * 4)) &
                                0xfu);
      ny_trace_line(
          "ABI",
          "[abi-expanded] %s call=%s slot=%zu value=v%d actual=%s formal=%s",
          b->current_fn_name ? b->current_fn_name : "?",
          symbol ? symbol : "<indirect>", i, args[i],
          nyr_scalar_class_name(actual), nyr_scalar_class_name(formal));
    }
  }
  if (!ext && builtin_c_call) {
    switch (builtin_kind) {
    /* Native allocations receive raw i64 byte counts.  `rt_malloc` accepts
     * the interpreter's tagged count ABI, which halves every odd count and
     * corrupts the heap on writes past that truncated allocation. */
    case NY_BUILTIN_ALLOC_MALLOC:
      symbol = "rt_malloc_i64";
      break;
    case NY_BUILTIN_ALLOC_ZALLOC:
      symbol = "rt_zalloc_raw";
      break;
    case NY_BUILTIN_ALLOC_CALLOC:
      symbol = "rt_malloc_i64";
      break;
    case NY_BUILTIN_ALLOC_REALLOC:
      symbol = "rt_realloc_raw";
      break;
    case NY_BUILTIN_ALLOC_FREE:
      symbol = "rt_free";
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
  /* Stamp a call result from the selected declaration's resolved ABI class.
   * A call expression may have no semantic fact in the native-only pipeline
   * (or may retain a contextual fact from its use site); leaving that result
   * UNKNOWN makes immediate member chains treat a nominal object returned by
   * `self` as a raw scalar. */
  nyr_scalar_class_t result_class = NYIR_SCALAR_UNKNOWN;
  /*
   * A `self` return hands back the receiver: an object/container handle for
   * every impl carrier in this codebase (vec2/3/4 vecdicts, user nominal
   * types).  HM can resolve the method's `self` result rep to F64 when the
   * body flows through float helpers, and stamping that rep here classifies
   * the returned dict handle as a raw f64 register — the next member chain
   * then trips the call scalar-class verifier.  The receiver's own register
   * class stays authoritative for self returns.
   */
  bool self_return = return_type && (strcmp(return_type, "self") == 0 ||
                                     strcmp(return_type, "?self") == 0);
  if (self_return) {
    result_class = NYIR_SCALAR_UNKNOWN;
  } else if (return_type &&
             nyr_rep_for_type(b, return_type) != NY_SEM_REP_UNRESOLVED) {
    /* An explicit return annotation is the function's ABI contract. Body
     * inference can be broader (for example a dynamic dictionary lookup in a
     * function declared to return `list`) or simply stale after
     * specialization; use it only for unannotated returns. Otherwise callers
     * inherit an aggregate/tagged class from the body and reject or
     * mis-handle the declared typed-buffer value. */
    result_class =
        ny_native_scalar_class_from_sem_rep(nyr_rep_for_type(b, return_type));
  } else if (callee_fn && callee_fn->as.fn.return_semantic.resolved) {
    ny_sem_rep_t semantic_rep = callee_fn->as.fn.return_semantic.rep;
    ny_sem_rep_t declared_rep = nyr_rep_for_type(b, return_type);
    bool semantic_is_float =
        semantic_rep == NY_SEM_REP_F32 || semantic_rep == NY_SEM_REP_F64;
    bool declared_is_float =
        declared_rep == NY_SEM_REP_F32 || declared_rep == NY_SEM_REP_F64;
    /* A concrete nominal/container return type wins over a stale float fact
     * inferred from the implementation body. For example vec3.divs returns
     * its declared vec3 handle even though its body uses f64 component math.
     */
    if (return_type && !declared_is_float && semantic_is_float)
      result_class = ny_native_scalar_class_from_sem_rep(declared_rep);
    else
      result_class = ny_native_scalar_class_from_sem_rep(semantic_rep);
  } else if (return_type) {
    result_class =
        ny_native_scalar_class_from_sem_rep(nyr_rep_for_type(b, return_type));
  } else if (e->semantic.resolved) {
    result_class = ny_native_scalar_class_from_sem_rep(e->semantic.rep);
  }
  /*
   * A resolved return fact coherent with the annotation owns the scalar
   * class even when the spelling classifies differently (aliases and other
   * spellings the name mapping misses).  Body-derived broader facts
   * (tagged/aggregate/object under a container or nominal spelling, or a
   * float fact under a concrete nominal return) stay subordinate to the
   * declared contract, matching the branch rules above.
   */
  bool have_coherent_return_semantic = false;
  if (!self_return && callee_fn && callee_fn->as.fn.return_semantic.resolved &&
      (!return_type ||
       ny_sem_rep_coherent_with_type(return_type,
                                     callee_fn->as.fn.return_semantic.rep))) {
    ny_sem_rep_t semantic_rep = callee_fn->as.fn.return_semantic.rep;
    nyr_scalar_class_t semantic_class =
        ny_native_scalar_class_from_sem_rep(semantic_rep);
    if (semantic_class != NYIR_SCALAR_UNKNOWN) {
      have_coherent_return_semantic = true;
      ny_sem_rep_t declared_rep = nyr_rep_for_type(b, return_type);
      bool semantic_is_float =
          semantic_rep == NY_SEM_REP_F32 || semantic_rep == NY_SEM_REP_F64;
      bool declared_is_float =
          declared_rep == NY_SEM_REP_F32 || declared_rep == NY_SEM_REP_F64;
      bool declared_beats_stale_float =
          return_type && !declared_is_float && semantic_is_float;
      if (!declared_beats_stale_float &&
          (semantic_rep == NY_SEM_REP_RAW_INT ||
           semantic_rep == NY_SEM_REP_F32 || semantic_rep == NY_SEM_REP_F64) &&
          semantic_class != result_class)
        result_class = semantic_class;
    }
  }
  /* Keep the expression's semantic carrier coherent with the same declared
   * return contract used to stamp its machine class. This matters for nominal
   * impl owners whose source spelling is an aggregate type name but whose
   * runtime value is an object handle. */
  if (!self_return && return_type && e->semantic.resolved) {
    ny_sem_rep_t return_rep = nyr_rep_for_type(b, return_type);
    if (return_rep != NY_SEM_REP_UNRESOLVED)
      ((expr_t *)e)->semantic.rep = return_rep;
  }
  /* Some standard constructors have a low-level `ptr` declaration even
   * though their language result is a typed buffer.  Preserve the resolved
   * expression ABI class at that representation boundary; stamping the
   * helper's generic pointer return caused valid list values to disagree
   * with typed-buffer formals in verifier-checked calls. */
  if (!self_return && e->semantic.resolved &&
      e->semantic.rep == NY_SEM_REP_TYPED_BUFFER && return_type &&
      ny_native_type_name_is_ptr(return_type))
    result_class = NYIR_SCALAR_TYPED_BUFFER;
  if (!self_return &&
      ((ext && ext->ret_f64) || ny_native_runtime_symbol_returns_f64(symbol) ||
       (!have_coherent_return_semantic &&
        ny_native_type_name_is_f64(return_type)))) {
    result_class = NYIR_SCALAR_F64;
  } else if (!self_return && ((ext && ext->ret_f32) ||
                              (!have_coherent_return_semantic &&
                               ny_native_type_name_is_f32(return_type)))) {
    result_class = NYIR_SCALAR_F32;
  } else if (!self_return && !have_coherent_return_semantic && !return_type &&
             nyr_expr_is_f64(b, e)) {
    result_class = NYIR_SCALAR_F64;
  }
  /* The runtime ABI row is authoritative for unresolved C-helper results.
   * Without this, bigint constructors and raw-int bridges returned UNKNOWN
   * from this direct-call path even though nyr_emit_runtime_call
   * already honors the same result descriptor. */
  /* A source-level intrinsic can bind to a runtime symbol without being
   * classified as a C call (for example __bigint_to_int ->
   * rt_bigint_to_i64_raw). Its runtime ABI row still defines the machine
   * result representation, so consult the row for any exact runtime symbol.
   */
  const ny_native_runtime_abi_decl_t *call_result_abi =
      symbol ? ny_native_runtime_abi_decl(symbol) : NULL;
  if (!self_return && call_result_abi) {
    /* An exact runtime ABI declaration describes the machine value returned
     * by the call.  Source-level intrinsic signatures can carry a stale
     * tagged semantic rep (notably __bigint_to_int), so prefer the runtime
     * contract for raw-int helpers even when semantic inference already set a
     * class.  Otherwise the consumer decodes a raw integer as a tagged value.
     */
    if (ny_native_runtime_abi_result_is_raw_int(call_result_abi))
      result_class = NYIR_SCALAR_I64_RAW;
    else if (result_class == NYIR_SCALAR_UNKNOWN &&
             (ny_native_runtime_abi_result_is_tagged_int(call_result_abi) ||
              ny_native_runtime_abi_result_is_tagged_value(call_result_abi) ||
              ny_native_runtime_abi_result_is_bigint(call_result_abi)))
      result_class = NYIR_SCALAR_I64_TAGGED;
    else if (result_class == NYIR_SCALAR_UNKNOWN &&
             ny_native_runtime_abi_result_is_cstr(call_result_abi))
      result_class = NYIR_SCALAR_STRING;
    else if (result_class == NYIR_SCALAR_UNKNOWN &&
             ny_native_runtime_abi_result_is_pointer(call_result_abi))
      result_class = NYIR_SCALAR_POINTER;
  }
  /* BigInt values use the same one-word tagged ABI for immediate integers
   * and heap handles. Inference can describe the nominal return as an
   * aggregate, but the declared bigint ABI is the authoritative carrier. */
  if (!self_return && return_type && ny_type_is(return_type, "bigint"))
    result_class = NYIR_SCALAR_I64_TAGGED;
  /* The C ABI returns signed i64 scalars raw even when the imported
   * declaration leaves the Ny result dynamic. Preserve that machine fact at
   * the call, then box only for the selected tagged-value contract. */
  bool box_signed_i64_return =
      ext && ext->ret_signed_i64 && result_class == NYIR_SCALAR_I64_TAGGED;
  if (box_signed_i64_return)
    result_class = NYIR_SCALAR_I64_RAW;
  if (has_aggregate_return && !has_sret) {
    if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ||
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F64)
      result_class = NYIR_SCALAR_F64;
    else if (ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HFA_F32)
      result_class = NYIR_SCALAR_F32;
  }
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event(
        "native-abi", "%s %s ret=%s rep=%d cls=%d self=%d",
        b->current_fn_name ? b->current_fn_name : "<entry>",
        symbol ? symbol : "<indirect>", return_type ? return_type : "<unknown>",
        callee_fn && callee_fn->as.fn.return_semantic.resolved
            ? (int)callee_fn->as.fn.return_semantic.rep
            : -1,
        (int)result_class, (int)self_return);
  if (ny_trace_enabled("NY_TRACE_ABI"))
    ny_trace_line(
        "ABI", "[abi-return] %s call=%s declared=%s source=%s class=%s",
        b->current_fn_name ? b->current_fn_name : "<entry>",
        symbol ? symbol : "<indirect>", return_type ? return_type : "<unknown>",
        callee_fn && callee_fn->as.fn.return_semantic.resolved
            ? ny_sem_rep_name(callee_fn->as.fn.return_semantic.rep)
            : "unresolved",
        nyr_scalar_class_name(result_class));
  /* calloc(nmemb, size) folds its two arguments into one byte count for
   * the single-argument rt_malloc_i64 bridge.  Without this, the VM
   * dispatch rejects the 2-word call ("expected 1 arguments, got 2"). */
  if (builtin_kind == NY_BUILTIN_ALLOC_CALLOC && argc == 2 && symbol &&
      strcmp(symbol, "rt_malloc_i64") == 0) {
    int product = nyr_push_val(b, NYIR_MUL_I64, args[0], args[1], 0, NULL);
    if (product < 0)
      return -1;
    args[0] = product;
    argc = 1;
  }
  int *extra = NULL;
  if (argc > 6) {
    size_t extra_len = argc - 6;
    if (extra_len > SIZE_MAX / sizeof(*extra)) {
      free(arg_sizes);
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    extra = (int *)malloc(extra_len * sizeof(*extra));
    if (!extra) {
      free(arg_sizes);
      return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
    }
    memcpy(extra, &args[6], extra_len * sizeof(*extra));
  }
  if (getenv("NYTRIX_DEBUG_CALLSYM") && symbol && strstr(symbol, "strip"))
    fprintf(
        stderr,
        "callsym-dbg: symbol=[%s] name=[%s] canonical=[%s] callee_fn=[%s]\n",
        symbol, name ? name : "(null)",
        e->semantic.canonical_callee ? e->semantic.canonical_callee : "(null)",
        callee_fn && callee_fn->as.fn.name ? callee_fn->as.fn.name : "(null)");
  int v = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_CALL,
          .dst = -1,
          .a = argc > 0 ? args[0] : -1,
          .b = argc > 1 ? args[1] : -1,
          .c = argc > 2 ? args[2] : -1,
          .d = argc > 3 ? args[3] : -1,
          .e = argc > 4 ? args[4] : -1,
          .f = argc > 5 ? args[5] : -1,
          .imm = (int64_t)argc,
          .flags =
              flags |
              (e->semantic.exclusive_borrow_proven ? NYIR_INST_F_NOALIAS : 0),
          .scalar_class = result_class,
          .semantic_ownership =
              e->semantic.exclusive_borrow_proven
                  ? NY_SEM_OWN_BORROWED
                  : (e->semantic.resolved ? (uint8_t)e->semantic.ownership
                                          : NY_SEM_OWN_UNKNOWN),
          .symbol = symbol,
          .extra_args = extra,
          .extra_args_len = argc > 6 ? argc - 6 : 0,
          .arg_sizes = arg_sizes,
          .ret_aggregate_size =
              has_aggregate_return && !has_sret ? ext->ret_aggregate_size : 0,
          .ret_aggregate_classes = {has_aggregate_return && !has_sret
                                        ? (uint8_t)ext->ret_aggregate_classes[0]
                                        : 0,
                                    has_aggregate_return && !has_sret
                                        ? (uint8_t)ext->ret_aggregate_classes[1]
                                        : 0}});
  if (v >= 0 && b->nyir.len > 0) {
    nyr_inst_t *call = &b->nyir.data[b->nyir.len - 1];
    memcpy(call->arg_scalar_classes, c_arg_scalar_classes,
           sizeof(call->arg_scalar_classes));
    memcpy(call->arg_ownership, c_arg_ownership, sizeof(call->arg_ownership));
    memcpy(call->arg_contracts, c_arg_contracts, sizeof(call->arg_contracts));
  }
  if (v < 0) {
    free(extra);
    free(arg_sizes);
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  }
  if (box_signed_i64_return) {
    v = nyr_emit_runtime_call(b, "rt_box_i64", v, -1, -1, 1,
                              NYIR_SCALAR_I64_TAGGED);
    if (v < 0) {
      free(extra);
      free(arg_sizes);
      return -1;
    }
  }
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
        else if (nyr_expr_is_cstr(b, source))
          static_tag = 121;
        else if (source &&
                 (nyr_expr_is_f64(b, source) || nyr_expr_is_f32(b, source)))
          static_tag = TAG_FLOAT;
        if (static_tag >= 0) {
          int tag = nyr_emit_const(b, static_tag);
          if (tag < 0 || !nyr_record_dyn_fact(b, v, NYR_FACT_DYN_TAG, tag))
            return -1;
        }
        break;
      }
    }
  }
  if (builtin_c_call &&
      !nyr_record_alloc_fact(b, v, nyr_literal_allocation_size(leaf, e))) {
    return -1;
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_HVA_V128) {
    unsigned elem_count = ext->ret_aggregate_size / 16u;
    if (elem_count < 1 || elem_count > 4 ||
        elem_count * 16u != ext->ret_aggregate_size) {
      return nyr_fail(b,
                      "native NYIR lower: invalid AAPCS64 HVA return layout"),
             -1;
    }
    for (unsigned i = 0; i < elem_count; ++i) {
      int captured = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .c = -1,
                                                       .imm = 10 + (int64_t)i});
      int off = i ? nyr_emit_const(b, (int64_t)i * 16) : -1;
      int addr =
          i ? nyr_emit_add_i64(b, aggregate_ret_ptr, off) : aggregate_ret_ptr;
      if (captured < 0 || addr < 0 ||
          nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_VEC4_STORE_I64,
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
      return nyr_fail(b,
                      "native NYIR lower: invalid AAPCS64 HFA return layout"),
             -1;
    }
    static const int f64_selectors[4] = {2, 3, 8, 9};
    int captured[4] = {-1, -1, -1, -1};
    for (unsigned i = 0; i < elem_count; ++i) {
      captured[i] =
          nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
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
        int off = i ? nyr_emit_const(b, (int64_t)i * 8) : -1;
        int addr =
            i ? nyr_emit_add_i64(b, aggregate_ret_ptr, off) : aggregate_ret_ptr;
        if (addr < 0 || !nyr_emit_store_i64(b, addr, captured[i]))
          return -1;
      }
    } else {
      int shift32 = nyr_emit_const(b, 32);
      if (shift32 < 0)
        return -1;
      for (unsigned pair = 0; pair * 2 < elem_count; ++pair) {
        unsigned lo_idx = pair * 2;
        int packed = captured[lo_idx];
        if (lo_idx + 1 < elem_count) {
          int shifted =
              nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                .dst = -1,
                                                .a = captured[lo_idx + 1],
                                                .b = shift32});
          packed = shifted < 0 ? -1
                               : nyr_ir_emit(&b->nyir,
                                           (nyr_inst_t){.op = NYIR_OR_I64,
                                                         .dst = -1,
                                                         .a = captured[lo_idx],
                                                         .b = shifted});
        }
        int off = pair ? nyr_emit_const(b, (int64_t)pair * 8) : -1;
        int addr = pair ? nyr_emit_add_i64(b, aggregate_ret_ptr, off)
                        : aggregate_ret_ptr;
        if (packed < 0 || addr < 0 || !nyr_emit_store_i64(b, addr, packed))
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
    second_ret = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .c = -1,
                                                   .imm = 1});
    if (second_ret < 0) {
      return nyr_fail(b, "native NYIR lower: secondary return "
                         "register capture failed"),
             -1;
    }
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE) {
    primary_ret = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                                    .dst = -1,
                                                    .a = -1,
                                                    .b = -1,
                                                    .c = -1,
                                                    .imm = 2});
    if (primary_ret < 0) {
      return nyr_fail(
                 b,
                 "native NYIR lower: primary return register capture failed"),
             -1;
    }
  }
  if (has_aggregate_return && !has_sret &&
      ext->ret_aggregate_classes[1] != NY_SYSV_AGG_NONE &&
      !capture_second_integer_first && !packed_integer_pair) {
    int selector = -1;
    if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER)
      /* A two-integer SysV result uses RAX then RDX.  A scalar/SSE first
       * result still leaves the integer member in the secondary register. */
      selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER ? 0 : 1;
    else if (ext->ret_aggregate_classes[1] == NY_SYSV_AGG_SSE)
      selector = ext->ret_aggregate_classes[0] == NY_SYSV_AGG_SSE ? 3 : 2;
    if (selector < 0) {
      return nyr_fail(b, "native NYIR lower: secondary aggregate return "
                         "class is not represented for the selected ABI"),
             -1;
    }
    second_ret = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .c = -1,
                                                   .imm = selector});
    if (second_ret < 0) {
      return nyr_fail(b, "native NYIR lower: return register capture failed"),
             -1;
    }
  }
  if (has_aggregate_return && !has_sret) {
    if ((second_ret >= 0 || packed_integer_pair) &&
        ext->ret_aggregate_size <= 8 &&
        ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER &&
        ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER) {
      int mask = nyr_emit_const(b, UINT64_C(0xffffffff));
      int shift = nyr_emit_const(b, 32);
      int lo = mask < 0 ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_AND_I64,
                                                            .dst = -1,
                                                            .a = primary_ret,
                                                            .b = mask});
      int high_raw = packed_integer_pair
                         ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SAR_I64,
                                                             .dst = -1,
                                                             .a = primary_ret,
                                                             .b = shift})
                         : second_ret;
      int hi = (high_raw < 0 || mask < 0)
                   ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_AND_I64,
                                                       .dst = -1,
                                                       .a = high_raw,
                                                       .b = mask});
      int shifted = (hi < 0 || shift < 0)
                        ? -1
                        : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                            .dst = -1,
                                                            .a = hi,
                                                            .b = shift});
      int packed = (lo < 0 || shifted < 0)
                       ? -1
                       : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_OR_I64,
                                                           .dst = -1,
                                                           .a = lo,
                                                           .b = shifted});
      if (packed < 0 || !nyr_emit_store_i64(b, aggregate_ret_ptr, packed))
        return -1;
    } else if (!nyr_emit_store_i64(b, aggregate_ret_ptr, primary_ret))
      return -1;
    if (second_ret >= 0 &&
        !(ext->ret_aggregate_size <= 8 &&
          ext->ret_aggregate_classes[0] == NY_SYSV_AGG_INTEGER &&
          ext->ret_aggregate_classes[1] == NY_SYSV_AGG_INTEGER)) {
      int off = nyr_emit_const(b, 8);
      int addr = off >= 0 ? nyr_emit_add_i64(b, aggregate_ret_ptr, off) : -1;
      if (addr < 0 || !nyr_emit_store_i64(b, addr, second_ret))
        return -1;
    }
  }
  if (return_type && strcmp(return_type, "bigint") == 0) {
    int bigint_tag = nyr_emit_const(b, TAG_BIGINT);
    if (bigint_tag < 0 ||
        !nyr_record_dyn_fact(b, v, NYR_FACT_DYN_TAG, bigint_tag))
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
    int meta_off = nyr_emit_const(b, -16);
    int meta = meta_off < 0 ? -1 : nyr_emit_add_i64(b, v, meta_off);
    int length = meta < 0 ? -1 : nyr_emit_load_i64(b, meta);
    if (length < 0 || !nyr_record_list_len_fact(b, v, length))
      return -1;
  }
  /* Normalize a declared tagged-integer result at the native scalar edge. */
  if (ny_native_runtime_abi_result_is_tagged_int(
          ny_native_runtime_abi_decl(runtime_symbol))) {
    int raw = nyr_emit_runtime_call(b, "rt_untag", v, -1, -1, 1, 0);
    if (raw < 0)
      return -1;
    v = raw;
  }

  /* A declared C `str` result is a borrowed char pointer. Normalize it at
   * the FFI edge while its declared type still proves that interpretation;
   * later dynamic code can inspect the managed string descriptor safely. */
  if (is_c_call && !has_aggregate_return &&
      ny_native_type_name_is_str(return_type)) {
    v = nyr_emit_runtime_call(b, "rt_cstr_to_str", v, -1, -1, 1, 0);
    if (v < 0)
      return -1;
  }
  return has_aggregate_return ? aggregate_ret_ptr : v;
}
static bool nyr_qualified_expr(const expr_t *e, char *out, size_t out_len) {
  if (!e || !out || out_len == 0)
    return false;
  if (e->kind == NY_E_SPLICE && e->as.splice.expr)
    return nyr_qualified_expr(e->as.splice.expr, out, out_len);
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    int n = snprintf(out, out_len, "%s", e->as.ident.name);
    return n >= 0 && (size_t)n < out_len;
  }
  if (e->kind == NY_E_MEMBER && e->as.member.target && e->as.member.name) {
    char prefix[512];
    if (!nyr_qualified_expr(e->as.member.target, prefix, sizeof(prefix)))
      return false;
    int n = snprintf(out, out_len, "%s.%s", prefix, e->as.member.name);
    return n >= 0 && (size_t)n < out_len;
  }
  return false;
}
