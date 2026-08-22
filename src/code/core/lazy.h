static size_t ny_lazy_name_set_slot(uint64_t hash, size_t cap) {
  uint64_t mixed = hash ^ (hash >> 33);
  mixed *= UINT64_C(0xff51afd7ed558ccd);
  mixed ^= mixed >> 33;
  return (size_t)mixed & (cap - 1);
}

static bool ny_lazy_name_set_grow(ny_name_set *set, size_t min_cap) {
  if (!set)
    return false;
  size_t cap = set->cap ? set->cap : 32;
  while (cap < min_cap)
    cap <<= 1;
  ny_name_set_slot *slots = calloc(cap, sizeof(*slots));
  if (!slots)
    return false;
  if (set->slots) {
    for (size_t i = 0; i < set->cap; ++i) {
      ny_name_set_slot old = set->slots[i];
      if (!old.name)
        continue;
      size_t idx = ny_lazy_name_set_slot(old.hash, cap);
      while (slots[idx].name)
        idx = (idx + 1) & (cap - 1);
      slots[idx] = old;
    }
    free(set->slots);
  }
  set->slots = slots;
  set->cap = cap;
  return true;
}

static bool ny_lazy_name_set_contains_hash(const ny_name_set *set,
                                           const char *name, uint64_t hash) {
  if (!set || !set->slots || !set->cap || !name || !*name)
    return false;
  size_t name_len = strlen(name);
  size_t idx = ny_lazy_name_set_slot(hash, set->cap);
  for (size_t probe = 0; probe < set->cap; ++probe) {
    const ny_name_set_slot *slot = &set->slots[idx];
    if (!slot->name)
      return false;
    if (slot->hash == hash && slot->len == name_len &&
        (slot->name == name || memcmp(slot->name, name, name_len) == 0))
      return true;
    idx = (idx + 1) & (set->cap - 1);
  }
  return false;
}

static bool ny_lazy_name_set_insert_hash(ny_name_set *set, const char *name,
                                         uint64_t hash) {
  if (!set || !name || !*name)
    return false;
  size_t name_len = strlen(name);
  if (ny_lazy_name_set_contains_hash(set, name, hash))
    return false;
  if (!set->cap || ((set->len + 1) * 10) >= (set->cap * 7)) {
    size_t target = set->cap ? set->cap << 1 : 32;
    if (!ny_lazy_name_set_grow(set, target))
      return false;
  }
  size_t idx = ny_lazy_name_set_slot(hash, set->cap);
  while (set->slots[idx].name)
    idx = (idx + 1) & (set->cap - 1);
  set->slots[idx].name = name;
  set->slots[idx].hash = hash;
  set->slots[idx].len = name_len;
  set->len++;
  return true;
}

static void ny_lazy_name_set_free(ny_name_set *set) {
  if (!set)
    return;
  free(set->slots);
  set->slots = NULL;
  set->cap = 0;
  set->len = 0;
}

static bool ny_lazy_emit_name_list_contains(const assigned_name_list *names,
                                            const assigned_hash_list *hashes,
                                            const uint64_t bloom[4],
                                            const ny_name_set *set,
                                            const char *name, uint64_t hash) {
  if (!name || !*name)
    return false;
  if (!ny_name_bloom_maybe_has(bloom, hash))
    return false;
  if (set && set->slots)
    return ny_lazy_name_set_contains_hash(set, name, hash);
  return assigned_name_has(names, hashes, name, hash, bloom);
}

static void ny_lazy_emit_add_stable_name(codegen_t *cg,
                                         assigned_name_list *names,
                                         assigned_hash_list *hashes,
                                         uint64_t bloom[4], ny_name_set *set,
                                         const char *name) {
  if (!cg || !names || !hashes || !name || !*name)
    return;
  uint64_t hash = ny_hash64_cstr(name);
  if (ny_lazy_emit_name_list_contains(names, hashes, bloom, set, name, hash))
    return;
  const char *stable = arena_strndup(cg->arena, name, strlen(name));
  vec_push(names, stable);
  vec_push(hashes, hash);
  ny_name_bloom_add(bloom, hash);
  ny_lazy_name_set_insert_hash(set, stable, hash);
}

static void ny_lazy_emit_add_name(codegen_t *cg, const char *name) {
  ny_lazy_emit_add_stable_name(cg, &cg->lazy_emit_names, &cg->lazy_emit_hashes,
                               cg->lazy_emit_bloom, &cg->lazy_emit_name_set,
                               name);
}

static bool ny_lazy_emit_collected_function(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  uint64_t hash = ny_hash64_cstr(name);
  return ny_lazy_emit_name_list_contains(
      &cg->lazy_emit_collected_names, &cg->lazy_emit_collected_hashes,
      cg->lazy_emit_collected_bloom, &cg->lazy_emit_collected_set, name, hash);
}

