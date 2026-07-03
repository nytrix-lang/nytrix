char *ny_type_pipeline_typed_json(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  ny_tp_json_t j = {0};
  tp_append(
      &j,
      "{\"engine\":\"type-pipeline-v1\",\"schema\":\"typed_ast.v1\","
      "\"notes\":[\"HM principal type facts for the current Ny surface\"],"
      "\"stats\":{\"functions\":%zu,\"mono_candidates\":%zu,\"hm_errors\":%zu,"
      "\"hm_type_nodes\":%zu,\"dynamic_fallbacks\":%zu,\"strict_types\":%s},"
      "\"solver\":",
      ctx.funcs.len, ctx.mono_candidates, ctx.diagnostics.len, hm.type_nodes,
      ctx.fallbacks.len, (cg && cg->strict_types) ? "true" : "false");
  tp_emit_solver_json(&j, &ctx);
  tp_append(&j, ",\"schemes\":");
  hm_emit_schemes_json(&j, &hm);
  tp_append(&j, ",\"functions\":[");
  bool first = true;
  for (size_t i = 0; i < ctx.funcs.len; ++i)
    tp_emit_function_json(&j, &ctx, &ctx.funcs.data[i], &first);
  tp_append(&j, "],\"diagnostics\":");
  tp_emit_diagnostics_json(&j, &ctx);
  tp_append(&j, ",\"hm_fallbacks\":");
  tp_emit_fallbacks_json(&j, &ctx);
  tp_append(&j, ",\"dynamic_fallbacks\":");
  tp_emit_fallbacks_json(&j, &ctx);
  tp_append(&j, "}");
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"engine\":\"type-pipeline-v1\",\"functions\":[]}");
}

static void tp_validate_operator_decls(ny_tp_ctx_t *ctx);
static void tp_validate_impl_coherence(ny_tp_ctx_t *ctx);
static void tp_emit_operator_obligations_stmt(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                              ny_tp_env_t *env, stmt_t *s,
                                              bool *first);
static void tp_validate_abi_program(ny_tp_ctx_t *ctx);

int ny_type_pipeline_validate_hm(program_t *prog, codegen_t *cg,
                                 const char *source_name, bool include_std,
                                 bool emit_diagnostics) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  int count = (int)ctx.diagnostics.len;
  if (emit_diagnostics) {
    for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
      const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
      tp_emit_user_diag(d, "HM validation failed");
    }
  }
  if (emit_diagnostics && ctx.diagnostics.len == 0)
    (void)tp_emit_fallback_warnings(&ctx);
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return count;
}

int ny_type_pipeline_validate_semantics(
    program_t *prog, codegen_t *cg, const char *source_name, bool include_std,
    ny_type_pipeline_stage_t max_stage, ny_type_pipeline_stage_t *failed_stage,
    bool emit_diagnostics, char **errors_json_out) {
  if (failed_stage)
    *failed_stage = NY_TYPE_PIPELINE_STAGE_OK;
  if (errors_json_out)
    *errors_json_out = NULL;
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);

  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  if (ctx.diagnostics.len > 0) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_HM;
    if (emit_diagnostics) {
      for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
        const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
        tp_emit_user_diag(d, "HM validation failed");
      }
    }
    int count = (int)ctx.diagnostics.len;
    if (errors_json_out)
      *errors_json_out = tp_errors_v1_json(&ctx, "hm", source_name,
                                           "HM type validation failed");
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return count;
  }
  if (max_stage <= NY_TYPE_PIPELINE_STAGE_HM) {
    if (emit_diagnostics)
      (void)tp_emit_fallback_warnings(&ctx);
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return 0;
  }

  tp_validate_operator_decls(&ctx);
  tp_validate_impl_coherence(&ctx);
  bool first = true;
  ny_tp_env_t env = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_operator_obligations_stmt(NULL, &ctx, &env, prog->body.data[i],
                                      &first);
  tp_env_dispose(&env);
  if (ctx.diagnostics.len > 0) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_TRAIT;
    if (emit_diagnostics) {
      for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
        const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
        tp_emit_user_diag(d, "trait validation failed");
      }
    }
    int count = (int)ctx.diagnostics.len;
    if (errors_json_out)
      *errors_json_out = tp_errors_v1_json(&ctx, "trait", source_name,
                                           "trait validation failed");
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return count;
  }
  if (max_stage <= NY_TYPE_PIPELINE_STAGE_TRAIT) {
    if (emit_diagnostics)
      (void)tp_emit_fallback_warnings(&ctx);
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return 0;
  }

  tp_validate_abi_program(&ctx);
  if (ctx.diagnostics.len > 0) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_ABI;
    if (emit_diagnostics) {
      for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
        const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
        tp_emit_user_diag(d, "ABI validation failed");
      }
    }
    int count = (int)ctx.diagnostics.len;
    if (errors_json_out)
      *errors_json_out =
          tp_errors_v1_json(&ctx, "abi", source_name, "ABI validation failed");
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return count;
  }

  if (emit_diagnostics)
    (void)tp_emit_fallback_warnings(&ctx);
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return 0;
}

static bool tp_operator_module_active(ny_tp_ctx_t *ctx,
                                      const ny_operator_def_t *def) {
  if (!def || !def->module_name || !*def->module_name)
    return true;
  if (ctx && ctx->cg && ctx->cg->current_module_name &&
      strcmp(ctx->cg->current_module_name, def->module_name) == 0)
    return true;
  return ctx && ny_is_module_active(ctx->cg, def->module_name);
}

static bool tp_type_compatible_for_decl(const char *want, const char *got) {
  if (!want || !*want || !got || !*got)
    return true;
  if (strcmp(want, got) == 0 || strcmp(want, "any") == 0 ||
      strcmp(got, "any") == 0)
    return true;
  const char *w = tp_skip_nullable(want);
  const char *g = tp_skip_nullable(got);
  if (strcmp(w, g) == 0)
    return true;
  if (tp_type_is_group(w) && tp_type_group_accepts(w, g))
    return true;
  if (tp_type_is_group(g) && tp_type_group_accepts(g, w))
    return true;
  if (strcmp(w, "number") == 0 && (tp_is_int_type(g) || tp_is_float_type(g)))
    return true;
  if (strcmp(w, "integer") == 0 && tp_is_int_type(g))
    return true;
  if (strcmp(w, "float") == 0 && tp_is_float_type(g))
    return true;
  if (strcmp(w, "nil") == 0 && (strcmp(g, "nil") == 0 || got[0] == '?'))
    return true;
  return false;
}

static fun_sig *tp_operator_target_sig(ny_tp_ctx_t *ctx,
                                       const ny_operator_def_t *def) {
  return tp_lookup_sig(ctx ? ctx->cg : NULL, def ? def->target_name : NULL);
}

