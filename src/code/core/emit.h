void codegen_prepare(codegen_t *cg) {
  if (!cg || !cg->prog || cg->is_preparing)
    return;

  cg->is_preparing = true;
  NY_COMPILER_ASSERT(cg->module != NULL, "codegen_prepare missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL,
                     "codegen_prepare missing LLVM builder");
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_prepare program body vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->prog->body.data != NULL || cg->prog->body.len == 0,
                     "codegen_prepare program body vector has len but no data");
  NY_COMPILER_ASSERT(cg->extra_progs.len <= cg->extra_progs.cap,
                     "codegen_prepare extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
                     "codegen_prepare extra_progs vector has len but no data");

  if (cg->debug_symbols) {
    stmt_t *first_stmt =
        (cg->prog->body.len > 0) ? cg->prog->body.data[0] : NULL;
    NY_COMPILER_ASSERT(first_stmt != NULL || cg->prog->body.len == 0,
                       "codegen_prepare first top-level statement is null");
    const char *main_file =
        (cg->debug_main_file && *cg->debug_main_file)
            ? cg->debug_main_file
            : (first_stmt ? first_stmt->tok.filename : "<inline>");

    bool inline_source = !main_file || !*main_file || main_file[0] == '<' ||
                         strcmp(main_file, "-") == 0;
    if (inline_source && cg->user_source && cg->user_source_len > 0) {
      char inline_file[PATH_MAX];
      char inline_name[64];
      snprintf(inline_name, sizeof(inline_name), "ny_inline_%ld.ny",
               (long)getpid());
      ny_join_path(inline_file, sizeof(inline_file), ny_get_temp_dir(),
                   inline_name);
      FILE *f = fopen(inline_file, "w");
      if (f) {
        fwrite(cg->user_source, 1, cg->user_source_len, f);
        fclose(f);

        static char abs_inline[4096];
        if (ny_realpath(inline_file, abs_inline)) {
          main_file = abs_inline;
        } else {
          main_file = inline_file;
        }

        size_t start = cg->prog->body.len > 20 ? cg->prog->body.len - 20 : 0;
        for (size_t i = start; i < cg->prog->body.len; i++) {
          stmt_t *s = cg->prog->body.data[i];
          NY_COMPILER_ASSERTF(s != NULL,
                              "null debug inline-source stmt at index %zu", i);
          if (!s)
            continue;
          const char *fn = s->tok.filename;
          if (fn) {
            s->tok.filename = main_file;
          }
        }
      }
    }
    codegen_debug_init(cg, main_file);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null top-level statement at index %zu", i);
    if (!s)
      continue;
    collect_use_aliases(cg, s);
    collect_use_modules(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null impl registration stmt at index %zu",
                        i);
    if (!s)
      continue;
    ny_register_impl_types_stmt(cg, s);
  }

  {
    struct timespec ts;
    int64_t deadline = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
      deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + 15000000000LL;
    ny_ffi_set_global_deadline(deadline);
  }
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null signature collection stmt at index %zu", i);
    if (!s)
      continue;
    collect_sigs(cg, s);
  }
  ny_ffi_set_global_deadline(0);

  process_default_core_imports(cg);

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null import processing stmt at index %zu",
                        i);
    if (!s)
      continue;
    process_use_imports(cg, s);
  }

  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null export processing stmt at index %zu",
                        i);
    if (!s)
      continue;
    process_exports(cg, s);
  }

  ny_build_link_allowed_modules(cg);
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL, "null link processing stmt at index %zu", i);
    if (!s)
      continue;
    process_links(cg, s, NULL);
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    NY_COMPILER_ASSERTF(prog->body.len <= prog->body.cap,
                        "extra program %zu body vector len exceeds cap", p);
    NY_COMPILER_ASSERTF(prog->body.data != NULL || prog->body.len == 0,
                        "extra program %zu body vector has len but no data", p);
    for (size_t i = 0; i < prog->body.len; i++) {
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null extra-program link stmt p=%zu index=%zu", p, i);
      if (!s)
        continue;
      process_links(cg, s, NULL);
    }
  }

  infer_pure_functions(cg);

  cg->is_preparing = false;
}

void codegen_repopulate_interns(codegen_t *cg) {
  if (!cg || !cg->module)
    return;

  for (LLVMValueRef g = LLVMGetFirstGlobal(cg->module); g;
       g = LLVMGetNextGlobal(g)) {
    const char *name = LLVMGetValueName(g);
    if (!name)
      continue;

    if (strncmp(name, ".str.runtime.", 13) == 0) {
      const char *suffix = name + 13;
      char data_name[256];
      snprintf(data_name, sizeof(data_name), ".str.data.%s", suffix);
      LLVMValueRef dg = ny_get_global(cg, data_name);
      if (!dg) {
        const char *dot = strchr(suffix, '.');
        if (dot && dot > suffix) {
          size_t base_len = (size_t)(dot - suffix);
          if (base_len < sizeof(data_name) - sizeof(".str.data.")) {
            snprintf(data_name, sizeof(data_name), ".str.data.%.*s",
                     (int)base_len, suffix);
            dg = ny_get_global(cg, data_name);
          }
        }
      }
      if (dg) {
        bool exists = false;
        for (size_t i = 0; i < cg->interns.len; i++) {
          string_intern *old = &cg->interns.data[i];
          if (old->val == g) {
            exists = true;
            break;
          }
        }
        if (exists)
          continue;
        string_intern in = {0};
        in.gv = dg;
        in.val = g;
        in.module = cg->module;
        vec_push(&cg->interns, in);
      }
    }
  }
}

