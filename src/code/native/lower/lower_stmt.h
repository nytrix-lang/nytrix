static bool ny_native_nir_lower_var(ny_native_nir_builder_t *b, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_del || v->is_destructure)
    return ny_native_nir_fail(b,
                              "native NYIR lower: only simple def/mut bindings are supported");
  for (size_t i = 0; i < v->names.len; ++i) {
    const char *name = v->names.data[i];
    if (!name || strcmp(name, "_") == 0)
      continue;
    if (i >= v->exprs.len || !v->exprs.data[i])
      return ny_native_nir_fail(b,
                                "native NYIR lower: local '%s' needs an initializer",
                                name);
    bool is_f64 = i < v->types.len && ny_native_type_name_is_f64(v->types.data[i]);
    bool is_f32 = i < v->types.len && ny_native_type_name_is_f32(v->types.data[i]);
    bool is_cstr = i < v->types.len && v->types.data[i] &&
                   strcmp(v->types.data[i], "str") == 0;
    bool is_any = i < v->types.len && v->types.data[i] &&
                  strcmp(v->types.data[i], "any") == 0;
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f64 = ny_native_nir_expr_is_f64(b, v->exprs.data[i]);
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f32 = ny_native_nir_expr_is_f32(b, v->exprs.data[i]);
    if (!is_cstr && i < v->exprs.len)
      is_cstr = ny_native_nir_expr_is_cstr(b, v->exprs.data[i]);
    if (!is_any && !is_cstr && i < v->exprs.len &&
        ny_native_nir_expr_is_any(b, v->exprs.data[i]))
      is_any = true;
    bool is_bool = i < v->types.len && v->types.data[i] &&
                   strcmp(v->types.data[i], "bool") == 0;
    if (!is_bool && i < v->exprs.len &&
        ny_native_nir_expr_is_bool(b, v->exprs.data[i]))
      is_bool = true;
    ny_native_nir_local_t *l =
        v->is_decl ? ny_native_nir_bind_local_typed(b, name, is_f64, is_f32,
                                                     is_cstr)
                   : ny_native_nir_add_local(b, name);
    if (l && is_f64)
      l->is_f64 = true;
    if (l && is_f32)
      l->is_f32 = true;
    if (l && is_cstr)
      l->is_cstr = true;
    if (l && is_bool)
      l->is_bool = true;
    if (l && is_any)
      l->is_any = true;
    bool is_list =
        (i < v->types.len && ny_native_type_name_is_list(v->types.data[i])) ||
        ny_native_nir_expr_is_list(b, v->exprs.data[i]);
    if (l && is_list && !l->is_list) {
      l->is_list = true;
      l->list_len_slot = b->next_local_slot++;
    }
    bool typed_scalar_list =
        i < v->types.len && v->types.data[i] &&
        ny_native_type_name_is_list(v->types.data[i]) &&
        !ny_native_type_name_is_dyn_list(v->types.data[i]);
    if (l && is_list) {
      l->is_dyn_list =
          !typed_scalar_list &&
          ((i < v->types.len &&
            ny_native_type_name_is_dyn_list(v->types.data[i])) ||
           ny_native_nir_expr_is_dyn_list(b, v->exprs.data[i]));
      l->list_literal =
          ny_native_nir_resolve_list_literal(b, v->exprs.data[i], 0);
    }
    if (typed_scalar_list)
      l->list_literal = NULL;
    int64_t declared_fin_bound =
        i < v->types.len ? ny_native_parse_fin_bound(v->types.data[i]) : 0;
    expr_t *init = v->exprs.data[i];
    int val = -1;
    int64_t saved_list_elem_size = b->current_list_elem_size;
    const char *decl_type =
        i < v->types.len ? v->types.data[i] : NULL;
    b->current_list_elem_size =
        is_list && decl_type && ny_native_type_name_is_list(decl_type) &&
                !ny_native_type_name_is_dyn_list(decl_type) &&
                v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_LIST &&
                v->exprs.data[i]->as.list_like.len == 0
            ? 8
            : 0;
    if (is_f32 && init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_FLOAT)
      val = ny_native_nir_emit_const_f32(b, init->as.literal.as.f);
    else if (v->is_decl && !v->is_mut && init && init->kind == NY_E_LIST &&
             init->as.list_like.len == 1) {
      val = ny_native_nir_lower_expr(b, init->as.list_like.data[0]);
    } else {
      val = ny_native_nir_lower_expr(b, init);
      if (val >= 0) {
        /*
         * Coerce an initializer whose expression type differs from the
         * declared scalar type.  `def f64 y = <f32 expr>` widens via
         * f32->f64; `def f32 y = <f64 expr>` narrows via f64->f32.  A float
         * literal bound to f32 was already emitted as a F32 constant above.
         */
        bool init_is_f64 = ny_native_nir_expr_is_f64(b, init);
        bool init_is_f32 = ny_native_nir_expr_is_f32(b, init);
        if (is_f64 && init_is_f32 && !init_is_f64)
          val = ny_native_nir_emit_f32_to_f64(b, val);
        else if (is_f32 && init_is_f64 && !init_is_f32)
          val = ny_native_nir_emit_f64_to_f32(b, val);
      }
    }
    b->current_list_elem_size = saved_list_elem_size;
    if (val < 0)
      return false;
    if (!ny_native_nir_store_local_value(b, l->slot, val))
      return false;
    /*
     * A candidate has one owner: a mutable declaration from a string literal.
     * Any subsequent ordinary identifier read clears this bit.
     */
    l->sb_candidate = v->is_decl && v->is_mut && init &&
                      init->kind == NY_E_LITERAL &&
                      init->as.literal.kind == NY_LIT_STR;
    if (declared_fin_bound > 0 && l->fin_bound == 0)
      l->fin_bound = declared_fin_bound;
    b->last_value = val;
  }
  return true;
}

static bool ny_native_nir_lower_if(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.iff.init && !ny_native_nir_lower_stmt(b, s->as.iff.init))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.iff.test);
  if (cond < 0)
    return false;
  /*
   * Statement-only `if` has no expression result.  Do not synthesize a
   * merge local from the prior statement's value: that can mix unrelated
   * scalar types across the branch (for example f64 work before int code).
   */
  if (!s->as.iff.alt) {
    int then_label = b->next_label++;
    int end_label = b->next_label++;
    bool entry_return = b->emitted_return;
    int entry_last_value = b->last_value;
    if (!ny_native_nir_emit_br_if(b, cond, then_label) ||
        !ny_native_nir_emit_br(b, end_label) ||
        !ny_native_nir_emit_label(b, then_label))
      return false;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(b, s->as.iff.conseq))
      return false;
    if (!b->emitted_return && !ny_native_nir_emit_br(b, end_label))
      return false;
    if (!ny_native_nir_emit_label(b, end_label))
      return false;
    b->emitted_return = entry_return;
    b->last_value = entry_last_value;
    return true;
  }
  int then_label = b->next_label++;
  int else_label = b->next_label++;
  int merge_label = b->next_label++;
  if (!ny_native_nir_emit_br_if(b, cond, then_label) ||
      !ny_native_nir_emit_br(b, else_label) ||
      !ny_native_nir_emit_label(b, then_label))
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  bool has_alt = s->as.iff.alt != NULL;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;

  /*
   * If no else, pre-store entry_last_value as the false-branch result.
   */
  if (!has_alt && !entry_return && entry_last_value >= 0 &&
      !ny_native_nir_store_local_value(b, result_slot, entry_last_value))
    return false;

  /*
   * Then branch.
   */
  b->emitted_return = false;
  if (!ny_native_nir_lower_scoped_body(b, s->as.iff.conseq))
    return false;
  bool conseq_returns = b->emitted_return;
  if (!conseq_returns) {
    int conseq_val = b->last_value;
    if (conseq_val >= 0 &&
        !ny_native_nir_store_local_value(b, result_slot, conseq_val))
      return false;
    /*
     * Jump to the merge point (before end_label) so both branches converge
     * before the shared load.local.
     */
    if (!ny_native_nir_emit_br(b, merge_label))
      return false;
  }
  if (!ny_native_nir_emit_label(b, else_label))
    return false;

  /*
   * Else branch.
   */
  b->emitted_return = false;
  b->last_value = entry_last_value;
  if (has_alt && !ny_native_nir_lower_scoped_body(b, s->as.iff.alt))
    return false;
  bool alt_returns = b->emitted_return;
  if (!alt_returns && has_alt) {
    int alt_val = b->last_value;
    if (alt_val >= 0 &&
        !ny_native_nir_store_local_value(b, result_slot, alt_val))
      return false;
  }

  /*
   * Merge point: both branches converge here.
   */
  b->emitted_return = entry_return || (has_alt && conseq_returns && alt_returns);
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
  }
  int merged = ny_native_nir_load_local_value(b, result_slot);
  if (merged < 0)
    return false;
  b->last_value = merged;
  return true;
}
static bool ny_native_nir_lower_while(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  int head_label = b->next_label++;
  int update_label = s->as.whl.update ? b->next_label++ : head_label;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.whl.test);
  if (cond < 0)
    return false;
  if (!ny_native_nir_emit_br_if(b, cond, body_label) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, body_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  size_t loop_i = b->loop_depth;
  if (!ny_native_nir_push_loop(b, head_label, update_label, end_label))
    return false;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_scoped_body(b, s->as.whl.body);
  if (body_ok && s->as.whl.update) {
    b->emitted_return = false;
    body_ok = ny_native_nir_emit_label(b, update_label) &&
              ny_native_nir_lower_scoped_body(b, s->as.whl.update);
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  return ny_native_nir_emit_br(b, head_label) &&
         ny_native_nir_emit_label(b, end_label);
}

static bool ny_native_nir_iterable_is_range(const expr_t *iterable,
                                            const expr_t **lo,
                                            const expr_t **hi) {
  if (!iterable || iterable->kind != NY_E_BINARY || !iterable->as.binary.op ||
      strcmp(iterable->as.binary.op, "..") != 0)
    return false;
  if (lo)
    *lo = iterable->as.binary.left;
  if (hi)
    *hi = iterable->as.binary.right;
  return true;
}

/*
 * Narrow ownership proof for the benchmark shape:
 *   mut s = "literal"; for i in lo..hi { s = s + "literal" }
 * The body is exactly one assignment, the lhs is the same local, and the
 * candidate has not had an ordinary read since its literal initialization.
 * Consequently no alias can observe the builder.  The caller finalizes at the
 * loop join before lowering any following statement.
 */
static bool ny_native_nir_self_concat_append(const stmt_t *body,
                                              ny_native_nir_local_t *local,
                                              const expr_t **rhs_out) {
  if (!body || !local || !rhs_out)
    return false;
  if (body->kind == NY_S_BLOCK) {
    if (body->as.block.body.len != 1)
      return false;
    body = body->as.block.body.data[0];
  }
  if (!body || body->kind != NY_S_VAR || body->as.var.is_decl ||
      body->as.var.names.len != 1 || body->as.var.exprs.len != 1 ||
      !body->as.var.names.data[0] || strcmp(body->as.var.names.data[0], local->name))
    return false;
  const expr_t *sum = body->as.var.exprs.data[0];
  if (!sum || sum->kind != NY_E_BINARY || !sum->as.binary.op ||
      strcmp(sum->as.binary.op, "+") || !sum->as.binary.left ||
      sum->as.binary.left->kind != NY_E_IDENT ||
      !sum->as.binary.left->as.ident.name ||
      strcmp(sum->as.binary.left->as.ident.name, local->name))
    return false;
  const expr_t *rhs = sum->as.binary.right;
  if (!rhs || rhs->kind != NY_E_LITERAL || rhs->as.literal.kind != NY_LIT_STR)
    return false;
  *rhs_out = rhs;
  return true;
}

static bool ny_native_nir_lower_for_range(ny_native_nir_builder_t *b,
                                          const stmt_t *s) {
  const expr_t *lo_expr = NULL;
  const expr_t *hi_expr = NULL;
  if (!s->as.fr.iter_var || !ny_native_nir_iterable_is_range(s->as.fr.iterable,
                                                             &lo_expr, &hi_expr)) {
    return ny_native_nir_fail(
        b, "native NYIR lower: for loops currently support `for name in lo..hi` ranges");
  }

  ny_native_nir_local_t *promoted = NULL;
  const expr_t *append_rhs = NULL;
  for (size_t n = b->local_count; n > 0; --n) {
    ny_native_nir_local_t *candidate = &b->locals[n - 1];
    if (candidate->is_cstr && candidate->sb_candidate && !candidate->is_sb &&
        ny_native_nir_self_concat_append(s->as.fr.body, candidate, &append_rhs)) {
      promoted = candidate;
      break;
    }
  }
  if (promoted) {
    int initial = ny_native_nir_load_local_value(b, promoted->slot);
    int builder = initial < 0 ? -1 : ny_native_nir_emit_runtime_call(
        b, "rt_native_cstr_builder_new", initial, -1, -1, 1, 0);
    if (builder < 0)
      return false;
    promoted->sb_slot = b->next_local_slot++;
    promoted->is_sb = true;
    promoted->sb_candidate = false;
    if (!ny_native_nir_store_local_value(b, promoted->sb_slot, builder))
      return false;
  }

  size_t loop_scope_mark = ny_native_nir_scope_mark(b);
  ny_native_nir_local_t *iter = ny_native_nir_bind_local(b, s->as.fr.iter_var);
  if (!iter)
    return false;
  ny_native_nir_local_t *index = NULL;
  if (s->as.fr.iter_index_var) {
    index = ny_native_nir_bind_local(b, s->as.fr.iter_index_var);
    if (!index)
      return false;
  }

  int lo = ny_native_nir_lower_expr(b, lo_expr);
  int hi = ny_native_nir_lower_expr(b, hi_expr);
  int hi_slot = ny_native_nir_temp_slot(b);
  if (lo < 0 || hi < 0 || !ny_native_nir_store_local_value(b, iter->slot, lo) ||
      !ny_native_nir_store_local_value(b, hi_slot, hi))
    return false;
  if (index) {
    int zero = ny_native_nir_emit_const(b, 0);
    if (zero < 0 || !ny_native_nir_store_local_value(b, index->slot, zero))
      return false;
  }

  int head_label = b->next_label++;
  int update_label = b->next_label++;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;

int cur = ny_native_nir_load_local_value(b, iter->slot);
  int end = ny_native_nir_load_local_value(b, hi_slot);
  /*
   * `lo..hi` is an inclusive Nytrix range.  Native lowering must keep the
   * terminal iteration; using `<` made every native loop silently omit `hi`,
   * so optimized native output disagreed with both the interpreter and C.
   */
  int in_range = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                    .dst = -1,
                                                    .a = cur,
                                                    .b = end,
                                                    .cmp = NYIR_CMP_LE});
  if (in_range < 0 || !ny_native_nir_emit_br_if(b, in_range, body_label) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, body_label))
    return false;

  size_t loop_i = b->loop_depth;
  if (!ny_native_nir_push_loop(b, head_label, update_label, end_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok;
  if (promoted) {
    int builder = ny_native_nir_load_local_value(b, promoted->sb_slot);
    int suffix = ny_native_nir_lower_expr(b, append_rhs);
    int ignored = (builder < 0 || suffix < 0) ? -1 :
        ny_native_nir_emit_runtime_call(b, "rt_native_cstr_builder_append",
                                        builder, suffix, -1, 2, 0);
    body_ok = ignored >= 0;
  } else {
    body_ok = ny_native_nir_lower_scoped_body(b, s->as.fr.body);
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;

  b->emitted_return = false;
  if (!ny_native_nir_emit_label(b, update_label))
    return false;
  int one = ny_native_nir_emit_const(b, 1);
  cur = ny_native_nir_load_local_value(b, iter->slot);
  int next = one >= 0 && cur >= 0
                 ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = cur,
                                                         .b = one})
                 : -1;
  if (next < 0 || !ny_native_nir_store_local_value(b, iter->slot, next))
    return false;
  if (index) {
    int old_idx = ny_native_nir_load_local_value(b, index->slot);
    int next_idx = old_idx >= 0
                       ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADD_I64,
                                                               .dst = -1,
                                                               .a = old_idx,
                                                               .b = one})
                       : -1;
    if (next_idx < 0 || !ny_native_nir_store_local_value(b, index->slot, next_idx))
      return false;
  }

  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  bool ok = ny_native_nir_emit_br(b, head_label) &&
            ny_native_nir_emit_label(b, end_label);
  /*
   * The promoted value becomes an ordinary immutable C string before the
   * first post-loop read, scope exit, or control-flow merge.
   */
  if (ok && promoted) {
    int builder = ny_native_nir_load_local_value(b, promoted->sb_slot);
    int value = builder < 0 ? -1 : ny_native_nir_emit_runtime_call(
        b, "rt_native_cstr_builder_finalize", builder, -1, -1, 1, 0);
    if (value < 0 || !ny_native_nir_store_local_value(b, promoted->slot, value))
      ok = false;
    /*
     * C-string length metadata must describe the finalized buffer, not the
     * literal initializer retained before promotion.
     */
    if (ok) {
      int length = ny_native_nir_emit_runtime_call(
          b, "rt_native_cstr_len", value, -1, -1, 1, 0);
      int tag = length < 0 ? -1 : ny_native_nir_emit_const(b, 121);
      if (length < 0 || tag < 0 ||
          !ny_native_nir_store_local_value(b, promoted->dyn_str_len_slot, length) ||
          !ny_native_nir_store_local_value(b, promoted->dyn_tag_slot, tag))
        ok = false;
    }
    promoted->is_sb = false;
    promoted->sb_slot = -1;
  }
  ny_native_nir_scope_restore(b, loop_scope_mark);
  return ok;
}