static void ny_lazy_emit_mark_collected_function(codegen_t *cg,
                                                 const char *name) {
  if (!cg || !name || !*name)
    return;
  ny_lazy_emit_add_stable_name(cg, &cg->lazy_emit_collected_names,
                               &cg->lazy_emit_collected_hashes,
                               cg->lazy_emit_collected_bloom,
                               &cg->lazy_emit_collected_set, name);
}

static void ny_lazy_emit_add_resolved_name(codegen_t *cg, const char *name,
                                           int depth) {
  if (!cg || !name || !*name || depth > 8)
    return;
  ny_lazy_emit_add_name(cg, name);
  const char *resolved = resolve_import_alias(cg, name);
  if (resolved && *resolved && strcmp(resolved, name) != 0)
    ny_lazy_emit_add_resolved_name(cg, resolved, depth + 1);
}

static bool ny_lazy_emit_has_name(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  uint64_t hash = ny_hash64_cstr(name);
  return ny_lazy_emit_name_list_contains(&cg->lazy_emit_names,
                                         &cg->lazy_emit_hashes,
                                         cg->lazy_emit_bloom,
                                         &cg->lazy_emit_name_set, name, hash);
}

static bool ny_lazy_emit_has_name_or_qname(codegen_t *cg, const char *name,
                                           const char *cur_mod) {
  if (!cg || !name || !*name)
    return false;
  if (ny_lazy_emit_has_name(cg, name))
    return true;
  const char *leaf = ny_name_leaf(name);
  if (leaf && leaf != name && ny_lazy_emit_has_name(cg, leaf))
    return true;
  if (!cur_mod || !*cur_mod)
    return false;
  size_t mlen = strlen(cur_mod);
  if (strncmp(name, cur_mod, mlen) == 0 && name[mlen] == '.')
    return ny_lazy_emit_has_name(cg, name);
  char qname[1536];
  int n = snprintf(qname, sizeof(qname), "%s.%s", cur_mod, name);
  return n > 0 && (size_t)n < sizeof(qname) && ny_lazy_emit_has_name(cg, qname);
}

bool ny_lazy_emit_stdlib_var_needed(codegen_t *cg, stmt_t *s,
                                    const char *cur_mod) {
  if (!cg || !s || s->kind != NY_S_VAR)
    return true;
  if (!cg->lazy_emit_stdlib_enabled || !ny_is_stdlib_tok(s->tok) ||
      ny_codegen_stmt_is_source_file(cg, s) ||
      ny_codegen_module_is_source_file(cg, cur_mod))
    return true;
  if (cur_mod && strcmp(cur_mod, "std.core.syntax.type") == 0)
    return true;
  for (size_t i = 0; i < s->as.var.names.len; ++i) {
    const char *name = s->as.var.names.data[i];
    const char *tail = ny_name_leaf(name);
    if (tail && (strncmp(tail, "_TAG_", 5) == 0 ||
                 strncmp(tail, "_CORE_TAG_", 10) == 0))
      return true;
    if (ny_lazy_emit_has_name_or_qname(cg, name, cur_mod))
      return true;
  }
  return false;
}

static bool ny_lazy_emit_is_conservative_keep(const char *final_name) {
  if (!final_name || !*final_name)
    return true;
  const char *tail = ny_name_leaf(final_name);
  if (!tail || !*tail)
    return true;

  if (strncmp(final_name, "std.math.nt.__", 14) == 0 ||
      strncmp(final_name, "std.math.matrix.__", 18) == 0)
    return true;
  return false;
}

static void ny_lazy_emit_collect_stmt(codegen_t *cg, stmt_t *s);

typedef struct {
  codegen_t *cg;
  assigned_name_list locals;
  assigned_hash_list local_hashes;
  uint64_t local_bloom[4];
} ny_lazy_emit_collect_ctx_t;

static bool ny_lazy_emit_local_has(ny_lazy_emit_collect_ctx_t *ctx,
                                   const char *name) {
  if (!ctx || !name || !*name)
    return false;
  return assigned_name_contains(&ctx->locals, &ctx->local_hashes,
                                ctx->local_bloom, name);
}

static void ny_lazy_emit_local_add(ny_lazy_emit_collect_ctx_t *ctx,
                                   const char *name) {
  if (!ctx || !name || !*name)
    return;
  assigned_name_add(&ctx->locals, &ctx->local_hashes, ctx->local_bloom, name);
}

static const char *ny_lazy_emit_module_prefix(codegen_t *cg,
                                              const char *qname) {
  if (!cg || !qname || !*qname)
    return NULL;
  const char *dot = strrchr(qname, '.');
  if (!dot || dot == qname)
    return NULL;
  return arena_strndup(cg->arena, qname, (size_t)(dot - qname));
}