static ny_operator_def_t *tp_find_operator_def(ny_tp_ctx_t *ctx, const char *op,
                                               const char *lt, const char *rt,
                                               size_t *count_out) {
  size_t count = 0;
  ny_operator_def_t *found = NULL;
  if (ctx && ctx->cg && op && lt && rt) {
    for (size_t i = ctx->cg->operators.len; i > 0; --i) {
      ny_operator_def_t *def = &ctx->cg->operators.data[i - 1];
      if (!def->op || strcmp(def->op, op) != 0)
        continue;
      if (!tp_operator_module_active(ctx, def))
        continue;
      if (!tp_type_eq(def->left_type, lt) || !tp_type_eq(def->right_type, rt))
        continue;
      count++;
      if (!found)
        found = def;
    }
  }
  if (count_out)
    *count_out = count;
  return found;
}

static bool tp_operator_has_previous_duplicate(ny_tp_ctx_t *ctx,
                                               const ny_operator_def_t *def) {
  if (!ctx || !ctx->cg || !def)
    return false;
  for (size_t i = 0; i < ctx->cg->operators.len; ++i) {
    ny_operator_def_t *other = &ctx->cg->operators.data[i];
    if (other == def)
      return false;
    if (!tp_stmt_in_scope(ctx, other->stmt))
      continue;
    if (tp_type_eq(other->op, def->op) &&
        tp_type_eq(other->left_type, def->left_type) &&
        tp_type_eq(other->right_type, def->right_type))
      return true;
  }
  return false;
}

static void tp_validate_operator_decl(ny_tp_ctx_t *ctx,
                                      ny_operator_def_t *def) {
  if (!ctx || !def || !tp_stmt_in_scope(ctx, def->stmt))
    return;
  token_t tok = def->stmt ? def->stmt->tok : (token_t){0};
  if (tp_operator_has_previous_duplicate(ctx, def)) {
    tp_add_diag(ctx, tok, "trait", "operator-duplicate",
                "duplicate operator '%s' for %s and %s",
                def->op ? def->op : "?",
                def->left_type ? def->left_type : "unknown",
                def->right_type ? def->right_type : "unknown");
    return;
  }

  fun_sig *target = tp_operator_target_sig(ctx, def);
  if (!target) {
    tp_add_diag(ctx, tok, "trait", "operator-target-missing",
                "operator target '%s' is not defined",
                def->target_name ? def->target_name : "<missing>");
    return;
  }
  if (!target->is_variadic && target->arity != 2) {
    tp_add_diag(ctx, tok, "trait", "operator-target-arity",
                "operator target '%s' must take exactly two arguments",
                def->target_name ? def->target_name : "<missing>");
  }

  const char *left_param = ny_sig_param_type(target, 0);
  const char *right_param = ny_sig_param_type(target, 1);
  if (left_param && !tp_type_compatible_for_decl(left_param, def->left_type)) {
    tp_add_diag(ctx, tok, "trait", "operator-left-mismatch",
                "operator target '%s' expects %s for left operand but operator "
                "declares %s",
                def->target_name ? def->target_name : "<missing>", left_param,
                def->left_type ? def->left_type : "unknown");
  }
  if (right_param &&
      !tp_type_compatible_for_decl(right_param, def->right_type)) {
    tp_add_diag(ctx, tok, "trait", "operator-right-mismatch",
                "operator target '%s' expects %s for right operand but "
                "operator declares %s",
                def->target_name ? def->target_name : "<missing>", right_param,
                def->right_type ? def->right_type : "unknown");
  }

  const char *target_ret = target->return_type && *target->return_type
                               ? target->return_type
                               : target->inferred_return_type;
  if (target_ret && def->return_type && *def->return_type &&
      !tp_type_compatible_for_decl(def->return_type, target_ret)) {
    tp_add_diag(ctx, tok, "trait", "operator-return-mismatch",
                "operator target '%s' returns %s but operator declares %s",
                def->target_name ? def->target_name : "<missing>", target_ret,
                def->return_type);
  }
}

static void tp_validate_operator_decls(ny_tp_ctx_t *ctx) {
  if (!ctx || !ctx->cg)
    return;
  for (size_t i = 0; i < ctx->cg->operators.len; ++i)
    tp_validate_operator_decl(ctx, &ctx->cg->operators.data[i]);
}

typedef struct tp_impl_method_entry_t {
  const char *owner;
  const char *method;
  size_t arity;
  token_t tok;
} tp_impl_method_entry_t;
typedef VEC(tp_impl_method_entry_t) tp_impl_method_entry_list;

static const char *tp_method_tail(const char *name) {
  if (!name)
    return "";
  const char *tail = strrchr(name, '.');
  return tail ? tail + 1 : name;
}

static void tp_impl_coherence_add(ny_tp_ctx_t *ctx,
                                  tp_impl_method_entry_list *entries,
                                  const char *owner, stmt_t *method) {
  if (!ctx || !entries || !owner || !*owner || !method ||
      method->kind != NY_S_FUNC || !tp_stmt_in_scope(ctx, method))
    return;
  const char *name = tp_method_tail(method->as.fn.name);
  size_t arity = method->as.fn.params.len;
  for (size_t i = 0; i < entries->len; ++i) {
    tp_impl_method_entry_t *prev = &entries->data[i];
    if (prev->owner && strcmp(prev->owner, owner) == 0 && prev->method &&
        strcmp(prev->method, name) == 0 && prev->arity == arity) {
      tp_add_diag(
          ctx, method->tok, "trait", "impl-method-duplicate",
          "duplicate impl method '%s.%s/%zu' overlaps a visible impl at %s:%d",
          owner, name, arity,
          prev->tok.filename ? prev->tok.filename : "unknown", prev->tok.line);
      return;
    }
  }
  tp_impl_method_entry_t entry = {
      .owner = owner, .method = name, .arity = arity, .tok = method->tok};
  vec_push(entries, entry);
}

static void tp_validate_impl_coherence_stmt(ny_tp_ctx_t *ctx,
                                            tp_impl_method_entry_list *entries,
                                            stmt_t *s) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      tp_impl_coherence_add(ctx, entries, s->as.impl.type_name,
                            s->as.impl.methods.data[i]);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_validate_impl_coherence_stmt(ctx, entries, s->as.module.body.data[i]);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_validate_impl_coherence_stmt(ctx, entries, s->as.block.body.data[i]);
    break;
  default:
    break;
  }
}

static void tp_validate_impl_coherence(ny_tp_ctx_t *ctx) {
  tp_impl_method_entry_list entries = {0};
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i)
    tp_validate_impl_coherence_stmt(ctx, &entries, ctx->prog->body.data[i]);
  vec_free(&entries);
}