static bool ny_native_nir_lower_for(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.fr.init || s->as.fr.cond || s->as.fr.update)
    return ny_native_nir_fail(
        b, "native NYIR lower: only Nytrix iterator loops are supported here; use `for name in lo..hi { ... }` because ';' starts a comment");
  return ny_native_nir_lower_for_range(b, s);
}

static bool ny_native_nir_pattern_is_wildcard(const expr_t *pat) {
  return pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
         strcmp(pat->as.ident.name, "_") == 0;
}

static int ny_native_nir_emit_cmp(ny_native_nir_builder_t *b, int a, int rhs,
                                  nyir_cmp_t cmp) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs,
                                               .cmp = cmp});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_emit_bool_binop(ny_native_nir_builder_t *b,
                                         nyir_op_t op, int a, int rhs) {
  int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = op,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int ny_native_nir_lower_match_pattern(ny_native_nir_builder_t *b,
                                             int test_value,
                                             const expr_t *pat) {
  if (ny_native_nir_pattern_is_wildcard(pat))
    return ny_native_nir_emit_const(b, 1);

  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    int lo = ny_native_nir_lower_expr(b, pat->as.binary.left);
    int hi = ny_native_nir_lower_expr(b, pat->as.binary.right);
    if (lo < 0 || hi < 0)
      return -1;
    int ge = ny_native_nir_emit_cmp(b, test_value, lo, NYIR_CMP_GE);
    int le = ny_native_nir_emit_cmp(b, test_value, hi, NYIR_CMP_LE);
    if (ge < 0 || le < 0)
      return -1;
    return ny_native_nir_emit_bool_binop(b, NYIR_AND_I64, ge, le);
  }

  int rhs = ny_native_nir_lower_expr(b, pat);
  if (rhs < 0)
    return -1;
  return ny_native_nir_emit_cmp(b, test_value, rhs, NYIR_CMP_EQ);
}

static int ny_native_nir_lower_match_patterns(ny_native_nir_builder_t *b,
                                              int test_value,
                                              const match_arm_t *arm) {
  if (!arm || arm->patterns.len == 0)
    return ny_native_nir_emit_const(b, 0);
  int combined = -1;
  for (size_t i = 0; i < arm->patterns.len; ++i) {
    int cur = ny_native_nir_lower_match_pattern(b, test_value,
                                                arm->patterns.data[i]);
    if (cur < 0)
      return -1;
    combined = combined < 0
                   ? cur
                   : ny_native_nir_emit_bool_binop(b, NYIR_OR_I64,
                                                   combined, cur);
    if (combined < 0)
      return -1;
  }
  return combined;
}

static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test)
    return ny_native_nir_fail(b, "native NYIR lower: malformed case/match");

  int test_value = ny_native_nir_lower_expr(b, s->as.match.test);
  if (test_value < 0)
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  int merge_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  bool all_taken_paths_return = true;

  int initial = entry_last_value >= 0 ? entry_last_value
                                      : ny_native_nir_emit_const(b, 0);
  if (initial < 0 || !ny_native_nir_store_local_value(b, result_slot, initial))
    return false;

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    int next_label = b->next_label++;
    int pat = ny_native_nir_lower_match_patterns(b, test_value, arm);
    if (pat < 0)
      return false;
    int pat_false = ny_native_nir_emit_is_zero(b, pat);
    if (pat_false < 0 || !ny_native_nir_emit_br_if(b, pat_false, next_label))
      return false;

    if (arm->guard) {
      int guard = ny_native_nir_lower_expr(b, arm->guard);
      int guard_false = guard >= 0 ? ny_native_nir_emit_is_zero(b, guard) : -1;
      if (guard_false < 0 ||
          !ny_native_nir_emit_br_if(b, guard_false, next_label))
        return false;
    }

    b->last_value = -1;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(b, arm->conseq))
      return false;
    bool arm_returns = b->emitted_return;
    if (!arm_returns) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }

    if (!ny_native_nir_emit_label(b, next_label))
      return false;
  }

  if (s->as.match.default_conseq) {
    b->last_value = -1;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(b, s->as.match.default_conseq))
      return false;
    bool default_returns = b->emitted_return;
    if (!default_returns) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }
  } else {
    all_taken_paths_return = false;
  }

  b->emitted_return = entry_return || all_taken_paths_return;
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
    int loaded = ny_native_nir_load_local_value(b, result_slot);
    if (loaded < 0)
      return false;
    b->last_value = loaded;
  }
  return true;
}

/*
 * Convert only a direct self-call in return position and only when all
 * parameters use the scalar ABI.  Argument expressions are evaluated before
 * any parameter slot is overwritten, preserving ordinary call semantics.
 */
static int ny_native_nir_lower_tail_return(ny_native_nir_builder_t *b,
                                           const expr_t *value) {
  if (!b || !b->tail_recur_enabled || b->loop_depth != 0 ||
      b->defer_count != 0 || !value || value->kind != NY_E_CALL ||
      !value->as.call.callee || value->as.call.callee->kind != NY_E_IDENT ||
      !b->current_fn_name ||
      strcmp(value->as.call.callee->as.ident.name, b->current_fn_name) != 0 ||
      value->as.call.args.len != b->tail_param_count)
    return 0;
  size_t count = b->tail_param_count;
  int *args = count ? malloc(count * sizeof(*args)) : NULL;
  if (count && !args)
    return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  for (size_t i = 0; i < count; ++i) {
    args[i] = ny_native_nir_lower_expr(b, value->as.call.args.data[i].val);
    if (args[i] < 0) {
      free(args);
      return -1;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (!ny_native_nir_store_local_value(b, b->tail_param_slots[i], args[i])) {
      free(args);
      return -1;
    }
  }
  free(args);
  if (!ny_native_nir_emit_br(b, b->tail_loop_label))
    return -1;
  b->emitted_return = true;
  b->last_value = -1;
  return 1;
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (ny_native_nir_ignored_stmt(s) ||
      (s && (s->kind == NY_S_FUNC || s->kind == NY_S_LEMMA)))
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t mark = s->as.block.transparent ? b->local_count
                                           : ny_native_nir_scope_mark(b);
    size_t defer_mark = b->defer_count;
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *item = s->as.block.body.data[i];
      if (i + 1 == s->as.block.body.len && s == b->tail_body &&
          item && item->kind == NY_S_EXPR) {
        int tail = ny_native_nir_lower_tail_return(b, item->as.expr.expr);
        if (tail < 0) {
          if (!s->as.block.transparent)
            ny_native_nir_scope_restore(b, mark);
          return false;
        }
        if (tail > 0)
          break;
      }
      if (!ny_native_nir_lower_stmt(b, item)) {
        if (!s->as.block.transparent)
          ny_native_nir_scope_restore(b, mark);
        return false;
      }
      if (b->emitted_return)
        break;
    }
    if (!s->as.block.transparent) {
      /*
       * Defer bodies lower as ordinary statements and would clobber
       * last_value; the block's trailing expression must win for
       * implicit returns.
       */
      int saved_last = b->last_value;
      if (!ny_native_nir_emit_defers(b, defer_mark)) {
        b->last_value = saved_last;
        return false;
      }
      b->last_value = saved_last;
      ny_native_nir_scope_restore(b, mark);
    }
    return true;
  }
  case NY_S_VAR:
    return ny_native_nir_lower_var(b, s);
  case NY_S_EXPR: {
    int v = ny_native_nir_lower_expr(b, s->as.expr.expr);
    if (v < 0)
      return false;
    b->last_value = v;
    return true;
  }
  case NY_S_IF:
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        b->options) {
      bool selected = false;
      if (ny_native_target_eval_bool(b->options, s->as.iff.test, &selected)) {
        if (s->as.iff.init && !ny_native_nir_lower_stmt(b, s->as.iff.init))
          return false;
        return ny_native_nir_lower_stmt(
            b, selected ? s->as.iff.conseq : s->as.iff.alt);
      }
    }
    return ny_native_nir_lower_if(b, s);
  case NY_S_WHILE:
    return ny_native_nir_lower_while(b, s);
  case NY_S_FOR:
    return ny_native_nir_lower_for(b, s);
  case NY_S_TRY:
    /*
     * Shared NYIR has no exception edge.  Native runtime calls report errors
     * through their ordinary return values, so preserve the body path and
     * leave handler selection to the caller's explicit status checks.
     */
    return s->as.tr.body ? ny_native_nir_lower_stmt(b, s->as.tr.body) : true;
  case NY_S_MATCH:
    return ny_native_nir_lower_match(b, s);
  case NY_S_DEFER:
    return ny_native_nir_push_defer(b, s->as.de.body);
  case NY_S_BREAK:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: break outside loop");
    if (!ny_native_nir_emit_defers(
            b, b->loop_frames[b->loop_depth - 1].defer_mark))
      return false;
    b->emitted_return = true;
    return ny_native_nir_emit_br(b,
                                 b->loop_frames[b->loop_depth - 1].end_label);
  case NY_S_CONTINUE:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: continue outside loop");
    if (!ny_native_nir_emit_defers(
            b, b->loop_frames[b->loop_depth - 1].defer_mark))
      return false;
    b->emitted_return = true;
    return ny_native_nir_emit_br(
        b, b->loop_frames[b->loop_depth - 1].continue_label);
  case NY_S_RETURN: {
    int tail = s->as.ret.value
                 ? ny_native_nir_lower_tail_return(b, s->as.ret.value)
                 : 0;
    if (tail < 0)
      return false;
    if (tail > 0)
      return true;
    int v = s->as.ret.value ? ny_native_nir_lower_expr(b, s->as.ret.value)
                            : ny_native_nir_emit_const(b, 0);
    if (v < 0)
      return false;
    if (!ny_native_nir_emit_defers(b, 0))
      return false;
    return ny_native_nir_emit_ret(b, v);
  }
  default:
    return ny_native_nir_fail(b,
                              "native NYIR lower: statement kind %d is not in shared NYIR yet",
                              (int)s->kind);
  }
}