static bool ny_lazy_emit_expr_pre(ny_visitor_t *v, expr_t *e) {
  if (!v || !e)
    return true;
  ny_lazy_emit_collect_ctx_t *ctx = (ny_lazy_emit_collect_ctx_t *)v->ctx;
  codegen_t *cg = ctx ? ctx->cg : NULL;
  if (!cg)
    return true;
  switch (e->kind) {
  case NY_E_IDENT:
    if (ny_lazy_emit_local_has(ctx, e->as.ident.name))
      break;
    ny_lazy_emit_add_resolved_name(cg, e->as.ident.name, 0);
    if (cg->current_module_name && *cg->current_module_name &&
        e->as.ident.name && !strchr(e->as.ident.name, '.')) {
      ny_lazy_emit_add_name(
          cg, codegen_qname(cg, e->as.ident.name, cg->current_module_name));
    }
    break;
  case NY_E_MEMBER: {
    bool resolved_member = false;
    if (e->as.member.target && e->as.member.name) {
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, NULL, 0, e->as.member.target,
                                      module_path, sizeof(module_path))) {
        if (ny_resolve_module_function_path(cg, module_path, e->as.member.name,
                                            resolved_fun,
                                            sizeof(resolved_fun))) {
          ny_lazy_emit_add_resolved_name(cg, resolved_fun, 0);
          resolved_member = true;
        }
        char resolved_global[1280];
        int gw = snprintf(resolved_global, sizeof(resolved_global), "%s.%s",
                          module_path, e->as.member.name);
        if (gw > 0 && (size_t)gw < sizeof(resolved_global)) {
          ny_lazy_emit_add_resolved_name(cg, resolved_global, 0);
          resolved_member = true;
        }
      }
    }
    if (!resolved_member)
      ny_lazy_emit_add_resolved_name(cg, e->as.member.name, 0);
    if (e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name && e->as.member.name) {
      const char *target = e->as.member.target->as.ident.name;
      for (size_t i = 0; i < cg->aliases.len; ++i) {
        binding *al = &cg->aliases.data[i];
        if (!al->name || strcmp(al->name, target) != 0)
          continue;
        const char *module_name = (const char *)al->stmt_t;
        if (!module_name || !*module_name)
          continue;
        char dotted[512];
        int nw = snprintf(dotted, sizeof(dotted), "%s.%s", module_name,
                          e->as.member.name);
        if (nw > 0 && (size_t)nw < sizeof(dotted))
          ny_lazy_emit_add_resolved_name(cg, dotted, 0);
        break;
      }
    }
    break;
  }
  case NY_E_MEMCALL: {
    bool resolved_memcall = false;
    if (e->as.memcall.target && e->as.memcall.name) {
      char module_path[1024];
      char resolved_fun[1280];
      if (ny_resolve_module_expr_path(cg, NULL, 0, e->as.memcall.target,
                                      module_path, sizeof(module_path)) &&
          ny_resolve_module_function_path(cg, module_path, e->as.memcall.name,
                                          resolved_fun, sizeof(resolved_fun))) {
        ny_lazy_emit_add_resolved_name(cg, resolved_fun, 0);
        resolved_memcall = true;
      }
    }
    if (!resolved_memcall)
      ny_lazy_emit_add_resolved_name(cg, e->as.memcall.name, 0);
    break;
  }
  case NY_E_INFERRED_MEMBER:
    ny_lazy_emit_add_resolved_name(cg, e->as.inferred_member.name, 0);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    ny_lazy_emit_collect_stmt(cg, e->as.lambda.body);
    break;
  default:
    break;
  }
  return true;
}

static bool ny_lazy_emit_stmt_pre(ny_visitor_t *v, stmt_t *s) {
  if (!v || !s)
    return true;
  ny_lazy_emit_collect_ctx_t *ctx = (ny_lazy_emit_collect_ctx_t *)v->ctx;
  codegen_t *cg = ctx ? ctx->cg : NULL;
  if (!cg)
    return true;
  if (s->kind == NY_S_IF) {
    bool truthy = false;
    if (ny_eval_comptime_if(cg, s, &truthy)) {
      ny_visit_stmt(v, truthy ? s->as.iff.conseq : s->as.iff.alt);
      return false;
    }
  }
  if (s->kind == NY_S_FUNC) {
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      ny_lazy_emit_local_add(ctx, s->as.fn.params.data[i].name);
  } else if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      ny_lazy_emit_local_add(ctx, s->as.var.names.data[i]);
  } else if (s->kind == NY_S_FOR && s->as.fr.iter_var) {
    ny_lazy_emit_local_add(ctx, s->as.fr.iter_var);
    if (s->as.fr.iter_index_var)
      ny_lazy_emit_local_add(ctx, s->as.fr.iter_index_var);
  }
  if (s->kind == NY_S_GUARD && s->as.guard.type_name) {
    ny_lazy_emit_add_name(cg, s->as.guard.type_name);
  }
  return true;
}

static void ny_lazy_emit_collect_stmt(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  ny_lazy_emit_collect_ctx_t ctx = {0};
  ctx.cg = cg;
  vec_init(&ctx.locals);
  vec_init(&ctx.local_hashes);
  ny_visitor_t vis = {0};
  vis.ctx = &ctx;
  vis.visit_expr_pre = ny_lazy_emit_expr_pre;
  vis.visit_stmt_pre = ny_lazy_emit_stmt_pre;
  ny_visit_stmt(&vis, s);
  vec_free(&ctx.locals);
  vec_free(&ctx.local_hashes);
}

