/*
 * Type pipeline emit: serializes the fully-typed AST to JSON for
 * artifact-based compilation, tooling, and pipeline-stage validation.
 */
static void hm_run(ny_hm_state_t *hm, ny_tp_ctx_t *ctx);
static void hm_run_reachable(ny_hm_state_t *hm, ny_tp_ctx_t *ctx,
                             stmt_t *const *targets, size_t target_count);
static void hm_dispose(ny_hm_state_t *hm);
static void hm_emit_schemes_json(ny_tp_json_t *j, ny_hm_state_t *hm);
static void tp_validate_contract_call_proofs(program_t *prog, ny_tp_ctx_t *tp);
static void tp_emit_callable_rows_json(ny_tp_json_t *j, const ny_tp_ctx_t *ctx);
static void tp_emit_proof_obligations_json(ny_tp_json_t *j, ny_tp_ctx_t *ctx);
static bool tp_contract_effect_row_open(const ny_type_t *effect_type);

char *ny_type_pipeline_typed_json(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ctx.collect_semantic_artifact_facts = true;
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  for (size_t i = 0; i < ctx.funcs.len; ++i)
    ny_analyze_function_call_effect_facts(cg, ctx.funcs.data[i].stmt);
  tp_validate_contract_call_proofs(prog, &ctx);
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
  tp_append(&j, ",\"layouts\":");
  tp_emit_semantic_layouts_json(&j, &ctx);
  tp_append(&j, ",\"callable_effects\":");
  tp_emit_callable_rows_json(&j, &ctx);
  tp_append(&j, ",\"proof_obligations\":");
  tp_emit_proof_obligations_json(&j, &ctx);
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

int ny_type_pipeline_list_fallbacks(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  size_t total = ctx.fallbacks.len;
  for (size_t i = 0; i < ctx.fallbacks.len; ++i) {
    const ny_tp_fallback_t *fb = &ctx.fallbacks.data[i];
    if (!fb || !fb->code)
      continue;
    const char *file = fb->tok.filename ? fb->tok.filename : "<source>";
    printf("fallback %s stage=%s expr=%s at %s:%d:%d reason=%s\n", fb->code,
           fb->stage ? fb->stage : "type",
           fb->expr_kind ? fb->expr_kind : "expr", file, fb->tok.line,
           fb->tok.col, fb->reason ? fb->reason : "");
  }
  printf("%zu fallbacks listed\n", total);
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return 0;
}

int ny_type_pipeline_explain_semantics(program_t *prog, codegen_t *cg,
                                       const char *source_name,
                                       bool include_std, const char *topic) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);

  printf("=== Semantic Explanations for '%s' ===\n\n",
         source_name ? source_name : "<source>");

  bool show_types = !topic || strcmp(topic, "all") == 0 ||
                    strcmp(topic, "types") == 0 ||
                    strcmp(topic, "signatures") == 0;
  bool show_solver = !topic || strcmp(topic, "all") == 0 ||
                     strcmp(topic, "solver") == 0 ||
                     strcmp(topic, "proofs") == 0;
  bool show_fallbacks = !topic || strcmp(topic, "all") == 0 ||
                        strcmp(topic, "fallbacks") == 0 ||
                        strcmp(topic, "any") == 0;

  if (show_solver) {
    printf("[Solver & Proof Architecture]\n");
    printf("  requested backend : %s\n",
           ctx.solver.requested ? ctx.solver.requested : "auto");
    printf("  active backend    : %s\n",
           ctx.solver.backend ? ctx.solver.backend : "presburger/fm");
    printf("  Z3 available      : %s\n",
           ctx.solver.z3_available ? "yes" : "no");
    printf("  Z3 checks run     : %u\n", ctx.solver.z3_checks);
    printf("  Z3 obligations    : %u\n", ctx.solver.z3_obligations);
    printf("  strict types mode : %s\n",
           (cg && cg->strict_types) ? "enabled" : "disabled");
    printf("\n");
  }

  if (show_types) {
    printf("[Inferred Functions & Signatures (%zu functions, %zu mono "
           "candidates)]\n",
           ctx.funcs.len, ctx.mono_candidates);
    for (size_t i = 0; i < ctx.funcs.len; ++i) {
      const ny_tp_func_fact_t *fn = &ctx.funcs.data[i];
      if (!fn || !fn->name)
        continue;
      printf("  fn %s%s%s(", fn->owner ? fn->owner : "", fn->owner ? "." : "",
             fn->name);
      for (size_t p = 0; p < fn->params.len; ++p) {
        const ny_tp_param_fact_t *param = &fn->params.data[p];
        const char *pname = (fn->stmt && p < fn->stmt->as.fn.params.len)
                                ? fn->stmt->as.fn.params.data[p].name
                                : NULL;
        printf("%s%s: %s", p > 0 ? ", " : "", pname ? pname : "arg",
               param->inferred_type ? param->inferred_type : "any");
      }
      printf(")\n");
    }
    printf("\n");
  }

  if (show_fallbacks) {
    printf("[Dynamic Fallbacks & Degradations to Any (%zu total)]\n",
           ctx.fallbacks.len);
    if (ctx.fallbacks.len == 0) {
      printf("  none — all operations resolved to concrete representations.\n");
    } else {
      for (size_t i = 0; i < ctx.fallbacks.len; ++i) {
        const ny_tp_fallback_t *fb = &ctx.fallbacks.data[i];
        if (!fb || !fb->code)
          continue;
        const char *file = fb->tok.filename ? fb->tok.filename : "<source>";
        printf("  - %s at %s:%d:%d [%s %s]: %s\n", fb->code, file, fb->tok.line,
               fb->tok.col, fb->stage ? fb->stage : "type",
               fb->expr_kind ? fb->expr_kind : "expr",
               fb->reason ? fb->reason : "dynamic fallback");
      }
    }
    printf("\n");
  }

  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return 0;
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

static void tp_ctx_build_fact_only(ny_tp_ctx_t *ctx, program_t *prog,
                                   codegen_t *cg, const char *source_name) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->prog = prog;
  ctx->cg = cg;
  ctx->source_name = source_name;
  ctx->include_std = true;
  vec_init(&ctx->funcs);
  vec_init(&ctx->diagnostics);
  vec_init(&ctx->fallbacks);
  if (prog) {
    for (size_t i = 0; i < prog->body.len; ++i)
      tp_collect_func_defs_stmt(ctx, prog->body.data[i], NULL);
  }
}

static void tp_persist_native_program(program_t *prog, codegen_t *cg,
                                      const char *source_name) {
  if (!prog)
    return;
  ny_tp_ctx_t ctx;
  tp_ctx_build_fact_only(&ctx, prog, cg, source_name);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
}

void ny_type_pipeline_persist_native_facts(program_t *prog, codegen_t *cg,
                                           const char *source_name) {
  tp_persist_native_program(prog, cg, source_name);
  for (size_t i = 0; cg && i < cg->extra_progs.len; ++i)
    tp_persist_native_program(cg->extra_progs.data[i], cg, source_name);
}

static void tp_ctx_build_reachable_fact_only(ny_tp_ctx_t *ctx, program_t *prog,
                                             const stmt_t *const *functions,
                                             size_t function_count,
                                             const char *source_name) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->prog = prog;
  ctx->cg = NULL;
  ctx->source_name = source_name;
  ctx->include_std = false;
  vec_init(&ctx->funcs);
  vec_init(&ctx->diagnostics);
  vec_init(&ctx->fallbacks);
  for (size_t i = 0; i < function_count; ++i) {
    const stmt_t *fn = functions ? functions[i] : NULL;
    if (!fn || fn->kind != NY_S_FUNC)
      continue;
    ny_tp_func_fact_t fact = {
        .name = fn->as.fn.name,
        .stmt = (stmt_t *)fn,
    };
    vec_push(&ctx->funcs, fact);
  }
}

void ny_type_pipeline_persist_native_reachable_facts(
    program_t *prog, const stmt_t *const *functions, size_t function_count,
    const char *source_name) {
  if (!prog || !functions || function_count == 0)
    return;
  ny_tp_ctx_t ctx;
  tp_ctx_build_reachable_fact_only(&ctx, prog, functions, function_count,
                                   source_name);
  ny_hm_state_t hm;
  hm_run_reachable(&hm, &ctx, (stmt_t *const *)functions, function_count);
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
}

typedef struct tp_requires_proof_frame_t {
  const stmt_func_t *current_fn;
  expr_t *premise;
  const char *structural_parent;
  const char **structural_children;
  size_t structural_child_count;
  bool structural_child_rebound;
  const expr_t *skip_expr_root;
  size_t skip_expr_depth;
  bool current_fn_params_rebound;
} tp_requires_proof_frame_t;

typedef struct tp_requires_stmt_frame_t {
  stmt_t *stmt;
  expr_t *premise;
  const char *structural_parent;
  const char **structural_children;
  size_t structural_child_count;
  bool structural_child_rebound;
} tp_requires_stmt_frame_t;

typedef struct tp_requires_proof_ctx_t {
  ny_tp_ctx_t *tp;
  const stmt_func_t *current_fn;
  expr_t *premise;
  const char *structural_parent;
  const char **structural_children;
  size_t structural_child_count;
  bool structural_child_rebound;
  const expr_t *skip_expr_root;
  size_t skip_expr_depth;
  bool current_fn_params_rebound;
  VEC(tp_requires_proof_frame_t) fn_stack;
  VEC(tp_requires_stmt_frame_t) stmt_stack;
  VEC(expr_t *) match_expr_stack;
} tp_requires_proof_ctx_t;

typedef struct tp_param_mutation_scan_t {
  codegen_t *cg;
  const expr_t *premise;
  bool invalidated;
} tp_param_mutation_scan_t;

typedef struct tp_contract_operator_scan_t {
  ny_tp_ctx_t *tp;
  const stmt_func_t *fn;
  bool has_custom_operator;
} tp_contract_operator_scan_t;

static bool tp_operator_module_active(ny_tp_ctx_t *ctx,
                                      const ny_operator_def_t *def);

static bool tp_contract_type_name_matches(const char *actual,
                                          const char *expected) {
  if (!actual || !expected)
    return false;
  if (strcmp(expected, "Fin<") == 0) {
    size_t len = strlen(actual);
    return len > 5 && strncmp(actual, "Fin<", 4) == 0 && actual[len - 1] == '>';
  }
  return strcmp(actual, expected) == 0;
}

static bool tp_contract_param_has_type(const stmt_func_t *fn, const char *name,
                                       const char *type_name) {
  if (!fn || !name)
    return false;
  for (size_t i = 0; i < fn->params.len; ++i) {
    const param_t *param = &fn->params.data[i];
    if (param->name && strcmp(param->name, name) == 0)
      return tp_contract_type_name_matches(param->type, type_name);
  }
  return false;
}

static bool tp_contract_has_custom_operator(ny_tp_ctx_t *tp, expr_t *expr);

static bool tp_contract_declares_name(const stmt_t *stmt, const char *name) {
  if (!stmt || !name)
    return false;
  if (stmt->kind == NY_S_VAR && stmt->as.var.is_decl) {
    for (size_t i = 0; i < stmt->as.var.names.len; ++i)
      if (strcmp(stmt->as.var.names.data[i], name) == 0)
        return true;
  }
  return stmt->kind == NY_S_GUARD && stmt->as.guard.name &&
         strcmp(stmt->as.guard.name, name) == 0;
}

static bool tp_contract_visible_typed_name(const tp_requires_proof_ctx_t *ctx,
                                           const stmt_func_t *fn,
                                           const expr_t *expr,
                                           const char *type_name) {
  const char *name = expr->as.ident.name;
  if (!name)
    return false;
  if (ctx && fn == ctx->current_fn) {
    for (size_t i = ctx->stmt_stack.len; i-- > 0;) {
      const stmt_t *stmt = ctx->stmt_stack.data[i].stmt;
      if (stmt->kind == NY_S_FUNC)
        break;
      if (stmt->kind == NY_S_FOR &&
          ((stmt->as.fr.iter_var && strcmp(stmt->as.fr.iter_var, name) == 0) ||
           (stmt->as.fr.iter_index_var &&
            strcmp(stmt->as.fr.iter_index_var, name) == 0))) {
        if (stmt->as.fr.iter_var && strcmp(stmt->as.fr.iter_var, name) == 0 &&
            stmt->as.fr.iterable && stmt->as.fr.iterable->kind == NY_E_BINARY &&
            stmt->as.fr.iterable->as.binary.op &&
            strcmp(stmt->as.fr.iterable->as.binary.op, "..") == 0 &&
            !tp_contract_has_custom_operator(ctx->tp,
                                             (expr_t *)stmt->as.fr.iterable) &&
            strcmp(type_name, "int") == 0)
          return true;
        return expr->semantic.resolved && expr->semantic.resolved_type_name &&
               tp_contract_type_name_matches(expr->semantic.resolved_type_name,
                                             type_name);
      }
      if (stmt->kind == NY_S_BLOCK && i + 1 < ctx->stmt_stack.len) {
        const stmt_t *child = ctx->stmt_stack.data[i + 1].stmt;
        size_t prior = 0;
        while (prior < stmt->as.block.body.len &&
               stmt->as.block.body.data[prior] != child)
          ++prior;
        while (prior > 0) {
          const stmt_t *declaration = stmt->as.block.body.data[--prior];
          if (tp_contract_declares_name(declaration, name))
            return expr->semantic.resolved &&
                   expr->semantic.resolved_type_name &&
                   tp_contract_type_name_matches(
                       expr->semantic.resolved_type_name, type_name);
        }
      }
      const stmt_t *init = stmt->kind == NY_S_WHILE ? stmt->as.whl.init
                           : stmt->kind == NY_S_FOR ? stmt->as.fr.init
                           : stmt->kind == NY_S_IF  ? stmt->as.iff.init
                                                    : NULL;
      if (tp_contract_declares_name(init, name))
        return expr->semantic.resolved && expr->semantic.resolved_type_name &&
               tp_contract_type_name_matches(expr->semantic.resolved_type_name,
                                             type_name);
    }
  }
  return tp_contract_param_has_type(fn, name, type_name);
}

static bool tp_contract_visible_int_name(const tp_requires_proof_ctx_t *ctx,
                                         const stmt_func_t *fn,
                                         const expr_t *expr) {
  return tp_contract_visible_typed_name(ctx, fn, expr, "int");
}

static bool tp_contract_visible_bool_name(const tp_requires_proof_ctx_t *ctx,
                                          const stmt_func_t *fn,
                                          const expr_t *expr) {
  return tp_contract_visible_typed_name(ctx, fn, expr, "bool");
}

static bool tp_contract_visible_fin_name(const tp_requires_proof_ctx_t *ctx,
                                         const stmt_func_t *fn,
                                         const expr_t *expr) {
  return tp_contract_visible_typed_name(ctx, fn, expr, "Fin<");
}

static bool tp_contract_visible_fin_bound(const tp_requires_proof_ctx_t *ctx,
                                          const stmt_func_t *fn,
                                          const expr_t *expr,
                                          int64_t *bound_out) {
  if (!expr || expr->kind != NY_E_IDENT || !bound_out ||
      !tp_contract_visible_fin_name(ctx, fn, expr))
    return false;
  const char *type_name = expr->semantic.resolved_type_name &&
                                  tp_contract_type_name_matches(
                                      expr->semantic.resolved_type_name, "Fin<")
                              ? expr->semantic.resolved_type_name
                              : NULL;
  if (!type_name && fn) {
    for (size_t i = 0; i < fn->params.len; ++i) {
      const param_t *param = &fn->params.data[i];
      if (param->name && expr->as.ident.name &&
          strcmp(param->name, expr->as.ident.name) == 0 &&
          tp_contract_type_name_matches(param->type, "Fin<")) {
        type_name = param->type;
        break;
      }
    }
  }
  return type_name &&
         ny_fin_resolve_bound(ctx && ctx->tp ? ctx->tp->cg : NULL, NULL, 0,
                              type_name, bound_out) &&
         *bound_out > 0;
}

static bool tp_contract_int_term(const tp_requires_proof_ctx_t *ctx,
                                 const stmt_func_t *fn, const expr_t *expr,
                                 size_t depth) {
  if (!expr || depth > 64)
    return false;
  if (expr->kind == NY_E_LITERAL)
    return expr->tok.kind != NY_T_NIL &&
           ny_literal_is_signed_i64(&expr->as.literal);
  if (expr->kind == NY_E_IDENT)
    return tp_contract_visible_int_name(ctx, fn, expr) ||
           tp_contract_visible_fin_name(ctx, fn, expr);
  if (expr->kind == NY_E_UNARY)
    return expr->as.unary.op && strcmp(expr->as.unary.op, "-") == 0 &&
           tp_contract_int_term(ctx, fn, expr->as.unary.right, depth + 1);
  if (expr->kind == NY_E_BINARY && expr->as.binary.op && expr->as.binary.left &&
      expr->as.binary.right) {
    const char *op = expr->as.binary.op;
    bool integer_operator = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                            strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                            strcmp(op, "%") == 0;
    return integer_operator &&
           tp_contract_int_term(ctx, fn, expr->as.binary.left, depth + 1) &&
           tp_contract_int_term(ctx, fn, expr->as.binary.right, depth + 1);
  }
  return false;
}

static bool tp_contract_int_formula(const tp_requires_proof_ctx_t *ctx,
                                    const stmt_func_t *fn, const expr_t *expr,
                                    size_t depth);

static bool tp_contract_bool_term(const tp_requires_proof_ctx_t *ctx,
                                  const stmt_func_t *fn, const expr_t *expr,
                                  size_t depth) {
  if (!expr || depth > 64)
    return false;
  if (expr->kind == NY_E_LITERAL)
    return expr->as.literal.kind == NY_LIT_BOOL;
  if (expr->kind == NY_E_IDENT)
    return tp_contract_visible_bool_name(ctx, fn, expr);
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      strcmp(expr->as.unary.op, "!") == 0)
    return tp_contract_bool_term(ctx, fn, expr->as.unary.right, depth + 1);
  if (expr->kind == NY_E_LOGICAL && expr->as.logical.op &&
      expr->as.logical.left && expr->as.logical.right &&
      (strcmp(expr->as.logical.op, "&&") == 0 ||
       strcmp(expr->as.logical.op, "||") == 0))
    return tp_contract_bool_term(ctx, fn, expr->as.logical.left, depth + 1) &&
           tp_contract_bool_term(ctx, fn, expr->as.logical.right, depth + 1);
  if (expr->kind == NY_E_BINARY && expr->as.binary.op && expr->as.binary.left &&
      expr->as.binary.right) {
    const char *op = expr->as.binary.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)
      return (tp_contract_bool_term(ctx, fn, expr->as.binary.left, depth + 1) &&
              tp_contract_bool_term(ctx, fn, expr->as.binary.right,
                                    depth + 1)) ||
             (tp_contract_int_term(ctx, fn, expr->as.binary.left, depth + 1) &&
              tp_contract_int_term(ctx, fn, expr->as.binary.right, depth + 1));
    if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, ">=") == 0)
      return tp_contract_int_term(ctx, fn, expr->as.binary.left, depth + 1) &&
             tp_contract_int_term(ctx, fn, expr->as.binary.right, depth + 1);
  }
  return false;
}

static bool tp_contract_int_formula(const tp_requires_proof_ctx_t *ctx,
                                    const stmt_func_t *fn, const expr_t *expr,
                                    size_t depth) {
  if (!expr || depth > 64)
    return false;
  if (expr->kind == NY_E_LITERAL)
    return expr->as.literal.kind == NY_LIT_BOOL;
  if (expr->kind == NY_E_IDENT)
    return tp_contract_visible_bool_name(ctx, fn, expr);
  if (expr->kind == NY_E_UNARY)
    return expr->as.unary.op && strcmp(expr->as.unary.op, "!") == 0 &&
           tp_contract_int_formula(ctx, fn, expr->as.unary.right, depth + 1);
  if (expr->kind == NY_E_LOGICAL && expr->as.logical.op &&
      expr->as.logical.left && expr->as.logical.right) {
    const char *op = expr->as.logical.op;
    bool logical_operator = strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
    return logical_operator &&
           tp_contract_int_formula(ctx, fn, expr->as.logical.left, depth + 1) &&
           tp_contract_int_formula(ctx, fn, expr->as.logical.right, depth + 1);
  }
  if (expr->kind == NY_E_BINARY && expr->as.binary.op && expr->as.binary.left &&
      expr->as.binary.right) {
    const char *op = expr->as.binary.op;
    bool relation = strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                    strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                    strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
    if (!relation)
      return false;
    bool integer_comparison =
        tp_contract_int_term(ctx, fn, expr->as.binary.left, depth + 1) &&
        tp_contract_int_term(ctx, fn, expr->as.binary.right, depth + 1);
    if (integer_comparison)
      return true;
    bool equality = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
    return equality &&
           tp_contract_bool_term(ctx, fn, expr->as.binary.left, depth + 1) &&
           tp_contract_bool_term(ctx, fn, expr->as.binary.right, depth + 1);
  }
  return false;
}

static size_t tp_contract_measure_arity(const tp_requires_proof_ctx_t *ctx,
                                        const stmt_func_t *fn,
                                        const expr_t *measure) {
  if (!measure)
    return 0;
  if (measure->kind != NY_E_TUPLE)
    return tp_contract_int_term(ctx, fn, measure, 0) ||
                   tp_contract_bool_term(ctx, fn, measure, 0)
               ? 1
               : 0;
  if (measure->as.list_like.len < 2)
    return 0;
  for (size_t i = 0; i < measure->as.list_like.len; ++i) {
    expr_t *component = measure->as.list_like.data[i];
    if (!tp_contract_int_term(ctx, fn, component, 0) &&
        !tp_contract_bool_term(ctx, fn, component, 0))
      return 0;
  }
  return measure->as.list_like.len;
}

static expr_t *tp_contract_measure_component(const expr_t *measure,
                                             size_t index) {
  if (!measure)
    return NULL;
  return measure->kind == NY_E_TUPLE ? (index < measure->as.list_like.len
                                            ? measure->as.list_like.data[index]
                                            : NULL)
         : index == 0                ? (expr_t *)measure
                                     : NULL;
}

static expr_t *tp_contract_make_binary(arena_t *arena, token_t tok,
                                       const char *op, expr_t *left,
                                       expr_t *right) {
  expr_t *expr = arena_alloc(arena, sizeof(*expr));
  if (!expr)
    return NULL;
  *expr = (expr_t){.kind = NY_E_BINARY, .tok = tok};
  expr->as.binary.op = op;
  expr->as.binary.left = left;
  expr->as.binary.right = right;
  return expr;
}

static expr_t *tp_contract_make_logical(arena_t *arena, token_t tok,
                                        const char *op, expr_t *left,
                                        expr_t *right) {
  expr_t *expr = arena_alloc(arena, sizeof(*expr));
  if (!expr)
    return NULL;
  *expr = (expr_t){.kind = NY_E_LOGICAL, .tok = tok};
  expr->as.logical.op = op;
  expr->as.logical.left = left;
  expr->as.logical.right = right;
  return expr;
}

/*
 * The independent integer proof kernels model booleans as 0/1. Lower typed
 * boolean atoms to an explicit comparison before sending contracts to them.
 */