/*
 * Shared NYIR optimization + verification step.  After calling this the
 * builder's NYIR is ready for codegen or diagnostics.
 */
static bool ny_native_nir_finalize(ny_native_nir_builder_t *b,
                                     char *err, size_t err_len) {
  nyir_opt_stats_t stats;
  if (b->options &&
      (b->options->native_backend == NY_NATIVE_BACKEND_X86_64 ||
       b->options->native_backend == NY_NATIVE_BACKEND_AARCH64))
    nyir_set_preserve_phis(true);
  /*
   * Initial lowering is a verifier boundary too: no optimization pass should
   * have to defend itself against malformed values, labels, effects, or CFG
   * structure produced upstream.
   */
  if (!nyir_verify(&b->nyir, err, err_len))
    goto fail;
  ny_native_profile_select(b->profile_name ? b->profile_name : "rt_main");
  if (getenv("NY_TRACE_O3") && b->opt_level >= 3)
    nyir_optimize_debug(&b->nyir, stderr, &stats, b->opt_level);
  else if (!nyir_optimize_with_stats(&b->nyir, &stats, b->opt_level)) {
    if (verbose_enabled >= 1)
      fprintf(stderr, "nyir opt FAILED\n");
    if (err && err_len > 0 && err[0] == '\0')
      snprintf(err, err_len, "native NYIR: optimization failed");
    goto fail;
  }
  if (!nyir_verify(&b->nyir, err, err_len))
    goto fail;
  if (verbose_enabled >= 1 && stats.total_time_ms > 0.001) {
    bool grew = stats.after_insts > stats.before_insts;
    size_t delta = grew ? stats.after_insts - stats.before_insts
                        : stats.before_insts - stats.after_insts;
    double pct = stats.before_insts > 0
                     ? (grew ? 1.0 : -1.0) * 100.0 * (double)delta /
                           stats.before_insts
                     : 0.0;
    fprintf(stderr, "nyir finalize: %zu→%zu insts (%c%zu, %+.1f%%) in %.2fms\n",
            stats.before_insts, stats.after_insts, grew ? '+' : '-', delta, pct,
            stats.total_time_ms);
  }
  return true;
fail:
  /*
   * Reduced repro dump on failure.
   */
  fprintf(stderr, "native NYIR repro (optimize/verify failed): %s\n",
          err && err[0] ? err : "unknown error");
  nyir_dump(stderr, &b->nyir, "<failed>");
  return false;
}

static bool ny_native_nir_opt_dump(FILE *out, ny_native_nir_builder_t *b,
                                   const char *name, const ny_options *opt) {
  nyir_opt_stats_t stats;
  if (b->options &&
      (b->options->native_backend == NY_NATIVE_BACKEND_X86_64 ||
       b->options->native_backend == NY_NATIVE_BACKEND_AARCH64))
    nyir_set_preserve_phis(true);
  if (!nyir_verify(&b->nyir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      ny_native_set_err(b->err, b->err_len,
                        "native NYIR dump: initial verifier rejected input");
    return false;
  }
  bool optimized = opt && opt->nyir_dump_raw
                       ? nyir_optimize_debug(&b->nyir, out, &stats,
                                               b->opt_level)
                       : nyir_optimize_with_stats(&b->nyir, &stats,
                                                    b->opt_level);
  if (!optimized ||
      !nyir_verify(&b->nyir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      ny_native_set_err(b->err, b->err_len, "native NYIR dump: optimization failed");
    return false;
  }
  if (opt && opt->nyir_dump_stats)
    nyir_dump_stats(out, &stats);
  nyir_dump(out, &b->nyir, name);
  if (opt && opt->nyir_dump_cfg)
    nyir_dump_cfg(out, &b->nyir, name);
  return true;
}

/*
 * Lower a single function stmt into a finalized nyir_func_t.
 * Returns true on success; caller must nyir_func_free(out) when done.
 */
static bool ny_native_nir_build_function(const program_t *prog, const stmt_t *fn,
                                        nyir_func_t *out, char *err,
                                        size_t err_len, int opt_level,
                                        const ny_options *options) {
  if (!fn || fn->kind != NY_S_FUNC || !out)
    return false;
  memset(out, 0, sizeof(*out));
  int function_opt_level = fn->as.fn.attr_optimize
                               ? fn->as.fn.attr_optimize_level
                               : opt_level;
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                                .prog = prog, .options = options,
                                .profile_name = fn->as.fn.name ? fn->as.fn.name : "<fn>",
                                .opt_level = function_opt_level};
  if (ny_native_type_name_is_f64(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F64;
  else if (ny_native_type_name_is_f32(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F32;
  if (!ny_native_nir_set_param_types(&b, fn)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  int arg_index = 0;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const char *param_type = fn->as.fn.params.data[i].type;
    bool is_list = ny_native_type_name_is_list(param_type);
    bool is_str = ny_native_type_name_is_str(param_type);
    bool is_any = ny_native_type_name_is_any(param_type);
    ny_native_nir_local_t *param = ny_native_nir_bind_local_typed(
        &b, fn->as.fn.params.data[i].name,
        ny_native_type_name_is_f64(param_type),
        ny_native_type_name_is_f32(param_type),
        is_str);
    if (!param) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    param->is_any = is_any;
    param->is_list = is_list;
    param->is_dyn_list =
        (is_list &&
         (ny_native_type_name_is_dyn_list(param_type) ||
          ny_native_nir_function_param_is_dyn_list(prog, fn, i)));
    param->is_cstr = is_str;
    param->arg_slot = arg_index;
    arg_index++;
    if (is_list) {
      param->list_len_slot = b.next_local_slot++;
      param->list_len_arg_slot = arg_index;
      arg_index++;
    } else if (is_str || is_any) {
      param->dyn_str_len_slot = b.next_local_slot++;
      param->dyn_tag_slot = b.next_local_slot++;
      param->dyn_str_len_arg_slot = arg_index;
      arg_index++;
      param->dyn_tag_arg_slot = arg_index;
      arg_index++;
    }
    param->fin_bound =
        ny_native_parse_fin_bound(param_type);
  }
  b.current_fn_name = fn->as.fn.name;
  b.nyir.param_count = (unsigned)arg_index;
  /*
   * Function prologue: move expanded arguments from their incoming
   * positions (ld #arg_slot) into the local slots that the body expects.
   */
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    ny_native_nir_local_t *param = ny_native_nir_find_local(&b, fn->as.fn.params.data[i].name);
    if (!param)
      continue;
    if (param->arg_slot >= 0) {
      int arg_val = nyir_emit(&b.nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = param->arg_slot});
      if (arg_val >= 0)
        ny_native_nir_store_local_value(&b, param->slot, arg_val);
    }
    if (param->is_list && param->list_len_arg_slot >= 0) {
      int len_val = nyir_emit(&b.nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = param->list_len_arg_slot});
      if (len_val >= 0)
        ny_native_nir_store_local_value(&b, param->list_len_slot, len_val);
    } else if ((param->is_cstr || param->is_any) && param->dyn_str_len_arg_slot >= 0) {
      int len_val = nyir_emit(&b.nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = param->dyn_str_len_arg_slot});
      int tag_val = nyir_emit(&b.nyir, (nyir_inst_t){.op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = param->dyn_tag_arg_slot});
      if (len_val >= 0)
        ny_native_nir_store_local_value(&b, param->dyn_str_len_slot, len_val);
      if (tag_val >= 0)
        ny_native_nir_store_local_value(&b, param->dyn_tag_slot, tag_val);
    }
  }
  b.tail_body = fn->as.fn.body;
  bool scalar_tail = b.current_fn_name && fn->as.fn.body &&
                     !ny_native_nir_stmt_has_defer(fn->as.fn.body, 0);
  for (size_t i = 0; scalar_tail && i < fn->as.fn.params.len; ++i) {
    const char *type = fn->as.fn.params.data[i].type;
    if (ny_native_type_name_is_list(type) ||
        ny_native_type_name_is_str(type) ||
        ny_native_type_name_is_any(type))
      scalar_tail = false;
  }
  if (scalar_tail) {
    b.tail_param_count = fn->as.fn.params.len;
    if (b.tail_param_count > 0) {
      b.tail_param_slots = calloc(b.tail_param_count,
                                  sizeof(*b.tail_param_slots));
      if (!b.tail_param_slots) {
        nyir_func_free(&b.nyir);
        ny_native_nir_builder_dispose(&b);
        return ny_native_nir_fail(&b, NY_NATIVE_ALLOC_FAIL);
      }
      for (size_t i = 0; i < b.tail_param_count; ++i) {
        ny_native_nir_local_t *param = ny_native_nir_find_local(
            &b, fn->as.fn.params.data[i].name);
        if (!param) {
          scalar_tail = false;
          break;
        }
        b.tail_param_slots[i] = param->slot;
      }
    }
    if (scalar_tail) {
      b.tail_loop_label = b.next_label++;
      b.tail_recur_enabled = true;
      if (!ny_native_nir_emit_label(&b, b.tail_loop_label)) {
        nyir_func_free(&b.nyir);
        ny_native_nir_builder_dispose(&b);
        return false;
      }
    }
  }
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  if (ok && !b.emitted_return) {
    /*
     * Capture the implicit return value before defer bodies run: defer
     * statements lower as expression statements and would clobber
     * last_value.
     */
    int ret = b.last_value >= 0 ? b.last_value
                                : ny_native_nir_emit_const(&b, 0);
    ok = ret >= 0 && ny_native_nir_emit_defers(&b, 0);
    if (ok)
      ok = ny_native_nir_emit_ret(&b, ret);
  }
  if (ok)
    ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nyir;
  else
    nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  return ok;
}

/*
 * Build extern table from #include and extern top-level statements.
 * Populates the table with NY name → C symbol mappings so the call
 * lowerer can emit correct linker symbols for extern C functions.
 *
 * Like codegen_collect_links()/process_links(), this recurses through
 * the program: extern declarations are not guaranteed to be top-level.
 * The prelude/stdlib and the script wrapper nest user declarations
 * inside NY_S_MODULE/NY_S_BLOCK (and control-flow bodies), so a flat
 * scan of prog->body would silently miss them.
 */
static bool ny_native_target_platform_ident(const ny_options *opt,
                                             const char *name, bool *out) {
  if (!name || !out)
    return false;
  const char *triple = opt ? opt->host_triple : NULL;
  const char *host_os = ny_host_os_name();
  const char *host_arch = ny_host_arch_name();
  bool is_windows = triple && (strstr(triple, "windows") || strstr(triple, "mingw") ||
                               strstr(triple, "msvc") || strstr(triple, "win32"));
  bool is_macos = triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
                             strstr(triple, "macos"));
  bool is_linux = triple && strstr(triple, "linux");
  if (!triple || !*triple) {
    is_windows = strcmp(host_os, "windows") == 0;
    is_macos = strcmp(host_os, "macos") == 0;
    is_linux = strcmp(host_os, "linux") == 0;
  }
  if (opt && opt->native_abi == NY_NATIVE_ABI_WIN64)
    is_windows = true, is_macos = false, is_linux = false;

  bool is_x86_64 = false, is_x86 = false, is_aarch64 = false;
  bool is_arm = false, is_riscv = false;
  if (opt) {
    switch (opt->native_backend) {
    case NY_NATIVE_BACKEND_X86_64: is_x86_64 = is_x86 = true; break;
    case NY_NATIVE_BACKEND_X86: is_x86 = true; break;
    case NY_NATIVE_BACKEND_AARCH64: is_aarch64 = is_arm = true; break;
    case NY_NATIVE_BACKEND_ARM: is_arm = true; break;
    case NY_NATIVE_BACKEND_RISCV: is_riscv = true; break;
    default: break;
    }
  }
  if (!is_x86 && !is_aarch64 && !is_arm && !is_riscv) {
    const char *arch = triple && *triple ? triple : host_arch;
    is_x86_64 = strstr(arch, "x86_64") || strstr(arch, "amd64");
    is_x86 = is_x86_64 || strstr(arch, "i386") || strstr(arch, "i686") ||
             strcmp(arch, "x86") == 0;
    is_aarch64 = strstr(arch, "aarch64") || strstr(arch, "arm64");
    is_arm = is_aarch64 || (strstr(arch, "arm") && !strstr(arch, "aarch64"));
    is_riscv = strstr(arch, "riscv") != NULL;
  }
  bool is_unix = !is_windows && (is_linux || is_macos ||
                                  strcmp(host_os, "unknown") != 0);
  const struct { const char *name; bool value; } values[] = {
      {"linux", is_linux}, {"LINUX", is_linux}, {"IS_LINUX", is_linux},
      {"macos", is_macos}, {"mac", is_macos}, {"MACOS", is_macos},
      {"IS_MACOS", is_macos}, {"windows", is_windows},
      {"IS_WINDOWS", is_windows}, {"unix", is_unix}, {"posix", is_unix},
      {"UNIX", is_unix}, {"IS_UNIX", is_unix}, {"x86_64", is_x86_64},
      {"x64", is_x86_64}, {"IS_X86_64", is_x86_64}, {"x86", is_x86},
      {"IS_X86", is_x86}, {"aarch64", is_aarch64}, {"arm64", is_aarch64},
      {"IS_AARCH64", is_aarch64}, {"arm", is_arm}, {"IS_ARM", is_arm},
      {"riscv", is_riscv}, {"IS_RISCV", is_riscv},
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    if (strcmp(name, values[i].name) == 0) {
      *out = values[i].value;
      return true;
    }
  }
  return false;
}