void codegen_rebind_llvm_symbols(codegen_t *cg) {
  if (!cg || !cg->module)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    const char *llvm_name =
        (sig->llvm_name && *sig->llvm_name) ? sig->llvm_name :
        ((sig->link_name && *sig->link_name) ? sig->link_name : sig->name);
    if (!llvm_name || !*llvm_name)
      continue;
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, llvm_name);
    if (!fn)
      continue;
    sig->value = fn;
    sig->type = LLVMGlobalGetValueType(fn);
  }
  for (size_t i = 0; i < cg->global_vars.len; i++) {
    binding *b = &cg->global_vars.data[i];
    if (!b->name || !*b->name)
      continue;
    LLVMValueRef gv = LLVMGetNamedGlobal(cg->module, b->name);
    if (gv)
      b->value = gv;
  }
}

void codegen_export_extern_link_names(codegen_t *cg) {
  /*
   * Extern declarations declared with `as "cname"` carry their linker symbol
   * in link_name, but the LLVM value is emitted under the Nytrix name (for
   * example `std.os.disasm._cs_version`).  Calls already reference the value
   * handle, so renaming the value right before object emission makes the
   * emitted relocation match the C symbol that the linked library exports.
   * Skip a rename when the linker name is already claimed by a different
   * value; the old (broken) mapping is kept instead of inventing a new one.
   */
  if (!cg || !cg->module)
    return;
  for (size_t i = 0; i < cg->fun_sigs.len; i++) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->is_extern || !sig->link_name || !*sig->link_name)
      continue;
    const char *llvm_name =
        (sig->llvm_name && *sig->llvm_name) ? sig->llvm_name : sig->name;
    if (!llvm_name || !*llvm_name || strcmp(llvm_name, sig->link_name) == 0)
      continue;
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, llvm_name);
    if (!fn)
      continue;
    LLVMValueRef clash = LLVMGetNamedFunction(cg->module, sig->link_name);
    if (clash && clash != fn)
      continue;
    LLVMSetValueName2(fn, sig->link_name, strlen(sig->link_name));
  }
}

static LLVMValueRef ny_const_string_runtime_initializer(
    codegen_t *cg, LLVMValueRef str_array_global,
    LLVMValueRef runtime_ptr_global, LLVMTypeRef i8_ty) {
  if (!cg || !str_array_global || !runtime_ptr_global)
    return NULL;
  LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, false)};
  LLVMValueRef str_data_ptr =
      LLVMConstInBoundsGEP2(i8_ty, str_array_global, indices, 1);
  LLVMTypeRef value_ty = LLVMGlobalGetValueType(runtime_ptr_global);
  LLVMTypeKind value_kind = LLVMGetTypeKind(value_ty);
  if (value_kind == LLVMPointerTypeKind)
    return LLVMConstPointerCast(str_data_ptr, value_ty);
  if (value_kind == LLVMIntegerTypeKind)
    return LLVMConstPtrToInt(str_data_ptr, value_ty);
  return NULL;
}

static bool ny_set_const_string_runtime_initializer(
    codegen_t *cg, LLVMValueRef str_array_global,
    LLVMValueRef runtime_ptr_global, LLVMTypeRef i8_ty) {
  LLVMValueRef init = ny_const_string_runtime_initializer(
      cg, str_array_global, runtime_ptr_global, i8_ty);
  if (!init)
    return false;
  LLVMSetInitializer(runtime_ptr_global, init);
  return true;
}