static expr_t *tp_contract_bool_to_integer_formula(
    arena_t *arena, const tp_requires_proof_ctx_t *ctx, const stmt_func_t *fn,
    expr_t *expr, size_t depth) {
  if (!expr || depth > 64)
    return NULL;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_BOOL)
    return expr;
  if (expr->kind == NY_E_IDENT &&
      tp_contract_visible_bool_name(ctx, fn, expr)) {
    expr_t *zero = arena_alloc(arena, sizeof(*zero));
    if (!zero)
      return NULL;
    *zero = (expr_t){.kind = NY_E_LITERAL, .tok = expr->tok};
    zero->as.literal.kind = NY_LIT_INT;
    zero->as.literal.as.i = 0;
    return tp_contract_make_binary(arena, expr->tok, "!=", expr, zero);
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      strcmp(expr->as.unary.op, "!") == 0) {
    expr_t *inner = tp_contract_bool_to_integer_formula(
        arena, ctx, fn, expr->as.unary.right, depth + 1);
    expr_t *copy = inner ? arena_alloc(arena, sizeof(*copy)) : NULL;
    if (!copy)
      return NULL;
    *copy = *expr;
    copy->as.unary.right = inner;
    return copy;
  }
  if (expr->kind == NY_E_LOGICAL && expr->as.logical.op &&
      (strcmp(expr->as.logical.op, "&&") == 0 ||
       strcmp(expr->as.logical.op, "||") == 0)) {
    expr_t *left = tp_contract_bool_to_integer_formula(
        arena, ctx, fn, expr->as.logical.left, depth + 1);
    expr_t *right = tp_contract_bool_to_integer_formula(
        arena, ctx, fn, expr->as.logical.right, depth + 1);
    return left && right
               ? tp_contract_make_logical(arena, expr->tok, expr->as.logical.op,
                                          left, right)
               : NULL;
  }
  if (expr->kind == NY_E_BINARY && expr->as.binary.op &&
      (strcmp(expr->as.binary.op, "==") == 0 ||
       strcmp(expr->as.binary.op, "!=") == 0) &&
      tp_contract_bool_term(ctx, fn, expr->as.binary.left, depth + 1) &&
      tp_contract_bool_term(ctx, fn, expr->as.binary.right, depth + 1)) {
    expr_t *left = tp_contract_bool_to_integer_formula(
        arena, ctx, fn, expr->as.binary.left, depth + 1);
    expr_t *right = tp_contract_bool_to_integer_formula(
        arena, ctx, fn, expr->as.binary.right, depth + 1);
    return left && right
               ? tp_contract_make_binary(arena, expr->tok, expr->as.binary.op,
                                         left, right)
               : NULL;
  }
  return expr;
}

static bool tp_contract_bool_literal(const expr_t *expr, bool *value) {
  if (!expr || expr->kind != NY_E_LITERAL ||
      expr->as.literal.kind != NY_LIT_BOOL)
    return false;
  if (value)
    *value = expr->as.literal.as.b;
  return true;
}

static expr_t *tp_contract_bool_constant(arena_t *arena, token_t tok,
                                         bool value) {
  expr_t *expr = arena_alloc(arena, sizeof(*expr));
  if (!expr)
    return NULL;
  *expr = (expr_t){.kind = NY_E_LITERAL, .tok = tok};
  expr->as.literal.kind = NY_LIT_BOOL;
  expr->as.literal.as.b = value;
  return expr;
}

static expr_t *tp_contract_bool_not(arena_t *arena, token_t tok,
                                    expr_t *value) {
  expr_t *expr = arena_alloc(arena, sizeof(*expr));
  if (!expr)
    return NULL;
  *expr = (expr_t){.kind = NY_E_UNARY, .tok = tok};
  expr->as.unary.op = "!";
  expr->as.unary.right = value;
  return expr;
}

static expr_t *tp_contract_bool_equal(arena_t *arena, token_t tok, expr_t *left,
                                      expr_t *right) {
  bool constant = false;
  if (tp_contract_bool_literal(left, &constant))
    return constant ? right : tp_contract_bool_not(arena, tok, right);
  if (tp_contract_bool_literal(right, &constant))
    return constant ? left : tp_contract_bool_not(arena, tok, left);
  expr_t *both_true = tp_contract_make_logical(arena, tok, "&&", left, right);
  expr_t *both_false = tp_contract_make_logical(
      arena, tok, "&&", tp_contract_bool_not(arena, tok, left),
      tp_contract_bool_not(arena, tok, right));
  return both_true && both_false
             ? tp_contract_make_logical(arena, tok, "||", both_true, both_false)
             : NULL;
}

/*
 * Lexicographic order over bounded integer and finite boolean components.
 */
static expr_t *tp_contract_lex_decrease(arena_t *arena, token_t tok,
                                        const tp_requires_proof_ctx_t *ctx,
                                        const stmt_func_t *fn,
                                        const expr_t *next,
                                        const expr_t *current, size_t arity) {
  expr_t *lex = NULL;
  for (size_t i = arity; i-- > 0;) {
    expr_t *next_i = tp_contract_measure_component(next, i);
    expr_t *current_i = tp_contract_measure_component(current, i);
    bool next_bool = tp_contract_bool_term(ctx, fn, next_i, 0);
    bool current_bool = tp_contract_bool_term(ctx, fn, current_i, 0);
    if (next_bool != current_bool)
      return NULL;
    expr_t *less = NULL;
    if (current_bool) {
      bool next_value = false, current_value = false;
      bool next_constant = tp_contract_bool_literal(next_i, &next_value);
      bool current_constant =
          tp_contract_bool_literal(current_i, &current_value);
      if (next_constant && current_constant)
        less =
            tp_contract_bool_constant(arena, tok, !next_value && current_value);
      else if (next_constant)
        less = next_value ? tp_contract_bool_constant(arena, tok, false)
                          : current_i;
      else if (current_constant)
        less = current_value ? tp_contract_bool_not(arena, tok, next_i)
                             : tp_contract_bool_constant(arena, tok, false);
      else
        less = tp_contract_make_logical(
            arena, tok, "&&", tp_contract_bool_not(arena, tok, next_i),
            current_i);
    } else if (tp_contract_int_term(ctx, fn, next_i, 0) &&
               tp_contract_int_term(ctx, fn, current_i, 0)) {
      less = tp_contract_make_binary(arena, tok, "<", next_i, current_i);
    }
    if (!less)
      return NULL;
    if (lex) {
      expr_t *equal =
          current_bool
              ? tp_contract_bool_equal(arena, tok, next_i, current_i)
              : tp_contract_make_binary(arena, tok, "==", next_i, current_i);
      expr_t *tie =
          equal ? tp_contract_make_logical(arena, tok, "&&", equal, lex) : NULL;
      lex = tie ? tp_contract_make_logical(arena, tok, "||", less, tie) : NULL;
      if (!lex)
        return NULL;
    } else {
      lex = less;
    }
  }
  return lex;
}

static expr_t *tp_contract_measure_lower_bound(
    arena_t *arena, token_t tok, const tp_requires_proof_ctx_t *ctx,
    const stmt_func_t *fn, const expr_t *measure, size_t arity) {
  expr_t *minimum = arena_alloc(arena, sizeof(*minimum));
  if (!minimum)
    return NULL;
  *minimum = (expr_t){.kind = NY_E_LITERAL, .tok = tok};
  minimum->as.literal.kind = NY_LIT_INT;
  minimum->as.literal.as.i = INT64_MIN;
  expr_t *bound = NULL;
  for (size_t i = 0; i < arity; ++i) {
    expr_t *component = tp_contract_measure_component(measure, i);
    if (tp_contract_bool_term(ctx, fn, component, 0))
      continue;
    int64_t fin_bound = 0;
    if (tp_contract_visible_fin_bound(ctx, fn, component, &fin_bound))
      continue;
    if (!tp_contract_int_term(ctx, fn, component, 0))
      return NULL;
    expr_t *above_min =
        tp_contract_make_binary(arena, tok, ">", component, minimum);
    if (!above_min)
      return NULL;
    bound = bound ? tp_contract_make_logical(arena, tok, "&&", bound, above_min)
                  : above_min;
    if (!bound)
      return NULL;
  }
  if (bound)
    return bound;
  expr_t *true_value = arena_alloc(arena, sizeof(*true_value));
  if (!true_value)
    return NULL;
  *true_value = (expr_t){.kind = NY_E_LITERAL, .tok = tok};
  true_value->as.literal.kind = NY_LIT_BOOL;
  true_value->as.literal.as.b = true;
  return true_value;
}

/*
 * Return a supported over-approximation of a control-flow guard. Dropping an
 * unsupported conjunct is sound because the original guard still implies
 * every retained conjunct. For disjunctions, both sides must be supported;
 * otherwise the unsupported side could make the whole guard true.
 */
static expr_t *tp_contract_guard_fact(tp_requires_proof_ctx_t *ctx,
                                      const stmt_func_t *fn, expr_t *guard,
                                      size_t depth) {
  if (!ctx || !ctx->tp || !ctx->tp->cg || !guard || depth > 64)
    return NULL;
  if (tp_contract_int_formula(ctx, fn, guard, 0))
    return guard;
  if (guard->kind != NY_E_LOGICAL || !guard->as.logical.op ||
      !guard->as.logical.left || !guard->as.logical.right)
    return NULL;
  bool is_and = strcmp(guard->as.logical.op, "&&") == 0;
  bool is_or = strcmp(guard->as.logical.op, "||") == 0;
  if (!is_and && !is_or)
    return NULL;
  expr_t *left =
      tp_contract_guard_fact(ctx, fn, guard->as.logical.left, depth + 1);
  expr_t *right =
      tp_contract_guard_fact(ctx, fn, guard->as.logical.right, depth + 1);
  if (is_and && (!left || !right))
    return left ? left : right;
  if (!left || !right)
    return NULL;
  expr_t *combined = arena_alloc(ctx->tp->cg->arena, sizeof(*combined));
  if (!combined)
    return NULL;
  *combined = (expr_t){.kind = NY_E_LOGICAL, .tok = guard->tok};
  combined->as.logical.op = is_and ? "&&" : "||";
  combined->as.logical.left = left;
  combined->as.logical.right = right;
  return combined;
}

static bool tp_contract_decide(tp_requires_proof_ctx_t *ctx, expr_t *obligation,
                               ny_proof_policy_result_t *result);

static bool tp_contract_range_endpoint(const tp_requires_proof_ctx_t *ctx,
                                       const stmt_func_t *fn,
                                       const expr_t *expr) {
  if (!expr || (expr->kind != NY_E_IDENT && expr->kind != NY_E_LITERAL))
    return false;
  return tp_contract_int_term(ctx, fn, expr, 0);
}

static bool tp_contract_range_upper_is_safe(tp_requires_proof_ctx_t *ctx,
                                            const stmt_func_t *fn,
                                            expr_t *upper) {
  if (!ctx || !upper)
    return false;
  if (upper->kind == NY_E_LITERAL)
    return ny_literal_is_signed_i64(&upper->as.literal) &&
           upper->as.literal.as.i < INT64_MAX;
  int64_t fin_bound = 0;
  if (tp_contract_visible_fin_bound(ctx, fn, upper, &fin_bound))
    return fin_bound < INT64_MAX;

  expr_t *maximum = arena_alloc(ctx->tp->cg->arena, sizeof(*maximum));
  if (!maximum)
    return false;
  *maximum = (expr_t){.kind = NY_E_LITERAL, .tok = upper->tok};
  maximum->as.literal.kind = NY_LIT_INT;
  maximum->as.literal.as.i = INT64_MAX;
  expr_t *no_increment_overflow = tp_contract_make_binary(
      ctx->tp->cg->arena, upper->tok, "<", upper, maximum);
  ny_proof_policy_result_t result = {0};
  return no_increment_overflow &&
         tp_contract_decide(ctx, no_increment_overflow, &result) &&
         result.verdict == NY_PROOF_VERDICT_PROVED;
}

static expr_t *tp_contract_iterable_range_fact(tp_requires_proof_ctx_t *ctx,
                                               const stmt_func_t *fn,
                                               const stmt_t *loop) {
  if (!ctx || !ctx->tp || !ctx->tp->cg || !loop || loop->kind != NY_S_FOR ||
      !loop->as.fr.iter_var || !loop->as.fr.iterable)
    return NULL;
  expr_t *range = loop->as.fr.iterable;
  if (range->kind != NY_E_BINARY || !range->as.binary.op ||
      strcmp(range->as.binary.op, "..") != 0 || !range->as.binary.left ||
      !range->as.binary.right ||
      !tp_contract_range_endpoint(ctx, fn, range->as.binary.left) ||
      !tp_contract_range_endpoint(ctx, fn, range->as.binary.right) ||
      !tp_contract_range_upper_is_safe(ctx, fn, range->as.binary.right) ||
      tp_contract_has_custom_operator(ctx->tp, range))
    return NULL;

  expr_t *index = arena_alloc(ctx->tp->cg->arena, sizeof(*index));
  if (!index)
    return NULL;
  *index = (expr_t){.kind = NY_E_IDENT, .tok = range->tok};
  index->as.ident.name = loop->as.fr.iter_var;
  index->semantic.resolved = true;
  index->semantic.resolved_type_name = "int";

  expr_t *at_or_above_start = tp_contract_make_binary(
      ctx->tp->cg->arena, range->tok, ">=", index, range->as.binary.left);
  expr_t *at_or_below_end = tp_contract_make_binary(
      ctx->tp->cg->arena, range->tok, "<=", index, range->as.binary.right);
  if (!at_or_above_start || !at_or_below_end)
    return NULL;
  return tp_contract_make_logical(ctx->tp->cg->arena, range->tok, "&&",
                                  at_or_above_start, at_or_below_end);
}

static bool tp_contract_mentions_name_depth(const expr_t *expr,
                                            const char *name, size_t depth) {
  if (!expr || !name)
    return false;
  if (depth > 128)
    return true;
  if (expr->kind == NY_E_IDENT)
    return expr->as.ident.name && strcmp(expr->as.ident.name, name) == 0;
  if (expr->kind == NY_E_UNARY)
    return tp_contract_mentions_name_depth(expr->as.unary.right, name,
                                           depth + 1);
  if (expr->kind == NY_E_BINARY)
    return tp_contract_mentions_name_depth(expr->as.binary.left, name,
                                           depth + 1) ||
           tp_contract_mentions_name_depth(expr->as.binary.right, name,
                                           depth + 1);
  if (expr->kind == NY_E_LOGICAL)
    return tp_contract_mentions_name_depth(expr->as.logical.left, name,
                                           depth + 1) ||
           tp_contract_mentions_name_depth(expr->as.logical.right, name,
                                           depth + 1);
  if (expr->kind == NY_E_MEMBER)
    return tp_contract_mentions_name_depth(expr->as.member.target, name,
                                           depth + 1);
  if (expr->kind == NY_E_CALL) {
    if (tp_contract_mentions_name_depth(expr->as.call.callee, name, depth + 1))
      return true;
    for (size_t i = 0; i < expr->as.call.args.len; ++i)
      if (tp_contract_mentions_name_depth(expr->as.call.args.data[i].val, name,
                                          depth + 1))
        return true;
  }
  if (expr->kind == NY_E_MEMCALL) {
    if (tp_contract_mentions_name_depth(expr->as.memcall.target, name,
                                        depth + 1))
      return true;
    for (size_t i = 0; i < expr->as.memcall.args.len; ++i)
      if (tp_contract_mentions_name_depth(expr->as.memcall.args.data[i].val,
                                          name, depth + 1))
        return true;
  }
  if (expr->kind == NY_E_TERNARY)
    return tp_contract_mentions_name_depth(expr->as.ternary.cond, name,
                                           depth + 1) ||
           tp_contract_mentions_name_depth(expr->as.ternary.true_expr, name,
                                           depth + 1) ||
           tp_contract_mentions_name_depth(expr->as.ternary.false_expr, name,
                                           depth + 1);
  if (expr->kind == NY_E_TUPLE) {
    for (size_t i = 0; i < expr->as.list_like.len; ++i)
      if (tp_contract_mentions_name_depth(expr->as.list_like.data[i], name,
                                          depth + 1))
        return true;
  }
  return false;
}

static bool tp_contract_mentions_name(const expr_t *expr, const char *name) {
  return tp_contract_mentions_name_depth(expr, name, 0);
}

static fun_sig *tp_contract_attached_call_sig(codegen_t *cg,
                                              const expr_t *call) {
  if (!cg || !call ||
      call->semantic.member_call_kind != NY_SEM_CALL_DIRECT_ATTACHED ||
      !call->semantic.canonical_callee)
    return NULL;
  fun_sig *sig = lookup_fun_exact(cg, call->semantic.canonical_callee);
  return sig && sig->stmt_t == call->semantic.canonical_callee_stmt &&
                 sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC
             ? sig
             : NULL;
}

static const char *tp_contract_member_name(const expr_t *call) {
  if (!call)
    return NULL;
  if (call->kind == NY_E_MEMCALL)
    return call->as.memcall.name;
  if (call->kind == NY_E_CALL && call->as.call.callee &&
      call->as.call.callee->kind == NY_E_MEMBER)
    return call->as.call.callee->as.member.name;
  return NULL;
}

static fun_sig *tp_contract_unique_attached_sig(codegen_t *cg,
                                                const char *member_name) {
  if (!cg || !member_name || !*member_name)
    return NULL;
  /*
   * Contract expressions may not yet carry semantic call-resolution facts.
   * Only recover a method by its leaf when that leaf names one attached
   * declaration in the compilation; overloaded leaves stay undecided.
   */
  fun_sig *match = NULL;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *candidate = &cg->fun_sigs.data[i];
    if (!candidate->is_attached_method || !candidate->name)
      continue;
    const char *leaf = strrchr(candidate->name, '.');
    leaf = leaf ? leaf + 1 : candidate->name;
    if (strcmp(leaf, member_name) != 0)
      continue;
    if (match)
      return NULL;
    match = candidate;
  }
  return match;
}

static expr_t *tp_contract_attached_receiver(codegen_t *cg, expr_t *call) {
  if (!call)
    return NULL;
  expr_t *target = call->kind == NY_E_MEMCALL ? call->as.memcall.target
                   : call->kind == NY_E_CALL && call->as.call.callee &&
                           call->as.call.callee->kind == NY_E_MEMBER
                       ? call->as.call.callee->as.member.target
                       : NULL;
  fun_sig *sig = NULL;
  if (call->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED)
    sig = tp_contract_attached_call_sig(cg, call);
  else if (call->semantic.member_call_kind == NY_SEM_CALL_NONE)
    sig = tp_contract_unique_attached_sig(cg, tp_contract_member_name(call));
  bool attached = sig && sig->is_attached_method;
  if (!target || !attached)
    return NULL;
  char module_path[1024];
  return ny_resolve_module_expr_path(cg, NULL, 0, target, module_path,
                                     sizeof(module_path))
             ? NULL
             : target;
}

static bool tp_contract_caller_declares_name(const tp_requires_proof_ctx_t *ctx,
                                             const char *name);

static fun_sig *tp_contract_global_callable_sig(codegen_t *cg, const char *name,
                                                int arity, size_t depth) {
  if (!cg || !name || depth > 32)
    return NULL;
  fun_sig *direct = lookup_fun(cg, name, arity);
  if (direct)
    return direct;
  for (size_t i = 0; cg->prog && i < cg->prog->body.len; ++i) {
    stmt_t *global = cg->prog->body.data[i];
    if (!global || global->kind != NY_S_VAR || !global->as.var.is_decl ||
        global->as.var.is_mut || global->as.var.is_del ||
        global->as.var.is_destructure || global->as.var.names.len != 1 ||
        global->as.var.exprs.len != 1 || !global->as.var.names.data[0] ||
        strcmp(global->as.var.names.data[0], name) != 0)
      continue;
    expr_t *initializer = global->as.var.exprs.data[0];
    return initializer && initializer->kind == NY_E_IDENT &&
                   initializer->as.ident.name
               ? tp_contract_global_callable_sig(cg, initializer->as.ident.name,
                                                 arity, depth + 1)
               : NULL;
  }
  for (size_t i = 0; i < cg->global_vars.len; ++i) {
    binding *global = &cg->global_vars.data[i];
    if (global->name && strcmp(global->name, name) == 0)
      return !global->is_mut ? global->direct_callable_sig : NULL;
  }
  return NULL;
}

typedef struct {
  const char *name;
  stmt_t *declaration;
  size_t matches;
  bool supported;
} tp_contract_local_callable_scan_t;

static bool tp_contract_local_callable_scan_stmt(ny_visitor_t *visitor,
                                                 stmt_t *stmt) {
  tp_contract_local_callable_scan_t *scan = visitor ? visitor->ctx : NULL;
  if (!scan || !stmt)
    return true;
  if (stmt->kind == NY_S_FUNC)
    return false;
  if (stmt->kind != NY_S_VAR || !stmt->as.var.is_decl)
    return true;
  for (size_t i = 0; i < stmt->as.var.names.len; ++i) {
    const char *name = stmt->as.var.names.data[i];
    if (!name || strcmp(name, scan->name) != 0)
      continue;
    ++scan->matches;
    scan->declaration = stmt;
    if (stmt->as.var.is_mut || stmt->as.var.is_del ||
        stmt->as.var.is_destructure || stmt->as.var.names.len != 1 ||
        stmt->as.var.exprs.len != 1 || !stmt->as.var.exprs.data[0])
      scan->supported = false;
  }
  return true;
}

static fun_sig *tp_contract_callable_sig(codegen_t *cg,
                                         const stmt_func_t *context_fn,
                                         const char *name, int arity,
                                         size_t depth) {
  if (!cg || !name || !*name || depth > 32)
    return NULL;
  if (context_fn) {
    tp_requires_proof_ctx_t context = {.current_fn = context_fn};
    if (tp_contract_caller_declares_name(&context, name)) {
      if (!context_fn->body)
        return NULL;
      tp_contract_local_callable_scan_t scan = {.name = name,
                                                .supported = true};
      ny_visitor_t visitor = {
          .ctx = &scan, .visit_stmt_pre = tp_contract_local_callable_scan_stmt};
      ny_visit_stmt(&visitor, context_fn->body);
      if (!scan.supported || scan.matches != 1 || !scan.declaration)
        return NULL;
      expr_t *initializer = scan.declaration->as.var.exprs.data[0];
      if (!initializer || initializer->kind != NY_E_IDENT ||
          !initializer->as.ident.name)
        return NULL;
      return tp_contract_callable_sig(
          cg, context_fn, initializer->as.ident.name, arity, depth + 1);
    }
  }
  fun_sig *direct = lookup_fun(cg, name, arity);
  if (direct)
    return direct;
  return context_fn ? tp_contract_global_callable_sig(cg, name, arity, 0)
                    : NULL;
}

static fun_sig *tp_contract_direct_call_sig(codegen_t *cg,
                                            const stmt_func_t *context_fn,
                                            const expr_t *call) {
  if (!cg || !call)
    return NULL;
  const expr_t *member_target = NULL;
  const char *member_name = NULL;
  if (call->kind == NY_E_CALL) {
    const expr_t *callee = call->as.call.callee;
    if (!callee)
      return NULL;
    if (callee->kind == NY_E_IDENT && callee->as.ident.name) {
      return tp_contract_callable_sig(cg, context_fn, callee->as.ident.name,
                                      call->as.call.args.len, 0);
    }
    if (callee->kind != NY_E_MEMBER)
      return NULL;
    member_target = callee->as.member.target;
    member_name = callee->as.member.name;
  } else if (call->kind == NY_E_MEMCALL) {
    member_target = call->as.memcall.target;
    member_name = call->as.memcall.name;
  } else {
    return NULL;
  }
  if (!member_target || !member_name)
    return NULL;

  char module_path[1024];
  bool is_module = ny_resolve_module_expr_path(
      cg, NULL, 0, (expr_t *)member_target, module_path, sizeof(module_path));
  if (!is_module) {
    fun_sig *attached = tp_contract_attached_call_sig(cg, call);
    if (attached)
      return attached;
    if (call->semantic.member_call_kind != NY_SEM_CALL_NONE)
      return NULL;
    return tp_contract_unique_attached_sig(cg, tp_contract_member_name(call));
  }
  char resolved_fun[1280];
  if (!ny_resolve_module_function_path(cg, module_path, member_name,
                                       resolved_fun, sizeof(resolved_fun))) {
    ny_proof_debug_at(2, call->tok,
                      "pure predicate expansion skipped: module function "
                      "does not resolve");
    return NULL;
  }
  fun_sig *sig = lookup_fun(cg, resolved_fun, 0);
  if (!sig)
    ny_proof_debug_at(2, call->tok,
                      "pure predicate expansion skipped: no signature for "
                      "resolved module function");
  return sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC ? sig : NULL;
}