/*
 * Evaluate the platform queries accepted in comptime branch conditions.  This
 * deliberately recognizes only zero-argument host queries: an arbitrary call
 * must remain unknown so extern collection continues to inspect both paths.
 */
static bool ny_native_target_platform_string(const ny_options *opt,
                                             const expr_t *e,
                                             const char **out) {
  if (!e || !out)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_STR) {
    *out = e->as.literal.as.s.data;
    return true;
  }
  if (e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT || e->as.call.args.len != 0)
    return false;

  const char *name = e->as.call.callee->as.ident.name;
  if (!name)
    return false;
  bool value = false;
  if (strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) {
    if (ny_native_target_platform_ident(opt, "windows", &value) && value)
      *out = "windows";
    else if (ny_native_target_platform_ident(opt, "macos", &value) && value)
      *out = "macos";
    else if (ny_native_target_platform_ident(opt, "linux", &value) && value)
      *out = "linux";
    else
      *out = ny_host_os_name();
    return true;
  }
  if (strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) {
    if (ny_native_target_platform_ident(opt, "x86_64", &value) && value)
      *out = "x86_64";
    else if (ny_native_target_platform_ident(opt, "aarch64", &value) && value)
      *out = "aarch64";
    else if (ny_native_target_platform_ident(opt, "riscv", &value) && value)
      *out = "riscv";
    else if (ny_native_target_platform_ident(opt, "x86", &value) && value)
      *out = "x86";
    else if (ny_native_target_platform_ident(opt, "arm", &value) && value)
      *out = "arm";
    else
      *out = ny_host_arch_name();
    return true;
  }
  return false;
}

bool ny_native_target_eval_bool(const ny_options *opt, const expr_t *e,
                                bool *out) {
  if (!e || !out)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL) {
      *out = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      *out = e->as.literal.as.i != 0;
      return true;
    }
    return false;
  case NY_E_IDENT:
    return ny_native_target_platform_ident(opt, e->as.ident.name, out);
  case NY_E_UNARY: {
    bool v = false;
    if (!e->as.unary.op || strcmp(e->as.unary.op, "!") != 0 ||
        !ny_native_target_eval_bool(opt, e->as.unary.right, &v))
      return false;
    *out = !v;
    return true;
  }
  case NY_E_LOGICAL: {
    bool l = false, r = false;
    if (!e->as.logical.op ||
        !ny_native_target_eval_bool(opt, e->as.logical.left, &l))
      return false;
    if (strcmp(e->as.logical.op, "&&") == 0) {
      if (!l) { *out = false; return true; }
      if (!ny_native_target_eval_bool(opt, e->as.logical.right, &r)) return false;
      *out = r; return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      if (l) { *out = true; return true; }
      if (!ny_native_target_eval_bool(opt, e->as.logical.right, &r)) return false;
      *out = r; return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    if (!e->as.binary.op ||
        (strcmp(e->as.binary.op, "==") != 0 &&
         strcmp(e->as.binary.op, "!=") != 0))
      return false;
    const char *left = NULL;
    const char *right = NULL;
    if (!ny_native_target_platform_string(opt, e->as.binary.left, &left) ||
        !ny_native_target_platform_string(opt, e->as.binary.right, &right))
      return false;
    bool equal = strcmp(left, right) == 0;
    *out = strcmp(e->as.binary.op, "==") == 0 ? equal : !equal;
    return true;
  }
  case NY_E_COMPTIME: {
    const stmt_t *body = e->as.comptime_expr.body;
    if (!body)
      return false;
    if (body->kind == NY_S_BLOCK && body->as.block.body.len == 1)
      body = body->as.block.body.data[0];
    if (!body)
      return false;
    if (body->kind == NY_S_RETURN)
      return ny_native_target_eval_bool(opt, body->as.ret.value, out);
    if (body->kind == NY_S_EXPR)
      return ny_native_target_eval_bool(opt, body->as.expr.expr, out);
    return false;
  }
  default:
    return false;
  }
}

static bool ny_native_nir_collect_extern(const stmt_t *s, ny_extern_table_t *t,
                                         bool aapcs, const ny_options *opt,
                                         char *err, size_t err_len) {
  if (!s)
    return true;
  if (s->kind == NY_S_EXTERN) {
    const char *ny_name = s->as.ext.name;
    const char *c_sym = s->as.ext.link_name ? s->as.ext.link_name : ny_name;
    unsigned pc = (unsigned)s->as.ext.params.len;
    if (!ny_extern_table_add(t, ny_name, c_sym, pc, false, 0, NULL, NULL)) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "NYIR extern: conflicting or duplicate extern '%s' "
                 "(C symbol '%s' conflicts with earlier declaration)",
                 ny_name, c_sym);
      return false;
    }
    return true;
  }
  if (s->kind == NY_S_INCLUDE) {
    const char *prefix = s->as.inc.prefix;
    char *src = ny_read_file(s->as.inc.path);
    if (!src)
      return true;
    size_t srclen = strlen(src);
    ny_parser_t *parser = malloc(sizeof(*parser));
    if (!parser) {
      free(src);
      if (err && err_len > 0)
        snprintf(err, err_len, "NYIR extern: out of memory parsing #include");
      return false;
    }
    ny_parse_init(parser, src, srclen);
    ny_cdecl_t decl;
    while (ny_parse_decl(parser, &decl) > 0) {
      if (decl.kind != NY_CDECL_FUNC)
        continue;
      size_t nlen = decl.name.len;
      char cname[256];
      if (nlen >= sizeof(cname))
        nlen = sizeof(cname) - 1;
      memcpy(cname, decl.name.start, nlen);
      cname[nlen] = '\0';
      char ny_name[512];
      if (prefix && prefix[0]) {
        int nn = snprintf(ny_name, sizeof(ny_name), "%s.%s", prefix, cname);
        if (nn < 0 || (size_t)nn >= sizeof(ny_name))
          continue;
      } else {
        size_t nn = nlen;
        if (nn >= sizeof(ny_name))
          nn = sizeof(ny_name) - 1;
        memcpy(ny_name, cname, nn);
        ny_name[nn] = '\0';
      }
      /*
       * Compute aggregate return size and per-argument aggregate sizes
       */
      uint32_t ret_agg = 0;
      ny_sysv_agg_class_t ret_agg_classes[2] = {NY_SYSV_AGG_NONE,
                                                NY_SYSV_AGG_NONE};
      if (decl.type.kind == NY_CTYPE_STRUCT || decl.type.kind == NY_CTYPE_UNION) {
        ret_agg = (uint32_t)decl.type.aggregate_size;
      } else if (decl.type.kind == NY_CTYPE_NAMED && decl.type.ptr_depth == 0) {
        /*
         * Named typedef that may be a struct — aggregate_size if present
         */
        ret_agg = (uint32_t)decl.type.aggregate_size;
      }
      if (ret_agg > 0 &&
          !(aapcs ? ny_native_aapcs_classify_aggregate(parser, &decl.type,
                                                        ret_agg_classes)
                   : ny_native_sysv_classify_aggregate(parser, &decl.type,
                                                       ret_agg_classes)))
        ret_agg_classes[0] = NY_SYSV_AGG_UNSUPPORTED;
      uint32_t arg_agg[NY_C_MAX_PARAMS] = {0};
      for (unsigned pi = 0; pi < decl.param_count && pi < NY_C_MAX_PARAMS; pi++) {
        const ny_ctype_t *pt = &decl.params[pi];
        if ((pt->kind == NY_CTYPE_STRUCT || pt->kind == NY_CTYPE_UNION) &&
            pt->ptr_depth == 0) {
          arg_agg[pi] = (uint32_t)pt->aggregate_size;
        } else if (pt->kind == NY_CTYPE_NAMED && pt->ptr_depth == 0) {
          arg_agg[pi] = (uint32_t)pt->aggregate_size;
        }
        if (arg_agg[pi] > 0) {
          ny_sysv_agg_class_t classes[2] = {NY_SYSV_AGG_NONE,
                                            NY_SYSV_AGG_NONE};
          if (arg_agg[pi] > NYIR_ARG_AGG_SIZE_MASK ||
              !(aapcs ? ny_native_aapcs_classify_aggregate(parser, pt, classes)
                       : ny_native_sysv_classify_aggregate(parser, pt,
                                                           classes))) {
            classes[0] = NY_SYSV_AGG_UNSUPPORTED;
            classes[1] = NY_SYSV_AGG_NONE;
          }
          arg_agg[pi] =
              (arg_agg[pi] & NYIR_ARG_AGG_SIZE_MASK) |
              ((uint32_t)classes[0] << NYIR_ARG_AGG_CLASS0_SHIFT) |
              ((uint32_t)classes[1] << NYIR_ARG_AGG_CLASS1_SHIFT);
        }
      }
      char *ny_name_dup = ny_strdup(ny_name);
      char *c_sym = ny_strdup(cname);
      if (!ny_name_dup || !c_sym ||
          !ny_extern_table_add(t, ny_name_dup, c_sym, decl.param_count, true,
                               ret_agg, ret_agg_classes, arg_agg)) {
        free(ny_name_dup);
        free(c_sym);
        ny_parse_cleanup(parser);
        free(parser);
        free(src);
        if (err && err_len > 0)
          snprintf(err, err_len, "NYIR extern: table full from #include");
        return false;
      }
      if (!prefix || !prefix[0]) {
        char default_name[512];
        int nn = snprintf(default_name, sizeof(default_name), "c.%s", cname);
        char *default_name_dup =
            nn > 0 && (size_t)nn < sizeof(default_name)
                ? ny_strdup(default_name)
                : NULL;
        char *default_c_sym = ny_strdup(cname);
        if (!default_name_dup || !default_c_sym ||
            !ny_extern_table_add(t, default_name_dup, default_c_sym,
                                 decl.param_count, true, ret_agg,
                                 ret_agg_classes, arg_agg)) {
          free(default_name_dup);
          free(default_c_sym);
          ny_parse_cleanup(parser);
          free(parser);
          free(src);
          if (err && err_len > 0)
            snprintf(err, err_len,
                     "NYIR extern: table full from default C namespace");
          return false;
        }
      }
    }
    ny_parse_cleanup(parser);
    free(parser);
    free(src);
    return true;
  }
  /*
   * Recurse through container statements, mirroring process_links().
   */
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.module.body.data[i], t, aapcs, opt, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.block.body.data[i], t, aapcs, opt, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(opt, s->as.iff.test, &selected)) {
      const stmt_t *branch = selected ? s->as.iff.conseq : s->as.iff.alt;
      return !branch || ny_native_nir_collect_extern(branch, t, aapcs, opt,
                                                      err, err_len);
    }
    if (s->as.iff.conseq &&
        !ny_native_nir_collect_extern(s->as.iff.conseq, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.iff.alt &&
        !ny_native_nir_collect_extern(s->as.iff.alt, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body &&
        !ny_native_nir_collect_extern(s->as.whl.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.whl.update &&
        !ny_native_nir_collect_extern(s->as.whl.update, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.whl.init &&
        !ny_native_nir_collect_extern(s->as.whl.init, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init &&
        !ny_native_nir_collect_extern(s->as.fr.init, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.fr.body &&
        !ny_native_nir_collect_extern(s->as.fr.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.fr.update &&
        !ny_native_nir_collect_extern(s->as.fr.update, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_TRY) {
    if (s->as.tr.body &&
        !ny_native_nir_collect_extern(s->as.tr.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.tr.handler &&
        !ny_native_nir_collect_extern(s->as.tr.handler, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_DEFER) {
    if (s->as.de.body &&
        !ny_native_nir_collect_extern(s->as.de.body, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (s->as.match.arms.data[i].conseq &&
          !ny_native_nir_collect_extern(s->as.match.arms.data[i].conseq, t,
                                        aapcs, opt, err, err_len))
        return false;
    if (s->as.match.default_conseq &&
        !ny_native_nir_collect_extern(s->as.match.default_conseq, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  return true;
}

static bool ny_native_nir_build_extern_table(const program_t *prog,
                                              ny_extern_table_t *t,
                                              bool aapcs,
                                              const ny_options *opt, char *err,
                                              size_t err_len) {
  if (!t)
    return false;
  ny_extern_table_init(t);
  if (!prog)
    return true;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!ny_native_nir_collect_extern(s, t, aapcs, opt, err, err_len))
      return false;
  }
  return true;
}

/*
 * Per-pass oracle callback state.  The IR layer calls this after every
 * successful verifier checkpoint while optimizing rt_main.
 */
typedef struct {
  const ny_options *opt;
  nyir_func_t *funcs;
  const char **names;
  size_t count;
} ny_native_oracle_ctx_t;

static bool ny_native_per_pass_oracle_cb(const nyir_func_t *f,
                                         const char *pass_name,
                                         void *userdata) {
  ny_native_oracle_ctx_t *ctx = (ny_native_oracle_ctx_t *)userdata;
  char err[512] = {0};
  bool ok = ny_native_result_oracle_for_nir(
      (nyir_func_t *)f, ctx->funcs, ctx->names, ctx->count, ctx->opt, err,
      sizeof(err));
  if (!ok) {
    fprintf(stderr, "native NYIR: per-pass oracle failed after %s: %s\n",
            pass_name ? pass_name : "pass", err[0] ? err : NY_NATIVE_UNKNOWN_ERR);
  }
  return ok;
}

/*
 * Lower the top-level program statements into a finalized nyir_func_t for
 * rt_main.  Returns true on success; caller must nyir_func_free(out).
 */
static bool ny_native_nir_build_rt_main(const program_t *prog, nyir_func_t *out,
                                        const ny_extern_table_t *externs,
                                        char *err, size_t err_len, int opt_level,
                                        const ny_options *options) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                               .externs = externs, .prog = prog, .options = options,
                               .profile_name = "rt_main",
                               .current_fn_name = "rt_main",
                               .opt_level = opt_level};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      /*
       * Scan for a user-defined main function and call it.
       */
      const stmt_t *main_fn = NULL;
      for (size_t i = 0; prog && i < prog->body.len; ++i) {
        const stmt_t *s = prog->body.data[i];
        if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
            strcmp(s->as.fn.name, "main") == 0) {
          main_fn = s;
          break;
        }
      }
      if (!main_fn) {
        ny_native_nir_fail(&b, "native NYIR: program has no raw expression result");
        nyir_func_free(&b.nyir);
        ny_native_nir_builder_dispose(&b);
        return false;
      }
      int call_val = nyir_emit(&b.nyir, (nyir_inst_t){.op = NYIR_CALL,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .c = -1,
                                                       .imm = 0,
                                                       .flags = 0,
                                                       .symbol = "main"});
      if (call_val < 0) {
        ny_native_nir_fail(&b, "native NYIR: failed to emit call to main");
        nyir_func_free(&b.nyir);
        ny_native_nir_builder_dispose(&b);
        return false;
      }
      b.last_value = call_val;
    }
    int ret_val = b.last_value;
    if (!ny_native_nir_emit_defers(&b, 0)) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, ret_val)) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
  }
  bool ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nyir;
  else
    nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  return ok;
}

typedef struct {
  const program_t *prog;
  const ny_options *opt;
  const stmt_t **funcs;
  size_t count;
  size_t max_funcs;
  const char *query_fn;
  size_t query_param;
  const stmt_t *query_scope;
  bool query_dyn_list;
} ny_native_fn_collector_t;
static const expr_t *ny_native_nir_find_var_init_in_stmt(
    const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || !*name || depth > 32)
    return NULL;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], name) == 0 &&
          i < s->as.var.exprs.len)
        return s->as.var.exprs.data[i];
    return NULL;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_var_init_in_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_var_init_in_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_FUNC:
    return ny_native_nir_find_var_init_in_stmt(s->as.fn.body, name, depth + 1);
  case NY_S_IF: {
    const expr_t *found =
        ny_native_nir_find_var_init_in_stmt(s->as.iff.conseq, name, depth + 1);
    return found ? found
                 : ny_native_nir_find_var_init_in_stmt(
                       s->as.iff.alt, name, depth + 1);
  }
  case NY_S_WHILE:
    return ny_native_nir_find_var_init_in_stmt(s->as.whl.body, name, depth + 1);
  case NY_S_FOR:
    return ny_native_nir_find_var_init_in_stmt(s->as.fr.body, name, depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const expr_t *found = ny_native_nir_find_var_init_in_stmt(
          s->as.match.arms.data[i].conseq, name, depth + 1);
      if (found)
        return found;
    }
    return ny_native_nir_find_var_init_in_stmt(
        s->as.match.default_conseq, name, depth + 1);
  default:
    return NULL;
  }
}
static bool ny_native_nir_expr_has_dyn_list_shape(
    const program_t *prog, const expr_t *e, unsigned depth);

static const expr_t *ny_native_nir_find_global_var_init(
    const stmt_t *s, const char *name, const char *filename, unsigned depth) {
  if (!s || !name || !*name || depth > 32)
    return NULL;
  switch (s->kind) {
  case NY_S_VAR:
    if (filename && s->tok.filename &&
        strcmp(filename, s->tok.filename) != 0)
      return NULL;
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], name) == 0 &&
          i < s->as.var.exprs.len)
        return s->as.var.exprs.data[i];
    return NULL;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_global_var_init(
          s->as.block.body.data[i], name, filename, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const expr_t *found = ny_native_nir_find_global_var_init(
          s->as.module.body.data[i], name, filename, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  default:
    return NULL;
  }
}