static void tp_emit_group_contract(ny_tp_json_t *j, bool *first,
                                   const char *owner, const char *name,
                                   const char *slot, const char *group,
                                   const char *kind) {
  if (!j || !first || !group || !tp_type_is_group(group))
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"kind\":");
  tp_json_str(j, kind ? kind : "value");
  tp_append(j, ",\"owner\":");
  tp_json_str(j, owner ? owner : "");
  tp_append(j, ",\"name\":");
  tp_json_str(j, name ? name : "");
  tp_append(j, ",\"slot\":");
  tp_json_str(j, slot ? slot : "");
  tp_append(j, ",\"group\":");
  tp_json_str(j, tp_group_canon(group));
  tp_append(j, ",\"capability\":");
  tp_json_str(j, tp_group_capability(group));
  tp_append(j, ",\"zero_cost\":true,\"status\":\"static-conformance\"}");
}

static void tp_emit_group_conformance_json(ny_tp_json_t *j, ny_tp_ctx_t *ctx) {
  bool first = true;
  tp_append(j, "[");
  for (size_t i = 0; ctx && i < ctx->funcs.len; ++i) {
    ny_tp_func_fact_t *fact = &ctx->funcs.data[i];
    stmt_t *fn = fact->stmt;
    if (!fn || fn->kind != NY_S_FUNC || !tp_stmt_in_scope(ctx, fn))
      continue;
    for (size_t k = 0; k < fn->as.fn.params.len; ++k) {
      param_t *p = &fn->as.fn.params.data[k];
      tp_emit_group_contract(j, &first, fact->owner, fn->as.fn.name, p->name,
                             p->type, "param");
    }
    tp_emit_group_contract(j, &first, fact->owner, fn->as.fn.name, "return",
                           fn->as.fn.return_type, "return");
  }
  for (size_t i = 0; ctx && ctx->cg && i < ctx->cg->operators.len; ++i) {
    ny_operator_def_t *op = &ctx->cg->operators.data[i];
    if (!tp_stmt_in_scope(ctx, op->stmt))
      continue;
    tp_emit_group_contract(j, &first, "", op->target_name, "left",
                           op->left_type, "operator");
    tp_emit_group_contract(j, &first, "", op->target_name, "right",
                           op->right_type, "operator");
    tp_emit_group_contract(j, &first, "", op->target_name, "return",
                           op->return_type, "operator");
  }
  tp_append(j, "]");
}

static void tp_emit_operator_obligation_json(ny_tp_json_t *j, bool *first,
                                             const char *op, const char *lt,
                                             const char *rt,
                                             ny_operator_def_t *def,
                                             size_t matches) {
  if (!j || !first)
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"op\":");
  tp_json_str(j, op ? op : "");
  tp_append(j, ",\"left\":");
  tp_json_str(j, lt ? lt : "any");
  tp_append(j, ",\"right\":");
  tp_json_str(j, rt ? rt : "any");
  tp_append(j, ",\"status\":");
  tp_json_str(j, matches > 1 ? "ambiguous" : (def ? "resolved" : "unresolved"));
  tp_append(j, ",\"return\":");
  tp_json_str(j, def && def->return_type ? def->return_type : "");
  tp_append(j, ",\"target\":");
  tp_json_str(j, def && def->target_name ? def->target_name : "");
  tp_append(j, "}");
}

static void tp_emit_operator_obligations_expr(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                              ny_tp_env_t *env, expr_t *e,
                                              bool *first) {
  if (!ctx || !e)
    return;
  switch (e->kind) {
  case NY_E_BINARY: {
    char *lt = tp_expr_type(ctx, env, e->as.binary.left, 0);
    char *rt = tp_expr_type(ctx, env, e->as.binary.right, 0);
    const char *op = e->as.binary.op;
    if (op && (tp_is_arith_op(op) || tp_is_cmp_op(op))) {
      size_t matches = 0;
      ny_operator_def_t *def = tp_find_operator_def(ctx, op, lt, rt, &matches);
      bool equality = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
      bool nominal = tp_is_nominal_custom_type(ctx, lt) ||
                     tp_is_nominal_custom_type(ctx, rt);
      bool needs_trait = matches > 0 || (!equality && nominal);
      if (needs_trait)
        tp_emit_operator_obligation_json(j, first, op, lt, rt, def, matches);
      if (needs_trait && matches == 0) {
        tp_add_diag(ctx, e->tok, "trait", "operator-unresolved",
                    "unresolved operator '%s' for %s and %s", op,
                    lt ? lt : "unknown", rt ? rt : "unknown");
      } else if (needs_trait && matches > 1) {
        tp_add_diag(ctx, e->tok, "trait", "operator-ambiguous",
                    "ambiguous operator '%s' for %s and %s", op,
                    lt ? lt : "unknown", rt ? rt : "unknown");
      }
    }
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.binary.left, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.binary.right, first);
    free(lt);
    free(rt);
    break;
  }
  case NY_E_CALL:
    if (e->as.call.callee)
      tp_emit_operator_obligations_expr(j, ctx, env, e->as.call.callee, first);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.call.args.data[i].val, first);
    break;
  case NY_E_MEMCALL:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.memcall.target, first);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.memcall.args.data[i].val, first);
    break;
  case NY_E_LOGICAL:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.logical.left, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.logical.right, first);
    break;
  case NY_E_UNARY:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.unary.right, first);
    break;
  case NY_E_TERNARY:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.ternary.cond, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.ternary.true_expr,
                                      first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.ternary.false_expr,
                                      first);
    break;
  case NY_E_INDEX:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.target, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.start, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.stop, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.step, first);
    break;
  case NY_E_MEMBER:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.member.target, first);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      tp_emit_operator_obligations_expr(j, ctx, env, e->as.list_like.data[i],
                                        first);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.dict.pairs.data[i].key, first);
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.dict.pairs.data[i].value, first);
    }
    break;
  default:
    break;
  }
}