void codegen_emit(codegen_t *cg) {
  NY_COMPILER_ASSERT(cg != NULL, "codegen_emit missing codegen");
  if (!cg)
    return;
  NY_COMPILER_ASSERT(cg->prog != NULL, "codegen_emit missing program");
  NY_COMPILER_ASSERT(cg->module != NULL, "codegen_emit missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL, "codegen_emit missing LLVM builder");
  if (!cg->prog)
    return;
  cg->builtin_shadow_cache_stable_len = cg->fun_sigs.len;
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_emit program body vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->prog->body.data != NULL || cg->prog->body.len == 0,
                     "codegen_emit program body vector has len but no data");
  NY_COMPILER_ASSERT(cg->extra_progs.len <= cg->extra_progs.cap,
                     "codegen_emit extra_progs vector len exceeds cap");
  NY_COMPILER_ASSERT(cg->extra_progs.data != NULL || cg->extra_progs.len == 0,
                     "codegen_emit extra_progs vector has len but no data");

  scope gsc[NY_SCOPE_STACK_CAP] = {0};
  size_t gd = 0;

  ny_lazy_emit_build_reachable_set(cg);

  ny_tick_t t_emit_top = ny_ticks_now();
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    cg->current_module_name = NULL;
    stmt_t *s = cg->prog->body.data[i];
    NY_COMPILER_ASSERTF(s != NULL,
                        "null top-level stmt during emit at index %zu", i);
    if (!s)
      continue;
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
        !ny_stmt_tree_is_source_context(cg, s)) {
      continue;
    }
    emit_top_functions(cg, s, gsc, gd, NULL);
  }
  for (size_t p = 0; p < cg->extra_progs.len; p++) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    NY_COMPILER_ASSERTF(
        prog->body.len <= prog->body.cap,
        "extra program %zu body vector len exceeds cap during emit", p);
    NY_COMPILER_ASSERTF(
        prog->body.data != NULL || prog->body.len == 0,
        "extra program %zu body vector has len but no data during emit", p);
    for (size_t i = 0; i < prog->body.len; i++) {
      cg->current_module_name = NULL;
      stmt_t *s = prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null extra-program stmt during emit p=%zu index=%zu",
                          p, i);
      if (!s)
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !ny_stmt_tree_is_source_context(cg, s)) {
        continue;
      }
      emit_top_functions(cg, s, gsc, gd, NULL);
    }
  }
  ny_lazy_emit_demand_referenced(cg, gsc, gd, "top");
  if (verbose_enabled >= 1)
    fprintf(stderr, "[*] Codegen: emit top:       %.4fs\n",
            ny_ticks_elapsed_sec(t_emit_top));
}

typedef struct {
  assigned_name_list *names;
  assigned_hash_list *hashes;
  uint64_t *bloom;
} ny_top_entry_block_ctx_t;

static bool ny_collect_top_entry_blocked_expr_pre(ny_visitor_t *v, expr_t *e) {
  if (!v || !e || e->kind != NY_E_IDENT)
    return true;
  ny_top_entry_block_ctx_t *ctx = (ny_top_entry_block_ctx_t *)v->ctx;
  if (!ctx)
    return true;
  assigned_name_add(ctx->names, ctx->hashes, ctx->bloom, e->as.ident.name);
  return true;
}

static bool ny_collect_top_entry_blocked_stmt_pre(ny_visitor_t *v, stmt_t *s) {
  if (!v || !s || s->kind != NY_S_VAR)
    return true;
  ny_top_entry_block_ctx_t *ctx = (ny_top_entry_block_ctx_t *)v->ctx;
  if (!ctx)
    return true;
  for (size_t i = 0; i < s->as.var.names.len; ++i) {
    const char *name = s->as.var.names.data[i];
    if (name && *name)
      assigned_name_add(ctx->names, ctx->hashes, ctx->bloom, name);
  }
  return true;
}

static void ny_collect_top_entry_blocked_names(stmt_t *s,
                                               assigned_name_list *names,
                                               assigned_hash_list *hashes,
                                               uint64_t bloom[4]) {
  if (!s || !names || !hashes)
    return;
  if (s->kind == NY_S_FUNC) {
    ny_top_entry_block_ctx_t ctx = {names, hashes, bloom};
    ny_visitor_t vis = {0};
    vis.ctx = &ctx;
    vis.visit_expr_pre = ny_collect_top_entry_blocked_expr_pre;
    vis.visit_stmt_pre = ny_collect_top_entry_blocked_stmt_pre;
    ny_visit_stmt(&vis, s->as.fn.body);
    return;
  }
  if (s->kind == NY_S_IF) {
    ny_collect_top_entry_blocked_names(s->as.iff.conseq, names, hashes, bloom);
    ny_collect_top_entry_blocked_names(s->as.iff.alt, names, hashes, bloom);
  } else if (s->kind == NY_S_GUARD) {
    ny_collect_top_entry_blocked_names(s->as.guard.fallback, names, hashes,
                                       bloom);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_collect_top_entry_blocked_names(s->as.block.body.data[i], names,
                                         hashes, bloom);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_collect_top_entry_blocked_names(s->as.module.body.data[i], names,
                                         hashes, bloom);
  }
}