static bool tp_contract_calls_are_pure_depth(codegen_t *cg,
                                             const stmt_func_t *context_fn,
                                             const expr_t *expr, size_t depth,
                                             bool *saw_call) {
  if (!expr)
    return true;
  if (depth > 128)
    return false;
  switch (expr->kind) {
  case NY_E_IDENT:
  case NY_E_LITERAL:
    return true;
  case NY_E_CALL: {
    *saw_call = true;
    /*
     * The interpreter executes the predicate while proving a compile-time
     * call. Admit only direct identifier, qualified-module, or uniquely
     * resolved attached-method calls with a completed pure/effect-free
     * summary; unknown and indirect calls stay fail-closed.
     */
    fun_sig *sig = tp_contract_direct_call_sig(cg, context_fn, expr);
    if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
        !sig->is_pure || !sig->effects_known || sig->effects != NY_FX_NONE)
      return false;
    expr_t *receiver = tp_contract_attached_receiver(cg, (expr_t *)expr);
    if (receiver && !tp_contract_calls_are_pure_depth(cg, context_fn, receiver,
                                                      depth + 1, saw_call))
      return false;
    for (size_t i = 0; i < expr->as.call.args.len; ++i)
      if (!tp_contract_calls_are_pure_depth(cg, context_fn,
                                            expr->as.call.args.data[i].val,
                                            depth + 1, saw_call))
        return false;
    return true;
  }
  case NY_E_UNARY:
    return tp_contract_calls_are_pure_depth(
        cg, context_fn, expr->as.unary.right, depth + 1, saw_call);
  case NY_E_BINARY:
    return tp_contract_calls_are_pure_depth(
               cg, context_fn, expr->as.binary.left, depth + 1, saw_call) &&
           tp_contract_calls_are_pure_depth(
               cg, context_fn, expr->as.binary.right, depth + 1, saw_call);
  case NY_E_LOGICAL:
    return tp_contract_calls_are_pure_depth(
               cg, context_fn, expr->as.logical.left, depth + 1, saw_call) &&
           tp_contract_calls_are_pure_depth(
               cg, context_fn, expr->as.logical.right, depth + 1, saw_call);
  case NY_E_MEMBER:
    return tp_contract_calls_are_pure_depth(
               cg, context_fn, expr->as.member.target, depth + 1, saw_call) &&
           tp_contract_calls_are_pure_depth(cg, context_fn,
                                            expr->as.member.computed_name,
                                            depth + 1, saw_call);
  case NY_E_MEMCALL:
    *saw_call = true;
    {
      fun_sig *sig = tp_contract_direct_call_sig(cg, context_fn, expr);
      if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
          !sig->is_pure || !sig->effects_known || sig->effects != NY_FX_NONE)
        return false;
    }
    expr_t *receiver = tp_contract_attached_receiver(cg, (expr_t *)expr);
    if (receiver && !tp_contract_calls_are_pure_depth(cg, context_fn, receiver,
                                                      depth + 1, saw_call))
      return false;
    if (!expr->as.memcall.args.data && expr->as.memcall.args.len != 0)
      return false;
    for (size_t i = 0; i < expr->as.memcall.args.len; ++i)
      if (!tp_contract_calls_are_pure_depth(cg, context_fn,
                                            expr->as.memcall.args.data[i].val,
                                            depth + 1, saw_call))
        return false;
    return true;
  case NY_E_TERNARY:
    return tp_contract_calls_are_pure_depth(
               cg, context_fn, expr->as.ternary.cond, depth + 1, saw_call) &&
           tp_contract_calls_are_pure_depth(cg, context_fn,
                                            expr->as.ternary.true_expr,
                                            depth + 1, saw_call) &&
           tp_contract_calls_are_pure_depth(cg, context_fn,
                                            expr->as.ternary.false_expr,
                                            depth + 1, saw_call);
  case NY_E_TUPLE:
    for (size_t i = 0; i < expr->as.list_like.len; ++i)
      if (!tp_contract_calls_are_pure_depth(
              cg, context_fn, expr->as.list_like.data[i], depth + 1, saw_call))
        return false;
    return true;
  default:
    return false;
  }
}

static expr_t *tp_contract_match_value(arena_t *arena, const expr_t *match,
                                       const stmt_func_t *fn, size_t depth);

static expr_t *tp_contract_lower_nested_matches(arena_t *arena,
                                                const expr_t *expr,
                                                const stmt_func_t *fn,
                                                size_t depth) {
  if (!expr || depth > 64)
    return NULL;
  if (expr->kind == NY_E_MATCH) {
    expr_t *lowered = tp_contract_match_value(arena, expr, fn, depth + 1);
    return lowered && lowered != expr
               ? tp_contract_lower_nested_matches(arena, lowered, fn, depth + 1)
               : NULL;
  }
  expr_t *left = NULL;
  expr_t *right = NULL;
  expr_t *third = NULL;
  switch (expr->kind) {
  case NY_E_UNARY:
    left = tp_contract_lower_nested_matches(arena, expr->as.unary.right, fn,
                                            depth + 1);
    if (!left)
      return NULL;
    if (left == expr->as.unary.right)
      return (expr_t *)expr;
    break;
  case NY_E_BINARY:
    left = tp_contract_lower_nested_matches(arena, expr->as.binary.left, fn,
                                            depth + 1);
    right = tp_contract_lower_nested_matches(arena, expr->as.binary.right, fn,
                                             depth + 1);
    if (!left || !right)
      return NULL;
    if (left == expr->as.binary.left && right == expr->as.binary.right)
      return (expr_t *)expr;
    break;
  case NY_E_LOGICAL:
    left = tp_contract_lower_nested_matches(arena, expr->as.logical.left, fn,
                                            depth + 1);
    right = tp_contract_lower_nested_matches(arena, expr->as.logical.right, fn,
                                             depth + 1);
    if (!left || !right)
      return NULL;
    if (left == expr->as.logical.left && right == expr->as.logical.right)
      return (expr_t *)expr;
    break;
  case NY_E_TERNARY:
    left = tp_contract_lower_nested_matches(arena, expr->as.ternary.cond, fn,
                                            depth + 1);
    right = tp_contract_lower_nested_matches(arena, expr->as.ternary.true_expr,
                                             fn, depth + 1);
    third = tp_contract_lower_nested_matches(arena, expr->as.ternary.false_expr,
                                             fn, depth + 1);
    if (!left || !right || !third)
      return NULL;
    if (left == expr->as.ternary.cond && right == expr->as.ternary.true_expr &&
        third == expr->as.ternary.false_expr)
      return (expr_t *)expr;
    break;
  case NY_E_CALL: {
    expr_t *callee = expr->as.call.callee
                         ? tp_contract_lower_nested_matches(
                               arena, expr->as.call.callee, fn, depth + 1)
                         : NULL;
    if (expr->as.call.callee && !callee)
      return NULL;
    bool changed = callee != expr->as.call.callee;
    if (expr->as.call.args.len) {
      if (!expr->as.call.args.data)
        return NULL;
      for (size_t i = 0; i < expr->as.call.args.len; ++i) {
        expr_t *arg = tp_contract_lower_nested_matches(
            arena, expr->as.call.args.data[i].val, fn, depth + 1);
        if (!arg)
          return NULL;
        changed |= arg != expr->as.call.args.data[i].val;
      }
    }
    if (!changed)
      return (expr_t *)expr;
    call_arg_t *args = NULL;
    if (expr->as.call.args.len) {
      args = arena_alloc(arena, expr->as.call.args.len *
                                    sizeof(*expr->as.call.args.data));
      if (!args)
        return NULL;
      for (size_t i = 0; i < expr->as.call.args.len; ++i) {
        args[i] = expr->as.call.args.data[i];
        args[i].val =
            tp_contract_lower_nested_matches(arena, args[i].val, fn, depth + 1);
        if (!args[i].val)
          return NULL;
      }
    }
    expr_t *copy = arena_alloc(arena, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *expr;
    copy->as.call.callee = callee;
    if (args)
      copy->as.call.args.data = args;
    return copy;
  }
  case NY_E_MEMCALL: {
    expr_t *target = expr->as.memcall.target
                         ? tp_contract_lower_nested_matches(
                               arena, expr->as.memcall.target, fn, depth + 1)
                         : NULL;
    if (expr->as.memcall.target && !target)
      return NULL;
    bool changed = target != expr->as.memcall.target;
    if (expr->as.memcall.args.len) {
      if (!expr->as.memcall.args.data)
        return NULL;
      for (size_t i = 0; i < expr->as.memcall.args.len; ++i) {
        expr_t *arg = tp_contract_lower_nested_matches(
            arena, expr->as.memcall.args.data[i].val, fn, depth + 1);
        if (!arg)
          return NULL;
        changed |= arg != expr->as.memcall.args.data[i].val;
      }
    }
    if (!changed)
      return (expr_t *)expr;
    call_arg_t *args = NULL;
    if (expr->as.memcall.args.len) {
      args = arena_alloc(arena, expr->as.memcall.args.len *
                                    sizeof(*expr->as.memcall.args.data));
      if (!args)
        return NULL;
      for (size_t i = 0; i < expr->as.memcall.args.len; ++i) {
        args[i] = expr->as.memcall.args.data[i];
        args[i].val =
            tp_contract_lower_nested_matches(arena, args[i].val, fn, depth + 1);
        if (!args[i].val)
          return NULL;
      }
    }
    expr_t *copy = arena_alloc(arena, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *expr;
    copy->as.memcall.target = target;
    if (args)
      copy->as.memcall.args.data = args;
    return copy;
  }
  case NY_E_MEMBER:
    left = expr->as.member.target
               ? tp_contract_lower_nested_matches(arena, expr->as.member.target,
                                                  fn, depth + 1)
               : NULL;
    right = expr->as.member.computed_name
                ? tp_contract_lower_nested_matches(
                      arena, expr->as.member.computed_name, fn, depth + 1)
                : NULL;
    if ((expr->as.member.target && !left) ||
        (expr->as.member.computed_name && !right))
      return NULL;
    if (left == expr->as.member.target &&
        right == expr->as.member.computed_name)
      return (expr_t *)expr;
    break;
  case NY_E_TUPLE: {
    if (expr->as.list_like.len && !expr->as.list_like.data)
      return NULL;
    bool changed = false;
    for (size_t i = 0; i < expr->as.list_like.len; ++i) {
      expr_t *item = tp_contract_lower_nested_matches(
          arena, expr->as.list_like.data[i], fn, depth + 1);
      if (!item)
        return NULL;
      changed |= item != expr->as.list_like.data[i];
    }
    if (!changed)
      return (expr_t *)expr;
    expr_t **items = arena_alloc(arena, expr->as.list_like.len *
                                            sizeof(*expr->as.list_like.data));
    if (!items)
      return NULL;
    for (size_t i = 0; i < expr->as.list_like.len; ++i) {
      items[i] = tp_contract_lower_nested_matches(
          arena, expr->as.list_like.data[i], fn, depth + 1);
      if (!items[i])
        return NULL;
    }
    expr_t *copy = arena_alloc(arena, sizeof(*copy));
    if (!copy)
      return NULL;
    *copy = *expr;
    copy->as.list_like.data = items;
    return copy;
  }
  default:
    return (expr_t *)expr;
  }
  expr_t *copy = arena_alloc(arena, sizeof(*copy));
  if (!copy)
    return NULL;
  *copy = *expr;
  switch (expr->kind) {
  case NY_E_UNARY:
    copy->as.unary.right = left;
    break;
  case NY_E_BINARY:
    copy->as.binary.left = left;
    copy->as.binary.right = right;
    break;
  case NY_E_LOGICAL:
    copy->as.logical.left = left;
    copy->as.logical.right = right;
    break;
  case NY_E_TERNARY:
    copy->as.ternary.cond = left;
    copy->as.ternary.true_expr = right;
    copy->as.ternary.false_expr = third;
    break;
  case NY_E_MEMBER:
    copy->as.member.target = left;
    copy->as.member.computed_name = right;
    break;
  default:
    return NULL;
  }
  return copy;
}

static bool tp_contract_calls_are_pure(codegen_t *cg,
                                       const stmt_func_t *context_fn,
                                       const expr_t *expr) {
  bool saw_call = false;
  return tp_contract_calls_are_pure_depth(cg, context_fn, expr, 0, &saw_call) &&
         saw_call;
}

static bool tp_contract_pure_expr_supported(
    codegen_t *cg, arena_t *arena, const stmt_func_t *fn, const expr_t *expr,
    expr_t **placeholders, char **placeholder_names, expr_t **arguments) {
  if (!cg || !arena || !fn || !expr || !placeholders || !placeholder_names ||
      !arguments)
    return false;
  expr_t *expanded = (expr_t *)expr;
  for (size_t i = 0; i < fn->params.len; ++i) {
    const char *name = fn->params.data[i].name;
    if (!name || !*name || !placeholders[i] || !placeholder_names[i] ||
        !arguments[i])
      return false;
    expanded = ny_expr_subst(arena, expanded, name, placeholders[i]);
    if (!expanded)
      return false;
  }
  for (size_t i = 0; i < fn->params.len; ++i) {
    expanded =
        ny_expr_subst(arena, expanded, placeholder_names[i], arguments[i]);
    if (!expanded)
      return false;
  }
  expanded = tp_contract_lower_nested_matches(arena, expanded, fn, 0);
  if (!expanded)
    return false;
  bool saw_call = false;
  return tp_contract_calls_are_pure_depth(cg, fn, expanded, 0, &saw_call);
}

static bool tp_contract_if_init_parts(const stmt_t *stmt, const char **name,
                                      expr_t **value) {
  if (name)
    *name = NULL;
  if (value)
    *value = NULL;
  if (!stmt || stmt->kind != NY_S_IF || !name || !value)
    return false;
  const stmt_t *init = stmt->as.iff.init;
  if (!init)
    return true;
  if (init->kind != NY_S_VAR || !init->as.var.is_decl || init->as.var.is_mut ||
      init->as.var.is_del || init->as.var.is_destructure ||
      init->as.var.names.len != 1 || init->as.var.exprs.len != 1 ||
      !init->as.var.names.data[0] || !init->as.var.exprs.data[0])
    return false;
  *name = init->as.var.names.data[0];
  *value = init->as.var.exprs.data[0];
  return true;
}

static bool tp_contract_pure_body_supported(codegen_t *cg, arena_t *arena,
                                            const stmt_func_t *fn,
                                            const stmt_t *stmt,
                                            expr_t **placeholders,
                                            char **placeholder_names,
                                            expr_t **arguments, size_t depth) {
  if (!cg || !fn || !stmt || depth > 64)
    return false;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!tp_contract_pure_body_supported(
              cg, arena, fn, stmt->as.block.body.data[i], placeholders,
              placeholder_names, arguments, depth + 1))
        return false;
    return true;
  case NY_S_VAR:
    if (!stmt->as.var.is_decl || stmt->as.var.is_mut || stmt->as.var.is_del ||
        stmt->as.var.is_destructure ||
        stmt->as.var.names.len != stmt->as.var.exprs.len)
      return false;
    for (size_t i = 0; i < stmt->as.var.exprs.len; ++i)
      if (!tp_contract_pure_expr_supported(
              cg, arena, fn, stmt->as.var.exprs.data[i], placeholders,
              placeholder_names, arguments))
        return false;
    return true;
  case NY_S_EXPR:
    return tp_contract_pure_expr_supported(cg, arena, fn, stmt->as.expr.expr,
                                           placeholders, placeholder_names,
                                           arguments);
  case NY_S_RETURN:
    return tp_contract_pure_expr_supported(cg, arena, fn, stmt->as.ret.value,
                                           placeholders, placeholder_names,
                                           arguments);
  case NY_S_IF: {
    const char *init_name = NULL;
    expr_t *init_value = NULL;
    return tp_contract_if_init_parts(stmt, &init_name, &init_value) &&
           (!init_value || tp_contract_pure_expr_supported(
                               cg, arena, fn, init_value, placeholders,
                               placeholder_names, arguments)) &&
           stmt->as.iff.test &&
           tp_contract_pure_expr_supported(cg, arena, fn, stmt->as.iff.test,
                                           placeholders, placeholder_names,
                                           arguments) &&
           tp_contract_pure_body_supported(cg, arena, fn, stmt->as.iff.conseq,
                                           placeholders, placeholder_names,
                                           arguments, depth + 1) &&
           (!stmt->as.iff.alt ||
            tp_contract_pure_body_supported(cg, arena, fn, stmt->as.iff.alt,
                                            placeholders, placeholder_names,
                                            arguments, depth + 1));
  }
  case NY_S_MATCH:
    if (!stmt->as.match.test || !tp_contract_pure_expr_supported(
                                    cg, arena, fn, stmt->as.match.test,
                                    placeholders, placeholder_names, arguments))
      return false;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      if ((arm->guard && !tp_contract_pure_expr_supported(
                             cg, arena, fn, arm->guard, placeholders,
                             placeholder_names, arguments)) ||
          !tp_contract_pure_body_supported(cg, arena, fn, arm->conseq,
                                           placeholders, placeholder_names,
                                           arguments, depth + 1))
        return false;
    }
    return !stmt->as.match.default_conseq ||
           tp_contract_pure_body_supported(
               cg, arena, fn, stmt->as.match.default_conseq, placeholders,
               placeholder_names, arguments, depth + 1);
  default:
    return false;
  }
}

static bool tp_contract_effect_row_open(const ny_type_t *effect_type) {
  ny_type_t *row = ny_type_find((ny_type_t *)effect_type);
  for (size_t depth = 0; row && depth < 128; ++depth) {
    if (row->kind != NY_TYPE_ROW)
      return row->kind == NY_TYPE_VAR || row->kind == NY_TYPE_EFFECT_VAR;
    if (!row->as.row.label) {
      row = ny_type_find(row->as.row.tail);
      continue;
    }
    row = ny_type_find(row->as.row.tail);
  }
  return false;
}

static const char *tp_callable_kind_name(const fun_sig *sig) {
  if (!sig->stmt_t) {
    /*
     * Statements-less registrations are runtime builtins and C imports.
     */
    if (sig->is_extern || sig->is_native_abi || sig->link_name)
      return "imported";
    return "builtin";
  }
  if (sig->stmt_t->kind == NY_S_EXTERN)
    return "extern";
  return "function";
}

static const char *tp_callable_unknown_reason(const fun_sig *sig) {
  if (!sig->stmt_t) {
    if (sig->is_extern || sig->is_native_abi || sig->link_name)
      return "imported";
    return "builtin";
  }
  if (sig->stmt_t->kind == NY_S_EXTERN)
    return "extern";
  if (sig->effect_type && tp_contract_effect_row_open(sig->effect_type))
    return "open callback tail";
  return "unresolved body effects";
}

static void tp_emit_callable_row_json(ny_tp_json_t *j, const fun_sig *sig) {
  uint32_t effects = NY_FX_NONE;
  ny_type_t *effect_type = NULL;
  bool known = false;
  if (sig->effects_known) {
    known = true;
    effects = sig->effects;
    effect_type = sig->effect_type;
  } else {
    const sema_func_t *sema = tp_sig_sema(sig);
    if (sema && sema->effects_known) {
      known = true;
      effects = sema->effects;
    } else if (ny_sig_callable_effect_facts(sig, &effects)) {
      known = true;
    }
  }
  const char *kind = tp_callable_kind_name(sig);
  tp_append(j, "{\"name\":");
  tp_json_str(j, sig->name ? sig->name : "");
  tp_append(j, ",\"owner\":");
  tp_json_str(j, sig->owner_type ? sig->owner_type : "");
  tp_append(j, ",\"kind\":");
  tp_json_str(j, kind);
  tp_append(j,
            ",\"arity\":%d,\"variadic\":%s,\"known\":%s,\"row\":", sig->arity,
            sig->is_variadic ? "true" : "false", known ? "true" : "false");
  if (!known) {
    tp_append(j, "null,\"reason\":");
    tp_json_str(j, tp_callable_unknown_reason(sig));
    tp_append(j, ",\"source_origin\":null,\"emission_origin\":null}");
    return;
  }
  ny_type_arena_t arena;
  ny_type_arena_init(&arena);
  ny_type_t *row =
      ny_type_effect_row_with_builtin_mask(&arena, effects, effect_type);
  char *serialized = row ? ny_type_effect_row_serialize(row) : NULL;
  if (serialized) {
    tp_json_str(j, serialized);
    free(serialized);
  } else {
    tp_append(j, "null");
  }
  ny_type_arena_reset(&arena);
  tp_append(j, ",\"reason\":null,\"source_origin\":");
  if (sig->stmt_t)
    tp_emit_token_origin_json(j, &sig->stmt_t->tok);
  else
    tp_append(j, "null");
  tp_append(j, ",\"emission_origin\":");
  if (sig->stmt_t)
    tp_emit_token_origin_json(j, sig->stmt_t->expansion_origin);
  else
    tp_append(j, "null");
  tp_append(j, "}");
}

/*
 * Unit-wide callable effect rows: every callable the analyzed unit registers
 * (its functions, externs, C imports, and runtime builtins) with the closed
 * effect row known at this stage, or `known:false` plus a machine-readable
 * `reason` ("extern", "builtin", "imported", "open callback tail",
 * "unresolved body effects") when the compiler has no closed row. A
 * known:false row is not a pure row. First-name registration wins, matching
 * call lookup semantics; statements outside the artifact's source scope are
 * skipped exactly like the typed function records.
 */