static bool ny_native_nir_expr_has_dyn_list_shape_in_scope(
    const program_t *prog, const stmt_t *scope, const expr_t *e,
    unsigned depth) {
  if (!e || depth > 32)
    return false;
  if (e->kind == NY_E_IDENT && prog) {
    const expr_t *init =
        ny_native_nir_find_var_init_in_stmt(scope, e->as.ident.name, 0);
    if (init && e->tok.filename && init->tok.filename &&
        strcmp(e->tok.filename, init->tok.filename) != 0)
      init = NULL;
    if (!init) {
      for (size_t i = 0; i < prog->body.len && !init; ++i)
        init = ny_native_nir_find_global_var_init(
            prog->body.data[i], e->as.ident.name, e->tok.filename, 0);
    }
    if (init && init != e)
      return ny_native_nir_expr_has_dyn_list_shape_in_scope(
          prog, scope, init, depth + 1);
    return false;
  }
  return ny_native_nir_expr_has_dyn_list_shape(prog, e, depth);
}

static bool ny_native_nir_expr_has_dyn_list_shape(
    const program_t *prog, const expr_t *e, unsigned depth) {
  if (!e || depth > 32)
    return false;
  if (e->kind == NY_E_LIST) {
    if (e->as.list_like.len == 0)
      return true;
    bool has_f64 = false, has_int = false;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const expr_t *el = e->as.list_like.data[i];
      const expr_t *scalar = el;
      if (scalar && scalar->kind == NY_E_UNARY && scalar->as.unary.op &&
          (strcmp(scalar->as.unary.op, "+") == 0 ||
           strcmp(scalar->as.unary.op, "-") == 0))
        scalar = scalar->as.unary.right;
      if (!scalar || scalar->kind != NY_E_LITERAL)
        return true;
      if (scalar->as.literal.kind == NY_LIT_STR)
        return true;
      if (scalar->as.literal.kind == NY_LIT_FLOAT)
        has_f64 = true;
      else
        has_int = true;
    }
    return has_f64 && has_int;
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      strcmp(e->as.memcall.name, "append") == 0)
    return true;
  if (e->kind == NY_E_IDENT && prog) {
    for (size_t i = 0; i < prog->body.len; ++i) {
      const expr_t *init = ny_native_nir_find_var_init_in_stmt(
          prog->body.data[i], e->as.ident.name, depth + 1);
      if (init &&
          ny_native_nir_expr_has_dyn_list_shape(prog, init, depth + 1))
        return true;
    }
  }
  return false;
}