static void ny_lazy_emit_collect_stmt_in_module(codegen_t *cg, stmt_t *s,
                                                const char *cur_mod) {
  if (!cg || !s)
    return;
  const char *saved_mod = cg->current_module_name;
  cg->current_module_name = cur_mod;
  ny_lazy_emit_collect_stmt(cg, s);
  cg->current_module_name = saved_mod;
}

static bool ny_lazy_emit_collect_reached_var_deps_stmt(codegen_t *cg, stmt_t *s,
                                                       const char *cur_mod) {
  if (!cg || !s)
    return false;
  bool changed = false;
  switch (s->kind) {
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (ny_lazy_emit_collect_reached_var_deps_stmt(
              cg, s->as.module.body.data[i], s->as.module.name))
        changed = true;
    }
    return changed;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (ny_lazy_emit_collect_reached_var_deps_stmt(
              cg, s->as.block.body.data[i], cur_mod))
        changed = true;
    }
    return changed;
  case NY_S_VAR:
    if (!ny_is_stdlib_tok(s->tok) || ny_codegen_stmt_is_source_file(cg, s) ||
        !ny_lazy_emit_stdlib_var_needed(cg, s, cur_mod))
      return false;
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name)
        continue;
      const char *qname = codegen_qname(cg, name, cur_mod);
      if (ny_lazy_emit_collected_function(cg, qname))
        continue;
      ny_lazy_emit_mark_collected_function(cg, qname);
      size_t before = cg->lazy_emit_names.len;
      ny_lazy_emit_collect_stmt_in_module(cg, s, cur_mod);
      if (ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_VARS")) {
        fprintf(stderr, "[lazy-stdlib-codegen] var_collect %s +%zu\n", qname,
                cg->lazy_emit_names.len - before);
      }
      if (cg->lazy_emit_names.len != before)
        changed = true;
      break;
    }
    return changed;
  default:
    return false;
  }
}

static bool ny_lazy_emit_collect_reached_var_deps(codegen_t *cg) {
  if (!cg || !cg->prog)
    return false;
  bool changed = false;
  for (size_t i = 0; i < cg->prog->body.len; ++i) {
    if (ny_lazy_emit_collect_reached_var_deps_stmt(cg, cg->prog->body.data[i],
                                                  NULL))
      changed = true;
  }
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; ++i) {
      if (ny_lazy_emit_collect_reached_var_deps_stmt(cg, prog->body.data[i],
                                                    NULL))
        changed = true;
    }
  }
  return changed;
}

static void ny_lazy_emit_add_function_name(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return;
  ny_lazy_emit_add_name(cg, name);
}

static bool ny_lazy_emit_function_reached(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  return ny_lazy_emit_has_name(cg, name);
}

bool ny_codegen_token_is_source_file(codegen_t *cg, token_t tok) {
  if (!cg || !cg->source_main_file || !*cg->source_main_file || !tok.filename ||
      !*tok.filename)
    return false;
  if (strcmp(cg->source_main_file, tok.filename) == 0)
    return true;
  size_t want_len = strlen(cg->source_main_file);
  size_t got_len = strlen(tok.filename);
  if (want_len < got_len &&
      strcmp(tok.filename + got_len - want_len, cg->source_main_file) == 0) {
    char sep = tok.filename[got_len - want_len - 1];
    if (sep == '/' || sep == '\\')
      return true;
  }
  if (got_len < want_len &&
      strcmp(cg->source_main_file + want_len - got_len, tok.filename) == 0) {
    char sep = cg->source_main_file[want_len - got_len - 1];
    if (sep == '/' || sep == '\\')
      return true;
  }
  return false;
}

bool ny_codegen_stmt_is_source_file(codegen_t *cg, stmt_t *s) {
  return s && ny_codegen_token_is_source_file(cg, s->tok);
}

bool ny_codegen_module_is_source_file(codegen_t *cg, const char *module_name) {
  if (!cg || !cg->source_main_file || !*cg->source_main_file ||
      !module_name || !*module_name)
    return false;
  if (strncmp(module_name, "std.", 4) != 0)
    return false;

  char rel[1024];
  size_t pos = 0;
  const char *p = module_name + 4;
  memcpy(rel, "lib/", 4);
  pos = 4;
  while (*p && pos + 4 < sizeof(rel)) {
    rel[pos++] = *p == '.' ? '/' : *p;
    p++;
  }
  if (*p || pos + 3 >= sizeof(rel))
    return false;
  memcpy(rel + pos, ".ny", 4);
  pos += 3;
  rel[pos] = '\0';

  size_t want_len = strlen(cg->source_main_file);
  size_t rel_len = strlen(rel);
  if (want_len == rel_len && strcmp(cg->source_main_file, rel) == 0)
    return true;
  if (want_len > rel_len &&
      strcmp(cg->source_main_file + want_len - rel_len, rel) == 0) {
    char sep = cg->source_main_file[want_len - rel_len - 1];
    return sep == '/' || sep == '\\';
  }
  return false;
}