static void tp_emit_callable_rows_json(ny_tp_json_t *j,
                                       const ny_tp_ctx_t *ctx) {
  tp_append(j, "{\"schema\":\"nytrix.semantic.callable-effects\","
               "\"version\":1,\"items\":[");
  bool first = true;
  for (size_t i = 0; ctx && ctx->cg && i < ctx->cg->fun_sigs.len; ++i) {
    const fun_sig *sig = &ctx->cg->fun_sigs.data[i];
    if (!sig || !sig->name || !*sig->name)
      continue;
    if (sig->stmt_t && !tp_stmt_in_scope(ctx, sig->stmt_t))
      continue;
    bool duplicate = false;
    for (size_t k = 0; k < i; ++k) {
      const fun_sig *prev = &ctx->cg->fun_sigs.data[k];
      if (prev && prev->name && strcmp(prev->name, sig->name) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;
    if (!first)
      tp_append(j, ",");
    first = false;
    tp_emit_callable_row_json(j, sig);
  }
  tp_append(j, "]}");
}

typedef struct tp_proof_obligations_ctx_t {
  ny_tp_json_t *j;
  ny_tp_ctx_t *tp;
  bool first;
  VEC(const stmt_func_t *) fn_stack;
} tp_proof_obligations_ctx_t;

static const stmt_func_t *
tp_proof_enclosing_fn(const tp_proof_obligations_ctx_t *pc) {
  return pc && pc->fn_stack.len ? pc->fn_stack.data[pc->fn_stack.len - 1]
                                : NULL;
}

static void tp_proof_record_begin_json(tp_proof_obligations_ctx_t *pc,
                                       const char *kind) {
  if (!pc->first)
    tp_append(pc->j, ",");
  pc->first = false;
  tp_append(pc->j, "{\"kind\":");
  tp_json_str(pc->j, kind);
  const stmt_func_t *fn = tp_proof_enclosing_fn(pc);
  tp_append(pc->j, ",\"function\":");
  tp_json_str(pc->j, fn && fn->name ? fn->name : "");
}

static void
tp_proof_record_status_json(ny_tp_json_t *j, bool decided,
                            const ny_proof_policy_result_t *result) {
  const char *status = !decided ? "unchecked"
                       : result->verdict == NY_PROOF_VERDICT_PROVED
                           ? "proved"
                           : "not_proven";
  tp_append(j, ",\"status\":");
  tp_json_str(j, status);
  tp_append(j, ",\"verdict\":");
  if (decided && result->verdict == NY_PROOF_VERDICT_REFUTED)
    tp_json_str(j, "refuted");
  else if (decided && result->verdict == NY_PROOF_VERDICT_UNKNOWN)
    tp_json_str(j, "unknown");
  else
    tp_append(j, "null");
  tp_append(j, "}");
}

static void tp_emit_invariant_obligation_json(tp_proof_obligations_ctx_t *pc,
                                              stmt_t *whl) {
  expr_t *invariant = whl->as.whl.invariant;
  if (!invariant)
    return;
  ny_proof_policy_result_t result;
  memset(&result, 0, sizeof(result));
  bool decided = ny_proof_policy_decide(pc->tp->cg, NULL, 0, invariant, "",
                                        false, &result);
  tp_proof_record_begin_json(pc, "invariant");
  tp_append(pc->j, ",\"formula\":");
  tp_emit_obligation_formula_json(pc->j, invariant);
  tp_append(pc->j, ",\"source_origin\":");
  tp_emit_obligation_source_origin_json(pc->j, invariant);
  tp_append(pc->j, ",\"emission_origin\":");
  tp_emit_token_origin_json(pc->j, whl->expansion_origin);
  tp_proof_record_status_json(pc->j, decided, &result);
}

static void tp_emit_refinement_obligation_json(tp_proof_obligations_ctx_t *pc,
                                               stmt_t *var) {
  for (size_t i = 0; i < var->as.var.names.len && i < var->as.var.types.len &&
                     i < var->as.var.exprs.len;
       ++i) {
    const char *type_name = var->as.var.types.data[i];
    expr_t *init = var->as.var.exprs.data[i];
    if (!type_name || !*type_name || !init)
      continue;
    refine_def_t *ref = ny_lookup_refine(pc->tp->cg, type_name);
    if (!ref || !ref->predicate || !ref->param)
      continue;
    expr_t *inst =
        ny_expr_subst(pc->tp->cg->arena, ref->predicate, ref->param, init);
    if (!inst)
      continue;
    /*
     * Refinement predicates are contract-safe obligations, mirroring the
     * runtime check in ny_refine_eval_predicate.
     */
    ny_proof_policy_result_t result;
    memset(&result, 0, sizeof(result));
    bool decided =
        ny_proof_policy_decide(pc->tp->cg, NULL, 0, inst, "", true, &result);
    tp_proof_record_begin_json(pc, "refinement");
    tp_append(pc->j, ",\"name\":");
    tp_json_str(pc->j, ref->name ? ref->name : "");
    tp_append(pc->j, ",\"formula\":");
    tp_emit_obligation_formula_json(pc->j, inst);
    tp_append(pc->j, ",\"source_origin\":");
    tp_emit_obligation_source_origin_json(pc->j, init);
    tp_append(pc->j, ",\"emission_origin\":");
    tp_emit_token_origin_json(pc->j, var->expansion_origin);
    tp_proof_record_status_json(pc->j, decided, &result);
  }
}

static void tp_emit_fin_bound_record_json(tp_proof_obligations_ctx_t *pc,
                                          const char *function,
                                          const param_t *param) {
  if (!pc || !param || !param->type || !ny_type_is_fin(param->type))
    return;
  int64_t bound = 0;
  bool known = ny_fin_resolve_bound(pc->tp->cg, NULL, 0, param->type, &bound);
  tp_proof_record_begin_json(pc, "fin-bound");
  if (!tp_proof_enclosing_fn(pc)) {
    tp_append(pc->j, ",\"owner\":");
    tp_json_str(pc->j, function ? function : "");
  }
  tp_append(pc->j, ",\"name\":");
  tp_json_str(pc->j, param->name ? param->name : "");
  tp_append(pc->j, ",\"type\":");
  tp_json_str(pc->j, param->type);
  tp_append(pc->j, ",\"lower_inclusive\":0,\"upper_exclusive\":");
  if (known)
    tp_append(pc->j, "%lld", (long long)bound);
  else
    tp_append(pc->j, "null");
  tp_append(pc->j, ",\"source_origin\":{\"file\":");
  tp_json_str(pc->j, param->tok.filename ? param->tok.filename : "");
  tp_append(pc->j, ",\"line\":%d,\"column\":%d},\"status\":", param->tok.line,
            param->tok.col);
  tp_json_str(pc->j, known ? "assumed" : "unknown");
  tp_append(pc->j, ",\"verdict\":null}");
}

static void tp_emit_lemma_obligation_json(tp_proof_obligations_ctx_t *pc,
                                          stmt_t *stmt) {
  const stmt_lemma_t *lemma = &stmt->as.lemma;
  tp_proof_record_begin_json(pc, "lemma");
  tp_append(pc->j, ",\"name\":");
  tp_json_str(pc->j, lemma->name ? lemma->name : "");
  tp_append(pc->j, ",\"formula\":");
  tp_emit_obligation_formula_json(pc->j, lemma->proposition);
  tp_append(pc->j, ",\"source_origin\":{\"file\":");
  tp_json_str(pc->j, stmt->tok.filename ? stmt->tok.filename : "");
  tp_append(
      pc->j,
      ",\"line\":%d,\"column\":%d},\"status\":\"unchecked\",\"verdict\":null}",
      stmt->tok.line, stmt->tok.col);
  for (size_t i = 0; i < lemma->params.len; ++i)
    tp_emit_fin_bound_record_json(pc, lemma->name, &lemma->params.data[i]);
}

static void tp_emit_impl_law_obligation_json(tp_proof_obligations_ctx_t *pc,
                                             stmt_t *impl, expr_t *law) {
  if (!law)
    return;
  /*
   * Mirror tp_validate_impl_coherence_stmt: each law is a versioned
   * obligation through the policy funnel with a comptime fallback, and the
   * record distinguishes a refuted law (decided false, by solver
   * counterexample or exact comptime folding) from an undecided one.
   */
  ny_proof_policy_result_t policy_result;
  memset(&policy_result, 0, sizeof(policy_result));
  bool decided = ny_proof_policy_decide(pc->tp->cg, NULL, 0, law, "", false,
                                        &policy_result);
  bool refuted = decided && policy_result.verdict == NY_PROOF_VERDICT_REFUTED;
  bool proved = decided && policy_result.verdict == NY_PROOF_VERDICT_PROVED;
  if (!proved) {
    ny_ct_fast_val_t value = ny_ct_fast_none();
    if (ny_try_eval_comptime_expr_fast(pc->tp->cg, law, &value, 0) &&
        value.kind == NY_CT_FAST_BOOL) {
      if (value.b) {
        if (policy_result.obligation[0] ||
            ny_proof_policy_obligation_build(pc->tp->cg, NULL, 0, law, "",
                                             false, policy_result.obligation,
                                             sizeof(policy_result.obligation)))
          proved = ny_proof_policy_mark_fold(&policy_result);
      } else {
        /*
         * Exact compile-time evaluation decided the law false.
         */
        refuted = true;
      }
    }
  }
  tp_proof_record_begin_json(pc, "impl-law");
  tp_append(pc->j, ",\"trait\":");
  tp_json_str(pc->j, impl->as.impl.trait_name ? impl->as.impl.trait_name : "");
  tp_append(pc->j, ",\"type\":");
  tp_json_str(pc->j, impl->as.impl.type_name ? impl->as.impl.type_name : "");
  tp_append(pc->j, ",\"formula\":");
  tp_emit_obligation_formula_json(pc->j, law);
  tp_append(pc->j, ",\"source_origin\":");
  tp_emit_obligation_source_origin_json(pc->j, law);
  tp_append(pc->j, ",\"emission_origin\":");
  tp_emit_token_origin_json(pc->j, impl->expansion_origin);
  tp_append(pc->j, ",\"status\":");
  tp_json_str(pc->j, proved ? "proved" : refuted ? "refuted" : "unknown");
  tp_append(pc->j, "}");
}

static bool tp_proof_obligations_stmt_pre(ny_visitor_t *visitor, stmt_t *stmt) {
  tp_proof_obligations_ctx_t *pc =
      visitor ? (tp_proof_obligations_ctx_t *)visitor->ctx : NULL;
  if (!pc || !stmt || stmt->kind == NY_S_MACRO)
    return false;
  /*
   * Out-of-scope trees (stdlib, other files) are excluded exactly like the
   * typed function and layout records.
   */
  if (!tp_stmt_in_scope(pc->tp, stmt))
    return false;
  switch (stmt->kind) {
  case NY_S_FUNC: {
    stmt_func_t *fn = &stmt->as.fn;
    vec_push(&pc->fn_stack, fn);
    for (size_t i = 0; i < fn->params.len; ++i)
      tp_emit_fin_bound_record_json(pc, fn->name, &fn->params.data[i]);
    break;
  }
  case NY_S_LEMMA:
    tp_emit_lemma_obligation_json(pc, stmt);
    break;
  case NY_S_WHILE:
    tp_emit_invariant_obligation_json(pc, stmt);
    break;
  case NY_S_VAR:
    tp_emit_refinement_obligation_json(pc, stmt);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; pc->tp->cg && i < stmt->as.impl.laws.len; ++i)
      tp_emit_impl_law_obligation_json(pc, stmt, stmt->as.impl.laws.data[i]);
    break;
  default:
    break;
  }
  return true;
}

static void tp_proof_obligations_stmt_post(ny_visitor_t *visitor,
                                           stmt_t *stmt) {
  tp_proof_obligations_ctx_t *pc =
      visitor ? (tp_proof_obligations_ctx_t *)visitor->ctx : NULL;
  if (!pc || !stmt || stmt->kind != NY_S_FUNC || pc->fn_stack.len == 0)
    return;
  --pc->fn_stack.len;
}

/*
 * Unit-wide proof obligations beyond the per-call requires/decreases facts:
 * loop `invariant` propositions from while headers, trait `impl` law
 * propositions, and refinement construction obligations from annotated
 * declarations. Statuses are honest: "unchecked" when the policy could not
 * construct a checked obligation, "proved"/"not_proven" otherwise (with a
 * tri-state verdict), and impl laws distinguish "refuted" from "unknown"
 * exactly like the trait validator.
 */
static void tp_emit_proof_obligations_json(ny_tp_json_t *j, ny_tp_ctx_t *ctx) {
  tp_append(j, "{\"schema\":\"nytrix.semantic.proof-obligations\","
               "\"version\":1,\"items\":[");
  tp_proof_obligations_ctx_t pc = {.j = j, .tp = ctx, .first = true};
  vec_init(&pc.fn_stack);
  ny_visitor_t visitor = {.ctx = &pc,
                          .visit_stmt_pre = tp_proof_obligations_stmt_pre,
                          .visit_stmt_post = tp_proof_obligations_stmt_post};
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i)
    ny_visit_stmt(&visitor, ctx->prog->body.data[i]);
  vec_free(&pc.fn_stack);
  tp_append(j, "]}");
}

static bool tp_contract_call_may_write(codegen_t *cg, const expr_t *expr) {
  if (expr->kind == NY_E_ASM)
    return true;
  if (expr->kind != NY_E_CALL && expr->kind != NY_E_MEMCALL)
    return false;
  fun_sig *callee = cg && expr->semantic.canonical_callee
                        ? lookup_fun_exact(cg, expr->semantic.canonical_callee)
                        : NULL;
  return !callee || !callee->effects_known ||
         (callee->effects & (NY_FX_WRITE_MEMORY | NY_FX_FFI | NY_FX_THREAD)) !=
             0;
}

static bool tp_param_scan_name(tp_param_mutation_scan_t *scan,
                               const char *name) {
  if (scan && tp_contract_mentions_name(scan->premise, name)) {
    scan->invalidated = true;
    return true;
  }
  return false;
}

static void tp_param_scan_pattern(tp_param_mutation_scan_t *scan,
                                  expr_t *pattern) {
  ny_call_arg_list *args = NULL;
  if (pattern && pattern->kind == NY_E_CALL)
    args = &pattern->as.call.args;
  else if (pattern && pattern->kind == NY_E_MEMCALL)
    args = &pattern->as.memcall.args;
  for (size_t i = 0; args && i < args->len; ++i) {
    expr_t *binding = args->data[i].val;
    if (binding && binding->kind == NY_E_IDENT && binding->as.ident.name &&
        strcmp(binding->as.ident.name, "_") != 0)
      tp_param_scan_name(scan, binding->as.ident.name);
  }
}

static void tp_param_scan_patterns(tp_param_mutation_scan_t *scan,
                                   ny_match_arm_list *arms) {
  if (!scan || !arms)
    return;
  for (size_t i = 0; i < arms->len && !scan->invalidated; ++i) {
    match_arm_t *arm = &arms->data[i];
    for (size_t j = 0; j < arm->patterns.len && !scan->invalidated; ++j)
      tp_param_scan_pattern(scan, arm->patterns.data[j]);
  }
}

static const char *tp_contract_assigned_name(const expr_t *expr) {
  const char *name = NULL;
  if (expr->kind == NY_E_BINARY && expr->as.binary.op && expr->as.binary.left &&
      expr->as.binary.left->kind == NY_E_IDENT) {
    const char *op = expr->as.binary.op;
    size_t op_len = strlen(op);
    bool assignment = strcmp(op, "=") == 0 ||
                      (op_len > 1 && op[op_len - 1] == '=' &&
                       strcmp(op, "==") != 0 && strcmp(op, "!=") != 0 &&
                       strcmp(op, "<=") != 0 && strcmp(op, ">=") != 0);
    if (assignment)
      name = expr->as.binary.left->as.ident.name;
  } else if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
             expr->as.unary.right && expr->as.unary.right->kind == NY_E_IDENT &&
             (strcmp(expr->as.unary.op, "++") == 0 ||
              strcmp(expr->as.unary.op, "--") == 0)) {
    name = expr->as.unary.right->as.ident.name;
  }
  return name;
}

static bool tp_param_mutation_scan_pre(ny_visitor_t *visitor, expr_t *expr) {
  tp_param_mutation_scan_t *scan =
      visitor ? (tp_param_mutation_scan_t *)visitor->ctx : NULL;
  if (!scan || !expr)
    return true;
  if (tp_contract_call_may_write(scan->cg, expr)) {
    scan->invalidated = true;
    return false;
  }
  if (expr->kind == NY_E_LAMBDA || expr->kind == NY_E_FN) {
    for (size_t i = 0; i < expr->as.lambda.params.len; ++i) {
      if (tp_param_scan_name(scan, expr->as.lambda.params.data[i].name))
        return false;
    }
    ny_visit_stmt(visitor, expr->as.lambda.body);
  } else if (expr->kind == NY_E_MATCH) {
    tp_param_scan_patterns(scan, &expr->as.match.arms);
    if (scan->invalidated)
      return false;
    ny_visit_expr(visitor, expr->as.match.test);
    for (size_t i = 0; i < expr->as.match.arms.len; ++i) {
      match_arm_t *arm = &expr->as.match.arms.data[i];
      ny_visit_expr(visitor, arm->guard);
      ny_visit_stmt(visitor, arm->conseq);
    }
    ny_visit_stmt(visitor, expr->as.match.default_conseq);
    return false;
  }
  return !tp_param_scan_name(scan, tp_contract_assigned_name(expr));
}

static bool tp_param_mutation_scan_stmt_pre(ny_visitor_t *visitor,
                                            stmt_t *stmt) {
  tp_param_mutation_scan_t *scan =
      visitor ? (tp_param_mutation_scan_t *)visitor->ctx : NULL;
  if (!scan || !stmt)
    return true;
  switch (stmt->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < stmt->as.var.names.len; ++i)
      if (tp_param_scan_name(scan, stmt->as.var.names.data[i]))
        return false;
    break;
  case NY_S_GUARD:
    if (tp_param_scan_name(scan, stmt->as.guard.name))
      return false;
    break;
  case NY_S_FOR:
    if (tp_param_scan_name(scan, stmt->as.fr.iter_var) ||
        tp_param_scan_name(scan, stmt->as.fr.iter_index_var))
      return false;
    break;
  case NY_S_TRY:
    if (tp_param_scan_name(scan, stmt->as.tr.err))
      return false;
    break;
  case NY_S_FUNC:
    for (size_t i = 0; i < stmt->as.fn.params.len; ++i)
      if (tp_param_scan_name(scan, stmt->as.fn.params.data[i].name))
        return false;
    break;
  case NY_S_MATCH:
    tp_param_scan_patterns(scan, &stmt->as.match.arms);
    if (scan->invalidated)
      return false;
    ny_visit_expr(visitor, stmt->as.match.test);
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      match_arm_t *arm = &stmt->as.match.arms.data[i];
      ny_visit_expr(visitor, arm->guard);
      ny_visit_stmt(visitor, arm->conseq);
    }
    ny_visit_stmt(visitor, stmt->as.match.default_conseq);
    return false;
  default:
    break;
  }
  return true;
}

static bool tp_statement_changes_premise(tp_requires_proof_ctx_t *ctx,
                                         stmt_t *stmt, const expr_t *premise) {
  if (!stmt || !premise)
    return false;
  tp_param_mutation_scan_t scan = {.cg = ctx->tp->cg, .premise = premise};
  ny_visitor_t visitor = {
      .ctx = &scan,
      .visit_expr_pre = tp_param_mutation_scan_pre,
      .visit_stmt_pre = tp_param_mutation_scan_stmt_pre,
  };
  ny_visit_stmt(&visitor, stmt);
  return scan.invalidated;
}

static bool tp_guard_changes_values(tp_requires_proof_ctx_t *ctx,
                                    expr_t *guard) {
  tp_param_mutation_scan_t scan = {.cg = ctx->tp->cg, .premise = guard};
  ny_visitor_t visitor = {
      .ctx = &scan,
      .visit_expr_pre = tp_param_mutation_scan_pre,
      .visit_stmt_pre = tp_param_mutation_scan_stmt_pre,
  };
  ny_visit_expr(&visitor, guard);
  return scan.invalidated;
}

static const char *tp_contract_operand_type_depth(const expr_t *expr,
                                                  const stmt_func_t *fn,
                                                  size_t depth) {
  if (depth > 64)
    return NULL;
  if (!expr)
    return NULL;
  if (expr->semantic.resolved_type_name)
    return expr->semantic.resolved_type_name;
  if (expr->kind == NY_E_LITERAL)
    return tp_literal_type(&expr->as.literal, expr->tok);
  if (expr->semantic.rep == NY_SEM_REP_STRING)
    return "str";
  if (expr->kind == NY_E_IDENT && expr->as.ident.name && fn) {
    for (size_t i = 0; i < fn->params.len; ++i) {
      const param_t *param = &fn->params.data[i];
      if (param->name && param->type &&
          strcmp(param->name, expr->as.ident.name) == 0)
        return param->type;
    }
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      strcmp(expr->as.unary.op, "-") == 0 &&
      tp_contract_operand_type_depth(expr->as.unary.right, fn, depth + 1))
    return "int";
  if (expr->kind == NY_E_BINARY && expr->as.binary.op && expr->as.binary.left &&
      expr->as.binary.right) {
    const char *op = expr->as.binary.op;
    bool integer_arithmetic = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                              strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                              strcmp(op, "%") == 0;
    const char *left_type = integer_arithmetic
                                ? tp_contract_operand_type_depth(
                                      expr->as.binary.left, fn, depth + 1)
                                : NULL;
    const char *right_type = integer_arithmetic
                                 ? tp_contract_operand_type_depth(
                                       expr->as.binary.right, fn, depth + 1)
                                 : NULL;
    if (left_type && right_type && strcmp(left_type, "int") == 0 &&
        strcmp(right_type, "int") == 0)
      return "int";
  }
  return NULL;
}

static const char *tp_contract_operand_type(const expr_t *expr,
                                            const stmt_func_t *fn) {
  return tp_contract_operand_type_depth(expr, fn, 0);
}

static bool tp_contract_operator_scan_pre(ny_visitor_t *visitor, expr_t *expr) {
  tp_contract_operator_scan_t *scan =
      visitor ? (tp_contract_operator_scan_t *)visitor->ctx : NULL;
  const char *op = NULL;
  const char *left_type = NULL;
  const char *right_type = NULL;
  if (!scan || !scan->tp || !scan->tp->cg || !expr)
    return true;
  if (expr->kind == NY_E_BINARY) {
    op = expr->as.binary.op;
    left_type = tp_contract_operand_type(expr->as.binary.left, scan->fn);
    right_type = tp_contract_operand_type(expr->as.binary.right, scan->fn);
  } else if (expr->kind == NY_E_LOGICAL) {
    op = expr->as.logical.op;
    left_type = right_type = "bool";
  } else if (expr->kind == NY_E_UNARY) {
    op = expr->as.unary.op;
    left_type = right_type =
        strcmp(op ? op : "", "!") == 0
            ? "bool"
            : tp_contract_operand_type(expr->as.unary.right, scan->fn);
  }
  for (size_t i = 0; op && i < scan->tp->cg->operators.len; ++i) {
    const ny_operator_def_t *definition = &scan->tp->cg->operators.data[i];
    if (!tp_operator_module_active(scan->tp, definition))
      continue;
    if (definition->op && strcmp(definition->op, op) == 0 &&
        definition->left_type && definition->right_type &&
        (!left_type || strcmp(definition->left_type, left_type) == 0) &&
        (!right_type || strcmp(definition->right_type, right_type) == 0)) {
      scan->has_custom_operator = true;
      break;
    }
  }
  return !scan->has_custom_operator;
}

static bool tp_contract_has_custom_operator_for_fn(ny_tp_ctx_t *tp,
                                                   expr_t *expr,
                                                   const stmt_func_t *fn) {
  if (!tp || !tp->cg || !expr)
    return false;
  tp_contract_operator_scan_t scan = {.tp = tp, .fn = fn};
  ny_visitor_t visitor = {
      .ctx = &scan,
      .visit_expr_pre = tp_contract_operator_scan_pre,
  };
  ny_visit_expr(&visitor, expr);
  return scan.has_custom_operator;
}

static bool tp_contract_has_custom_operator(ny_tp_ctx_t *tp, expr_t *expr) {
  return tp_contract_has_custom_operator_for_fn(tp, expr, NULL);
}

typedef struct tp_contract_name_scan_t {
  const char *name;
  bool found;
} tp_contract_name_scan_t;

static bool tp_contract_name_scan_stmt_pre(ny_visitor_t *visitor,
                                           stmt_t *stmt) {
  tp_contract_name_scan_t *scan =
      visitor ? (tp_contract_name_scan_t *)visitor->ctx : NULL;
  if (!scan || !scan->name || !stmt)
    return true;
  switch (stmt->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < stmt->as.var.names.len; ++i)
      scan->found |= stmt->as.var.names.data[i] &&
                     strcmp(stmt->as.var.names.data[i], scan->name) == 0;
    break;
  case NY_S_GUARD:
    scan->found |=
        stmt->as.guard.name && strcmp(stmt->as.guard.name, scan->name) == 0;
    break;
  case NY_S_FOR:
    scan->found |= (stmt->as.fr.iter_var &&
                    strcmp(stmt->as.fr.iter_var, scan->name) == 0) ||
                   (stmt->as.fr.iter_index_var &&
                    strcmp(stmt->as.fr.iter_index_var, scan->name) == 0);
    break;
  case NY_S_TRY:
    scan->found |= stmt->as.tr.err && strcmp(stmt->as.tr.err, scan->name) == 0;
    break;
  case NY_S_FUNC:
    for (size_t i = 0; i < stmt->as.fn.params.len; ++i)
      scan->found |= stmt->as.fn.params.data[i].name &&
                     strcmp(stmt->as.fn.params.data[i].name, scan->name) == 0;
    /*
     * A nested function's locals do not shadow at the caller.
     */
    return false;
  default:
    break;
  }
  return !scan->found;
}

/*
 * Defaults are emitted at the call site. Do not resolve a default identifier
 * as a callee module constant if a declaration in the caller could shadow it.
 */
static bool tp_contract_caller_declares_name(const tp_requires_proof_ctx_t *ctx,
                                             const char *name) {
  if (!ctx || !ctx->current_fn || !name)
    return true;
  for (size_t i = 0; i < ctx->current_fn->params.len; ++i)
    if (ctx->current_fn->params.data[i].name &&
        strcmp(ctx->current_fn->params.data[i].name, name) == 0)
      return true;
  tp_contract_name_scan_t scan = {.name = name};
  ny_visitor_t visitor = {.ctx = &scan,
                          .visit_stmt_pre = tp_contract_name_scan_stmt_pre};
  ny_visit_stmt(&visitor, ctx->current_fn->body);
  return scan.found;
}

