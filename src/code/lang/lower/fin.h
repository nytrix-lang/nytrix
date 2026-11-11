/*
 * Native lowering: resolve Fin-index bounds for safe buffer-check elision.
 */
/*
 * Check whether an intrinsic call's index operand has a Fin type whose
 * bound allows bounds-check elision.  buffer_byte_len must be > 0 (pass
 * the known allocation / tbuf capacity in bytes; 0 = unknown).
 * Checks both function parameter types and local variable type annotations.
 */
static nyr_local_t *nyr_find_local(nyr_builder_impl_t *b, const char *name);

static int64_t nyr_fin_bound_for_name(const nyr_builder_impl_t *b,
                                      const char *name, unsigned depth);

static int64_t nyr_fin_bound_for_var(const nyr_builder_impl_t *b,
                                     const stmt_t *s, const char *name,
                                     unsigned depth) {
  if (!s || s->kind != NY_S_VAR || !name)
    return 0;
  for (size_t i = 0; i < s->as.var.names.len; ++i) {
    if (!s->as.var.names.data[i] || strcmp(s->as.var.names.data[i], name) != 0)
      continue;
    if (i < s->as.var.types.len) {
      int64_t bound = ny_native_resolve_fin_bound(b, s->as.var.types.data[i]);
      if (bound > 0)
        return bound;
    }
    if (i < s->as.var.exprs.len && s->as.var.exprs.data[i] &&
        s->as.var.exprs.data[i]->kind == NY_E_IDENT)
      return nyr_fin_bound_for_name(b, s->as.var.exprs.data[i]->as.ident.name,
                                    depth + 1);
  }
  return 0;
}

static int64_t nyr_fin_bound_for_name(const nyr_builder_impl_t *b,
                                      const char *name, unsigned depth) {
  if (!b || !name || !name[0] || depth > 16)
    return 0;
  nyr_local_t *local = nyr_find_local((nyr_builder_t *)b, name);
  if (local && local->fin_bound > 0)
    return local->fin_bound;
  if (!b->prog)
    return 0;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (!s)
      continue;
    if (strcmp(b->current_fn_name ? b->current_fn_name : "", "rt_main") == 0 &&
        s->kind == NY_S_VAR) {
      int64_t bound = nyr_fin_bound_for_var(b, s, name, depth);
      if (bound > 0)
        return bound;
    }
    if (s->kind != NY_S_FUNC || !s->as.fn.name ||
        strcmp(s->as.fn.name, b->current_fn_name ? b->current_fn_name : "") !=
            0)
      continue;
    for (size_t pi = 0; pi < s->as.fn.params.len; ++pi) {
      if (s->as.fn.params.data[pi].name &&
          strcmp(s->as.fn.params.data[pi].name, name) == 0)
        return ny_native_resolve_fin_bound(b, s->as.fn.params.data[pi].type);
    }
    if (!s->as.fn.body || s->as.fn.body->kind != NY_S_BLOCK)
      continue;
    for (size_t si = 0; si < s->as.fn.body->as.block.body.len; ++si) {
      const stmt_t *vs = s->as.fn.body->as.block.body.data[si];
      if (!vs || vs->kind != NY_S_VAR)
        continue;
      int64_t bound = nyr_fin_bound_for_var(b, vs, name, depth);
      if (bound > 0)
        return bound;
    }
  }
  return 0;
}

static bool nyr_index_fin_bound_elision(const nyr_builder_impl_t *b,
                                        const expr_t *e, size_t index_arg_pos,
                                        int64_t buffer_byte_len) {
  if (!b || !b->current_fn_name || !e || buffer_byte_len <= 0 ||
      index_arg_pos >= e->as.call.args.len)
    return false;
  const expr_t *idx_expr = e->as.call.args.data[index_arg_pos].val;
  if (!idx_expr)
    return false;
  int64_t scale = 1;
  const char *idx_name = NULL;
  if (idx_expr->kind == NY_E_IDENT) {
    idx_name = idx_expr->as.ident.name;
  } else if (idx_expr->kind == NY_E_BINARY && idx_expr->as.binary.op &&
             strcmp(idx_expr->as.binary.op, "*") == 0) {
    const expr_t *l = idx_expr->as.binary.left;
    const expr_t *r = idx_expr->as.binary.right;
    if (l && l->kind == NY_E_IDENT && r && r->kind == NY_E_LITERAL &&
        r->as.literal.kind == NY_LIT_INT && r->as.literal.as.i > 0) {
      idx_name = l->as.ident.name;
      scale = r->as.literal.as.i;
    } else if (r && r->kind == NY_E_IDENT && l && l->kind == NY_E_LITERAL &&
               l->as.literal.kind == NY_LIT_INT && l->as.literal.as.i > 0) {
      idx_name = r->as.ident.name;
      scale = l->as.literal.as.i;
    }
  }
  int64_t fin_bound = nyr_fin_bound_for_name(b, idx_name, 0);
  return fin_bound > 0 && fin_bound <= INT64_MAX / scale &&
         fin_bound * scale <= buffer_byte_len;
}

/*
 * Resolve the comptime buffer byte length from the first argument of a
 * tbuf intrinsic call (__load64_idx, f64buf_load, etc.).  Returns 0 if
 * the pointer argument isn't a named local with known buffer_byte_len.
 */
static int64_t nyr_resolve_buf_byte_len(const nyr_builder_impl_t *b,
                                        const expr_t *e) {
  if (!b || !e || e->as.call.args.len == 0)
    return 0;
  const expr_t *ptr_arg = e->as.call.args.data[0].val;
  if (!ptr_arg || ptr_arg->kind != NY_E_IDENT)
    return 0;
  nyr_local_t *bl = nyr_find_local((nyr_builder_t *)b, ptr_arg->as.ident.name);
  return (bl && bl->buffer_byte_len > 0) ? bl->buffer_byte_len : 0;
}