bool ny_stmt_tree_has_source_file(codegen_t *cg, stmt_t *s) {
  if (!s)
    return false;
  if (ny_codegen_stmt_is_source_file(cg, s))
    return true;
  switch (s->kind) {
  case NY_S_LEMMA:
    return true;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.module.body.data[i]))
        return true;
    }
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.block.body.data[i]))
        return true;
    }
    return false;
  case NY_S_IF:
    return ny_stmt_tree_has_source_file(cg, s->as.iff.conseq) ||
           ny_stmt_tree_has_source_file(cg, s->as.iff.alt);
  case NY_S_GUARD:
    return ny_stmt_tree_has_source_file(cg, s->as.guard.fallback);
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.impl.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.struc.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; i++) {
      if (ny_stmt_tree_has_source_file(cg, s->as.layout.methods.data[i]))
        return true;
    }
    return false;
  default:
    return false;
  }
}

static bool ny_stmt_tree_is_source_context(codegen_t *cg, stmt_t *s) {
  if (!s)
    return false;
  if (ny_stmt_tree_has_source_file(cg, s))
    return true;
  return s->kind == NY_S_MODULE &&
         ny_codegen_module_is_source_file(cg, s->as.module.name);
}

static bool ny_stmt_tree_has_zero_arg_call_named(const stmt_t *s,
                                                 const char *name);

static bool ny_expr_is_zero_arg_ident_call(const expr_t *e, const char *name) {
  return e && e->kind == NY_E_CALL &&
         ny_expr_ident_is_name(e->as.call.callee, name) &&
         e->as.call.args.len == 0;
}

static bool ny_expr_tree_has_zero_arg_call_named(const expr_t *e,
                                                 const char *name) {
  if (!e || !name || !*name)
    return false;
  if (ny_expr_is_zero_arg_ident_call(e, name))
    return true;
  switch (e->kind) {
  case NY_E_UNARY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.unary.right, name);
  case NY_E_BINARY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.binary.left, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.binary.right, name);
  case NY_E_LOGICAL:
    return ny_expr_tree_has_zero_arg_call_named(e->as.logical.left, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.logical.right, name);
  case NY_E_TERNARY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.ternary.cond, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.ternary.true_expr, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.ternary.false_expr, name);
  case NY_E_CALL:
    if (ny_expr_tree_has_zero_arg_call_named(e->as.call.callee, name))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(e->as.call.args.data[i].val,
                                               name))
        return true;
    }
    return false;
  case NY_E_MEMCALL:
    if (ny_expr_tree_has_zero_arg_call_named(e->as.memcall.target, name))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(e->as.memcall.args.data[i].val,
                                               name))
        return true;
    }
    return false;
  case NY_E_INDEX:
    return ny_expr_tree_has_zero_arg_call_named(e->as.index.target, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.index.start, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.index.stop, name) ||
           ny_expr_tree_has_zero_arg_call_named(e->as.index.step, name);
  case NY_E_LAMBDA:
  case NY_E_FN:
    return false;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(e->as.list_like.data[i], name))
        return true;
    }
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      dict_pair_t pair = e->as.dict.pairs.data[i];
      if (ny_expr_tree_has_zero_arg_call_named(pair.key, name) ||
          ny_expr_tree_has_zero_arg_call_named(pair.value, name))
        return true;
    }
    return false;
  case NY_E_COMPTIME:
    return ny_stmt_tree_has_zero_arg_call_named(e->as.comptime_expr.body,
                                                name);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t part = e->as.fstring.parts.data[i];
      if (part.kind == NY_FSP_EXPR &&
          ny_expr_tree_has_zero_arg_call_named(part.as.e, name))
        return true;
    }
    return false;
  case NY_E_MATCH:
    return ny_expr_tree_has_zero_arg_call_named(e->as.match.test, name);
  case NY_E_MEMBER:
    return ny_expr_tree_has_zero_arg_call_named(e->as.member.target, name);
  case NY_E_PTR_TYPE:
    return ny_expr_tree_has_zero_arg_call_named(e->as.ptr_type.target, name);
  case NY_E_DEREF:
    return ny_expr_tree_has_zero_arg_call_named(e->as.deref.target, name);
  case NY_E_SIZEOF:
    return ny_expr_tree_has_zero_arg_call_named(e->as.szof.target, name);
  case NY_E_TRY:
    return ny_expr_tree_has_zero_arg_call_named(e->as.try_expr.target, name);
  default:
    return false;
  }
}