static expr_t *tp_contract_clone_default_int_expr(
    tp_requires_proof_ctx_t *ctx, const stmt_func_t *callee,
    size_t parameter_index, expr_t **actuals, const bool *assigned,
    const expr_t *expr, size_t depth) {
  if (!ctx || !ctx->tp || !ctx->tp->cg || !expr || depth > 64)
    return NULL;
  codegen_t *cg = ctx->tp->cg;
  if (expr->kind == NY_E_LITERAL &&
      (expr->as.literal.kind == NY_LIT_BOOL ||
       (expr->tok.kind != NY_T_NIL &&
        ny_literal_is_signed_i64(&expr->as.literal)))) {
    expr_t *copy = arena_alloc(cg->arena, sizeof(*copy));
    if (copy)
      *copy = *expr;
    return copy;
  }
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    for (size_t i = 0; callee && i < parameter_index && i < callee->params.len;
         ++i) {
      const param_t *earlier = &callee->params.data[i];
      if (!earlier->name || strcmp(earlier->name, expr->as.ident.name) != 0)
        continue;
      expr_t *actual = assigned && assigned[i] && actuals ? actuals[i] : NULL;
      bool actual_matches =
          actual &&
          (earlier->type && strcmp(earlier->type, "bool") == 0
               ? tp_contract_bool_term(ctx, ctx->current_fn, actual, 0)
               : tp_contract_int_term(ctx, ctx->current_fn, actual, 0));
      if (!actual_matches || tp_contract_has_custom_operator(ctx->tp, actual) ||
          tp_guard_changes_values(ctx, actual))
        return NULL;
      return actual;
    }
    if (tp_contract_caller_declares_name(ctx, expr->as.ident.name))
      return NULL;
    binding *global = lookup_global(cg, expr->as.ident.name);
    ny_ct_fast_val_t value = {0};
    bool known_value =
        global && !global->is_mut && global->name &&
        ny_try_eval_binding_comptime_const(cg, global, global->name, &value,
                                           0) &&
        (value.kind == NY_CT_FAST_INT || value.kind == NY_CT_FAST_BOOL);
    if (!known_value)
      return NULL;
    expr_t *literal = arena_alloc(cg->arena, sizeof(*literal));
    if (!literal)
      return NULL;
    *literal = (expr_t){.kind = NY_E_LITERAL, .tok = expr->tok};
    if (value.kind == NY_CT_FAST_BOOL) {
      literal->as.literal.kind = NY_LIT_BOOL;
      literal->as.literal.as.b = value.b;
    } else {
      literal->as.literal.kind = NY_LIT_INT;
      literal->as.literal.as.i = value.i;
    }
    return literal;
  }
  if (expr->kind != NY_E_UNARY && expr->kind != NY_E_BINARY)
    return NULL;
  expr_t *copy = arena_alloc(cg->arena, sizeof(*copy));
  if (!copy)
    return NULL;
  *copy = *expr;
  if (expr->kind == NY_E_UNARY) {
    if (!expr->as.unary.op || strcmp(expr->as.unary.op, "-") != 0)
      return NULL;
    copy->as.unary.right = tp_contract_clone_default_int_expr(
        ctx, callee, parameter_index, actuals, assigned, expr->as.unary.right,
        depth + 1);
    return copy->as.unary.right ? copy : NULL;
  }
  const char *op = expr->as.binary.op;
  if (!op ||
      (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0 &&
       strcmp(op, "/") != 0 && strcmp(op, "%") != 0))
    return NULL;
  copy->as.binary.left = tp_contract_clone_default_int_expr(
      ctx, callee, parameter_index, actuals, assigned, expr->as.binary.left,
      depth + 1);
  copy->as.binary.right = tp_contract_clone_default_int_expr(
      ctx, callee, parameter_index, actuals, assigned, expr->as.binary.right,
      depth + 1);
  return copy->as.binary.left && copy->as.binary.right ? copy : NULL;
}

static expr_t *tp_contract_default_int_value(
    tp_requires_proof_ctx_t *ctx, const char *callee_module_name,
    const char *callee_source_file, const char *caller_file,
    const stmt_func_t *callee, size_t parameter_index, expr_t **actuals,
    const bool *assigned, const expr_t *default_expr) {
  if (!ctx || !ctx->tp || !ctx->tp->cg || !default_expr)
    return NULL;
  if ((!callee_module_name || !*callee_module_name) &&
      (!callee_source_file || !caller_file ||
       strcmp(callee_source_file, caller_file) != 0))
    return NULL;
  codegen_t *cg = ctx->tp->cg;
  const char *saved_module = cg->current_module_name;
  if (callee_module_name && *callee_module_name)
    cg->current_module_name = callee_module_name;
  expr_t *resolved = tp_contract_clone_default_int_expr(
      ctx, callee, parameter_index, actuals, assigned, default_expr, 0);
  cg->current_module_name = saved_module;
  return resolved;
}

static expr_t *tp_contract_guard(tp_requires_proof_ctx_t *ctx, expr_t *premise,
                                 expr_t *guard, bool negate) {
  if (!guard)
    return premise;
  if (negate) {
    expr_t *inverted = arena_alloc(ctx->tp->cg->arena, sizeof(*inverted));
    if (!inverted)
      return premise;
    *inverted = (expr_t){.kind = NY_E_UNARY, .tok = guard->tok};
    inverted->as.unary.op = "!";
    inverted->as.unary.right = guard;
    guard = inverted;
  }
  if (!premise)
    return guard;
  expr_t *combined = arena_alloc(ctx->tp->cg->arena, sizeof(*combined));
  if (!combined)
    return premise;
  *combined = (expr_t){.kind = NY_E_LOGICAL, .tok = guard->tok};
  combined->as.logical.op = "&&";
  combined->as.logical.left = premise;
  combined->as.logical.right = guard;
  return combined;
}

static bool tp_contract_stmt_returns(const stmt_t *stmt) {
  if (!stmt)
    return false;
  if (stmt->kind == NY_S_RETURN)
    return true;
  if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (tp_contract_stmt_returns(stmt->as.block.body.data[i]))
        return true;
  }
  return stmt->kind == NY_S_IF &&
         tp_contract_stmt_returns(stmt->as.iff.conseq) &&
         tp_contract_stmt_returns(stmt->as.iff.alt);
}

static bool tp_contract_bind_names(tp_requires_proof_ctx_t *ctx,
                                   const expr_t *expr, scope *proof_scope,
                                   size_t depth) {
  if (!expr)
    return true;
  if (depth > 64)
    return false;
  if (expr->kind == NY_E_IDENT) {
    bool is_int = tp_contract_visible_int_name(ctx, ctx->current_fn, expr);
    bool is_bool = tp_contract_visible_bool_name(ctx, ctx->current_fn, expr);
    int64_t fin_bound = 0;
    bool is_fin =
        tp_contract_visible_fin_bound(ctx, ctx->current_fn, expr, &fin_bound);
    if (!is_int && !is_bool && !is_fin)
      return false;
    for (size_t i = 0; i < proof_scope->vars.len; ++i)
      if (strcmp(proof_scope->vars.data[i].name, expr->as.ident.name) == 0)
        return true;
    if (proof_scope->vars.len == proof_scope->vars.cap)
      return false;
    proof_scope->vars.data[proof_scope->vars.len++] =
        (binding){.name = expr->as.ident.name,
                  .type_name = is_bool ? "bool" : "int",
                  .has_int_range = true,
                  .int_min_raw = (is_bool || is_fin) ? 0 : INT64_MIN,
                  .int_max_raw = is_bool  ? 1
                                 : is_fin ? fin_bound - 1
                                          : INT64_MAX};
  } else if (expr->kind == NY_E_UNARY) {
    return tp_contract_bind_names(ctx, expr->as.unary.right, proof_scope,
                                  depth + 1);
  } else if (expr->kind == NY_E_BINARY) {
    return tp_contract_bind_names(ctx, expr->as.binary.left, proof_scope,
                                  depth + 1) &&
           tp_contract_bind_names(ctx, expr->as.binary.right, proof_scope,
                                  depth + 1);
  } else if (expr->kind == NY_E_LOGICAL) {
    return tp_contract_bind_names(ctx, expr->as.logical.left, proof_scope,
                                  depth + 1) &&
           tp_contract_bind_names(ctx, expr->as.logical.right, proof_scope,
                                  depth + 1);
  }
  return true;
}

static bool tp_contract_count_names(const expr_t *expr, size_t depth,
                                    size_t *count) {
  if (!expr)
    return true;
  if (!count || depth > 64)
    return false;
  if (expr->kind == NY_E_IDENT) {
    if (*count == SIZE_MAX)
      return false;
    ++*count;
    return true;
  }
  if (expr->kind == NY_E_UNARY)
    return tp_contract_count_names(expr->as.unary.right, depth + 1, count);
  if (expr->kind == NY_E_BINARY)
    return tp_contract_count_names(expr->as.binary.left, depth + 1, count) &&
           tp_contract_count_names(expr->as.binary.right, depth + 1, count);
  if (expr->kind == NY_E_LOGICAL)
    return tp_contract_count_names(expr->as.logical.left, depth + 1, count) &&
           tp_contract_count_names(expr->as.logical.right, depth + 1, count);
  return true;
}

static void tp_contract_invalidate(tp_requires_proof_ctx_t *ctx,
                                   const char *name, bool all) {
  if (all || tp_contract_mentions_name(ctx->premise, name))
    ctx->premise = NULL;
  for (size_t i = ctx->stmt_stack.len; i-- > 0;) {
    tp_requires_stmt_frame_t *frame = &ctx->stmt_stack.data[i];
    if (frame->stmt->kind == NY_S_FUNC)
      break;
    if (all || tp_contract_mentions_name(frame->premise, name))
      frame->premise = NULL;
  }
}

static expr_t *tp_contract_single_block_value(stmt_t *stmt) {
  if (!stmt)
    return NULL;
  if (stmt->kind == NY_S_BLOCK) {
    if (stmt->as.block.body.len != 1)
      return NULL;
    stmt = stmt->as.block.body.data[0];
  }
  if (stmt->kind == NY_S_EXPR)
    return stmt->as.expr.expr;
  if (stmt->kind == NY_S_RETURN)
    return stmt->as.ret.value;
  return NULL;
}

static expr_t *
tp_contract_match_value_parts(arena_t *arena, const expr_t *match_test,
                              const match_arm_t *arms, size_t arm_count,
                              stmt_t *default_conseq, const stmt_func_t *fn,
                              size_t depth);

static bool tp_contract_finite_match_exhaustive(const match_arm_t *arms,
                                                size_t arm_count,
                                                int64_t bound) {
  if (!arms || !arm_count || bound <= 0)
    return false;
  int64_t covered_through = -1;
  while (covered_through < bound - 1) {
    int64_t next = covered_through + 1;
    int64_t furthest = covered_through;
    for (size_t i = 0; i < arm_count; ++i) {
      const match_arm_t *arm = &arms[i];
      if (arm->guard || !arm->patterns.len)
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        const expr_t *pattern = arm->patterns.data[j];
        int64_t lower = 0;
        int64_t upper = 0;
        if (!pattern)
          return false;
        if (pattern->kind == NY_E_BINARY && pattern->as.binary.op &&
            strcmp(pattern->as.binary.op, "..") == 0) {
          if (!pattern->as.binary.left || !pattern->as.binary.right ||
              pattern->as.binary.left->kind != NY_E_LITERAL ||
              pattern->as.binary.right->kind != NY_E_LITERAL ||
              !ny_literal_is_signed_i64(&pattern->as.binary.left->as.literal) ||
              !ny_literal_is_signed_i64(&pattern->as.binary.right->as.literal))
            return false;
          lower = pattern->as.binary.left->as.literal.as.i;
          upper = pattern->as.binary.right->as.literal.as.i;
        } else if (pattern->kind == NY_E_LITERAL &&
                   ny_literal_is_signed_i64(&pattern->as.literal)) {
          lower = pattern->as.literal.as.i;
          upper = lower;
        } else {
          return false;
        }
        if (lower <= next && upper >= next && upper > furthest)
          furthest = upper;
      }
    }
    if (furthest == covered_through)
      return false;
    covered_through = furthest;
  }
  return true;
}

static expr_t *tp_contract_branch_value(arena_t *arena, stmt_t *stmt,
                                        const stmt_func_t *fn, size_t depth) {
  if (!arena || !stmt || depth > 32)
    return NULL;
  stmt_t *block = NULL;
  size_t local_count = 0;
  if (stmt->kind == NY_S_BLOCK) {
    block = stmt;
    if (block->as.block.body.len == 0)
      return NULL;
    local_count = block->as.block.body.len - 1;
    stmt = block->as.block.body.data[local_count];
    if (!stmt)
      return NULL;
  }
  expr_t *value = tp_contract_single_block_value(stmt);
  if (!value) {
    if (stmt->kind == NY_S_MATCH) {
      value = tp_contract_match_value_parts(
          arena, stmt->as.match.test, stmt->as.match.arms.data,
          stmt->as.match.arms.len, stmt->as.match.default_conseq, fn,
          depth + 1);
    } else {
      const char *init_name = NULL;
      expr_t *init_value = NULL;
      if (!tp_contract_if_init_parts(stmt, &init_name, &init_value) ||
          !stmt->as.iff.test || !stmt->as.iff.alt)
        return NULL;
      expr_t *test = (expr_t *)stmt->as.iff.test;
      if (init_name) {
        test = ny_expr_subst(arena, test, init_name, init_value);
        if (!test)
          return NULL;
      }
      expr_t *yes =
          tp_contract_branch_value(arena, stmt->as.iff.conseq, fn, depth + 1);
      expr_t *no =
          tp_contract_branch_value(arena, stmt->as.iff.alt, fn, depth + 1);
      expr_t *not_test = arena_alloc(arena, sizeof(*not_test));
      if (init_name && yes && no) {
        yes = ny_expr_subst(arena, yes, init_name, init_value);
        no = ny_expr_subst(arena, no, init_name, init_value);
      }
      if (!yes || !no || !not_test)
        return NULL;
      *not_test = (expr_t){.kind = NY_E_UNARY, .tok = test->tok};
      not_test->as.unary.op = "!";
      not_test->as.unary.right = test;
      expr_t *then_term =
          tp_contract_make_logical(arena, stmt->tok, "&&", test, yes);
      expr_t *else_term =
          tp_contract_make_logical(arena, stmt->tok, "&&", not_test, no);
      value = then_term && else_term
                  ? tp_contract_make_logical(arena, stmt->tok, "||", then_term,
                                             else_term)
                  : NULL;
    }
  }
  if (!value)
    return NULL;
  if (value->kind == NY_E_MATCH) {
    value = tp_contract_match_value_parts(
        arena, value->as.match.test, value->as.match.arms.data,
        value->as.match.arms.len, value->as.match.default_conseq, fn,
        depth + 1);
    if (!value)
      return NULL;
  }
  for (size_t i = local_count; i > 0; --i) {
    stmt_t *local = block->as.block.body.data[i - 1];
    if (!local || local->kind != NY_S_VAR || !local->as.var.is_decl ||
        local->as.var.is_mut || local->as.var.is_del ||
        local->as.var.is_destructure || local->as.var.names.len != 1 ||
        local->as.var.exprs.len != 1 || !local->as.var.names.data[0] ||
        !local->as.var.exprs.data[0])
      return NULL;
    value = ny_expr_subst(arena, value, local->as.var.names.data[0],
                          local->as.var.exprs.data[0]);
    if (!value)
      return NULL;
  }
  return value;
}

static expr_t *
tp_contract_match_value_parts(arena_t *arena, const expr_t *match_test,
                              const match_arm_t *arms, size_t arm_count,
                              stmt_t *default_conseq, const stmt_func_t *fn,
                              size_t depth) {
  if (!arena || !match_test || (!arms && arm_count) || depth > 32)
    return NULL;
  bool exhaustive_bool = false;
  const char *match_test_type = match_test->kind == NY_E_IDENT
                                    ? match_test->semantic.resolved_type_name
                                    : NULL;
  if ((!match_test_type || strcmp(match_test_type, "bool") != 0) &&
      match_test->kind == NY_E_IDENT && match_test->as.ident.name && fn) {
    for (size_t i = 0; i < fn->params.len; ++i) {
      const param_t *param = &fn->params.data[i];
      if (param->name && strcmp(param->name, match_test->as.ident.name) == 0) {
        match_test_type = param->type;
        break;
      }
    }
  }
  int64_t finite_bound = 0;
  bool finite_integer_match =
      match_test->kind == NY_E_IDENT && match_test_type &&
      tp_contract_type_name_matches(match_test_type, "Fin<") &&
      ny_fin_resolve_bound(NULL, NULL, 0, match_test_type, &finite_bound) &&
      finite_bound > 0;
  bool integer_match =
      finite_integer_match ||
      (match_test_type && (strcmp(match_test_type, "int") == 0 ||
                           strcmp(match_test_type, "i64") == 0));
  if (!default_conseq && match_test_type &&
      strcmp(match_test_type, "bool") == 0) {
    bool has_false = false;
    bool has_true = false;
    exhaustive_bool = arm_count > 0;
    for (size_t i = 0; exhaustive_bool && i < arm_count; ++i) {
      const match_arm_t *arm = &arms[i];
      if (!arm->patterns.len || arm->guard) {
        exhaustive_bool = false;
        break;
      }
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        const expr_t *pattern = arm->patterns.data[j];
        if (!pattern || pattern->kind != NY_E_LITERAL ||
            pattern->as.literal.kind != NY_LIT_BOOL) {
          exhaustive_bool = false;
          break;
        }
        if (pattern->as.literal.as.b)
          has_true = true;
        else
          has_false = true;
      }
    }
    exhaustive_bool = exhaustive_bool && has_false && has_true;
  }
  bool exhaustive_finite =
      !default_conseq && finite_integer_match &&
      tp_contract_finite_match_exhaustive(arms, arm_count, finite_bound);
  ny_proof_debug_at(3, match_test->tok,
                    "match proof lowering: test_type=%s default=%d "
                    "exhaustive_bool=%d exhaustive_finite=%d arms=%zu",
                    match_test_type ? match_test_type : "<unknown>",
                    default_conseq != NULL, exhaustive_bool, exhaustive_finite,
                    arm_count);
  if (!default_conseq && !exhaustive_bool && !exhaustive_finite)
    return NULL;
  expr_t *covered = NULL;
  expr_t *result = NULL;
  for (size_t i = 0; i < arm_count; ++i) {
    const match_arm_t *arm = &arms[i];
    if (!arm->patterns.len || !arm->conseq)
      return NULL;
    expr_t *pattern_test = NULL;
    for (size_t j = 0; j < arm->patterns.len; ++j) {
      expr_t *pattern = arm->patterns.data[j];
      if (!pattern)
        return NULL;
      expr_t *equal = NULL;
      if (pattern->kind == NY_E_BINARY && pattern->as.binary.op &&
          strcmp(pattern->as.binary.op, "..") == 0) {
        if (!integer_match || !pattern->as.binary.left ||
            !pattern->as.binary.right ||
            pattern->as.binary.left->kind != NY_E_LITERAL ||
            pattern->as.binary.right->kind != NY_E_LITERAL ||
            !ny_literal_is_signed_i64(&pattern->as.binary.left->as.literal) ||
            !ny_literal_is_signed_i64(&pattern->as.binary.right->as.literal))
          return NULL;
        expr_t *lower = tp_contract_make_binary(arena, pattern->tok,
                                                ">=", (expr_t *)match_test,
                                                pattern->as.binary.left);
        expr_t *upper = tp_contract_make_binary(arena, pattern->tok,
                                                "<=", (expr_t *)match_test,
                                                pattern->as.binary.right);
        equal = lower && upper ? tp_contract_make_logical(arena, pattern->tok,
                                                          "&&", lower, upper)
                               : NULL;
      } else {
        if (pattern->kind != NY_E_LITERAL ||
            (pattern->as.literal.kind != NY_LIT_BOOL &&
             !ny_literal_is_signed_i64(&pattern->as.literal)))
          return NULL;
        equal =
            exhaustive_bool
                ? (pattern->as.literal.as.b
                       ? (expr_t *)match_test
                       : tp_contract_bool_not(arena, match_test->tok,
                                              (expr_t *)match_test))
                : tp_contract_make_binary(arena, pattern->tok,
                                          "==", (expr_t *)match_test, pattern);
      }
      pattern_test = pattern_test
                         ? tp_contract_make_logical(arena, pattern->tok, "||",
                                                    pattern_test, equal)
                         : equal;
      if (!pattern_test)
        return NULL;
    }

    expr_t *selected = pattern_test;
    if (covered) {
      expr_t *not_covered =
          tp_contract_bool_not(arena, match_test->tok, covered);
      selected = not_covered
                     ? tp_contract_make_logical(arena, match_test->tok, "&&",
                                                selected, not_covered)
                     : NULL;
    }
    if (selected && arm->guard)
      selected = tp_contract_make_logical(arena, arm->guard->tok, "&&",
                                          selected, arm->guard);
    expr_t *branch =
        tp_contract_branch_value(arena, arm->conseq, fn, depth + 1);
    expr_t *term = selected && branch
                       ? tp_contract_make_logical(arena, match_test->tok, "&&",
                                                  selected, branch)
                       : NULL;
    result = result ? tp_contract_make_logical(arena, match_test->tok, "||",
                                               result, term)
                    : term;
    covered = covered ? tp_contract_make_logical(arena, match_test->tok, "||",
                                                 covered, selected)
                      : selected;
    if (!result || !covered)
      return NULL;
  }

  if (exhaustive_bool || exhaustive_finite)
    return result;

  expr_t *fallback =
      tp_contract_branch_value(arena, default_conseq, fn, depth + 1);
  expr_t *not_covered =
      covered ? tp_contract_bool_not(arena, match_test->tok, covered)
              : tp_contract_bool_constant(arena, match_test->tok, true);
  expr_t *fallback_term =
      fallback && not_covered
          ? tp_contract_make_logical(arena, match_test->tok, "&&", not_covered,
                                     fallback)
          : NULL;
  if (!fallback_term)
    return NULL;
  return result ? tp_contract_make_logical(arena, match_test->tok, "||", result,
                                           fallback_term)
                : fallback_term;
}

static expr_t *tp_contract_match_value(arena_t *arena, const expr_t *match,
                                       const stmt_func_t *fn, size_t depth) {
  return match && match->kind == NY_E_MATCH
             ? tp_contract_match_value_parts(
                   arena, match->as.match.test, match->as.match.arms.data,
                   match->as.match.arms.len, match->as.match.default_conseq, fn,
                   depth)
             : NULL;
}