static void tp_emit_operator_obligations_stmt(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                              ny_tp_env_t *env, stmt_t *s,
                                              bool *first) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_FUNC: {
    ny_tp_env_t fn_env = {0};
    ny_tp_func_fact_t *fact = tp_find_func(ctx, s->as.fn.name);
    for (size_t i = 0; i < s->as.fn.params.len; ++i) {
      param_t *p = &s->as.fn.params.data[i];
      const char *pt = p->type ? p->type : tp_func_param_inferred(fact, i);
      tp_env_set(&fn_env, p->name, pt ? pt : "any", false);
    }
    tp_emit_operator_obligations_stmt(j, ctx, &fn_env, s->as.fn.body, first);
    tp_env_dispose(&fn_env);
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const char *decl =
          i < s->as.var.types.len ? s->as.var.types.data[i] : NULL;
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      tp_emit_operator_obligations_expr(j, ctx, env, init, first);
      char *it = init ? tp_expr_type(ctx, env, init, 0) : ny_strdup("any");
      tp_env_set(env, name, decl ? decl : it, s->as.var.is_mut);
      free(it);
    }
    break;
  case NY_S_EXPR:
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.expr.expr, first);
    break;
  case NY_S_RETURN:
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.ret.value, first);
    break;
  case NY_S_IF:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.iff.init, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.iff.test, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.iff.conseq, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.iff.alt, first);
    break;
  case NY_S_WHILE:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.whl.init, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.whl.test, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.whl.body, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.whl.update, first);
    break;
  case NY_S_FOR:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.fr.init, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.fr.cond, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.fr.iterable, first);
    if (s->as.fr.iter_var)
      tp_env_set(env, s->as.fr.iter_var, "any", true);
    if (s->as.fr.iter_index_var)
      tp_env_set(env, s->as.fr.iter_index_var, "int", true);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.fr.body, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.fr.update, first);
    break;
  case NY_S_TRY:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.tr.body, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.tr.handler, first);
    break;
  case NY_S_MATCH:
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.match.test, first);
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      tp_emit_operator_obligations_stmt(j, ctx, env,
                                        s->as.match.arms.data[i].conseq, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.match.default_conseq,
                                      first);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_emit_operator_obligations_stmt(j, ctx, env, s->as.block.body.data[i],
                                        first);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_emit_operator_obligations_stmt(j, ctx, env, s->as.module.body.data[i],
                                        first);
    break;
  default:
    break;
  }
}

char *ny_type_pipeline_resolved_json(program_t *prog, codegen_t *cg,
                                     const char *source_name,
                                     bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_tp_json_t j = {0};
  tp_append(&j, "{\"schema\":\"resolved.v1\",\"operators\":[");
  size_t emitted = 0;
  for (size_t i = 0; cg && i < cg->operators.len; ++i) {
    ny_operator_def_t *op = &cg->operators.data[i];
    if (!tp_stmt_in_scope(&ctx, op->stmt))
      continue;
    if (emitted++)
      tp_append(&j, ",");
    tp_append(&j, "{\"op\":");
    tp_json_str(&j, op->op ? op->op : "");
    tp_append(&j, ",\"left\":");
    tp_json_str(&j, op->left_type ? op->left_type : "");
    tp_append(&j, ",\"right\":");
    tp_json_str(&j, op->right_type ? op->right_type : "");
    tp_append(&j, ",\"return\":");
    tp_json_str(&j, op->return_type ? op->return_type : "");
    tp_append(&j, ",\"target\":");
    tp_json_str(&j, op->target_name ? op->target_name : "");
    tp_append(&j, "}");
  }
  tp_validate_operator_decls(&ctx);
  tp_validate_impl_coherence(&ctx);
  tp_append(&j, "],\"operator_obligations\":[");
  bool first_ob = true;
  ny_tp_env_t env = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_operator_obligations_stmt(&j, &ctx, &env, prog->body.data[i],
                                      &first_ob);
  tp_env_dispose(&env);
  tp_append(&j, "],\"type_groups\":");
  tp_emit_type_groups_json(&j);
  tp_append(&j, ",\"protocol_conformance\":");
  tp_emit_group_conformance_json(&j, &ctx);
  tp_append(&j, ",\"trait_conformance\":");
  tp_emit_group_conformance_json(&j, &ctx);
  tp_append(&j, ",\"diagnostics\":");
  tp_emit_diagnostics_json(&j, &ctx);
  tp_append(&j, "}");
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"schema\":\"resolved.v1\",\"operators\":[]}");
}

int ny_type_pipeline_validate_trait(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std,
                                    bool emit_diagnostics) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  tp_validate_operator_decls(&ctx);
  tp_validate_impl_coherence(&ctx);
  bool first = true;
  ny_tp_env_t env = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_operator_obligations_stmt(NULL, &ctx, &env, prog->body.data[i],
                                      &first);
  tp_env_dispose(&env);
  int count = (int)ctx.diagnostics.len;
  if (emit_diagnostics) {
    for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
      const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
      ny_diag_error(d->tok, "%s",
                    d->message ? d->message : "type validation failed");
    }
  }
  tp_ctx_dispose(&ctx);
  return count;
}

typedef struct ny_tp_flow_len_t {
  const char *name;
  int64_t len;
} ny_tp_flow_len_t;
typedef VEC(ny_tp_flow_len_t) ny_tp_flow_len_list;

static bool tp_expr_str_lit(expr_t *e, const char **out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  if (out)
    *out = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
  return true;
}

static const char *tp_expr_ident(expr_t *e) {
  return (e && e->kind == NY_E_IDENT) ? e->as.ident.name : NULL;
}

static int64_t tp_static_list_len(expr_t *e) {
  if (!e)
    return -1;
  if (e->kind == NY_E_LIST || e->kind == NY_E_TUPLE || e->kind == NY_E_SET)
    return (int64_t)e->as.list_like.len;
  return -1;
}

static void tp_flow_len_set(ny_tp_flow_len_list *env, const char *name,
                            int64_t len) {
  if (!env || !name || !*name || len < 0)
    return;
  for (size_t i = env->len; i > 0; --i) {
    if (env->data[i - 1].name && strcmp(env->data[i - 1].name, name) == 0) {
      env->data[i - 1].len = len;
      return;
    }
  }
  ny_tp_flow_len_t item = {.name = name, .len = len};
  vec_push(env, item);
}

static int64_t tp_flow_len_get(ny_tp_flow_len_list *env, const char *name) {
  if (!env || !name)
    return -1;
  for (size_t i = env->len; i > 0; --i)
    if (env->data[i - 1].name && strcmp(env->data[i - 1].name, name) == 0)
      return env->data[i - 1].len;
  return -1;
}

static void tp_emit_range_refinement_json(ny_tp_json_t *j, bool *first,
                                          const char *var, const char *source,
                                          bool has_min, int64_t min_v,
                                          bool has_max, int64_t max_v,
                                          const char *branch) {
  if (!j || !first || !var || !*var || (!has_min && !has_max))
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"var\":");
  tp_json_str(j, var);
  tp_append(j, ",\"source\":");
  tp_json_str(j, source ? source : "condition");
  tp_append(j, ",\"branch\":");
  tp_json_str(j, branch ? branch : "");
  if (has_min)
    tp_append(j, ",\"min\":%lld", (long long)min_v);
  if (has_max)
    tp_append(j, ",\"max\":%lld", (long long)max_v);
  tp_append(j, "}");
}