static bool ny_stmt_tree_has_zero_arg_call_named(const stmt_t *s,
                                                 const char *name) {
  if (!s || !name || !*name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_tree_has_zero_arg_call_named(s->as.block.body.data[i], name))
        return true;
    }
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      if (ny_expr_tree_has_zero_arg_call_named(s->as.var.exprs.data[i], name))
        return true;
    }
    return false;
  case NY_S_EXPR:
    return ny_expr_tree_has_zero_arg_call_named(s->as.expr.expr, name);
  case NY_S_IF:
    return ny_expr_tree_has_zero_arg_call_named(s->as.iff.test, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.iff.init, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.iff.conseq, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.iff.alt, name);
  case NY_S_GUARD:
    return ny_expr_tree_has_zero_arg_call_named(s->as.guard.value, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.guard.fallback, name);
  case NY_S_WHILE:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.whl.init, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.whl.test, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.whl.invariant, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.whl.update, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.whl.body, name);
  case NY_S_FOR:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.fr.init, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.fr.cond, name) ||
           ny_expr_tree_has_zero_arg_call_named(s->as.fr.iterable, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.fr.update, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.fr.body, name);
  case NY_S_TRY:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.tr.body, name) ||
           ny_stmt_tree_has_zero_arg_call_named(s->as.tr.handler, name);
  case NY_S_RETURN:
    return ny_expr_tree_has_zero_arg_call_named(s->as.ret.value, name);
  case NY_S_DEFER:
    return ny_stmt_tree_has_zero_arg_call_named(s->as.de.body, name);
  case NY_S_LEMMA:
    return ny_expr_tree_has_zero_arg_call_named(s->as.lemma.proposition, name);
  case NY_S_MATCH:
    return ny_expr_tree_has_zero_arg_call_named(s->as.match.test, name);
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      if (ny_stmt_tree_has_zero_arg_call_named(s->as.module.body.data[i], name))
        return true;
    }
    return false;
  default:
    return false;
  }
}

static bool ny_stmt_has_main_guard(const stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_IF &&
      ny_expr_tree_has_zero_arg_call_named(s->as.iff.test, "__main"))
    return true;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_has_main_guard(s->as.block.body.data[i]))
        return true;
    }
  }
  return false;
}

bool ny_program_has_explicit_main_entry(codegen_t *cg, program_t *prog) {
  if (!prog)
    return false;
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *s = prog->body.data[i];
    if (!s)
      continue;
    if (cg && cg->source_main_file && *cg->source_main_file) {
      if (!ny_stmt_tree_is_source_context(cg, s))
        continue;
    } else if (ny_is_stdlib_tok(s->tok)) {
      continue;
    }
    if (s->kind == NY_S_EXPR &&
        ny_expr_is_zero_arg_ident_call(s->as.expr.expr, "main"))
      return true;
    if (ny_stmt_has_main_guard(s))
      return true;
  }
  return false;
}

static bool ny_lazy_emit_treat_as_root(codegen_t *cg, stmt_t *s,
                                       const char *cur_mod) {
  return s &&
         (!ny_is_stdlib_tok(s->tok) || ny_codegen_stmt_is_source_file(cg, s) ||
          ny_codegen_module_is_source_file(cg, cur_mod));
}

static void ny_lazy_emit_seed_stmt(codegen_t *cg, stmt_t *s,
                                   const char *cur_mod) {
  if (!cg || !s)
    return;
  switch (s->kind) {
  case NY_S_FUNC: {
    const char *final_name = codegen_qname(cg, s->as.fn.name, cur_mod);
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod)) {
      ny_lazy_emit_add_function_name(cg, final_name);
      if (!ny_lazy_emit_collected_function(cg, final_name)) {
        ny_lazy_emit_mark_collected_function(cg, final_name);
        ny_lazy_emit_collect_stmt_in_module(cg, s, cur_mod);
      }
    }
    return;
  }
  case NY_S_LEMMA: {
    const char *final_name = codegen_qname(cg, s->as.lemma.name, cur_mod);
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod)) {
      ny_lazy_emit_add_function_name(cg, final_name);
      if (!ny_lazy_emit_collected_function(cg, final_name)) {
        ny_lazy_emit_mark_collected_function(cg, final_name);
        ny_lazy_emit_collect_stmt_in_module(cg, s, cur_mod);
      }
    }
    return;
  }
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.impl.methods.data[i], cur_mod);
    return;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.struc.methods.data[i], cur_mod);
    return;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.layout.methods.data[i], cur_mod);
    return;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.module.body.data[i], s->as.module.name);
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      ny_lazy_emit_seed_stmt(cg, s->as.block.body.data[i], cur_mod);
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod))
      ny_lazy_emit_collect_stmt(cg, s);
    return;
  default:
    if (ny_lazy_emit_treat_as_root(cg, s, cur_mod))
      ny_lazy_emit_collect_stmt(cg, s);
    return;
  }
}