static expr_t *
tp_contract_inline_pure_calls_depth(codegen_t *cg, arena_t *arena, expr_t *expr,
                                    const stmt_func_t *context_fn,
                                    size_t depth) {
  if (depth > 32)
    return NULL;
  if (expr->kind == NY_E_MATCH) {
    expr_t *expanded =
        tp_contract_match_value(arena, expr, context_fn, depth + 1);
    return expanded ? tp_contract_inline_pure_calls_depth(cg, arena, expanded,
                                                          context_fn, depth + 1)
                    : NULL;
  }
  if (expr->kind == NY_E_CALL || expr->kind == NY_E_MEMCALL) {
    fun_sig *sig = tp_contract_direct_call_sig(cg, context_fn, expr);
    stmt_t *fn = sig ? sig->stmt_t : NULL;
    const ny_call_arg_list *call_args =
        expr->kind == NY_E_CALL ? &expr->as.call.args : &expr->as.memcall.args;
    expr_t *receiver = tp_contract_attached_receiver(cg, expr);
    size_t receiver_count = receiver ? 1 : 0;
    bool signature_is_pure =
        sig && sig->is_pure && sig->effects_known && sig->effects == NY_FX_NONE;
    bool declared_pure = fn && fn->kind == NY_S_FUNC && fn->as.fn.attr_pure;
    bool effect_polymorphic = sig && sig->effects_known &&
                              tp_contract_effect_row_open(sig->effect_type);
    if (!sig || !fn || fn->kind != NY_S_FUNC || sig->is_recursive ||
        (!signature_is_pure && !declared_pure && !effect_polymorphic) ||
        !fn->as.fn.body || (!call_args->data && call_args->len != 0) ||
        call_args->len > fn->as.fn.params.len ||
        receiver_count > fn->as.fn.params.len - call_args->len) {
      return NULL;
    }
    stmt_t *body = fn->as.fn.body;
    if (body->kind != NY_S_BLOCK || body->as.block.body.len == 0)
      return NULL;
    size_t body_count = body->as.block.body.len;
    stmt_t *tail = body->as.block.body.data[body_count - 1];
    expr_t *value = tp_contract_branch_value(arena, tail, &fn->as.fn, 0);
    if (!value && tail && tail->kind == NY_S_MATCH) {
      expr_t match_expr = {.kind = NY_E_MATCH, .tok = tail->tok};
      match_expr.as.match = tail->as.match;
      value = tp_contract_match_value(arena, &match_expr, &fn->as.fn, 0);
    }
    if (!value)
      return NULL;
    size_t local_count = body_count - 1;
    if (fn->as.fn.return_type && strcmp(fn->as.fn.return_type, "bool") == 0) {
      size_t statement_index = body_count - 1;
      while (statement_index > 0) {
        stmt_t *guard = body->as.block.body.data[statement_index - 1];
        const char *init_name = NULL;
        expr_t *init_value = NULL;
        if (!guard || guard->kind != NY_S_IF || !guard->as.iff.test ||
            guard->as.iff.alt ||
            !tp_contract_if_init_parts(guard, &init_name, &init_value))
          break;
        expr_t *test = guard->as.iff.test;
        if (init_name) {
          test = ny_expr_subst(arena, test, init_name, init_value);
          if (!test)
            return NULL;
        }
        expr_t *yes = tp_contract_branch_value(arena, guard->as.iff.conseq,
                                               &fn->as.fn, 0);
        expr_t *not_test = arena_alloc(arena, sizeof(*not_test));
        if (init_name && yes)
          yes = ny_expr_subst(arena, yes, init_name, init_value);
        if (!yes || !not_test)
          return NULL;
        *not_test = (expr_t){.kind = NY_E_UNARY, .tok = test->tok};
        not_test->as.unary.op = "!";
        not_test->as.unary.right = test;
        expr_t *then_term =
            tp_contract_make_logical(arena, guard->tok, "&&", test, yes);
        expr_t *else_term =
            tp_contract_make_logical(arena, guard->tok, "&&", not_test, value);
        value = then_term && else_term
                    ? tp_contract_make_logical(arena, guard->tok, "||",
                                               then_term, else_term)
                    : NULL;
        if (!value)
          return NULL;
        --statement_index;
      }
      local_count = statement_index;
    }
    for (size_t i = local_count; i > 0; --i) {
      stmt_t *local = body->as.block.body.data[i - 1];
      if (!local || local->kind != NY_S_VAR || !local->as.var.is_decl ||
          local->as.var.is_mut || local->as.var.is_del ||
          local->as.var.is_destructure || local->as.var.names.len != 1 ||
          local->as.var.exprs.len != 1 || !local->as.var.names.data[0] ||
          !local->as.var.exprs.data[0])
        return NULL;
      value = ny_expr_subst(arena, value, local->as.var.names.data[0],
                            local->as.var.exprs.data[0]);
      if (!value)
        return NULL;
    }
    size_t arg_count = call_args->len + receiver_count;
    size_t param_count = fn->as.fn.params.len;
    expr_t **arguments =
        param_count ? arena_alloc(arena, param_count * sizeof(*arguments))
                    : NULL;
    expr_t **placeholders =
        param_count ? arena_alloc(arena, param_count * sizeof(*placeholders))
                    : NULL;
    char **placeholder_names =
        param_count
            ? arena_alloc(arena, param_count * sizeof(*placeholder_names))
            : NULL;
    bool *assigned = param_count
                         ? arena_alloc(arena, param_count * sizeof(*assigned))
                         : NULL;
    if (param_count &&
        (!arguments || !placeholders || !placeholder_names || !assigned))
      return NULL;
    if (assigned)
      memset(assigned, 0, param_count * sizeof(*assigned));
    expr_t *expanded = value;
    size_t next_positional = 0;
    for (size_t i = 0; i < arg_count; ++i) {
      const call_arg_t *arg =
          i < receiver_count ? NULL : &call_args->data[i - receiver_count];
      expr_t *actual = i < receiver_count ? receiver : arg->val;
      const char *arg_name = arg ? arg->name : NULL;
      if (!actual)
        return NULL;
      size_t param_index = param_count;
      if (arg_name) {
        for (param_index = 0; param_index < param_count; ++param_index) {
          const char *param_name = fn->as.fn.params.data[param_index].name;
          if (param_name && strcmp(param_name, arg_name) == 0)
            break;
        }
        if (param_index == param_count)
          return NULL;
      } else {
        while (next_positional < param_count && assigned[next_positional])
          ++next_positional;
        if (next_positional == param_count)
          return NULL;
        param_index = next_positional++;
      }
      if (!fn->as.fn.params.data[param_index].name ||
          !*fn->as.fn.params.data[param_index].name || assigned[param_index])
        return NULL;
      assigned[param_index] = true;
      arguments[param_index] = tp_contract_inline_pure_calls_depth(
          cg, arena, actual, context_fn, depth + 1);
      if (!arguments[param_index])
        return NULL;
    }

    for (size_t i = 0; i < param_count; ++i) {
      param_t *param = &fn->as.fn.params.data[i];
      if (!param->name || !*param->name)
        return NULL;
      char *name = arena_alloc(arena, 80);
      if (!name)
        return NULL;
      for (size_t suffix = 0;; ++suffix) {
        snprintf(name, 80, "__ny_contract_inline_%zu_%zu_%zu", depth, i,
                 suffix);
        bool collision = tp_contract_mentions_name(value, name);
        if (!collision && receiver)
          collision = tp_contract_mentions_name(receiver, name);
        for (size_t j = 0; !collision && j < call_args->len; ++j)
          collision = tp_contract_mentions_name(call_args->data[j].val, name);
        for (size_t j = 0; !collision && j < param_count; ++j)
          collision =
              tp_contract_mentions_name(fn->as.fn.params.data[j].def, name);
        if (!collision)
          break;
        if (suffix == SIZE_MAX)
          return NULL;
      }
      placeholder_names[i] = name;
      placeholders[i] = arena_alloc(arena, sizeof(*placeholders[i]));
      if (!placeholders[i])
        return NULL;
      *placeholders[i] = (expr_t){.kind = NY_E_IDENT, .tok = expr->tok};
      placeholders[i]->as.ident.name = name;
      expanded = ny_expr_subst(arena, expanded, param->name, placeholders[i]);
      if (!expanded)
        return NULL;
    }
    for (size_t i = 0; i < param_count; ++i) {
      if (assigned[i])
        continue;
      param_t *param = &fn->as.fn.params.data[i];
      if (!param->def)
        return NULL;
      expr_t *default_value = param->def;
      for (size_t j = 0; j < i; ++j) {
        if (!arguments[j])
          return NULL;
        default_value =
            ny_expr_subst(arena, default_value, fn->as.fn.params.data[j].name,
                          placeholders[j]);
        if (!default_value)
          return NULL;
      }
      for (size_t j = 0; j < i; ++j) {
        default_value = ny_expr_subst(arena, default_value,
                                      placeholder_names[j], arguments[j]);
        if (!default_value)
          return NULL;
      }
      arguments[i] = tp_contract_inline_pure_calls_depth(
          cg, arena, default_value, &fn->as.fn, depth + 1);
      if (!arguments[i])
        return NULL;
      assigned[i] = true;
    }
    for (size_t i = 0; i < param_count; ++i)
      if (!arguments[i])
        return NULL;
    if ((declared_pure || effect_polymorphic) &&
        !tp_contract_pure_body_supported(cg, arena, &fn->as.fn, fn->as.fn.body,
                                         placeholders, placeholder_names,
                                         arguments, 0)) {
      ny_proof_debug_at(2, expr->tok,
                        "pure predicate expansion skipped: unsupported body");
      return NULL;
    }
    for (size_t i = 0; i < param_count; ++i) {
      expanded =
          ny_expr_subst(arena, expanded, placeholder_names[i], arguments[i]);
      if (!expanded)
        return NULL;
    }
    return tp_contract_inline_pure_calls_depth(cg, arena, expanded, &fn->as.fn,
                                               depth + 1);
  }

  expr_t *left = NULL;
  expr_t *right = NULL;
  expr_t *cond = NULL;
  expr_t *yes = NULL;
  expr_t *no = NULL;
  switch (expr->kind) {
  case NY_E_IDENT:
  case NY_E_LITERAL:
    return expr;
  case NY_E_UNARY:
    right = tp_contract_inline_pure_calls_depth(cg, arena, expr->as.unary.right,
                                                context_fn, depth + 1);
    if (!right)
      return NULL;
    if (right == expr->as.unary.right)
      return expr;
    break;
  case NY_E_BINARY:
    left = tp_contract_inline_pure_calls_depth(cg, arena, expr->as.binary.left,
                                               context_fn, depth + 1);
    right = tp_contract_inline_pure_calls_depth(
        cg, arena, expr->as.binary.right, context_fn, depth + 1);
    if (!left || !right)
      return NULL;
    if (left == expr->as.binary.left && right == expr->as.binary.right)
      return expr;
    break;
  case NY_E_LOGICAL:
    left = tp_contract_inline_pure_calls_depth(cg, arena, expr->as.logical.left,
                                               context_fn, depth + 1);
    right = tp_contract_inline_pure_calls_depth(
        cg, arena, expr->as.logical.right, context_fn, depth + 1);
    if (!left || !right)
      return NULL;
    if (left == expr->as.logical.left && right == expr->as.logical.right)
      return expr;
    break;
  case NY_E_MEMBER:
    left = tp_contract_inline_pure_calls_depth(
        cg, arena, expr->as.member.target, context_fn, depth + 1);
    if (!left)
      return NULL;
    if (left == expr->as.member.target)
      return expr;
    break;
  case NY_E_TERNARY:
    cond = tp_contract_inline_pure_calls_depth(cg, arena, expr->as.ternary.cond,
                                               context_fn, depth + 1);
    yes = tp_contract_inline_pure_calls_depth(
        cg, arena, expr->as.ternary.true_expr, context_fn, depth + 1);
    no = tp_contract_inline_pure_calls_depth(
        cg, arena, expr->as.ternary.false_expr, context_fn, depth + 1);
    if (!cond || !yes || !no)
      return NULL;
    if (cond == expr->as.ternary.cond && yes == expr->as.ternary.true_expr &&
        no == expr->as.ternary.false_expr)
      return expr;
    break;
  default:
    return NULL;
  }

  expr_t *copy = arena_alloc(arena, sizeof(*copy));
  if (!copy)
    return NULL;
  *copy = *expr;
  if (expr->kind == NY_E_UNARY)
    copy->as.unary.right = right;
  else if (expr->kind == NY_E_BINARY) {
    copy->as.binary.left = left;
    copy->as.binary.right = right;
  } else if (expr->kind == NY_E_LOGICAL) {
    copy->as.logical.left = left;
    copy->as.logical.right = right;
  } else if (expr->kind == NY_E_MEMBER) {
    copy->as.member.target = left;
  } else {
    copy->as.ternary.cond = cond;
    copy->as.ternary.true_expr = yes;
    copy->as.ternary.false_expr = no;
  }
  return copy;
}

static bool tp_contract_decide(tp_requires_proof_ctx_t *ctx, expr_t *obligation,
                               ny_proof_policy_result_t *result) {
  if (!ctx || !ctx->tp || !ctx->tp->cg || !obligation)
    return false;
  arena_t *arena = ctx->tp->cg->arena;
  expr_t *raw_premise =
      ctx->premise ? tp_contract_inline_pure_calls_depth(
                         ctx->tp->cg, arena, ctx->premise, ctx->current_fn, 0)
                   : NULL;
  expr_t *raw_goal = tp_contract_inline_pure_calls_depth(
      ctx->tp->cg, arena, obligation, ctx->current_fn, 0);
  if (!raw_goal || (ctx->premise && !raw_premise)) {
    ny_proof_debug_at(2, obligation->tok,
                      "shared contract solver skipped: unsupported predicate "
                      "expansion");
    return false;
  }
  bool goal_has_custom_operator = tp_contract_has_custom_operator_for_fn(
      ctx->tp, raw_goal, ctx->current_fn);
  bool premise_has_custom_operator =
      raw_premise && tp_contract_has_custom_operator_for_fn(
                         ctx->tp, raw_premise, ctx->current_fn);
  if (goal_has_custom_operator || premise_has_custom_operator) {
    ny_proof_debug_at(2, obligation->tok,
                      "shared contract solver skipped: custom operator");
    return false;
  }
  expr_t *premise = raw_premise
                        ? tp_contract_bool_to_integer_formula(
                              arena, ctx, ctx->current_fn, raw_premise, 0)
                        : NULL;
  expr_t *goal = tp_contract_bool_to_integer_formula(
      arena, ctx, ctx->current_fn, raw_goal, 0);
  if (!goal || (ctx->premise && !premise)) {
    ny_proof_debug_at(2, obligation->tok,
                      "shared contract solver skipped: boolean lowering "
                      "failed");
    return false;
  }
  size_t binding_capacity = 0;
  if (!tp_contract_count_names(premise, 0, &binding_capacity) ||
      !tp_contract_count_names(goal, 0, &binding_capacity) ||
      binding_capacity == SIZE_MAX ||
      binding_capacity + 1 > SIZE_MAX / sizeof(binding)) {
    ny_proof_debug_at(2, obligation->tok,
                      "shared contract solver skipped: identifier set is too "
                      "large");
    return false;
  }
  if (binding_capacity == 0)
    binding_capacity = 1;
  binding *parameters = calloc(binding_capacity, sizeof(*parameters));
  if (!parameters) {
    ny_proof_debug_at(2, obligation->tok,
                      "shared contract solver skipped: binding allocation "
                      "failed");
    return false;
  }
  scope proof_scope = {0};
  proof_scope.vars.data = parameters;
  proof_scope.vars.cap = binding_capacity;
  if (!tp_contract_bind_names(ctx, premise, &proof_scope, 0) ||
      !tp_contract_bind_names(ctx, goal, &proof_scope, 0)) {
    free(parameters);
    ny_proof_debug_at(2, obligation->tok,
                      "shared contract solver skipped: unsupported identifier");
    return false;
  }
  /*
   * A conservative bloom filter avoids heap allocation for these temporary
   * bindings. They also shadow same-named globals during proof search.
   */
  for (size_t i = 0; i < 4; ++i)
    proof_scope.name_bloom[i] = UINT64_MAX;
  expr_t negated = {.kind = NY_E_UNARY, .tok = goal->tok};
  negated.as.unary.op = "!";
  negated.as.unary.right = premise;
  expr_t implication = {.kind = NY_E_LOGICAL, .tok = goal->tok};
  implication.as.logical.op = "||";
  implication.as.logical.left = premise ? &negated : NULL;
  implication.as.logical.right = goal;
  bool decided =
      ny_proof_policy_decide(ctx->tp->cg, &proof_scope, 0,
                             premise ? &implication : goal, "", true, result);
  ny_proof_debug_at(2, obligation->tok, "shared contract solver result=%s",
                    decided && result->verdict == NY_PROOF_VERDICT_PROVED
                        ? "proved"
                        : (decided ? "unknown or refuted" : "undecided"));
  if (!decided || result->verdict != NY_PROOF_VERDICT_PROVED)
    ny_proof_debug_at(
        2, obligation->tok,
        "shared contract solver detail: decided=%s verdict=%d tactic=%s "
        "backend=%s explanation=%s",
        decided ? "yes" : "no", result->verdict,
        result->tactic[0] ? result->tactic : "none",
        result->backend[0] ? result->backend : "none",
        result->explanation[0] ? result->explanation : "none");
  if ((!decided || result->verdict != NY_PROOF_VERDICT_PROVED) &&
      ny_proof_debug_level() >= 3)
    ny_proof_debug_at(3, obligation->tok,
                      "shared contract solver obligation: %s",
                      result->obligation[0] ? result->obligation : "none");
  free(parameters);
  return decided && result->verdict == NY_PROOF_VERDICT_PROVED;
}

static bool
tp_contract_structural_child_contains(const tp_requires_proof_ctx_t *ctx,
                                      const char *name) {
  if (!ctx || !name)
    return false;
  for (size_t i = 0; i < ctx->structural_child_count; ++i)
    if (ctx->structural_children[i] &&
        strcmp(ctx->structural_children[i], name) == 0)
      return true;
  return false;
}

static bool tp_contract_structural_children_add(tp_requires_proof_ctx_t *ctx,
                                                const char *name) {
  if (!ctx || !ctx->tp || !ctx->tp->cg || !ctx->tp->cg->arena || !name)
    return false;
  if (tp_contract_structural_child_contains(ctx, name))
    return true;
  size_t count = ctx->structural_child_count + 1;
  const char **children =
      arena_alloc(ctx->tp->cg->arena, count * sizeof(*children));
  if (!children)
    return false;
  if (ctx->structural_child_count)
    memcpy(children, ctx->structural_children,
           ctx->structural_child_count * sizeof(*children));
  children[count - 1] = name;
  ctx->structural_children = children;
  ctx->structural_child_count = count;
  return true;
}

static void tp_contract_structural_children_remove(tp_requires_proof_ctx_t *ctx,
                                                   const char *name) {
  if (!ctx || !name || !tp_contract_structural_child_contains(ctx, name))
    return;
  size_t count = ctx->structural_child_count - 1;
  const char **children =
      count && ctx->tp && ctx->tp->cg && ctx->tp->cg->arena
          ? arena_alloc(ctx->tp->cg->arena, count * sizeof(*children))
          : NULL;
  if (count && !children) {
    ctx->structural_children = NULL;
    ctx->structural_child_count = 0;
    return;
  }
  size_t out = 0;
  for (size_t i = 0; i < ctx->structural_child_count; ++i)
    if (ctx->structural_children[i] &&
        strcmp(ctx->structural_children[i], name) != 0)
      children[out++] = ctx->structural_children[i];
  ctx->structural_children = children;
  ctx->structural_child_count = out;
}

static const expr_t *
tp_contract_structural_measure_root(const tp_requires_proof_ctx_t *ctx,
                                    const stmt_func_t *fn,
                                    const expr_t *measure) {
  if (!measure)
    return NULL;
  if (measure->kind == NY_E_IDENT)
    return measure;
  if (measure->kind == NY_E_TUPLE && measure->as.list_like.len >= 2) {
    for (size_t i = 0; i < measure->as.list_like.len; ++i) {
      expr_t *component = measure->as.list_like.data[i];
      if (component && component->kind == NY_E_IDENT &&
          !tp_contract_int_term(ctx, fn, component, 0) &&
          !tp_contract_bool_term(ctx, fn, component, 0))
        return component;
    }
  }
  return NULL;
}

static size_t tp_contract_structural_measure_index(const expr_t *measure,
                                                   const expr_t *root) {
  if (!measure || !root)
    return SIZE_MAX;
  if (measure->kind != NY_E_TUPLE)
    return measure == root ? 0 : SIZE_MAX;
  for (size_t i = 0; i < measure->as.list_like.len; ++i)
    if (measure->as.list_like.data[i] == root)
      return i;
  return SIZE_MAX;
}

static size_t tp_contract_measure_component_count(const expr_t *measure) {
  return measure && measure->kind == NY_E_TUPLE ? measure->as.list_like.len
         : measure                              ? 1
                                                : 0;
}

static bool tp_contract_is_enum_type(const codegen_t *cg,
                                     const char *type_name) {
  if (!cg || !type_name || !*type_name)
    return false;
  for (size_t i = 0; i < cg->enums.len; ++i) {
    const enum_def_t *enum_def = cg->enums.data[i];
    if (enum_def && enum_def->name && strcmp(enum_def->name, type_name) == 0)
      return true;
  }
  return false;
}

static bool tp_contract_structural_pattern_children(
    tp_requires_proof_ctx_t *ctx, const stmt_func_t *fn, expr_t *match_test,
    match_arm_t *arms, size_t arm_count, stmt_t *arm_body,
    const char ***children_out, size_t *count_out) {
  if (children_out)
    *children_out = NULL;
  if (count_out)
    *count_out = 0;
  const expr_t *measure_root =
      fn ? tp_contract_structural_measure_root(ctx, fn, fn->decreases_expr)
         : NULL;
  if (!ctx || !ctx->tp || !ctx->tp->cg || !fn || !match_test || !arms ||
      !arm_body || !measure_root || !measure_root->as.ident.name ||
      match_test->kind != NY_E_IDENT || !match_test->as.ident.name)
    return false;
  bool is_measure =
      strcmp(measure_root->as.ident.name, match_test->as.ident.name) == 0;
  bool is_descendant =
      tp_contract_structural_child_contains(ctx, match_test->as.ident.name);
  if (!is_measure && !is_descendant)
    return false;

  const char *measure_type = NULL;
  for (size_t i = 0; i < fn->params.len; ++i) {
    const param_t *param = &fn->params.data[i];
    if (param->name && strcmp(param->name, measure_root->as.ident.name) == 0) {
      measure_type = param->type;
      break;
    }
  }
  if (!measure_type)
    return false;

  for (size_t i = 0; i < arm_count; ++i) {
    match_arm_t *arm = &arms[i];
    if (arm->conseq != arm_body || arm->patterns.len != 1)
      continue;
    expr_t *pattern = arm->patterns.data[0];
    if (!pattern ||
        (pattern->kind != NY_E_CALL && pattern->kind != NY_E_MEMCALL))
      continue;
    char *member_name = ny_adt_member_call_full_name(ctx->tp->cg, pattern);
    enum_def_t *owner = NULL;
    enum_member_def_t *member =
        member_name ? lookup_enum_member_owner(ctx->tp->cg, member_name, &owner)
                    : NULL;
    call_arg_t *args = NULL;
    size_t arg_count = 0;
    if (!ny_expr_call_args(pattern, &args, &arg_count))
      continue;
    if (!member || !owner || !owner->name ||
        strcmp(owner->name, measure_type) != 0 ||
        arg_count != member->fields.len)
      continue;
    const char **children =
        member->fields.len ? arena_alloc(ctx->tp->cg->arena,
                                         member->fields.len * sizeof(*children))
                           : NULL;
    if (member->fields.len && !children)
      return false;
    size_t child_count = 0;
    for (size_t field = 0; field < member->fields.len; ++field) {
      const char *field_type = member->fields.data[field].type_name;
      expr_t *binding = args[field].val;
      if (!tp_contract_is_enum_type(ctx->tp->cg, field_type) || !binding ||
          binding->kind != NY_E_IDENT || !binding->as.ident.name ||
          strcmp(binding->as.ident.name, "_") == 0)
        continue;
      bool duplicate = false;
      for (size_t j = 0; j < child_count; ++j)
        duplicate |= strcmp(children[j], binding->as.ident.name) == 0;
      if (!duplicate)
        children[child_count++] = binding->as.ident.name;
    }
    if (!child_count)
      continue;
    if (children_out)
      *children_out = children;
    if (count_out)
      *count_out = child_count;
    return true;
  }
  return false;
}

static bool
tp_contract_structural_measure_descends(const tp_requires_proof_ctx_t *ctx,
                                        const stmt_func_t *fn,
                                        const expr_t *next_measure) {
  const expr_t *measure_root =
      fn ? tp_contract_structural_measure_root(ctx, fn, fn->decreases_expr)
         : NULL;
  const expr_t *next_root =
      tp_contract_structural_measure_root(ctx, fn, next_measure);
  return ctx && measure_root && measure_root->as.ident.name && next_root &&
         next_root->as.ident.name && ctx->structural_parent &&
         ctx->structural_child_count && !ctx->structural_child_rebound &&
         strcmp(measure_root->as.ident.name, ctx->structural_parent) == 0 &&
         tp_contract_structural_child_contains(ctx, next_root->as.ident.name);
}