static const char *tp_reverse_cmp_op(const char *op) {
  if (!op)
    return NULL;
  if (strcmp(op, "<") == 0)
    return ">";
  if (strcmp(op, "<=") == 0)
    return ">=";
  if (strcmp(op, ">") == 0)
    return "<";
  if (strcmp(op, ">=") == 0)
    return "<=";
  return op;
}

static void tp_emit_range_condition(ny_tp_json_t *j, bool *first, expr_t *e,
                                    const char *source) {
  if (!j || !first || !e || e->kind != NY_E_BINARY || !e->as.binary.op)
    return;
  const char *var = tp_expr_ident(e->as.binary.left);
  int64_t v = 0;
  const char *op = e->as.binary.op;
  if (!var || !ny_expr_literal_i64(e->as.binary.right, &v)) {
    var = tp_expr_ident(e->as.binary.right);
    if (!var || !ny_expr_literal_i64(e->as.binary.left, &v))
      return;
    op = tp_reverse_cmp_op(e->as.binary.op);
  }
  if (strcmp(op, "<") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v - 1,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, true, v, false, 0,
                                  "else");
  } else if (strcmp(op, "<=") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, true, v + 1, false, 0,
                                  "else");
  } else if (strcmp(op, ">") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, true, v + 1, false, 0,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v,
                                  "else");
  } else if (strcmp(op, ">=") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, true, v, false, 0,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v - 1,
                                  "else");
  } else if (strcmp(op, "==") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, true, v, true, v,
                                  "then");
  }
}

static void tp_emit_index_proof_json(ny_tp_json_t *j, bool *first,
                                     const char *container, const char *index,
                                     int64_t index_lit, bool has_index_lit,
                                     int64_t len, const char *source) {
  if (!j || !first)
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  bool proven =
      len >= 0 && ((has_index_lit && index_lit >= 0 && index_lit < len) ||
                   (!has_index_lit && index && *index));
  tp_append(j, "{\"container\":");
  tp_json_str(j, container ? container : "");
  tp_append(j, ",\"index\":");
  if (has_index_lit) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)index_lit);
    tp_json_str(j, buf);
  } else {
    tp_json_str(j, index ? index : "");
  }
  tp_append(j, ",\"len\":%lld,\"source\":", (long long)len);
  tp_json_str(j, source ? source : "index");
  tp_append(j, ",\"status\":");
  tp_json_str(j, proven ? "static-len-known" : "observed");
  tp_append(j, "}");
}

static void tp_emit_discriminant_json(ny_tp_json_t *j, bool *first,
                                      const char *subject, const char *variant,
                                      const char *source, const char *binding) {
  if (!j || !first || !variant || !*variant)
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"subject\":");
  tp_json_str(j, subject ? subject : "");
  tp_append(j, ",\"variant\":");
  tp_json_str(j, variant);
  tp_append(j, ",\"source\":");
  tp_json_str(j, source ? source : "match");
  tp_append(j, ",\"binding\":");
  tp_json_str(j, binding ? binding : "");
  tp_append(j, "}");
}

static void tp_emit_pattern_discriminant(ny_tp_json_t *j, bool *first,
                                         const char *subject, expr_t *pattern) {
  if (!pattern)
    return;
  if (pattern->kind == NY_E_IDENT) {
    tp_emit_discriminant_json(j, first, subject, pattern->as.ident.name,
                              "match", "");
  } else if (pattern->kind == NY_E_LITERAL) {
    char buf[96];
    if (pattern->as.literal.kind == NY_LIT_STR) {
      snprintf(buf, sizeof(buf), "str:%.*s", (int)pattern->as.literal.as.s.len,
               pattern->as.literal.as.s.data ? pattern->as.literal.as.s.data
                                             : "");
    } else if (pattern->as.literal.kind == NY_LIT_INT) {
      snprintf(buf, sizeof(buf), "int:%lld",
               (long long)pattern->as.literal.as.i);
    } else if (pattern->as.literal.kind == NY_LIT_BOOL) {
      snprintf(buf, sizeof(buf), "bool:%s",
               pattern->as.literal.as.b ? "true" : "false");
    } else {
      snprintf(buf, sizeof(buf), "literal");
    }
    tp_emit_discriminant_json(j, first, subject, buf, "match", "");
  }
}

static void tp_emit_flow_expr(ny_tp_json_t *nullable_j, ny_tp_json_t *range_j,
                              ny_tp_json_t *index_j, expr_t *e,
                              ny_tp_flow_len_list *lens, bool *first_nullable,
                              bool *first_range, bool *first_index) {
  if (!e)
    return;
  ny_null_narrow_list_t list;
  vec_init(&list);
  if (ny_null_narrow_collect(e, &list) && !ny_null_narrow_list_empty(&list)) {
    for (size_t i = 0; i < list.len; ++i) {
      const ny_null_narrow_info_t *info = &list.data[i];
      if (!info->name)
        continue;
      if (!*first_nullable)
        tp_append(nullable_j, ",");
      *first_nullable = false;
      tp_append(nullable_j, "{\"var\":");
      tp_json_str(nullable_j, info->name);
      tp_append(nullable_j, ",\"then\":");
      tp_json_str(nullable_j, info->true_nonnull ? "non-null" : "");
      tp_append(nullable_j, ",\"else\":");
      tp_json_str(nullable_j, info->false_nonnull ? "non-null" : "");
      tp_append(nullable_j, "}");
    }
  }
  vec_free(&list);
  tp_emit_range_condition(range_j, first_range, e, "condition");
  if (e->kind == NY_E_LOGICAL) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.logical.left, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.logical.right, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_UNARY) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.unary.right, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_BINARY) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.binary.left, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.binary.right, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_INDEX) {
    const char *container = tp_expr_ident(e->as.index.target);
    const char *idx_name = tp_expr_ident(e->as.index.start);
    int64_t idx_lit = 0;
    bool has_idx_lit = ny_expr_literal_i64(e->as.index.start, &idx_lit);
    int64_t len = tp_static_list_len(e->as.index.target);
    if (len < 0)
      len = tp_flow_len_get(lens, container);
    tp_emit_index_proof_json(index_j, first_index, container, idx_name, idx_lit,
                             has_idx_lit, len, "index-expression");
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.index.target, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.index.start, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_CALL) {
    const char *name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    if (name &&
        (strcmp(name, "assert_compile_range") == 0 ||
         strcmp(name, "range_proven") == 0) &&
        e->as.call.args.len >= 3) {
      int64_t min_v = 0, max_v = 0;
      bool has_min = ny_expr_literal_i64(e->as.call.args.data[1].val, &min_v);
      bool has_max = ny_expr_literal_i64(e->as.call.args.data[2].val, &max_v);
      const char *var = tp_expr_ident(e->as.call.args.data[0].val);
      if (!var && ny_expr_literal_i64(e->as.call.args.data[0].val, NULL))
        var = "<literal>";
      tp_emit_range_refinement_json(range_j, first_range, var,
                                    strcmp(name, "range_proven") == 0
                                        ? "range_proven"
                                        : "assert_compile_range",
                                    has_min, min_v, has_max, max_v, "asserted");
    } else if (name &&
               (strcmp(name, "assert_compile_index") == 0 ||
                strcmp(name, "index_proven") == 0) &&
               e->as.call.args.len >= 2) {
      const char *container = tp_expr_ident(e->as.call.args.data[0].val);
      const char *idx_name = tp_expr_ident(e->as.call.args.data[1].val);
      int64_t idx_lit = 0;
      bool has_idx_lit =
          ny_expr_literal_i64(e->as.call.args.data[1].val, &idx_lit);
      int64_t len = tp_static_list_len(e->as.call.args.data[0].val);
      if (len < 0)
        len = tp_flow_len_get(lens, container);
      tp_emit_index_proof_json(
          index_j, first_index, container, idx_name, idx_lit, has_idx_lit, len,
          strcmp(name, "index_proven") == 0 ? "index_proven"
                                            : "assert_compile_index");
    }
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.call.callee, lens,
                      first_nullable, first_range, first_index);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_emit_flow_expr(nullable_j, range_j, index_j,
                        e->as.call.args.data[i].val, lens, first_nullable,
                        first_range, first_index);
  } else if (e->kind == NY_E_MEMCALL) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.memcall.target, lens,
                      first_nullable, first_range, first_index);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_emit_flow_expr(nullable_j, range_j, index_j,
                        e->as.memcall.args.data[i].val, lens, first_nullable,
                        first_range, first_index);
  }
}