static const stmt_t *ny_native_nir_find_user_function_in_prog(
    const program_t *prog, const char *name) {
  if (!prog || !name)
    return NULL;
  if (ny_native_fn_cache.prog == prog)
    return ny_native_fn_cache_lookup(name);
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *found =
        ny_native_nir_find_user_function_in_stmt(prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

static void ny_native_add_reachable_fn(ny_native_fn_collector_t *col,
                                       const char *name);
static bool ny_native_nir_qualified_expr(const expr_t *e, char *out,
                                         size_t out_len);
static bool ny_native_scan_qualified_call(
    ny_native_fn_collector_t *col, const expr_t *target, const char *method) {
  if (!col || !target || !method || !*method)
    return false;
  char qualified[512];
  if (!ny_native_nir_qualified_expr(target, qualified, sizeof(qualified)))
    return false;
  size_t used = strlen(qualified);
  if (used + 1 + strlen(method) >= sizeof(qualified))
    return false;
  qualified[used++] = '.';
  strcpy(qualified + used, method);
  const char *dot = strchr(qualified, '.');
  if (!dot || dot == qualified)
    return false;
  char alias[256];
  size_t alias_len = (size_t)(dot - qualified);
  if (alias_len >= sizeof(alias))
    return false;
  memcpy(alias, qualified, alias_len);
  alias[alias_len] = '\0';
  ny_native_nir_builder_t probe = {.prog = col->prog};
  const char *module = ny_native_nir_resolve_use_alias(&probe, alias);
  if (!module)
    return false;
  char canonical[512];
  int n = snprintf(canonical, sizeof(canonical), "%s%s", module, dot);
  if (n < 0 || (size_t)n >= sizeof(canonical))
    return false;
  ny_native_add_reachable_fn(col, canonical);
  return true;
}
static void ny_native_scan_expr_for_calls(const expr_t *e,
                                          ny_native_fn_collector_t *col);
static void ny_native_scan_stmt_for_calls(const stmt_t *s,
                                          ny_native_fn_collector_t *col);


static bool ny_native_nir_stdlib_call_lowered_inline(const char *name) {
  if (!name || !*name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  if (strcmp(name, "std.core.str.replace") == 0 ||
      strcmp(name, "std.core.replace") == 0 ||
      strcmp(name, "std.os.ui.render.init_window") == 0 ||
      strcmp(name, "std.os.ui.render.close_window") == 0 ||
      strcmp(name, "std.os.ui.render.font_load_first") == 0 ||
      strcmp(name, "std.os.ui.render.font_destroy") == 0 ||
      strcmp(name, "std.os.ui.render.measure_text") == 0 ||
      strcmp(name, "std.os.ui.render.window_should_close") == 0 ||
      strcmp(name, "std.os.ui.render.begin_frame_clear") == 0 ||
      strcmp(name, "std.os.ui.render.framebuffer_size_f64") == 0 ||
      strcmp(name, "std.os.ui.render.get_frame_time") == 0 ||
      strcmp(name, "std.os.ui.render.set_ortho_2d") == 0 ||
      strcmp(name, "std.os.ui.render.draw_rect") == 0 ||
      strcmp(name, "std.os.ui.render.draw_circle") == 0 ||
      strcmp(name, "std.os.ui.render.draw_text_centered") == 0 ||
      strcmp(name, "std.os.ui.render.end_frame") == 0 ||
      strcmp(name, "std.os.ui.window.set_should_close") == 0 ||
      strcmp(name, "std.os.ui.window.input.key_down") == 0 ||
      strcmp(leaf, "str_replace") == 0 ||
      strcmp(leaf, "dict") == 0 ||
      strcmp(leaf, "to_str") == 0 ||
      strcmp(leaf, "__to_str") == 0 ||
      strcmp(leaf, "key_down") == 0 ||
      strcmp(leaf, "abs") == 0 ||
      strcmp(leaf, "clamp") == 0 ||
      strcmp(leaf, "lerp") == 0 ||
      strcmp(leaf, "min") == 0 ||
      strcmp(leaf, "max") == 0 ||
      strcmp(leaf, "prove") == 0 ||
      strcmp(leaf, "static_assert") == 0 ||
      strcmp(leaf, "panic") == 0 ||
      strcmp(leaf, "memchr") == 0 ||
      strcmp(leaf, "memcmp") == 0 ||
      strcmp(leaf, "memcpy") == 0 ||
      strcmp(leaf, "memmove") == 0 ||
      strcmp(leaf, "memset") == 0 ||
      strcmp(leaf, "strchr") == 0 ||
      strcmp(leaf, "strcmp") == 0)
    return true;
  if (strstr(name, ".get_size") || strstr(name, ".get_pos") ||
      strstr(name, ".get_cursor_pos") || strstr(name, ".get_key_state") ||
      strstr(name, ".get_mouse_button_state"))
    return true;
  if (strstr(name, "x11_backend.") || strstr(name, "win32_impl.") ||
      strstr(name, "cocoa_impl.") || strstr(name, "wayland_backend.")) {
    return strstr(name, ".get_monitors") ||
           strstr(name, ".get_video_modes") ||
           strstr(name, ".get_primary_monitor") ||
           strstr(name, ".get_window_monitor") ||
           strstr(name, ".get_video_mode") ||
           strstr(name, ".create_cursor") ||
           strstr(name, ".create_standard_cursor") ||
           strstr(name, ".get_gamma_ramp") ||
           strstr(name, ".get_monitor_pos") ||
           strstr(name, ".get_monitor_physical_size") ||
           strstr(name, ".get_window_content_scale") ||
           strstr(name, ".get_monitor_content_scale") ||
           strstr(name, ".get_key_scancode") ||
           strstr(name, ".get_key_name") ||
           strstr(name, ".get_clipboard") ||
           strstr(name, ".get_primary_selection") ||
           strstr(name, ".set_pos") ||
           strstr(name, ".set_size") ||
           strstr(name, ".set_title") ||
           strstr(name, ".set_cursor_pos") ||
           strstr(name, ".set_input_mode") ||
           strstr(name, ".set_window_") ||
           strstr(name, ".show_window") ||
           strstr(name, ".hide_window") ||
           strstr(name, ".focus_window") ||
           strstr(name, ".post_empty_event");
  }
  return false;
}

static void ny_native_add_reachable_fn(ny_native_fn_collector_t *col,
                                       const char *name) {
  if (!col || !name || !*name)
    return;
  const stmt_t *fn = ny_native_nir_find_user_function_in_prog(col->prog, name);
  if (!fn || fn->as.fn.is_extern || fn->as.fn.link_name)
    return;
  /*
   * Native leaf / builtin-alloc names (print, addr_of, borrow, float,
   * malloc, free, ...) are lowered directly to runtime calls in the
   * shared-NYIR path, so their stdlib bodies must not be force-built:
   * those bodies use NY_E_LIST / NY_E_MEMBER constructs the shared lowerer
   * does not support yet.  User-defined shadowing functions are collected
   * normally (they are user code, not stdlib).
   */
  const char *leaf = ny_native_leaf_name(name);
  if (ny_is_stdlib_tok(fn->tok) &&
      (ny_native_leaf_kind(leaf) != NY_NATIVE_LEAF_NONE ||
       ny_builtin_alloc_kind(leaf) != NY_BUILTIN_ALLOC_NONE ||
       ny_native_nir_stdlib_call_lowered_inline(name)))
    return;
  for (size_t i = 0; i < col->count; ++i) {
    if (col->funcs[i] == fn ||
        (col->funcs[i]->as.fn.name && fn->as.fn.name &&
         strcmp(col->funcs[i]->as.fn.name, fn->as.fn.name) == 0))
      return;
  }
  if (col->count < col->max_funcs) {
    if (getenv("NY_TRACE_NATIVE_REACHABLE"))
      fprintf(stderr, "native reachable: %s\n", name);
    col->funcs[col->count++] = fn;
  }
}

static void ny_native_scan_expr_for_calls(const expr_t *e,
                                          ny_native_fn_collector_t *col) {
  if (!e || !col)
    return;
  switch (e->kind) {
  case NY_E_IDENT:
    break;
  case NY_E_CALL: {
    const char *callee_leaf = NULL;
    if (e->as.call.callee) {
      if (e->as.call.callee->kind == NY_E_IDENT) {
        const char *callee_name = e->as.call.callee->as.ident.name;
        callee_leaf = ny_native_leaf_name(callee_name);
        if (col->query_fn && callee_name &&
            strcmp(callee_name, col->query_fn) == 0 &&
            col->query_param < e->as.call.args.len &&
            ny_native_nir_expr_has_dyn_list_shape_in_scope(
                col->prog, col->query_scope,
                e->as.call.args.data[col->query_param].val, 0))
          col->query_dyn_list = true;
        ny_native_add_reachable_fn(col, callee_name);
      } else if (e->as.call.callee->kind == NY_E_MEMBER) {
        bool qualified = ny_native_scan_qualified_call(
            col, e->as.call.callee->as.member.target,
            e->as.call.callee->as.member.name);
        if (!qualified)
          ny_native_add_reachable_fn(col, e->as.call.callee->as.member.name);
      }
      ny_native_scan_expr_for_calls(e->as.call.callee, col);
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      const expr_t *arg = e->as.call.args.data[i].val;
      if (i == 0 && callee_leaf &&
          (strcmp(callee_leaf, "thread_spawn") == 0 ||
           strcmp(callee_leaf, "thread_spawn_call") == 0 ||
           strcmp(callee_leaf, "thread_launch") == 0 ||
           strcmp(callee_leaf, "thread_launch_call") == 0) &&
          arg && arg->kind == NY_E_IDENT && arg->as.ident.name)
        ny_native_add_reachable_fn(col, arg->as.ident.name);
      ny_native_scan_expr_for_calls(arg, col);
    }
    break;
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name) {
      bool qualified = ny_native_scan_qualified_call(
          col, e->as.memcall.target, e->as.memcall.name);
      bool runtime_method =
          strcmp(e->as.memcall.name, "append") == 0 ||
          strcmp(e->as.memcall.name, "get") == 0 ||
          strcmp(e->as.memcall.name, "set") == 0 ||
          strcmp(e->as.memcall.name, "len") == 0 ||
          strcmp(e->as.memcall.name, "has") == 0 ||
          strcmp(e->as.memcall.name, "contains") == 0 ||
          strcmp(e->as.memcall.name, "exists") == 0 ||
          strcmp(e->as.memcall.name, "delete") == 0 ||
          strcmp(e->as.memcall.name, "remove") == 0;
      if (!qualified && !runtime_method)
        ny_native_add_reachable_fn(col, e->as.memcall.name);
    }
    ny_native_scan_expr_for_calls(e->as.memcall.target, col);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_native_scan_expr_for_calls(e->as.memcall.args.data[i].val, col);
    break;
  case NY_E_MEMBER:
    ny_native_scan_expr_for_calls(e->as.member.target, col);
    break;
  case NY_E_UNARY:
    ny_native_scan_expr_for_calls(e->as.unary.right, col);
    break;
  case NY_E_BINARY:
    ny_native_scan_expr_for_calls(e->as.binary.left, col);
    ny_native_scan_expr_for_calls(e->as.binary.right, col);
    break;
  case NY_E_LOGICAL:
    ny_native_scan_expr_for_calls(e->as.logical.left, col);
    ny_native_scan_expr_for_calls(e->as.logical.right, col);
    break;
  case NY_E_TERNARY:
    ny_native_scan_expr_for_calls(e->as.ternary.cond, col);
    ny_native_scan_expr_for_calls(e->as.ternary.true_expr, col);
    ny_native_scan_expr_for_calls(e->as.ternary.false_expr, col);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      ny_native_scan_expr_for_calls(e->as.list_like.data[i], col);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      ny_native_scan_expr_for_calls(e->as.dict.pairs.data[i].key, col);
      ny_native_scan_expr_for_calls(e->as.dict.pairs.data[i].value, col);
    }
    break;
  case NY_E_INDEX:
    ny_native_scan_expr_for_calls(e->as.index.target, col);
    ny_native_scan_expr_for_calls(e->as.index.start, col);
    ny_native_scan_expr_for_calls(e->as.index.stop, col);
    ny_native_scan_expr_for_calls(e->as.index.step, col);
    break;
  case NY_E_MATCH:
    ny_native_scan_expr_for_calls(e->as.match.test, col);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      for (size_t p = 0; p < e->as.match.arms.data[i].patterns.len; ++p)
        ny_native_scan_expr_for_calls(
            e->as.match.arms.data[i].patterns.data[p], col);
      ny_native_scan_expr_for_calls(e->as.match.arms.data[i].guard, col);
      ny_native_scan_stmt_for_calls(e->as.match.arms.data[i].conseq, col);
    }
    ny_native_scan_stmt_for_calls(e->as.match.default_conseq, col);
    break;
  case NY_E_COMPTIME:
    ny_native_scan_stmt_for_calls(e->as.comptime_expr.body, col);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    ny_native_scan_stmt_for_calls(e->as.lambda.body, col);
    break;
  default:
    break;
  }
}
static bool ny_native_nir_function_param_is_dyn_list(const program_t *prog,
                                                     const stmt_t *fn,
                                                     size_t param_index) {
  if (!prog || !fn || fn->kind != NY_S_FUNC || !fn->as.fn.name)
    return false;
  ny_native_fn_collector_t query = {
      .prog = prog,
      .query_fn = fn->as.fn.name,
      .query_param = param_index,
  };
  for (size_t i = 0; i < prog->body.len; ++i) {
    query.query_scope = prog->body.data[i];
    ny_native_scan_stmt_for_calls(prog->body.data[i], &query);
  }
  return query.query_dyn_list;
}

static void ny_native_scan_stmt_for_calls(const stmt_t *s,
                                          ny_native_fn_collector_t *col) {
  if (!s || !col)
    return;
  switch (s->kind) {
  case NY_S_EXPR:
    ny_native_scan_expr_for_calls(s->as.expr.expr, col);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      ny_native_scan_expr_for_calls(s->as.var.exprs.data[i], col);
    break;
  case NY_S_RETURN:
    ny_native_scan_expr_for_calls(s->as.ret.value, col);
    break;
  case NY_S_IF: {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        ny_native_target_eval_bool(col->opt, s->as.iff.test, &selected)) {
      ny_native_scan_stmt_for_calls(
          selected ? s->as.iff.conseq : s->as.iff.alt, col);
      break;
    }
    ny_native_scan_expr_for_calls(s->as.iff.test, col);
    ny_native_scan_stmt_for_calls(s->as.iff.conseq, col);
    ny_native_scan_stmt_for_calls(s->as.iff.alt, col);
    break;
  }
  case NY_S_WHILE:
    ny_native_scan_expr_for_calls(s->as.whl.test, col);
    ny_native_scan_stmt_for_calls(s->as.whl.body, col);
    break;
  case NY_S_FOR:
    ny_native_scan_expr_for_calls(s->as.fr.iterable, col);
    ny_native_scan_expr_for_calls(s->as.fr.cond, col);
    ny_native_scan_stmt_for_calls(s->as.fr.init, col);
    ny_native_scan_stmt_for_calls(s->as.fr.update, col);
    ny_native_scan_stmt_for_calls(s->as.fr.body, col);
    break;
  case NY_S_MATCH:
    ny_native_scan_expr_for_calls(s->as.match.test, col);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      for (size_t p = 0; p < s->as.match.arms.data[i].patterns.len; ++p)
        ny_native_scan_expr_for_calls(
            s->as.match.arms.data[i].patterns.data[p], col);
      ny_native_scan_expr_for_calls(s->as.match.arms.data[i].guard, col);
      ny_native_scan_stmt_for_calls(s->as.match.arms.data[i].conseq, col);
    }
    ny_native_scan_stmt_for_calls(s->as.match.default_conseq, col);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_scan_stmt_for_calls(s->as.block.body.data[i], col);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_scan_stmt_for_calls(s->as.module.body.data[i], col);
    break;
  case NY_S_FUNC:
    ny_native_scan_stmt_for_calls(s->as.fn.body, col);
    break;
  default:
    break;
  }
}

static size_t ny_native_collect_reachable_fns(const program_t *prog,
                                              const ny_options *opt,
                                              const stmt_t **out_funcs,
                                              size_t max_funcs) {
  if (!prog || !out_funcs || max_funcs == 0)
    return 0;
  ny_native_fn_collector_t col = {
      .prog = prog,
      .opt = opt,
      .funcs = out_funcs,
      .max_funcs = max_funcs,
  };

  /*
   * Seed only from executable top-level statements below.  Pre-seeding every
   * user function consumes the bounded native function/object tables with
   * dead helpers before the actual entry-point call graph is traversed.
   */

  /*
   * 2. Scan top-level user statements in prog->body for calls.  Skipping
   * stdlib-token statements is what keeps the whole standard library out of
   * the native build: prog->body contains every stdlib module body, and
   * scanning them here would force-build the full stdlib (borrow of unary,
   * store.local dominance, ... fail in the shared-NYIR lowerer).  Stdlib
   * helpers are pulled in only when user code actually calls them.
   */
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind != NY_S_FUNC && !ny_is_stdlib_tok(s->tok))
      ny_native_scan_stmt_for_calls(s, &col);
  }

  /*
   * 1b. Seed the entry point function so that programs consisting entirely
   * of function definitions (no top-level statements) still have their
   * call graph traversed.  Without this, `fn main() { ... }` programs
   * produce zero reachable functions, leaving the inline callee table
   * empty and forcing every call through the slow ABI path.
   */
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
        (strcmp(s->as.fn.name, "main") == 0 ||
         strcmp(s->as.fn.name, "entry") == 0)) {
      ny_native_add_reachable_fn(&col, s->as.fn.name);
      break;
    }
  }

  /*
   * 3. Transitive worklist loop over collected functions
   */
  size_t idx = 0;
  while (idx < col.count) {
    const stmt_t *fn = col.funcs[idx++];
    ny_native_scan_stmt_for_calls(fn->as.fn.body, &col);
  }

  return col.count;
}

/*
 * Interprocedural effect inference for user-function calls.
 *
 * The runtime-symbol table (nyir_call_effect_summary) classifies known
 * rt_* helpers but reports CALL|FFI|UNKNOWN_SIDE_EFFECT for every user
 * function, which makes LICM (and DCE) treat all user calls as opaque.
 * After all reachable function bodies are built and optimized, we compute
 * the true observable effect set of each body -- including what its own
 * callees do, iterated to a fixed point for recursion -- and tag the CALL
 * instructions with that summary (NYIR_INST_F_EFFECTS_KNOWN).  A function
 * whose body (and transitively its callees) only reads its own frame and
 * computes arithmetic is then hoistable out of loops, which is exactly the
 * gap that made recursive pure helpers like gcbench's tree_count stay
 * inside hot loops while C hoists them.
 *
 * Only caller-observable effects are tracked: a function's own
 * READ_LOCAL/WRITE_LOCAL (private frame) and CONTROL (internal branches)
 * are invisible to the caller and stripped from the summary.
 */

