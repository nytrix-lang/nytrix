static void codegen_free_owned_binding_name(binding *b) {
  if (!b || !b->owned || !b->name)
    return;
  if (!ny_intern_contains_ptr(b->name))
    free((void *)b->name);
  b->name = NULL;
}

static void codegen_free_owned_alias_binding(binding *b) {
  if (!b || !b->owned)
    return;
  codegen_free_owned_binding_name(b);
  free((void *)b->stmt_t);
  b->stmt_t = NULL;
}

static void codegen_free_layout_def(layout_def_t *def) {
  if (!def)
    return;
  bool owns_field_strings = def->stmt == NULL;
  if (def->name && !ny_intern_contains_ptr(def->name))
    free((void *)def->name);
  def->name = NULL;
  for (size_t i = 0; i < def->fields.len; i++) {
    if (owns_field_strings) {
      free((void *)def->fields.data[i].name);
      free((void *)def->fields.data[i].type_name);
    }
  }
  vec_free(&def->fields);
  vec_free(&def->deftype_param_names);
  vec_free(&def->deftype_param_vals);
  if (def->heap_allocated)
    free(def);
}

void codegen_dispose(codegen_t *cg) {
  if (!cg)
    return;

  if (cg->di_builder) {
    codegen_debug_finalize(cg);
  }
  ny_sym_state_free(cg);
  if (cg->alloca_builder) {
    LLVMDisposeBuilder(cg->alloca_builder);
    cg->alloca_builder = NULL;
  }
  if (cg->ee) {
    LLVMDisposeExecutionEngine(cg->ee);
    cg->ee = NULL;
  }
  if (cg->orc_jit) {
    ny_orc_jit_dispose(cg->orc_jit);
    cg->orc_jit = NULL;
  }
  if (cg->builder) {
    LLVMDisposeBuilder(cg->builder);
    cg->builder = NULL;
  }
  if (cg->llvm_ctx_owned) {
    if (cg->module) {
      LLVMDisposeModule(cg->module);
      cg->module = NULL;
    }
    if (cg->ctx) {
      LLVMContextDispose(cg->ctx);
      cg->ctx = NULL;
    }
  }
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    void *byval_data = cg->fun_sigs.data[i].native_byval_param_layouts.data;
    if (!byval_data)
      continue;
    for (size_t j = i + 1; j < cg->fun_sigs.len; ++j) {
      if (cg->fun_sigs.data[j].native_byval_param_layouts.data != byval_data)
        continue;
      cg->fun_sigs.data[j].native_byval_param_layouts.data = NULL;
      cg->fun_sigs.data[j].native_byval_param_layouts.len = 0;
      cg->fun_sigs.data[j].native_byval_param_layouts.cap = 0;
    }
  }
  for (size_t i = 0; i < cg->fun_sigs.len; i++)
    ny_fun_sig_free_members(&cg->fun_sigs.data[i]);
  vec_free(&cg->fun_sigs);
  for (size_t i = 0; i < cg->global_vars.len; i++)
    codegen_free_owned_binding_name(&cg->global_vars.data[i]);
  vec_free(&cg->global_vars);
  for (size_t i = 0; i < cg->interns.len; i++) {
    void *alloc = cg->interns.data[i].alloc;
    if (!alloc)
      continue;
    bool seen = false;
    for (size_t j = 0; j < i; j++) {
      if (cg->interns.data[j].alloc == alloc) {
        seen = true;
        break;
      }
    }
    if (!seen)
      free(alloc);
  }
  vec_free(&cg->interns);
  if (cg->intern_map) {
    free(cg->intern_map);
    cg->intern_map = NULL;
  }
  free(cg->builtin_shadow_cache);
  cg->builtin_shadow_cache = NULL;
  for (size_t i = 0; i < cg->aliases.len; i++)
    codegen_free_owned_alias_binding(&cg->aliases.data[i]);
  vec_free(&cg->aliases);
  free(cg->module_alias_index);
  cg->module_alias_index = NULL;
  cg->module_alias_index_cap = 0;
  cg->module_alias_index_len = 0;
  for (size_t i = 0; i < cg->import_aliases.len; i++)
    codegen_free_owned_alias_binding(&cg->import_aliases.data[i]);
  vec_free(&cg->import_aliases);
  for (size_t i = 0; i < cg->user_import_aliases.len; i++)
    codegen_free_owned_alias_binding(&cg->user_import_aliases.data[i]);
  vec_free(&cg->user_import_aliases);
  vec_free(&cg->import_alias_hashes);
  vec_free(&cg->user_import_alias_hashes);
  free(cg->import_alias_index);
  free(cg->user_import_alias_index);
  cg->import_alias_index = NULL;
  cg->user_import_alias_index = NULL;
  cg->import_alias_index_cap = 0;
  cg->user_import_alias_index_cap = 0;
  free(cg->module_stmt_index);
  cg->module_stmt_index = NULL;
  cg->module_stmt_index_cap = 0;
  cg->module_stmt_index_len = 0;
  free(cg->module_stmt_lookup_cache);
  cg->module_stmt_lookup_cache = NULL;
  free(cg->module_public_target_cache);
  cg->module_public_target_cache = NULL;
  free(cg->use_alias_lookup_cache);
  cg->use_alias_lookup_cache = NULL;
  for (size_t i = 0; i < cg->use_modules.len; i++)
    free(cg->use_modules.data[i]);
  vec_free(&cg->use_modules);
  for (size_t i = 0; i < cg->user_use_modules.len; i++)
    free(cg->user_use_modules.data[i]);
  vec_free(&cg->user_use_modules);
  for (size_t i = 0; i < cg->link_allowed_modules.len; i++)
    free(cg->link_allowed_modules.data[i]);
  vec_free(&cg->link_allowed_modules);
  for (size_t i = 0; i < cg->tagged_types.len; i++)
    free(cg->tagged_types.data[i]);
  vec_free(&cg->tagged_types);
  vec_free(&cg->lazy_emit_names);
  vec_free(&cg->lazy_emit_hashes);
  ny_lazy_name_set_free(&cg->lazy_emit_name_set);
  vec_free(&cg->lazy_emit_collected_names);
  vec_free(&cg->lazy_emit_collected_hashes);
  ny_lazy_name_set_free(&cg->lazy_emit_collected_set);
  vec_free(&cg->labels);
  vec_free(&cg->operators);
  vec_free(&cg->enums);
  for (size_t i = 0; i < cg->layouts.len; i++)
    codegen_free_layout_def(cg->layouts.data[i]);
  vec_free(&cg->layouts);
  vec_free(&cg->mono_specs);
  for (size_t i = 0; i < cg->links.len; i++)
    free(cg->links.data[i]);
  vec_free(&cg->links);
  for (size_t i = 0; i < cg->ffi.defines.len; i++)
    free(cg->ffi.defines.data[i]);
  vec_free(&cg->ffi.defines);
  for (size_t i = 0; i < cg->extra_progs.len; i++) {
    program_t *prog = cg->extra_progs.data[i];
    arena_t *arena = i < cg->extra_arenas.len ? cg->extra_arenas.data[i] : NULL;
    bool arena_seen = false;
    for (size_t j = 0; j < i && j < cg->extra_arenas.len; j++) {
      if (cg->extra_arenas.data[j] == arena) {
        arena_seen = true;
        break;
      }
    }
    if (arena && arena != (arena_t *)cg->arena && !arena_seen)
      program_free(prog, arena);
    if (prog && prog != cg->prog)
      free(prog);
  }
  for (size_t i = cg->extra_progs.len; i < cg->extra_arenas.len; i++) {
    arena_t *arena = cg->extra_arenas.data[i];
    bool arena_seen = false;
    for (size_t j = 0; j < i; j++) {
      if (cg->extra_arenas.data[j] == arena) {
        arena_seen = true;
        break;
      }
    }
    if (arena && arena != (arena_t *)cg->arena && !arena_seen) {
      arena_free(arena);
      free(arena);
    }
  }
  vec_free(&cg->extra_arenas);
  vec_free(&cg->extra_progs);
  if (cg->prog && cg->prog_owned) {
    program_free(cg->prog, (arena_t *)cg->arena);
    free(cg->prog);
  }
}