static void tp_emit_flow_stmt(ny_tp_json_t *nullable_j, ny_tp_json_t *range_j,
                              ny_tp_json_t *index_j, ny_tp_json_t *disc_j,
                              ny_tp_ctx_t *ctx, stmt_t *s,
                              ny_tp_flow_len_list *lens, bool *first_nullable,
                              bool *first_range, bool *first_index,
                              bool *first_disc) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_IF:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.iff.test, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.iff.init,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.iff.conseq, lens, first_nullable, first_range,
                      first_index, first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.iff.alt,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    break;
  case NY_S_WHILE:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.whl.test, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.whl.init,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.whl.body,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.whl.update, lens, first_nullable, first_range,
                      first_index, first_disc);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      int64_t len = tp_static_list_len(init);
      if (len >= 0)
        tp_flow_len_set(lens, name, len);
      tp_emit_flow_expr(nullable_j, range_j, index_j, init, lens,
                        first_nullable, first_range, first_index);
    }
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                        s->as.block.body.data[i], lens, first_nullable,
                        first_range, first_index, first_disc);
    break;
  case NY_S_FUNC:
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.fn.body,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    break;
  case NY_S_EXPR:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.expr.expr, lens,
                      first_nullable, first_range, first_index);
    break;
  case NY_S_RETURN:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.ret.value, lens,
                      first_nullable, first_range, first_index);
    break;
  case NY_S_GUARD:
    tp_emit_discriminant_json(
        disc_j, first_disc, tp_expr_ident(s->as.guard.value),
        s->as.guard.type_name, "layout_guard", s->as.guard.name);
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.guard.value, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.guard.fallback, lens, first_nullable, first_range,
                      first_index, first_disc);
    break;
  case NY_S_MATCH: {
    const char *subject = tp_expr_ident(s->as.match.test);
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.match.test, lens,
                      first_nullable, first_range, first_index);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t k = 0; k < arm->patterns.len; ++k)
        tp_emit_pattern_discriminant(disc_j, first_disc, subject,
                                     arm->patterns.data[k]);
      tp_emit_flow_expr(nullable_j, range_j, index_j, arm->guard, lens,
                        first_nullable, first_range, first_index);
      tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, arm->conseq,
                        lens, first_nullable, first_range, first_index,
                        first_disc);
    }
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.match.default_conseq, lens, first_nullable,
                      first_range, first_index, first_disc);
    break;
  }
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                        s->as.module.body.data[i], lens, first_nullable,
                        first_range, first_index, first_disc);
    break;
  default:
    break;
  }
}

char *ny_type_pipeline_refined_json(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_tp_json_t j = {0}, nullable_j = {0}, range_j = {0}, index_j = {0},
               disc_j = {0};
  bool first_nullable = true;
  bool first_range = true;
  bool first_index = true;
  bool first_disc = true;
  ny_tp_flow_len_list lens;
  vec_init(&lens);
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_flow_stmt(&nullable_j, &range_j, &index_j, &disc_j, &ctx,
                      prog->body.data[i], &lens, &first_nullable, &first_range,
                      &first_index, &first_disc);
  tp_append(&j,
            "{\"schema\":\"refined.v1\",\"nullable_refinements\":[%s],"
            "\"range_refinements\":[%s],\"index_proofs\":[%s],"
            "\"discriminant_refinements\":[%s],\"diagnostics\":[]}",
            nullable_j.data ? nullable_j.data : "",
            range_j.data ? range_j.data : "", index_j.data ? index_j.data : "",
            disc_j.data ? disc_j.data : "");
  free(nullable_j.data);
  free(range_j.data);
  free(index_j.data);
  free(disc_j.data);
  vec_free(&lens);
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"schema\":\"refined.v1\",\"nullable_refinements\":[]}");
}

static const char *tp_layout_carrier(size_t size) {
  if (size == 1)
    return "i8";
  if (size == 2)
    return "i16";
  if (size <= 4)
    return "i32";
  if (size <= 8)
    return "i64";
  return "aggregate";
}

static const char *tp_layout_abi_policy(size_t size) {
  if (size == 0)
    return "empty";
  if (size == 1 || size == 2 || size == 4 || size == 8)
    return "direct-scalar";
  return "aggregate-memory";
}

static layout_field_info_t *tp_layout_field(layout_def_t *layout,
                                            const char *field_name) {
  if (!layout || !field_name)
    return NULL;
  for (size_t i = 0; i < layout->fields.len; ++i) {
    layout_field_info_t *f = &layout->fields.data[i];
    if (f->name && strcmp(f->name, field_name) == 0)
      return f;
  }
  return NULL;
}

static void tp_validate_layout_call_expr(ny_tp_ctx_t *ctx, expr_t *e);