static void ny_apply_top_level_typeinfer_to_sema(typeinfer_ctx_t *ctx,
                                                 stmt_t *s) {
  if (!ctx || !s)
    return;
  switch (s->kind) {
  case NY_S_VAR: {
    if (s->sema_kind != NY_STMT_SEMA_VAR)
      return;
       sema_var_t *sv = (sema_var_t *)s->sema;
       if (!sv)
         return;
       arena_t *sema_arena = ctx->cg ? ctx->cg->arena : NULL;
       for (size_t i = 0; i < s->as.var.names.len; ++i) {
         const char *name = s->as.var.names.data[i];
         while (sv->is_int_proven.len <= i) {
           if (sema_arena)
             vec_push_arena(sema_arena, &sv->is_int_proven, false);
           else
             vec_push(&sv->is_int_proven, false);
         }
         while (sv->is_f64_proven.len <= i) {
           if (sema_arena)
             vec_push_arena(sema_arena, &sv->is_f64_proven, false);
           else
             vec_push(&sv->is_f64_proven, false);
         }
         while (sv->escapes.len <= i) {
           if (sema_arena)
             vec_push_arena(sema_arena, &sv->escapes, false);
           else
             vec_push(&sv->escapes, false);
         }
      bool proven_i64 = name && typeinfer_is_i64(ctx, name) &&
                        !typeinfer_needs_dynamic(ctx, name);
      bool proven_f64 = name && typeinfer_is_f64(ctx, name) &&
                        !typeinfer_needs_dynamic(ctx, name);
      bool escapes = name && typeinfer_escapes(ctx, name);
      sv->is_int_proven.data[i] = proven_i64;
      sv->is_f64_proven.data[i] = proven_f64;
      sv->escapes.data[i] = escapes;
    }
    return;
  }
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_apply_top_level_typeinfer_to_sema(ctx, s->as.block.body.data[i]);
    return;
  case NY_S_IF:
    ny_apply_top_level_typeinfer_to_sema(ctx, s->as.iff.conseq);
    ny_apply_top_level_typeinfer_to_sema(ctx, s->as.iff.alt);
    return;
  case NY_S_GUARD:
    ny_apply_top_level_typeinfer_to_sema(ctx, s->as.guard.fallback);
    return;
  default:
    return;
  }
}

/*
 * Collect direct module dependencies from a module's top-level use statements.
 * Returns the number of dependency names written into deps[] (capped at max_deps).
 * Only considers bare `use` or `use .. import *` that import an entire module
 * present in prog->body — selective imports (e.g. `use m as x` with no import_all)
 * do not create a module-level def initialization dependency.
 */