static expr_t *tp_contract_structural_prefix_equal(
    arena_t *arena, token_t tok, const tp_requires_proof_ctx_t *ctx,
    const stmt_func_t *fn, const expr_t *next_measure,
    const expr_t *current_measure, size_t structural_index) {
  if (!arena || !next_measure || !current_measure)
    return NULL;
  expr_t *equal = NULL;
  for (size_t i = 0; i < structural_index; ++i) {
    expr_t *next = tp_contract_measure_component(next_measure, i);
    expr_t *current = tp_contract_measure_component(current_measure, i);
    bool next_bool = tp_contract_bool_term(ctx, fn, next, 0);
    bool current_bool = tp_contract_bool_term(ctx, fn, current, 0);
    expr_t *component_equal =
        next_bool && current_bool
            ? tp_contract_bool_equal(arena, tok, next, current)
        : !next_bool && !current_bool &&
                tp_contract_int_term(ctx, fn, next, 0) &&
                tp_contract_int_term(ctx, fn, current, 0)
            ? tp_contract_make_binary(arena, tok, "==", next, current)
            : NULL;
    if (!component_equal)
      return NULL;
    equal = equal ? tp_contract_make_logical(arena, tok, "&&", equal,
                                             component_equal)
                  : component_equal;
    if (!equal)
      return NULL;
  }
  if (equal)
    return equal;
  expr_t *true_value = arena_alloc(arena, sizeof(*true_value));
  if (!true_value)
    return NULL;
  *true_value = (expr_t){.kind = NY_E_LITERAL, .tok = tok};
  true_value->as.literal.kind = NY_LIT_BOOL;
  true_value->as.literal.as.b = true;
  return true_value;
}

static bool tp_requires_proof_stmt_pre(ny_visitor_t *visitor, stmt_t *stmt) {
  tp_requires_proof_ctx_t *ctx =
      visitor ? (tp_requires_proof_ctx_t *)visitor->ctx : NULL;
  if (!ctx || !stmt)
    return true;
  if (stmt->kind == NY_S_LEMMA || stmt->kind == NY_S_MACRO)
    return false;
  stmt_t *parent = ctx->stmt_stack.len
                       ? ctx->stmt_stack.data[ctx->stmt_stack.len - 1].stmt
                       : NULL;
  tp_requires_stmt_frame_t statement_frame = {
      .stmt = stmt,
      .premise = ctx->premise,
      .structural_parent = ctx->structural_parent,
      .structural_children = ctx->structural_children,
      .structural_child_count = ctx->structural_child_count,
      .structural_child_rebound = ctx->structural_child_rebound,
  };
  vec_push(&ctx->stmt_stack, statement_frame);
  if (stmt->kind != NY_S_FUNC) {
    expr_t *guard = NULL;
    expr_t *iterable_range_fact = NULL;
    bool negate = false;
    if (parent && parent->kind == NY_S_IF &&
        (stmt == parent->as.iff.conseq || stmt == parent->as.iff.alt)) {
      guard = parent->as.iff.test;
      negate = stmt == parent->as.iff.alt;
    } else if (parent && parent->kind == NY_S_WHILE &&
               stmt == parent->as.whl.body) {
      if (tp_statement_changes_premise(ctx, parent, ctx->premise))
        ctx->premise = NULL;
      guard = parent->as.whl.test;
    } else if (parent && parent->kind == NY_S_FOR &&
               stmt == parent->as.fr.body) {
      if (tp_statement_changes_premise(ctx, parent, ctx->premise))
        ctx->premise = NULL;
      tp_contract_invalidate(ctx, parent->as.fr.iter_var, false);
      tp_contract_invalidate(ctx, parent->as.fr.iter_index_var, false);
      tp_contract_structural_children_remove(ctx, parent->as.fr.iter_var);
      tp_contract_structural_children_remove(ctx, parent->as.fr.iter_index_var);
      guard = parent->as.fr.cond;
      iterable_range_fact =
          tp_contract_iterable_range_fact(ctx, ctx->current_fn, parent);
    }
    expr_t *match_test = NULL;
    match_arm_t *match_arms = NULL;
    size_t match_arm_count = 0;
    if (parent && parent->kind == NY_S_MATCH) {
      match_test = parent->as.match.test;
      match_arms = parent->as.match.arms.data;
      match_arm_count = parent->as.match.arms.len;
    } else if (ctx->match_expr_stack.len) {
      expr_t *match_expr =
          ctx->match_expr_stack.data[ctx->match_expr_stack.len - 1];
      match_test = match_expr->as.match.test;
      match_arms = match_expr->as.match.arms.data;
      match_arm_count = match_expr->as.match.arms.len;
    }
    const char **children = NULL;
    size_t child_count = 0;
    if (tp_contract_structural_pattern_children(
            ctx, ctx->current_fn, match_test, match_arms, match_arm_count, stmt,
            &children, &child_count)) {
      if (!tp_contract_structural_child_contains(ctx,
                                                 match_test->as.ident.name))
        ctx->structural_parent = match_test->as.ident.name;
      ctx->structural_children = children;
      ctx->structural_child_count = child_count;
      for (size_t i = 0; i < child_count; ++i) {
        expr_t child_ident = {.kind = NY_E_IDENT};
        child_ident.as.ident.name = (char *)children[i];
        ctx->structural_child_rebound =
            ctx->structural_child_rebound ||
            tp_statement_changes_premise(ctx, stmt, &child_ident);
      }
    }
    expr_t *guard_fact =
        ctx->current_fn ? tp_contract_guard_fact(ctx, ctx->current_fn, guard, 0)
                        : NULL;
    bool guard_fact_safe =
        guard_fact &&
        (!negate || tp_contract_int_formula(ctx, ctx->current_fn, guard, 0)) &&
        !tp_guard_changes_values(ctx, guard) &&
        !tp_contract_has_custom_operator(ctx->tp, guard);
    bool iterable_range_fact_safe =
        iterable_range_fact && parent &&
        !tp_statement_changes_premise(ctx, parent,
                                      parent->as.fr.iterable->as.binary.left) &&
        !tp_statement_changes_premise(
            ctx, parent, parent->as.fr.iterable->as.binary.right) &&
        !tp_guard_changes_values(ctx, parent->as.fr.iterable) &&
        !tp_contract_has_custom_operator(ctx->tp, parent->as.fr.iterable);
    if (iterable_range_fact_safe) {
      expr_t *range_guard = iterable_range_fact;
      if (guard_fact_safe)
        range_guard =
            tp_contract_make_logical(ctx->tp->cg->arena, range_guard->tok, "&&",
                                     range_guard, guard_fact);
      if (range_guard)
        ctx->premise = tp_contract_guard(ctx, ctx->premise, range_guard, false);
    }
    if (guard_fact_safe && !iterable_range_fact_safe)
      ctx->premise = tp_contract_guard(ctx, ctx->premise, guard_fact, negate);
    return true;
  }
  tp_requires_proof_frame_t frame = {
      .current_fn = ctx->current_fn,
      .premise = ctx->premise,
      .structural_parent = ctx->structural_parent,
      .structural_children = ctx->structural_children,
      .structural_child_count = ctx->structural_child_count,
      .structural_child_rebound = ctx->structural_child_rebound,
      .skip_expr_root = ctx->skip_expr_root,
      .skip_expr_depth = ctx->skip_expr_depth,
      .current_fn_params_rebound = ctx->current_fn_params_rebound,
  };
  vec_push(&ctx->fn_stack, frame);
  ctx->current_fn = &stmt->as.fn;
  ctx->premise = stmt->as.fn.requires_cond;
  ctx->structural_parent = NULL;
  ctx->structural_children = NULL;
  ctx->structural_child_count = 0;
  ctx->structural_child_rebound = false;
  ctx->current_fn_params_rebound = tp_statement_changes_premise(
      ctx, stmt->as.fn.body, ctx->current_fn->decreases_expr);
  ctx->skip_expr_root = ctx->current_fn->requires_cond
                            ? ctx->current_fn->requires_cond
                            : ctx->current_fn->decreases_expr;
  ctx->skip_expr_depth = 0;
  return true;
}

static void tp_requires_proof_stmt_post(ny_visitor_t *visitor, stmt_t *stmt) {
  tp_requires_proof_ctx_t *ctx =
      visitor ? (tp_requires_proof_ctx_t *)visitor->ctx : NULL;
  if (!ctx || !stmt || ctx->stmt_stack.len == 0 ||
      ctx->stmt_stack.data[ctx->stmt_stack.len - 1].stmt != stmt)
    return;
  tp_requires_stmt_frame_t statement_frame =
      ctx->stmt_stack.data[ctx->stmt_stack.len - 1];
  expr_t *if_fact =
      ctx->current_fn && stmt->kind == NY_S_IF
          ? tp_contract_guard_fact(ctx, ctx->current_fn, stmt->as.iff.test, 0)
          : NULL;
  --ctx->stmt_stack.len;
  ctx->premise = statement_frame.premise;
  ctx->structural_parent = statement_frame.structural_parent;
  ctx->structural_children = statement_frame.structural_children;
  ctx->structural_child_count = statement_frame.structural_child_count;
  ctx->structural_child_rebound = statement_frame.structural_child_rebound;
  if (stmt->kind != NY_S_FUNC) {
    if (stmt->kind == NY_S_VAR) {
      for (size_t i = 0; i < stmt->as.var.names.len; ++i)
        tp_contract_invalidate(ctx, stmt->as.var.names.data[i], false);
      if (ctx->structural_child_count && stmt->as.var.names.len) {
        bool shadows_child = false;
        if (stmt->as.var.is_decl || stmt->as.var.is_del)
          for (size_t i = 0; i < stmt->as.var.names.len; ++i) {
            const char *name = stmt->as.var.names.data[i];
            shadows_child |= tp_contract_structural_child_contains(ctx, name);
            tp_contract_structural_children_remove(ctx, name);
          }
        if (!shadows_child && !ctx->structural_child_rebound &&
            stmt->as.var.is_decl && !stmt->as.var.is_mut &&
            !stmt->as.var.is_del && !stmt->as.var.is_destructure &&
            stmt->as.var.names.len == 1 && stmt->as.var.names.data[0] &&
            stmt->as.var.exprs.len == 1 && stmt->as.var.exprs.data[0] &&
            stmt->as.var.exprs.data[0]->kind == NY_E_IDENT &&
            stmt->as.var.exprs.data[0]->as.ident.name &&
            tp_contract_structural_child_contains(
                ctx, stmt->as.var.exprs.data[0]->as.ident.name))
          (void)tp_contract_structural_children_add(ctx,
                                                    stmt->as.var.names.data[0]);
      }
    } else if (stmt->kind == NY_S_GUARD) {
      tp_contract_invalidate(ctx, stmt->as.guard.name, false);
      tp_contract_structural_children_remove(ctx, stmt->as.guard.name);
    }
    /*
     * The fallthrough after an early-return branch excludes that branch.
     */
    if (if_fact && tp_contract_stmt_returns(stmt->as.iff.conseq) &&
        tp_contract_int_formula(ctx, ctx->current_fn, stmt->as.iff.test, 0) &&
        !tp_statement_changes_premise(ctx, stmt->as.iff.alt,
                                      stmt->as.iff.test) &&
        !tp_guard_changes_values(ctx, stmt->as.iff.test) &&
        !tp_contract_has_custom_operator(ctx->tp, stmt->as.iff.test))
      ctx->premise = tp_contract_guard(ctx, ctx->premise, if_fact, true);
    else if (ctx->current_fn && stmt->kind == NY_S_IF &&
             tp_contract_stmt_returns(stmt->as.iff.alt) && if_fact &&
             !tp_statement_changes_premise(ctx, stmt->as.iff.conseq,
                                           stmt->as.iff.test) &&
             !tp_guard_changes_values(ctx, stmt->as.iff.test) &&
             !tp_contract_has_custom_operator(ctx->tp, stmt->as.iff.test))
      ctx->premise = tp_contract_guard(ctx, ctx->premise, if_fact, false);
    return;
  }
  if (ctx->fn_stack.len == 0)
    return;
  tp_requires_proof_frame_t frame = ctx->fn_stack.data[--ctx->fn_stack.len];
  ctx->current_fn = frame.current_fn;
  ctx->premise = frame.premise;
  ctx->structural_parent = frame.structural_parent;
  ctx->structural_children = frame.structural_children;
  ctx->structural_child_count = frame.structural_child_count;
  ctx->structural_child_rebound = frame.structural_child_rebound;
  ctx->skip_expr_root = frame.skip_expr_root;
  ctx->skip_expr_depth = frame.skip_expr_depth;
  ctx->current_fn_params_rebound = frame.current_fn_params_rebound;
}

static bool tp_requires_proof_expr_pre(ny_visitor_t *visitor, expr_t *expr) {
  tp_requires_proof_ctx_t *ctx =
      visitor ? (tp_requires_proof_ctx_t *)visitor->ctx : NULL;
  if (expr && (expr->kind == NY_E_LAMBDA || expr->kind == NY_E_FN))
    return false;
  if (ctx && expr && ctx->skip_expr_depth > 0) {
    ctx->skip_expr_depth++;
    return true;
  }
  if (ctx && expr && ctx->skip_expr_root == expr) {
    ctx->skip_expr_depth = 1;
    return true;
  }
  if (expr && (expr->kind == NY_E_COMPTIME || expr->kind == NY_E_QUOTE))
    return false;
  if (ctx && expr && expr->kind == NY_E_MATCH)
    vec_push(&ctx->match_expr_stack, expr);
  bool is_call = expr && expr->kind == NY_E_CALL;
  bool is_memcall = expr && expr->kind == NY_E_MEMCALL;
  const expr_t *call_callee = is_call ? expr->as.call.callee : NULL;
  while (call_callee && call_callee->kind == NY_E_SPLICE)
    call_callee = call_callee->as.splice.expr;

  const expr_t *member_target =
      is_memcall ? expr->as.memcall.target
      : (call_callee && call_callee->kind == NY_E_MEMBER)
          ? call_callee->as.member.target
          : NULL;
  bool module_call = false;
  char module_path[1024];
  if (member_target && ctx && ctx->tp && ctx->tp->cg)
    module_call = ny_resolve_module_expr_path(ctx->tp->cg, NULL, 0,
                                              (expr_t *)member_target,
                                              module_path, sizeof(module_path));
  if (module_call && !expr->semantic.canonical_callee_stmt && ctx && ctx->tp &&
      ctx->tp->cg) {
    const char *member_name = is_memcall ? expr->as.memcall.name
                              : (is_call && expr->as.call.callee &&
                                 expr->as.call.callee->kind == NY_E_MEMBER)
                                  ? expr->as.call.callee->as.member.name
                                  : NULL;
    char resolved_fun[1280];
    if (member_name &&
        ny_resolve_module_function_path(ctx->tp->cg, module_path, member_name,
                                        resolved_fun, sizeof(resolved_fun))) {
      fun_sig *resolved_sig = lookup_fun(ctx->tp->cg, resolved_fun, 0);
      if (resolved_sig && resolved_sig->stmt_t &&
          resolved_sig->stmt_t->kind == NY_S_FUNC) {
        expr->semantic.canonical_callee =
            ny_intern_get(ny_intern_cstr(resolved_sig->name));
        expr->semantic.canonical_callee_stmt = resolved_sig->stmt_t;
      }
    }
  }
  bool attached_call =
      !module_call && member_target &&
      expr->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED;
  const expr_t *receiver = !attached_call ? NULL
                           : is_memcall
                               ? expr->as.memcall.target
                               : expr->as.call.callee->as.member.target;
  const ny_call_arg_list *call_args = !expr        ? NULL
                                      : is_memcall ? &expr->as.memcall.args
                                      : is_call    ? &expr->as.call.args
                                                   : NULL;
  size_t receiver_count = attached_call ? 1 : 0;
  stmt_t *target = expr ? expr->semantic.canonical_callee_stmt : NULL;
  if (ctx && ctx->tp && target && target->kind == NY_S_FUNC &&
      target->as.fn.requires_cond &&
      tp_contract_has_custom_operator(ctx->tp, target->as.fn.requires_cond))
    expr->semantic.requires_check_custom_operator = true;
  if (ctx && ctx->current_fn && ctx->current_fn->requires_cond && is_call) {
    const expr_t *callee_expr = expr->as.call.callee;
    int target_kind = expr->semantic.canonical_callee_stmt
                          ? expr->semantic.canonical_callee_stmt->kind
                          : -1;
    ny_proof_debug_at(
        2, expr->tok,
        "shared requires saw call name=%s target_kind=%d direct_ident=%s",
        callee_expr && callee_expr->kind == NY_E_IDENT &&
                callee_expr->as.ident.name
            ? callee_expr->as.ident.name
            : "<other>",
        target_kind,
        callee_expr && callee_expr->kind == NY_E_IDENT ? "yes" : "no");
  }
  if (!ctx || !ctx->tp || !ctx->tp->cg || !expr || (!is_call && !is_memcall) ||
      (is_memcall && !attached_call && !module_call) ||
      (is_call && expr->as.call.callee &&
       expr->as.call.callee->kind == NY_E_MEMBER &&
       expr->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT &&
       !module_call) ||
      !expr->semantic.canonical_callee_stmt ||
      expr->semantic.canonical_callee_stmt->kind != NY_S_FUNC ||
      (is_call && (!call_callee || (call_callee->kind != NY_E_IDENT &&
                                    call_callee->kind != NY_E_MEMBER))))
    return true;

  stmt_func_t *callee = &expr->semantic.canonical_callee_stmt->as.fn;
  fun_sig *callee_sig =
      expr->semantic.canonical_callee
          ? lookup_fun_exact(ctx->tp->cg, expr->semantic.canonical_callee)
          : NULL;
  if (callee_sig && callee_sig->stmt_t != expr->semantic.canonical_callee_stmt)
    callee_sig = NULL;
  const char *callee_module_name = callee_sig ? callee_sig->module_name : NULL;
  if ((!callee_module_name || !*callee_module_name) &&
      expr->semantic.canonical_callee) {
    const char *dot = strrchr(expr->semantic.canonical_callee, '.');
    if (dot && dot != expr->semantic.canonical_callee) {
      size_t module_len = (size_t)(dot - expr->semantic.canonical_callee);
      char *module_name = arena_alloc(ctx->tp->cg->arena, module_len + 1);
      if (module_name) {
        memcpy(module_name, expr->semantic.canonical_callee, module_len);
        module_name[module_len] = '\0';
        callee_module_name = module_name;
      }
    }
  }
  const char *callee_source_file =
      callee_sig && callee_sig->source_file
          ? callee_sig->source_file
          : expr->semantic.canonical_callee_stmt->tok.filename;
  bool requires_proof =
      callee->requires_cond && !expr->semantic.requires_proven_by_caller;
  bool decreases_proof = ctx->current_fn && !ctx->current_fn_params_rebound &&
                         ctx->current_fn->decreases_expr &&
                         callee->decreases_expr &&
                         !expr->semantic.decreases_proven_by_caller;
  if (requires_proof) {
    expr->semantic.requires_obligation = NULL;
    expr->semantic.requires_premise = NULL;
    expr->semantic.requires_obligation_checked = false;
    expr->semantic.requires_obligation_proved = false;
  }
  if (decreases_proof) {
    expr->semantic.decreases_descent_obligation = NULL;
    expr->semantic.decreases_lower_bound_obligation = NULL;
    expr->semantic.decreases_obligation_checked = false;
    expr->semantic.decreases_obligation_proved = false;
    expr->semantic.decreases_obligation_structural = false;
  }
  if (!requires_proof && !decreases_proof)
    return true;
  size_t explicit_arg_count = call_args ? call_args->len : 0;
  size_t arg_count = explicit_arg_count + receiver_count;
  size_t parameter_count = callee->params.len;
  size_t callee_measure_arity =
      decreases_proof
          ? tp_contract_measure_arity(NULL, callee, callee->decreases_expr)
          : 0;
  size_t caller_measure_arity =
      decreases_proof
          ? tp_contract_measure_arity(ctx, ctx->current_fn,
                                      ctx->current_fn->decreases_expr)
          : 0;
  const expr_t *callee_structural_root =
      decreases_proof ? tp_contract_structural_measure_root(
                            NULL, callee, callee->decreases_expr)
                      : NULL;
  const expr_t *caller_structural_root =
      decreases_proof
          ? tp_contract_structural_measure_root(ctx, ctx->current_fn,
                                                ctx->current_fn->decreases_expr)
          : NULL;
  size_t callee_structural_index = tp_contract_structural_measure_index(
      callee->decreases_expr, callee_structural_root);
  size_t caller_structural_index = tp_contract_structural_measure_index(
      ctx->current_fn ? ctx->current_fn->decreases_expr : NULL,
      caller_structural_root);
  bool structural_measure_candidate =
      decreases_proof && callee_measure_arity == 0 &&
      caller_measure_arity == 0 && callee_structural_root &&
      caller_structural_root &&
      callee_structural_index == caller_structural_index &&
      tp_contract_measure_component_count(callee->decreases_expr) ==
          tp_contract_measure_component_count(
              ctx->current_fn ? ctx->current_fn->decreases_expr : NULL);
  ny_proof_debug_at(
      2, expr->tok, "shared requires candidate caller=%s callee=%s",
      ctx->current_fn && ctx->current_fn->name ? ctx->current_fn->name
                                               : "<top-level>",
      callee->name ? callee->name : "<anon>");
  if (!call_args || parameter_count < arg_count) {
    ny_proof_debug_at(2, expr->tok,
                      "shared requires skipped: missing callee contract or "
                      "unsupported argument arity");
    return true;
  }
  arena_t *arena = ctx->tp->cg->arena;
  expr_t *callee_formula =
      requires_proof ? tp_contract_inline_pure_calls_depth(
                           ctx->tp->cg, arena, callee->requires_cond, callee, 0)
                     : NULL;
  expr_t *caller_formula =
      ctx->premise ? tp_contract_inline_pure_calls_depth(
                         ctx->tp->cg, arena, ctx->premise, ctx->current_fn, 0)
                   : NULL;
  bool requires_formula_supported =
      !requires_proof ||
      (callee_formula &&
       tp_contract_int_formula(NULL, callee, callee_formula, 0) &&
       (!ctx->premise ||
        (caller_formula &&
         tp_contract_int_formula(ctx, ctx->current_fn, caller_formula, 0))));
  bool exact_requires_fallback =
      requires_proof && (!requires_formula_supported || !ctx->current_fn) &&
      !ctx->premise;
  if ((ctx->premise &&
       !tp_contract_int_formula(ctx, ctx->current_fn, ctx->premise, 0)) ||
      (requires_proof && !requires_formula_supported &&
       !exact_requires_fallback) ||
      (decreases_proof &&
       (callee_measure_arity == 0 ||
        caller_measure_arity != callee_measure_arity) &&
       !structural_measure_candidate)) {
    ny_proof_debug_at(2, expr->tok,
                      "shared requires skipped: unsupported integer "
                      "contract formula");
    return true;
  }
  expr_t *obligation = callee->requires_cond;
  expr_t *next_measure = decreases_proof ? callee->decreases_expr : NULL;
  expr_t **actuals =
      parameter_count ? arena_alloc(arena, parameter_count * sizeof(*actuals))
                      : NULL;
  bool *assigned = parameter_count
                       ? arena_alloc(arena, parameter_count * sizeof(*assigned))
                       : NULL;
  bool *needed = parameter_count
                     ? arena_alloc(arena, parameter_count * sizeof(*needed))
                     : NULL;
  expr_t *placeholders =
      parameter_count
          ? arena_alloc(arena, parameter_count * sizeof(*placeholders))
          : NULL;
  const char **placeholder_names =
      parameter_count
          ? arena_alloc(arena, parameter_count * sizeof(*placeholder_names))
          : NULL;
  if (parameter_count &&
      (!actuals || !assigned || !needed || !placeholders || !placeholder_names))
    return true;
  for (size_t i = 0; i < parameter_count; ++i) {
    assigned[i] = false;
    actuals[i] = NULL;
    needed[i] =
        tp_contract_mentions_name(obligation, callee->params.data[i].name) ||
        tp_contract_mentions_name(next_measure, callee->params.data[i].name);
  }
  if (attached_call) {
    const char *receiver_name = callee->params.data[0].name;
    bool receiver_used = tp_contract_mentions_name(obligation, receiver_name) ||
                         tp_contract_mentions_name(next_measure, receiver_name);
    if (!receiver ||
        (receiver_used &&
         !tp_contract_int_term(ctx, ctx->current_fn, receiver, 0)) ||
        tp_contract_has_custom_operator(ctx->tp, (expr_t *)receiver) ||
        tp_guard_changes_values(ctx, (expr_t *)receiver))
      return true;
    actuals[0] = (expr_t *)receiver;
    assigned[0] = true;
  }
  /*
   * Match call normalization: positional arguments fill the next unassigned
   * formal, even when named arguments appeared earlier in source order.
   */
  size_t next_positional = receiver_count;
  for (size_t i = 0; i < explicit_arg_count; ++i) {
    const call_arg_t *arg = &call_args->data[i];
    size_t parameter_index = callee->params.len;
    if (arg->name) {
      parameter_index = callee->params.len;
      for (size_t j = receiver_count; j < callee->params.len; ++j)
        if (arg->name && callee->params.data[j].name &&
            strcmp(arg->name, callee->params.data[j].name) == 0) {
          parameter_index = j;
          break;
        }
    } else {
      while (next_positional < callee->params.len && assigned[next_positional])
        ++next_positional;
      parameter_index = next_positional++;
    }
    if (parameter_index >= callee->params.len || assigned[parameter_index] ||
        !arg->val || !callee->params.data[parameter_index].name)
      return true;
    if (tp_contract_has_custom_operator(ctx->tp, arg->val) ||
        tp_guard_changes_values(ctx, arg->val)) {
      ny_proof_debug_at(2, expr->tok,
                        "shared requires skipped: custom operator or "
                        "side-effecting argument");
      return true;
    }
    assigned[parameter_index] = true;
    actuals[parameter_index] = arg->val;
  }
  /*
   * A default can use an earlier formal whose value then flows into the
   * contract even when the contract names only a later formal. Propagate this
   * dependency backwards through omitted defaults before validating actuals.
   */
  for (size_t i = parameter_count; i-- > 0;) {
    if (!needed[i] || assigned[i] || !callee->params.data[i].def)
      continue;
    const expr_t *def = callee->params.data[i].def;
    for (size_t j = 0; j < i; ++j)
      if (tp_contract_mentions_name(def, callee->params.data[j].name))
        needed[j] = true;
  }
  for (size_t i = 0; i < parameter_count; ++i) {
    if (!assigned[i] || !needed[i])
      continue;
    bool bool_parameter = callee->params.data[i].type &&
                          strcmp(callee->params.data[i].type, "bool") == 0;
    bool valid_actual =
        bool_parameter
            ? tp_contract_bool_term(ctx, ctx->current_fn, actuals[i], 0)
            : tp_contract_int_term(ctx, ctx->current_fn, actuals[i], 0);
    bool structural_measure_actual =
        structural_measure_candidate && callee_structural_root &&
        callee_structural_root->as.ident.name && callee->params.data[i].name &&
        strcmp(callee_structural_root->as.ident.name,
               callee->params.data[i].name) == 0 &&
        tp_contract_structural_measure_descends(ctx, ctx->current_fn,
                                                actuals[i]);
    valid_actual = valid_actual || structural_measure_actual;
    if (!valid_actual && !exact_requires_fallback) {
      ny_proof_debug_at(2, expr->tok,
                        "shared requires skipped: argument is not a supported "
                        "integer or boolean term");
      return true;
    }
  }
  for (size_t i = 0; i < parameter_count; ++i) {
    if (assigned[i])
      continue;
    const param_t *parameter = &callee->params.data[i];
    if (!parameter->def)
      return true;
    expr_t *default_value =
        needed[i] && !exact_requires_fallback
            ? tp_contract_default_int_value(ctx, callee_module_name,
                                            callee_source_file,
                                            expr->tok.filename, callee, i,
                                            actuals, assigned, parameter->def)
            : parameter->def;
    if (!default_value)
      return true;
    actuals[i] = default_value;
    assigned[i] = true;
  }
  for (size_t i = 0; i < parameter_count; ++i) {
    placeholder_names[i] = NULL;
    if (!tp_contract_mentions_name(obligation, callee->params.data[i].name) &&
        !tp_contract_mentions_name(next_measure, callee->params.data[i].name))
      continue;
    char name_buf[64];
    int name_len =
        snprintf(name_buf, sizeof(name_buf), "\001ny_contract_%zu", i);
    if (name_len <= 0 || (size_t)name_len >= sizeof(name_buf))
      return true;
    char *name = arena_alloc(arena, (size_t)name_len + 1);
    if (!name)
      return true;
    memcpy(name, name_buf, (size_t)name_len + 1);
    placeholder_names[i] = name;
    placeholders[i] = (expr_t){.kind = NY_E_IDENT, .tok = expr->tok};
    placeholders[i].as.ident.name = name;
    if (obligation)
      obligation = ny_expr_subst(arena, obligation, callee->params.data[i].name,
                                 &placeholders[i]);
    if (next_measure)
      next_measure = ny_expr_subst(
          arena, next_measure, callee->params.data[i].name, &placeholders[i]);
  }
  for (size_t i = 0; i < parameter_count; ++i) {
    if (!placeholder_names[i])
      continue;
    if (obligation)
      obligation =
          ny_expr_subst(arena, obligation, placeholder_names[i], actuals[i]);
    if (next_measure)
      next_measure =
          ny_expr_subst(arena, next_measure, placeholder_names[i], actuals[i]);
  }
  bool has_custom_operator =
      tp_contract_has_custom_operator_for_fn(ctx->tp, ctx->premise,
                                             ctx->current_fn) ||
      tp_contract_has_custom_operator_for_fn(ctx->tp, obligation,
                                             ctx->current_fn) ||
      tp_contract_has_custom_operator_for_fn(ctx->tp, next_measure,
                                             ctx->current_fn) ||
      (decreases_proof &&
       tp_contract_has_custom_operator_for_fn(
           ctx->tp, ctx->current_fn->decreases_expr, ctx->current_fn));
  if (has_custom_operator) {
    ny_proof_debug_at(2, expr->tok,
                      "shared requires skipped: active custom operator");
    return true;
  }

  if (requires_proof) {
    expr->semantic.requires_obligation = obligation;
    expr->semantic.requires_premise = ctx->premise;
  }

  /*
   * The caller-side implication is a contract-safe obligation: the policy
   * funnel records it and only accepts proved certificates from the
   * bounded kernels under the wrapping-arithmetic guard.
   */
  if (requires_proof) {
    ny_proof_policy_result_t result = {0};
    bool proved = false;
    if (exact_requires_fallback) {
      /*
       * General predicates remain outside the bounded arithmetic checker.
       * If argument substitution made the complete predicate compile-time
       * exact, accept only an independently evaluated true result; false or
       * unknown still receives no caller-side proof.
       */
      ny_ct_fast_val_t value = ny_ct_fast_none();
      bool evaluated =
          ny_try_eval_comptime_expr_fast(ctx->tp->cg, obligation, &value, 0);
      if (!evaluated && tp_contract_calls_are_pure(ctx->tp->cg, ctx->current_fn,
                                                   obligation)) {
        stmt_t expr_stmt = {.kind = NY_S_EXPR, .tok = obligation->tok};
        expr_stmt.as.expr.expr = obligation;
        evaluated =
            ny_try_eval_comptime_interp_value(ctx->tp->cg, &expr_stmt, &value);
      }
      proved = evaluated && value.kind == NY_CT_FAST_BOOL && value.b;
      ny_ct_fast_val_free(&value);
    } else {
      proved = tp_contract_decide(ctx, obligation, &result);
    }
    ny_proof_debug_at(2, expr->tok, "shared requires implication %s",
                      proved ? "proved" : "unknown or refuted");
    expr->semantic.requires_obligation_checked = true;
    expr->semantic.requires_obligation_proved = proved;
    if (proved)
      expr->semantic.requires_proven_by_caller = true;
  }
  if (decreases_proof) {
    bool structural_descends =
        structural_measure_candidate && tp_contract_structural_measure_descends(
                                            ctx, ctx->current_fn, next_measure);
    bool structural_prefix_equal =
        structural_descends && caller_structural_index == 0;
    if (structural_descends && !structural_prefix_equal) {
      expr_t *prefix_equal = tp_contract_structural_prefix_equal(
          arena, expr->tok, ctx, ctx->current_fn, next_measure,
          ctx->current_fn->decreases_expr, caller_structural_index);
      ny_proof_policy_result_t prefix_result = {0};
      structural_prefix_equal =
          prefix_equal && tp_contract_decide(ctx, prefix_equal, &prefix_result);
    }
    if (structural_descends && structural_prefix_equal) {
      expr->semantic.decreases_descent_obligation = next_measure;
      expr->semantic.decreases_obligation_checked = true;
      expr->semantic.decreases_obligation_proved = true;
      expr->semantic.decreases_obligation_structural = true;
      expr->semantic.decreases_proven_by_caller = true;
    } else {
      expr_t *descent = tp_contract_lex_decrease(
          arena, expr->tok, ctx, ctx->current_fn, next_measure,
          ctx->current_fn->decreases_expr, caller_measure_arity);
      expr_t *lower_bound = tp_contract_measure_lower_bound(
          arena, expr->tok, ctx, ctx->current_fn,
          ctx->current_fn->decreases_expr, caller_measure_arity);
      ny_proof_policy_result_t descent_result = {0};
      ny_proof_policy_result_t bound_result = {0};
      bool descends =
          descent && tp_contract_decide(ctx, descent, &descent_result);
      bool bounded =
          lower_bound && tp_contract_decide(ctx, lower_bound, &bound_result);
      expr->semantic.decreases_descent_obligation = descent;
      expr->semantic.decreases_lower_bound_obligation = lower_bound;
      expr->semantic.decreases_obligation_checked = true;
      expr->semantic.decreases_obligation_proved = descends && bounded;
      ny_proof_debug_at(
          2, expr->tok,
          "shared recursive measure checks descent=%s lower-bound=%s",
          descends ? "proved" : "unknown", bounded ? "proved" : "unknown");
      expr->semantic.decreases_proven_by_caller = descends && bounded;
    }
    ny_proof_debug_at(2, expr->tok, "shared recursive measure %s",
                      expr->semantic.decreases_proven_by_caller
                          ? "strictly descends without overflow and is "
                            "bounded below on recursive edges"
                          : "descent or lower bound is unknown");
  }
  return true;
}