static void ny_lazy_emit_build_reachable_set(codegen_t *cg) {
  if (!cg || !cg->prog)
    return;
  if (!cg->comptime && (cg->is_repl || cg->emit_module_decls_only))
    return;
  if (cg->skip_stdlib && !cg->emit_cached_stdlib_init)
    return;
  if (!ny_env_enabled_default_on("NYTRIX_LAZY_STDLIB_CODEGEN") &&
      !ny_env_enabled("NYTRIX_UNSAFE_LAZY_STDLIB_CODEGEN"))
    return;
  if (!cg->comptime && (!cg->source_main_file || !*cg->source_main_file))
    return;
  bool trace_lazy = ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_CODEGEN");
  ny_tick_t t_lazy = trace_lazy ? ny_ticks_now() : 0;
  cg->lazy_emit_stdlib_enabled = true;
  if (!cg->lazy_emit_stdlib_enabled)
    return;
  if (trace_lazy) {
    fprintf(stderr,
            "[lazy-stdlib-codegen] begin body=%zu extra=%zu fun_sigs=%zu\n",
            cg->prog->body.len, cg->extra_progs.len, cg->fun_sigs.len);
  }
  ny_lazy_emit_add_name(cg, "main");
  ny_lazy_emit_add_name(cg, "_ny_top_entry");
  for (size_t i = 0; i < cg->prog->body.len; i++)
    ny_lazy_emit_seed_stmt(cg, cg->prog->body.data[i], NULL);
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; i++)
      ny_lazy_emit_seed_stmt(cg, prog->body.data[i], NULL);
  }
  if (trace_lazy) {
    fprintf(stderr,
            "[lazy-stdlib-codegen] seed names=%zu collected=%zu fun_sigs=%zu "
            "elapsed=%.4fs\n",
            cg->lazy_emit_names.len, cg->lazy_emit_collected_names.len,
            cg->fun_sigs.len, ny_ticks_elapsed_sec(t_lazy));
  }

  bool changed = true;
  size_t guard = 0;
  while (changed && guard++ < 64) {
    changed = false;
    size_t before = cg->lazy_emit_names.len;
    size_t collected_before = cg->lazy_emit_collected_names.len;
    ny_tick_t t_round = trace_lazy ? ny_ticks_now() : 0;
    for (size_t i = 0; i < cg->fun_sigs.len; i++) {
      fun_sig *sig = &cg->fun_sigs.data[i];
      if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC || !sig->name)
        continue;
      if (!ny_lazy_emit_function_reached(cg, sig->name))
        continue;
      if (!ny_lazy_emit_collected_function(cg, sig->name)) {
        const char *sig_mod = sig->module_name && *sig->module_name
                                  ? sig->module_name
                                  : ny_lazy_emit_module_prefix(cg, sig->name);
        ny_lazy_emit_mark_collected_function(cg, sig->name);
        ny_lazy_emit_collect_stmt_in_module(cg, sig->stmt_t, sig_mod);
      }
      ny_lazy_emit_add_function_name(cg, sig->name);
    }
    if (ny_lazy_emit_collect_reached_var_deps(cg))
      changed = true;
    changed = changed || cg->lazy_emit_names.len != before;
    if (trace_lazy) {
      fprintf(stderr,
              "[lazy-stdlib-codegen] round=%zu names=%zu +%zu collected=%zu "
              "+%zu elapsed=%.4fs\n",
              guard, cg->lazy_emit_names.len, cg->lazy_emit_names.len - before,
              cg->lazy_emit_collected_names.len,
              cg->lazy_emit_collected_names.len - collected_before,
              ny_ticks_elapsed_sec(t_round));
    }
  }
  if (trace_lazy) {
    fprintf(
        stderr,
        "[lazy-stdlib-codegen] reachable_names=%zu collected=%zu total=%.4fs\n",
        cg->lazy_emit_names.len, cg->lazy_emit_collected_names.len,
        ny_ticks_elapsed_sec(t_lazy));
  }
}

void ny_lazy_emit_prepare_reachable(codegen_t *cg) {
  ny_lazy_emit_build_reachable_set(cg);
}

static bool ny_lazy_emit_should_emit_func(codegen_t *cg, stmt_t *s,
                                          const char *final_name) {
  if (!cg || !cg->lazy_emit_stdlib_enabled || !s || !ny_is_stdlib_tok(s->tok))
    return true;
  if (ny_codegen_stmt_is_source_file(cg, s))
    return true;
  if (ny_lazy_emit_is_conservative_keep(final_name))
    return true;
  return ny_lazy_emit_function_reached(cg, final_name);
}

static void emit_top_functions(codegen_t *cg, stmt_t *s, scope *gsc, size_t gd,
                        const char *cur_mod) {
  if (s->kind == NY_S_FUNC) {
    if (!ny_emit_module_match(cg, cur_mod))
      return;
    cg->current_module_name = cur_mod;
    const char *final_name = codegen_qname(cg, s->as.fn.name, cur_mod);
    if (!ny_lazy_emit_should_emit_func(cg, s, final_name))
      return;
    if (cg->lazy_emit_stdlib_enabled &&
        ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_EMIT") &&
        ny_is_stdlib_tok(s->tok)) {
      fprintf(stderr, "[lazy-stdlib-codegen] emit_top %s\n",
              final_name ? final_name : "(nil)");
    }
    gen_func(cg, s, final_name, gsc, gd, NULL);
  } else if (s->kind == NY_S_LEMMA) {
    /*
     * Lemmas are erased at runtime, no runtime codegen needed.
     */
  } else if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      emit_top_functions(cg, s->as.impl.methods.data[i], gsc, gd, cur_mod);
  } else if (s->kind == NY_S_STRUCT) {
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      emit_top_functions(cg, s->as.struc.methods.data[i], gsc, gd, cur_mod);
  } else if (s->kind == NY_S_LAYOUT) {
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      emit_top_functions(cg, s->as.layout.methods.data[i], gsc, gd, cur_mod);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      emit_top_functions(cg, s->as.module.body.data[i], gsc, gd,
                         s->as.module.name);
  } else if (s->kind == NY_S_BLOCK && s->as.block.transparent) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      emit_top_functions(cg, s->as.block.body.data[i], gsc, gd, cur_mod);
  }
}