static size_t codegen_collect_module_deps(const stmt_t *mod, const char **deps,
                                          size_t max_deps) {
  size_t count = 0;
  if (!mod || mod->kind != NY_S_MODULE)
    return 0;
  for (size_t i = 0; i < mod->as.module.body.len && count < max_deps; ++i) {
    const stmt_t *child = mod->as.module.body.data[i];
    if (!child || child->kind != NY_S_USE)
      continue;
    if (ny_stmt_is_bare_std_use(child))
      continue;
    const char *target = child->as.use.module;
    if (!target || !*target)
      continue;
    /*
     * Only treat full-module imports as init dependencies.
     */
    if (!child->as.use.import_all && child->as.use.imports.len > 0 &&
        !child->as.use.alias)
      continue;
    /*
     * Avoid duplicates.
     */
    bool dup = false;
    for (size_t j = 0; j < count; ++j) {
      if (strcmp(deps[j], target) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup)
      deps[count++] = target;
  }
  return count;
}

/*
 * Produce a topological ordering of NY_S_MODULE statements from prog->body.
 * Modules that depend on other modules (via `use`) are emitted after their
 * dependencies.  The output order[] array receives indices into prog->body
 * for module statements, in dependency-first order.  Non-module statements
 * are omitted from order[].
 *
 * Returns the number of module indices written to order[].
 */
static size_t codegen_topo_sort_modules(const codegen_t *cg, size_t *order,
                                        size_t max_order) {
  if (!cg || !cg->prog)
    return 0;
  size_t n = cg->prog->body.len;
  /*
   * Count modules and build index map.
   */
  size_t mod_count = 0;
  for (size_t i = 0; i < n; ++i) {
    if (cg->prog->body.data[i] &&
        cg->prog->body.data[i]->kind == NY_S_MODULE)
      ++mod_count;
  }
  if (mod_count == 0)
    return 0;
  /*
   * Allocate temporary arrays on the heap (freed before return).
   */
  size_t *mod_indices = (size_t *)calloc(mod_count, sizeof(size_t));
  const char **mod_names = (const char **)calloc(mod_count, sizeof(const char *));
  int *visited = (int *)calloc(mod_count, sizeof(int));
  /*
   * visited: 0=unvisited, 1=in-stack, 2=done
   */
  if (!mod_indices || !mod_names || !visited) {
    free(mod_indices);
    free(mod_names);
    free(visited);
    return 0;
  }
  {
    size_t mi = 0;
    for (size_t i = 0; i < n; ++i) {
      if (cg->prog->body.data[i] &&
          cg->prog->body.data[i]->kind == NY_S_MODULE) {
        mod_indices[mi] = i;
        mod_names[mi] = cg->prog->body.data[i]->as.module.name;
        ++mi;
      }
    }
  }
  /*
   * Build name-to-index lookup.
   */
  size_t out_count = 0;
  /*
   * Recursive post-order DFS with cycle detection.
   * We use an explicit stack to avoid recursion depth issues.
   */
  enum { STACK_MAX = 4096 };
  struct { size_t mi; size_t child_idx; } stack[STACK_MAX];
  int sp = 0;
  for (size_t root = 0; root < mod_count; ++root) {
    if (visited[root] != 0)
      continue;
    /*
     * Push root.
     */
    if (sp >= STACK_MAX)
      break;
    stack[sp].mi = root;
    stack[sp].child_idx = 0;
    visited[root] = 1; /* in-stack */
    ++sp;
    while (sp > 0) {
      size_t cur = stack[sp - 1].mi;
      size_t ci = stack[sp - 1].child_idx;
      const stmt_t *mod = cg->prog->body.data[mod_indices[cur]];
      /*
       * Collect deps of current module.
       */
      const char *dep_names[256];
      size_t ndeps = codegen_collect_module_deps(mod, dep_names, 256);
      /*
       * Advance to next unvisited dependency.
       */
      bool found_child = false;
      for (; ci < ndeps; ++ci) {
        /*
         * Find dep module index.
         */
        size_t dep_mi = (size_t)-1;
        for (size_t k = 0; k < mod_count; ++k) {
          if (mod_names[k] && strcmp(mod_names[k], dep_names[ci]) == 0) {
            dep_mi = k;
            break;
          }
        }
        if (dep_mi == (size_t)-1 || dep_mi == cur)
          continue;
        if (visited[dep_mi] == 1) {
          /*
           * Cycle — skip (module already on stack).
           */
          continue;
        }
        if (visited[dep_mi] == 2)
          continue;
        /*
         * Push child.
         */
        stack[sp - 1].child_idx = ci + 1;
        if (sp >= STACK_MAX)
          break;
        stack[sp].mi = dep_mi;
        stack[sp].child_idx = 0;
        visited[dep_mi] = 1;
        ++sp;
        found_child = true;
        break;
      }
      if (!found_child) {
        /*
         * All deps visited — emit this module.
         */
        visited[cur] = 2;
        if (out_count < max_order)
          order[out_count++] = mod_indices[cur];
        --sp;
      }
    }
  }
  free(mod_indices);
  free(mod_names);
  free(visited);
  return out_count;
}

LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name) {
  NY_COMPILER_ASSERT(cg != NULL, "codegen_emit_script missing codegen");
  if (!cg)
    return NULL;
  NY_COMPILER_ASSERT(cg->prog != NULL, "codegen_emit_script missing program");
  NY_COMPILER_ASSERT(cg->module != NULL,
                     "codegen_emit_script missing LLVM module");
  NY_COMPILER_ASSERT(cg->builder != NULL,
                     "codegen_emit_script missing LLVM builder");
  NY_COMPILER_ASSERT(name && *name, "codegen_emit_script missing entry name");
  if (!cg->prog || !name || !*name)
    return NULL;
  NY_COMPILER_ASSERT(cg->type_i64 != NULL,
                     "codegen_emit_script missing i64 type");
  NY_COMPILER_ASSERT(cg->prog->body.len <= cg->prog->body.cap,
                     "codegen_emit_script program body vector len exceeds cap");
  NY_COMPILER_ASSERT(
      cg->prog->body.data != NULL || cg->prog->body.len == 0,
      "codegen_emit_script program body vector has len but no data");

  cg->current_module_name = NULL;
  LLVMValueRef fn = ny_get_named_fn(cg, name);
  if (ny_fn_has_body(fn))
    return fn;
  if (!fn) {
    fn = LLVMAddFunction(cg->module, name,
                         LLVMFunctionType(cg->type_i64, NULL, 0, 0));
  }
  ny_debug_apply_fn_attrs(cg, fn);
  LLVMMetadataRef prev_scope = cg->di_scope;
  LLVMMetadataRef prev_loc = cg->di_loc;
  LLVMBasicBlockRef cur = ny_cur_block(cg);
  LLVMBasicBlockRef init_block = ny_bb_fn(fn, "init");
  LLVMBasicBlockRef body_block = ny_bb_fn(fn, "body");
  if (cg->debug_symbols && cg->di_builder) {
    token_t tok = {0};
    tok.filename = cg->debug_main_file ? cg->debug_main_file : "<inline>";
    tok.line = 1;
    tok.col = 0;
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, fn, name, tok);
    if (sp)
      cg->di_scope = sp;
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
    if (cg->alloca_builder)
      LLVMSetCurrentDebugLocation2(cg->alloca_builder, NULL);
  }
  ny_pos(cg, body_block);
  scope sc[NY_SCOPE_STACK_CAP] = {0};
  size_t d = 0;
  assigned_name_list top_entry_blocked_names = {0};
  assigned_hash_list top_entry_blocked_hashes = {0};
  uint64_t top_entry_blocked_bloom[4] = {0, 0, 0, 0};

  if (cg->opt_type_infer && cg->prog && cg->prog->body.len > 0) {
    typeinfer_ctx_t infer_ctx = {0};
    typeinfer_ctx_init(&infer_ctx, 256, sc, cg);

    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null top-level typeinfer stmt at index %zu", i);
      if (!s)
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !cg->emit_cached_stdlib_init && !ny_stmt_tree_is_source_context(cg, s))
        continue;
      if (s->kind != NY_S_FUNC)
        typeinfer_walk_stmt(&infer_ctx, s);
    }
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(
          s != NULL, "null top-level typeinfer apply stmt at index %zu", i);
      if (!s)
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !cg->emit_cached_stdlib_init && !ny_stmt_tree_is_source_context(cg, s))
        continue;
      if (s->kind != NY_S_FUNC)
        ny_apply_top_level_typeinfer_to_sema(&infer_ctx, s);
    }
    for (size_t i = 0; i < infer_ctx.var_names_len; ++i) {
      const char *vname = infer_ctx.vars[i].name;
      if (!vname || !*vname)
        continue;
      if (typeinfer_needs_dynamic(&infer_ctx, vname))
        assigned_name_add(&top_entry_blocked_names, &top_entry_blocked_hashes,
                          top_entry_blocked_bloom, vname);
    }

    typeinfer_apply_to_scopes(&infer_ctx, sc, 1);
    typeinfer_ctx_dispose(&infer_ctx);
  }

  if (cg->prog && cg->prog->body.len > 0) {
    /*
     * Source tokens keep the parser spelling (often repository-relative),
     * while -g resolves debug_main_file to an absolute path.  Compilation
     * ownership must use the parser spelling; using the debug path here made
     * the top-level filter silently omit user functions in debug builds.
     */
    const char *root_file =
        (cg->source_main_file && *cg->source_main_file) ? cg->source_main_file
                                                         : NULL;
    if (!root_file && cg->debug_main_file && *cg->debug_main_file)
      root_file = cg->debug_main_file;
    bool has_user_top_funcs = false;
    if (!root_file) {
      for (size_t i = 0; i < cg->prog->body.len; i++) {
        stmt_t *s = cg->prog->body.data[i];
        if (!s || ny_is_stdlib_tok(s->tok))
          continue;
        if (s->tok.filename && *s->tok.filename) {
          root_file = s->tok.filename;
          break;
        }
        if (ny_stmt_contains_top_function(s))
          has_user_top_funcs = true;
      }
    }
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      if (!s || ny_is_stdlib_tok(s->tok))
        continue;
      if (ny_stmt_contains_top_function(s)) {
        has_user_top_funcs = true;
        break;
      }
    }
    if (!root_file)
      root_file = cg->debug_main_file;
    size_t start_idx = 0;
    if (root_file && *root_file) {
      start_idx = cg->prog->body.len;
      while (start_idx > 0) {
        stmt_t *s = cg->prog->body.data[start_idx - 1];
        if (!s || ny_is_stdlib_tok(s->tok))
          break;
        if (!s->tok.filename || strcmp(root_file, s->tok.filename) != 0)
          break;
        start_idx--;
      }
      if (start_idx == cg->prog->body.len)
        start_idx = 0;
    }
    for (size_t i = start_idx; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null top-entry blocked-name stmt at index %zu", i);
      if (!s)
        continue;
      if (ny_is_stdlib_tok(s->tok))
        continue;
      if (root_file && s->tok.filename &&
          strcmp(root_file, s->tok.filename) != 0)
        continue;
      ny_collect_top_entry_blocked_names(s, &top_entry_blocked_names,
                                         &top_entry_blocked_hashes,
                                         top_entry_blocked_bloom);
    }
  }
  cg->top_entry_blocked_names_data = top_entry_blocked_names.data;
  cg->top_entry_blocked_names_len = top_entry_blocked_names.len;
  cg->top_entry_blocked_hashes_data = top_entry_blocked_hashes.data;
  cg->top_entry_blocked_hashes_len = top_entry_blocked_hashes.len;
  cg->top_entry_blocked_bloom[0] = top_entry_blocked_bloom[0];
  cg->top_entry_blocked_bloom[1] = top_entry_blocked_bloom[1];
  cg->top_entry_blocked_bloom[2] = top_entry_blocked_bloom[2];
  cg->top_entry_blocked_bloom[3] = top_entry_blocked_bloom[3];
  cg->top_entry_local_hoist_enabled = true;

  LLVMValueRef std_init = ny_get_named_fn(cg, "__std_init");
  if (std_init) {
    LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(std_init), std_init,
                   NULL, 0, "");
  }

  size_t stmt_count = 0;
  /*
   * Emit module-level defs in dependency order so that a module's
   * use-dependencies are initialized before the module itself.
   * Non-module statements keep their original source order.
   */
  {
    size_t mod_order[4096];
    size_t mod_n = codegen_topo_sort_modules(
        cg, mod_order, sizeof(mod_order) / sizeof(mod_order[0]));
    bool *emitted =
        (bool *)calloc(cg->prog->body.len > 0 ? cg->prog->body.len : 1,
                       sizeof(bool));
    /*
     * Emit modules in topological (dependency-first) order.
     */
    for (size_t mi = 0; mi < mod_n; ++mi) {
      size_t i = mod_order[mi];
      stmt_t *s = cg->prog->body.data[i];
      if (!s || s->kind != NY_S_MODULE)
        continue;
      if (emitted && emitted[i])
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !cg->emit_cached_stdlib_init &&
          !ny_stmt_tree_is_source_context(cg, s))
        continue;
      if (emitted)
        emitted[i] = true;
      cg->current_module_name = NULL;
      NY_COMPILER_ASSERTF(
          d < 64,
          "top-level scope depth %zu exceeds fixed stack before module %zu",
          d, i);
      if (stmt_count > 0 && stmt_count % 100 == 0) {
        LLVMBasicBlockRef next_bb = ny_bb_fn(fn, "top_chunk");
        ny_br(cg, next_bb);
        ny_pos(cg, next_bb);
      }
      gen_stmt(cg, sc, &d, s, 0, false);
      cg->current_module_name = NULL;
      NY_COMPILER_ASSERTF(
          d < 64,
          "top-level scope depth %zu exceeds fixed stack after module %zu",
          d, i);
      stmt_count++;
    }
    /*
     * Emit non-module statements in original source order.
     */
    for (size_t i = 0; i < cg->prog->body.len; i++) {
      stmt_t *s = cg->prog->body.data[i];
      NY_COMPILER_ASSERTF(s != NULL,
                          "null top-level script stmt at index %zu", i);
      if (!s)
        continue;
      if (s->kind == NY_S_MODULE || s->kind == NY_S_FUNC)
        continue;
      if (emitted && emitted[i])
        continue;
      if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
          !cg->emit_cached_stdlib_init &&
          !ny_stmt_tree_is_source_context(cg, s))
        continue;
      cg->current_module_name = NULL;
      NY_COMPILER_ASSERTF(
          d < 64,
          "top-level scope depth %zu exceeds fixed stack before stmt %zu", d,
          i);
      if (stmt_count > 0 && stmt_count % 100 == 0) {
        LLVMBasicBlockRef next_bb = ny_bb_fn(fn, "top_chunk");
        ny_br(cg, next_bb);
        ny_pos(cg, next_bb);
      }
      gen_stmt(cg, sc, &d, s, 0, false);
      cg->current_module_name = NULL;
      NY_COMPILER_ASSERTF(
          d < 64,
          "top-level scope depth %zu exceeds fixed stack after stmt %zu", d,
          i);
      stmt_count++;
    }
    free(emitted);
  }
  ny_lazy_emit_demand_referenced(cg, sc, d, "script");
  cg->current_module_name = NULL;
  if (!ny_has_terminator(cg)) {
    LLVMBuildRet(cg->builder, ny_c1(cg));
  }
  ny_pos(cg, init_block);
  if (cg->debug_symbols && cg->di_builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  codegen_emit_string_init(cg);
  ny_br(cg, body_block);
  vec_free(&sc[0].defers);
  vec_free(&sc[0].vars);
  if (cur) {
    ny_pos(cg, cur);
  }
  cg->top_entry_local_hoist_enabled = false;
  cg->top_entry_blocked_names_data = NULL;
  cg->top_entry_blocked_names_len = 0;
  cg->top_entry_blocked_hashes_data = NULL;
  cg->top_entry_blocked_hashes_len = 0;
  cg->top_entry_blocked_bloom[0] = 0;
  cg->top_entry_blocked_bloom[1] = 0;
  cg->top_entry_blocked_bloom[2] = 0;
  cg->top_entry_blocked_bloom[3] = 0;
  vec_free(&top_entry_blocked_names);
  vec_free(&top_entry_blocked_hashes);
  cg->di_scope = prev_scope;
  cg->di_loc = prev_loc;
  if (cg->debug_symbols && cg->builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, prev_loc);
    if (cg->alloca_builder)
      LLVMSetCurrentDebugLocation2(cg->alloca_builder, prev_loc);
  }
  return fn;
}