static void tp_requires_proof_expr_post(ny_visitor_t *visitor, expr_t *expr) {
  tp_requires_proof_ctx_t *ctx =
      visitor ? (tp_requires_proof_ctx_t *)visitor->ctx : NULL;
  if (!ctx || !expr)
    return;
  if (expr->kind == NY_E_MATCH && ctx->match_expr_stack.len &&
      ctx->match_expr_stack.data[ctx->match_expr_stack.len - 1] == expr)
    --ctx->match_expr_stack.len;
  if (ctx->skip_expr_depth == 0) {
    if (tp_contract_call_may_write(ctx->tp->cg, expr))
      tp_contract_invalidate(ctx, NULL, true);
    else
      tp_contract_invalidate(ctx, tp_contract_assigned_name(expr), false);
    return;
  }
  ctx->skip_expr_depth--;
  if (ctx->skip_expr_depth != 0)
    return;
  if (ctx->current_fn &&
      ctx->current_fn->requires_cond == ctx->skip_expr_root &&
      ctx->current_fn->decreases_expr != ctx->skip_expr_root)
    ctx->skip_expr_root = ctx->current_fn->decreases_expr;
  else
    ctx->skip_expr_root = NULL;
}

static void tp_validate_contract_call_proofs(program_t *prog, ny_tp_ctx_t *tp) {
  if (!prog || !tp || !tp->cg)
    return;
  tp_requires_proof_ctx_t ctx = {.tp = tp};
  ny_visitor_t visitor = {
      .ctx = &ctx,
      .visit_expr_pre = tp_requires_proof_expr_pre,
      .visit_expr_post = tp_requires_proof_expr_post,
      .visit_stmt_pre = tp_requires_proof_stmt_pre,
      .visit_stmt_post = tp_requires_proof_stmt_post,
  };
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_visit_stmt(&visitor, prog->body.data[i]);
  vec_free(&ctx.fn_stack);
  vec_free(&ctx.stmt_stack);
  vec_free(&ctx.match_expr_stack);
}

bool ny_type_pipeline_verify_proof_claims(program_t *prog, codegen_t *cg,
                                          const char *source_name) {
  if (!prog || !cg)
    return false;
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, false);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  tp_validate_contract_call_proofs(prog, &ctx);
  bool valid = ny_verify_totality(cg);
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return valid;
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

  tp_validate_contract_call_proofs(prog, &ctx);
  if (!ny_verify_totality(cg)) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_TRAIT;
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return 1;
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
  /*
   * The same declared type may appear leaf-qualified at one site and fully
   * qualified at another (gf.ny operators declare gf2bv_bv while the
   * impl methods carry core.math.crypto.gf.gf2bv_bv).
   */
  {
    const char *wl = strrchr(w, '.');
    const char *gl = strrchr(g, '.');
    wl = wl ? wl + 1 : w;
    gl = gl ? gl + 1 : g;
    if (strcmp(wl, gl) == 0)
      return true;
  }
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
  if (!ctx || !ctx->cg || !def || !def->target_name)
    return NULL;
  fun_sig *sig = tp_lookup_sig(ctx->cg, def->target_name);
  if (sig)
    return sig;

  /*
   * Operator declarations name attached targets as
   * `module.owner.method`, while the source signature can be indexed by its
   * module-qualified function name.  Resolve the owner-qualified form against
   * the attached signature metadata instead of rejecting a valid method when
   * its flattened name differs.
   */
  const char *target_module = def->module_name;
  const char *target_owner = def->left_type;
  const char *target_method = def->target_name;
  const char *method_dot = strrchr(def->target_name, '.');
  if (method_dot) {
    if (method_dot == def->target_name || !method_dot[1])
      return NULL;
    const char *owner_dot = method_dot;
    while (owner_dot > def->target_name && owner_dot[-1] != '.')
      owner_dot--;
    if (owner_dot == def->target_name || owner_dot == method_dot)
      return NULL;
    owner_dot--;
    size_t module_len = (size_t)(owner_dot - def->target_name);
    size_t owner_len = (size_t)(method_dot - owner_dot - 1);
    if (module_len == 0 || module_len >= 1024)
      return NULL;
    char target_module_name[1024];
    memcpy(target_module_name, def->target_name, module_len);
    target_module_name[module_len] = '\0';
    const char *target_owner_name = owner_dot + 1;
    fun_sig *module_alias_match = NULL;
    for (size_t i = 0; i < ctx->cg->fun_sigs.len; ++i) {
      fun_sig *candidate = &ctx->cg->fun_sigs.data[i];
      if (!candidate->is_attached_method || !candidate->module_name ||
          !candidate->owner_type || !candidate->name)
        continue;
      const char *leaf = strrchr(candidate->name, '.');
      leaf = leaf ? leaf + 1 : candidate->name;
      const char *candidate_owner_leaf = strrchr(candidate->owner_type, '.');
      candidate_owner_leaf = candidate_owner_leaf ? candidate_owner_leaf + 1
                                                  : candidate->owner_type;
      bool owner_matches =
          (strlen(candidate->owner_type) == owner_len &&
           memcmp(candidate->owner_type, target_owner_name, owner_len) == 0) ||
          (strlen(candidate_owner_leaf) == owner_len &&
           memcmp(candidate_owner_leaf, target_owner_name, owner_len) == 0);
      if (!owner_matches || strcmp(leaf, method_dot + 1) != 0)
        continue;
      if (strlen(candidate->module_name) == module_len &&
          memcmp(candidate->module_name, target_module_name, module_len) == 0)
        return candidate;
      if (ny_codegen_module_matches_std(ctx->cg, candidate->module_name,
                                        target_module_name)) {
        if (module_alias_match)
          return NULL;
        module_alias_match = candidate;
      }
    }
    return module_alias_match;
  }
  if (!target_module || !*target_module || !target_owner || !*target_owner ||
      !target_method || !*target_method)
    return NULL;
  const char *target_owner_leaf = strrchr(target_owner, '.');
  target_owner_leaf = target_owner_leaf ? target_owner_leaf + 1 : target_owner;
  fun_sig *owner_method = NULL;
  for (size_t i = 0; i < ctx->cg->fun_sigs.len; ++i) {
    fun_sig *candidate = &ctx->cg->fun_sigs.data[i];
    if (!candidate->is_attached_method || !candidate->module_name ||
        !candidate->owner_type || !candidate->name)
      continue;
    const char *leaf = strrchr(candidate->name, '.');
    leaf = leaf ? leaf + 1 : candidate->name;
    const char *candidate_owner_leaf = strrchr(candidate->owner_type, '.');
    candidate_owner_leaf =
        candidate_owner_leaf ? candidate_owner_leaf + 1 : candidate->owner_type;
    if (strcmp(candidate_owner_leaf, target_owner_leaf) != 0 ||
        strcmp(leaf, target_method) != 0)
      continue;
    if (strcmp(candidate->module_name, target_module) == 0)
      return candidate;
    if (ny_codegen_module_matches_std(ctx->cg, candidate->module_name,
                                      target_module)) {
      if (owner_method)
        return NULL;
      owner_method = candidate;
    }
  }
  return owner_method;
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
      /*
       * A module can be reached through multiple import paths. Repeated
       * registrations of the same exported operator target are one candidate,
       * not an overload ambiguity; distinct targets remain ambiguous.
       */
      if (found && def->module_name && *def->module_name &&
          found->module_name &&
          strcmp(def->module_name, found->module_name) == 0 &&
          def->target_name && *def->target_name && found->target_name &&
          strcmp(def->target_name, found->target_name) == 0)
        continue;
      count++;
      if (!found)
        found = def;
      else
        break;
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
        strcmp(prev->method, name) == 0) {
      /*
       * The stdlib re-exports delegating impl methods across modules
       * (core.mod vs math/nt bigint.as_bytes, math/big vs math/nt
       * bigint.xor). Those overlaps are coherent when both declarations ship
       * with the stdlib. Ny does not resolve attached-method overloads by
       * arity yet, so a user declaration with the same owner/name must fail
       * even when its parameter count differs.
       */
      if (ny_is_stdlib_tok(prev->tok) && ny_is_stdlib_tok(method->tok))
        return;
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
    for (size_t i = 0; ctx->cg && i < s->as.impl.laws.len; ++i) {
      expr_t *law = s->as.impl.laws.data[i];
      if (!law)
        continue;
      /*
       * Each law is a versioned obligation run through the policy funnel;
       * the comptime fallback stays, and the diagnostic distinguishes a
       * refuted law (false) from an undecided one (unknown).
       */
      ny_proof_policy_result_t policy_result = {0};
      bool decided = ny_proof_policy_decide(ctx->cg, NULL, 0, law, "", false,
                                            &policy_result);
      bool proved = decided && policy_result.verdict == NY_PROOF_VERDICT_PROVED;
      if (!proved) {
        ny_ct_fast_val_t value = ny_ct_fast_none();
        if (ny_try_eval_comptime_expr_fast(ctx->cg, law, &value, 0) &&
            value.kind == NY_CT_FAST_BOOL && value.b) {
          if (policy_result.obligation[0] ||
              ny_proof_policy_obligation_build(
                  ctx->cg, NULL, 0, law, "", false, policy_result.obligation,
                  sizeof(policy_result.obligation)))
            proved = ny_proof_policy_mark_fold(&policy_result);
        }
      }
      if (!proved) {
        if (decided && policy_result.verdict == NY_PROOF_VERDICT_REFUTED) {
          tp_add_diag(
              ctx, law->tok, "trait", "impl-law-unproved",
              "law verification failed in 'impl %s for %s': law is "
              "false%s%s",
              s->as.impl.trait_name ? s->as.impl.trait_name : "<trait>",
              s->as.impl.type_name ? s->as.impl.type_name : "<type>",
              decided && policy_result.cex[0] ? " (counterexample " : "",
              decided && policy_result.cex[0] ? policy_result.cex : ")");
        } else {
          tp_add_diag(ctx, law->tok, "trait", "impl-law-unproved",
                      "law verification failed in 'impl %s for %s': law could "
                      "not be proved (%s)",
                      s->as.impl.trait_name ? s->as.impl.trait_name : "<trait>",
                      s->as.impl.type_name ? s->as.impl.type_name : "<type>",
                      decided && policy_result.explanation[0]
                          ? policy_result.explanation
                          : "policy could not construct a checked obligation");
        }
      }
    }
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
      /*
       * Publish the type stage's unique selection for backend lowering. The
       * AST walk remains a compatibility fallback for untyped entry paths,
       * but must not silently re-resolve a different overload.
       */
      if (def && matches == 1 && def->stmt)
        e->semantic.canonical_callee_stmt = def->stmt;
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
      if (pattern->tok.kind == NY_T_NIL)
        snprintf(buf, sizeof(buf), "nil");
      else
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
  if (!ctx->include_std) {
    if (ny_is_stdlib_tok(e->tok))
      return;
    if (ctx->source_name && e->tok.filename &&
        strcmp(ctx->source_name, e->tok.filename) != 0)
      return;
  }
  bool is_size = strcmp(name, "__layout_size") == 0;
  bool is_align = strcmp(name, "__layout_align") == 0;
  bool is_offset = strcmp(name, "__layout_offset") == 0;
  bool is_store = strcmp(name, "store_layout") == 0;
  bool is_field_store = strcmp(name, "store_layout_field") == 0;
  bool is_load = strcmp(name, "load_layout") == 0;
  if (!is_size && !is_align && !is_offset && !is_store && !is_field_store &&
      !is_load)
    return;
  size_t layout_arg = is_store || is_field_store || is_load ? 1u : 0u;
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
    if (ny_is_stdlib_tok(e->tok))
      return;
    tp_add_diag(ctx, e->tok, "abi", "layout-unknown", "unknown layout '%s'",
                layout_name);
    return;
  }
  if (is_offset || is_load || is_field_store) {
    size_t field_arg = is_load || is_field_store ? 2u : 1u;
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
  if (is_field_store && args->len != 4u) {
    tp_add_diag(
        ctx, e->tok, "abi", "layout-field-store-arity",
        "store_layout_field expects dst, layout name, field name, and value");
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
  if (!ctx->include_std) {
    if (ny_is_stdlib_tok(e->tok))
      return;
    if (ctx->source_name && e->tok.filename &&
        strcmp(ctx->source_name, e->tok.filename) != 0)
      return;
  }
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
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      stmt_t *child = s->as.block.body.data[i];
      if (tp_stmt_in_scope(ctx, child))
        tp_validate_abi_stmt(ctx, child);
    }
    break;
  case NY_S_FUNC:
    if (tp_stmt_in_scope(ctx, s))
      tp_validate_abi_stmt(ctx, s->as.fn.body);
    break;
  case NY_S_MODULE:
    if (tp_stmt_in_scope(ctx, s)) {
      for (size_t i = 0; i < s->as.module.body.len; ++i)
        tp_validate_abi_stmt(ctx, s->as.module.body.data[i]);
    }
    break;
  default:
    break;
  }
}

static void tp_validate_abi_program(ny_tp_ctx_t *ctx) {
  if (!ctx || !ctx->prog)
    return;
  for (size_t i = 0; i < ctx->prog->body.len; ++i) {
    stmt_t *s = ctx->prog->body.data[i];
    if (tp_stmt_in_scope(ctx, s))
      tp_validate_abi_stmt(ctx, s);
  }
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
  size_t acceptance_rate_percent =
      ctx.mono_candidates ? (accepted * 100u) / ctx.mono_candidates : 0;
  tp_append(&j,
            "],\"monomorphization\":{\"candidates\":%zu,\"accepted\":%zu,"
            "\"rejected\":%zu,\"acceptance_rate_percent\":%zu,"
            "\"candidate_decisions\":[",
            ctx.mono_candidates, accepted, rejected, acceptance_rate_percent);
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
    tp_json_str(&j,
                spec->return_kind == NY_MONO_TYPE_INT
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
      tp_json_str(&j,
                  spec->types[k] == NY_MONO_TYPE_INT
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