static void tp_validate_layout_call_args(ny_tp_ctx_t *ctx, expr_t *e,
                                         const char *name,
                                         ny_call_arg_list *args) {
  if (!ctx || !e || !name || !args)
    return;
  bool is_size = strcmp(name, "__layout_size") == 0;
  bool is_align = strcmp(name, "__layout_align") == 0;
  bool is_offset = strcmp(name, "__layout_offset") == 0;
  bool is_store = strcmp(name, "store_layout") == 0;
  bool is_load = strcmp(name, "load_layout") == 0;
  if (!is_size && !is_align && !is_offset && !is_store && !is_load)
    return;
  size_t layout_arg = is_store || is_load ? 1u : 0u;
  if (args->len <= layout_arg) {
    tp_add_diag(ctx, e->tok, "abi", "layout-arity",
                "%s expects a string literal layout name", name);
    return;
  }
  const char *layout_name = NULL;
  if (!tp_expr_str_lit(args->data[layout_arg].val, &layout_name)) {
    tp_add_diag(ctx, e->tok, "abi", "layout-nonliteral",
                "%s expects a string literal layout name", name);
    return;
  }
  layout_def_t *layout = lookup_layout(ctx->cg, layout_name);
  if (!layout) {
    tp_add_diag(ctx, e->tok, "abi", "layout-unknown", "unknown layout '%s'",
                layout_name);
    return;
  }
  if (is_offset || is_load) {
    size_t field_arg = is_load ? 2u : 1u;
    if (args->len <= field_arg) {
      tp_add_diag(ctx, e->tok, "abi", "layout-field-missing",
                  "%s expects a string literal field name", name);
      return;
    }
    const char *field_name = NULL;
    if (!tp_expr_str_lit(args->data[field_arg].val, &field_name)) {
      tp_add_diag(ctx, e->tok, "abi", "layout-field-nonliteral",
                  "%s expects a string literal field name", name);
      return;
    }
    if (!tp_layout_field(layout, field_name)) {
      tp_add_diag(ctx, e->tok, "abi", "layout-field-unknown",
                  "unknown field '%s' in layout '%s'", field_name, layout_name);
    }
  }
  if (is_store && args->len != layout->fields.len + 2u) {
    tp_add_diag(ctx, e->tok, "abi", "layout-store-arity",
                "store_layout('%s') expects %zu field value(s), got %zu",
                layout_name, layout->fields.len,
                args->len > 2u ? args->len - 2u : 0u);
  }
}