static bool ny_fn_has_body(LLVMValueRef fn) {
  return fn && LLVMGetFirstBasicBlock(fn) != NULL;
}

static bool ny_emit_referenced_function_declarations(codegen_t *cg, scope *gsc,
                                                     size_t gd) {
  if (!cg || !cg->lazy_emit_stdlib_enabled)
    return false;
  bool emitted = false;
  bool trace_uses = ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_USES");
  size_t trace_limit = 80;
  const char *trace_limit_env = getenv("NYTRIX_TRACE_LAZY_STDLIB_USE_LIMIT");
  if (trace_limit_env && *trace_limit_env) {
    long parsed = strtol(trace_limit_env, NULL, 10);
    if (parsed > 0)
      trace_limit = (size_t)parsed;
  }
  static size_t trace_use_count = 0;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig || sig->is_extern || !sig->stmt_t ||
        sig->stmt_t->kind != NY_S_FUNC || !sig->value)
      continue;
    if (ny_fn_has_body(sig->value))
      continue;
    if (!LLVMGetFirstUse(sig->value))
      continue;
    if (trace_uses && trace_use_count < trace_limit) {
      LLVMUseRef use = LLVMGetFirstUse(sig->value);
      LLVMValueRef user = use ? LLVMGetUser(use) : NULL;
      char *printed = user ? LLVMPrintValueToString(user) : NULL;
      fprintf(stderr,
              "[lazy-stdlib-codegen] demand_use %s llvm=%s user=%s\n",
              sig->name ? sig->name : "(nil)",
              sig->value ? LLVMGetValueName(sig->value) : "(nil)",
              printed ? printed : "(nil)");
      if (printed)
        LLVMDisposeMessage(printed);
      trace_use_count++;
    }
    const char *saved_mod = cg->current_module_name;
    const char *sig_mod = sig->module_name && *sig->module_name
                              ? sig->module_name
                              : ny_lazy_emit_module_prefix(cg, sig->name);
    if (!ny_lazy_emit_collected_function(cg, sig->name)) {
      ny_lazy_emit_mark_collected_function(cg, sig->name);
      ny_lazy_emit_collect_stmt_in_module(cg, sig->stmt_t, sig_mod);
      ny_lazy_emit_add_function_name(cg, sig->name);
      ny_lazy_emit_collect_reached_var_deps(cg);
      if (ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_CODEGEN")) {
        fprintf(stderr, "[lazy-stdlib-codegen] demand_collect %s\n",
                sig->name);
      }
    }
    cg->current_module_name = sig_mod;
    gen_func(cg, sig->stmt_t, sig->name, gsc, gd, NULL);
    cg->current_module_name = saved_mod;
    emitted = true;
  }
  return emitted;
}

void ny_lazy_emit_demand_referenced(codegen_t *cg, scope *gsc, size_t gd,
                                    const char *phase) {
  if (!cg || !cg->lazy_emit_stdlib_enabled)
    return;
  LLVMBasicBlockRef saved_block = ny_cur_block(cg);
  size_t rounds = 0;
  while (rounds++ < 64 &&
         ny_emit_referenced_function_declarations(cg, gsc, gd)) {
  }
  if (saved_block)
    ny_pos(cg, saved_block);
  if (ny_env_enabled("NYTRIX_TRACE_LAZY_STDLIB_CODEGEN")) {
    fprintf(stderr, "[lazy-stdlib-codegen] %s_demand_rounds=%zu\n",
            phase && *phase ? phase : "unknown", rounds > 0 ? rounds - 1 : 0);
  }
}

static bool ny_stmt_contains_top_function(stmt_t *s) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_FUNC:
    return true;
  case NY_S_LEMMA:
    return true;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++) {
      if (ny_stmt_contains_top_function(s->as.impl.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; i++) {
      if (ny_stmt_contains_top_function(s->as.struc.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; i++) {
      if (ny_stmt_contains_top_function(s->as.layout.methods.data[i]))
        return true;
    }
    return false;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      if (ny_stmt_contains_top_function(s->as.module.body.data[i]))
        return true;
    }
    return false;
  case NY_S_BLOCK:
    if (!s->as.block.transparent)
      return false;
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_contains_top_function(s->as.block.body.data[i]))
        return true;
    }
    return false;
  default:
    return false;
  }
}