static unsigned ny_native_nir_observable_effects(
    const nyir_inst_t *in, const char **names, size_t count,
    const unsigned *summaries) {
  if (!in)
    return 0;
  unsigned e;
  if (in->op == NYIR_CALL) {
    if (in->symbol && names && count) {
      for (size_t i = 0; i < count; ++i) {
        if (names[i] && strcmp(names[i], in->symbol) == 0)
          return summaries[i]; /* already includes NYIR_EFFECT_CALL */
      }
    }
    e = nyir_call_effect_summary(in);
  } else {
    e = in->effects | nyir_inst_effects(in);
  }
  e &= ~(NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL |
         NYIR_EFFECT_CONTROL);
  return e;
}

static void ny_native_nir_compute_call_effects(nyir_func_t *funcs,
                                               const char **names,
                                               size_t count,
                                               unsigned *summaries) {
  if (!funcs || !names || !summaries || count == 0)
    return;
  for (size_t i = 0; i < count; ++i)
    summaries[i] = 0;
  /*
   * Gauss-Seidel fixed point over the call graph.  Summaries only grow
   * (OR), so starting from zero and iterating to a fixpoint is sound and
   * terminates; recursion keeps a self-call's contribution fixed after the
   * first pass because its summary is already folded in.
   */
  size_t max_iters = count + 2;
  for (size_t iter = 0; iter < max_iters; ++iter) {
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
      unsigned s = 0;
      for (size_t j = 0; j < funcs[i].len; ++j)
        s |= ny_native_nir_observable_effects(&funcs[i].data[j], names,
                                              count, summaries);
      if (s != summaries[i]) {
        summaries[i] = s;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
}

/*
 * Attach the inferred summary to calls of user functions whose effects are
 * provably pure (no writes, allocation, IO, threads, FFI, or unknown
 * behavior).  Such calls become LICM candidates and result-unused DCE
 * candidates, matching how C compilers treat pure helpers.
 */
static void ny_native_nir_patch_call_effects(
    nyir_func_t *f, const char **names, size_t count,
    const unsigned *summaries) {
  if (!f || !names || !summaries || count == 0)
    return;
  for (size_t j = 0; j < f->len; ++j) {
    nyir_inst_t *in = &f->data[j];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    for (size_t k = 0; k < count; ++k) {
      if (!names[k] || strcmp(names[k], in->symbol) != 0)
        continue;
      unsigned s = summaries[k];
      const unsigned harmless = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY |
                                NYIR_EFFECT_MAY_TRAP;
      if ((s & ~harmless) == 0) {
        in->effects = s | NYIR_EFFECT_CALL;
        in->flags |= NYIR_INST_F_EFFECTS_KNOWN;
        if (getenv("NY_TRACE_PURE_CALLS"))
          fprintf(stderr, "pure-call: %s tagged effects=0x%x\n",
                  in->symbol, (unsigned)in->effects);
      } else if (getenv("NY_TRACE_PURE_CALLS")) {
        fprintf(stderr, "pure-call: %s NOT tagged effects=0x%x\n",
                in->symbol, (unsigned)s);
      }
      break;
    }
  }
}

bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         nyir_func_t *rt_main_out,
                         nyir_func_t *funcs_out, size_t *func_count,
                         const char **func_names_out, size_t max_funcs,
                         char *err, size_t err_len) {
  if (!prog || !rt_main_out)
    return false;
  ny_native_fn_cache_build(prog);
  ny_native_strtab_clear();
  ny_native_consttab_clear();
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_register_const_defs(prog, prog->body.data[i]);
  ny_native_profile_clear();
  int opt_level = opt ? opt->opt_level : 1;
  /*
   * Native tiers select real NYIR optimization pipelines.  Previously the
   * tier name only changed the report label and budgets, so cache entries and
   * emitted code were identical across explicit tiers.
   */
  if (opt) {
    switch (opt->native_tier) {
    case NY_NATIVE_TIER_BASELINE:
      opt_level = 0;
      break;
    case NY_NATIVE_TIER_STENCIL:
      opt_level = 1;
      break;
    case NY_NATIVE_TIER_FAST:
      opt_level = 2;
      break;
    case NY_NATIVE_TIER_OPT:
      opt_level = 3;
      break;
    case NY_NATIVE_TIER_AUTO:
    case NY_NATIVE_TIER_LLVM:
    default:
      break;
    }
  }
  if (opt && opt->llvm_lto && opt_level < 3)
    opt_level = 3;
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                           opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  char profile_err[256] = {0};
  if (opt && opt->native_profile_use_path &&
      !ny_native_profile_load_path(opt->native_profile_use_path, profile_err,
                                   sizeof(profile_err))) {
    ny_native_set_err(err, err_len, "native PGO: %s", profile_err);
    return false;
  }
  memset(rt_main_out, 0, sizeof(*rt_main_out));
  if (func_count)
    *func_count = 0;
  if (func_names_out)
    memset((void *)func_names_out, 0, max_funcs * sizeof(*func_names_out));

  /*
   * Build extern table from #include and extern statements.
   */
  ny_extern_table_t externs;
  bool aapcs = opt &&
               (opt->native_backend == NY_NATIVE_BACKEND_AARCH64 ||
                opt->native_abi == NY_NATIVE_ABI_AAPCS);
  char extern_err[256] = {0};
  if (!ny_native_nir_build_extern_table(prog, &externs, aapcs, opt, extern_err,
                                        sizeof(extern_err))) {
    ny_native_set_err(err, err_len, "NYIR extern: %s", extern_err);
    ny_extern_table_free(&externs);
    ny_native_profile_clear();
    return false;
  }

  /*
   * Collect and build reachable non-extern Nytrix functions found anywhere
   * in the program.  This lets rt_main call stdlib helpers like key_down
   * that live inside module bodies without exceeding max_funcs capacity.
   */
  const stmt_t **all_fns = NULL;
  size_t all_fn_count = 0;
  const char **func_names = NULL;
  if (funcs_out && func_count && max_funcs > 0) {
    all_fns = calloc(max_funcs, sizeof(*all_fns));
    func_names = calloc(max_funcs, sizeof(*func_names));
    if (!all_fns || !func_names) {
      free(all_fns);
      free(func_names);
      ny_native_set_err(err, err_len, NY_NATIVE_ALLOC_FAIL);
      ny_extern_table_free(&externs);
      ny_native_profile_clear();
      return false;
    }
    all_fn_count = ny_native_collect_reachable_fns(prog, opt, all_fns, max_funcs);
    if (getenv("NY_TRACE_NATIVE_REACHABLE"))
      fprintf(stderr, "native reachable functions: %zu (capacity %zu)\n", all_fn_count,
              max_funcs);
    size_t count = 0;
    for (size_t i = 0; i < all_fn_count; ++i) {
      const stmt_t *s = all_fns[i];
      if (!s)
        continue;
      char local_err[256] = {0};
      if (!ny_native_nir_build_function(prog, s, &funcs_out[count], local_err,
                                       sizeof(local_err), opt_level, opt)) {
        /*
         * Lowering failure: free already-built functions.
         */
        for (size_t j = 0; j < count; ++j)
          nyir_func_free(&funcs_out[j]);
        *func_count = 0;
        if (err && err_len > 0 && local_err[0])
          ny_native_set_err(err, err_len, "%s", local_err);
        if (!opt || (!opt->native_dump_ir_path && !opt->nyir_dump_bin_path)) {
          free(all_fns);
          free(func_names);
          ny_native_profile_clear();
          return false;
        }
      } else {
        func_names[count] = s->as.fn.name ? s->as.fn.name : "<fn>";
        if (func_names_out)
          func_names_out[count] = func_names[count];
        count++;
      }
    }
    *func_count = count;
  }

  /*
   * Offer user functions as inlining candidates for rt_main.
   */
  static nyir_inline_callee_t inline_callees[128];
  size_t inline_count = 0;
  if (funcs_out && func_count && *func_count > 0) {
    for (size_t i = 0; i < *func_count && inline_count < 128; ++i) {
      if (!func_names[i])
        continue;
      inline_callees[inline_count].name = func_names[i];
      inline_callees[inline_count].func = &funcs_out[i];
      inline_count++;
    }
    nyir_set_inline_callees(inline_callees, inline_count);
    /*
     * Function bodies are lowered before the candidate table is available.
     * Give already-built bodies one small-inlining sweep now, so wrappers
     * such as chain(add(...), mul(...)) become straight-line candidates for
     * rt_main's general inliner.
     */
    for (size_t i = 0; i < *func_count; ++i) {
      (void)nyir_mem2reg(&funcs_out[i]);
    }
    for (size_t i = 0; i < *func_count; ++i) {
      if (nyir_inline_small(&funcs_out[i]) &&
          nyir_mem2reg(&funcs_out[i]) &&
          nyir_copy_prop(&funcs_out[i]) &&
          nyir_dce(&funcs_out[i]) &&
          nyir_compact(&funcs_out[i])) {
        nyir_refresh_metadata(&funcs_out[i]);
        continue;
      }
      for (size_t j = 0; j < *func_count; ++j)
        nyir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyir_set_inline_callees(NULL, 0);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      ny_native_set_err(err, err_len, NY_NATIVE_ALLOC_FAIL);
      return false;
    }
    /*
     * Inline larger pure wrappers after leaf calls are folded.  This second
     * sweep is what turns a call chain into one direct arithmetic body; the
     * general inliner still enforces its existing size, ABI, and CFG guards.
     */
    for (size_t i = 0; i < *func_count; ++i) {
      if (nyir_inline_general(&funcs_out[i])) {
        nyir_refresh_metadata(&funcs_out[i]);
        continue;
      }
      for (size_t j = 0; j < *func_count; ++j)
        nyir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyir_set_inline_callees(NULL, 0);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      ny_native_set_err(err, err_len, NY_NATIVE_ALLOC_FAIL);
      return false;
    }
    /*
     * Function bodies were initially finalized before the callee registry was
     * available, so post-build inlining can expose new loops, bounds checks,
     * scalar-replacement opportunities, and vectorizable arithmetic after the
     * original O2 pipeline has already run.  Re-run the normal optimizer now
     * while the monomorphic callee table is live.  This makes optimization
     * ordering intentional instead of leaving inlined wrapper bodies in a
     * half-optimized state when rt_main later clones them into hot loops.
     */
    for (size_t i = 0; i < *func_count; ++i) {
      bool opt_ok = (opt && (opt->nyir_pass_stats || opt->nyir_verify))
                        ? nyir_optimize_debug(&funcs_out[i], stderr, NULL, opt_level)
                        : nyir_optimize(&funcs_out[i], opt_level);
      if (opt_ok) {
        nyir_refresh_metadata(&funcs_out[i]);
        continue;
      }
      for (size_t j = 0; j < *func_count; ++j)
        nyir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyir_set_inline_callees(NULL, 0);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      ny_native_set_err(err, err_len,
                        "native NYIR: post-inline optimization failed");
      return false;
    }
  }

  /*
   * Bodies are now final (dead allocations, stores, and wrappers have been
   * eliminated), so their observable effect sets are trustworthy.  Infer
   * them and tag user-function calls so LICM can hoist provably pure calls
   * (recursive helpers included) out of rt_main's hot loops.
   */
  unsigned *call_effect_summaries = NULL;
  if (funcs_out && func_count && *func_count > 0) {
    call_effect_summaries =
        calloc(*func_count, sizeof(*call_effect_summaries));
    if (!call_effect_summaries) {
      ny_native_set_err(err, err_len, NY_NATIVE_ALLOC_FAIL);
      for (size_t j = 0; j < *func_count; ++j)
        nyir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyir_set_inline_callees(NULL, 0);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      return false;
    }
    ny_native_nir_compute_call_effects(funcs_out, func_names, *func_count,
                                       call_effect_summaries);
    /*
     * Tag inside the collected bodies too: any call inlined from them into
     * rt_main later keeps its trusted effect summary.
     */
    for (size_t i = 0; i < *func_count; ++i)
      ny_native_nir_patch_call_effects(&funcs_out[i], func_names,
                                       *func_count, call_effect_summaries);
  }

  /*
   * Build rt_main with extern table.  When requested, run the VM/native result
   * oracle after every optimization pass on rt_main so any pass-level bug is
   * caught before final codegen.
   */
  ny_native_oracle_ctx_t oracle_ctx = {opt, funcs_out, (const char **)func_names,
                                       func_count ? *func_count : 0};
  if (opt && opt->native_oracle_per_pass && funcs_out && func_count)
    nyir_set_per_pass_oracle(true, ny_native_per_pass_oracle_cb, &oracle_ctx);
  bool ok = ny_native_nir_build_rt_main(prog, rt_main_out, &externs, err, err_len,
                                        opt_level, opt);
  nyir_set_per_pass_oracle(false, NULL, NULL);
  /*
   * Tag rt_main's direct calls to pure user functions before the first
   * optimization pass, so loop-invariant pure calls (e.g. tree_count(4))
   * are hoisted by LICM instead of re-executed every iteration.
   */
  if (ok && call_effect_summaries && func_count && *func_count > 0)
    ny_native_nir_patch_call_effects(rt_main_out, func_names, *func_count,
                                     call_effect_summaries);
  /*
   * Run the full optimization pipeline on rt_main to enable vectorization,
   * loop optimizations, and other transforms before inlining.
   */
  if (ok)
    ok = nyir_optimize(rt_main_out, opt_level);
  /*
   * The general inliner deliberately rejects callees that still carry local
   * stores.  The small pure-body splice can remap those private slots safely,
   * so give rt_main the same leaf-wrapper sweep already used for collected
   * user functions.  This is the hot call-chain path: it removes the
   * rt_main -> wrapper call without changing the externally visible ABI.
   */
  if (ok && inline_count > 0) {
    if (!nyir_inline_small(rt_main_out) ||
        !nyir_inline_general(rt_main_out) ||
        !nyir_optimize(rt_main_out, opt_level)) {
      ny_native_set_err(err, err_len,
                        "native NYIR: post-inline rt_main optimization failed");
      ok = false;
    } else {
      /*
       * rt_main is optimized once before this explicit wrapper sweep.  A
       * second full pipeline is intentional: inlining can expose entire
       * counted loops and managed-buffer accesses that the first pass could
       * not see through a call boundary.  Scalar-only cleanup left those hot
       * regions unvectorized and with stale BCE/LICM opportunities.
       */
      nyir_refresh_metadata(rt_main_out);
    }
  }
  nyir_set_inline_callees(NULL, 0);
  nyir_set_tv_seed(0);

  /*
   * Inlining passes (including mem2reg) may have introduced PHIs in user
   * functions and rt_main.  For backends that don't preserve PHIs (i386,
   * portable, etc.), eliminate them now before codegen.
   */
  if (!nyir_get_preserve_phis()) {
    for (size_t i = 0; funcs_out && i < *func_count; ++i) {
      if (!nyir_phi_elim(&funcs_out[i])) {
        if (err && err_len > 0 && err[0] == '\0')
          snprintf(err, err_len, "native NYIR: phi elimination failed");
        ok = false;
        break;
      }
      nyir_refresh_metadata(&funcs_out[i]);
    }
    if (ok && rt_main_out) {
      if (!nyir_phi_elim(rt_main_out)) {
        if (err && err_len > 0 && err[0] == '\0')
          snprintf(err, err_len, "native NYIR: phi elimination failed");
        ok = false;
      }
      nyir_refresh_metadata(rt_main_out);
    }
  }

  free(call_effect_summaries);
  ny_extern_table_free(&externs);
  ny_native_profile_clear();
  free(all_fns);
  free(func_names);
  return ok;
}

bool ny_native_nir_dump_function(FILE *out, const program_t *prog,
                                 const stmt_t *fn, char *err,
                                 size_t err_len, const ny_options *opt) {
  if (!fn || fn->kind != NY_S_FUNC)
    return true;
  ny_native_fn_cache_build(prog);
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                         opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_native_nir_builder_t b = {.last_value = -1,
                               .err = err,
                               .err_len = err_len,
                               .prog = prog,
                               .options = opt,
                               .current_fn_name = fn->as.fn.name,
                               .opt_level = opt ? opt->opt_level : 1};
  if (ny_native_type_name_is_f64(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F64;
  else if (ny_native_type_name_is_f32(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F32;
  if (!ny_native_nir_set_param_types(&b, fn)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    ny_native_nir_local_t *param = ny_native_nir_bind_local_typed(
        &b, fn->as.fn.params.data[i].name,
        ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
        ny_native_type_name_is_f32(fn->as.fn.params.data[i].type),
        fn->as.fn.params.data[i].type &&
            strcmp(fn->as.fn.params.data[i].type, "str") == 0);
    if (!param) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    param->is_any = ny_native_type_name_is_any(fn->as.fn.params.data[i].type);
    param->is_list =
        ny_native_type_name_is_list(fn->as.fn.params.data[i].type);
    param->is_dyn_list =
        param->is_list &&
        ny_native_type_name_is_dyn_list(fn->as.fn.params.data[i].type);
    if (param->is_list)
      param->list_len_slot = b.next_local_slot++;
    else if (param->is_cstr || param->is_any) {
      param->dyn_str_len_slot = b.next_local_slot++;
      param->dyn_tag_slot = b.next_local_slot++;
    }
    param->fin_bound =
        ny_native_parse_fin_bound(fn->as.fn.params.data[i].type);
  }
  b.current_fn_name = fn->as.fn.name;
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  if (ok && !b.emitted_return) {
    /*
     * Capture the implicit return value before defer bodies run: defer
     * statements lower as expression statements and would clobber
     * last_value.
     */
    int ret = b.last_value >= 0 ? b.last_value
                                : ny_native_nir_emit_const(&b, 0);
    ok = ret >= 0 && ny_native_nir_emit_defers(&b, 0);
    if (ok)
      ok = ny_native_nir_emit_ret(&b, ret);
  }
  if (ok)
    ok = ny_native_nir_opt_dump(out, &b,
                                fn->as.fn.name ? fn->as.fn.name : "<fn>", opt);
  nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  return ok;
}

bool ny_native_nir_dump_rt_main(FILE *out, const program_t *prog, char *err,
                                size_t err_len, const ny_options *opt) {
  nyir_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyir_set_pass_controls(opt ? opt->nyir_disable_pass : NULL,
                           opt ? opt->nyir_stop_after : NULL);
  nyir_set_verify_each_pass(opt && opt->nyir_verify);
  nyir_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_extern_table_t externs;
  bool aapcs = opt &&
               (opt->native_backend == NY_NATIVE_BACKEND_AARCH64 ||
                opt->native_abi == NY_NATIVE_ABI_AAPCS);
  if (!ny_native_nir_build_extern_table(prog, &externs, aapcs, opt, err, err_len)) {
    ny_extern_table_free(&externs);
    return false;
  }
  ny_native_nir_builder_t b = {.last_value = -1,
                               .err = err,
                               .err_len = err_len,
                               .externs = &externs,
                               .prog = prog,
                               .options = opt,
                               .profile_name = "rt_main",
                               .current_fn_name = "rt_main",
                               .opt_level = opt ? opt->opt_level : 1};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR dump unavailable: program has no raw expression result");
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
  }
  bool ok = ny_native_nir_opt_dump(out, &b, "rt_main", opt);
  nyir_func_free(&b.nyir);
  ny_native_nir_builder_dispose(&b);
  ny_extern_table_free(&externs);
  return ok;
}

size_t ny_native_nir_local_count(const nyir_func_t *f);
bool ny_native_ensure_parent_dir_for_path(const char *path);

static bool ny_native_nir_write_u16le(FILE *out, uint16_t value) {
  unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
  return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

static bool ny_native_nir_write_u32le(FILE *out, uint32_t value) {
  unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                            (unsigned char)(value >> 16), (unsigned char)(value >> 24)};
  return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

static bool ny_native_nir_write_binary_blob(FILE *out, const nyir_func_t *nyir,
                                            const char *name, char *err,
                                            size_t err_len) {
  FILE *tmp = tmpfile();
  if (!tmp)
    return ny_native_set_err(err, err_len, "native NYIR bundle: failed to create temporary blob"), false;
  bool ok = nyir_dump_binary(tmp, nyir, name);
  long bytes = ok && fflush(tmp) == 0 && fseek(tmp, 0, SEEK_END) == 0 ? ftell(tmp) : -1;
  if (!ok || bytes < 0 || (unsigned long)bytes > UINT32_MAX || fseek(tmp, 0, SEEK_SET) != 0 ||
      !ny_native_nir_write_u32le(out, (uint32_t)bytes)) {
    fclose(tmp);
    return ny_native_set_err(err, err_len, "native NYIR bundle: failed to encode %s", name), false;
  }
  unsigned char buffer[4096];
  size_t left = (size_t)bytes;
  while (left > 0) {
    size_t want = left < sizeof(buffer) ? left : sizeof(buffer);
    size_t got = fread(buffer, 1, want, tmp);
    if (got != want || fwrite(buffer, 1, got, out) != got) {
      fclose(tmp);
      return ny_native_set_err(err, err_len, "native NYIR bundle: failed to write %s", name), false;
    }
    left -= got;
  }
  fclose(tmp);
  return true;
}

bool ny_native_nir_dump_program_binary(FILE *out, const program_t *prog,
                                       const ny_options *opt, char *err,
                                       size_t err_len) {
  if (!out || !prog)
    return ny_native_set_err(err, err_len, "native NYIR bundle: missing program output"), false;
  enum { NY_NATIVE_NIR_BUNDLE_VERSION = 1 };
  nyir_func_t rt_main = {0};
  size_t wanted = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      wanted++;
  }
  if (wanted > NY_NATIVE_NIR_BUNDLE_MAX_FUNCS)
    return ny_native_set_err(err, err_len,
                             "native NYIR bundle: %zu functions exceed bundle limit %u",
                             wanted, NY_NATIVE_NIR_BUNDLE_MAX_FUNCS), false;
  nyir_func_t *funcs = wanted ? calloc(wanted, sizeof(*funcs)) : NULL;
  const char **names = wanted ? calloc(wanted, sizeof(*names)) : NULL;
  if (wanted && (!funcs || !names)) {
    free(funcs);
    free(names);
    return ny_native_set_err(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  }
  size_t named = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[named++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }
  size_t count = 0;
  bool ok = ny_native_build_nir(prog, opt, &rt_main, funcs, &count,
                                names, wanted, err, err_len);
  if (!ok || count != wanted) {
    if (ok)
      ny_native_set_err(err, err_len,
                        "native NYIR bundle: every user function must lower before serialization");
    ok = false;
    goto done;
  }
  if (fwrite("NYIP", 1, 4, out) != 4 ||
      !ny_native_nir_write_u16le(out, NY_NATIVE_NIR_BUNDLE_VERSION) ||
      !ny_native_nir_write_u16le(out, 0) ||
      !ny_native_nir_write_u32le(out, (uint32_t)(count + 1)) ||
      !ny_native_nir_write_binary_blob(out, &rt_main, "rt_main", err, err_len)) {
    ok = false;
    goto done;
  }
  for (size_t i = 0; i < count; ++i)
    if (!ny_native_nir_write_binary_blob(out, &funcs[i], names[i], err, err_len)) {
      ok = false;
      goto done;
    }
done:
  nyir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    nyir_func_free(&funcs[i]);
  free(funcs);
  free(names);
  return ok;
}

bool ny_native_write_nir_metadata_report(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len) {
  if (!opt || !opt->nyir_metadata_report)
    return true;
  FILE *out = stderr;
  if (opt->nyir_metadata_report_path && opt->nyir_metadata_report_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_metadata_report_path);
    out = fopen(opt->nyir_metadata_report_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native NYIR metadata: failed to open %s: %s",
                        opt->nyir_metadata_report_path, strerror(errno));
      return false;
    }
  }

  if (opt->nyir_metadata_bin_path && opt->nyir_metadata_bin_path[0]) {
    FILE *in = fopen(opt->nyir_metadata_bin_path, "rb");
    if (!in) {
      if (out != stderr)
        fclose(out);
      ny_native_set_err(err, err_len,
                        "native NYIR metadata: failed to open %s: %s",
                        opt->nyir_metadata_bin_path, strerror(errno));
      return false;
    }
    nyir_func_t f = {0};
    char name[128] = {0};
    char local_err[512] = {0};
    bool ok = nyir_load_binary(in, &f, name, sizeof(name), local_err,
                                 sizeof(local_err));
    fclose(in);
    if (ok) {
      nyir_metadata_summary_t summary = {0};
      ok = nyir_metadata_summary(&f, &summary, local_err,
                                   sizeof(local_err));
      if (ok) {
        fprintf(out, "nyir metadata report functions=1 source=binary path=%s\n",
                opt->nyir_metadata_bin_path);
        nyir_metadata_summary_dump(out, name[0] ? name : "rt_main",
                                     &summary);
      }
    }
    nyir_func_free(&f);
    if (out != stderr)
      fclose(out);
    if (!ok) {
      ny_native_set_err(err, err_len, "%s",
                        local_err[0] ? local_err
                                     : "native NYIR binary metadata failed");
      return false;
    }
    if (err && err_len > 0)
      err[0] = '\0';
    return true;
  }

  nyir_func_t rt_main = {0};
  nyir_func_t funcs[NY_NATIVE_LIVE_MAX_FUNCS] = {{0}};
  const char *names[NY_NATIVE_LIVE_MAX_FUNCS] = {0};
  size_t func_count = 0;
  char local_err[512] = {0};
  bool ok = ny_native_build_nir(
      prog, opt, &rt_main, funcs, &func_count, names,
      NY_NATIVE_LIVE_MAX_FUNCS, local_err, sizeof(local_err));

  if (!ok) {
    if (out != stderr)
      fclose(out);
    ny_native_set_err(err, err_len, "%s",
                      local_err[0] ? local_err : "native NYIR build failed");
    return false;
  }

  fprintf(out, "nyir metadata report functions=%zu\n", func_count + 1);
  for (size_t i = 0; i < func_count; ++i) {
    nyir_metadata_summary_t summary = {0};
    if (!nyir_metadata_summary(&funcs[i], &summary, local_err,
                                 sizeof(local_err))) {
      ok = false;
      break;
    }
    nyir_metadata_summary_dump(out, names[i] ? names[i] : "<fn>",
                                 &summary);
  }
  if (ok) {
    nyir_metadata_summary_t summary = {0};
    if (nyir_metadata_summary(&rt_main, &summary, local_err,
                                sizeof(local_err)))
      nyir_metadata_summary_dump(out, "rt_main", &summary);
    else
      ok = false;
  }

  nyir_func_free(&rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&funcs[i]);
  if (out != stderr)
    fclose(out);
  if (!ok) {
    ny_native_set_err(err, err_len, "%s",
                      local_err[0] ? local_err : "native NYIR metadata failed");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