void codegen_emit_string_init(codegen_t *cg) {
  LLVMTypeRef i8_ptr_ty = LLVMPointerType(ny_i8_ty(cg), 0);
  LLVMTypeRef i8_ty = ny_i8_ty(cg);
  bool const_string_global_init =
      ny_codegen_speed_profile_enabled(cg) ||
      ny_fast_path_enabled(cg, "NYTRIX_CONST_STRING_GLOBAL_INIT");

  size_t str_count = 0;
  bool init_all = (cg->current_module_name == NULL);
  for (size_t i = 0; i < cg->interns.len; i++) {
    if ((init_all || cg->interns.data[i].module == cg->module) &&
        cg->interns.data[i].gv && cg->interns.data[i].val)
      str_count++;
  }

  if (str_count > 0) {
    LLVMValueRef *elements = malloc(str_count * sizeof(LLVMValueRef));
    if (!elements)
      return;
    size_t idx = 0;
    for (size_t i = 0; i < cg->interns.len; i++) {
      string_intern *si = &cg->interns.data[i];
      if ((!init_all && si->module != cg->module) || !si->gv || !si->val)
        continue;
      elements[idx++] = LLVMConstPointerCast(si->gv, i8_ptr_ty);
    }

    LLVMValueRef used_global = LLVMGetNamedGlobal(cg->module, "llvm.used");
    if (used_global) {

      LLVMValueRef old_init = LLVMGetInitializer(used_global);
      size_t old_count = LLVMGetArrayLength(LLVMTypeOf(old_init));
      if (old_count > SIZE_MAX - str_count ||
          old_count + str_count > SIZE_MAX / sizeof(LLVMValueRef)) {
        free(elements);
        return;
      }
      size_t new_count = old_count + str_count;
      LLVMValueRef *new_elements = malloc(new_count * sizeof(LLVMValueRef));
      if (!new_elements) {
        free(elements);
        return;
      }
      for (size_t j = 0; j < old_count; j++)
        new_elements[j] = LLVMGetAggregateElement(old_init, (unsigned)j);
      for (size_t j = 0; j < str_count; j++)
        new_elements[old_count + j] = elements[j];
      ny_replace_llvm_used_global(cg->module, i8_ptr_ty, new_elements,
                                  new_count);
      free(new_elements);
    } else {
      ny_replace_llvm_used_global(cg->module, i8_ptr_ty, elements, str_count);
    }
    free(elements);
  }

  if (cg->emit_module_decls_only) {
    LLVMValueRef g = LLVMGetFirstGlobal(cg->module);
    while (g) {
      const char *gname = LLVMGetValueName(g);
      if (gname && strncmp(gname, ".str.data.", 10) == 0) {

        bool found = false;
        LLVMValueRef ug = LLVMGetNamedGlobal(cg->module, "llvm.used");
        if (ug) {
          LLVMValueRef init = LLVMGetInitializer(ug);
          size_t uc = LLVMGetArrayLength(LLVMTypeOf(init));
          for (size_t j = 0; j < uc; j++) {
            LLVMValueRef elem = LLVMGetAggregateElement(init, (unsigned)j);
            if (elem == g ||
                (elem && LLVMIsAConstantExpr(elem) &&
                 LLVMGetOperand(LLVMIsAConstantExpr(elem), 0) == g)) {
              found = true;
              break;
            }
          }
        }
        if (!found) {
          LLVMValueRef cast = LLVMConstPointerCast(g, i8_ptr_ty);
          LLVMValueRef ug2 = LLVMGetNamedGlobal(cg->module, "llvm.used");
          if (ug2) {
            LLVMValueRef old_init = LLVMGetInitializer(ug2);
            size_t old_count = LLVMGetArrayLength(LLVMTypeOf(old_init));
            if (old_count == SIZE_MAX ||
                old_count + 1 > SIZE_MAX / sizeof(LLVMValueRef))
              return;
            size_t new_count = old_count + 1;
            LLVMValueRef *new_elements =
                malloc(new_count * sizeof(LLVMValueRef));
            if (!new_elements)
              return;
            for (size_t j = 0; j < old_count; j++)
              new_elements[j] = LLVMGetAggregateElement(old_init, (unsigned)j);
            new_elements[old_count] = cast;
            ny_replace_llvm_used_global(cg->module, i8_ptr_ty, new_elements,
                                        new_count);
            free(new_elements);
          }
        }
      }
      g = LLVMGetNextGlobal(g);
    }
  }

  for (size_t i = 0; i < cg->interns.len; i++) {
    if (!init_all && cg->interns.data[i].module != cg->module)
      continue;
    LLVMValueRef str_array_global = cg->interns.data[i].gv;
    LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
    if (!str_array_global || !runtime_ptr_global)
      continue;
    if (const_string_global_init &&
        ny_set_const_string_runtime_initializer(cg, str_array_global,
                                                runtime_ptr_global, i8_ty))
      continue;
    LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);

    LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
    LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(
        cg->builder, i8_ty, str_array_global, indices, 1, "");
    if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
      ny_store(cg, runtime_ptr_global, str_data_ptr);
    } else {
      LLVMValueRef str_data_int = ny_ptr2i64(cg, str_data_ptr, "");
      ny_store(cg, runtime_ptr_global, str_data_int);
    }
  }

  if (init_all) {
    size_t old_intern_len = cg->interns.len;
    codegen_repopulate_interns(cg);
    for (size_t i = old_intern_len; i < cg->interns.len; i++) {
      if (cg->interns.data[i].module != cg->module)
        continue;
      LLVMValueRef str_array_global = cg->interns.data[i].gv;
      LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
      if (!str_array_global || !runtime_ptr_global)
        continue;
      if (const_string_global_init &&
          ny_set_const_string_runtime_initializer(cg, str_array_global,
                                                  runtime_ptr_global, i8_ty))
        continue;
      LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);

      LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
      LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(
          cg->builder, i8_ty, str_array_global, indices, 1, "");
      if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
        ny_store(cg, runtime_ptr_global, str_data_ptr);
      } else {
        LLVMValueRef str_data_int = ny_ptr2i64(cg, str_data_ptr, "");
        ny_store(cg, runtime_ptr_global, str_data_int);
      }
    }
  }
}