static void tp_validate_layout_call_expr(ny_tp_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return;
  switch (e->kind) {
  case NY_E_CALL: {
    const char *name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    tp_validate_layout_call_args(ctx, e, name, &e->as.call.args);
    tp_validate_layout_call_expr(ctx, e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_validate_layout_call_expr(ctx, e->as.call.args.data[i].val);
    break;
  }
  case NY_E_MEMCALL:
    tp_validate_layout_call_expr(ctx, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_validate_layout_call_expr(ctx, e->as.memcall.args.data[i].val);
    break;
  case NY_E_BINARY:
    tp_validate_layout_call_expr(ctx, e->as.binary.left);
    tp_validate_layout_call_expr(ctx, e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    tp_validate_layout_call_expr(ctx, e->as.logical.left);
    tp_validate_layout_call_expr(ctx, e->as.logical.right);
    break;
  case NY_E_UNARY:
    tp_validate_layout_call_expr(ctx, e->as.unary.right);
    break;
  case NY_E_TERNARY:
    tp_validate_layout_call_expr(ctx, e->as.ternary.cond);
    tp_validate_layout_call_expr(ctx, e->as.ternary.true_expr);
    tp_validate_layout_call_expr(ctx, e->as.ternary.false_expr);
    break;
  case NY_E_INDEX:
    tp_validate_layout_call_expr(ctx, e->as.index.target);
    tp_validate_layout_call_expr(ctx, e->as.index.start);
    tp_validate_layout_call_expr(ctx, e->as.index.stop);
    tp_validate_layout_call_expr(ctx, e->as.index.step);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      tp_validate_layout_call_expr(ctx, e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      tp_validate_layout_call_expr(ctx, e->as.dict.pairs.data[i].key);
      tp_validate_layout_call_expr(ctx, e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_MEMBER:
    tp_validate_layout_call_expr(ctx, e->as.member.target);
    break;
  default:
    break;
  }
}

static void tp_validate_abi_stmt(ny_tp_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      tp_validate_layout_call_expr(ctx, s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    tp_validate_layout_call_expr(ctx, s->as.expr.expr);
    break;
  case NY_S_RETURN:
    tp_validate_layout_call_expr(ctx, s->as.ret.value);
    break;
  case NY_S_IF:
    tp_validate_abi_stmt(ctx, s->as.iff.init);
    tp_validate_layout_call_expr(ctx, s->as.iff.test);
    tp_validate_abi_stmt(ctx, s->as.iff.conseq);
    tp_validate_abi_stmt(ctx, s->as.iff.alt);
    break;
  case NY_S_WHILE:
    tp_validate_abi_stmt(ctx, s->as.whl.init);
    tp_validate_layout_call_expr(ctx, s->as.whl.test);
    tp_validate_abi_stmt(ctx, s->as.whl.body);
    tp_validate_abi_stmt(ctx, s->as.whl.update);
    break;
  case NY_S_FOR:
    tp_validate_abi_stmt(ctx, s->as.fr.init);
    tp_validate_layout_call_expr(ctx, s->as.fr.cond);
    tp_validate_layout_call_expr(ctx, s->as.fr.iterable);
    tp_validate_abi_stmt(ctx, s->as.fr.body);
    tp_validate_abi_stmt(ctx, s->as.fr.update);
    break;
  case NY_S_GUARD:
    tp_validate_layout_call_expr(ctx, s->as.guard.value);
    tp_validate_abi_stmt(ctx, s->as.guard.fallback);
    break;
  case NY_S_MATCH:
    tp_validate_layout_call_expr(ctx, s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t k = 0; k < arm->patterns.len; ++k)
        tp_validate_layout_call_expr(ctx, arm->patterns.data[k]);
      tp_validate_layout_call_expr(ctx, arm->guard);
      tp_validate_abi_stmt(ctx, arm->conseq);
    }
    tp_validate_abi_stmt(ctx, s->as.match.default_conseq);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_validate_abi_stmt(ctx, s->as.block.body.data[i]);
    break;
  case NY_S_FUNC:
    tp_validate_abi_stmt(ctx, s->as.fn.body);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_validate_abi_stmt(ctx, s->as.module.body.data[i]);
    break;
  default:
    break;
  }
}

static void tp_validate_abi_program(ny_tp_ctx_t *ctx) {
  if (!ctx || !ctx->prog)
    return;
  for (size_t i = 0; i < ctx->prog->body.len; ++i)
    tp_validate_abi_stmt(ctx, ctx->prog->body.data[i]);
}

int ny_type_pipeline_validate_abi(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std,
                                  bool emit_diagnostics) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  tp_validate_abi_program(&ctx);
  int count = (int)ctx.diagnostics.len;
  if (emit_diagnostics) {
    for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
      const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
      ny_diag_error(d->tok, "%s",
                    d->message ? d->message : "ABI validation failed");
    }
  }
  tp_ctx_dispose(&ctx);
  return count;
}

char *ny_type_pipeline_lowered_json(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  tp_validate_abi_program(&ctx);
  ny_tp_json_t j = {0};
  tp_append(&j, "{\"schema\":\"lowered.v1\",\"layouts\":[");
  size_t emitted = 0;
  for (size_t i = 0; cg && i < cg->layouts.len; ++i) {
    layout_def_t *layout = cg->layouts.data[i];
    if (!layout || !tp_stmt_in_scope(&ctx, layout->stmt))
      continue;
    if (emitted++)
      tp_append(&j, ",");
    tp_append(&j, "{\"name\":");
    tp_json_str(&j, layout->name ? layout->name : "");
    tp_append(&j, ",\"size\":%zu,\"align\":%zu,\"pack\":%zu,\"carrier\":",
              layout->size, layout->align, layout->pack);
    tp_json_str(&j, tp_layout_carrier(layout->size));
    tp_append(&j, ",\"abi_policy\":");
    tp_json_str(&j, tp_layout_abi_policy(layout->size));
    tp_append(&j, ",\"direct\":%s,\"field_count\":%zu,\"fields\":[",
              strcmp(tp_layout_abi_policy(layout->size), "direct-scalar") == 0
                  ? "true"
                  : "false",
              layout->fields.len);
    for (size_t k = 0; k < layout->fields.len; ++k) {
      layout_field_info_t *f = &layout->fields.data[k];
      if (k)
        tp_append(&j, ",");
      tp_append(&j, "{\"name\":");
      tp_json_str(&j, f->name ? f->name : "");
      tp_append(&j, ",\"type\":");
      tp_json_str(&j, f->type_name ? f->type_name : "");
      tp_append(&j, ",\"offset\":%zu,\"size\":%zu,\"align\":%zu}", f->offset,
                f->size, f->align);
    }
    tp_append(&j, "]}");
  }
  size_t accepted = cg ? cg->mono_specs.len : 0;
  size_t rejected =
      ctx.mono_candidates > accepted ? ctx.mono_candidates - accepted : 0;
  tp_append(&j,
            "],\"monomorphization\":{\"candidates\":%zu,\"accepted\":%zu,"
            "\"rejected\":%zu,\"candidate_decisions\":[",
            ctx.mono_candidates, accepted, rejected);
  bool first_candidate = true;
  for (size_t i = 0; i < ctx.funcs.len; ++i) {
    ny_tp_func_fact_t *fact = &ctx.funcs.data[i];
    if (!fact->call_candidates)
      continue;
    if (!first_candidate)
      tp_append(&j, ",");
    first_candidate = false;
    bool useful_raw = false;
    tp_append(&j, "{\"name\":");
    tp_json_str(&j, fact->name ? fact->name : "");
    tp_append(&j, ",\"call_candidates\":%zu,\"arg_proofs\":[",
              fact->call_candidates);
    for (size_t k = 0; k < fact->params.len; ++k) {
      if (k)
        tp_append(&j, ",");
      const char *t = tp_func_param_inferred(fact, k);
      bool raw = tp_is_int_type(t) || tp_is_float_type(t);
      useful_raw = useful_raw || raw;
      tp_append(&j, "{\"type\":");
      tp_json_str(&j, t ? t : "any");
      tp_append(&j, ",\"raw\":%s}", raw ? "true" : "false");
    }
    tp_append(&j, "],\"decision\":");
    tp_json_str(&j, useful_raw ? "eligible-for-codegen-specialization"
                               : "rejected-no-raw-proof");
    tp_append(&j, "}");
  }
  tp_append(&j, "],\"accepted_specs\":[");
  for (size_t i = 0; cg && i < cg->mono_specs.len; ++i) {
    ny_mono_specialization_t *spec = &cg->mono_specs.data[i];
    if (i)
      tp_append(&j, ",");
    tp_append(&j, "{\"base\":");
    tp_json_str(&j, spec->base_name ? spec->base_name : "");
    tp_append(&j, ",\"specialized\":");
    tp_json_str(&j, spec->specialized_name ? spec->specialized_name : "");
    tp_append(&j, ",\"arity\":%d,\"body_cost\":%zu,\"reason\":", spec->arity,
              spec->body_cost);
    tp_json_str(&j,
                spec->accept_reason ? spec->accept_reason : "arg-specialized");
    tp_append(&j, ",\"return_policy\":");
    tp_json_str(&j,
                spec->return_policy ? spec->return_policy : "tagged-return");
    tp_append(&j, ",\"raw_return_proven\":%s,\"return_kind\":",
              spec->raw_return_proven ? "true" : "false");
    tp_json_str(&j, spec->return_kind == NY_MONO_TYPE_INT
                        ? "int"
                        : (spec->return_kind == NY_MONO_TYPE_F64
                               ? "f64"
                               : (spec->return_kind == NY_MONO_TYPE_LIST
                                      ? "list"
                                      : (spec->return_kind == NY_MONO_TYPE_F64_LIST
                                             ? "list<f64>"
                                             : ""))));
    tp_append(&j, ",\"arg_types\":[");
    for (int k = 0; k < spec->arity && k < NY_MONO_MAX_ARITY; ++k) {
      if (k)
        tp_append(&j, ",");
      tp_json_str(&j, spec->types[k] == NY_MONO_TYPE_INT
                          ? "int"
                          : (spec->types[k] == NY_MONO_TYPE_F64
                                 ? "f64"
                                 : (spec->types[k] == NY_MONO_TYPE_LIST
                                        ? "list"
                                        : (spec->types[k] == NY_MONO_TYPE_F64_LIST
                                               ? "list<f64>"
                                               : "dynamic"))));
    }
    tp_append(&j, "],\"raw_return_active\":%s,\"inline_body_eligible\":%s}",
              spec->raw_return_active ? "true" : "false",
              spec->inline_body_eligible ? "true" : "false");
  }
  tp_append(&j,
            "],\"inline_body_uses\":%zu,\"masked_range_uses\":%zu,"
            "\"rejection_reasons\":{\"not_specialized_or_capped\":%zu}},"
            "\"diagnostics\":",
            cg ? cg->mono_inline_body_uses : 0,
            cg ? cg->mono_masked_range_uses : 0, rejected);
  tp_emit_diagnostics_json(&j, &ctx);
  tp_append(&j, "}");
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"schema\":\"lowered.v1\",\"layouts\":[]}");
}
