static bool ny_native_target_platform_ident(const ny_options *opt,
                                            const char *name, bool *out);

static bool nyr_negative_literal_i64(const expr_t *e, int64_t *out) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL) {
    if (e->as.literal.as.i < 0) {
      if (out)
        *out = e->as.literal.as.i;
      return true;
    }
    return false;
  }
  if (e->kind != NY_E_UNARY || !e->as.unary.op ||
      strcmp(e->as.unary.op, "-") != 0 || !e->as.unary.right ||
      e->as.unary.right->kind != NY_E_LITERAL ||
      e->as.unary.right->as.literal.kind != NY_LIT_INT ||
      e->as.unary.right->tok.kind == NY_T_NIL ||
      e->as.unary.right->as.literal.as.i < 0)
    return false;
  if (out)
    *out = -e->as.unary.right->as.literal.as.i;
  return true;
}

static bool nyr_expr_is_callable_value(nyr_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_FN || e->kind == NY_E_LAMBDA)
    return true;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *local = nyr_find_expr_local(b, e);
    if (local)
      return local->semantic_rep == NY_SEM_REP_CLOSURE ||
             local->callable_expr || local->callable_name;
    return (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_CLOSURE) ||
           nyr_find_user_function(b, e->as.ident.name) != NULL;
  }
  return e->semantic.resolved && e->semantic.rep == NY_SEM_REP_CLOSURE;
}

/*
 * Shared `.long` receiver classification.  The memcall and member spellings
 * must apply exactly the same slot facts so they cannot drift apart: a
 * bigint slot never holds a raw machine word, so its stale RAW_INT fact
 * must not select the raw scalar bridge.
 */
static bool nyr_long_receiver_is_raw_word(const nyr_local_t *l) {
  return l && !l->is_any && !l->is_cstr && !l->is_list && !l->is_dict &&
         !l->is_f64 && !l->is_f32 && !l->is_bigint &&
         l->semantic_rep == NY_SEM_REP_RAW_INT;
}

static const expr_t *nyr_find_bound_top_level_value(nyr_builder_impl_t *b,
                                                    const char *name,
                                                    const char *source_file) {
  if (!b || !name)
    return NULL;
  const expr_t *value =
      source_file ? nyr_find_top_level_value_in_source(b, name, source_file)
                  : nyr_find_top_level_value(b, name);
  if (value || !b->module_name || !*b->module_name)
    return value;
  char qualified[512];
  int n = snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name, name);
  if (n <= 0 || (size_t)n >= sizeof(qualified))
    return NULL;
  return nyr_find_top_level_value(b, qualified);
}

static const stmt_t *nyr_namespace_function_value(nyr_builder_impl_t *b,
                                                  const expr_t *e) {
  if (!b || !e)
    return NULL;
  const expr_t *target = NULL;
  const char *member = NULL;
  if (e->kind == NY_E_MEMBER) {
    target = e->as.member.target;
    member = e->as.member.name;
  }
  if (!target || !member)
    return NULL;
  if (nyr_expr_is_cstr(b, target))
    return NULL;

  const expr_t *root = target;
  while (root) {
    if (root->kind == NY_E_MEMBER) {
      root = root->as.member.target;
      continue;
    }
    if (root->kind == NY_E_SPLICE) {
      root = root->as.splice.expr;
      continue;
    }
    break;
  }
  if (!root || root->kind != NY_E_IDENT || !root->as.ident.name)
    return NULL;
  const expr_t *global_value = nyr_find_bound_top_level_value(
      b, root->as.ident.name, root->tok.filename);
  const bool bound_value =
      nyr_find_local(b, root->as.ident.name) != NULL || global_value != NULL;
  char qualified[512];
  if (!nyr_qualified_expr(target, qualified, sizeof(qualified)))
    return NULL;
  const stmt_t *enum_stmt = NULL;
  const stmt_enum_item_t *enum_item = NULL;
  int64_t enum_value = 0;
  if (nyr_find_enum_member(b, qualified, &enum_stmt, &enum_item, &enum_value))
    return NULL;
  const char *module_alias =
      bound_value ? NULL : nyr_resolve_use_alias(b, root->as.ident.name);
  if (!module_alias && !bound_value) {
    nyr_builder_impl_t probe = *b;
    probe.source_file = NULL;
    module_alias = nyr_resolve_use_alias(&probe, root->as.ident.name);
  }
  bool module_namespace =
      (!bound_value && module_alias) ||
      (!nyr_find_local(b, root->as.ident.name) &&
       !nyr_find_user_function(b, root->as.ident.name) && !global_value &&
       !nyr_find_imported_value(b, root->as.ident.name));
  if (!module_namespace)
    return NULL;

  size_t used = strlen(qualified);
  size_t member_len = strlen(member);
  if (used + member_len + 2 > sizeof(qualified))
    return NULL;
  qualified[used++] = '.';
  memcpy(qualified + used, member, member_len + 1);

  const char *dot = strchr(qualified, '.');
  if (dot && module_alias) {
    size_t alias_len = (size_t)(dot - qualified);
    char alias[256];
    if (alias_len < sizeof(alias)) {
      memcpy(alias, qualified, alias_len);
      alias[alias_len] = '\0';
      const char *resolved = nyr_resolve_use_alias(b, alias);
      if (!resolved)
        resolved = module_alias;
      char canonical[512];
      int n = snprintf(canonical, sizeof(canonical), "%s%s", resolved, dot);
      if (n <= 0 || (size_t)n >= sizeof(canonical))
        return NULL;
      memcpy(qualified, canonical, (size_t)n + 1);
    }
  }
  const stmt_t *function = nyr_find_user_function(b, qualified);
  if (!function && e->semantic.canonical_callee_stmt &&
      e->semantic.canonical_callee_stmt->kind == NY_S_FUNC)
    function = e->semantic.canonical_callee_stmt;
  return function;
}

static const char *nyr_callable_storage_marker(const nyr_builder_t *b,
                                               const stmt_t *callable_fn) {
  bool word_args =
      !b || !b->options || b->options->native_backend != NY_NATIVE_BACKEND_LLVM;
  bool raw_scalar_args = callable_fn && callable_fn->kind == NY_S_FUNC &&
                         !callable_fn->as.fn.is_variadic;
  if (raw_scalar_args) {
    for (size_t i = 0; i < callable_fn->as.fn.params.len; ++i) {
      const char *type = callable_fn->as.fn.params.data[i].type;
      if (!type || !ny_native_type_name_is_int(type) ||
          ny_native_type_name_is_128bit_int(type)) {
        raw_scalar_args = false;
        break;
      }
    }
  }
  bool returns_bool = callable_fn && callable_fn->kind == NY_S_FUNC &&
                      callable_fn->as.fn.return_type &&
                      strcmp(callable_fn->as.fn.return_type, "bool") == 0;
  bool returns_f64 =
      callable_fn && callable_fn->kind == NY_S_FUNC &&
      ((callable_fn->as.fn.return_semantic.resolved &&
        callable_fn->as.fn.return_semantic.rep == NY_SEM_REP_F64) ||
       ny_native_type_name_is_f64(callable_fn->as.fn.return_type));
  bool returns_raw_int = false;
  if (callable_fn && callable_fn->kind == NY_S_FUNC) {
    const stmt_func_t *fn = &callable_fn->as.fn;
    returns_raw_int = fn->return_semantic.resolved &&
                      fn->return_semantic.rep == NY_SEM_REP_RAW_INT &&
                      ny_sem_rep_coherent_with_type(fn->return_type,
                                                    fn->return_semantic.rep) &&
                      !ny_native_type_name_is_128bit_int(fn->return_type);
    if (!returns_raw_int && fn->return_type)
      returns_raw_int = ny_native_type_name_is_int(fn->return_type) &&
                        !ny_native_type_name_is_128bit_int(fn->return_type);
  }
  if (raw_scalar_args) {
    if (returns_bool)
      return word_args ? "rt_mark_dynamic_bool_callable_word_args"
                       : "rt_mark_dynamic_bool_callable";
    if (returns_raw_int)
      return word_args ? "rt_mark_dynamic_callable_word_args_raw_result"
                       : "rt_mark_dynamic_callable_raw_result";
    return word_args ? "rt_mark_dynamic_callable_word_args"
                     : "rt_mark_dynamic_callable";
  }
  if (returns_bool)
    return word_args ? "rt_mark_dynamic_bool_callable_tagged_word_args"
                     : "rt_mark_dynamic_bool_callable_tagged_args";
  if (returns_f64)
    return word_args ? "rt_mark_dynamic_callable_word_args_f64_result"
                     : "rt_mark_dynamic_callable_tagged_args_f64_result";
  if (returns_raw_int)
    return word_args ? "rt_mark_dynamic_callable_tagged_word_args_raw_result"
                     : "rt_mark_dynamic_callable_tagged_args_raw_result";
  return word_args ? "rt_mark_dynamic_callable_tagged_word_args"
                   : "rt_mark_dynamic_callable_tagged_args";
}

static int nyr_lower_expr_impl(nyr_builder_t *b, const expr_t *e) {
  if (!e) {
    return nyr_fail(b, "native NYIR lower: missing expression"), -1;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL)
      return nyr_emit_const(b, e->as.literal.as.b ? 1 : 0);
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return e->as.literal.hint == NY_LIT_HINT_F32
                 ? nyr_emit_const_f32(b, e->as.literal.as.f)
                 : nyr_emit_const_f64(b, e->as.literal.as.f);
    if (e->tok.kind == NY_T_NIL)
      return nyr_emit_const(b, NY_IMM_NIL);
    if (e->as.literal.kind == NY_LIT_STR) {
      /*
       * C-string pointer via interned .Lnystr.N (appended into the object).
       */
      const char *s = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
      size_t slen = e->as.literal.as.s.len;
      const char *sym = nyr_strtab_intern(s, slen, NULL, 0);
      if (!sym) {
        return nyr_fail(b, "native NYIR lower: string table full or OOM"), -1;
      }
      int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = sym});
      if (addr < 0) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      /* String literals enter the managed string representation at their
       * source boundary. Generic runtime code can then classify them by the
       * object descriptor instead of guessing from readable bytes. */
      int string =
          nyr_emit_runtime_call(b, "rt_cstr_to_str", addr, -1, -1, 1, 0);
      if (string < 0)
        return -1;
      int length = nyr_emit_const(b, (int64_t)slen);
      int tag = nyr_emit_const(b, TAG_STR_CONST);
      if (length >= 0 && tag >= 0) {
        nyr_record_dyn_fact(b, string, NYR_FACT_DYN_STR_LEN, length);
        nyr_record_dyn_fact(b, string, NYR_FACT_DYN_TAG, tag);
      }
      return string;
    }
    if (e->as.literal.kind != NY_LIT_INT) {
      return nyr_fail(b, "native NYIR lower: only int/bool/f64/nil/string "
                         "literals are supported"),
             -1;
    }
    int64_t int_lit = e->as.literal.as.i;
    /*
     * The tagged-int encoding (v << 1 | 1) cannot hold |v| >= 2^62, so a
     * decimal literal at that magnitude is a bigint: materialize it as a
     * bigint object instead of a raw i64, so type(x) == "bigint" and
     * to_str(x) round-trip the decimal digits.
     */
    if (int_lit >= (INT64_C(1) << 62) || int_lit <= -(INT64_C(1) << 62)) {
      const char *digits = e->as.literal.numeric_text;
      size_t digits_len = e->as.literal.numeric_len;
      bool decimal =
          digits && digits_len > 0 &&
          !(digits_len > 2 && digits[0] == '0' &&
            (digits[1] == 'x' || digits[1] == 'X' || digits[1] == 'b' ||
             digits[1] == 'B' || digits[1] == 'o' || digits[1] == 'O'));
      if (decimal) {
        const char *sym = nyr_strtab_intern(digits, digits_len, NULL, 0);
        int addr =
            sym ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                    .dst = -1,
                                                    .a = -1,
                                                    .b = -1,
                                                    .imm = 0,
                                                    .symbol = sym})
                : -1;
        if (addr < 0) {
          return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
        }
        return nyr_emit_runtime_call(b, "rt_bigint_from_str", addr, -1, -1, 1,
                                     0);
      }
      if (digits && digits_len > 0) {
        const char *sym = nyr_strtab_intern(digits, digits_len, NULL, 0);
        int addr =
            sym ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                    .dst = -1,
                                                    .a = -1,
                                                    .b = -1,
                                                    .imm = 0,
                                                    .symbol = sym})
                : -1;
        if (addr < 0) {
          return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
        }
        return nyr_emit_runtime_call(b, "rt_bigint_from_literal", addr, -1, -1,
                                     1, 0);
      }
      int raw = nyr_emit_const(b, int_lit);
      return raw < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", raw,
                                             -1, -1, 1, 0);
    }
    return nyr_emit_const(b, int_lit);
  case NY_E_EMBED: {
    const char *path = e->as.embed.path;
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
      return nyr_fail(b, "native NYIR lower: failed to open embed file '%s'",
                      path ? path : ""),
             -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
      fclose(file);
      return -1;
    }
    long end = ftell(file);
    if (end < 0 || (uint64_t)end > SIZE_MAX - 1 ||
        fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      return -1;
    }
    size_t len = (size_t)end;
    char *data = malloc(len + 1);
    if (!data) {
      fclose(file);
      return -1;
    }
    bool read_ok = fread(data, 1, len, file) == len;
    fclose(file);
    if (!read_ok) {
      free(data);
      return -1;
    }
    data[len] = '\0';
    const char *sym = nyr_strtab_intern(data, len, NULL, 0);
    free(data);
    if (!sym)
      return -1;
    int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .symbol = sym});
    int length = nyr_emit_const(b, (int64_t)len);
    int tag = nyr_emit_const(b, TAG_STR_CONST);
    if (addr < 0 || length < 0 || tag < 0)
      return -1;
    nyr_record_dyn_fact(b, addr, NYR_FACT_DYN_STR_LEN, length);
    nyr_record_dyn_fact(b, addr, NYR_FACT_DYN_TAG, tag);
    return addr;
  }
  case NY_E_IDENT: {
    nyr_local_t *l =
        nyr_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!l && !b->exact_syntax_ctx)
      l = nyr_find_local_nearest(b, e->as.ident.name);
    /* Resolve enum constants before looking at top-level cells.  Enums are
     * sometimes also represented by compatibility globals (especially after
     * module flattening); those globals may be uninitialized in native-only
     * lowering and shadow the enum's source-level discriminant.  A lexical
     * local still wins, as usual. */
    if (!l) {
      const stmt_t *enum_stmt = NULL;
      const stmt_enum_item_t *enum_item = NULL;
      int64_t enum_value = 0;
      if (nyr_find_enum_member(b, e->as.ident.name, &enum_stmt, &enum_item,
                               &enum_value) &&
          enum_item && enum_item->fields.len == 0)
        return nyr_emit_enum_tag(b, enum_stmt, enum_value);
    }
    /* A mutable top-level binding is a cell, not a compile-time alias for its
     * initializer.  The fallback resolver below intentionally folds `def`
     * constants, but doing that for `mut s = 100; s += 1; print(s)` reloads
     * the initializer (100) after the store of 101.  Resolve the cell before
     * any initializer folding so entry code and functions observe updates. */
    if (!l && e->as.ident.name) {
      const stmt_t *global_stmt = NULL;
      if (b->profile_name && strcmp(b->profile_name, "rt_main") == 0 &&
          (!b->module_name || !b->module_name[0]) && b->source_file &&
          b->prog) {
        global_stmt = nyr_find_top_level_var_in_source_in_prog(
            b, b->prog, e->as.ident.name, b->source_file);
      } else {
        global_stmt = nyr_find_top_level_var_stmt(b, e->as.ident.name);
      }
      if (global_stmt && global_stmt->kind == NY_S_VAR &&
          global_stmt->as.var.is_mut) {
        const char *global_name = nyr_global_symbol(b, e->as.ident.name);
        if (global_name) {
          int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .imm = 0,
                                                       .symbol = global_name});
          if (addr < 0)
            return -1;
          int value = nyr_load_global_scalar(b, addr, global_name, e);
          nyr_record_global_dyn_tag(b, global_name, value);
          return value;
        }
      }
    }
    if (!l && b->current_fn_name) {
      const char *dot = strrchr(b->current_fn_name, '.');
      if (dot && dot[1]) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                         (int)(dot - b->current_fn_name), b->current_fn_name,
                         e->as.ident.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          const char *global_name = nyr_globaltab_name(qualified);
          if (global_name) {
            int addr =
                nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .imm = 0,
                                                  .symbol = global_name});
            if (addr < 0)
              return -1;
            int value = nyr_load_global_scalar(b, addr, global_name, e);
            nyr_record_global_dyn_tag(b, global_name, value);
            return value;
          }
        }
      }
    }
    if (!l && b->module_name && b->module_name[0]) {
      const stmt_t *owner =
          nyr_find_module_walk_in_prog(b, b->prog, b->module_name);
      const expr_t *local_global =
          nyr_find_module_member_in(b, owner, e->as.ident.name, 0);
      if (local_global && local_global != e && b->resolve_depth < 64) {
        int64_t constant_value = 0;
        if (nyr_fold_top_level_int_in_module(
                b, NULL, b->module_name, local_global, &constant_value, 0)) {
          return nyr_emit_const(b, constant_value);
        }
        ++b->resolve_depth;
        int r = nyr_lower_expr(b, local_global);
        --b->resolve_depth;
        return r;
      }
      char qualified[512];
      int n = snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name,
                       e->as.ident.name);
      const char *global_name = n > 0 && (size_t)n < sizeof(qualified)
                                    ? nyr_globaltab_name(qualified)
                                    : NULL;
      if (global_name) {
        int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = global_name});
        if (addr < 0)
          return -1;
        int value = nyr_load_global_scalar(b, addr, global_name, e);
        nyr_record_global_dyn_tag(b, global_name, value);
        return value;
      }
    }
    /* A same-source function definition shadows foreign globals and
     * top-level values when referenced as a value (notably a named callback
     * such as `filter(p)`).  Imported `#main`/self-test blocks register
     * short bindings that would otherwise capture the reference and emit
     * the foreign initializer with out-of-module lambdas. */
    if (!l && e->as.ident.name && nyr_user_defined_fn(b, e->as.ident.name)) {
      char function_symbol[512];
      const stmt_t *callback_fn = nyr_find_user_function(b, e->as.ident.name);
      const char *callback_name = callback_fn && callback_fn->as.fn.name
                                      ? callback_fn->as.fn.name
                                      : e->as.ident.name;
      int symbol_len = snprintf(function_symbol, sizeof(function_symbol),
                                "ny_fn_%s", callback_name);
      if (symbol_len < 0 || (size_t)symbol_len >= sizeof(function_symbol)) {
        return nyr_fail(b, "native NYIR lower: function symbol too long"), -1;
      }
      int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = function_symbol});
      if (addr < 0) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      return addr;
    }
    /* Prefer an exact global registration over a qualified tail match.  The
     * main program can legitimately define a name such as `d` while a
     * loaded stdlib module also exports `module.d`; resolving the tail first
     * silently redirects the user's local/global reference to the module
     * object. */
    if (!l && nyr_globaltab_has(e->as.ident.name)) {
      /* A bare global registration can originate from an imported #main
       * block.  In the root program that short name must not capture an
       * unresolved read (or an enum member) from the user's source.  Keep
       * exact bare globals for the current module and for declarations whose
       * token belongs to the source currently being lowered. */
      const expr_t *source_global =
          b->source_file ? nyr_find_top_level_value_in_source(
                               b, e->as.ident.name, b->source_file)
                         : NULL;
      bool allow_bare_global =
          (b->module_name && b->module_name[0]) || source_global != NULL;
      if (!allow_bare_global)
        goto skip_exact_global;
      const char *global_name = nyr_globaltab_name(e->as.ident.name);
      int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = global_name});
      if (addr < 0)
        return -1;
      int value = nyr_load_global_scalar(b, addr, global_name, e);
      nyr_record_global_dyn_tag(b, global_name, value);
      return value;
    }
  skip_exact_global:
    if (!l && e->as.ident.name &&
        nyr_find_imported_value(b, e->as.ident.name)) {
      const char *global_name = nyr_global_symbol(b, e->as.ident.name);
      if (global_name) {
        int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = global_name});
        if (addr < 0)
          return -1;
        int value = nyr_load_global_scalar(b, addr, global_name, e);
        nyr_record_global_dyn_tag(b, global_name, value);
        return value;
      }
    }
    if (!l && (b->module_name && b->module_name[0])) {
      const char *global_name = nyr_globaltab_name_tail(e->as.ident.name);
      if (global_name) {
        int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = global_name});
        if (addr < 0)
          return -1;
        int value = nyr_load_global_scalar(b, addr, global_name, e);
        nyr_record_global_dyn_tag(b, global_name, value);
        return value;
      }
    }
    if (!l) {
      /* In the root program, only declarations from the current source are
       * valid bare values.  The all-program lookup also sees imported
       * #main/self-test locals; lowering one of those as a fallback can
       * produce a null initializer (and, for calls, a runtime segfault). */
      const expr_t *global =
          (b->module_name && b->module_name[0])
              ? nyr_find_top_level_value(b, e->as.ident.name)
              : (b->source_file ? nyr_find_top_level_value_in_source(
                                      b, e->as.ident.name, b->source_file)
                                : NULL);
      if (!global && (!b->module_name || !b->module_name[0]))
        global = nyr_find_imported_value(b, e->as.ident.name);
      /* Module-lowered code also reads imported defs by bare leaf name
       * (core.os.ui.render.gl.state reading gl.constants._OFF_U2 through its
       * use chain).  The resolver is memoized and owner-module-scoped, so
       * consulting it here is cheap and cannot shadow the local/owner
       * lookups above. */
      if (!global)
        global = nyr_find_imported_value(b, e->as.ident.name);
      /*
       * Stdlib module defs are stored with a qualified name
       * (core.math.big.BF_PRECISION); a bare intra-module reference only
       * carries the leaf.  Resolve against the current module's namespace
       * first (derived from the qualified current_fn_name) so module-local
       * constants (both compile-time and runtime-computed defs) resolve.
       */
      if (!global && b->current_fn_name) {
        const char *dot = strrchr(b->current_fn_name, '.');
        if (dot && dot != b->current_fn_name && dot[1]) {
          const char *qualified = nyr_builder_own_qualified_prefix(
              b, b->current_fn_name, (size_t)(dot - b->current_fn_name),
              e->as.ident.name);
          if (qualified)
            global = nyr_find_top_level_value(b, qualified);
        }
      }
      if (global && global != e && global->kind != NY_E_IDENT) {
        if (b->resolve_depth >= 64) {
          global = NULL; /* over-deep/cyclic def chain: stop recursing */
        } else {
          ++b->resolve_depth;
          int r = nyr_lower_expr(b, global);
          --b->resolve_depth;
          return r;
        }
      }
    }
    if (!l) {
      bool platform_value = false;
      if (ny_native_target_platform_ident(b->options, e->as.ident.name,
                                          &platform_value))
        return nyr_emit_const(b, platform_value ? 1 : 0);
      const char *platform = nyr_platform_string(b->options, e->as.ident.name);
      if (platform) {
        const char *sym =
            nyr_strtab_intern(platform, strlen(platform), NULL, 0);
        int addr =
            sym ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                    .dst = -1,
                                                    .a = -1,
                                                    .b = -1,
                                                    .imm = 0,
                                                    .symbol = sym})
                : -1;
        int length = nyr_emit_const(b, (int64_t)strlen(platform));
        int tag = nyr_emit_const(b, 121);
        if (addr < 0 || length < 0 || tag < 0) {
          return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
        }
        if (!nyr_record_dyn_fact(b, addr, NYR_FACT_DYN_STR_LEN, length) ||
            !nyr_record_dyn_fact(b, addr, NYR_FACT_DYN_TAG, tag))
          return -1;
        return addr;
      }
    }
    if (!l) {
      if (b->module_name && b->module_name[0]) {
        char qualified[512];
        int qn = snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name,
                          e->as.ident.name);
        int64_t qualified_constant = 0;
        if (qn > 0 && (size_t)qn < sizeof(qualified) &&
            nyr_consttab_get(qualified, &qualified_constant)) {
          return nyr_emit_const(b, qualified_constant);
        }
      }
      int64_t imported_constant = 0;
      /*
       * Imported module constants are stored under their qualified symbol
       * names, while references inside a module use the imported leaf.
       * Fold an unambiguous qualified suffix before treating it as a symbol.
       */
      if (b->externs &&
          ny_native_c_define_lookup(&b->externs->defines, e->as.ident.name,
                                    &imported_constant))
        return nyr_emit_const(b, imported_constant);
      if (nyr_consttab_get_tail(e->as.ident.name, &imported_constant)) {
        return nyr_emit_const(b, imported_constant);
      }
      const stmt_t *adt_enum = NULL;
      const stmt_enum_item_t *adt_item = NULL;
      int64_t adt_tag = 0;
      if (nyr_find_enum_member(b, e->as.ident.name, &adt_enum, &adt_item,
                               &adt_tag)) {
        if (adt_item && adt_item->fields.len == 0)
          return nyr_emit_enum_tag(b, adt_enum, adt_tag);
      }
    }
    /* The tuple type can occur as value-level type evidence in generated
     * stdlib code.  It is a runtime tag, not a linkable global. */
    if (!l && e->as.ident.name && strcmp(e->as.ident.name, "tuple") == 0)
      return nyr_emit_const(b, rt_runtime_tag_raw_name(
                                   e->as.ident.name, strlen(e->as.ident.name)));
    /* Exported error/warning kinds are immutable string definitions.  When an
     * imported module loses its qualified def during native collection, keep
     * the value semantics instead of emitting a bogus data relocation. */
    if (!l && e->as.ident.name &&
        (strncmp(e->as.ident.name, "ERR", 3) == 0 ||
         strncmp(e->as.ident.name, "WARN", 4) == 0)) {
      const char *p =
          e->as.ident.name + (strncmp(e->as.ident.name, "ERR", 3) == 0 ? 3 : 4);
      char text[128];
      size_t n = 0;
      bool is_error_name = e->as.ident.name[0] == 'E';
      if (*p == '_')
        ++p;
      /* The bare exported sentinels are `ERR`/`WARN`, not one-character
       * strings.  The suffixed constants use the compact symbolic spelling
       * (`ERR_TYPE` -> `err.type`). */
      if (!*p) {
        const char *base = is_error_name ? "err" : "warn";
        size_t base_len = is_error_name ? 3u : 4u;
        memcpy(text, base, base_len);
        n = base_len;
      } else {
        text[n++] = is_error_name ? 'e' : 'w';
      }
      for (; *p && n + 1 < sizeof(text); ++p)
        text[n++] = *p == '_' ? '.' : (char)tolower((unsigned char)*p);
      text[n] = '\0';
      const char *sym = nyr_strtab_intern(text, n, NULL, 0);
      int addr = sym ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                         .dst = -1,
                                                         .a = -1,
                                                         .b = -1,
                                                         .symbol = sym})
                     : -1;
      return addr < 0 ? -1 : addr;
    }
    if (!l) {
      char function_symbol[512];
      const stmt_t *referenced_fn = nyr_find_user_function(b, e->as.ident.name);
      if (!referenced_fn)
        referenced_fn = nyr_find_imported_function(b, e->as.ident.name);
      const char *leaf = ny_name_leaf(e->as.ident.name);
      if (leaf && strcmp(leaf, "zalloc") == 0) {
        int addr =
            nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .imm = 0,
                                              .symbol = "rt_zalloc_raw"});
        if (addr < 0)
          nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
        return addr;
      }
      bool compiled_fn =
          referenced_fn && !referenced_fn->as.fn.is_extern &&
          !referenced_fn->as.fn.link_name &&
          ny_native_runtime_symbol_for_expr(e->as.ident.name, leaf, e) ==
              NULL &&
          (!leaf || ny_native_runtime_symbol_for_expr(NULL, leaf, e) == NULL) &&
          (!leaf || ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_NONE) &&
          (!leaf || ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE);
      bool user_function =
          nyr_user_defined_fn(b, e->as.ident.name) || compiled_fn;
      /* core.math exports these immutable scalar constants.  When a root
       * program imports the module with `use core.math`, the expanded module
       * AST may not retain a materializable global symbol; preserve the
       * language-level constants at the native boundary. */
      if (!referenced_fn && !user_function && e->as.ident.name) {
        double constant = 0.0;
        bool known = true;
        if (strcmp(e->as.ident.name, "PI") == 0)
          constant = 3.14159265358979323846;
        else if (strcmp(e->as.ident.name, "PHI") == 0)
          constant = 1.61803398874989484820;
        else if (strcmp(e->as.ident.name, "E") == 0)
          constant = 2.71828182845904523536;
        else if (strcmp(e->as.ident.name, "TAU") == 0)
          constant = 6.28318530717958647692;
        else if (strcmp(e->as.ident.name, "LN2") == 0)
          constant = 0.69314718055994530941;
        else if (strcmp(e->as.ident.name, "LN10") == 0)
          constant = 2.30258509299404568402;
        else
          known = false;
        if (known)
          return nyr_emit_const_f64(b, constant);
      }
      if (!referenced_fn && !user_function &&
          ny_native_runtime_symbol_for_expr(e->as.ident.name, leaf, e) ==
              NULL &&
          (!leaf || ny_native_runtime_symbol_for_expr(NULL, leaf, e) == NULL) &&
          (!leaf || ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_NONE) &&
          (!leaf || ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE)) {
        return nyr_fail(b, "undefined symbol '%s'", e->as.ident.name), -1;
      }
      const char *symbol = e->as.ident.name;
      if (compiled_fn && referenced_fn->as.fn.name)
        symbol = referenced_fn->as.fn.name;
      if (e->semantic.canonical_callee && e->semantic.canonical_callee_stmt &&
          e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
          ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok))
        symbol = e->semantic.canonical_callee;
      if (user_function) {
        int n = snprintf(function_symbol, sizeof(function_symbol), "ny_fn_%s",
                         symbol);
        if (n < 0 || (size_t)n >= sizeof(function_symbol)) {
          return nyr_fail(b, "native NYIR lower: function symbol too long"), -1;
        }
        symbol = function_symbol;
      }
      int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = symbol});
      if (addr < 0) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      /*
       * A top-level def constant registered in the consttab is stored as an
       * 8-byte .data definition: a value-context reference loads it rather
       * than returning the symbol's address (R3).
       */
      if (nyr_consttab_has(e->as.ident.name))
        return nyr_emit_load_i64(b, addr);
      return addr;
    }
    /*
     * Any ordinary read can create or observe an alias; only the exact
     * loop-local promotion path below may consume a candidate without this.
     */
    if (!l) {
      return nyr_fail(b, "undefined symbol '%s'", e->as.ident.name), -1;
    }
    l->sb_candidate = false;
    int val = nyr_load_local_value(b, l->slot);
    /* LOAD_LOCAL does not carry the declared floating slot class by itself.
     * Preserve the local's established storage ABI on the producer so a
     * typed-float consumer cannot mistake a native F64 load for tagged `any`
     * and decode it again. */
    if (val >= 0 && (l->is_f64 || l->is_f32) && b->nyir.len > 0) {
      nyr_inst_t *producer = &b->nyir.data[b->nyir.len - 1];
      if (producer->dst == val && producer->op == NYIR_LOAD_LOCAL &&
          producer->imm == l->slot)
        producer->scalar_class = l->is_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
    }
    if (val >= 0 && l->semantic_rep == NY_SEM_REP_POINTER && b->nyir.len > 0) {
      nyr_inst_t *producer = &b->nyir.data[b->nyir.len - 1];
      if (producer->dst == val && producer->op == NYIR_LOAD_LOCAL &&
          producer->imm == l->slot)
        producer->scalar_class = NYIR_SCALAR_POINTER;
    }
    if (val >= 0) {
      if (l->list_len_slot >= 0) {
        int llen = nyr_load_local_value(b, l->list_len_slot);
        if (llen >= 0)
          nyr_record_list_len_fact(b, val, llen);
      }
      if (l->dyn_str_len_slot >= 0) {
        int slen = nyr_load_local_value(b, l->dyn_str_len_slot);
        if (slen >= 0)
          nyr_record_dyn_fact(b, val, NYR_FACT_DYN_STR_LEN, slen);
      }
      if (l->dyn_tag_slot >= 0) {
        int stag = nyr_load_local_value(b, l->dyn_tag_slot);
        if (stag >= 0)
          nyr_record_dyn_fact(b, val, NYR_FACT_DYN_TAG, stag);
      }
    }
    return val;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right) {
      return nyr_fail(b, "native NYIR lower: malformed unary"), -1;
    }
    /* The positive magnitude of INT64_MIN cannot itself fit in i64. The
     * parser keeps it as an out-of-range literal so it can also represent
     * bigint values; when unary minus is applied to the unsuffixed decimal
     * magnitude, lower the signed result directly instead of negating a
     * saturated INT64_MAX field or round-tripping through bigint conversion. */
    const expr_t *unary_arg = e->as.unary.right;
    if (strcmp(e->as.unary.op, "-") == 0 && unary_arg->kind == NY_E_LITERAL &&
        unary_arg->as.literal.kind == NY_LIT_INT &&
        unary_arg->as.literal.integer_outside_i64 &&
        !unary_arg->as.literal.hint_explicit &&
        unary_arg->as.literal.numeric_text &&
        unary_arg->as.literal.numeric_len == 19 &&
        memcmp(unary_arg->as.literal.numeric_text, "9223372036854775808", 19) ==
            0)
      return nyr_emit_const(b, INT64_MIN);
    /* `async f(a, b)` is a task constructor, not an ordinary unary
     * operation.  Keep the callback address and its raw native arguments in
     * a heap vector: the scheduler copies that vector before the lowering
     * frame can disappear.  The high bit on argc selects the raw callback
     * dispatcher (native function addresses must never go through the tagged
     * callable dispatcher). */
    if (strcmp(e->as.unary.op, "async") == 0) {
      const expr_t *task = e->as.unary.right;
      const expr_t *callee = task;
      const call_arg_t *async_args = NULL;
      size_t argc = 0;
      if (task->kind == NY_E_CALL) {
        callee = task->as.call.callee;
        async_args = task->as.call.args.data;
        argc = task->as.call.args.len;
      } else if (task->kind == NY_E_MEMCALL) {
        return nyr_fail(b, "native NYIR lower: async member calls should be "
                           "wrapped in a function or lambda"),
               -1;
      }
      if (!callee || argc > 15) {
        return nyr_fail(b, "native NYIR lower: async requires a callable "
                           "with at most 15 arguments"),
               -1;
      }
      const stmt_t *callback = NULL;
      if (callee->kind == NY_E_FN || callee->kind == NY_E_LAMBDA) {
        ny_native_lambda_entry_t *entry =
            ny_native_lambda_find(ny_native_lambda_cache_for(b), callee);
        callback = entry ? entry->fn : NULL;
      } else if (callee->kind == NY_E_IDENT && callee->as.ident.name) {
        callback = nyr_find_user_function(b, callee->as.ident.name);
      }
      call_arg_t expanded_args[15] = {0};
      if (callback && callback->kind == NY_S_FUNC) {
        const ny_param_list *params = &callback->as.fn.params;
        if (params->len > 15 || argc > params->len)
          return nyr_fail(b,
                          "native NYIR lower: async call to '%s' has "
                          "an unsupported argument count",
                          callee->kind == NY_E_IDENT && callee->as.ident.name
                              ? callee->as.ident.name
                              : "<callable>"),
                 -1;
        for (size_t i = 0; i < argc; ++i) {
          if (async_args[i].name)
            return nyr_fail(b, "native NYIR lower: async call syntax does "
                               "not support named arguments"),
                   -1;
          expanded_args[i] = async_args[i];
        }
        for (size_t i = argc; i < params->len; ++i) {
          expr_t *default_expr = params->data[i].def;
          if (!default_expr)
            return nyr_fail(b,
                            "native NYIR lower: async call to '%s' "
                            "requires argument %zu",
                            callee->kind == NY_E_IDENT && callee->as.ident.name
                                ? callee->as.ident.name
                                : "<callable>",
                            i + 1),
                   -1;
          for (size_t j = 0; j < i; ++j) {
            const char *formal = params->data[j].name;
            if (formal && expanded_args[j].val)
              default_expr = ny_expr_subst(&b->default_arg_arena, default_expr,
                                           formal, expanded_args[j].val);
          }
          expanded_args[i].val = default_expr;
        }
        async_args = expanded_args;
        argc = params->len;
      }
      int fn = nyr_lower_expr(b, callee);
      if (fn < 0)
        return -1;
      int argv = nyr_emit_const(b, 0);
      if (argc > 0) {
        int bytes = nyr_emit_const(b, (int64_t)(argc * sizeof(int64_t)));
        argv = bytes < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_zalloc_raw", bytes, -1,
                                                 -1, 1, 0);
        if (argv < 0)
          return -1;
        for (size_t i = 0; i < argc; ++i) {
          if (async_args[i].name) {
            return nyr_fail(b, "native NYIR lower: async call syntax does "
                               "not support named arguments"),
                   -1;
          }
          int arg = nyr_lower_expr(b, async_args[i].val);
          /* rt_store64_idx takes a byte offset, not an element index. */
          int index = nyr_emit_const(b, (int64_t)(i * sizeof(int64_t)));
          if (arg < 0 || index < 0 ||
              nyr_emit_runtime_call(b, "rt_store64_idx", argv, index, arg, 3,
                                    0) < 0)
            return -1;
        }
      }
      /* The scheduler calls native code directly, but its result may already
       * be in the tagged any ABI.  Carry the declared return representation
       * and raw-code-pointer representation as separate ABI values. */
      int result_kind = nyr_fn_result_kind(b, callback);
      bool dynamic_callback = false;
      if (callback && callback->kind == NY_S_FUNC) {
        for (size_t i = 0; i < callback->as.fn.params.len && i < argc; ++i) {
          const param_t *param = &callback->as.fn.params.data[i];
          /* A resolved TAGGED_DYNAMIC fact also selects the dynamic callback
           * dispatcher when it agrees with the declared spelling. */
          bool param_is_dynamic =
              param->type
                  ? ny_native_type_name_is_any(param->type) ||
                        (param->semantic.resolved &&
                         param->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC &&
                         ny_sem_rep_coherent_with_type(param->type,
                                                       param->semantic.rep))
                  : !nyr_untyped_param_is_raw(callback, param->name);
          if (param_is_dynamic) {
            dynamic_callback = true;
            break;
          }
        }
      }
      if (dynamic_callback) {
        const char *marker =
            result_kind == 0
                ? "rt_mark_dynamic_callable_tagged_word_args_raw_result"
            : result_kind == 2
                ? "rt_mark_dynamic_bool_callable_tagged_word_args"
                : "rt_mark_dynamic_callable_tagged_word_args";
        fn = nyr_emit_runtime_call(b, marker, fn, -1, -1, 1, 0);
        if (fn < 0)
          return -1;
      }
      /* RT_DEF callback APIs take raw C ABI counts, like their argc/argv
       * companion thread entry points. Do not tag `argc` here: odd counts
       * would be indistinguishable from already tagged integers at runtime. */
      int argc_value = nyr_emit_const(b, (int64_t)argc);
      int raw_fn = nyr_emit_const(
          b,
          rt_tag_v((dynamic_callback || callee->kind == NY_E_LAMBDA) ? 0 : 1));
      int tagged_result_value = nyr_emit_const(b, rt_tag_v(result_kind));
      if (argc_value < 0 || raw_fn < 0 || tagged_result_value < 0)
        return -1;
      return nyr_emit_runtime_call4(b, "rt_async_task_new_abi", fn, argc_value,
                                    argv, raw_fn, tagged_result_value, 5, 0);
    }
    /* Keep unary-minus precedence over integer power independent of the
     * enclosing consumer ABI.  Calls such as `assert_eq(-2^2, -4)` request a
     * dynamic argument, which otherwise forces the integer power through the
     * boxed `pow` helper before unary lowering and turns subtraction into
     * arithmetic on a heap handle. */
    if (strcmp(e->as.unary.op, "-") == 0 &&
        e->as.unary.right->kind == NY_E_BINARY &&
        e->as.unary.right->as.binary.op &&
        strcmp(e->as.unary.right->as.binary.op, "**") == 0) {
      int64_t power = 0;
      if (nyr_fold_const_pow(e->as.unary.right->as.binary.left,
                             e->as.unary.right->as.binary.right, &power) &&
          power != INT64_MIN && power <= (INT64_C(1) << 62) - 1 &&
          power >= -(INT64_C(1) << 62) + 1)
        return nyr_emit_const(b, -power);
    }
    int rv = nyr_lower_expr(b, e->as.unary.right);
    if (rv < 0)
      return -1;
    if (strcmp(e->as.unary.op, "await") == 0)
      return nyr_emit_runtime_call(b, "rt_async_await_blocking", rv, -1, -1, 1,
                                   0);
    if (strcmp(e->as.unary.op, "+") == 0)
      return rv;
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (nyr_expr_is_bigint(b, e->as.unary.right))
        return nyr_emit_runtime_call(b, "rt_bigint_neg_raw", rv, -1, -1, 1, 0);
      /* Keep values in the native scalar domain only while they fit the
       * language's signed small-int range. */
      int64_t known = 0;
      if (nyr_eval_proof_i64(b, e->as.unary.right, 0, &known) &&
          known != INT64_MIN && known <= (INT64_C(1) << 62) - 1 &&
          known >= -(INT64_C(1) << 62) + 1)
        return nyr_emit_const(b, -known);
      if (nyr_expr_is_f32(b, e->as.unary.right)) {
        /* A literal folds to the negated constant: `0.0f - v` is IEEE
         * +0.0 for v = 0.0, so `-0.0f` would lose its sign bit (the
         * scalar-classes -0.0 contract). */
        const expr_t *operand = e->as.unary.right;
        if (operand->kind == NY_E_LITERAL &&
            operand->as.literal.kind == NY_LIT_FLOAT) {
          int negated = nyr_emit_const_f32(b, (float)-operand->as.literal.as.f);
          if (negated < 0)
            return -1;
          return negated;
        }
        int zero = nyr_emit_const_f32(b, 0.0f);
        if (zero < 0)
          return -1;
        return nyr_ir_emit(
            &b->nyir,
            (nyr_inst_t){.op = NYIR_SUB_F32, .dst = -1, .a = zero, .b = rv});
      }
      if (nyr_expr_is_f64(b, e->as.unary.right)) {
        const expr_t *operand = e->as.unary.right;
        if (operand->kind == NY_E_LITERAL &&
            operand->as.literal.kind == NY_LIT_FLOAT) {
          int negated = nyr_emit_const_f64(b, -operand->as.literal.as.f);
          if (negated < 0)
            return -1;
          return negated;
        }
        int zero = nyr_emit_const_f64(b, 0.0);
        if (zero < 0)
          return -1;
        return nyr_ir_emit(
            &b->nyir,
            (nyr_inst_t){.op = NYIR_SUB_F64, .dst = -1, .a = zero, .b = rv});
      }
      int zero = nyr_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = zero, .b = rv});
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      if (nyr_expr_is_f64(b, e->as.unary.right)) {
        int zero = nyr_emit_const_f64(b, 0.0);
        if (zero < 0)
          return -1;
        return nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                 .dst = -1,
                                                 .a = rv,
                                                 .b = zero,
                                                 .cmp = NYIR_CMP_EQ});
      }
      int truthy = nyr_emit_truthy(b, rv);
      return truthy < 0 ? -1 : nyr_emit_is_zero(b, truthy);
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      if (nyr_expr_is_bigint(b, e->as.unary.right) ||
          nyr_expr_is_any(b, e->as.unary.right))
        return nyr_emit_runtime_call(b, "rt_not", rv, -1, -1, 1, 0);
      int mask = nyr_emit_const(b, -1);
      if (mask < 0)
        return -1;
      return nyr_ir_emit(
          &b->nyir,
          (nyr_inst_t){.op = NYIR_XOR_I64,
                        .dst = -1,
                        .a = rv,
                        .b = mask,
                        /* This branch is selected only for a proven raw
                         * scalar operand.  Keep its physical ABI explicit;
                         * an `any` use-site annotation must not turn the
                         * raw zero result of ~(-1) into tagged nil. */
                        .scalar_class = NYIR_SCALAR_I64_RAW});
    }
    return nyr_fail(b, "native NYIR lower: unsupported unary operator '%s'",
                    e->as.unary.op),
           -1;
  }
  case NY_E_BINARY:
    return nyr_lower_binary(b, e);
  case NY_E_LOGICAL: {
    if (!e->as.logical.op || (strcmp(e->as.logical.op, "&&") != 0 &&
                              strcmp(e->as.logical.op, "||") != 0)) {
      return nyr_fail(b, "native NYIR lower: unsupported logical operator '%s'",
                      e->as.logical.op ? e->as.logical.op : "(null)"),
             -1;
    }
    return nyr_lower_logical(b, e->as.logical.left, e->as.logical.right,
                             strcmp(e->as.logical.op, "||") == 0);
  }
  case NY_E_TERNARY:
    if (e->as.ternary.nil_coalesce)
      return nyr_lower_coalesce(b, e, e->as.ternary.true_expr,
                                e->as.ternary.false_expr);
    return nyr_lower_ternary(b, e->as.ternary.cond, e->as.ternary.true_expr,
                             e->as.ternary.false_expr);
  case NY_E_COMPTIME: {
    const stmt_t *body = e->as.comptime_expr.body;
    /* `#run` and `comptime` share the bounded interpreter. Evaluate supported
     * forms before ordinary lowering, which would otherwise execute them at
     * runtime. Keep the legacy fallback for forms outside its value domain. */
    if (body) {
      ny_ct_fast_val_t value = ny_ct_fast_none();
      if (ny_try_eval_comptime_interp_program((program_t *)b->prog,
                                              (stmt_t *)body, &value)) {
        int lowered = -1;
        switch (value.kind) {
        case NY_CT_FAST_NONE:
          lowered = nyr_emit_const(b, 0);
          break;
        case NY_CT_FAST_BOOL:
          lowered = nyr_emit_const(
              b, e->semantic.resolved &&
                         e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC
                     ? (value.b ? NY_IMM_TRUE : NY_IMM_FALSE)
                     : (value.b ? 1 : 0));
          break;
        case NY_CT_FAST_INT:
          lowered = nyr_emit_const(b, value.i);
          bool comptime_int_is_tagged =
              e->semantic.resolved &&
              e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
          if (lowered >= 0 && comptime_int_is_tagged)
            lowered = nyr_emit_runtime_call(b, "rt_box_i64", lowered, -1, -1, 1,
                                            NYIR_SCALAR_I64_TAGGED);
          /* The interpreter returns a sequence `.get` as a dynamic value.
           * Preserve that tagged ABI even though its compile-time payload is
           * represented by NY_CT_FAST_INT internally. */
          {
            const expr_t *result = nyr_comptime_result_expr(body);
            const expr_t *target = nyr_comptime_sequence_get_target(result);
            if (target &&
                (nyr_expr_is_list(b, target) || nyr_expr_is_tuple(b, target)) &&
                !nyr_expr_is_dyn_list(b, target) &&
                ny_small_int_fits_i64(value.i) && !comptime_int_is_tagged)
              lowered =
                  nyr_emit_runtime_call(b, "rt_tag", lowered, -1, -1, 1, 0);
          }
          break;
        case NY_CT_FAST_FLOAT:
          lowered = nyr_emit_const_f64(b, value.f64);
          break;
        case NY_CT_FAST_STR:
          lowered = nyr_emit_cstr_const(b, value.s ? value.s : "");
          break;
        default:
          break;
        }
        ny_ct_fast_val_free(&value);
        if (lowered >= 0)
          return lowered;
      } else {
        ny_ct_fast_val_free(&value);
      }
    }
    if (body) {
      if (body->kind == NY_S_BLOCK) {
        int comptime_last_value = -1;
        for (size_t i = 0; i < body->as.block.body.len; ++i) {
          const stmt_t *item = body->as.block.body.data[i];
          if (!item)
            continue;
          if (item->kind == NY_S_RETURN && item->as.ret.value) {
            const expr_t *value = item->as.ret.value;
            bool saved_dynamic_get = b->force_dynamic_get_any;
            if (value->kind == NY_E_MEMCALL && value->as.memcall.name &&
                strcmp(value->as.memcall.name, "get") == 0 &&
                nyr_expr_is_list(b, value->as.memcall.target) &&
                !nyr_expr_is_dyn_list(b, value->as.memcall.target))
              b->force_dynamic_get_any = true;
            int lowered = nyr_lower_expr(b, value);
            b->force_dynamic_get_any = saved_dynamic_get;
            return lowered;
          }
          if (item->kind == NY_S_EXPR && item->as.expr.expr) {
            int v = nyr_lower_expr(b, item->as.expr.expr);
            if (v >= 0)
              comptime_last_value = v;
          } else {
            if (!nyr_lower_stmt(b, item))
              return -1;
          }
        }
        if (comptime_last_value >= 0)
          return comptime_last_value;
        return nyr_emit_const(b, 0);
      }
      if (body->kind == NY_S_RETURN && body->as.ret.value) {
        const expr_t *value = body->as.ret.value;
        bool saved_dynamic_get = b->force_dynamic_get_any;
        if (value->kind == NY_E_MEMCALL && value->as.memcall.name &&
            strcmp(value->as.memcall.name, "get") == 0 &&
            nyr_expr_is_list(b, value->as.memcall.target) &&
            !nyr_expr_is_dyn_list(b, value->as.memcall.target))
          b->force_dynamic_get_any = true;
        int lowered = nyr_lower_expr(b, value);
        b->force_dynamic_get_any = saved_dynamic_get;
        return lowered;
      }
      if (body->kind == NY_S_EXPR && body->as.expr.expr)
        return nyr_lower_expr(b, body->as.expr.expr);
      if (!nyr_lower_stmt(b, body))
        return -1;
      return b->last_value >= 0 ? b->last_value : nyr_emit_const(b, 0);
    }
    bool value = false;
    if (nyr_target_eval_bool(NULL, e, &value))
      return nyr_emit_const(b, value ? 1 : 0);
    return nyr_emit_const(b, 0);
  }
  case NY_E_QUOTE: {
    uint32_t prev_ctx = b->current_syntax_ctx;
    if (e->as.quote.syntax_ctx != 0)
      b->current_syntax_ctx = e->as.quote.syntax_ctx;
    int res = -1;
    if (e->as.quote.expr) {
      res = nyr_lower_expr(b, e->as.quote.expr);
    } else if (e->as.quote.body) {
      const stmt_t *body = e->as.quote.body;
      if (body->kind == NY_S_BLOCK) {
        int last_v = -1;
        for (size_t i = 0; i < body->as.block.body.len; ++i) {
          const stmt_t *item = body->as.block.body.data[i];
          if (!item)
            continue;
          if (item->kind == NY_S_RETURN && item->as.ret.value) {
            res = nyr_lower_expr(b, item->as.ret.value);
            break;
          }
          if (item->kind == NY_S_EXPR && item->as.expr.expr) {
            int v = nyr_lower_expr(b, item->as.expr.expr);
            if (v >= 0)
              last_v = v;
          } else {
            if (!nyr_lower_stmt(b, item)) {
              res = -1;
              break;
            }
          }
        }
        if (res < 0)
          res = last_v >= 0 ? last_v : nyr_emit_const(b, 0);
      } else if (body->kind == NY_S_RETURN && body->as.ret.value) {
        res = nyr_lower_expr(b, body->as.ret.value);
      } else if (body->kind == NY_S_EXPR && body->as.expr.expr) {
        res = nyr_lower_expr(b, body->as.expr.expr);
      } else {
        if (!nyr_lower_stmt(b, body))
          res = -1;
        else
          res = b->last_value >= 0 ? b->last_value : nyr_emit_const(b, 0);
      }
    } else {
      res = nyr_emit_const(b, 0);
    }
    b->current_syntax_ctx = prev_ctx;
    return res;
  }
  case NY_E_SPLICE: {
    if (!e->as.splice.expr)
      return nyr_emit_const(b, 0);
    /* A splice keeps the caller's syntax mark. Do not fall back by spelling
     * to a same-named binding introduced by the surrounding quote; that
     * would let quote-local declarations capture the spliced identifier. */
    bool previous_exact_syntax_ctx = b->exact_syntax_ctx;
    b->exact_syntax_ctx = true;
    int value = nyr_lower_expr(b, e->as.splice.expr);
    b->exact_syntax_ctx = previous_exact_syntax_ctx;
    return value;
  }
  case NY_E_EXISTS: {
    ny_ct_fast_val_t fval = ny_ct_fast_none();
    if (ny_try_eval_comptime_expr_fast(NULL, (expr_t *)e, &fval, 0) &&
        fval.kind == NY_CT_FAST_BOOL) {
      return nyr_emit_const(b, fval.b ? 1 : 0);
    }
    return nyr_fail(b, "native NYIR lower: existential refinement "
                       "must be resolvable at compile time"),
           -1;
  }
  case NY_E_MATCH: {
    stmt_t match = {.kind = NY_S_MATCH, .tok = e->tok};
    match.as.match = e->as.match;
    bool saved_match_any = b->match_result_any;
    b->match_result_any = nyr_expr_is_any(b, e);
    if (!nyr_lower_match(b, &match))
      return -1;
    b->match_result_any = saved_match_any;
    /* Match arms already lower through the common value ABI.  Re-tagging the
     * joined result here corrupts pointer/string arms by treating their
     * address as an integer; dynamic consumers decode the arm representation
     * at their own boundary instead. */
    return b->last_value;
  }
  case NY_E_CALL:
    /* The stdlib int(any) compatibility wrapper decodes tagged values into
     * raw i64.  An integer produced by a raw bitwise global is already
     * normalized at that boundary, so lower the value using its physical
     * representation instead of inlining the wrapper and decoding twice. */
    if (e->as.call.args.len == 1 && e->as.call.args.data[0].val &&
        nyr_raw_bitwise_value(b, e->as.call.args.data[0].val, 0) &&
        ny_native_call_leaf(e) && strcmp(ny_native_call_leaf(e), "int") == 0 &&
        e->as.call.args.len == 1 && e->as.call.args.data[0].val) {
      const expr_t *arg_expr = e->as.call.args.data[0].val;
      int value = nyr_lower_expr(b, arg_expr);
      if (value < 0)
        return -1;
      nyr_scalar_class_t cls = nyr_value_scalar_class(b, value);
      if (cls == NYIR_SCALAR_F64)
        return nyr_emit_f64_to_i64(b, value);
      if (cls == NYIR_SCALAR_F32)
        return nyr_emit_f32_to_i64(b, value);
      if (cls == NYIR_SCALAR_I64_RAW)
        return value;
      if (cls == NYIR_SCALAR_I64_TAGGED || nyr_expr_is_any(b, arg_expr))
        return nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1,
                                     NYIR_SCALAR_I64_RAW);
      return value;
    }
    return nyr_lower_call(b, e);
  case NY_E_SIZEOF: {
    const char *type_name = e->as.szof.is_type ? e->as.szof.type_name : NULL;
    if (!type_name)
      type_name = e->as.szof.resolved_type_name;
    if (!type_name && e->as.szof.target &&
        e->as.szof.target->kind == NY_E_IDENT)
      type_name = e->as.szof.target->as.ident.name;
    size_t size = 0;
    size_t align = 0;
    bool resolved = type_name && nyr_primitive_layout(type_name, &size, &align);
    if (!resolved)
      resolved = type_name &&
                 nyr_ast_layout_query(b, type_name, NULL, &size, &align, NULL);
    if (!resolved) {
      return nyr_fail(b, "native NYIR lower: sizeof cannot resolve type '%s'",
                      type_name ? type_name : ""),
             -1;
    }
    (void)align;
    return nyr_emit_const(b, (int64_t)size);
  }
  case NY_E_DEREF: {
    int addr = nyr_lower_expr(b, e->as.deref.target);
    if (addr < 0)
      return -1;
    int v = nyr_ir_emit(
        &b->nyir,
        (nyr_inst_t){
            .op = NYIR_LOAD_I64, .dst = -1, .a = addr, .b = -1, .c = -1});
    if (v < 0)
      nyr_fail(b, "native NYIR lower: deref load failed");
    return v;
  }
  case NY_E_ASM: {
    const char *tpl = e->as.as_asm.code ? e->as.as_asm.code : "";
    const char *cons = e->as.as_asm.constraints ? e->as.as_asm.constraints : "";
    if (e->as.as_asm.args.len == 0) {
      if (!*tpl || strcmp(tpl, "nop") == 0 || strcmp(tpl, "nop;") == 0 ||
          strcmp(tpl, "nop\n") == 0 || strcmp(tpl, "nop;nop") == 0 ||
          strcmp(tpl, "nop;nop;nop") == 0 || strcmp(tpl, "pause") == 0 ||
          strcmp(tpl, "pause;") == 0 || strcmp(tpl, "lfence") == 0 ||
          strcmp(tpl, "mfence") == 0 || strcmp(tpl, "sfence") == 0 ||
          strcmp(tpl, "ud2") == 0 || strcmp(tpl, "yield") == 0 ||
          strcmp(tpl, "wfe") == 0 || strcmp(tpl, "wfi") == 0 ||
          strcmp(tpl, "sev") == 0 || strcmp(tpl, "isb") == 0 ||
          strcmp(tpl, "isb sy") == 0 || strcmp(tpl, "dmb") == 0 ||
          strcmp(tpl, "dmb ish") == 0 || strcmp(tpl, "dmb sy") == 0 ||
          strcmp(tpl, "dsb") == 0 || strcmp(tpl, "dsb ish") == 0 ||
          strcmp(tpl, "dsb sy") == 0 || strcmp(tpl, "xor %eax,%eax") == 0 ||
          strcmp(tpl, "xorl %eax,%eax") == 0 ||
          strcmp(tpl, "xor %%eax,%%eax") == 0 ||
          strcmp(tpl, "xor %rax,%rax") == 0 ||
          strcmp(tpl, "xorq %rax,%rax") == 0)
        return nyr_emit_const(b, 0);
      if (strstr(tpl, "lea") && strstr(tpl, "ret") && b->local_count >= 2) {
        int left = nyr_load_local_value(b, b->locals[0].slot);
        int right = left < 0 ? -1 : nyr_load_local_value(b, b->locals[1].slot);
        return left < 0 || right < 0
                   ? -1
                   : ny_native_asm_emit_binop(b, NYIR_ADD_I64, left, right);
      }
    }
    if (e->as.as_asm.args.len == 1 && strcmp(cons, "=r,r") == 0 &&
        (strcmp(tpl, "mov $1, $0") == 0 || strcmp(tpl, "mov %1, %0") == 0 ||
         strcmp(tpl, "movq $1, $0") == 0 || strcmp(tpl, "movl $1, $0") == 0 ||
         strcmp(tpl, "mov $1,$0") == 0 || strcmp(tpl, "mov %1,%0") == 0))
      return nyr_lower_expr(b, e->as.as_asm.args.data[0]);
    if (e->as.as_asm.args.len == 2 &&
        (strcmp(tpl, "addq $1, $0") == 0 || strcmp(tpl, "add %1, %0") == 0 ||
         strcmp(tpl, "or $0, $1, $2") == 0)) {
      int a = nyr_lower_expr(b, e->as.as_asm.args.data[0]);
      int c = a < 0 ? -1 : nyr_lower_expr(b, e->as.as_asm.args.data[1]);
      if (a < 0 || c < 0)
        return -1;
      return ny_native_asm_emit_binop(
          b, strncmp(tpl, "or ", 3) == 0 ? NYIR_OR_I64 : NYIR_ADD_I64, a, c);
    }
    if (e->as.as_asm.args.len == 2 && strstr(tpl, "lea") && strstr(tpl, "$1") &&
        strstr(tpl, "$2")) {
      int a = nyr_lower_expr(b, e->as.as_asm.args.data[0]);
      int c = a < 0 ? -1 : nyr_lower_expr(b, e->as.as_asm.args.data[1]);
      return a < 0 || c < 0 ? -1
                            : ny_native_asm_emit_binop(b, NYIR_ADD_I64, a, c);
    }
    return nyr_lower_aarch64_asm(b, e);
  }
  case NY_E_FSTRING: {
    /*
     * F-string: concatenate static parts + interpolated values into a
     * runtime C string via rt_cstr_concat.  Static-only f-strings
     * fold into a single interned literal.
     * NOTE: concat chains allocate intermediate C strings via
     * rt_cstr_concat and only the final pointer survives to the
     * caller. This matches the existing string '+' lowering; the
     * intermediates are intentionally not freed on the native-only path.
     */
    const ny_fstring_part_list *parts = &e->as.fstring.parts;
    bool all_static = true;
    size_t total = 0;
    for (size_t i = 0; i < parts->len; ++i) {
      if (parts->data[i].kind != NY_FSP_STR) {
        all_static = false;
        break;
      }
      total += parts->data[i].as.s.len;
    }
    if (all_static) {
      char *joined = malloc(total + 1);
      if (!joined) {
        return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
      }
      size_t off = 0;
      for (size_t i = 0; i < parts->len; ++i) {
        const char *data =
            parts->data[i].as.s.data ? parts->data[i].as.s.data : "";
        memcpy(joined + off, data, parts->data[i].as.s.len);
        off += parts->data[i].as.s.len;
      }
      joined[off] = '\0';
      const char *sym = nyr_strtab_intern(joined, off, NULL, 0);
      free(joined);
      if (!sym) {
        return nyr_fail(b, "native NYIR lower: string table full or OOM"), -1;
      }
      int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = sym});
      if (addr < 0)
        nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
      return addr;
    }
    int acc = -1;
    for (size_t i = 0; i < parts->len; ++i) {
      int part_val = -1;
      if (parts->data[i].kind == NY_FSP_STR) {
        const char *s =
            parts->data[i].as.s.data ? parts->data[i].as.s.data : "";
        const char *sym =
            nyr_strtab_intern(s, parts->data[i].as.s.len, NULL, 0);
        if (!sym) {
          return nyr_fail(b, "native NYIR lower: string table full or OOM"), -1;
        }
        part_val = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = sym});
      } else {
        const expr_t *sub = parts->data[i].as.e;
        part_val = nyr_lower_expr(b, sub);
        if (part_val < 0)
          return -1;
        if (nyr_expr_is_cstr(b, sub)) {
          /*
           * already a runtime C string — use directly
           */
        } else if (nyr_expr_is_f32(b, sub)) {
          int f64 = nyr_emit_f32_to_f64(b, part_val);
          part_val = f64 < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw",
                                                     f64, -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else if (nyr_expr_is_f64(b, sub)) {
          part_val = nyr_emit_runtime_call(b, "rt_f64_to_cstr_raw", part_val,
                                           -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else if (nyr_expr_is_any(b, sub)) {
          part_val = nyr_emit_runtime_call(b, "rt_any_to_cstr", part_val, -1,
                                           -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else if (nyr_expr_is_bool(b, sub)) {
          part_val = nyr_emit_runtime_call(b, "rt_bool_to_cstr", part_val, -1,
                                           -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else {
          /*
           * Raw i64 (matches print's is_cstr ? cstr : i64 convention).
           */
          part_val = nyr_emit_runtime_call(b, "rt_i64_to_cstr_raw", part_val,
                                           -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        }
      }
      if (part_val < 0)
        return -1;
      if (acc < 0) {
        acc = part_val;
        continue;
      }
      acc = nyr_emit_runtime_call(b, "rt_cstr_concat", acc, part_val, -1, 2, 0);
      if (acc < 0)
        return -1;
    }
    return acc;
  }
  case NY_E_DICT: {
    size_t count = e->as.dict.pairs.len;
    int capacity = nyr_emit_const(b, (int64_t)(count < 4 ? 8 : count * 2));
    if (capacity < 0)
      return -1;
    int dict =
        nyr_emit_runtime_call(b, "rt_dict_new_raw", capacity, -1, -1, 1, 0);
    if (dict < 0)
      return -1;
    int dict_slot = b->next_local_slot++;
    if (!nyr_store_local_value(b, dict_slot, dict))
      return -1;
    for (size_t i = 0; i < count; ++i) {
      const expr_t *key = e->as.dict.pairs.data[i].key;
      const expr_t *value = e->as.dict.pairs.data[i].value;
      int key_reg = nyr_lower_dict_key(b, key);
      int value_reg = nyr_lower_expr(b, value);
      if (key_reg < 0 || value_reg < 0)
        return -1;
      /* Dict slots are dynamic `any` values.  Native boolean expressions use
       * raw 0/1 for branch arithmetic, but stored dynamic booleans use the
       * canonical Ny immediates. */
      if (value->kind == NY_E_LITERAL &&
          value->as.literal.kind == NY_LIT_BOOL) {
        value_reg = nyr_emit_const(b, value->as.literal.as.b ? NY_IMM_TRUE
                                                             : NY_IMM_FALSE);
        if (value_reg < 0)
          return -1;
      }
      /* Dictionary slots use the public dynamic-value ABI.  Keep literal
       * construction consistent with indexed/set calls by boxing typed f64
       * values before handing them to the integer-shaped runtime bridge.
       * The lowered register is authoritative when semantic inference has
       * attached a stale float fact to a raw integer local. */
      nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value_reg);
      if (value_class == NYIR_SCALAR_F32) {
        value_reg = nyr_emit_f32_to_f64(b, value_reg);
        if (value_reg < 0)
          return -1;
        value_class = NYIR_SCALAR_F64;
      }
      if (value_class == NYIR_SCALAR_F64 ||
          (value_class == NYIR_SCALAR_UNKNOWN && nyr_expr_is_f64(b, value))) {
        int bits =
            nyr_emit_runtime_call(b, "rt_f64_bits", value_reg, -1, -1, 1, 0);
        value_reg = bits < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                     -1, -1, 1, 0);
        if (value_reg < 0)
          return -1;
      }
      /* Typed string parameters and locals use the borrowed C-string ABI.
       * Dynamic dictionary slots require the managed Ny string descriptor,
       * so normalize at the storage boundary before publishing the value. */
      if (nyr_expr_is_cstr(b, value)) {
        value_reg =
            nyr_emit_runtime_call(b, "rt_cstr_to_str", value_reg, -1, -1, 1, 0);
        if (value_reg < 0)
          return -1;
      }
      /* Dynamic integer parameters arrive in the legacy tagged ABI, while
       * native dictionary payloads are raw scalar slots.  Normalize only
       * values proven dynamic; raw integer literals must remain untouched
       * because odd raw values are valid native integers too. */
      bool is_bool_val = nyr_expr_is_bool(b, value);
      bool integer_literal = value && value->kind == NY_E_LITERAL &&
                             value->as.literal.kind == NY_LIT_INT &&
                             value->tok.kind != NY_T_NIL;
      bool raw_integer = integer_literal ||
                         (!is_bool_val && value && value->semantic.resolved &&
                          value->semantic.rep == NY_SEM_REP_RAW_INT);
      const char *value_type = nyr_expr_type_name(b, value);
      raw_integer = raw_integer || (!is_bool_val && value_type &&
                                    ny_native_type_name_is_int(value_type));
      if (!raw_integer && !is_bool_val && value && value->kind == NY_E_BINARY &&
          value->as.binary.op && !nyr_expr_is_any(b, value) &&
          !nyr_expr_is_f64(b, value) && !nyr_expr_is_f32(b, value) &&
          !nyr_expr_is_cstr(b, value) && !nyr_expr_is_ptr(b, value) &&
          !nyr_expr_is_dict(b, value) && !nyr_expr_is_list(b, value) &&
          !nyr_expr_is_bool(b, value) &&
          (strcmp(value->as.binary.op, "+") == 0 ||
           strcmp(value->as.binary.op, "-") == 0 ||
           strcmp(value->as.binary.op, "*") == 0 ||
           strcmp(value->as.binary.op, "/") == 0 ||
           strcmp(value->as.binary.op, "%") == 0))
        raw_integer = true;
      if (value->kind == NY_E_IDENT && value->as.ident.name) {
        const nyr_local_t *value_local = nyr_find_expr_local(b, value);
        if (value_local && value_local->is_bool)
          is_bool_val = true;
        raw_integer = raw_integer ||
                      (!is_bool_val && value_local &&
                       (value_local->semantic_rep == NY_SEM_REP_RAW_INT ||
                        ny_native_type_name_is_int(value_local->type_name)));
      }
      const nyr_local_t *callable_local = nyr_find_expr_local(b, value);
      bool callable_value = nyr_expr_is_callable_value(b, value);
      value_class = nyr_value_scalar_class(b, value_reg);
      if (value_class == NYIR_SCALAR_F64 || value_class == NYIR_SCALAR_F32)
        callable_value = false;
      if (callable_value) {
        raw_integer = false;
        const stmt_t *callable_fn = NULL;
        if (value->kind == NY_E_FN || value->kind == NY_E_LAMBDA) {
          ny_native_lambda_entry_t *entry =
              ny_native_lambda_find(ny_native_lambda_cache_for(b), value);
          callable_fn = entry ? entry->fn : NULL;
        } else if (callable_local) {
          if (callable_local->callable_name)
            callable_fn =
                nyr_find_user_function(b, callable_local->callable_name);
          if (!callable_fn && callable_local->callable_expr) {
            ny_native_lambda_entry_t *entry = ny_native_lambda_find(
                ny_native_lambda_cache_for(b), callable_local->callable_expr);
            callable_fn = entry ? entry->fn : NULL;
          }
        } else if (value->kind == NY_E_IDENT && value->as.ident.name) {
          callable_fn = nyr_find_user_function(b, value->as.ident.name);
        }
        /* Dynamic dictionary setters normalize scalar-looking payloads.
         * NYIR function symbols are opaque hashes, so mark callbacks before
         * storage and retain their argument and result ABIs for later calls. */
        const char *callable_marker =
            nyr_callable_storage_marker(b, callable_fn);
        value_reg = nyr_emit_runtime_call(b, callable_marker, value_reg, -1, -1,
                                          1, NYIR_SCALAR_I64_TAGGED);
        if (value_reg < 0)
          return -1;
      }
      /* The producer's physical ABI wins over stale semantic integer facts.
       * A ternary can join an `any` arm and a raw integer arm into a tagged
       * local while HM still records the local's inferred type as `int`; do
       * not tag that already-canonical dynamic word a second time. */
      if (nyr_value_scalar_class(b, value_reg) == NYIR_SCALAR_I64_TAGGED)
        raw_integer = false;
      /* Dictionary values are public `any` values.  Box every value proven
       * raw-integer before storing it, including odd integers from typed
       * locals.  Leaving those raw makes a later dynamic read indistinguish-
       * able from a tagged integer and corrupts equality/callback results. */
      if (is_bool_val) {
        value_reg = nyr_box_bool(b, value_reg);
        if (value_reg < 0)
          return -1;
      } else if (!callable_value &&
                 (raw_integer || nyr_value_scalar_class(b, value_reg) ==
                                     NYIR_SCALAR_I64_RAW) &&
                 !nyr_expr_is_cstr(b, value)) {
        value_reg = nyr_emit_runtime_call(b, "rt_tag", value_reg, -1, -1, 1, 0);
        if (value_reg < 0)
          return -1;
      }
      /* A proven-dynamic value already carries the canonical dynamic word
       * and is stored as-is.  Unboxing it through rt_any_to_i64 turned int 0
       * into a raw 0 payload, which the getter must read back as nil (raw 0
       * is reserved), so {"a": any_param} returned nil for the int 0 case. */
      int saved_dict = nyr_load_local_value(b, dict_slot);
      bool key_is_string =
          nyr_expr_is_cstr(b, key) ||
          (key->kind == NY_E_LITERAL && key->as.literal.kind == NY_LIT_STR);
      int set_result =
          saved_dict < 0
              ? -1
              : nyr_emit_runtime_call(b,
                                      key_is_string
                                          ? "rt_native_dict_set_str_compact"
                                          : "rt_native_dict_set_nir_i64",
                                      saved_dict, key_reg, value_reg, 3, 0);
      if (set_result < 0)
        return -1;
      /* Setters mutate the dictionary in place.  Keep the allocator result
       * as the expression value; relying on a setter's return register made
       * optimized machine calls lose the live dictionary pointer. */
    }
    return nyr_load_local_value(b, dict_slot);
  }
  case NY_E_TUPLE:
  case NY_E_LIST: {
    /*
     * Constant list literal → pooled .data array.  The value is the array
     * address, so indexing lowers to address math + load.  Non-constant
     * list construction is not supported by shared NYIR yet.
     */
    size_t count = e->as.list_like.len;
    if (count > 65536) {
      return nyr_fail(b, "native NYIR lower: list literal must have "
                         "at most 65536 elements"),
             -1;
    }
    if (count == 0) {
      int n = nyr_emit_const(b, 0);
      int width = nyr_emit_const(b, b->current_list_elem_size > 0
                                        ? b->current_list_elem_size
                                        : (b->force_dynamic_list ? 24 : 8));
      int base =
          n < 0 || width < 0
              ? -1
              : nyr_emit_runtime_call(b, "rt_tbuf_new_raw", n, width, -1, 2, 0);
      int length = n < 0 ? -1 : nyr_emit_const(b, 0);
      if (base < 0 || length < 0 || !nyr_record_list_len_fact(b, base, length))
        return -1;
      return e->kind == NY_E_TUPLE ? nyr_emit_runtime_call(b, "__list_as_tuple",
                                                           base, -1, -1, 1, 0)
                                   : base;
    }
    bool all_constant = !b->force_dynamic_list;
    bool has_string_constant = false;
    bool has_numeric_constant = false;
    for (size_t i = 0; i < count; ++i) {
      const expr_t *el = e->as.list_like.data[i];
      if (el && el->kind == NY_E_UNARY && el->as.unary.op &&
          (strcmp(el->as.unary.op, "+") == 0 ||
           strcmp(el->as.unary.op, "-") == 0))
        el = el->as.unary.right;
      if (!el || el->kind != NY_E_LITERAL) {
        all_constant = false;
        break;
      }
      if (el->as.literal.kind == NY_LIT_STR)
        has_string_constant = true;
      else
        has_numeric_constant = true;
    }
    if (has_string_constant && has_numeric_constant)
      all_constant = false;
    /* Pooled literal arrays have no element metadata. Float literals must
     * use the descriptor path so an `any` read can distinguish IEEE bits from
     * integer payloads after crossing a function or nested-list boundary. */
    if (all_constant && has_numeric_constant) {
      bool all_f64_literal = true;
      for (size_t i = 0; i < count; ++i)
        if (e->as.list_like.data[i]->as.literal.kind != NY_LIT_FLOAT) {
          all_f64_literal = false;
          break;
        }
      if (all_f64_literal)
        all_constant = false;
      if (b->current_list_elem_f64 && !all_f64_literal)
        all_constant = false;
    }
    /*
     * The pooled object format stores numeric words directly, but string
     * elements need process-local pointers.  Its embedded-string offsets are
     * not valid values for NYIR VM or native consumers, so build any
     * string-containing literal through the runtime tbuf path below.
     */
    if (has_string_constant)
      all_constant = false;
    if (!all_constant) {
      bool all_f64 = true;
      bool all_raw_i64 = !b->force_dynamic_list;
      for (size_t i = 0; i < count; ++i) {
        const expr_t *element = e->as.list_like.data[i];
        bool element_is_callable = nyr_expr_is_callable_value(b, element);
        bool element_f64 =
            b->current_list_elem_f64 || nyr_expr_is_f64(b, element);
        if (!element_f64) {
          all_f64 = false;
        }
        /* A list whose elements are proven raw scalar integers needs neither
         * descriptor slots nor per-element runtime tag discovery.  Besides
         * avoiding needless metadata, this preserves scalar-replacement for
         * transient literals such as `[i]` in hot loops. */
        if (nyr_expr_is_any(b, element) || nyr_expr_is_cstr(b, element) ||
            nyr_expr_is_dict(b, element) || nyr_expr_is_list(b, element) ||
            element_is_callable || nyr_expr_is_f64(b, element) ||
            nyr_expr_is_f32(b, element) ||
            (element->semantic.resolved &&
             (element->semantic.rep == NY_SEM_REP_OBJECT ||
              element->semantic.rep == NY_SEM_REP_POINTER ||
              element->semantic.rep == NY_SEM_REP_CLOSURE ||
              element->semantic.rep == NY_SEM_REP_AGGREGATE ||
              element->semantic.rep == NY_SEM_REP_TYPED_BUFFER)))
          all_raw_i64 = false;
        /* Bool elements carry raw 0/1 from comparison lowering; stored raw,
         * a boxed read classifies true as tagged integer 0. */
        if (nyr_expr_is_bool(b, element))
          all_raw_i64 = false;
        /* Comparison results lower as raw 0/1.  Stored raw, a boxed read
         * classifies `true` as tagged integer 0 (it printed 0 and was falsy
         * after leaving the list), so bool elements need the descriptor
         * path with canonical boxed bool words. */
        if (nyr_expr_is_bool(b, element))
          all_raw_i64 = false;
      }
      if (all_f64) {
        int n = nyr_emit_const(b, (int64_t)count);
        /* A float list crosses `any` boundaries (camera vectors, nested
         * lists, callbacks), so retain an explicit descriptor tag instead of
         * exposing IEEE bits as an integer elem-8 payload. */
        int width = nyr_emit_const(b, 24);
        int base = n < 0 || width < 0
                       ? -1
                       : nyr_emit_runtime_call(b, "rt_tbuf_new_raw", n, width,
                                               -1, 2, 0);
        if (base < 0)
          return -1;
        for (size_t i = 0; i < count; ++i) {
          const expr_t *element = e->as.list_like.data[i];
          int value = nyr_lower_expr(b, element);
          if (b->current_list_elem_f64 && !nyr_expr_is_f64(b, element)) {
            value = nyr_emit_i64_to_f64(b, value);
          }
          int off = nyr_emit_const(b, (int64_t)(i * 24));
          int slot = value < 0 || off < 0 ? -1 : nyr_emit_add_i64(b, base, off);
          int tag = slot < 0 ? -1 : nyr_emit_const(b, TAG_FLOAT);
          int tag_addr = slot < 0 || tag < 0
                             ? -1
                             : nyr_emit_add_i64(b, slot, nyr_emit_const(b, 16));
          if (slot < 0 || tag_addr < 0 || !nyr_emit_store_f64(b, slot, value) ||
              !nyr_emit_store_i64(b, tag_addr, tag))
            return -1;
        }
        int list_len = nyr_emit_const(b, (int64_t)count);
        if (list_len < 0 || !nyr_record_list_len_fact(b, base, list_len))
          return -1;
        if (count <= (size_t)(INT64_MAX / 24))
          nyr_record_alloc_fact(b, base, (int64_t)(count * 24));
        return e->kind == NY_E_TUPLE
                   ? nyr_emit_runtime_call(b, "__list_as_tuple", base, -1, -1,
                                           1, 0)
                   : base;
      }
      /* Proven raw-i64 elements have an unambiguous scalar-list contract even
       * when they are computed values rather than literals.  Elem-size 8 is
       * itself the runtime type evidence: rt_tbuf_index_any_raw boxes a raw
       * zero as integer 0 for this representation, while nil/any/string/list
       * elements were already excluded above and remain descriptor-backed.
       * Keeping computed homogeneous integer lists scalar avoids turning
       * transient state vectors (for example SHA-256's [a..h]) into repeated
       * dynamic index/unbox traffic. */
      if (all_raw_i64) {
        int n = nyr_emit_const(b, (int64_t)count);
        int width = nyr_emit_const(b, 8);
        int base = n < 0 || width < 0
                       ? -1
                       : nyr_emit_runtime_call(b, "rt_tbuf_new_raw", n, width,
                                               -1, 2, 0);
        if (base < 0)
          return -1;
        for (size_t i = 0; i < count; ++i) {
          int value = nyr_lower_expr(b, e->as.list_like.data[i]);
          /* Element reads through a dynamic accessor (a seq formal, an
             any/unknown target) return the boxed dynamic word, but an
             elem-8 slot needs the raw payload: decode exactly once here,
             or the slot keeps a tagged word that later elem-8 reads and
             comparisons normalize a second time (zip2 pair rows). */
          const expr_t *eli = e->as.list_like.data[i];
          if (value >= 0 && eli &&
              (eli->kind == NY_E_INDEX ||
               (eli->kind == NY_E_MEMCALL && eli->as.memcall.name &&
                strcmp(eli->as.memcall.name, "get") == 0))) {
            const expr_t *rt = eli->kind == NY_E_INDEX ? eli->as.index.target
                                                       : eli->as.memcall.target;
            bool boxes =
                rt && (nyr_expr_is_any(b, rt) || nyr_expr_is_dyn_list(b, rt));
            if (rt && rt->kind == NY_E_IDENT && rt->as.ident.name) {
              const nyr_local_t *rtl = nyr_find_local(b, rt->as.ident.name);
              boxes = boxes || (rtl && (rtl->is_dyn_list ||
                                        (rtl->type_name &&
                                         strcmp(rtl->type_name, "seq") == 0)));
            }
            if (boxes && !nyr_expr_is_f64(b, eli) && !nyr_expr_is_cstr(b, eli))
              value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1,
                                            1, 0);
          }
          int off = nyr_emit_const(b, (int64_t)(i * 8));
          int slot = value < 0 || off < 0 ? -1 : nyr_emit_add_i64(b, base, off);
          if (slot < 0 || !nyr_emit_store_i64(b, slot, value))
            return -1;
        }
        int list_len = nyr_emit_const(b, (int64_t)count);
        if (list_len < 0 || !nyr_record_list_len_fact(b, base, list_len))
          return -1;
        if (count <= (size_t)(INT64_MAX / 8))
          nyr_record_alloc_fact(b, base, (int64_t)(count * 8));
        return e->kind == NY_E_TUPLE
                   ? nyr_emit_runtime_call(b, "__list_as_tuple", base, -1, -1,
                                           1, 0)
                   : base;
      }
      int n = nyr_emit_const(b, (int64_t)count);
      int width = nyr_emit_const(b, 24);
      int base =
          n < 0 || width < 0
              ? -1
              : nyr_emit_runtime_call(b, "rt_tbuf_new_raw", n, width, -1, 2, 0);
      if (base < 0)
        return -1;
      for (size_t i = 0; i < count; ++i) {
        const expr_t *el = e->as.list_like.data[i];
        bool saved_force_dynamic = b->force_dynamic_list;
        if (el->kind == NY_E_LIST || el->kind == NY_E_TUPLE)
          b->force_dynamic_list = true;
        int value = nyr_lower_expr(b, el);
        b->force_dynamic_list = saved_force_dynamic;
        const nyr_local_t *callable_local = nyr_find_expr_local(b, el);
        const stmt_t *callable_fn = NULL;
        if (el->kind == NY_E_FN || el->kind == NY_E_LAMBDA) {
          ny_native_lambda_entry_t *entry =
              ny_native_lambda_find(ny_native_lambda_cache_for(b), el);
          callable_fn = entry ? entry->fn : NULL;
        } else if (callable_local) {
          if (callable_local->callable_name)
            callable_fn =
                nyr_find_user_function(b, callable_local->callable_name);
          if (!callable_fn && callable_local->callable_expr) {
            ny_native_lambda_entry_t *entry = ny_native_lambda_find(
                ny_native_lambda_cache_for(b), callable_local->callable_expr);
            callable_fn = entry ? entry->fn : NULL;
          }
        } else if (el->kind == NY_E_IDENT && el->as.ident.name) {
          callable_fn = nyr_find_user_function(b, el->as.ident.name);
        }
        bool callable_value = callable_fn || nyr_expr_is_callable_value(b, el);
        nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
        if (value_class == NYIR_SCALAR_F64 || value_class == NYIR_SCALAR_F32)
          callable_value = false;
        if (callable_value && callable_fn) {
          const char *marker = nyr_callable_storage_marker(b, callable_fn);
          value = nyr_emit_runtime_call(b, marker, value, -1, -1, 1,
                                        NYIR_SCALAR_I64_TAGGED);
          if (value < 0)
            return -1;
        }
        int index = nyr_emit_const(b, (int64_t)i);
        int stride = nyr_emit_const(b, 24);
        int off = value < 0 || index < 0 || stride < 0
                      ? -1
                      : nyr_push_val(b, NYIR_MUL_I64, index, stride, 0, NULL);
        int slot = off < 0 ? -1 : nyr_emit_add_i64(b, base, off);
        if (slot < 0)
          return -1;
        bool is_string = nyr_expr_is_cstr(b, el);
        bool is_dict = nyr_expr_is_dict(b, el);
        bool is_list_elem = nyr_expr_is_list(b, el);
        bool is_f32 = value_class == NYIR_SCALAR_F32 || nyr_expr_is_f32(b, el);
        bool is_f64 = value_class == NYIR_SCALAR_F64 ||
                      (!is_f32 && nyr_expr_is_f64(b, el));
        /* Descriptor-backed list slots store floating payloads as f64.
         * Keeping an f32 value in the integer branch fed its bits to
         * rt_tag_or_raw_int and made list construction fail ABI verification
         * (or misclassify the value on backends without that verifier). */
        if (is_f32 && !is_f64) {
          value = nyr_emit_f32_to_f64(b, value);
          if (value < 0)
            return -1;
          is_f64 = true;
        }
        /* Native integer expressions such as from_int() already produce a
         * raw payload.  Do not classify an odd payload with rt_value_tag:
         * the descriptor reader would then untag it a second time. */
        bool raw_integer_expr =
            el->semantic.resolved && el->semantic.rep == NY_SEM_REP_RAW_INT;
        if (el->kind == NY_E_IDENT && el->as.ident.name) {
          const nyr_local_t *element_local =
              nyr_find_local(b, el->as.ident.name);
          raw_integer_expr = raw_integer_expr ||
                             (element_local && element_local->semantic_rep ==
                                                   NY_SEM_REP_RAW_INT);
        }
        /* A pooled C-string literal stored raw is indistinguishable from any
         * other .rodata pointer once it leaves the descriptor (is_str probes
         * the bytes before the pointer for a TAG header).  Materialize a
         * managed handle at construction so downstream is_str/iteration see
         * a real string ([(1, 2), range(2), "ab"] flattened lost the split). */
        if (is_string && el->kind == NY_E_LITERAL) {
          value =
              nyr_emit_runtime_call(b, "rt_cstr_to_str", value, -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        /* Comparison lowering yields raw 0/1.  Store canonical boxed bools
         * (NY_IMM_TRUE=8 / NY_IMM_FALSE=2) so reads classify as booleans;
         * a raw 1 read back as tagged integer 0 printed as 0 and was falsy
         * (the Rule 110 demo's rendered generations froze).  Scan-free:
         * the arithmetic box is correct for raw 0/1 and already-boxed 2/8
         * alike. */
        if (nyr_expr_is_bool(b, el) && !is_string && !is_f64) {
          int got =
              nyr_emit_cmp_i64(b, NYIR_CMP_NE, value, nyr_emit_const(b, 0));
          int not_false = nyr_emit_cmp_i64(b, NYIR_CMP_NE, value,
                                           nyr_emit_const(b, NY_IMM_FALSE));
          if (got < 0 || not_false < 0)
            return -1;
          int truthy = nyr_emit_binop(b, NYIR_AND_I64, got, not_false);
          int six = nyr_emit_const(b, 6);
          int mul = truthy < 0 || six < 0
                        ? -1
                        : nyr_emit_binop(b, NYIR_MUL_I64, truthy, six);
          value = mul < 0 ? -1
                          : nyr_emit_binop(b, NYIR_ADD_I64, mul,
                                           nyr_emit_const(b, NY_IMM_FALSE));
          if (value < 0)
            return -1;
        }
        /* Inferred `any` parameters arrive as value/length/tag triples. The
         * tag-aware runtime setter below writes their canonical value into a
         * descriptor slot; statically classified elements keep the direct
         * payload path. */
        /* An ADT constructor call produces a direct object handle, even when
         * inference leaves its value annotation broad.  It is not a tagged
         * `any` word and must not pass through rt_any_to_i64 (which decodes
         * tagged scalars and yields zero for a raw ADT pointer). */
        bool enum_object_value = false;
        const char *enum_constructor_name =
            el->kind == NY_E_CALL
                ? ny_native_call_leaf(el)
                : (el->kind == NY_E_MEMCALL ? el->as.memcall.name : NULL);
        if (enum_constructor_name) {
          const stmt_t *enum_stmt = NULL;
          const stmt_enum_item_t *enum_item = NULL;
          int64_t enum_tag = 0;
          enum_object_value = nyr_find_enum_member(
              b, enum_constructor_name, &enum_stmt, &enum_item, &enum_tag);
        }
        bool dynamic_value =
            !enum_object_value && (nyr_expr_is_any(b, el) || callable_value);
        if (!dynamic_value && el->kind == NY_E_IDENT && el->as.ident.name) {
          const nyr_local_t *element_local =
              nyr_find_local(b, el->as.ident.name);
          dynamic_value =
              element_local &&
              (element_local->semantic_rep == NY_SEM_REP_CLOSURE ||
               element_local->callable_expr || element_local->callable_name);
        }
        /* Invoking an `any`/closure parameter uses rt_call_anyN, whose
         * result is the canonical tagged dynamic word even when HM has
         * specialized this particular use to an integer. Treat that producer
         * representation as dynamic while constructing descriptor-backed
         * lists; otherwise the slot is mislabeled as raw-int and later reads
         * expose the tagged bits (23 instead of 11). */
        if (!dynamic_value && el->kind == NY_E_CALL && el->as.call.callee &&
            el->as.call.callee->kind == NY_E_IDENT &&
            el->as.call.callee->as.ident.name) {
          const nyr_local_t *callee_local =
              nyr_find_local(b, el->as.call.callee->as.ident.name);
          dynamic_value = callee_local &&
                          (callee_local->is_any ||
                           callee_local->semantic_rep == NY_SEM_REP_CLOSURE);
        }
        if (el->kind == NY_E_IDENT && el->as.ident.name) {
          const nyr_local_t *value_local = nyr_find_expr_local(b, el);
          dynamic_value = dynamic_value ||
                          (value_local && (value_local->is_any ||
                                           value_local->dyn_tag_slot >= 0));
        }
        if (is_f64 && value_class == NYIR_SCALAR_F64)
          dynamic_value = false;
        /* Preserve the source value's runtime tag when crossing from a
         * dynamic local into descriptor-backed storage.  The raw scalar
         * bridge intentionally converts tagged floats to integers, so
         * manually unboxing here would store 0.6 as integer 0.  The runtime
         * descriptor setter applies the complete value/tag conversion for
         * floats, strings, booleans, integers, and handles at this boundary. */
        if (dynamic_value) {
          int set = nyr_emit_runtime_call(b, "rt_tbuf_set_tagged", base, index,
                                          value, 3, 0);
          if (set < 0)
            return -1;
          continue;
        }
        int tag = -1;
        if (enum_object_value) {
          /* ADT handles carry their variant tag in the object header.  The
           * generic value-tag probe can mistake an aligned object address
           * for an integer payload; use the raw-word pointer classifier so
           * descriptor reads preserve the handle for pattern matching. */
          tag =
              nyr_emit_runtime_call(b, "rt_raw_word_tag", value, -1, -1, 1, 0);
        } else if (is_f64)
          tag = nyr_emit_const(b, TAG_FLOAT);
        else if (is_string)
          tag = nyr_emit_const(b, 121);
        else if (is_dict)
          tag = nyr_emit_const(b, 101);
        else if (is_list_elem || el->kind == NY_E_LIST ||
                 el->kind == NY_E_TUPLE)
          tag = nyr_emit_const(b, 100);
        else if (nyr_expr_is_bool(b, el))
          tag = nyr_emit_const(b, NY_TBUF_TAG_BOOL);
        else if (raw_integer_expr && !is_string && !is_f64 && !dynamic_value)
          tag = nyr_emit_const(b, 3);
        else if (count == 1 && !is_string && !is_f64 && !dynamic_value &&
                 el->kind == NY_E_IDENT) {
          /* A statically scalar local is already a raw integer payload.  The
           * generic tag bridge performs a heap/tag probe on every iteration;
           * descriptor reads return slot 0 unchanged for integer elements, so
           * the fixed integer tag is both sufficient and substantially cheaper.
           */
          tag = nyr_emit_const(b, 3);
        }
        if (tag < 0 && el->kind == NY_E_LITERAL &&
            el->as.literal.kind == NY_LIT_INT && el->tok.kind != NY_T_NIL) {
          /*
           * A literal slot payload is the raw machine word by construction.
           * rt_value_tag classifies odd words as tagged-int (tag 1), which
           * tells raw consumers to untag — [ptr, 17] then handed the int
           * param 8 (untag 17) instead of 17 at thread-arg reads.
           */
          tag = nyr_emit_const(b, 3);
        }
        if (tag < 0)
          tag =
              nyr_emit_runtime_call(b,
                                    (!is_string && !is_dict && !is_list_elem &&
                                     !is_f64 && el->kind != NY_E_LITERAL)
                                        ? "rt_tag_or_raw_int"
                                        : "rt_value_tag",
                                    value, -1, -1, 1, 0);
        int length = is_string
                         ? nyr_peek_dyn_fact(b, value, NYR_FACT_DYN_STR_LEN)
                         : nyr_emit_const(b, 0);
        if (length < 0)
          length = nyr_emit_const(b, 0);
        int payload = value;
        if (payload < 0 || length < 0 || tag < 0 ||
            !(is_f64 ? nyr_emit_store_f64(b, slot, payload)
                     : nyr_emit_store_i64(b, slot, payload)))
          return -1;
        int len_slot = nyr_emit_add_i64(b, slot, nyr_emit_const(b, 8));
        int tag_slot = nyr_emit_add_i64(b, slot, nyr_emit_const(b, 16));
        if (len_slot < 0 || tag_slot < 0 ||
            !nyr_emit_store_i64(b, len_slot, length) ||
            !nyr_emit_store_i64(b, tag_slot, tag))
          return -1;
      }
      int list_len = nyr_emit_const(b, (int64_t)count);
      if (list_len < 0 || !nyr_record_list_len_fact(b, base, list_len))
        return -1;
      nyr_record_alloc_fact(b, base, (int64_t)(count * 24));
      return e->kind == NY_E_TUPLE ? nyr_emit_runtime_call(b, "__list_as_tuple",
                                                           base, -1, -1, 1, 0)
                                   : base;
    }
    /*
     * Constant list literal → pooled .data array.  The value is the array
     * address, so indexing lowers to address math + load.
     */
    nyr_array_elem_t *values = calloc(count, sizeof(*values));
    if (!values) {
      return nyr_fail(b, "native NYIR lower: constant list allocation failed"),
             -1;
    }
    bool string_elements = false;
    bool saw_int_word = false;
    bool saw_float_word = false;
    for (size_t i = 0; i < count; ++i) {
      const expr_t *el = e->as.list_like.data[i];
      bool negate = false;
      if (el && el->kind == NY_E_UNARY && el->as.unary.op &&
          (strcmp(el->as.unary.op, "+") == 0 ||
           strcmp(el->as.unary.op, "-") == 0)) {
        negate = strcmp(el->as.unary.op, "-") == 0;
        el = el->as.unary.right;
      }
      if (!el || el->kind != NY_E_LITERAL) {
        nyr_fail(b, "native NYIR lower: only constant list literals "
                    "are supported in shared NYIR");
        free(values);
        return -1;
      }
      if (el->as.literal.kind == NY_LIT_INT) {
        if (string_elements) {
          nyr_fail(b,
                   "native NYIR lower: heterogeneous constant list "
                   "elements at %s:%d",
                   e->tok.filename ? e->tok.filename : "<source>", e->tok.line);
          free(values);
          return -1;
        }
        saw_int_word = true;
        int64_t value = el->as.literal.as.i;
        if (negate) {
          if (value == INT64_MIN) {
            nyr_fail(b,
                     "native NYIR lower: negated list integer overflows i64");
            free(values);
            return -1;
          }
          value = -value;
        }
        values[i].value = value;
        values[i].tag = 3;
      } else if (el->as.literal.kind == NY_LIT_FLOAT) {
        if (string_elements) {
          nyr_fail(b,
                   "native NYIR lower: heterogeneous constant list "
                   "elements at %s:%d",
                   e->tok.filename ? e->tok.filename : "<source>", e->tok.line);
          free(values);
          return -1;
        }
        double value = el->as.literal.as.f;
        values[i].value = ny_native_f64_bits(negate ? -value : value);
        saw_float_word = true;
        values[i].tag = TAG_FLOAT;
      } else if (!negate && el->as.literal.kind == NY_LIT_BOOL) {
        values[i].value = el->as.literal.as.b ? 1 : 0;
        values[i].tag = 3;
        saw_int_word = true;
      } else if (!negate && el->as.literal.kind == NY_LIT_STR) {
        if (i && !string_elements) {
          nyr_fail(b, "native NYIR lower: heterogeneous constant "
                      "list elements are not supported");
          free(values);
          return -1;
        }
        string_elements = true;
        values[i].str = el->as.literal.as.s.data;
        values[i].str_len = el->as.literal.as.s.len;
        values[i].tag = TAG_STR_CONST;
      } else {
        nyr_fail(b, "native NYIR lower: unsupported constant list element");
        free(values);
        return -1;
      }
    }
    /*
     * Mixed numeric lists need descriptors so index lowering can carry each
     * element's runtime type alongside its raw 64-bit payload.
     */
    size_t stride =
        string_elements || (saw_int_word && saw_float_word) ? 24 : 8;
    const char *arr_sym = nyr_arraytab_intern(values, count, stride, NULL, 0);
    free(values);
    if (!arr_sym)
      return -1;
    int addr = nyr_ir_emit(
        &b->nyir,
        (nyr_inst_t){.op = NYIR_ADDR_SYMBOL, .dst = -1, .symbol = arr_sym});
    if (addr < 0)
      return -1;
    addr = nyr_emit_runtime_call(b, "rt_tbuf_register_static_handle", addr, -1,
                                 -1, 1, 0);
    if (addr < 0)
      return -1;
    /* The pooled array is a read-only template, while language list literals
     * are mutable values.  A literal evaluated at each source occurrence
     * (and each function call) must receive its own tbuf header and elements;
     * otherwise equal literals alias and mutation of one list changes the
     * others.  Tuples remain safely shared because their language API is
     * immutable. */
    if (e->kind == NY_E_LIST) {
      addr = nyr_emit_runtime_call(b, "rt_tbuf_clone_raw", addr, -1, -1, 1, 0);
      if (addr < 0)
        return -1;
    }
    int length = nyr_emit_const(b, (int64_t)count);
    if (length < 0 || !nyr_record_list_len_fact(b, addr, length))
      return -1;
    if (count <= INT64_MAX / stride)
      nyr_record_alloc_fact(b, addr, (int64_t)(count * stride));
    return e->kind == NY_E_TUPLE
               ? nyr_emit_runtime_call(b, "__list_as_tuple", addr, -1, -1, 1, 0)
               : addr;
  }
  case NY_E_INDEX: {
    /* Slice syntax is represented as an index node with a stop/step.  A
     * string is a raw C-string at this lowering boundary, so lower only
     * actual slices here.  The previous `!stop` condition classified plain
     * `s[i]` as `s[i:]`, returning a suffix instead of one character. */
    if (e->as.index.target && nyr_expr_is_cstr(b, e->as.index.target) &&
        (e->as.index.stop || e->as.index.step) &&
        (!e->as.index.step ||
         (e->as.index.step->kind == NY_E_LITERAL &&
          e->as.index.step->as.literal.kind == NY_LIT_INT &&
          e->as.index.step->as.literal.as.i == 1))) {
      int string = nyr_lower_expr(b, e->as.index.target);
      /* Sequence bounds are native integers.  Keep this boundary explicit:
       * a stale contextual f64 annotation on `i + j` must be converted
       * numerically, not bitcast into an i64 index word. */
      int start = e->as.index.start ? nyr_lower_int_value(b, e->as.index.start)
                                    : nyr_emit_const(b, 0);
      int stop =
          e->as.index.stop
              ? nyr_lower_int_value(b, e->as.index.stop)
              : (string < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_cstr_len", string,
                                                    -1, -1, 1, 0));
      if (string < 0 || start < 0 || stop < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_cstr_slice", string, start, stop, 3,
                                   0);
    }
    bool target_is_dict = nyr_expr_is_dict(b, e->as.index.target);
    if (!target_is_dict && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT) {
      nyr_local_t *tl =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      if (tl && tl->is_dict)
        target_is_dict = true;
    }
    bool target_is_seq =
        !target_is_dict && (nyr_expr_is_list(b, e->as.index.target) ||
                            nyr_expr_is_tuple(b, e->as.index.target) ||
                            nyr_expr_is_bytes(b, e->as.index.target) ||
                            nyr_expr_is_cstr(b, e->as.index.target) ||
                            nyr_expr_is_range(b, e->as.index.target) ||
                            (nyr_expr_is_dyn_list(b, e->as.index.target) &&
                             !nyr_expr_is_any(b, e->as.index.target)));
    if (!target_is_seq && !target_is_dict && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT) {
      nyr_local_t *tl = nyr_find_local(b, e->as.index.target->as.ident.name);
      if (tl && (tl->is_list || tl->is_cstr || tl->is_bytes ||
                 (tl->type_name && strcmp(tl->type_name, "tuple") == 0)))
        target_is_seq = true;
    }
    bool non_int_index = false;
    if (e->as.index.start) {
      if (nyr_expr_is_cstr(b, e->as.index.start) ||
          nyr_expr_is_bool(b, e->as.index.start) ||
          (e->as.index.start->kind == NY_E_LITERAL &&
           (e->as.index.start->as.literal.kind == NY_LIT_STR ||
            e->as.index.start->as.literal.kind == NY_LIT_BOOL ||
            e->as.index.start->as.literal.kind == NY_LIT_FLOAT ||
            e->as.index.start->tok.kind == NY_T_NIL))) {
        non_int_index = true;
      }
    }
    if (target_is_seq && non_int_index) {
      return nyr_emit_runtime_call(b, "rt_index_key_error", -1, -1, -1, 0, 0);
    }
    if (e->as.index.target && !nyr_expr_is_bytes(b, e->as.index.target) &&
        nyr_expr_is_cstr(b, e->as.index.target) && e->as.index.start &&
        !e->as.index.stop && !e->as.index.step) {
      int str = nyr_lower_expr(b, e->as.index.target);
      int index = nyr_lower_int_value(b, e->as.index.start);
      if (str < 0 || index < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_cstr_index_read_raw", str, index, -1,
                                   2, 0);
    }
    if (e->as.index.target && nyr_expr_is_bytes(b, e->as.index.target) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int bytes = nyr_lower_expr(b, e->as.index.target);
      int index = nyr_lower_int_value(b, e->as.index.start);
      if (bytes < 0 || index < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_bytes_index_read_raw", bytes, index,
                                   -1, 2, 0);
    }
    if (e->as.index.target && nyr_expr_is_range(b, e->as.index.target) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int range = nyr_lower_expr(b, e->as.index.target);
      int index = nyr_lower_int_value(b, e->as.index.start);
      if (range < 0 || index < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_range_index_read_raw", range, index,
                                   -1, 2, 0);
    }
    /* An element loaded from a dynamic outer sequence can itself be a raw
     * typed list.  Reusing the outer dynamic stride (24-byte value/len/tag)
     * for the inner access reads the wrong slot; lower the outer expression
     * first and let the typed-buffer accessor use its actual 8-byte stride. */
    if (e->as.index.target && e->as.index.target->kind == NY_E_INDEX &&
        e->as.index.target->as.index.target &&
        (nyr_expr_is_list(b, e->as.index.target->as.index.target) ||
         nyr_expr_is_dyn_list(b, e->as.index.target->as.index.target) ||
         nyr_expr_is_any(b, e->as.index.target->as.index.target)) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int inner = nyr_lower_expr(b, e->as.index.target);
      int index = nyr_lower_int_value(b, e->as.index.start);
      if (inner < 0 || index < 0)
        return -1;
      /* The first read of a dynamic/nested sequence returns a canonical
       * dynamic value.  A chained subscript must keep that ABI for the second
       * read; feeding the tagged inner container to the raw accessor happens
       * to work for index zero but loses non-zero elements in arithmetic
       * expressions. */
      bool dynamic_inner =
          b->index_for_any_call || nyr_expr_is_any(b, e->as.index.target) ||
          nyr_expr_is_dyn_list(b, e->as.index.target) ||
          (e->as.index.target->kind == NY_E_INDEX &&
           e->as.index.target->as.index.target &&
           (nyr_expr_is_dyn_list(b, e->as.index.target->as.index.target) ||
            nyr_expr_is_any(b, e->as.index.target->as.index.target)));
      /* A statically typed outer list stores raw scalar elements in its
       * nested buffers.  Only a dynamic outer value requires the canonical
       * accessor for its inner read.  The result type alone cannot decide
       * this: a dynamic read may acquire a contextual scalar type. */
      const expr_t *outer = e->as.index.target->as.index.target;
      while (outer && outer->kind == NY_E_INDEX)
        outer = outer->as.index.target;
      const char *nested_result_type = nyr_expr_type_name(b, e);
      if (!nyr_expr_is_any(b, outer) && nested_result_type &&
          strcmp(nested_result_type, "any") != 0 &&
          strcmp(nested_result_type, "unknown") != 0)
        dynamic_inner = false;
      int value = nyr_emit_runtime_call(
          b, dynamic_inner ? "rt_tbuf_index_any_raw" : "rt_tbuf_index_read_raw",
          inner, index, -1, 2, 0);
      if (value < 0)
        return -1;
      /* Descriptor-backed inner lists return canonical dynamic values.  The
       * chained expression still has its declared scalar element type, so
       * decode that value at this boundary before native f64 consumers use
       * the register as a floating-point value. */
      return nyr_expr_is_f64(b, e)
                 ? nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                         NYIR_SCALAR_F64)
                 : value;
    }
    if (!target_is_seq && !target_is_dict && non_int_index &&
        e->as.index.target) {
      int target = nyr_lower_expr(b, e->as.index.target);
      int key = nyr_lower_expr(b, e->as.index.start);
      int fallback = nyr_emit_const(b, rt_tag_v(0));
      if (target < 0 || key < 0 || fallback < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_value_get_tagged", target, key,
                                   fallback, 3, 0);
    }
    if (!target_is_seq && e->as.index.target &&
        (e->as.index.target->kind == NY_E_IDENT ||
         nyr_expr_is_dict(b, e->as.index.target))) {
      bool is_dict = nyr_expr_is_dict(b, e->as.index.target);
      if (e->as.index.target->kind == NY_E_IDENT) {
        nyr_local_t *dict_local =
            nyr_find_local(b, e->as.index.target->as.ident.name);
        if (dict_local && dict_local->is_dict)
          is_dict = true;
      }
      if (is_dict && e->as.index.start && !e->as.index.stop &&
          !e->as.index.step) {
        /* No compile-time dict-literal folding here.  A `def` binding only
         * prevents rebinding, so the dict object can still gain or change
         * entries through set_idx/dict_set after its initializer runs; a
         * folded read would observe stale values.  The runtime bridge below
         * reads the live dictionary and returns the canonical dynamic word. */
        int dict = nyr_lower_expr(b, e->as.index.target);
        int key = nyr_lower_dict_key(b, e->as.index.start);
        /* Dictionary indexing is dynamic; a missing key's language default
         * is integer zero, not the nil immediate used by the raw bridge. */
        int fallback = nyr_emit_const(b, rt_tag_v(0));
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        bool string_key = nyr_expr_is_cstr(b, e->as.index.start) ||
                          (e->as.index.start->kind == NY_E_LITERAL &&
                           e->as.index.start->as.literal.kind == NY_LIT_STR);
        int value = nyr_emit_runtime_call(
            b, string_key ? "rt_dict_get_str_raw" : "rt_value_get_tagged", dict,
            key, fallback, 3, 0);
        /* The bridge exposes a canonical dynamic value and the read stays on
         * that ABI: `expr_is_any` advertises dictionary indexing as dynamic,
         * so print/equality render the word as-is and raw consumers (scalar
         * local stores, call arguments, dynamic arithmetic helpers) decode
         * exactly once at their own boundary.  Decoding here made those
         * consumers unbox a second time (int 1 printed as 0, 2 as false). */
        return value;
      }
    }
    /*
     * A one-element temporary indexed by its only constant index does not
     * escape the expression.  Lower the element directly instead of
     * allocating a 24-byte tagged tbuf and immediately loading it back.
     * Keep this strictly syntactic: dynamic indices and larger literals
     * retain the normal bounds-checked representation.
     */
    if (e->as.index.target && e->as.index.target->kind == NY_E_LIST &&
        e->as.index.target->as.list_like.len == 1 && e->as.index.start &&
        e->as.index.start->kind == NY_E_LITERAL &&
        e->as.index.start->as.literal.kind == NY_LIT_INT &&
        e->as.index.start->as.literal.as.i == 0) {
      return nyr_lower_expr(b, e->as.index.target->as.list_like.data[0]);
    }
    const expr_t *literal_target =
        nyr_resolve_list_literal(b, e->as.index.target, 0);
    if (literal_target && literal_target->kind == NY_E_LIST &&
        literal_target->as.list_like.len == 1 && e->as.index.start &&
        e->as.index.start->kind == NY_E_LITERAL &&
        e->as.index.start->as.literal.kind == NY_LIT_INT &&
        e->as.index.start->as.literal.as.i == 0)
      return nyr_lower_expr(b, literal_target->as.list_like.data[0]);
    bool dynamic_elements = literal_target
                                ? nyr_expr_is_dyn_list(b, literal_target)
                                : nyr_expr_is_dyn_list(b, e->as.index.target);
    bool unknown_target = false;
    bool global_list = false;
    /* An unparameterized list formal can receive either 8-byte scalar slots
     * or 24-byte dynamic descriptors.  Its ABI deliberately carries no
     * compile-time stride, so do not guess one from the formal type; the
     * typed-buffer accessor reads the runtime header and handles both forms. */
    bool runtime_tbuf_access = false;
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT) {
      nyr_local_t *l =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      bool explicit_scalar_target =
          l && l->is_list && !l->is_any && l->type_name &&
          !ny_native_type_name_is_dyn_list(l->type_name);
      bool explicit_int_element_target = false;
      if (explicit_scalar_target) {
        const char *element = strchr(l->type_name, '<');
        explicit_int_element_target =
            element && strncmp(element + 1, "int", 3) == 0 &&
            (element[4] == '>' || element[4] == '\0' ||
             isspace((unsigned char)element[4]));
      }
      if (!l && nyr_expr_is_dyn_list(b, e->as.index.target))
        runtime_tbuf_access = true;
      /* A global list can be rebound after its initializer, including from
       * another flattened module.  Without a local proof of immutability,
       * read its length and stride from the runtime descriptor. */
      bool is_root_entry =
          b->current_fn_name && strcmp(b->current_fn_name, "rt_main") == 0;
      global_list =
          (!l || is_root_entry) &&
          nyr_find_top_level_value(b, e->as.index.target->as.ident.name);
      /* resolve_list_literal only returns a literal for a top-level `def`
       * (never a `mut` rebindable slot, checked against every top-level
       * statement), so a non-NULL literal_target is a compile-time proof of
       * immutability.  A non-dynamic scalar list literal may then be indexed
       * with the fixed-width inline path instead of the runtime accessor. */
      bool sequence_formal =
          l && l->type_name && strcmp(l->type_name, "seq") == 0;
      if ((global_list && nyr_expr_is_dyn_list(b, e->as.index.target)) ||
          (l && !explicit_scalar_target &&
           (l->is_dyn_list || sequence_formal))) {
        /* A global list literal has no element-type proof at this use site:
         * `def xs = [1, 2]` is still a mutable, dynamically typed sequence.
         * Keep its reads on the tagged accessor so an untyped consumer such
         * as variadic print receives a canonical any value.  Only an
         * explicitly typed local/formal may select the raw elem-8 ABI. */
        dynamic_elements = true;
        runtime_tbuf_access = true;
      } else if (global_list && !nyr_index_of_typed_global_int_list(b, e)) {
        /* Root bindings are also present in the builder's local table while
         * lowering rt_main. Keep rebindable/untyped globals dynamic, but an
         * explicit list<int> local has a stable element contract even when
         * its list handle uses the runtime descriptor for bounds checking. */
        dynamic_elements = !explicit_int_element_target;
        runtime_tbuf_access = true;
      } else if (global_list) {
        /* A global homogeneous scalar list still has a runtime descriptor
         * because its binding is reassignable, but its element ABI is raw.
         * Keep the runtime bounds/stride check without routing the result
         * through the tagged-any accessor. */
        dynamic_elements = false;
        runtime_tbuf_access = true;
      } else if (l && l->is_list)
        dynamic_elements = false;
      else if (l)
        unknown_target = true;
    }
    if (!dynamic_elements && !literal_target && e->as.index.target &&
        e->as.index.target->kind != NY_E_IDENT &&
        !nyr_expr_is_list(b, e->as.index.target)) {
      dynamic_elements = true;
    }
    int base = nyr_lower_expr(b, e->as.index.target);
    int idx = nyr_lower_int_value(b, e->as.index.start);
    if (base < 0 || idx < 0)
      return -1;
    /*
     * Untyped local sequence bindings may still carry an inferred scalar
     * result on the index AST. Their runtime slots can be raw elem-8 values,
     * so a raw slot load followed by the generic arithmetic decoder halves
     * odd values. Keep the physical sequence boundary representation-aware:
     * the tbuf accessor boxes scalar slots and preserves descriptor values.
     */
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *local =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      bool dynamic_sequence = local && (nyr_local_is_dynamic_sequence(local) ||
                                        (local->is_any && local->is_list));
      bool raw_append_int_list =
          (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
           !local) &&
          nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name);
      if (dynamic_sequence && !raw_append_int_list)
        return nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw", base, idx, -1,
                                     2, 0);
      if (local && local->is_any)
        return nyr_emit_runtime_call(b, "rt_value_get_index_raw", base, idx, 0,
                                     3, 0);
    }
    /* A mutable inferred global integer list is stored in raw elem-8 slots.
     * Its dynamic carrier annotation must not send an ordinary integer
     * consumer through an any decoder: an odd payload such as 3 would be
     * untagged to 1. Restrict this shortcut to a mutable literal in the same
     * source file, with an integer result contract; explicit any boundaries
     * continue through the canonical accessor. */
    bool local_source_global = e->as.index.target &&
                               e->as.index.target->kind == NY_E_IDENT &&
                               e->as.index.target->as.ident.name;
    const stmt_t *index_decl =
        local_source_global
            ? nyr_find_top_level_var_stmt(b, e->as.index.target->as.ident.name)
            : NULL;
    const expr_t *global_literal =
        local_source_global
            ? nyr_find_top_level_value(b, e->as.index.target->as.ident.name)
            : NULL;
    bool raw_int_global =
        b->current_fn_name && strcmp(b->current_fn_name, "rt_main") == 0 &&
        index_decl && index_decl->kind == NY_S_VAR &&
        index_decl->as.var.is_mut &&
        nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name) &&
        global_literal && global_literal->kind == NY_E_LIST &&
        global_literal->as.list_like.len > 0 && !b->index_for_any_call &&
        !e->as.index.stop && !e->as.index.step;
    for (size_t i = 0; raw_int_global && i < global_literal->as.list_like.len;
         ++i) {
      const expr_t *element = global_literal->as.list_like.data[i];
      raw_int_global = element && element->kind == NY_E_LITERAL &&
                       element->as.literal.kind == NY_LIT_INT &&
                       element->tok.kind != NY_T_NIL;
    }
    if (raw_int_global) {
      ((expr_t *)e)->semantic.resolved = true;
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
      return nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx, -1,
                                   2, NYIR_SCALAR_I64_RAW);
    }
    /* Immutable homogeneous integer tuples use the same elem-8 payload
     * representation as scalar lists.  Tuple locals are not returned by
     * resolve_list_literal(), so the generic descriptor path otherwise loses
     * the raw scalar ABI and hands a tagged integer to native consumers. */
    const expr_t *tuple_literal = NULL;
    if (e->as.index.target && e->as.index.target->kind == NY_E_TUPLE) {
      tuple_literal = e->as.index.target;
    } else if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
               e->as.index.target->as.ident.name) {
      const char *name = e->as.index.target->as.ident.name;
      const stmt_t *decl = nyr_find_top_level_var_stmt(b, name);
      const nyr_local_t *local = nyr_find_local_nearest(b, name);
      /* A mutable global can be rebound after its initializer, so its
       * initial tuple literal does not prove the active runtime layout. */
      if ((!local || !local->semantic_mutable) &&
          (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_mut))
        tuple_literal = nyr_find_top_level_value(b, name);
    }
    bool raw_int_tuple = tuple_literal && tuple_literal->kind == NY_E_TUPLE &&
                         tuple_literal->as.list_like.len > 0;
    for (size_t i = 0; raw_int_tuple && i < tuple_literal->as.list_like.len;
         ++i) {
      const expr_t *element = tuple_literal->as.list_like.data[i];
      raw_int_tuple = element && element->kind == NY_E_LITERAL &&
                      element->as.literal.kind == NY_LIT_INT;
    }
    if (raw_int_tuple && !e->as.index.stop && !e->as.index.step) {
      ((expr_t *)e)->semantic.resolved = true;
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
      return nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx, -1,
                                   2, NYIR_SCALAR_I64_RAW);
    }
    /* A declared top-level list<int> has elem-8 scalar slots even when its
     * empty-list initializer makes the global expression look dynamic.
     * Read through the checked raw accessor so an element assigned to a
     * mutable index remains an i64 (a tagged read doubles its next index). */
    if (nyr_index_of_typed_global_int_list(b, e)) {
      ((expr_t *)e)->semantic.resolved = true;
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
      /*
       * The declared type proves the elem-8 layout, so read the slot
       * directly instead of paying the accessor's per-read handle/magic/
       * stride validation (the append-built promotion below shares this
       * emitter for the same reason).
       */
      bool canonical_payload = literal_target &&
                               literal_target->kind == NY_E_LIST &&
                               !nyr_expr_is_dyn_list(b, literal_target);
      int inline_read =
          nyr_emit_proven_elem8_read(b, base, idx, canonical_payload);
      return inline_read >= 0
                 ? inline_read
                 : nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx,
                                         -1, 2, 0);
    }
    /* An unannotated dynamic receiver can be a list, string, bytes object, or
     * dictionary at runtime.  Preserve that polymorphism at the shared
     * index boundary; the helper selects the concrete representation before
     * falling back to tagged dictionary lookup. */
    const nyr_local_t *dynamic_ident_local =
        e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
                e->as.index.target->as.ident.name
            ? nyr_find_local_nearest(b, e->as.index.target->as.ident.name)
            : NULL;
    if (dynamic_ident_local && !dynamic_ident_local->type_name &&
        !dynamic_ident_local->is_list && !e->as.index.stop && !e->as.index.step)
      return nyr_emit_runtime_call(b, "rt_value_get_index_raw", base, idx, 0, 3,
                                   0);
    bool explicit_scalar_index_target = false;
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *early_target =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      explicit_scalar_index_target =
          early_target && early_target->is_list && !early_target->is_any &&
          !early_target->is_dyn_list && early_target->type_name &&
          !ny_native_type_name_is_dyn_list(early_target->type_name);
    }
    /* A top-level mut proven by the whole-program scan holds an elem-8 int
     * buffer: its element reads are raw i64 loads, not dynamic boundary
     * crossings. */
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name &&
        (!b->current_fn_name || strcmp(b->current_fn_name, "rt_main") == 0 ||
         !nyr_find_local_nearest(b, e->as.index.target->as.ident.name)) &&
        nyr_top_mut_is_append_int_list(b, e->as.index.target->as.ident.name) &&
        !e->as.index.stop && !e->as.index.step) {
      /* The checker could not prove the element type while the list was an
       * untyped `[]`; the whole-program scan now has.  Publish the raw-int
       * representation on the node so every later consumer (print format,
       * comparisons, local fact propagation) agrees with the emitted ABI. */
      ((expr_t *)e)->semantic.resolved = true;
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
      int basev = nyr_lower_expr(b, e->as.index.target);
      int idxv = nyr_lower_int_value(b, e->as.index.start);
      if (basev < 0 || idxv < 0)
        return -1;
      int inline_read = nyr_emit_proven_elem8_read(b, base, idx, false);
      return inline_read >= 0
                 ? inline_read
                 : nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx,
                                         -1, 2, 0);
    }
    /* Dynamic sequence elements are canonical any values.  Route the whole
     * access through the representation-aware boundary before the fixed-width
     * list optimizer can mistake a descriptor slot for an i64 scalar. */
    if (e->as.index.target && !nyr_expr_is_any(b, e->as.index.target) &&
        nyr_expr_is_dyn_list(b, e->as.index.target) &&
        !explicit_scalar_index_target && !e->as.index.stop &&
        !e->as.index.step) {
      /* A dynamic sequence has no fixed storage stride, but the indexed
       * expression can still have a proven scalar result.  Preserve that
       * result ABI here: the tagged accessor is only correct for an `any`
       * result, while a proven int/float must remain raw for arithmetic and
       * equality.  The later runtime_tbuf path still performs the checked
       * descriptor access. */
      const char *early_result_type = nyr_expr_type_name(b, e);
      bool early_dynamic_result = !early_result_type ||
                                  strcmp(early_result_type, "any") == 0 ||
                                  strcmp(early_result_type, "unknown") == 0;
      if (early_dynamic_result) {
        return nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw", base, idx, -1,
                                     2, 0);
      }
    }
    /* A statically-known out-of-range list subscript is still a language
     * panic, not undefined memory access. Route the impossible literal index
     * through the checked runtime accessor so try/catch observes the ordinary
     * `index_read out of range` exception instead of a trap/adjacent load. */
    if (literal_target && literal_target->kind == NY_E_LIST &&
        e->as.index.start && e->as.index.start->kind == NY_E_LITERAL &&
        e->as.index.start->as.literal.kind == NY_LIT_INT) {
      int64_t literal_index = e->as.index.start->as.literal.as.i;
      int64_t literal_len = (int64_t)literal_target->as.list_like.len;
      if (literal_index >= literal_len || literal_index < -literal_len)
        return nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx, -1,
                                     2, 0);
    }
    bool typed_static_target = false;
    bool dynamic_value_target = false;
    bool typed_f64_target = false;
    bool typed_int_target = false;
    bool typed_descriptor_target = false;
    const nyr_local_t *target_local = NULL;
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      target_local =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      typed_static_target =
          target_local && target_local->is_list && !target_local->is_any;
      dynamic_value_target = target_local && target_local->is_any;
      if (target_local && target_local->type_name) {
        const char *lt = strchr(target_local->type_name, '<');
        const char *element_type = lt ? lt + 1 : target_local->type_name;
        bool scalar_element =
            element_type &&
            ((strncmp(element_type, "int", 3) == 0 &&
              (element_type[3] == '>' || element_type[3] == '\0' ||
               isspace((unsigned char)element_type[3]))) ||
             (strncmp(element_type, "bool", 4) == 0 &&
              (element_type[4] == '>' || element_type[4] == '\0' ||
               isspace((unsigned char)element_type[4]))) ||
             (strncmp(element_type, "f64", 3) == 0 &&
              (element_type[3] == '>' || element_type[3] == '\0' ||
               isspace((unsigned char)element_type[3]))) ||
             (strncmp(element_type, "f32", 3) == 0 &&
              (element_type[3] == '>' || element_type[3] == '\0' ||
               isspace((unsigned char)element_type[3]))));
        typed_int_target = typed_static_target && lt &&
                           strncmp(element_type, "int", 3) == 0 &&
                           (element_type[3] == '>' || element_type[3] == '\0' ||
                            isspace((unsigned char)element_type[3]));
        typed_f64_target = lt && strncmp(lt + 1, "f64", 3) == 0 &&
                           (lt[4] == '>' || isspace((unsigned char)lt[4]));
        typed_descriptor_target =
            strcmp(target_local->type_name, "seq") == 0 || !scalar_element;
        if (typed_f64_target || typed_descriptor_target)
          runtime_tbuf_access = true;
      }
      /* Inferred list type strings can collapse user ADTs to bare `list`.
       * Recover the descriptor requirement from the resolved initializer so
       * indexing a list of enum handles does not untag their pointer values. */
      const expr_t *local_list_literal =
          target_local && target_local->list_literal
              ? target_local->list_literal
              : literal_target;
      if (local_list_literal && local_list_literal->kind == NY_E_LIST) {
        for (size_t i = 0; i < local_list_literal->as.list_like.len; ++i) {
          const expr_t *element = local_list_literal->as.list_like.data[i];
          const char *constructor =
              element && element->kind == NY_E_CALL
                  ? ny_native_call_leaf(element)
                  : (element && element->kind == NY_E_MEMCALL
                         ? element->as.memcall.name
                         : NULL);
          const stmt_t *enum_stmt = NULL;
          const stmt_enum_item_t *enum_item = NULL;
          int64_t enum_tag = 0;
          if (constructor && nyr_find_enum_member(b, constructor, &enum_stmt,
                                                  &enum_item, &enum_tag)) {
            typed_descriptor_target = true;
            break;
          }
        }
      }
      if (typed_descriptor_target) {
        /* The accessor returns a canonical handle/tagged value for
         * descriptor slots, not a scalar merely because inference widened an
         * enum element.  Publish that ABI so callers do not integer-box the
         * returned object pointer. */
        ((expr_t *)e)->semantic.resolved = true;
        ((expr_t *)e)->semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
      }
    }
    /* A dynamic list does not imply a dynamic result.  Untyped list literals
     * are represented by descriptor tbufs, but their scalar elements still
     * feed ordinary integer/float arithmetic (for example fannkuch's
     * permutation index).  Only an explicit any-context requests the tagged
     * decoder; using the semantic dynamic marker here leaks tagged integers
     * into raw NYIR locals and can turn `while k != 0` into container logic. */
    /* Sequence formals carry the runtime tbuf descriptor, so their reads
     * must cross the tagged dynamic boundary even when the surrounding
     * expression is not an explicit `any` call.  Decode back to raw i64 only
     * for statically scalar list consumers; pointer/container values remain
     * unchanged through rt_any_to_i64. */
    /* A call-result receiver with no proven sequence representation may be a
     * legacy managed list or a native descriptor tbuf.  The fixed-width raw
     * path below assumes the latter and can read past the managed list's
     * payload.  Cross this boundary through the representation-aware decoder
     * exactly as an explicit `any` call does. */
    bool unknown_call_receiver = e->as.index.target &&
                                 e->as.index.target->kind != NY_E_IDENT &&
                                 !literal_target && !typed_static_target;
    bool dynamic_sequence_target =
        target_local && nyr_local_is_dynamic_sequence(target_local);
    if (!dynamic_sequence_target && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT)
      dynamic_sequence_target = nyr_expr_is_dyn_list(b, e->as.index.target);
    bool dynamic_index_result =
        b->index_for_any_call || (runtime_tbuf_access && dynamic_elements) ||
        unknown_call_receiver || dynamic_value_target ||
        dynamic_sequence_target || typed_f64_target ||
        typed_descriptor_target ||
        (target_local && target_local->is_list && !target_local->type_name);
    /* An unannotated list crossing a closure/function boundary carries only
     * the runtime descriptor.  A later inferred scalar element type must not
     * erase that representation fact: doing so turns an indexed read into a
     * raw pointer load and loses both negative-index normalization and bounds
     * errors.  Keep the common accessor until a concrete list element layout
     * is proven by the type contract. */
    if (target_local && target_local->is_list && !target_local->type_name &&
        !target_local->is_dyn_list)
      dynamic_index_result = true;
    /* A descriptor-backed nested access may still have a fully known result
     * type.  For example, indexing `list<list<int>>` produces `list<int>`,
     * and indexing that result produces `int`.  The storage stride is
     * dynamic, but the value ABI is not: routing either read through the
     * any accessor tags the scalar and later arithmetic adds tagged words.
     * Let the representation-aware raw path handle the descriptor while
     * preserving the proven element type. */
    const char *index_result_type = nyr_expr_type_name(b, e);
    bool index_result_known = index_result_type &&
                              strcmp(index_result_type, "any") != 0 &&
                              strcmp(index_result_type, "unknown") != 0;
    /* A tuple literal carries its element layout in the tbuf header, but it
     * is not an untyped sequence formal: when the checker has resolved this
     * access to a scalar, keep that scalar ABI.  Treating every tuple local
     * as a dynamic sequence routed integer elements through the `any`
     * accessor, which tagged the raw elem-8 payload immediately before a
     * native integer comparison consumed it. */
    bool scalar_tuple_read = index_result_known &&
                             nyr_expr_is_tuple(b, e->as.index.target) &&
                             (ny_native_type_name_is_int(index_result_type) ||
                              nyr_expr_is_f64(b, e));
    if (scalar_tuple_read) {
      dynamic_sequence_target = false;
      dynamic_value_target = false;
      typed_descriptor_target = false;
    }
    /* An unparameterized `list` formal is a dynamic sequence boundary even
     * when HM proves the current index expression to be `int`.  Its elem-8
     * slots hold raw payloads, while the expression ABI is still `any`; the
     * tagged accessor is what makes the later scalar/object consumer decode
     * the slot exactly once.  Do not let the inferred element type erase
     * that physical boundary.  Parameterized scalar/nested lists may use the
     * proven raw representation as before. */
    if (dynamic_index_result && index_result_known &&
        !dynamic_sequence_target && !unknown_call_receiver &&
        !b->index_for_any_call)
      dynamic_index_result = false;
    if (dynamic_index_result) {
      if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
          e->as.index.target->as.ident.name) {
        nyr_local_t *target_l =
            nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
        /*
         * Dynamic values and unparameterized list formals can receive either
         * a managed list or a native tbuf handle. The value-index adapter
         * dispatches on the runtime representation and preserves raw
         * elem-8 payloads when boxing the result.
         */
        if (target_l && target_l->is_any && !target_l->is_list) {
          return nyr_emit_runtime_call(b, "rt_value_get_index_raw", base, idx,
                                       0, 3, 0);
        }
        if (target_l && target_l->is_any && !target_l->is_list &&
            target_l->type_name &&
            ny_native_type_name_is_list(target_l->type_name) &&
            strcmp(target_l->type_name, "list") != 0)
          return nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw", base, idx,
                                       -1, 2, 0);
        /* An unannotated sequence formal keeps the compact value/length ABI,
         * but its actual call may be a list, string, or bytes object.  The
         * representation-aware accessor is the only correct common path. */
        if (target_l && target_l->is_list && !target_l->type_name &&
            !target_l->is_dyn_list)
          return nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw", base, idx,
                                       -1, 2, 0);
      }
      if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT) {
        nyr_local_t *target_l =
            nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
        /* Unparameterized `list` values can have elem-8 storage without a
         * statically proven integer element ABI. Keep both arms tagged unless
         * the source contract is specifically list<int>; the runtime stride
         * check proves width, not element representation. */
        if (target_l &&
            (target_l->is_list || (target_l->is_any && target_l->type_name &&
                                   strcmp(target_l->type_name, "list") == 0))) {
          /*
           * A local list carries its logical length in a companion slot.  The
           * old fast path ignored that fact, re-read count from the tbuf
           * header, then emitted ge-zero + lt-count + elem-size checks and a
           * join on every read.  Besides extra work, those ad-hoc branches are
           * invisible to NYIR's bounds-check elimination and loop passes.
           *
           * Keep only the representation guard here.  For the common elem-8
           * representation, normalize Python-style negative indices and emit
           * a real NYIR_BOUNDS_CHECK over the scaled byte offset.  The check
           * preserves out-of-range behavior while becoming eligible for BCE,
           * IRCE, LICM and LLVM's ordinary loop optimizers.  Descriptor lists
           * retain the representation-aware runtime fallback unchanged.
           */
          int eight = nyr_emit_const(b, 8);
          int count = nyr_peek_list_len_fact(b, base);
          if (count < 0 && target_l->list_len_slot >= 0)
            count = nyr_load_local_value(b, target_l->list_len_slot);
          if (count < 0) {
            int neg24 = nyr_emit_const(b, -24);
            int len_addr = neg24 < 0 ? -1 : nyr_emit_add_i64(b, base, neg24);
            count = len_addr < 0 ? -1 : nyr_emit_load_i64(b, len_addr);
          }
          /* A concrete list<int> contract already proves an elem-8 payload.
           * Avoid reloading the stride field and building a representation
           * diamond for every indexed read; keep the full runtime guard for
           * inferred or dynamically typed lists. */
          int is_e8 = -1;
          if (typed_int_target && !typed_descriptor_target) {
            is_e8 = nyr_emit_const(b, 1);
          } else {
            int neg16 = nyr_emit_const(b, -16);
            int esz_addr = neg16 < 0 ? -1 : nyr_emit_add_i64(b, base, neg16);
            int elem_sz = esz_addr < 0 ? -1 : nyr_emit_load_i64(b, esz_addr);
            is_e8 = (eight < 0 || elem_sz < 0 || count < 0)
                        ? -1
                        : nyr_emit_cmp_i64(b, NYIR_CMP_EQ, elem_sz, eight);
          }
          if (is_e8 >= 0) {
            int fast_lab = b->next_label++;
            int slow_lab = b->next_label++;
            int end_lab = b->next_label++;
            int res_slot = nyr_temp_slot(b);
            if (fast_lab >= 0 && slow_lab >= 0 && end_lab >= 0 &&
                res_slot >= 0) {
              if (!nyr_emit_br_if(b, is_e8, fast_lab) ||
                  !nyr_emit_br(b, slow_lab))
                return -1;

              if (!nyr_emit_label(b, fast_lab))
                return -1;
              int zero = nyr_emit_const(b, 0);
              int is_neg =
                  zero < 0 ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_LT, idx, zero);
              int wrapped = is_neg < 0 ? -1 : nyr_emit_add_i64(b, idx, count);
              int adj_idx =
                  wrapped < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SELECT_I64,
                                                          .dst = -1,
                                                          .a = is_neg,
                                                          .b = wrapped,
                                                          .c = idx});
              if (adj_idx < 0)
                nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
              int off8 = adj_idx < 0 ? -1
                                     : nyr_push_val(b, NYIR_MUL_I64, adj_idx,
                                                    eight, 0, NULL);
              int byte_len = count < 0 ? -1
                                       : nyr_push_val(b, NYIR_MUL_I64, count,
                                                      eight, 0, NULL);
              if (off8 < 0 || byte_len < 0 ||
                  !nyr_emit_bounds_check_value(b, base, off8, byte_len, 0))
                return -1;
              int addr = nyr_emit_add_i64(b, base, off8);
              if (addr < 0)
                return -1;
              if (byte_len >= 0 && b->nyir.len > 0 &&
                  b->nyir.data[b->nyir.len - 1].dst == addr &&
                  b->nyir.data[b->nyir.len - 1].op == NYIR_ADD_I64)
                b->nyir.data[b->nyir.len - 1].flags |=
                    NYIR_INST_F_IN_BOUNDS_ADDR;
              /* An untyped list formal can carry f64 values even though its
               * index expression has no standalone element type.  Arithmetic
               * lowering supplies the consumer's f64 context while lowering
               * the operand; honor it here so the descriptor arm is decoded
               * as a float instead of being mistaken for an integer. */
              bool is_f64 = nyr_expr_is_f64(b, e);
              bool contextual_untyped_f64 =
                  b->f64_context_depth != 0 && target_l &&
                  target_l->type_name &&
                  strcmp(target_l->type_name, "list") == 0;
              is_f64 = is_f64 || contextual_untyped_f64;
              if (!is_f64 && e->as.index.target &&
                  e->as.index.target->kind == NY_E_IDENT &&
                  e->as.index.target->as.ident.name) {
                const nyr_local_t *seq = nyr_find_local_nearest(
                    b, e->as.index.target->as.ident.name);
                const char *lt =
                    seq && seq->type_name ? strchr(seq->type_name, '<') : NULL;
                is_f64 = lt && strncmp(lt + 1, "f64", 3) == 0 &&
                         (lt[4] == '>' || isspace((unsigned char)lt[4]));
              }
              bool proven_scalar_layout =
                  typed_int_target && !typed_descriptor_target;
              /* A runtime elem_size == 8 check proves the physical slot
               * layout, but does not prove that a generic list element is an
               * integer. Keep untyped-list reads in the tagged value domain;
               * only a concrete scalar element contract permits raw loads.
               * This prevents a descriptor element from being decoded as an
               * integer when the slow arm rejoins the elem-8 fast arm. */
              bool raw_layout_ok =
                  !is_f64 && !typed_descriptor_target && proven_scalar_layout;
              if (raw_layout_ok && !nyr_mark_temp_slot_raw(b, res_slot))
                return -1;
              if (!is_f64 && !raw_layout_ok &&
                  !nyr_mark_temp_slot_tagged(b, res_slot))
                return -1;
              /* Both arms produce raw i64 for raw_layout_ok reads: a raw `1`
               * from the fast arm decoded as tagged integer zero while the
               * slow arm decoded correctly was the original reason both arms
               * routed through the dynamic accessor. */
              int fast_val =
                  !raw_layout_ok
                      ? nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw", base,
                                              adj_idx, -1, 2, 0)
                      : (is_f64 ? nyr_emit_load_f64(b, addr)
                                : nyr_emit_load_i64(b, addr));
              if (fast_val >= 0 && is_f64 && !raw_layout_ok)
                fast_val = nyr_emit_runtime_call(b, "rt_any_to_f64", fast_val,
                                                 -1, -1, 1, NYIR_SCALAR_F64);
              /* A direct elem-8 load has no runtime-call signature from
               * which to infer its carrier.  Mark the raw arm explicitly so
               * the temporary's join class agrees with the decoded slow
               * arm; otherwise a later typed-list store can persist a
               * tagged value into raw storage (heapsort's repeated swaps).
               */
              if (fast_val >= 0 && raw_layout_ok && !is_f64 &&
                  b->nyir.len > 0 &&
                  b->nyir.data[b->nyir.len - 1].dst == fast_val)
                b->nyir.data[b->nyir.len - 1].scalar_class =
                    NYIR_SCALAR_I64_RAW;
              if (fast_val < 0 ||
                  !nyr_store_local_value(b, res_slot, fast_val) ||
                  !nyr_emit_br(b, end_lab))
                return -1;

              if (!nyr_emit_label(b, slow_lab))
                return -1;
              int slow_val = nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw",
                                                   base, idx, -1, 2, 0);
              if (slow_val < 0)
                return -1;
              if (raw_layout_ok && !is_f64) {
                slow_val = nyr_emit_runtime_call(b, "rt_any_to_i64", slow_val,
                                                 -1, -1, 1, 0);
              } else if (is_f64) {
                slow_val = nyr_emit_runtime_call(b, "rt_any_to_f64", slow_val,
                                                 -1, -1, 1, NYIR_SCALAR_F64);
              }
              if (slow_val < 0 ||
                  !nyr_store_local_value(b, res_slot, slow_val) ||
                  !nyr_emit_br(b, end_lab))
                return -1;

              if (!nyr_emit_label(b, end_lab))
                return -1;
              /* Publish the representation both arms actually produce.
               * Dynamic accessors return tagged values when an elem-8 header
               * does not prove the sequence's scalar element ABI. */
              if (!is_f64) {
                ((expr_t *)e)->semantic.resolved = true;
                ((expr_t *)e)->semantic.rep = raw_layout_ok
                                                  ? NY_SEM_REP_RAW_INT
                                                  : NY_SEM_REP_TAGGED_DYNAMIC;
              }
              return nyr_load_local_value(b, res_slot);
            }
          }
        }
      }
      int value = nyr_emit_runtime_call(b, "rt_tbuf_index_any_raw", base, idx,
                                        -1, 2, 0);
      if (value < 0)
        return -1;
      /*
       * A call-result sequence may be either a managed list or a native
       * typed-buffer.  Its representation-aware accessor returns a canonical
       * dynamic word, even when type inference knows the element is scalar.
       * Keep that physical ABI visible so consumers decode exactly once.
       */
      if (unknown_call_receiver && !nyr_expr_is_f64(b, e)) {
        ((expr_t *)e)->semantic.resolved = true;
        ((expr_t *)e)->semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
      }
      if (b->f64_context_depth != 0 && !nyr_expr_is_f64(b, e))
        return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                     NYIR_SCALAR_F64);
      if (typed_static_target && target_local && target_local->type_name &&
          *target_local->type_name && !typed_descriptor_target &&
          !dynamic_sequence_target)
        return nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
      /* The accessor returns the canonical dynamic representation.  Consumers
       * that require a scalar decode it at their own semantic boundary; doing
       * it here loses descriptor/string elements from heterogeneous lists. */
      return value;
    }
    /* Native list/tuple values use the data pointer as their base.  The
     * ordinary byte-offset path is correct for non-negative indices, but a
     * negative index would address the typed-buffer header.  Route negative
     * literals and containers with unknown element stride through the
     * representation-aware accessor. */
    if (runtime_tbuf_access) {
      if (typed_f64_target || nyr_expr_is_f64(b, e))
        return nyr_emit_runtime_call(b, "rt_tbuf_index_f64", base, idx, -1, 2,
                                     NYIR_SCALAR_F64);
      bool raw_int_element =
          typed_int_target && !typed_descriptor_target && !dynamic_elements;
      int value =
          nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx, -1, 2,
                                raw_int_element ? NYIR_SCALAR_I64_RAW : 0);
      if (value < 0)
        return -1;
      if (raw_int_element) {
        ((expr_t *)e)->semantic.resolved = true;
        ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
      }
      return typed_f64_target || nyr_expr_is_f64(b, e)
                 ? nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                         NYIR_SCALAR_F64)
                 : value;
    }
    int64_t negative_index = 0;
    bool is_negative =
        nyr_negative_literal_i64(e->as.index.start, &negative_index);
    if (unknown_target && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *local =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      bool dynamic_receiver =
          local &&
          (local->is_any || local->is_dyn_list ||
           (local->type_name && (strcmp(local->type_name, "list") == 0 ||
                                 strcmp(local->type_name, "seq") == 0)));
      if (dynamic_receiver)
        return nyr_emit_runtime_call(b, "rt_value_get_index_raw", base, idx, 0,
                                     3, 0);
    }
    if (is_negative || unknown_target) {
      return nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx, -1,
                                   2, 0);
    }
    int width = nyr_emit_const(b, dynamic_elements ? 24 : 8);
    int off =
        width < 0 ? -1 : nyr_push_val(b, NYIR_MUL_I64, idx, width, 0, NULL);
    if (off < 0)
      return -1;
    int64_t byte_len = nyr_peek_alloc_fact(b, base);
    int length = nyr_peek_list_len_fact(b, base);
    if (literal_target && literal_target->kind == NY_E_LIST) {
      if (length < 0) {
        length = nyr_emit_const(b, (int64_t)literal_target->as.list_like.len);
      }
      if (byte_len <= 0) {
        byte_len = (int64_t)(literal_target->as.list_like.len *
                             (dynamic_elements ? 24 : 8));
      }
    }
    int dynamic_byte_len = -1;
    if (length >= 0) {
      dynamic_byte_len = nyr_push_val(b, NYIR_MUL_I64, length, width, 0, NULL);
      if (dynamic_byte_len < 0)
        return -1;
    }
    if (dynamic_byte_len < 0 && byte_len <= 0) {
      const nyr_local_t *target_l =
          (e->as.index.target && e->as.index.target->kind == NY_E_IDENT)
              ? nyr_find_local(b, e->as.index.target->as.ident.name)
              : NULL;
      if (target_l && target_l->is_list && !dynamic_elements &&
          !unknown_target) {
        int neg24 = nyr_emit_const(b, -24);
        int len_addr = neg24 < 0 ? -1 : nyr_emit_add_i64(b, base, neg24);
        int count = len_addr < 0 ? -1 : nyr_emit_load_i64(b, len_addr);
        int zero = nyr_emit_const(b, 0);
        int ge_zero = (zero < 0 || count < 0)
                          ? -1
                          : nyr_emit_cmp_i64(b, NYIR_CMP_GE, idx, zero);
        int lt_count =
            (ge_zero < 0) ? -1 : nyr_emit_cmp_i64(b, NYIR_CMP_LT, idx, count);
        int in_bounds =
            (lt_count < 0) ? -1
                           : nyr_emit_binop(b, NYIR_AND_I64, ge_zero, lt_count);
        if (in_bounds >= 0) {
          int fast_lab = b->next_label++;
          int slow_lab = b->next_label++;
          int end_lab = b->next_label++;
          int res_slot = nyr_temp_slot(b);
          if (fast_lab >= 0 && slow_lab >= 0 && end_lab >= 0 && res_slot >= 0) {
            if (!nyr_emit_br_if(b, in_bounds, fast_lab) ||
                !nyr_emit_br(b, slow_lab))
              return -1;

            if (!nyr_emit_label(b, fast_lab))
              return -1;
            int addr = off < 0 ? -1 : nyr_emit_add_i64(b, base, off);
            if (addr < 0)
              return -1;
            bool is_f64 = nyr_expr_is_f64(b, e);
            if (!is_f64 && e->as.index.target &&
                e->as.index.target->kind == NY_E_IDENT &&
                e->as.index.target->as.ident.name) {
              const nyr_local_t *seq =
                  nyr_find_local(b, e->as.index.target->as.ident.name);
              const char *lt =
                  seq && seq->type_name ? strchr(seq->type_name, '<') : NULL;
              is_f64 = lt && strncmp(lt + 1, "f64", 3) == 0 &&
                       (lt[4] == '>' || isspace((unsigned char)lt[4]));
            }
            int fast_val = is_f64 ? nyr_emit_load_f64(b, addr)
                                  : nyr_emit_load_i64(b, addr);
            if (fast_val >= 0 && !is_f64 && typed_static_target &&
                !typed_descriptor_target && b->nyir.len > 0 &&
                b->nyir.data[b->nyir.len - 1].dst == fast_val)
              b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
            if (fast_val < 0 || !nyr_store_local_value(b, res_slot, fast_val) ||
                !nyr_emit_br(b, end_lab))
              return -1;

            if (!nyr_emit_label(b, slow_lab))
              return -1;
            int slow_val = nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw",
                                                 base, idx, -1, 2, 0);
            if (slow_val >= 0 && typed_static_target &&
                !typed_descriptor_target && b->nyir.len > 0 &&
                b->nyir.data[b->nyir.len - 1].dst == slow_val)
              b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
            if (slow_val < 0 || !nyr_store_local_value(b, res_slot, slow_val) ||
                !nyr_emit_br(b, end_lab))
              return -1;

            if (!nyr_emit_label(b, end_lab))
              return -1;
            if (!is_f64 && typed_static_target && !typed_descriptor_target) {
              ((expr_t *)e)->semantic.resolved = true;
              ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
            }
            return nyr_load_local_value(b, res_slot);
          }
        }
      }
      return nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", base, idx, -1,
                                   2, 0);
    }
    if (!nyr_emit_bounds_check_value(b, base, off, dynamic_byte_len, byte_len))
      return -1;
    int addr = off < 0 ? -1 : nyr_emit_add_i64(b, base, off);
    if (addr < 0)
      return -1;
    if ((dynamic_byte_len >= 0 || byte_len > 0) && b->nyir.len > 0 &&
        b->nyir.data[b->nyir.len - 1].dst == addr &&
        b->nyir.data[b->nyir.len - 1].op == NYIR_ADD_I64)
      b->nyir.data[b->nyir.len - 1].flags |= NYIR_INST_F_IN_BOUNDS_ADDR;
    bool is_f64 = nyr_expr_is_f64(b, e);
    if (!is_f64 && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const nyr_local_t *seq =
          nyr_find_local_nearest(b, e->as.index.target->as.ident.name);
      const char *lt =
          seq && seq->type_name ? strchr(seq->type_name, '<') : NULL;
      is_f64 = lt && strncmp(lt + 1, "f64", 3) == 0 &&
               (lt[4] == '>' || isspace((unsigned char)lt[4]));
    }
    if (dynamic_elements) {
      int payload = nyr_emit_load_i64(b, addr);
      int o8 = nyr_emit_const(b, 8);
      int o16 = nyr_emit_const(b, 16);
      int a8 = o8 < 0 ? -1 : nyr_emit_add_i64(b, addr, o8);
      int a16 = o16 < 0 ? -1 : nyr_emit_add_i64(b, addr, o16);
      int len = a8 < 0 ? -1 : nyr_emit_load_i64(b, a8);
      int tag = a16 < 0 ? -1 : nyr_emit_load_i64(b, a16);
      if (payload < 0 || len < 0 || tag < 0)
        return -1;
      if (!nyr_record_dyn_fact(b, payload, NYR_FACT_DYN_STR_LEN, len) ||
          !nyr_record_dyn_fact(b, payload, NYR_FACT_DYN_TAG, tag))
        return -1;
      return payload;
    }
    int loaded =
        is_f64 ? nyr_emit_load_f64(b, addr) : nyr_emit_load_i64(b, addr);
    if (loaded >= 0 && !is_f64 && typed_static_target &&
        !typed_descriptor_target && b->nyir.len > 0 &&
        b->nyir.data[b->nyir.len - 1].dst == loaded)
      b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
    if (loaded >= 0 && !is_f64 && typed_static_target &&
        !typed_descriptor_target) {
      ((expr_t *)e)->semantic.resolved = true;
      ((expr_t *)e)->semantic.rep = NY_SEM_REP_RAW_INT;
    }
    return loaded;
  }
  case NY_E_MEMCALL: {
    const stmt_t *selected = e->semantic.canonical_callee_stmt;
    /* Handle typed-list length before attached stdlib method resolution.
     * Lowering the stdlib facade can otherwise discard the expanded length
     * ABI and emit a generic rt_len call in the loop condition. */
    const expr_t *length_target = e->as.memcall.target;
    nyr_local_t *length_local =
        length_target && length_target->kind == NY_E_IDENT
            ? nyr_find_local(b, length_target->as.ident.name)
            : NULL;
    bool typed_length_target =
        nyr_expr_is_list(b, length_target) ||
        (length_local && length_local->is_list && !length_local->is_any);
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "len") == 0 &&
        e->as.memcall.args.len == 0 && typed_length_target &&
        (!selected || ny_is_stdlib_tok(selected->tok))) {
      /* Typed-list parameters already carry their element count in the
       * expanded native ABI. Reuse that stable slot for immutable formals
       * instead of re-reading the buffer header. Besides avoiding a runtime
       * call, this keeps loop guards and indexed-access proofs on the same
       * SSA length value. Mutable formals retain the runtime query because
       * their backing list can change during the function. */
      if (length_local && length_local->arg_slot >= 0 &&
          length_local->list_len_arg_slot >= 0 &&
          length_local->list_len_slot >= 0 && !length_local->semantic_mutable)
        return nyr_load_local_value(b, length_local->list_len_slot);
      int list = nyr_lower_expr(b, length_target);
      return list < 0 ? -1
                      : nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1,
                                              -1, 1, 0);
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "pop") == 0 && e->as.memcall.args.len == 0 &&
        (!selected || ny_is_stdlib_tok(selected->tok)) &&
        nyr_expr_is_list(b, e->as.memcall.target)) {
      const expr_t *target = e->as.memcall.target;
      nyr_local_t *local = target->kind == NY_E_IDENT
                               ? nyr_find_local(b, target->as.ident.name)
                               : NULL;
      bool dynamic = nyr_expr_is_dyn_list(b, target);
      int list = nyr_lower_expr(b, target);
      int out =
          list < 0
              ? -1
              : nyr_emit_runtime_call(
                    b, dynamic ? "rt_tbuf_pop_any" : "rt_tbuf_pop_raw", list,
                    -1, -1, 1,
                    dynamic ? NYIR_SCALAR_I64_TAGGED : NYIR_SCALAR_I64_RAW);
      if (out < 0)
        return -1;
      int length = nyr_emit_known_list_pop_len(b, list);
      if (length < 0)
        length =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1, -1, 1, 0);
      if (length < 0 || !nyr_record_list_len_fact(b, list, length))
        return -1;
      if (local) {
        if (local->list_len_slot < 0)
          local->list_len_slot = b->next_local_slot++;
        if (!nyr_store_local_value(b, local->list_len_slot, length))
          return -1;
      }
      return out;
    }
    const expr_t *prior_get_receiver =
        nyr_attached_get_receiver(e->as.memcall.target);
    bool prior_get_is_dynamic_value =
        prior_get_receiver && (nyr_expr_is_any(b, prior_get_receiver) ||
                               nyr_expr_is_dict(b, prior_get_receiver));
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len >= 1 && e->as.memcall.args.len <= 2 &&
        nyr_expr_is_list(b, e->as.memcall.target) &&
        nyr_expr_is_dyn_list(b, e->as.memcall.target) &&
        !nyr_expr_is_any(b, e->as.memcall.target) &&
        !nyr_expr_is_dict(b, e->as.memcall.target) &&
        !prior_get_is_dynamic_value) {
      int list = nyr_lower_expr(b, e->as.memcall.target);
      int index = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      int fallback = e->as.memcall.args.len == 2
                         ? nyr_lower_expr(b, e->as.memcall.args.data[1].val)
                         : nyr_emit_const(b, 0);
      if (list < 0 || index < 0 || fallback < 0)
        return -1;
      const expr_t *fallback_expr =
          e->as.memcall.args.len == 2 ? e->as.memcall.args.data[1].val : NULL;
      const nyr_local_t *fallback_local =
          fallback_expr && fallback_expr->kind == NY_E_IDENT &&
                  fallback_expr->as.ident.name
              ? nyr_find_local_nearest(b, fallback_expr->as.ident.name)
              : NULL;
      bool fallback_is_closure =
          fallback_expr &&
          (fallback_expr->kind == NY_E_FN ||
           fallback_expr->kind == NY_E_LAMBDA ||
           fallback_expr->semantic.rep == NY_SEM_REP_CLOSURE ||
           (fallback_expr->kind == NY_E_IDENT && fallback_expr->as.ident.name &&
            ((!fallback_local &&
              nyr_find_user_function(b, fallback_expr->as.ident.name)) ||
             (fallback_local &&
              fallback_local->semantic_rep == NY_SEM_REP_CLOSURE))));
      if (fallback_is_closure) {
        fallback = nyr_emit_runtime_call(
            b, "rt_mark_dynamic_callable_tagged_args", fallback, -1, -1, 1, 0);
      } else if (nyr_expr_is_bool(b, fallback_expr)) {
        fallback = nyr_box_bool(b, fallback);
      } else if (nyr_expr_is_f64(b, fallback_expr) ||
                 nyr_expr_is_f32(b, fallback_expr)) {
        int as_f64 = nyr_expr_is_f32(b, fallback_expr)
                         ? nyr_emit_f32_to_f64(b, fallback)
                         : fallback;
        int bits = as_f64 < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                      -1, -1, 1, 0);
        fallback = bits < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                    -1, -1, 1, 0);
      } else if (nyr_expr_is_cstr(b, fallback_expr)) {
        fallback =
            nyr_emit_runtime_call(b, "rt_cstr_to_str", fallback, -1, -1, 1, 0);
      } else if (!nyr_expr_is_list(b, fallback_expr) &&
                 !nyr_expr_is_dict(b, fallback_expr) &&
                 !nyr_expr_is_bytes(b, fallback_expr) &&
                 !nyr_expr_is_tuple(b, fallback_expr) &&
                 !ny_expr_is_nil_literal(fallback_expr)) {
        fallback = nyr_box_raw_dynamic_value(b, fallback);
      }
      if (index < 0)
        return -1;
      nyr_scalar_class_t index_class = nyr_value_scalar_class(b, index);
      if (index_class == NYIR_SCALAR_I64_TAGGED) {
        index = nyr_emit_runtime_call(b, "rt_any_to_i64", index, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      }
      if (index < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_value_get_index_raw", list, index,
                                   fallback, 3, NYIR_SCALAR_I64_TAGGED);
    }
    /* A resolved attached method is more specific than a global function
     * with the same leaf.  Resolve it before namespace/value probing, which
     * can otherwise mistake `left.identity()` for the free `identity` when
     * both declarations are visible in the merged program. */
    if (e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED &&
        e->semantic.canonical_callee_stmt &&
        e->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
        e->semantic.canonical_callee_stmt->as.fn.name && e->as.memcall.target &&
        e->as.memcall.args.len < NYIR_CALL_MAX_ARGS) {
      const stmt_t *attached = e->semantic.canonical_callee_stmt;
      call_arg_t args[NYIR_CALL_MAX_ARGS];
      args[0] = (call_arg_t){.val = e->as.memcall.target};
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        args[i + 1] = e->as.memcall.args.data[i];
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = attached->as.fn.name;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic = e->semantic;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.as.call.callee = &callee;
      call.as.call.args.data = args;
      call.as.call.args.len = call.as.call.args.cap =
          e->as.memcall.args.len + 1;
      return nyr_lower_expr(b, &call);
    }
    /*
     * Zero-argument module calls are still calls, not function values. The
     * namespace-value helper operates on member expressions; using a
     * NY_E_MEMBER probe here resolves an imported declaration without
     * confusing `module.fn()` with the first-class value `module.fn`.
     */
    expr_t namespace_member = {.kind = NY_E_MEMBER, .tok = e->tok};
    namespace_member.as.member.target = e->as.memcall.target;
    namespace_member.as.member.name = e->as.memcall.name;
    namespace_member.semantic = e->semantic;
    const stmt_t *namespace_function =
        nyr_namespace_function_value(b, &namespace_member);
    if (namespace_function && namespace_function->as.fn.name) {
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = namespace_function->as.fn.name;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic = e->semantic;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.semantic.canonical_callee = namespace_function->as.fn.name;
      call.semantic.canonical_callee_stmt = (stmt_t *)namespace_function;
      call.as.call.callee = &callee;
      call.as.call.args = e->as.memcall.args;
      return nyr_lower_expr(b, &call);
    }
    const stmt_t *source_method = nyr_source_attached_result(b, e);
    if (source_method && e->as.memcall.args.len < NYIR_CALL_MAX_ARGS) {
      call_arg_t args[NYIR_CALL_MAX_ARGS];
      args[0] = (call_arg_t){.val = e->as.memcall.target};
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        args[i + 1] = e->as.memcall.args.data[i];
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = source_method->as.fn.name;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic.canonical_callee_stmt = (stmt_t *)source_method;
      call.semantic.requires_proven_by_caller =
          e->semantic.requires_proven_by_caller;
      call.semantic.requires_check_custom_operator =
          e->semantic.requires_check_custom_operator;
      call.as.call.callee = &callee;
      call.as.call.args.data = args;
      call.as.call.args.len = call.as.call.args.cap =
          e->as.memcall.args.len + 1;
      return nyr_lower_expr(b, &call);
    }
    char namespace_name[512];
    const expr_t *namespace_root = e->as.memcall.target;
    while (namespace_root && namespace_root->kind == NY_E_MEMBER)
      namespace_root = namespace_root->as.member.target;
    bool namespace_shadowed = false;
    if (namespace_root && namespace_root->kind == NY_E_IDENT &&
        namespace_root->as.ident.name) {
      const expr_t *global_value = nyr_find_bound_top_level_value(
          b, namespace_root->as.ident.name, namespace_root->tok.filename);
      namespace_shadowed =
          nyr_find_local(b, namespace_root->as.ident.name) != NULL ||
          global_value != NULL;
    }
    if (!namespace_shadowed && e->as.memcall.name &&
        nyr_qualified_expr(e->as.memcall.target, namespace_name,
                           sizeof(namespace_name))) {
      size_t used = strlen(namespace_name);
      size_t method_len = strlen(e->as.memcall.name);
      if (used + method_len + 2 <= sizeof(namespace_name)) {
        namespace_name[used++] = '.';
        memcpy(namespace_name + used, e->as.memcall.name, method_len + 1);
        const stmt_t *fn = nyr_find_user_function(b, namespace_name);
        if (fn && !ny_is_stdlib_tok(fn->tok)) {
          expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
          callee.as.ident.name = namespace_name;
          expr_t call = *e;
          call.kind = NY_E_CALL;
          call.as.call.callee = &callee;
          call.as.call.args = e->as.memcall.args;
          call.semantic.member_call_kind = NY_SEM_CALL_NONE;
          call.semantic.canonical_callee = NULL;
          call.semantic.canonical_callee_stmt = NULL;
          return nyr_lower_call(b, &call);
        }
      }
    }
    /* `module_alias.fn(...)` is a namespace call, never a receiver call.
     * Semantic dispatch can annotate the same syntax as an attached method;
     * canonicalize it before the dynamic-call path adds the alias as an ABI
     * argument and emits an unresolved `lea module_alias`. */
    const expr_t *alias_expr = e->as.memcall.target;
    while (alias_expr) {
      if (alias_expr->kind == NY_E_MEMBER) {
        alias_expr = alias_expr->as.member.target;
        continue;
      }
      if (alias_expr->kind == NY_E_SPLICE) {
        alias_expr = alias_expr->as.splice.expr;
        continue;
      }
      break;
    }
    if (alias_expr && alias_expr->kind == NY_E_IDENT &&
        alias_expr->as.ident.name && e->as.memcall.name) {
      const char *alias = alias_expr->as.ident.name;
      const expr_t *global_value =
          nyr_find_bound_top_level_value(b, alias, alias_expr->tok.filename);
      const bool bound_value =
          nyr_find_local(b, alias) != NULL || global_value != NULL;
      const char *module = bound_value ? NULL : nyr_resolve_use_alias(b, alias);
      if (!module && !bound_value) {
        nyr_builder_t probe = *b;
        probe.source_file = NULL;
        probe.locals = NULL;
        probe.local_count = probe.local_cap = 0;
        module = nyr_resolve_use_alias(&probe, alias);
      }
      if (module) {
        char canonical[512];
        int n = snprintf(canonical, sizeof(canonical), "%s.%s", module,
                         e->as.memcall.name);
        if (n <= 0 || (size_t)n >= sizeof(canonical)) {
          return nyr_fail(b, "native NYIR lower: module call name is too long"),
                 -1;
        }
        expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
        callee.as.ident.name = canonical;
        expr_t call = *e;
        call.kind = NY_E_CALL;
        call.semantic.member_call_kind = NY_SEM_CALL_NONE;
        call.semantic.canonical_callee = NULL;
        call.semantic.canonical_callee_stmt = NULL;
        call.as.call.callee = &callee;
        call.as.call.args = e->as.memcall.args;
        return nyr_lower_expr(b, &call);
      }
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "get") == 0 &&
        (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2) &&
        nyr_expr_is_list(b, e->as.memcall.target)) {
      const expr_t *index_expr = NULL;
      const expr_t *fallback_expr = NULL;
      size_t positional = 0;
      for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
        const call_arg_t *arg = &e->as.memcall.args.data[i];
        if (arg->name) {
          if (strcmp(arg->name, "key") == 0)
            index_expr = arg->val;
          else if (strcmp(arg->name, "default") == 0)
            fallback_expr = arg->val;
        } else if (positional++ == 0) {
          index_expr = arg->val;
        } else {
          fallback_expr = arg->val;
        }
      }
      if (!index_expr)
        return nyr_fail(b, "list.get requires a key argument"), -1;
      int list = nyr_lower_expr(b, e->as.memcall.target);
      int index = nyr_lower_expr(b, index_expr);
      int fallback = fallback_expr ? nyr_lower_expr(b, fallback_expr)
                                   : nyr_emit_const(b, 0);
      if (list < 0 || index < 0 || fallback < 0)
        return -1;
      bool dynamic_get = nyr_expr_is_dyn_list(b, e->as.memcall.target) ||
                         nyr_expr_is_any(b, e) || b->force_dynamic_get_any;
      /* The fallback lane is an i64 in both accessors. Dynamic buffers need
       * a boxed Ny float; typed scalar buffers store f64 payload bits and
       * therefore need the unboxed bit pattern. Never pass an XMM f64
       * register to the integer fallback lane. */
      bool fallback_f32 = fallback_expr && (nyr_expr_is_f32(b, fallback_expr) ||
                                            nyr_value_is_f32(b, fallback, 0));
      bool fallback_f64 = fallback_expr && (nyr_expr_is_f64(b, fallback_expr) ||
                                            nyr_value_is_f64(b, fallback, 0));
      if (fallback_f32 || fallback_f64) {
        int as_f64 = fallback_f32 ? nyr_emit_f32_to_f64(b, fallback) : fallback;
        int bits = as_f64 < 0
                       ? -1
                       : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64, -1, -1,
                                               1, NYIR_SCALAR_I64_RAW);
        fallback = bits < 0      ? -1
                   : dynamic_get ? nyr_emit_runtime_call(b, "rt_flt_box_val",
                                                         bits, -1, -1, 1, 0)
                                 : bits;
        if (fallback < 0)
          return -1;
      }
      /*
       * The source-level fallback is already in dynamic ABI form here. The
       * lower-level tbuf helper takes a raw fallback, so use the shared
       * representation-aware index bridge for dynamic results; it normalizes
       * tagged integer fallbacks and preserves booleans, floats, and handles.
       */
      const char *get_runtime =
          dynamic_get ? "rt_value_get_index_raw" : "rt_tbuf_get";
      int value = nyr_emit_runtime_call(
          b, get_runtime, list, index, fallback, 3,
          dynamic_get ? NYIR_SCALAR_I64_TAGGED : NYIR_SCALAR_UNKNOWN);
      /*
       * A nil default means the caller accepts a missing element: the result
       * is dynamic (value or nil), never a guaranteed f64.  Decoding it as
       * f64 turned `args.get(1, nil)` in a float-building context into an
       * f64-classified word, and the following `y == nil` compare then fed
       * that register to rt_any_eq and tripped the scalar-class verifier.
       */
      bool nil_default = e->as.memcall.args.len == 2 && fallback_expr &&
                         fallback_expr->tok.kind == NY_T_NIL;
      bool wants_f64 = !nil_default && !dynamic_get &&
                       (nyr_expr_is_f64(b, e) ||
                        (fallback_expr && nyr_expr_is_f64(b, fallback_expr)));
      if (wants_f64) {
        return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                     NYIR_SCALAR_F64);
      }
      if (!dynamic_get)
        return value;
      int tag = value < 0 ? -1
                          : nyr_emit_runtime_call(b, "rt_tbuf_tag", list, index,
                                                  -1, 2, 0);
      if (value < 0 || tag < 0 ||
          !nyr_record_dyn_fact(b, value, NYR_FACT_DYN_TAG, tag))
        return -1;
      return value;
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        (strcmp(e->as.memcall.name, "long") == 0 ||
         strcmp(e->as.memcall.name, "__long") == 0) &&
        e->as.memcall.args.len == 0) {
      /* int.long builds a BigInt.  rt_long is the dynamic-normalization
       * boundary (it decodes immediate integers), so a proven raw-int
       * receiver must take the raw scalar bridge rt_bigint_from_int or the
       * value halves (123.long read 61).  Non-scalar receivers (strings,
       * containers, any locals) keep the dynamic entry point. */
      const expr_t *recv = e->as.memcall.target;
      bool recv_raw =
          recv->semantic.resolved && recv->semantic.rep == NY_SEM_REP_RAW_INT;
      if (recv->kind == NY_E_LITERAL && recv->as.literal.kind == NY_LIT_INT &&
          recv->tok.kind != NY_T_NIL)
        recv_raw = true;
      if (recv->kind == NY_E_IDENT && recv->as.ident.name) {
        const nyr_local_t *rl = nyr_find_local(b, recv->as.ident.name);
        if (nyr_long_receiver_is_raw_word(rl))
          recv_raw = true;
      }
      int target = nyr_lower_expr(b, recv);
      if (target < 0)
        return -1;
      if (recv->kind == NY_E_IDENT && recv->as.ident.name) {
        const nyr_local_t *rl = nyr_find_local(b, recv->as.ident.name);
        if (rl && rl->is_list && rl->list_len_slot >= 0) {
          int count = nyr_load_local_value(b, rl->list_len_slot);
          if (count < 0)
            return -1;
          return nyr_emit_runtime_call(b, "rt_long_list_raw", target, count, -1,
                                       2, 0);
        }
      }
      /* A list payload unpacked from a typed sum type still has its native
       * tbuf representation, but its pattern binding may not carry a cached
       * length slot. Recover the count from the list header instead of
       * passing that raw pointer to rt_long's tagged-value ABI. */
      if (nyr_expr_is_list(b, recv) && !nyr_expr_is_dyn_list(b, recv)) {
        int count =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", target, -1, -1, 1, 0);
        return count < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_long_list_raw", target,
                                                 count, -1, 2, 0);
      }
      if (recv_raw && !nyr_expr_is_any(b, recv) && !nyr_expr_is_cstr(b, recv) &&
          !nyr_expr_is_bigint(b, recv) && !nyr_expr_is_list(b, recv) &&
          !nyr_expr_is_dict(b, recv))
        return nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", target, -1,
                                     -1, 1, 0);
      return nyr_emit_runtime_call(b, "rt_long", target, -1, -1, 1, 0);
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "extend") == 0 &&
        e->as.memcall.args.len == 1 &&
        nyr_expr_is_list(b, e->as.memcall.target) &&
        nyr_expr_is_list(b, e->as.memcall.args.data[0].val)) {
      int list = nyr_lower_expr(b, e->as.memcall.target);
      int other = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      if (list < 0 || other < 0)
        return -1;
      int out =
          nyr_emit_runtime_call(b, "rt_tbuf_extend", list, other, -1, 2, 0);
      return out < 0 ? -1 : out;
    }
    nyr_local_t *append_local =
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
                e->as.memcall.target->as.ident.name
            ? nyr_find_local(b, e->as.memcall.target->as.ident.name)
            : NULL;
    if (e->as.memcall.target &&
        ((append_local && append_local->is_list) ||
         e->as.memcall.target->kind == NY_E_CALL ||
         e->as.memcall.target->kind == NY_E_MEMCALL) &&
        e->as.memcall.name && strcmp(e->as.memcall.name, "append") == 0 &&
        e->as.memcall.args.len == 1) {
      int list = nyr_lower_expr(b, e->as.memcall.target);
      const expr_t *append_value = e->as.memcall.args.data[0].val;
      bool append_value_is_async =
          append_value && append_value->kind == NY_E_UNARY &&
          append_value->as.unary.op &&
          strcmp(append_value->as.unary.op, "async") == 0;
      /* A value read from an `any`/dynamic sequence is already in the
       * canonical tagged ABI.  Preserve that representation while lowering
       * an append consumer; ordinary scalar consumers still decode it. */
      bool append_value_for_any = nyr_expr_is_any(b, append_value) ||
                                  nyr_expr_is_dict(b, append_value) ||
                                  nyr_expr_is_list(b, append_value) ||
                                  nyr_expr_is_bytes(b, append_value) ||
                                  append_value_is_async;
      bool saved_value_for_any_call = b->value_for_any_call;
      if (append_value_for_any)
        b->value_for_any_call = true;
      int value = nyr_lower_expr(b, append_value);
      b->value_for_any_call = saved_value_for_any_call;
      if (list < 0 || value < 0)
        return -1;
      bool append_is_string = nyr_expr_is_cstr(b, append_value);
      if (!append_is_string && (append_value->kind == NY_E_CALL ||
                                append_value->kind == NY_E_MEMCALL)) {
        const char *return_type = nyr_expr_type_name(b, append_value);
        append_is_string =
            (return_type && (strcmp(return_type, "str") == 0 ||
                             strcmp(return_type, "string") == 0)) ||
            (append_value->semantic.resolved &&
             append_value->semantic.rep == NY_SEM_REP_STRING &&
             (!return_type || !*return_type ||
              ny_sem_rep_coherent_with_type(return_type,
                                            append_value->semantic.rep)));
      }
      if (append_is_string) {
        value = nyr_emit_runtime_call(b, "rt_cstr_to_str", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
      }
      int is_string = nyr_emit_const(b, append_is_string ? 1 : 0);
      const nyr_local_t *append_value_local =
          append_value && append_value->kind == NY_E_IDENT
              ? nyr_find_local(b, append_value->as.ident.name)
              : NULL;
      const char *append_symbol =
          (append_is_string ||
           (append_value_local && append_value_local->is_any) ||
           nyr_expr_is_any(b, append_value) ||
           nyr_expr_is_f64(b, append_value) ||
           nyr_expr_is_f32(b, append_value) ||
           nyr_expr_is_dict(b, append_value) ||
           nyr_expr_is_list(b, append_value) ||
           nyr_expr_is_bytes(b, append_value) || append_value_is_async ||
           (append_local && append_local->is_dyn_list)) &&
                  !nyr_expr_is_raw_dynamic_read(b, append_value)
              ? "rt_tbuf_append_tagged"
              : "rt_tbuf_append_raw";
      /* Raw integer appends have a dedicated runtime entry point.  Proven
       * integers cannot need the string-provenance check, and this call also
       * avoids passing append_raw's unused third ABI argument. */
      bool append_value_is_raw_integer =
          append_value &&
          ((append_value->kind == NY_E_LITERAL &&
            append_value->as.literal.kind == NY_LIT_INT &&
            append_value->tok.kind != NY_T_NIL) ||
           (append_value->semantic.resolved &&
            append_value->semantic.rep == NY_SEM_REP_RAW_INT) ||
           (append_value_local &&
            append_value_local->semantic_rep == NY_SEM_REP_RAW_INT));
      if (!append_is_string && !nyr_expr_is_raw_dynamic_read(b, append_value) &&
          strcmp(append_symbol, "rt_tbuf_append_raw") == 0 &&
          append_value_is_raw_integer)
        append_symbol = "rt_tbuf_append_i64_raw";
      if (strcmp(append_symbol, "rt_tbuf_append_tagged") == 0) {
        bool is_bool_val = nyr_expr_is_bool(b, append_value);
        if (append_value_local && append_value_local->is_bool)
          is_bool_val = true;
        if (is_bool_val) {
          value = nyr_box_bool(b, value);
        } else {
          bool raw_integer =
              append_value &&
              ((append_value->kind == NY_E_LITERAL &&
                append_value->as.literal.kind == NY_LIT_INT &&
                append_value->tok.kind != NY_T_NIL) ||
               (append_value->semantic.resolved &&
                append_value->semantic.rep == NY_SEM_REP_RAW_INT) ||
               (append_value_local &&
                append_value_local->semantic_rep == NY_SEM_REP_RAW_INT));
          if (raw_integer) {
            value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
          } else if (nyr_expr_is_f64(b, append_value) ||
                     nyr_expr_is_f32(b, append_value)) {
            int bits =
                nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
            value = bits < 0 ? -1
                             : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                     -1, -1, 1, 0);
          }
        }
        if (value < 0)
          return -1;
      }
      /*
       * The raw append stores the value in an elem-8 slot as raw machine
       * bits.  A float producer reaches this call as an f64-classified NYIR
       * value; forwarding it directly trips the call scalar-class verifier
       * (f64 producer, non-float formal) and would pass the bits through a
       * floating register at the machine layer.  Reinterpret once here so
       * the slot word is an integer-class IEEE bit pattern, mirroring what
       * the elem-8 fast read arm loads back.
       */
      if (strcmp(append_symbol, "rt_tbuf_append_raw") == 0 &&
          nyr_expr_is_f64(b, append_value)) {
        int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
        if (bits < 0)
          return -1;
        value = bits;
      }
      bool append_i64 = strcmp(append_symbol, "rt_tbuf_append_i64_raw") == 0;
      int out = append_i64      ? nyr_emit_runtime_call(b, append_symbol, list,
                                                        value, -1, 2, 0)
                : is_string < 0 ? -1
                                : nyr_emit_runtime_call(b, append_symbol, list,
                                                        value, is_string, 3, 0);
      int length = nyr_emit_known_list_append_len(b, list, out);
      if (out < 0 || length < 0 || !nyr_record_list_len_fact(b, out, length))
        return -1;
      /* A dynamic append can change a compact list into a descriptor-backed
       * sequence.  Keep that fact on the receiver so a later index does not
       * decode a string/object payload as a raw integer. */
      if (append_local && append_local->is_list &&
          strcmp(append_symbol, "rt_tbuf_append_tagged") == 0)
        append_local->is_dyn_list = true;
      if (e->as.memcall.target->kind == NY_E_IDENT) {
        nyr_local_t *target_local =
            nyr_find_local(b, e->as.memcall.target->as.ident.name);
        if (target_local) {
          target_local->is_list = true;
          if (target_local->list_len_slot < 0)
            target_local->list_len_slot = b->next_local_slot++;
          if (!nyr_store_local_value(b, target_local->list_len_slot, length))
            return -1;
          if (!nyr_store_local_value(b, target_local->slot, out))
            return -1;
          /* An untyped `mut xs = []` local stays on the dynamic accessor
           * path only because its element type is unknown.  The first
           * raw-int append settles that question: an empty list that only
           * ever received raw ints is the elem-8 scalar buffer the fast
           * accessors already target.  Promote the receiver once — the
           * empty-literal proof is consumed here, and any tagged append
           * above re-marks the list dynamic — so indexed loops over
           * append-built int lists stop paying the tagged element ABI on
           * every access. */
          if (target_local->is_dyn_list &&
              (strcmp(append_symbol, "rt_tbuf_append_raw") == 0 ||
               strcmp(append_symbol, "rt_tbuf_append_i64_raw") == 0) &&
              !append_is_string && append_value->kind != NY_E_CALL &&
              append_value->kind != NY_E_MEMCALL &&
              append_value->kind != NY_E_LIST &&
              !nyr_expr_is_f64(b, append_value) &&
              !nyr_expr_is_f32(b, append_value) &&
              target_local->list_started_empty) {
            target_local->is_dyn_list = false;
            target_local->is_any = false;
            target_local->type_name = "list<int>";
            target_local->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
            target_local->list_started_empty = false;
          }
        }
      }
      return out;
    }
    if (!e->as.memcall.target || !e->as.memcall.name) {
      return nyr_fail(b,
                      "native NYIR lower: member call has no target or method"),
             -1;
    }
    bool target_is_ident = e->as.memcall.target->kind == NY_E_IDENT &&
                           e->as.memcall.target->as.ident.name;
    nyr_local_t *local =
        target_is_ident ? nyr_find_local(b, e->as.memcall.target->as.ident.name)
                        : NULL;
    const char *method = e->as.memcall.name;
    const char *dynamic_target_ident = e->as.memcall.target->kind == NY_E_IDENT
                                           ? e->as.memcall.target->as.ident.name
                                           : NULL;
    bool dynamic_target_is_module =
        dynamic_target_ident && !local &&
        nyr_resolve_use_alias(b, dynamic_target_ident) != NULL;
    if (strcmp(method, "get") == 0 && e->as.memcall.args.len >= 1 &&
        e->as.memcall.args.len <= 2 && !dynamic_target_is_module &&
        nyr_expr_is_bytes(b, e->as.memcall.target)) {
      int value = nyr_lower_expr(b, e->as.memcall.target);
      int key = (nyr_expr_is_dict(b, e->as.memcall.target) ||
                 (local && local->is_dict))
                    ? nyr_lower_dict_key(b, e->as.memcall.args.data[0].val)
                    : nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      int fallback = e->as.memcall.args.len == 2
                         ? nyr_lower_expr(b, e->as.memcall.args.data[1].val)
                         : nyr_emit_const(b, 0);
      return value < 0 || key < 0 || fallback < 0
                 ? -1
                 : nyr_emit_runtime_call(b, "rt_bytes_get_raw", value, key,
                                         fallback, 3, 0);
    }
    if (strcmp(method, "get") == 0 && e->as.memcall.args.len >= 1 &&
        e->as.memcall.args.len <= 2 && !dynamic_target_is_module &&
        nyr_expr_is_cstr(b, e->as.memcall.target)) {
      int value = nyr_lower_expr(b, e->as.memcall.target);
      int key = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      int fallback = e->as.memcall.args.len == 2
                         ? nyr_lower_expr(b, e->as.memcall.args.data[1].val)
                         : nyr_emit_const(b, 0);
      if (value < 0 || key < 0 || fallback < 0)
        return -1;
      if (nyr_value_scalar_class(b, key) == NYIR_SCALAR_I64_TAGGED) {
        key = nyr_emit_runtime_call(b, "rt_any_to_i64", key, -1, -1, 1,
                                    NYIR_SCALAR_I64_RAW);
        if (key < 0)
          return -1;
      }
      return nyr_emit_runtime_call(b, "rt_value_get_index_raw", value, key,
                                   fallback, 3, NYIR_SCALAR_I64_TAGGED);
    }
    if (strcmp(method, "get") == 0 && e->as.memcall.args.len >= 1 &&
        e->as.memcall.args.len <= 2 && !dynamic_target_is_module) {
      /* No compile-time dict-literal folding here.  A `def` binding only
       * prevents rebinding, so the dict object can still gain or change
       * entries through set_idx/dict_set after its initializer runs, and a
       * folded read would observe stale values.  The runtime bridge below
       * reads the live dictionary and returns the canonical dynamic word. */
      int value = nyr_lower_expr(b, e->as.memcall.target);
      bool target_is_list = nyr_expr_is_list(b, e->as.memcall.target) ||
                            nyr_expr_is_dyn_list(b, e->as.memcall.target) ||
                            (local && local->is_list);
      bool target_is_tuple = nyr_expr_is_tuple(b, e->as.memcall.target);
      if (!target_is_list && !local &&
          e->as.memcall.target->kind == NY_E_IDENT &&
          e->as.memcall.target->as.ident.name) {
        const expr_t *global =
            nyr_find_top_level_value(b, e->as.memcall.target->as.ident.name);
        target_is_list = global && global != e->as.memcall.target &&
                         nyr_expr_is_list(b, global);
      }
      /* A tagged producer is not an object-class dictionary pointer merely
       * because source inference left no concrete sequence type. Nested
       * dynamic indices retain the runtime-checked member accessor ABI. */
      bool target_is_dict = nyr_expr_is_dict(b, e->as.memcall.target) ||
                            (local && local->is_dict);
      bool dynamic_target =
          (!target_is_dict &&
           nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED) ||
          target_is_tuple || e->as.memcall.target->kind == NY_E_CALL ||
          e->as.memcall.target->kind == NY_E_MEMCALL ||
          nyr_expr_is_dyn_list(b, e->as.memcall.target) ||
          /* An `any` local may hold a native tbuf even when flow inference
           * conservatively marks its current value as list-like.  The
           * generic tagged value-get entry point still consumes a tagged index
           * at that boundary, so retain the dynamic ABI for every any local. */
          (!target_is_dict && local && local->is_any);
      bool dict_like_target =
          !target_is_tuple && !nyr_expr_is_bytes(b, e->as.memcall.target) &&
          !target_is_list && !nyr_expr_is_range(b, e->as.memcall.target) &&
          !dynamic_target;
      int key = dict_like_target
                    ? nyr_lower_dict_key(b, e->as.memcall.args.data[0].val)
                    : nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      /* `rt_value_get_tagged` accepts the legacy tagged index ABI for dynamic
       * values, while NYIR lowers a raw native integer index.  Tag only this
       * dynamic boundary; typed tbuf/list access must keep its raw index. */
      const expr_t *key_arg = e->as.memcall.args.data[0].val;
      bool key_is_lit_int = key_arg && key_arg->kind == NY_E_LITERAL &&
                            key_arg->as.literal.kind == NY_LIT_INT &&
                            key_arg->tok.kind != NY_T_NIL;
      bool key_is_raw_int =
          key_is_lit_int ||
          (key_arg && key_arg->semantic.rep == NY_SEM_REP_RAW_INT) ||
          (!dict_like_target && nyr_expr_is_integer_index(b, key_arg, 0));
      const char *key_type = nyr_expr_type_name(b, key_arg);
      key_is_raw_int =
          key_is_raw_int || (key_type && strcmp(key_type, "int") == 0);
      if (!key_is_raw_int && key_arg && key_arg->kind == NY_E_IDENT) {
        nyr_local_t *kl = nyr_find_local(b, key_arg->as.ident.name);
        if (kl && kl->semantic_rep == NY_SEM_REP_RAW_INT)
          key_is_raw_int = true;
      }
      bool key_is_raw_value =
          nyr_value_scalar_class(b, key) == NYIR_SCALAR_I64_RAW;
      if (!dict_like_target &&
          (key_is_raw_int || key_is_raw_value ||
           nyr_expr_is_dyn_list(b, e->as.memcall.target)) &&
          !nyr_expr_is_cstr(b, key_arg)) {
        nyr_scalar_class_t key_class = nyr_value_scalar_class(b, key);
        if (key_class == NYIR_SCALAR_F64)
          key = nyr_emit_f64_to_i64(b, key);
        else if (key_class == NYIR_SCALAR_F32)
          key = nyr_emit_f32_to_i64(b, key);
        if (key < 0)
          return -1;
        int one = nyr_emit_const(b, 1);
        int shifted =
            one < 0 ? -1
                    : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = key,
                                                        .b = one});
        key = shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
      }
      int fallback = e->as.memcall.args.len == 2
                         ? nyr_lower_expr(b, e->as.memcall.args.data[1].val)
                         : nyr_emit_const(b, 0);
      nyr_scalar_class_t fallback_class = nyr_value_scalar_class(b, fallback);
      if (e->as.memcall.args.len == 2 && (fallback_class == NYIR_SCALAR_F64 ||
                                          fallback_class == NYIR_SCALAR_F32)) {
        int as_f64 = fallback_class == NYIR_SCALAR_F32
                         ? nyr_emit_f32_to_f64(b, fallback)
                         : fallback;
        int bits = as_f64 < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                      -1, -1, 1, 0);
        fallback = bits < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                    -1, -1, 1, 0);
      } else if ((dict_like_target || dynamic_target ||
                  nyr_expr_is_cstr(b, key_arg)) &&
                 e->as.memcall.args.len == 2 &&
                 (!nyr_expr_is_any(b, e->as.memcall.args.data[1].val) ||
                  nyr_expr_has_raw_int_abi(b, e->as.memcall.args.data[1].val,
                                           0) ||
                  (e->as.memcall.args.data[1].val &&
                   e->as.memcall.args.data[1].val->kind == NY_E_LITERAL &&
                   e->as.memcall.args.data[1].val->as.literal.kind ==
                       NY_LIT_INT &&
                   e->as.memcall.args.data[1].val->tok.kind != NY_T_NIL))) {
        const expr_t *fallback_expr = e->as.memcall.args.data[1].val;
        if (nyr_expr_is_bool(b, fallback_expr)) {
          fallback = nyr_box_bool(b, fallback);
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
      if (value < 0 || key < 0 || fallback < 0)
        return -1;
      bool target_is_dyn_list =
          !prior_get_is_dynamic_value &&
          (nyr_expr_is_dyn_list(b, e->as.memcall.target) ||
           (local && local->is_dyn_list));
      if (target_is_dyn_list) {
        if (nyr_value_scalar_class(b, key) == NYIR_SCALAR_I64_TAGGED) {
          key = nyr_emit_runtime_call(b, "rt_any_to_i64", key, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
          if (key < 0)
            return -1;
        }
        return nyr_emit_runtime_call(b, "rt_tbuf_get_any", value, key, fallback,
                                     3, NYIR_SCALAR_I64_TAGGED);
      }
      /* Typed list/bytes sequence methods expose raw slot payloads. Using
       * the dynamic tagged accessor here misclassifies an odd raw byte such
       * as 3 as tagged(1), so keep the index and result on the raw path. */
      if (target_is_list && !dynamic_target && !target_is_dyn_list) {
        return nyr_emit_runtime_call(b, "rt_tbuf_index_read_raw", value, key,
                                     fallback, 3, 0);
      }
      int got = -1;
      bool folded_get = false;
      const expr_t *literal_dict =
          e->as.memcall.target->kind == NY_E_DICT ? e->as.memcall.target : NULL;
      if (dict_like_target && literal_dict &&
          nyr_dict_literal_get_foldable(literal_dict) && key_arg &&
          key_arg->kind == NY_E_LITERAL) {
        const char *key_str = NULL;
        int64_t key_int = 0;
        bool int_key = false;
        if (key_arg->as.literal.kind == NY_LIT_INT &&
            key_arg->tok.kind != NY_T_NIL &&
            key_arg->as.literal.as.i > -(INT64_C(1) << 62) &&
            key_arg->as.literal.as.i < (INT64_C(1) << 62)) {
          key_int = key_arg->as.literal.as.i;
          int_key = true;
        } else if (key_arg->as.literal.kind == NY_LIT_STR &&
                   key_arg->as.literal.as.s.data &&
                   memchr(key_arg->as.literal.as.s.data, '\0',
                          key_arg->as.literal.as.s.len) == NULL) {
          key_str = key_arg->as.literal.as.s.data;
        }
        if (int_key || key_str) {
          bool found = false;
          const expr_t *literal_value = nyr_dict_literal_lookup(
              literal_dict, key_str, key_int, int_key, &found);
          if (!found) {
            folded_get = true;
            /* The requested fallback has already been evaluated and boxed
             * above, so a miss returns precisely that value. */
            got = fallback;
          } else if (literal_value &&
                     literal_value->as.literal.kind == NY_LIT_BOOL) {
            folded_get = true;
            got = nyr_emit_const(
                b, literal_value->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE);
          } else if (literal_value) {
            folded_get = true;
            int raw = nyr_lower_expr(b, literal_value);
            got = raw < 0
                      ? -1
                      : nyr_emit_runtime_call(b, "rt_tag", raw, -1, -1, 1, 0);
          }
        }
      }
      if (folded_get && got < 0)
        return -1;
      if (!folded_get)
        got = nyr_emit_runtime_call(
            b,
            (dict_like_target && !nyr_expr_is_cstr(b, e->as.memcall.target))
                ? nyr_dict_get_symbol(b, e->as.memcall.args.data[0].val)
            : nyr_expr_is_cstr(b, e->as.memcall.args.data[0].val)
                ? "rt_dict_get_str_raw"
                : "rt_value_get_tagged",
            value, key, fallback, 3, 0);
      /* The enclosing return type does not describe a getter's value.
       * Dictionary reads stay tagged until their actual consumer requests a
       * string; otherwise numeric fields become text in string-returning
       * helpers such as an image encoder. */
      bool wants_cstr = e->as.memcall.args.len == 2 && nyr_expr_is_cstr(b, e);
      bool nil_default = e->as.memcall.args.len == 2 &&
                         e->as.memcall.args.data[1].val &&
                         e->as.memcall.args.data[1].val->tok.kind == NY_T_NIL;
      bool wants_f64 = !wants_cstr && !nil_default &&
                       e->as.memcall.args.len == 2 &&
                       (b->f64_context_depth != 0 ||
                        nyr_expr_is_f64(b, e->as.memcall.args.data[1].val));
      if (got >= 0 && wants_cstr)
        return nyr_emit_runtime_call(b, "rt_any_to_cstr", got, -1, -1, 1, 0);
      return wants_f64 ? nyr_emit_runtime_call(b, "rt_any_to_f64", got, -1, -1,
                                               1, NYIR_SCALAR_F64)
                       : got;
    }
    if (strcmp(method, "slice") == 0 &&
        (e->as.memcall.args.len == 2 ||
         (e->as.memcall.args.len == 3 && e->as.memcall.args.data[2].val &&
          e->as.memcall.args.data[2].val->kind == NY_E_LITERAL &&
          e->as.memcall.args.data[2].val->as.literal.kind == NY_LIT_INT &&
          e->as.memcall.args.data[2].val->as.literal.as.i == 1)) &&
        (nyr_expr_is_list(b, e->as.memcall.target) ||
         nyr_expr_is_cstr(b, e->as.memcall.target))) {
      bool string_slice = nyr_expr_is_cstr(b, e->as.memcall.target);
      int list = nyr_lower_expr(b, e->as.memcall.target);
      int start = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      int stop = nyr_lower_expr(b, e->as.memcall.args.data[1].val);
      int out = list < 0 || start < 0 || stop < 0
                    ? -1
                    : nyr_emit_runtime_call(
                          b, string_slice ? "rt_cstr_slice" : "rt_tbuf_slice",
                          list, start, stop, 3, 0);
      if (out < 0)
        return -1;
      if (string_slice)
        return out;
      int length =
          nyr_emit_runtime_call(b, "rt_tbuf_len_raw", out, -1, -1, 1, 0);
      return length < 0 || !nyr_record_list_len_fact(b, out, length) ? -1 : out;
    }
    /*
     * The language resolves methods on `any` through its attached interface.
     * Native lowering represents that interface as the same ordinary function
     * call: receiver first, then the source arguments. Do not speculate that
     * an `any` value is a dictionary.
     */
    if (strcmp(method, "contains") == 0 && e->as.memcall.args.len == 1 &&
        !dynamic_target_is_module) {
      int container = nyr_lower_expr(b, e->as.memcall.target);
      int item = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      return container < 0 || item < 0
                 ? -1
                 : nyr_emit_runtime_call(b, "rt_contains_raw", container, item,
                                         -1, 2, 0);
    }
    /* Native-only lowering may receive an unannotated member call even when
     * its concrete receiver and attached declaration are both available.
     * Select that declaration before the generic any-method fallback; e.g.
     * `a.add(b).value` must call `SelfBox.add`, not `any.add`. */
    if (e->semantic.member_call_kind == NY_SEM_CALL_NONE &&
        !dynamic_target_is_module && e->as.memcall.target) {
      const char *receiver_type = nyr_expr_type_name(b, e->as.memcall.target);
      const stmt_t *attached =
          nyr_find_attached_method(b, e->as.memcall.target, method);
      if (receiver_type && !ny_native_type_name_is_any(receiver_type) &&
          attached && attached->kind == NY_S_FUNC && attached->as.fn.name &&
          e->as.memcall.args.len + 1 <= NYIR_CALL_MAX_ARGS) {
        call_arg_t args[NYIR_CALL_MAX_ARGS];
        args[0] = (call_arg_t){.val = e->as.memcall.target};
        for (size_t i = 0; i < e->as.memcall.args.len; ++i)
          args[i + 1] = e->as.memcall.args.data[i];
        expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
        callee.as.ident.name = attached->as.fn.name;
        expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
        call.semantic.canonical_callee_stmt = (stmt_t *)attached;
        call.semantic.requires_proven_by_caller =
            e->semantic.requires_proven_by_caller;
        call.semantic.requires_check_custom_operator =
            e->semantic.requires_check_custom_operator;
        call.as.call.callee = &callee;
        call.as.call.args.data = args;
        call.as.call.args.len = e->as.memcall.args.len + 1;
        call.as.call.args.cap = call.as.call.args.len;
        return nyr_lower_expr(b, &call);
      }
    }
    bool has_canonical_attached =
        (e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT ||
         e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED) &&
        (e->semantic.canonical_callee || e->semantic.canonical_callee_stmt);
    if ((e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT ||
         e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED) &&
        (has_canonical_attached || !nyr_user_defined_fn(b, method)) &&
        !(local && local->is_dict) &&
        !nyr_expr_is_dict(b, e->as.memcall.target)) {
      if (e->as.memcall.args.len + 1 > NYIR_CALL_MAX_ARGS) {
        return nyr_fail(b,
                        "native NYIR lower: dynamic method call exceeds "
                        "the maximum supported argument count (%d)",
                        NYIR_CALL_MAX_ARGS),
               -1;
      }
      const char *canonical = e->semantic.canonical_callee;
      if (!canonical && e->semantic.canonical_callee_stmt)
        canonical = e->semantic.canonical_callee_stmt->as.fn.name;
      const stmt_t *attached_method =
          nyr_find_attached_method(b, e->as.memcall.target, method);
      /* Keep the exact declaration selected by attached-method lookup.  The
       * owner.method spelling is only a source-level convenience and can
       * disagree with the module-qualified symbol collected by lowering. */
      if (attached_method && attached_method->as.fn.name)
        canonical = attached_method->as.fn.name;
      const stmt_t *canonical_decl =
          attached_method ? attached_method : e->semantic.canonical_callee_stmt;
      if (canonical && canonical_decl && canonical_decl->kind == NY_S_FUNC &&
          ny_is_stdlib_tok(canonical_decl->tok)) {
        const stmt_t *fn = nyr_find_user_function(b, canonical);
        if (fn && fn->as.fn.name && ny_is_stdlib_tok(fn->tok))
          canonical = fn->as.fn.name;
      }
      if (!canonical) {
        return nyr_fail(b,
                        "native NYIR lower: semantic member target is "
                        "missing for '%s'",
                        method),
               -1;
      }
      /*
       * A module namespace reference spelled `alias.method(...)` (for
       * example `it.map` where `it` aliases `core.iter`) is resolved to
       * the canonical module function by the semantic layer, but the target
       * is not a runtime receiver.  Prepending it as the first argument
       * materializes the module registry and shifts every real argument out
       * of its parameter slot.  When the target is a bare identifier with no
       * local, function, or top-level value binding it cannot be a method
       * receiver, so build the call with the real arguments only.
       */
      const expr_t *m_target = e->as.memcall.target;
      bool namespace_target = false;
      if (m_target && m_target->kind == NY_E_IDENT && m_target->as.ident.name) {
        const char *m_rcv = m_target->as.ident.name;
        const expr_t *global_value =
            nyr_find_bound_top_level_value(b, m_rcv, m_target->tok.filename);
        namespace_target = (!nyr_find_local(b, m_rcv) &&
                            nyr_resolve_use_alias(b, m_rcv) != NULL) ||
                           (!nyr_find_local(b, m_rcv) &&
                            !nyr_find_user_function(b, m_rcv) && !global_value);
      }
      call_arg_t args[NYIR_CALL_MAX_ARGS];
      size_t arg_count;
      if (namespace_target) {
        for (size_t i = 0; i < e->as.memcall.args.len; ++i)
          args[i] = e->as.memcall.args.data[i];
        arg_count = e->as.memcall.args.len;
      } else {
        args[0] = (call_arg_t){.val = (expr_t *)m_target};
        for (size_t i = 0; i < e->as.memcall.args.len; ++i)
          args[i + 1] = e->as.memcall.args.data[i];
        arg_count = e->as.memcall.args.len + 1;
      }
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = canonical;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic = e->semantic;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.semantic.canonical_callee = NULL;
      call.semantic.canonical_callee_stmt = NULL;
      call.as.call.callee = &callee;
      call.as.call.args.data = args;
      call.as.call.args.len = arg_count;
      call.as.call.args.cap = arg_count;
      return nyr_lower_expr(b, &call);
    }
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        (nyr_expr_is_bytes(b, e->as.memcall.target) ||
         (local && local->is_bytes))) {
      int target = nyr_lower_expr(b, e->as.memcall.target);
      if (target < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_len", target, -1, -1, 1, 0);
    }
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        nyr_expr_is_cstr(b, e->as.memcall.target)) {
      int target = nyr_lower_expr(b, e->as.memcall.target);
      if (target < 0)
        return -1;
      int dynamic_length = nyr_peek_dyn_fact(b, target, NYR_FACT_DYN_STR_LEN);
      if (dynamic_length >= 0)
        return dynamic_length;
      return nyr_emit_runtime_call(b, "rt_cstr_len", target, -1, -1, 1, 0);
    }
    if (e->as.memcall.target && strcmp(method, "append") == 0 &&
        e->as.memcall.args.len == 1) {
      int list = nyr_lower_expr(b, e->as.memcall.target);
      const expr_t *append_value = e->as.memcall.args.data[0].val;
      int value = nyr_lower_expr(b, append_value);
      if (list < 0 || value < 0)
        return -1;
      /*
       * The free-function append path boxes f64/f32 values to handles
       * before dynamic append; without it the value crosses as raw IEEE
       * bits and backends disagree on the registers (the owned object
       * emitter passes XMM while the runtime reads GP), so method
       * appends of floats printed garbage.  Mirror that handling here,
       * except for declared typed-float lists whose readers consume raw
       * bits through the scalar path.
       */
      bool append_value_f64 = nyr_expr_is_f64(b, append_value);
      bool append_value_f32 = nyr_expr_is_f32(b, append_value);
      bool append_target_declared_float = false;
      if ((append_value_f64 || append_value_f32) &&
          e->as.memcall.target->kind == NY_E_IDENT &&
          e->as.memcall.target->as.ident.name) {
        const nyr_local_t *target_local =
            nyr_find_local(b, e->as.memcall.target->as.ident.name);
        const char *target_type = target_local ? target_local->type_name : NULL;
        append_target_declared_float =
            target_local && target_local->is_list && !target_local->is_any &&
            target_type != NULL &&
            (strstr(target_type, "<float>") != NULL ||
             strstr(target_type, "<f32>") != NULL ||
             strstr(target_type, "<f64>") != NULL ||
             strstr(target_type, "float[]") != NULL ||
             strstr(target_type, "f32[]") != NULL ||
             strstr(target_type, "f64[]") != NULL);
      }
      if ((append_value_f64 || append_value_f32) &&
          !append_target_declared_float) {
        int f64v = value;
        if (append_value_f32) {
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
      int out = nyr_emit_runtime_call(b, "rt_append", list, value, -1, 2, 0);
      int length = nyr_emit_known_list_append_len(b, list, out);
      if (out < 0 || length < 0 || !nyr_record_list_len_fact(b, out, length))
        return -1;
      if (e->as.memcall.target->kind == NY_E_IDENT) {
        nyr_local_t *target_local =
            nyr_find_local(b, e->as.memcall.target->as.ident.name);
        if (target_local) {
          target_local->is_list = true;
          if (target_local->list_len_slot < 0)
            target_local->list_len_slot = b->next_local_slot++;
          if (!nyr_store_local_value(b, target_local->list_len_slot, length))
            return -1;
          if (!nyr_store_local_value(b, target_local->slot, out))
            return -1;
        }
      }
      return out;
    }
    /*
     * `list.len()` member call: same rationale as the free len(list) call —
     * the stdlib body dispatches on interpreter tags and reports 0 for a
     * native tbuf.  Route known lists to the tbuf length directly.
     */
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        !nyr_expr_is_bytes(b, e->as.memcall.target) &&
        (!(local && local->is_bytes)) &&
        ((local && local->is_list) ||
         nyr_expr_is_list(b, e->as.memcall.target) ||
         nyr_expr_is_dyn_list(b, e->as.memcall.target))) {
      int list = nyr_lower_expr(b, e->as.memcall.target);
      if (list < 0)
        return -1;
      int known_len = nyr_peek_list_len_fact(b, list);
      if (known_len >= 0)
        return known_len;
      return nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1, -1, 1, 0);
    }
    /* Unknown identifier receivers keep the strict dynamic ABI.  Their `.len`
     * operation must reject scalar values instead of silently returning zero.
     */
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        !nyr_expr_is_list(b, e->as.memcall.target) &&
        !nyr_expr_is_dyn_list(b, e->as.memcall.target) &&
        !nyr_expr_is_bytes(b, e->as.memcall.target) &&
        !nyr_expr_is_cstr(b, e->as.memcall.target) &&
        !nyr_expr_is_range(b, e->as.memcall.target)) {
      int value = nyr_lower_expr(b, e->as.memcall.target);
      return value < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_len_strict", value, -1,
                                               -1, 1, 0);
    }
    /* Untyped `any` receivers keep the dynamic ABI.  Their `.len` operation
     * still needs to understand native tbuf/list/string handles, but must not
     * force the function parameter itself into a list ABI (callbacks are
     * invoked through a two-argument dynamic call path). */
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        ((local && local->is_any) ||
         nyr_expr_is_any(b, e->as.memcall.target))) {
      int value = nyr_lower_expr(b, e->as.memcall.target);
      return value < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_len_strict", value, -1,
                                               -1, 1, 0);
    }
    if (!nyr_expr_is_bytes(b, e->as.memcall.target) &&
        (nyr_expr_is_list(b, e->as.memcall.target) ||
         (local && local->is_list && !local->is_any)) &&
        strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
      int list = nyr_lower_expr(b, e->as.memcall.target);
      int index = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      int value = nyr_lower_expr(b, e->as.memcall.args.data[1].val);
      if (list < 0 || index < 0 || value < 0)
        return -1;
      if (nyr_expr_is_f64(b, e->as.memcall.args.data[1].val)) {
        value = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1, 0);
        if (value < 0)
          return -1;
        return nyr_emit_runtime_call(b, "rt_tbuf_set_f64_bits", list, index,
                                     value, 3, 0);
      }
      return nyr_emit_runtime_call(b, "rt_tbuf_set_i64_raw", list, index, value,
                                   3, 0);
    }
    if (nyr_expr_is_bytes(b, e->as.memcall.target) &&
        strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
      int bytes = nyr_lower_expr(b, e->as.memcall.target);
      int index = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      int value = nyr_lower_expr(b, e->as.memcall.args.data[1].val);
      if (bytes < 0 || index < 0 || value < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_bytes_set_raw", bytes, index, value,
                                   3, 0);
    }
    const char *target_ident =
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT
            ? e->as.memcall.target->as.ident.name
            : NULL;
    /* A local receiver shadows a module alias with the same spelling.  In
     * particular, `io.set(...)` in net helpers is a dictionary mutation on
     * the `io` parameter, not a call to a hypothetical `core.io.set`.
     */
    bool is_module_namespace = target_ident && !local &&
                               nyr_resolve_use_alias(b, target_ident) != NULL;
    bool is_bytes = nyr_expr_is_bytes(b, e->as.memcall.target);
    bool is_set = nyr_expr_is_set(b, e->as.memcall.target);
    bool is_dict = !is_bytes && !is_set &&
                   (nyr_expr_is_dict(b, e->as.memcall.target) ||
                    (local && local->is_dict));
    if (!is_module_namespace && is_set &&
        (strcmp(method, "remove") == 0 || strcmp(method, "delete") == 0 ||
         strcmp(method, "sub") == 0) &&
        e->as.memcall.args.len == 1) {
      int set = nyr_lower_expr(b, e->as.memcall.target);
      int key = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      return set < 0 || key < 0 ? -1
                                : nyr_emit_runtime_call(b, "rt_set_remove", set,
                                                        key, -1, 2, 0);
    }
    if (!is_dict && !is_module_namespace && strcmp(method, "contains") == 0 &&
        e->as.memcall.args.len == 1 && e->as.memcall.args.data[0].val) {
      int container = nyr_lower_expr(b, e->as.memcall.target);
      int item = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      return container < 0 || item < 0
                 ? -1
                 : nyr_emit_runtime_call(b, "rt_contains_raw", container, item,
                                         -1, 2, 0);
    }
    const stmt_t *user_method_fn = nyr_find_user_function(b, method);
    if (!is_dict && user_method_fn) {
      /* Fall through to user method dispatch */
    } else if (!is_bytes && !is_module_namespace &&
               (strcmp(method, "get") == 0 || strcmp(method, "set") == 0 ||
                strcmp(method, "has") == 0 || strcmp(method, "contains") == 0 ||
                strcmp(method, "exists") == 0 || strcmp(method, "len") == 0 ||
                strcmp(method, "keys") == 0 || strcmp(method, "values") == 0 ||
                strcmp(method, "items") == 0 || strcmp(method, "delete") == 0 ||
                strcmp(method, "remove") == 0 ||
                strcmp(method, "merge") == 0)) {
      int dict = nyr_lower_expr(b, e->as.memcall.target);
      if (dict < 0)
        return -1;
      if (strcmp(method, "keys") == 0) {
        if (e->as.memcall.args.len != 0) {
          return nyr_fail(b, "native NYIR lower: dict.keys takes no arguments"),
                 -1;
        }
        return nyr_emit_runtime_call(b, "rt_dict_keys_raw", dict, -1, -1, 1, 0);
      }
      if (strcmp(method, "values") == 0 &&
          (is_dict || nyr_expr_is_dict(b, e->as.memcall.target))) {
        if (e->as.memcall.args.len != 0) {
          return nyr_fail(b,
                          "native NYIR lower: dict.values takes no arguments"),
                 -1;
        }
        return nyr_emit_runtime_call(b, "rt_dict_values_raw", dict, -1, -1, 1,
                                     0);
      }
      if (strcmp(method, "items") == 0 &&
          (is_dict || nyr_expr_is_dict(b, e->as.memcall.target))) {
        if (e->as.memcall.args.len != 0) {
          return nyr_fail(b,
                          "native NYIR lower: dict.items takes no arguments"),
                 -1;
        }
        return nyr_emit_runtime_call(b, "rt_dict_items_raw", dict, -1, -1, 1,
                                     0);
      }
      if (strcmp(method, "len") == 0) {
        if (e->as.memcall.args.len != 0) {
          return nyr_fail(b, "native NYIR lower: dict.len takes no arguments"),
                 -1;
        }
        const expr_t *dict_lit =
            nyr_resolve_dict_literal(b, e->as.memcall.target, 0);
        if (dict_lit && nyr_dict_literal_all_keys_constant(dict_lit))
          return nyr_emit_const(b, (int64_t)dict_lit->as.dict.pairs.len);
        if (nyr_expr_is_list(b, e->as.memcall.target) ||
            nyr_expr_is_dyn_list(b, e->as.memcall.target))
          return nyr_emit_runtime_call(b, "rt_tbuf_len_raw", dict, -1, -1, 1,
                                       0);
        if (nyr_expr_is_dict(b, e->as.memcall.target))
          return nyr_emit_runtime_call(b, "rt_dict_len_raw", dict, -1, -1, 1,
                                       0);
        return nyr_emit_runtime_call(b, "rt_len", dict, -1, -1, 1, 0);
      }
      if ((strcmp(method, "has") == 0 || strcmp(method, "contains") == 0 ||
           strcmp(method, "exists") == 0) &&
          e->as.memcall.args.len == 1) {
        const expr_t *dict_lit =
            nyr_resolve_dict_literal(b, e->as.memcall.target, 0);
        if (dict_lit) {
          const expr_t *k_arg = e->as.memcall.args.data[0].val;
          bool is_const = false;
          const char *kstr = NULL;
          int64_t kint = 0;
          bool is_int = false;
          if (k_arg && k_arg->kind == NY_E_LITERAL) {
            if (k_arg->as.literal.kind == NY_LIT_STR &&
                k_arg->as.literal.as.s.data) {
              kstr = k_arg->as.literal.as.s.data;
              is_const = true;
            } else if (k_arg->as.literal.kind == NY_LIT_INT &&
                       k_arg->tok.kind != NY_T_NIL) {
              kint = k_arg->as.literal.as.i;
              is_int = true;
              is_const = true;
            }
          }
          if (is_const) {
            bool found = false;
            (void)nyr_dict_literal_lookup(dict_lit, kstr, kint, is_int, &found);
            if (found)
              return nyr_emit_const(b, 1);
            if (nyr_dict_literal_all_keys_constant(dict_lit))
              return nyr_emit_const(b, 0);
          }
        }
        int key = nyr_lower_dict_key(b, e->as.memcall.args.data[0].val);
        if (key < 0)
          return -1;
        return nyr_emit_runtime_call(
            b,
            nyr_expr_is_cstr(b, e->as.memcall.args.data[0].val)
                ? "rt_dict_has_str_raw"
                : "rt_dict_has_raw",
            dict, key, -1, 2, NYIR_SCALAR_I64_RAW);
      }
      if ((strcmp(method, "delete") == 0 || strcmp(method, "remove") == 0) &&
          e->as.memcall.args.len == 1) {
        int key = nyr_lower_dict_key(b, e->as.memcall.args.data[0].val);
        if (key < 0)
          return -1;
        return nyr_emit_runtime_call(
            b,
            nyr_expr_is_cstr(b, e->as.memcall.args.data[0].val)
                ? "rt_dict_delete_str_raw"
                : "rt_dict_delete_raw",
            dict, key, -1, 2, 0);
      }
      if (strcmp(method, "merge") == 0 && e->as.memcall.args.len == 1) {
        int other = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
        if (other < 0)
          return -1;
        return nyr_emit_runtime_call(b, "rt_dict_merge_raw", dict, other, -1, 2,
                                     nyr_value_scalar_class(b, dict));
      }
      if (strcmp(method, "get") == 0 &&
          (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
        int key = nyr_lower_dict_key(b, e->as.memcall.args.data[0].val);
        const expr_t *fallback_expr =
            e->as.memcall.args.len == 2 ? e->as.memcall.args.data[1].val : NULL;
        int fallback = fallback_expr ? nyr_lower_expr(b, fallback_expr)
                                     : nyr_emit_const(b, 0);
        if (key < 0 || fallback < 0)
          return -1;
        /* The dictionary API returns a dynamic value even when its inferred
         * result is narrowed from the fallback. Preserve the scalar fallback
         * representation at the runtime boundary so a miss cannot turn raw
         * integer 7 into dynamic integer 3. */
        if (fallback_expr && (!nyr_expr_is_any(b, fallback_expr) ||
                              nyr_expr_has_raw_int_abi(b, fallback_expr, 0))) {
          if (nyr_expr_is_bool(b, fallback_expr)) {
            fallback = nyr_box_bool(b, fallback);
          } else if (nyr_expr_is_f64(b, fallback_expr) ||
                     nyr_expr_is_f32(b, fallback_expr)) {
            int f64 = nyr_expr_is_f32(b, fallback_expr)
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
        return nyr_emit_runtime_call(
            b, nyr_dict_get_symbol(b, e->as.memcall.args.data[0].val), dict,
            key, fallback, 3, 0);
      }
      if (strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
        int key = nyr_lower_dict_key(b, e->as.memcall.args.data[0].val);
        int value = nyr_lower_expr(b, e->as.memcall.args.data[1].val);
        if (key < 0 || value < 0)
          return -1;
        /* Native boolean expressions use raw 0/1 for control flow, but
         * dictionary slots are dynamic values.  Preserve the canonical Ny
         * bool immediates at this container boundary. */
        const expr_t *value_expr = e->as.memcall.args.data[1].val;
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
        } else if (nyr_expr_is_f64(b, value_expr) ||
                   nyr_expr_is_f32(b, value_expr)) {
          /* Dictionary values cross the public dynamic ABI.  Preserve a
           * float as a boxed float handle; storing its raw IEEE bits makes a
           * later `.get` interpret the bit pattern as an integer/pointer. */
          if (nyr_expr_is_f32(b, value_expr))
            value = nyr_emit_f32_to_f64(b, value);
          int bits = value < 0 ? -1
                               : nyr_emit_runtime_call(b, "rt_f64_bits", value,
                                                       -1, -1, 1, 0);
          value = bits < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_flt_box_val", bits,
                                                   -1, -1, 1, 0);
          if (value < 0)
            return -1;
        } else {
          bool raw_dict_integer =
              value_expr && !is_bool_val &&
              ((value_expr->kind == NY_E_LITERAL &&
                value_expr->as.literal.kind == NY_LIT_INT &&
                value_expr->tok.kind != NY_T_NIL) ||
               (value_expr->semantic.resolved &&
                value_expr->semantic.rep == NY_SEM_REP_RAW_INT));
          if (value_expr && !is_bool_val && value_expr->kind == NY_E_BINARY &&
              !nyr_expr_is_f64(b, value_expr) &&
              !nyr_expr_is_f32(b, value_expr) &&
              !nyr_expr_is_bool(b, value_expr) &&
              !nyr_expr_is_any(b, value_expr))
            raw_dict_integer = true;
          if (value_local && !is_bool_val &&
              value_local->semantic_rep == NY_SEM_REP_RAW_INT)
            raw_dict_integer = true;
          if (raw_dict_integer) {
            value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
            if (value < 0)
              return -1;
          }
        }
        return nyr_emit_runtime_call(
            b, nyr_dict_set_symbol(b, e->as.memcall.args.data[0].val), dict,
            key, value, 3, 0);
      }
      return nyr_fail(b, "native NYIR lower: unsupported dict.%s arity",
                      method),
             -1;
    }
    char qualified[512];
    if (!nyr_qualified_expr(e->as.memcall.target, qualified,
                            sizeof(qualified))) {
      const stmt_t *method_fn = nyr_find_user_function(b, e->as.memcall.name);
      if (method_fn) {
        call_arg_t args[NYIR_CALL_MAX_ARGS] = {{.val = e->as.memcall.target}};
        for (size_t i = 0;
             i < e->as.memcall.args.len && i + 1 < NYIR_CALL_MAX_ARGS; ++i)
          args[i + 1] = e->as.memcall.args.data[i];
        expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
        callee.as.ident.name =
            method_fn->as.fn.name ? method_fn->as.fn.name : e->as.memcall.name;
        expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
        /* The receiver expression may have been conservatively classified as
         * a dynamic attached call.  Once the resolver selected a concrete
         * user method, let ordinary call lowering derive the method's actual
         * return representation (otherwise a raw int result is passed to
         * `any` consumers without boxing). */
        call.semantic = (ny_expr_semantic_t){0};
        call.semantic.member_call_kind = NY_SEM_CALL_NONE;
        call.semantic.canonical_callee = NULL;
        call.semantic.canonical_callee_stmt = NULL;
        call.as.call.callee = &callee;
        call.as.call.args.data = args;
        call.as.call.args.len = e->as.memcall.args.len + 1;
        call.as.call.args.cap = call.as.call.args.len;
        return nyr_lower_expr(b, &call);
      }
      const char *receiver_type = nyr_expr_type_name(b, e->as.memcall.target);
      return nyr_fail(b,
                      "unresolved member call '%s.%s': no callable "
                      "method was resolved "
                      "for the receiver type",
                      receiver_type ? receiver_type : "<unknown>",
                      e->as.memcall.name ? e->as.memcall.name : "<member>"),
             -1;
    }
    int n = snprintf(qualified + strlen(qualified),
                     sizeof(qualified) - strlen(qualified), ".%s",
                     e->as.memcall.name);
    const char *dot = strchr(qualified, '.');
    if (dot && dot != qualified) {
      char alias[256];
      size_t alias_len = (size_t)(dot - qualified);
      if (alias_len < sizeof(alias)) {
        memcpy(alias, qualified, alias_len);
        alias[alias_len] = '\0';
        const char *module = nyr_resolve_use_alias(b, alias);
        if (module) {
          char canonical[512];
          int cn = snprintf(canonical, sizeof(canonical), "%s%s", module, dot);
          if (cn < 0 || (size_t)cn >= sizeof(canonical)) {
            return nyr_fail(
                       b, "native NYIR lower: canonical call name is too long"),
                   -1;
          }
          memcpy(qualified, canonical, (size_t)cn + 1);
        }
      }
    }
    if (n < 0 || (size_t)n >= sizeof(qualified) - strlen(qualified)) {
      return nyr_fail(b, "native NYIR lower: qualified call name is too long"),
             -1;
    }
    /*
     * Only a real C-header namespace named `c` may take the raw hardware
     * sqrt; a Nytrix local or module named `c` keeps its own `sqrt` method.
     */
    if (e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        strcmp(e->as.memcall.target->as.ident.name, "c") == 0 &&
        !nyr_find_local(b, "c") && !nyr_find_user_function(b, "c") &&
        strcmp(e->as.memcall.name, "sqrt") == 0 &&
        e->as.memcall.args.len == 1) {
      int arg = nyr_lower_expr(b, e->as.memcall.args.data[0].val);
      if (arg < 0)
        return -1;
      if (nyr_expr_is_f64(b, e->as.memcall.args.data[0].val))
        return nyr_push_val(b, NYIR_SQRT_F64, arg, -1, 0, NULL);
    }
    /* C-header namespaces are extern-table entries, not Nytrix receivers. */
    if (b->externs && ny_extern_table_lookup(b->externs, qualified)) {
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = qualified;
      expr_t call = *e;
      call.kind = NY_E_CALL;
      call.as.call.callee = &callee;
      call.as.call.args = e->as.memcall.args;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.semantic.canonical_callee = NULL;
      call.semantic.canonical_callee_stmt = NULL;
      return nyr_lower_expr(b, &call);
    }
    const stmt_t *qualified_fn = nyr_find_user_function(b, qualified);
    if (qualified_fn || is_module_namespace) {
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = qualified_fn && qualified_fn->as.fn.name
                                 ? qualified_fn->as.fn.name
                                 : qualified;
      expr_t call = *e;
      call.kind = NY_E_CALL;
      /* `qualified` is a lowering scratch buffer.  The synthetic call is
       * consumed immediately, but its semantic metadata must not retain that
       * stack address for later representation queries. */
      call.semantic.canonical_callee =
          qualified_fn ? qualified_fn->as.fn.name : NULL;
      call.semantic.canonical_callee_stmt = (stmt_t *)qualified_fn;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.as.call.callee = &callee;
      call.as.call.args = e->as.memcall.args;
      return nyr_lower_expr(b, &call);
    }
    const stmt_t *method_fn = nyr_find_user_function(b, e->as.memcall.name);
    const stmt_t *target_callee = e->semantic.canonical_callee_stmt;
    if (!target_callee && e->semantic.canonical_callee)
      target_callee = nyr_find_user_function(b, e->semantic.canonical_callee);
    if (!target_callee)
      target_callee = method_fn;
    if (target_callee) {
      call_arg_t args[NYIR_CALL_MAX_ARGS] = {{.val = e->as.memcall.target}};
      for (size_t i = 0;
           i < e->as.memcall.args.len && i + 1 < NYIR_CALL_MAX_ARGS; ++i)
        args[i + 1] = e->as.memcall.args.data[i];
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = target_callee->as.fn.name
                                 ? target_callee->as.fn.name
                                 : e->as.memcall.name;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      /* Reclassify after converting the member call to a direct call.  The
       * original `any.method(...)` annotation describes dispatch, not the
       * selected method's return ABI. */
      call.semantic = (ny_expr_semantic_t){0};
      call.semantic.canonical_callee = NULL;
      call.semantic.canonical_callee_stmt = (stmt_t *)target_callee;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.as.call.callee = &callee;
      call.as.call.args.data = args;
      call.as.call.args.len = e->as.memcall.args.len + 1;
      call.as.call.args.cap = call.as.call.args.len;
      return nyr_lower_expr(b, &call);
    }
    expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
    callee.as.ident.name = qualified;
    expr_t call = *e;
    call.kind = NY_E_CALL;
    call.as.call.callee = &callee;
    call.as.call.args = e->as.memcall.args;
    return nyr_lower_expr(b, &call);
  }
  case NY_E_MEMBER: {
    /* Specialize typed-list length before property/attached-method lowering
     * can expand the stdlib facade into its dynamic __list_len bridge. */
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0 &&
        e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name &&
        (!e->semantic.canonical_callee_stmt ||
         ny_is_stdlib_tok(e->semantic.canonical_callee_stmt->tok))) {
      const nyr_local_t *local =
          nyr_find_local(b, e->as.member.target->as.ident.name);
      if (local && local->is_list && !local->is_any) {
        if (local->arg_slot >= 0 && local->list_len_arg_slot >= 0 &&
            local->list_len_slot >= 0 && !local->semantic_mutable)
          return nyr_load_local_value(b, local->list_len_slot);
        int list = nyr_lower_expr(b, e->as.member.target);
        return list < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1,
                                                -1, 1, 0);
      }
    }
    if (!e->as.member.name || !e->as.member.target) {
      return nyr_fail(b, "native NYIR lower: malformed member access"), -1;
    }
    const stmt_t *qualified_function = nyr_namespace_function_value(b, e);
    if (qualified_function && qualified_function->as.fn.name) {
      expr_t function_value = {.kind = NY_E_IDENT, .tok = e->tok};
      function_value.as.ident.name = qualified_function->as.fn.name;
      return nyr_lower_expr(b, &function_value);
    }
    const stmt_t *source_property =
        nyr_find_attached_method(b, e->as.member.target, e->as.member.name);
    if (source_property && !ny_is_stdlib_tok(source_property->tok) &&
        source_property->as.fn.params.len == 1) {
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = source_property->as.fn.name;
      call_arg_t arg = {.val = e->as.member.target};
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic.canonical_callee_stmt = (stmt_t *)source_property;
      call.as.call.callee = &callee;
      call.as.call.args.data = &arg;
      call.as.call.args.len = call.as.call.args.cap = 1;
      return nyr_lower_expr(b, &call);
    }
    /* Semantic attached-call facts normally redirect properties through
     * their canonical function.  `.long` has a representation-independent
     * raw runtime ABI, so lower it before that redirection; otherwise the
     * generated wrapper can preserve the receiver instead of performing the
     * conversion when the receiver is a compact native list.  rt_long is a
     * dynamic-normalization boundary (it decodes immediate integers), so a
     * proven raw-int receiver must take the raw scalar bridge instead or
     * the value halves (123.long read 61). */
    if (strcmp(e->as.member.name, "long") == 0) {
      int target = nyr_lower_expr(b, e->as.member.target);
      if (target < 0)
        return -1;
      const expr_t *recv = e->as.member.target;
      if (recv && recv->kind == NY_E_IDENT && recv->as.ident.name) {
        const nyr_local_t *rl = nyr_find_local(b, recv->as.ident.name);
        if (rl && rl->is_list && rl->list_len_slot >= 0) {
          int count = nyr_load_local_value(b, rl->list_len_slot);
          if (count < 0)
            return -1;
          return nyr_emit_runtime_call(b, "rt_long_list_raw", target, count, -1,
                                       2, 0);
        }
      }
      if (nyr_expr_is_list(b, recv) && !nyr_expr_is_dyn_list(b, recv)) {
        int count =
            nyr_emit_runtime_call(b, "rt_tbuf_len_raw", target, -1, -1, 1, 0);
        return count < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_long_list_raw", target,
                                                 count, -1, 2, 0);
      }
      bool recv_raw = recv && recv->semantic.resolved &&
                      recv->semantic.rep == NY_SEM_REP_RAW_INT;
      if (recv && recv->kind == NY_E_LITERAL &&
          recv->as.literal.kind == NY_LIT_INT && recv->tok.kind != NY_T_NIL)
        recv_raw = true;
      if (recv && recv->kind == NY_E_IDENT && recv->as.ident.name) {
        const nyr_local_t *rl = nyr_find_local(b, recv->as.ident.name);
        if (nyr_long_receiver_is_raw_word(rl))
          recv_raw = true;
      }
      if (recv_raw && !nyr_expr_is_any(b, recv) && !nyr_expr_is_cstr(b, recv) &&
          !nyr_expr_is_bigint(b, recv) && !nyr_expr_is_list(b, recv) &&
          !nyr_expr_is_dict(b, recv))
        return nyr_emit_runtime_call(b, "rt_bigint_from_i64_raw", target, -1,
                                     -1, 1, 0);
      return nyr_emit_runtime_call(b, "rt_long", target, -1, -1, 1, 0);
    }
    if (strcmp(e->as.member.name, "len") == 0) {
      if (e->as.member.target->kind == NY_E_LIST ||
          e->as.member.target->kind == NY_E_TUPLE)
        return nyr_emit_const(b,
                              (int64_t)e->as.member.target->as.list_like.len);
      int target = nyr_lower_expr(b, e->as.member.target);
      if (target < 0)
        return -1;
      /* Typed/native strings use the raw C-string ABI.  The generic length
       * helper intentionally also understands buffers and tagged containers,
       * but its object probe can reject a headerless string constant that has
       * crossed a typed boundary. */
      if (nyr_expr_is_cstr(b, e->as.member.target))
        return nyr_emit_runtime_call(b, "rt_cstr_len", target, -1, -1, 1, 0);
      if (nyr_expr_is_any(b, e->as.member.target) ||
          (e->as.member.target->kind == NY_E_IDENT &&
           !nyr_expr_is_list(b, e->as.member.target) &&
           !nyr_expr_is_bytes(b, e->as.member.target) &&
           !nyr_expr_is_range(b, e->as.member.target)))
        return nyr_emit_runtime_call(b, "rt_len_strict", target, -1, -1, 1, 0);
      return nyr_emit_runtime_call(b, "rt_len", target, -1, -1, 1, 0);
    }
    /* A typed vector is a dynamic dictionary with a stable field contract.
     * Read its coordinates directly from that representation instead of
     * routing through an unqualified helper call; the latter loses the
     * receiver's value/length/tag facts across imported module boundaries.
     * This is driven by the resolved type, not by a stdlib symbol spelling. */
    const char *target_type = nyr_expr_type_name(b, e->as.member.target);
    if ((!target_type || strcmp(target_type, "any") == 0) &&
        e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name) {
      const nyr_local_t *target_local =
          nyr_find_local(b, e->as.member.target->as.ident.name);
      if (target_local)
        target_type = target_local->type_name;
      if (!target_type) {
        const expr_t *global =
            nyr_find_top_level_value(b, e->as.member.target->as.ident.name);
        if (global && global != e->as.member.target)
          target_type = nyr_expr_type_name(b, global);
      }
    }
    if ((!target_type || strcmp(target_type, "any") == 0) &&
        e->as.member.target->kind == NY_E_CALL &&
        e->as.member.target->as.call.callee &&
        e->as.member.target->as.call.callee->kind == NY_E_IDENT &&
        e->as.member.target->as.call.callee->as.ident.name && b->prog) {
      const char *ctor_name =
          e->as.member.target->as.call.callee->as.ident.name;
      const stmt_t *ctor_layout =
          nyr_find_layout_in_prog(b, b->prog, ctor_name);
      if (ctor_layout && (ctor_layout->kind == NY_S_STRUCT ||
                          ctor_layout->kind == NY_S_LAYOUT))
        target_type = ctor_layout->kind == NY_S_STRUCT
                          ? ctor_layout->as.struc.name
                          : ctor_layout->as.layout.name;
    }
    /* BigInt's byte conversion is a nominal attached operation.  Keep the
     * runtime bridge available when imported impl metadata is absent from the
     * current module walk; otherwise the unrelated global bytes(int)
     * constructor is selected by its leaf name and receives a bigint handle
     * as a byte count. */
    if (target_type && strcmp(target_type, "bigint") == 0 &&
        (strcmp(e->as.member.name, "bytes") == 0 ||
         strcmp(e->as.member.name, "as_bytes") == 0)) {
      int bigint = nyr_lower_expr(b, e->as.member.target);
      return bigint < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_bigint_to_bytes", bigint,
                                                -1, -1, 1, 0);
    }
    bool typed_vector = ny_type_vector_dimension(target_type) > 0;
    if (typed_vector && (strcmp(e->as.member.name, "x") == 0 ||
                         strcmp(e->as.member.name, "y") == 0 ||
                         strcmp(e->as.member.name, "z") == 0 ||
                         strcmp(e->as.member.name, "w") == 0)) {
      int target = nyr_lower_expr(b, e->as.member.target);
      int key = nyr_emit_const_cstr(b, e->as.member.name);
      int fallback = nyr_emit_const(b, 0);
      if (target < 0 || key < 0 || fallback < 0)
        return -1;
      int boxed = nyr_emit_runtime_call(b, "rt_dict_get_str_raw", target, key,
                                        fallback, 3, 0);
      return boxed < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_any_to_f64", boxed, -1,
                                               -1, 1, NYIR_SCALAR_F64);
    }
    /* A nominal attached property is a method call, even when the backing
     * record happens to contain a field with the same spelling.  Resolve the
     * selected declaration before raw layout access; otherwise a tagged
     * dynamic field word is returned through the raw scalar ABI and values
     * such as `box.value` are decoded as half their actual integer. */
    if (e->as.member.name &&
        (e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT ||
         e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED)) {
      const stmt_t *attached_method =
          nyr_find_attached_method(b, e->as.member.target, e->as.member.name);
      if (attached_method && attached_method->as.fn.name) {
        call_arg_t arg = {.val = e->as.member.target};
        expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
        callee.as.ident.name = attached_method->as.fn.name;
        expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
        call.semantic = (ny_expr_semantic_t){0};
        call.semantic.canonical_callee_stmt = (stmt_t *)attached_method;
        call.as.call.callee = &callee;
        call.as.call.args.data = &arg;
        call.as.call.args.len = call.as.call.args.cap = 1;
        return nyr_lower_expr(b, &call);
      }
    }
    /* User-defined layouts are raw, owned records rather than dictionaries.
     * Their constructor returns the data pointer, so resolve a member from
     * the declaration and load it at the declared byte offset.  Falling
     * through to rt_dict_get_str_raw made a valid `Vec2(...).x` look like a
     * dynamic record and discarded the field value. */
    if (target_type && e->as.member.name) {
      const stmt_t *layout = nyr_find_layout_in_prog(b, b->prog, target_type);
      if (layout &&
          (layout->kind == NY_S_STRUCT || layout->kind == NY_S_LAYOUT)) {
        const ny_layout_field_list *fields = layout->kind == NY_S_STRUCT
                                                 ? &layout->as.struc.fields
                                                 : &layout->as.layout.fields;
        const layout_field_t *field = NULL;
        for (size_t fi = 0; fi < fields->len; ++fi) {
          if (fields->data[fi].name &&
              strcmp(fields->data[fi].name, e->as.member.name) == 0) {
            field = &fields->data[fi];
            break;
          }
        }
        if (field && !field->is_array) {
          size_t field_size = 0, field_align = 0, field_offset = 0;
          if (!nyr_ast_layout_query(b, target_type, field->name, &field_size,
                                    &field_align, &field_offset)) {
            return nyr_fail(b, "native member '%s.%s' has no layout",
                            target_type, field->name),
                   -1;
          }
          int target = nyr_lower_expr(b, e->as.member.target);
          int offset = nyr_emit_const(b, (int64_t)field_offset);
          int address = target < 0 || offset < 0
                            ? -1
                            : nyr_emit_add_i64(b, target, offset);
          if (address < 0)
            return -1;
          size_t primitive_size = 0, primitive_align = 0;
          if (!nyr_primitive_layout(field->type_name, &primitive_size,
                                    &primitive_align))
            return address;
          if (ny_native_type_name_is_f64(field->type_name))
            return nyr_emit_load_f64(b, address);
          if (ny_native_type_name_is_f32(field->type_name))
            return nyr_emit_runtime_call(b, "rt_load32_f64", target, offset, -1,
                                         2, NYIR_SCALAR_F64);
          if (field_size <= 4) {
            int loaded =
                nyr_emit_runtime_call(b,
                                      field_size == 1   ? "rt_load8_idx"
                                      : field_size == 2 ? "rt_load16_idx"
                                                        : "rt_load32_idx",
                                      target, offset, -1, 2, 0);
            return loaded < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_any_to_i64",
                                                      loaded, -1, -1, 1, 0);
          }
          /* The declared field owns its storage ABI. A stale dynamic source
           * stamp must not turn a raw load into a tagged producer. */
          return nyr_ir_emit(
              &b->nyir,
              (nyr_inst_t){.op = NYIR_LOAD_I64,
                            .dst = -1,
                            .a = address,
                            .b = -1,
                            .c = -1,
                            .scalar_class = ny_native_scalar_class_from_sem_rep(
                                nyr_rep_for_type(b, field->type_name))});
        }
      }
    }
    /* An unresolved/dynamic receiver may still be a vector dictionary.  The
     * attached `x/y/z/w` helpers index through the dynamic container ABI and
     * can lose the third field; direct field lookup preserves the boxed value
     * and also remains correct for ordinary record dictionaries. */
    const expr_t *dict_lit =
        nyr_resolve_dict_literal(b, e->as.member.target, 0);
    if (dict_lit && e->as.member.name) {
      bool found = false;
      const expr_t *val = nyr_dict_literal_lookup(dict_lit, e->as.member.name,
                                                  0, false, &found);
      if (found && val) {
        int result = nyr_lower_expr(b, val);
        if (result >= 0 && e->semantic.resolved &&
            e->semantic.rep == NY_SEM_REP_RAW_INT &&
            !nyr_expr_is_raw_int(val)) {
          result =
              nyr_emit_runtime_call(b, "rt_any_to_i64", result, -1, -1, 1, 0);
        }
        return result;
      }
      if (!found && nyr_dict_literal_all_keys_constant(dict_lit)) {
        int fb = nyr_emit_const(b, 0);
        if (fb >= 0 && e->semantic.resolved &&
            e->semantic.rep != NY_SEM_REP_RAW_INT) {
          fb = nyr_emit_runtime_call(b, "rt_tag", fb, -1, -1, 1, 0);
        }
        return fb;
      }
    }
    bool dynamic_record = !target_type || strcmp(target_type, "any") == 0 ||
                          nyr_expr_is_any(b, e->as.member.target);
    if (dynamic_record && (strcmp(e->as.member.name, "x") == 0 ||
                           strcmp(e->as.member.name, "y") == 0 ||
                           strcmp(e->as.member.name, "z") == 0 ||
                           strcmp(e->as.member.name, "w") == 0)) {
      int target = nyr_lower_expr(b, e->as.member.target);
      int key = nyr_emit_const_cstr(b, e->as.member.name);
      int fallback = nyr_emit_const(b, 0);
      if (target < 0 || key < 0 || fallback < 0)
        return -1;
      return nyr_emit_runtime_call(b, "rt_dict_get_str_raw", target, key,
                                   fallback, 3, 0);
    }
    if (e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT ||
        e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED ||
        nyr_find_attached_method(b, e->as.member.target, e->as.member.name) !=
            NULL) {
      const char *canonical = e->semantic.canonical_callee;
      if (!canonical && e->semantic.canonical_callee_stmt)
        canonical = e->semantic.canonical_callee_stmt->as.fn.name;
      const stmt_t *attached_method =
          nyr_find_attached_method(b, e->as.member.target, e->as.member.name);
      if (attached_method && attached_method->as.fn.name)
        canonical = attached_method->as.fn.name;
      if (!canonical) {
        return nyr_fail(b,
                        "native NYIR lower: semantic property target "
                        "is missing for '%s'",
                        e->as.member.name),
               -1;
      }
      call_arg_t arg = {.val = e->as.member.target};
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = canonical;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic = e->semantic;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.semantic.canonical_callee = NULL;
      call.semantic.canonical_callee_stmt = e->semantic.canonical_callee_stmt;
      call.as.call.callee = &callee;
      call.as.call.args.data = &arg;
      call.as.call.args.len = call.as.call.args.cap = 1;
      return nyr_lower_expr(b, &call);
    }
    char mem_qname[128] = {0};
    if (e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name && e->as.member.name) {
      snprintf(mem_qname, sizeof(mem_qname), "%s.%s",
               e->as.member.target->as.ident.name, e->as.member.name);
      const stmt_t *adt_enum = NULL;
      const stmt_enum_item_t *adt_item = NULL;
      int64_t adt_tag = 0;
      if (nyr_find_enum_member(b, mem_qname, &adt_enum, &adt_item, &adt_tag)) {
        if (adt_item && adt_item->fields.len == 0) {
          return nyr_emit_enum_tag(b, adt_enum, adt_tag);
        }
      }
    }
    /* Resolve the fully qualified module member before consulting the
     * process-wide leaf-name constant table.  Re-export facades commonly
     * repeat names (for example vk.compute.FLAG = compute.FLAG); a tail-only
     * lookup can select an unrelated or not-yet-folded definition. */
    const expr_t *resolved_member = nyr_resolve_member_expr(b, e);
    /* Module-member lookup returns a declaration's initializer.  For mutable
     * module globals, loading that expression would replay the initializer
     * instead of reading the cell after a store. */
    if (resolved_member && resolved_member != e && b->prog) {
      ny_native_tlv_ensure_build(b, b->prog);
      const nyr_builder_impl_t *owner = b->cache_owner ? b->cache_owner : b;
      const char *qualified[1] = {NULL};
      char member_name[1024];
      char canonical_name[1024];
      canonical_name[0] = '\0';
      const char *module_owner =
          nyr_module_for_initializer_expr(b->prog, resolved_member);
      if (module_owner) {
        int n = snprintf(canonical_name, sizeof(canonical_name), "%s.%s",
                         module_owner, e->as.member.name);
        if (n < 0 || (size_t)n >= sizeof(canonical_name))
          canonical_name[0] = '\0';
      }
      if (nyr_qualified_expr(e, member_name, sizeof(member_name))) {
        const char *separator = strchr(member_name, '.');
        const char *root = member_name;
        const char *resolved_root = NULL;
        if (separator) {
          char root_name[512];
          size_t root_len = (size_t)(separator - member_name);
          if (root_len < sizeof(root_name)) {
            memcpy(root_name, member_name, root_len);
            root_name[root_len] = '\0';
            resolved_root = nyr_resolve_use_alias(b, root_name);
            if (!resolved_root) {
              nyr_builder_impl_t probe = *b;
              probe.cache_owner =
                  b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
              probe.source_file = NULL;
              resolved_root = nyr_resolve_use_alias(&probe, root_name);
            }
          }
        }
        int n = canonical_name[0]
                    ? (int)strlen(canonical_name)
                    : snprintf(canonical_name, sizeof(canonical_name), "%s%s",
                               resolved_root ? resolved_root : root,
                               separator ? separator : "");
        if (n > 0 && (size_t)n < sizeof(canonical_name)) {
          if (owner->tlv_ents) {
            for (size_t i = 0; i < owner->tlv_len; ++i) {
              const ny_tlv_ent_t *entry = &owner->tlv_ents[i];
              if (entry->qualified && entry->is_mut && entry->name &&
                  strcmp(entry->name, canonical_name) == 0) {
                qualified[0] = entry->name;
                break;
              }
            }
          }
        }
      }
      const char *symbol =
          qualified[0] ? nyr_globaltab_name(qualified[0])
                       : nyr_globaltab_name(canonical_name[0] ? canonical_name
                                                              : member_name);
      if (symbol) {
        int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = symbol});
        if (addr < 0)
          return -1;
        int value = nyr_load_global_scalar(b, addr, symbol, e);
        nyr_record_global_dyn_tag(b, symbol, value);
        return value;
      }
    }
    if (resolved_member && resolved_member != e)
      return nyr_lower_expr(b, resolved_member);
    /* Complete stdlib collection can omit the facade module from the direct
     * module table while retaining its root `use` declaration.  Resolve the
     * exported leaf through the import graph before falling back to the
     * process-wide tail constant table; otherwise a valid integer flag is
     * lowered as a missing dictionary member (zero). */
    const expr_t *imported_member =
        nyr_find_imported_value(b, e->as.member.name);
    if (imported_member && imported_member != e)
      return nyr_lower_expr(b, imported_member);
    int64_t constant = 0;
    bool constant_found = nyr_consttab_get(e->as.member.name, &constant);
    if (!constant_found)
      constant_found = nyr_consttab_get_tail(e->as.member.name, &constant);
    if (ny_trace_enabled("NY_TRACE_CONSTTAB"))
      fprintf(stderr, "native const lookup %s found=%d value=%lld\n",
              e->as.member.name, constant_found ? 1 : 0, (long long)constant);
    if (constant_found)
      return nyr_emit_const(b, constant);
    if (strcmp(e->as.member.name, "len") == 0) {
      /*
       * A mutable identifier may still resolve to its declaration initializer
       * in the top-level value table.  Folding `x.len` through that table
       * would report the initializer length after `x = append(x, value)`.
       * Only fold direct list literals here; identifiers use their current
       * native value.
       */
      const expr_t *list =
          e->as.member.target->kind == NY_E_LIST ? e->as.member.target : NULL;
      if (list)
        return nyr_emit_const(b, (int64_t)list->as.list_like.len);
      int target = nyr_lower_expr(b, e->as.member.target);
      /* Only use the compile-time length fact for non-identifier expressions.
       * Any identifier binding may alias a mutated list; always read from
       * memory to avoid stale cached lengths after clear()/append(). */
      bool target_is_var = (e->as.member.target->kind == NY_E_IDENT);
      int length = target_is_var ? -1 : nyr_peek_list_len_fact(b, target);
      if (target >= 0 && length >= 0)
        return length;
      if (target >= 0 &&
          (nyr_expr_is_bytes(b, e->as.member.target) ||
           (e->as.member.target->kind == NY_E_IDENT &&
            nyr_find_local(b, e->as.member.target->as.ident.name) &&
            nyr_find_local(b, e->as.member.target->as.ident.name)->is_bytes)))
        return nyr_emit_runtime_call(b, "rt_len", target, -1, -1, 1, 0);
      if (target >= 0 && nyr_expr_is_cstr(b, e->as.member.target)) {
        int dynamic_length = nyr_peek_dyn_fact(b, target, NYR_FACT_DYN_STR_LEN);
        if (dynamic_length >= 0)
          return dynamic_length;
        return nyr_emit_runtime_call(b, "rt_cstr_len", target, -1, -1, 1, 0);
      }
      /*
       * An `any` parameter may be called with a list at runtime, but its
       * member access keeps the checked dynamic-length contract.  Do not
       * specialize it to the raw typed-buffer ABI from a caller-side hint:
       * inlining can otherwise leave a tagged argument at rt_tbuf_len_raw.
       */
      nyr_local_t *member_local =
          e->as.member.target->kind == NY_E_IDENT
              ? nyr_find_local(b, e->as.member.target->as.ident.name)
              : NULL;
      if (target >= 0 && member_local && member_local->is_any)
        return nyr_emit_runtime_call(b, "rt_len_strict", target, -1, -1, 1, 0);
      /* Parameter identifiers are not always recognized by the expression
       * type query here (notably after ABI expansion).  Their local binding
       * still carries the authoritative static-list classification.  Route
       * those values through the checked typed-buffer length helper rather
       * than the generic tagged-value helper. */
      nyr_local_t *list_local =
          e->as.member.target->kind == NY_E_IDENT
              ? nyr_find_local(b, e->as.member.target->as.ident.name)
              : NULL;
      bool typed_list =
          nyr_expr_is_list(b, e->as.member.target) ||
          (list_local && list_local->is_list && !list_local->is_any);
      if (target >= 0 && typed_list)
        return nyr_emit_runtime_call(b, "rt_tbuf_len_raw", target, -1, -1, 1,
                                     0);
      if (target >= 0)
        return nyr_emit_runtime_call(b, "rt_len", target, -1, -1, 1, 0);
    }
    const stmt_t *method_fn = nyr_find_user_function(b, e->as.member.name);
    if (method_fn) {
      call_arg_t arg = {.val = e->as.member.target};
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name =
          method_fn->as.fn.name ? method_fn->as.fn.name : e->as.member.name;
      expr_t call = {.kind = NY_E_CALL, .tok = e->tok};
      call.semantic = e->semantic;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.semantic.canonical_callee = NULL;
      call.semantic.canonical_callee_stmt = NULL;
      call.as.call.callee = &callee;
      call.as.call.args.data = &arg;
      call.as.call.args.len = call.as.call.args.cap = 1;
      return nyr_lower_expr(b, &call);
    }
    int target_val = nyr_lower_expr(b, e->as.member.target);
    if (target_val >= 0) {
      int key_const = nyr_emit_const_cstr(b, e->as.member.name);
      int fallback = nyr_emit_const(b, 0);
      if (key_const >= 0 && fallback >= 0) {
        int value = nyr_emit_runtime_call(b, "rt_dict_get_str_raw", target_val,
                                          key_const, fallback, 3, 0);
        /* Dictionary fields are stored in the canonical tagged-value ABI.
         * A property expression is a value boundary, so decode a tagged
         * scalar exactly once while leaving handles (strings, buffers,
         * nested objects) unchanged. */
        return value < 0 ? -1
                         : nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1,
                                                 -1, 1, 0);
      }
    }
    return nyr_fail(
               b,
               "native NYIR lower: member access '%s' has unsupported semantic "
               "representation %d (resolved=%d, call=%d, callee=%s) in %s at "
               "%s:%d",
               e->as.member.name, (int)e->as.member.target->semantic.rep,
               e->as.member.target->semantic.resolved ? 1 : 0,
               (int)e->semantic.member_call_kind,
               e->semantic.canonical_callee ? e->semantic.canonical_callee
                                            : "<none>",
               b->current_fn_name ? b->current_fn_name : "<unknown>",
               e->tok.filename ? e->tok.filename : "<source>", e->tok.line),
           -1;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    ny_native_lambda_entry_t *lambda =
        ny_native_lambda_find(ny_native_lambda_cache_for(b), e);
    if (!lambda || !lambda->name) {
      return nyr_fail(b, "native NYIR lower: closure was not collected"), -1;
    }
    /* Address-taking uses the same linkage name as ordinary function values.
     * Keep this in shared lowering so assembly, object, and LLVM emission
     * consume one symbol instead of repairing generated names independently. */
    char function_symbol[512];
    int symbol_len = snprintf(function_symbol, sizeof(function_symbol),
                              "ny_fn_%s", lambda->name);
    if (symbol_len < 0 || (size_t)symbol_len >= sizeof(function_symbol)) {
      return nyr_fail(b, "native NYIR lower: function symbol too long"), -1;
    }
    /* Callback result ABI is part of the callable value. A captured lambda
     * can escape through `any` and be invoked after its source expression is
     * gone, so call-site inspection alone cannot recover a declared raw-int
     * or bool result. Record that result representation at closure creation;
     * later argument markers preserve these flags. */
    const char *result_marker = NULL;
    if (lambda->fn && lambda->fn->kind == NY_S_FUNC) {
      int result_kind = nyr_fn_result_kind(b, lambda->fn);
      bool callback_params_raw = true;
      bool callback_params_fnptr_only = lambda->fn->as.fn.params.len > 0;
      for (size_t i = 0; i < lambda->fn->as.fn.params.len; ++i) {
        const param_t *param = &lambda->fn->as.fn.params.data[i];
        callback_params_fnptr_only &=
            param->type && strstr(param->type, "fnptr") != NULL;
        bool try_dynamic = nyr_param_is_try_dynamic(b, lambda->fn, i);
        bool inferred_list =
            !try_dynamic && nyr_param_is_inferred_list(lambda->fn, i);
        bool dynamic_sequence =
            !try_dynamic && nyr_param_uses_dynamic_sequence_abi(lambda->fn, i);
        ny_sem_rep_t rep =
            try_dynamic ? NY_SEM_REP_TAGGED_DYNAMIC
                        : (param->semantic.resolved ? param->semantic.rep
                                                    : NY_SEM_REP_UNRESOLVED);
        bool dynamic_value_abi =
            try_dynamic || dynamic_sequence ||
            ny_native_type_name_is_any(param->type) ||
            (!param->type && (!inferred_list || dynamic_sequence) &&
             (rep == NY_SEM_REP_TAGGED_DYNAMIC ||
              rep == NY_SEM_REP_UNRESOLVED) &&
             !nyr_untyped_param_is_raw(lambda->fn, param->name));
        if (dynamic_value_abi) {
          callback_params_raw = false;
          break;
        }
      }
      bool returns_f64 =
          (lambda->fn->as.fn.return_semantic.resolved &&
           lambda->fn->as.fn.return_semantic.rep == NY_SEM_REP_F64) ||
          ny_native_type_name_is_f64(lambda->fn->as.fn.return_type);
      if (result_kind == 2)
        result_marker = callback_params_fnptr_only
                            ? "rt_mark_dynamic_bool_callable_tagged_word_args"
                        : callback_params_raw
                            ? "rt_mark_dynamic_bool_callable_word_args"
                            : "rt_mark_dynamic_bool_callable_tagged_args";
      else if (returns_f64)
        result_marker =
            callback_params_fnptr_only
                ? "rt_mark_dynamic_callable_tagged_word_args_f64_result"
            : callback_params_raw
                ? "rt_mark_dynamic_callable_word_args_f64_result"
                : "rt_mark_dynamic_callable_tagged_args_f64_result";
      else if (result_kind == 0 && lambda->fn->as.fn.return_type != NULL)
        result_marker =
            callback_params_fnptr_only
                ? "rt_mark_dynamic_callable_tagged_word_args_raw_result"
            : callback_params_raw
                ? "rt_mark_dynamic_callable_word_args_raw_result"
                : "rt_mark_dynamic_callable_tagged_args_raw_result";
      else if (callback_params_fnptr_only)
        result_marker = "rt_mark_dynamic_callable_tagged_word_args";
      else if (!callback_params_raw)
        result_marker = "rt_mark_dynamic_callable_tagged_args";
      else if (lambda->fn->as.fn.params.len > 0)
        result_marker = "rt_mark_dynamic_callable";
    }
    /* Capture only names that are locals of the enclosing native frame.  The
     * collector may have seen the lambda before this frame was lowered, so
     * complete the precise capture set at the value-creation site. */
    for (size_t i = 0; i < b->local_count; ++i) {
      nyr_local_t *outer = &b->locals[i];
      if (!outer->name || ny_native_lambda_param_named(e, outer->name) ||
          !nyr_stmt_uses_ident(e->as.lambda.body, outer->name))
        continue;
      if (!ny_native_lambda_capture_add(lambda, outer->name)) {
        return nyr_fail(b, "native NYIR: closure capture table is full"), -1;
      }
    }
    if (lambda->capture_count > 0) {
      int count = nyr_emit_const(b, (int64_t)lambda->capture_count * 8);
      int env = count < 0 ? -1
                          : nyr_emit_runtime_call(b, "rt_malloc", count, -1, -1,
                                                  1, 0);
      if (env < 0)
        return -1;
      for (size_t i = 0; i < lambda->capture_count; ++i) {
        nyr_local_t *outer = nyr_find_local(b, lambda->capture_names[i]);
        /* capture_types describes the ABI the generated lambda was compiled
         * against.  Do not overwrite it with the enclosing frame's storage
         * representation here: top-level/untyped locals commonly hold a
         * tagged dynamic word while the lambda body was proven to consume a
         * raw scalar.  Mutating the table after the synthetic function was
         * lowered both loses that contract and makes repeated closure
         * materialization depend on which frame happened to run first. */
        nyr_local_t capture_abi = lambda->capture_types[i];
        if (!outer) {
          const expr_t *top =
              nyr_find_top_level_value(b, lambda->capture_names[i]);
          if (top && !capture_abi.is_list && !capture_abi.is_dyn_list &&
              !capture_abi.is_cstr && !capture_abi.is_bytes &&
              !capture_abi.is_dict && !capture_abi.is_any &&
              !capture_abi.is_f64 && !capture_abi.is_f32 &&
              !capture_abi.is_bool && capture_abi.semantic_rep == 0) {
            capture_abi.is_list = nyr_expr_is_list(b, top);
            capture_abi.is_dyn_list = nyr_expr_is_dyn_list(b, top);
            capture_abi.is_cstr = nyr_expr_is_cstr(b, (expr_t *)top);
            capture_abi.is_bytes = nyr_expr_is_bytes(b, top);
            capture_abi.is_dict = nyr_expr_is_dict(b, top);
            capture_abi.is_any = nyr_expr_is_any(b, (expr_t *)top);
            capture_abi.is_f64 = nyr_expr_is_f64(b, top);
            capture_abi.is_f32 = nyr_expr_is_f32(b, top);
            capture_abi.is_bool = nyr_expr_is_bool(b, (expr_t *)top);
            if (top->semantic.resolved) {
              capture_abi.semantic_rep = top->semantic.rep;
              capture_abi.semantic_ownership = top->semantic.ownership;
              capture_abi.alias_class = top->semantic.alias_class;
              capture_abi.semantic_mutable = top->semantic.mutable_value;
            }
          }
        }
        int value = outer ? nyr_load_local_value(b, outer->slot) : -1;
        if (!outer) {
          /* Root immutable bindings may be emitted as module storage rather
           * than an entry-frame local.  They are still valid closure inputs;
           * load the registered cell at materialization time instead of
           * pretending every capture must be lexical.  Mutable root cells
           * are excluded from the capture table and resolve directly inside
           * the lambda, preserving shared-state semantics. */
          const char *global = nyr_global_symbol(b, lambda->capture_names[i]);
          if (global) {
            int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                         .dst = -1,
                                                         .a = -1,
                                                         .b = -1,
                                                         .imm = 0,
                                                         .symbol = global});
            value = addr < 0
                        ? -1
                        : (capture_abi.is_f64 ? nyr_emit_load_f64(b, addr)
                                              : nyr_emit_load_i64(b, addr));
          }
        }
        nyr_scalar_class_t captured_class =
            value >= 0 ? nyr_value_scalar_class(b, value) : NYIR_SCALAR_UNKNOWN;
        bool capture_type_unresolved =
            !capture_abi.is_any && !capture_abi.is_cstr &&
            !capture_abi.is_bytes && !capture_abi.is_dict &&
            !capture_abi.is_list && !capture_abi.is_dyn_list &&
            !capture_abi.is_f64 && !capture_abi.is_f32 &&
            capture_abi.semantic_rep == NY_SEM_REP_UNRESOLVED;
        if (capture_type_unresolved && (captured_class == NYIR_SCALAR_F64 ||
                                        captured_class == NYIR_SCALAR_F32)) {
          /* The collector can miss the concrete lane of a local assigned by
           * numeric arithmetic. Preserve the actual producer ABI for the
           * synthetic lambda before its body is emitted. */
          capture_abi.is_f64 = captured_class == NYIR_SCALAR_F64;
          capture_abi.is_f32 = captured_class == NYIR_SCALAR_F32;
          capture_abi.semantic_rep =
              capture_abi.is_f64 ? NY_SEM_REP_F64 : NY_SEM_REP_F32;
          lambda->capture_types[i] = capture_abi;
        }
        /* The closure environment is a typed ABI boundary.  If the enclosing
         * local is dynamic/tagged but the already-lowered lambda capture is a
         * scalar integer, decode once before storing the environment slot.
         * Handles, containers, floats and captures whose lambda ABI is itself
         * dynamic must retain their original representation. */
        bool capture_raw_scalar =
            !capture_abi.is_any && !capture_abi.is_cstr &&
            !capture_abi.is_bytes && !capture_abi.is_dict &&
            !capture_abi.is_list && !capture_abi.is_dyn_list &&
            !capture_abi.is_f64 && !capture_abi.is_f32 &&
            !capture_abi.is_bool &&
            capture_abi.semantic_rep != NY_SEM_REP_TAGGED_DYNAMIC;
        if (value >= 0 && outer && outer->is_any && capture_raw_scalar) {
          value =
              nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        /* Closure environments are byte-addressed and may contain captures
         * with different representations in adjacent slots. Store floats as
         * their IEEE bits in the generic word slot; the lambda entry restores
         * the F64 lane with rt_f64_from_bits. A direct F64 memory store here
         * would claim a floating memory class for an untyped malloc block. */
        if (value >= 0 && (capture_abi.is_f64 || capture_abi.is_f32)) {
          nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
          int as_f64 = value;
          if (value_class == NYIR_SCALAR_F32)
            as_f64 = nyr_emit_f32_to_f64(b, value);
          else if (value_class == NYIR_SCALAR_I64_TAGGED)
            as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                           NYIR_SCALAR_F64);
          else if (value_class != NYIR_SCALAR_F64)
            as_f64 = nyr_emit_i64_to_f64(b, value);
          int bits = as_f64 < 0 ? -1
                                : nyr_emit_runtime_call(b, "rt_f64_bits",
                                                        as_f64, -1, -1, 1, 0);
          if (bits < 0)
            return -1;
          value = bits;
        }
        int off = value < 0 ? -1 : nyr_emit_const(b, (int64_t)i * 8);
        int addr = value < 0 || off < 0 ? -1 : nyr_emit_add_i64(b, env, off);
        if (addr < 0 || !nyr_emit_store_i64(b, addr, value))
          return -1;
      }
      int cls_size = nyr_emit_const(b, 16);
      int closure =
          cls_size < 0
              ? -1
              : nyr_emit_runtime_call(b, "rt_malloc", cls_size, -1, -1, 1, 0);
      int code =
          closure < 0
              ? -1
              : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .imm = 0,
                                                  .symbol = function_symbol});
      int tag_off = closure < 0 ? -1 : nyr_emit_const(b, -8);
      int tag_addr = closure < 0 || tag_off < 0
                         ? -1
                         : nyr_emit_add_i64(b, closure, tag_off);
      int env_off = closure < 0 ? -1 : nyr_emit_const(b, 8);
      int env_addr = closure < 0 || env_off < 0
                         ? -1
                         : nyr_emit_add_i64(b, closure, env_off);
      int tag = closure < 0 ? -1 : nyr_emit_const(b, TAG_CLOSURE);
      if (closure < 0 || code < 0 || tag_addr < 0 || env_addr < 0 || tag < 0 ||
          !nyr_emit_store_i64(b, tag_addr, tag) ||
          !nyr_emit_store_i64(b, closure, code) ||
          !nyr_emit_store_i64(b, env_addr, env))
        return -1;
      return result_marker ? nyr_emit_runtime_call(b, result_marker, closure,
                                                   -1, -1, 1, 0)
                           : closure;
    }
    int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .imm = 0,
                                                 .symbol = function_symbol});
    if (addr < 0)
      nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
    return result_marker
               ? nyr_emit_runtime_call(b, result_marker, addr, -1, -1, 1, 0)
               : addr;
  }
  default:
    return nyr_fail(
               b,
               "native NYIR lower: expression kind %d ('%.*s') at %s:%d in %s "
               "is not "
               "in shared NYIR yet",
               (int)e->kind, (int)e->tok.len,
               e->tok.lexeme ? e->tok.lexeme : "",
               e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
               b->current_fn_name ? b->current_fn_name : "<unknown>"),
           -1;
  }
}

static int nyr_lower_expr(nyr_builder_t *b, const expr_t *e) {
  if (!b)
    return -1;
  size_t debug_start = b ? b->nyir.len : 0;
  ny_expr_semantic_t semantic = e ? e->semantic : (ny_expr_semantic_t){0};
  if (e && e->tok.kind == NY_T_NIL) {
    semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
    semantic.resolved = true;
  }
  if (e && e->kind == NY_E_IDENT && e->as.ident.name) {
    nyr_local_t *local =
        nyr_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!local)
      local = nyr_find_local_nearest(b, e->as.ident.name);
    if (local && local->is_any) {
      semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
      semantic.ownership = local->semantic_ownership;
      semantic.alias_class = local->alias_class;
      semantic.mutable_value = local->semantic_mutable;
      semantic.resolved = true;
    } else if (local && local->semantic_rep != NY_SEM_REP_UNRESOLVED) {
      /* The slot's concrete carrier is authoritative over a stale
       * use-site annotation.  Typed locals can inherit TAGGED_DYNAMIC from
       * an earlier generic inference pass even though the binder and the
       * defining instruction both establish a raw int (for example
       * `x15 = i64buf_load(...)`, followed by `x15 >> 3` in SHA-256).
       * Keeping the expression stamp here tagged routes raw shifts and
       * additions through rt_tag/rt_any_add and corrupts the schedule. */
      semantic.rep = local->semantic_rep;
      semantic.ownership = local->semantic_ownership;
      semantic.alias_class = local->alias_class;
      semantic.mutable_value = local->semantic_mutable;
      semantic.resolved = true;
    } else if (!semantic.resolved) {
      const expr_t *global = nyr_find_top_level_value(b, e->as.ident.name);
      if (!global && b->current_fn_name) {
        const char *dot = strrchr(b->current_fn_name, '.');
        if (dot && dot != b->current_fn_name) {
          const char *qualified = nyr_builder_own_qualified_prefix(
              b, b->current_fn_name, (size_t)(dot - b->current_fn_name),
              e->as.ident.name);
          if (qualified)
            global = nyr_find_top_level_value(b, qualified);
        }
      }
      if (global && global->semantic.resolved)
        semantic = global->semantic;
    }
  } else if (e && !semantic.resolved && e->kind == NY_E_CALL &&
             e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name) {
    const stmt_t *callee_fn =
        nyr_find_user_function(b, e->as.call.callee->as.ident.name);
    if (callee_fn && callee_fn->kind == NY_S_FUNC) {
      if (callee_fn->as.fn.return_semantic.resolved) {
        semantic = callee_fn->as.fn.return_semantic;
      } else if (callee_fn->as.fn.return_type) {
        /* The shared rep mapping classifies every declared spelling; the
         * hand-rolled chain it replaced stamped aggregate/pointer call
         * results as raw int. */
        semantic.rep = ny_semantic_rep_for_type(callee_fn->as.fn.return_type);
        semantic.resolved = true;
      }
    }
  }
  if (!semantic.resolved) {
    semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
    semantic.ownership = NY_SEM_OWN_UNKNOWN;
    semantic.resolved = true;
  }
  /* A proven exclusive-borrow call result has a concrete ownership fact even
   * when the type checker left the expression's general ownership unknown.
   * The generic expression annotation below is applied after call lowering,
   * so carry the proof here rather than letting that fallback erase it. */
  if (e && e->kind == NY_E_CALL && e->semantic.exclusive_borrow_proven) {
    semantic.ownership = NY_SEM_OWN_BORROWED;
    semantic.resolved = true;
  }
  int value = nyr_lower_expr_impl(b, e);
  /* Index lowering can refine a conservative source fact after it proves the
   * active element layout (for example, an append-built list<int> read).
   * Native intrinsic calls and binary operations can also publish a concrete
   * f64 result, and float-to-int conversion publishes raw-int, when the
   * semantic pass was skipped. Carry only these explicit refinements when
   * their representation agrees with the emitted SSA value. */
  if (e &&
      (e->kind == NY_E_INDEX ||
       (e->kind == NY_E_CALL && (e->semantic.rep == NY_SEM_REP_F64 ||
                                 e->semantic.rep == NY_SEM_REP_RAW_INT)) ||
       (e->kind == NY_E_BINARY && e->semantic.rep == NY_SEM_REP_F64)) &&
      e->semantic.resolved &&
      (!semantic.resolved || semantic.rep != e->semantic.rep) && value >= 0) {
    nyr_scalar_class_t expected =
        ny_native_scalar_class_from_sem_rep(e->semantic.rep);
    nyr_scalar_class_t actual = nyr_value_scalar_class(b, value);
    if (expected != NYIR_SCALAR_UNKNOWN && actual == expected) {
      semantic.rep = e->semantic.rep;
      semantic.resolved = true;
    }
  }
  if (e && (e->kind == NY_E_BINARY || e->kind == NY_E_CALL) &&
      ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("native-expr", "%s %s=%s",
                           b->current_fn_name ? b->current_fn_name : "<entry>",
                           e->kind == NY_E_BINARY ? "bin" : "call",
                           value < 0 ? "err" : "ok");
  /* Keep source provenance on the NYIR itself.  The lowering helpers emit
   * several instructions per expression, so annotate the whole newly-emitted
   * range while retaining precise locations already assigned by nested
   * expressions.  This is consumed by verifier/dump diagnostics and is the
   * source map used by native debug backends. */
  if (b && e && e->tok.line > 0) {
    const char *file = e->tok.filename ? e->tok.filename : b->source_file;
    for (size_t i = debug_start; i < b->nyir.len; ++i) {
      nyr_inst_t *inst = &b->nyir.data[i];
      if (!inst->debug.line) {
        inst->debug.file = file;
        inst->debug.line = (uint32_t)e->tok.line;
        inst->debug.column = (uint32_t)(e->tok.col > 0 ? e->tok.col : 1);
      }
    }
  }
  if (value < 0 || !e || !semantic.resolved)
    return value;
  for (size_t i = b->nyir.len; i > 0; --i) {
    nyr_inst_t *inst = &b->nyir.data[i - 1];
    if (inst->dst != value)
      continue;
    /* The semantic annotation describes this expression's consumer-facing
     * representation; an explicit producer class describes the SSA value
     * itself. A concrete f64 arithmetic result may flow into a tagged `any`
     * boundary, but that must not restamp its defining MUL_F64 as tagged.
     * Preserve every explicit producer class and use semantic fallback only
     * where lowering emitted no class. */
    nyr_scalar_class_t producer_class = nyr_value_scalar_class(b, value);
    bool concrete_float_producer =
        producer_class == NYIR_SCALAR_F32 || producer_class == NYIR_SCALAR_F64;
    bool raw_integer_producer =
        b->last_arith_expr == e && b->last_arith_raw &&
        (inst->op == NYIR_ADD_I64 || inst->op == NYIR_SUB_I64 ||
         inst->op == NYIR_MUL_I64 || inst->op == NYIR_DIV_I64 ||
         inst->op == NYIR_MOD_I64 || inst->op == NYIR_AND_I64 ||
         inst->op == NYIR_OR_I64 || inst->op == NYIR_XOR_I64 ||
         inst->op == NYIR_SHL_I64 || inst->op == NYIR_SAR_I64 ||
         inst->op == NYIR_ROR_I64 || inst->op == NYIR_CMP_I64);
    /* Small integer literals are emitted as raw NYIR constants even when
     * type inference left a conservative tagged-dynamic source stamp.  Keep
     * that producer fact exact so a typed i64 formal does not untag `21`
     * into `10` at the call boundary.  Large literals lower through the
     * BigInt constructors and are deliberately excluded. */
    int64_t raw_integer_value = 0;
    bool raw_integer_literal =
        ((e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
          e->tok.kind != NY_T_NIL &&
          (raw_integer_value = e->as.literal.as.i, true)) ||
         nyr_negative_literal_i64(e, &raw_integer_value)) &&
        raw_integer_value > -(INT64_C(1) << 62) &&
        raw_integer_value < (INT64_C(1) << 62) && inst->op == NYIR_CONST_I64;
    bool class_authoritative = inst->scalar_class != NYIR_SCALAR_UNKNOWN ||
                               concrete_float_producer || raw_integer_producer;
    if (inst->scalar_class == NYIR_SCALAR_UNKNOWN && concrete_float_producer)
      inst->scalar_class = producer_class;
    if (raw_integer_producer || raw_integer_literal)
      inst->scalar_class = NYIR_SCALAR_I64_RAW;
    else if (!class_authoritative)
      inst->scalar_class = ny_native_scalar_class_from_sem_rep(semantic.rep);
    if (raw_integer_literal && ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event(
          "NATIVE_ABI",
          "literal=%lld producer=i64-raw semantic=%s; preserve raw constant",
          (long long)raw_integer_value, ny_sem_rep_name(semantic.rep));
    inst->semantic_ownership = (uint8_t)semantic.ownership;
    inst->semantic_mutable = semantic.mutable_value;
    inst->alias_class = semantic.alias_class;
    break;
  }
  return value;
}

static int nyr_lower_logical(nyr_builder_t *b, const expr_t *left,
                             const expr_t *right, bool is_or) {
  if (left && left->kind == NY_E_LITERAL) {
    if (left->as.literal.kind == NY_LIT_INT) {
      int64_t val = left->as.literal.as.i;
      if (is_or) {
        if (val != 0)
          return nyr_emit_const(b, 1);
        return nyr_lower_expr(b, right);
      } else {
        if (val == 0)
          return nyr_emit_const(b, 0);
        return nyr_lower_expr(b, right);
      }
    } else if (left->as.literal.kind == NY_LIT_BOOL) {
      bool val = left->as.literal.as.b;
      if (is_or) {
        if (val)
          return nyr_emit_const(b, 1);
        return nyr_lower_expr(b, right);
      } else {
        if (!val)
          return nyr_emit_const(b, 0);
        return nyr_lower_expr(b, right);
      }
    }
  }

  int result_slot = nyr_temp_slot(b);
  int zero = nyr_emit_const(b, 0);
  int one = nyr_emit_const(b, 1);
  if (zero < 0 || one < 0)
    return -1;
  int true_label = b->next_label++;
  int end_label = b->next_label++;

  if (!nyr_store_local_value(b, result_slot, zero))
    return -1;

  int lhs = nyr_lower_expr(b, left);
  if (lhs < 0)
    return -1;

  if (is_or) {
    if (!nyr_emit_br_if(b, lhs, true_label))
      return -1;
  } else {
    /* Normalize first: a boxed false is NY_IMM_FALSE=2, and a raw != 0 test
     * alone would treat it as truthy (rule110's `if l && c && r` returned
     * all-true for three boxed falses and the automaton froze). */
    int lhs_truthy = nyr_emit_truthy(b, lhs);
    int lhs_zero = lhs_truthy < 0 ? -1 : nyr_emit_is_zero(b, lhs_truthy);
    if (lhs_zero < 0 || !nyr_emit_br_if(b, lhs_zero, end_label))
      return -1;
  }

  int rhs = nyr_lower_expr(b, right);
  if (rhs < 0)
    return -1;

  if (is_or) {
    if (!nyr_emit_br_if(b, rhs, true_label) || !nyr_emit_br(b, end_label))
      return -1;
  } else {
    int rhs_truthy = nyr_emit_truthy(b, rhs);
    int rhs_zero = rhs_truthy < 0 ? -1 : nyr_emit_is_zero(b, rhs_truthy);
    if (rhs_zero < 0 || !nyr_emit_br_if(b, rhs_zero, end_label))
      return -1;
  }

  if (!nyr_emit_label(b, true_label) ||
      !nyr_store_local_value(b, result_slot, one) || !nyr_emit_br(b, end_label))
    return -1;
  if (!nyr_emit_label(b, end_label))
    return -1;
  int merged = nyr_load_local_value(b, result_slot);
  return merged;
}

/* Lower a boolean expression directly into control flow. This preserves
 * short-circuit order without materializing intermediate booleans in locals
 * and PHIs, and keeps a value produced by the left operand available on the
 * path where the right operand runs. */
static bool nyr_lower_cond_branch(nyr_builder_t *b, const expr_t *condition,
                                  int true_label, int false_label) {
  if (condition && condition->kind == NY_E_LOGICAL &&
      condition->as.logical.op && condition->as.logical.left &&
      condition->as.logical.right) {
    bool is_and = strcmp(condition->as.logical.op, "&&") == 0;
    bool is_or = strcmp(condition->as.logical.op, "||") == 0;
    if (is_and || is_or) {
      int rhs_label = b->next_label++;
      bool left_ok = is_and
                         ? nyr_lower_cond_branch(b, condition->as.logical.left,
                                                 rhs_label, false_label)
                         : nyr_lower_cond_branch(b, condition->as.logical.left,
                                                 true_label, rhs_label);
      if (!left_ok || !nyr_emit_label(b, rhs_label))
        return false;
      return nyr_lower_cond_branch(b, condition->as.logical.right, true_label,
                                   false_label);
    }
  }
  int value = nyr_lower_expr(b, condition);
  return value >= 0 && nyr_emit_br_if(b, value, true_label) &&
         nyr_emit_br(b, false_label);
}

static int nyr_box_float_dynamic(nyr_builder_t *b, int value) {
  nyr_scalar_class_t value_class = nyr_value_scalar_class(b, value);
  if (value_class == NYIR_SCALAR_F32)
    value = nyr_emit_f32_to_f64(b, value);
  else if (value_class != NYIR_SCALAR_F64)
    return value;
  if (value < 0)
    return -1;
  int bits = nyr_emit_runtime_call(b, "rt_f64_bits", value, -1, -1, 1,
                                   NYIR_SCALAR_I64_RAW);
  return bits < 0 ? -1
                  : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1, 1,
                                          NYIR_SCALAR_I64_TAGGED);
}

static const stmt_t *nyr_callable_function_value(nyr_builder_t *b,
                                                 const expr_t *value) {
  if (!b || !value)
    return NULL;
  if (value->kind == NY_E_FN || value->kind == NY_E_LAMBDA) {
    ny_native_lambda_entry_t *entry =
        ny_native_lambda_find(ny_native_lambda_cache_for(b), value);
    return entry ? entry->fn : NULL;
  }
  if (value->kind == NY_E_IDENT && value->as.ident.name) {
    const nyr_local_t *local = nyr_find_expr_local(b, value);
    if (local) {
      if (local->callable_name)
        return nyr_find_user_function(b, local->callable_name);
      if (local->callable_expr)
        return nyr_callable_function_value(b, local->callable_expr);
      return NULL;
    }
    return nyr_find_user_function(b, value->as.ident.name);
  }
  if (value->kind == NY_E_MEMBER)
    return nyr_namespace_function_value((nyr_builder_impl_t *)b, value);
  return NULL;
}

static int nyr_lower_callable_ternary_arm(nyr_builder_t *b,
                                          const expr_t *value) {
  int result = nyr_lower_expr(b, value);
  const stmt_t *callable = nyr_callable_function_value(b, value);
  if (result >= 0 && callable) {
    const char *marker = nyr_callable_storage_marker(b, callable);
    result = nyr_emit_runtime_call(b, marker, result, -1, -1, 1,
                                   NYIR_SCALAR_I64_TAGGED);
  }
  return result;
}

static int nyr_lower_coalesce(nyr_builder_t *b, const expr_t *coalesce,
                              const expr_t *left, const expr_t *fallback) {
  if (!coalesce || !left || !fallback)
    return nyr_fail(b,
                    "native NYIR lower: malformed nil coalescing expression"),
           -1;
  if (ny_expr_is_nil_literal(left))
    return nyr_lower_callable_ternary_arm(b, fallback);

  int left_val = nyr_lower_callable_ternary_arm(b, left);
  if (left_val < 0)
    return -1;
  ny_sem_rep_t left_rep =
      left->semantic.resolved ? left->semantic.rep : NY_SEM_REP_UNRESOLVED;
  bool dynamic_left =
      nyr_expr_is_any(b, left) || left_rep == NY_SEM_REP_TAGGED_DYNAMIC;
  /* A boolean can use the raw 0/1 ABI even when its surrounding expression
   * is dynamic. Raw false is zero, but the language false immediate is not
   * nil; select it directly and box it only if the result requires `any`. */
  /* Optional chaining is represented as a ternary whose nil arm can remain
   * typed as the accessed member's concrete type.  That representation is
   * not proof that the expression is non-nil: the nil arm is still a runtime
   * possibility and must reach the coalescing fallback. */
  bool optional_nil_arm = left->kind == NY_E_TERNARY &&
                          ny_expr_is_nil_literal(left->as.ternary.false_expr);
  bool known_non_nil = !optional_nil_arm &&
                       (nyr_expr_is_bool(b, left) ||
                        (!dynamic_left && left_rep != NY_SEM_REP_UNRESOLVED));
  bool dynamic_join = coalesce->semantic.resolved &&
                      coalesce->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC;
  dynamic_join = dynamic_join || dynamic_left || nyr_expr_is_any(b, fallback) ||
                 nyr_expr_is_canonical_dynamic_value(b, fallback);
  if (known_non_nil) {
    if (dynamic_join &&
        nyr_value_scalar_class(b, left_val) == NYIR_SCALAR_I64_RAW)
      left_val = nyr_expr_is_bool(b, left)
                     ? nyr_box_bool(b, left_val)
                     : nyr_box_raw_dynamic_value(b, left_val);
    if (left_val >= 0 && dynamic_join)
      left_val = nyr_box_float_dynamic(b, left_val);
    return left_val;
  }

  int result_slot = nyr_temp_slot(b);
  int fallback_label = b->next_label++;
  int end_label = b->next_label++;
  int nil = nyr_emit_const(b, NY_IMM_NIL);
  int is_nil = nil < 0 ? -1
                       : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                           .dst = -1,
                                                           .a = left_val,
                                                           .b = nil,
                                                           .cmp = NYIR_CMP_EQ});
  if (is_nil < 0 || !nyr_emit_br_if(b, is_nil, fallback_label))
    return -1;
  if (dynamic_join &&
      nyr_value_scalar_class(b, left_val) == NYIR_SCALAR_I64_RAW)
    left_val = nyr_expr_is_bool(b, left)
                   ? nyr_box_bool(b, left_val)
                   : nyr_box_raw_dynamic_value(b, left_val);
  if (left_val >= 0 && dynamic_join)
    left_val = nyr_box_float_dynamic(b, left_val);
  if (left_val < 0 || !nyr_store_local_value(b, result_slot, left_val))
    return -1;
  if (dynamic_join && b->nyir.len > 0) {
    nyr_inst_t *store = &b->nyir.data[b->nyir.len - 1];
    if (store->op == NYIR_STORE_LOCAL && store->imm == result_slot)
      store->scalar_class = NYIR_SCALAR_I64_TAGGED;
  }
  if (!nyr_emit_br(b, end_label))
    return -1;
  if (!nyr_emit_label(b, fallback_label))
    return -1;
  int fallback_val = nyr_lower_callable_ternary_arm(b, fallback);
  if (fallback_val >= 0 && dynamic_join &&
      nyr_value_scalar_class(b, fallback_val) == NYIR_SCALAR_I64_RAW)
    fallback_val = nyr_expr_is_bool(b, fallback)
                       ? nyr_box_bool(b, fallback_val)
                       : nyr_box_raw_dynamic_value(b, fallback_val);
  if (fallback_val >= 0 && dynamic_join)
    fallback_val = nyr_box_float_dynamic(b, fallback_val);
  if (fallback_val < 0 || !nyr_store_local_value(b, result_slot, fallback_val))
    return -1;
  if (dynamic_join && b->nyir.len > 0) {
    nyr_inst_t *store = &b->nyir.data[b->nyir.len - 1];
    if (store->op == NYIR_STORE_LOCAL && store->imm == result_slot)
      store->scalar_class = NYIR_SCALAR_I64_TAGGED;
  }
  if (!nyr_emit_label(b, end_label))
    return -1;
  int merged = nyr_load_local_value(b, result_slot);
  if (merged >= 0 && dynamic_join && b->nyir.len > 0) {
    nyr_inst_t *load = &b->nyir.data[b->nyir.len - 1];
    if (load->op == NYIR_LOAD_LOCAL && load->imm == result_slot)
      load->scalar_class = NYIR_SCALAR_I64_TAGGED;
  }
  return merged;
}

static int nyr_lower_ternary(nyr_builder_t *b, const expr_t *cond,
                             const expr_t *true_expr,
                             const expr_t *false_expr) {
  if (!cond || !true_expr || !false_expr) {
    return nyr_fail(b, "native NYIR lower: malformed ternary expression"), -1;
  }
  if (cond->kind == NY_E_LITERAL) {
    if (cond->as.literal.kind == NY_LIT_INT) {
      return cond->as.literal.as.i != 0
                 ? nyr_lower_callable_ternary_arm(b, true_expr)
                 : nyr_lower_callable_ternary_arm(b, false_expr);
    }
    if (cond->as.literal.kind == NY_LIT_BOOL) {
      return cond->as.literal.as.b
                 ? nyr_lower_callable_ternary_arm(b, true_expr)
                 : nyr_lower_callable_ternary_arm(b, false_expr);
    }
  }
  int result_slot = nyr_temp_slot(b);
  int true_label = b->next_label++;
  int else_label = b->next_label++;
  int end_label = b->next_label++;

  int cond_val = nyr_lower_expr(b, cond);
  if (cond_val < 0)
    return -1;
  if (!nyr_emit_br_if(b, cond_val, true_label) || !nyr_emit_br(b, else_label) ||
      !nyr_emit_label(b, true_label))
    return -1;

  int true_val = nyr_lower_callable_ternary_arm(b, true_expr);
  bool true_dynamic_input = true_expr->kind == NY_E_BINARY &&
                            (nyr_expr_is_any(b, true_expr->as.binary.left) ||
                             nyr_expr_is_any(b, true_expr->as.binary.right));
  bool false_dynamic_input = false_expr->kind == NY_E_BINARY &&
                             (nyr_expr_is_any(b, false_expr->as.binary.left) ||
                              nyr_expr_is_any(b, false_expr->as.binary.right));
  bool dynamic_join = nyr_expr_is_any(b, true_expr) ||
                      nyr_expr_is_canonical_dynamic_value(b, true_expr) ||
                      nyr_expr_is_any(b, false_expr) ||
                      nyr_expr_is_canonical_dynamic_value(b, false_expr) ||
                      true_dynamic_input || false_dynamic_input;
  if (true_val >= 0 && dynamic_join &&
      nyr_value_scalar_class(b, true_val) == NYIR_SCALAR_I64_RAW)
    true_val = nyr_expr_is_bool(b, true_expr)
                   ? nyr_box_bool(b, true_val)
                   : nyr_box_raw_dynamic_value(b, true_val);
  if (true_val >= 0 && dynamic_join)
    true_val = nyr_box_float_dynamic(b, true_val);
  if (true_val < 0 || !nyr_store_local_value(b, result_slot, true_val))
    return -1;
  if (dynamic_join && b->nyir.len > 0) {
    nyr_inst_t *store = &b->nyir.data[b->nyir.len - 1];
    if (store->op == NYIR_STORE_LOCAL && store->imm == result_slot)
      store->scalar_class = NYIR_SCALAR_I64_TAGGED;
  }
  if (!nyr_emit_br(b, end_label))
    return -1;

  if (!nyr_emit_label(b, else_label))
    return -1;
  int false_val = nyr_lower_callable_ternary_arm(b, false_expr);
  if (false_val >= 0 && dynamic_join &&
      nyr_value_scalar_class(b, false_val) == NYIR_SCALAR_I64_RAW)
    false_val = nyr_expr_is_bool(b, false_expr)
                    ? nyr_box_bool(b, false_val)
                    : nyr_box_raw_dynamic_value(b, false_val);
  if (false_val >= 0 && dynamic_join)
    false_val = nyr_box_float_dynamic(b, false_val);
  if (false_val < 0 || !nyr_store_local_value(b, result_slot, false_val))
    return -1;
  if (dynamic_join && b->nyir.len > 0) {
    nyr_inst_t *store = &b->nyir.data[b->nyir.len - 1];
    if (store->op == NYIR_STORE_LOCAL && store->imm == result_slot)
      store->scalar_class = NYIR_SCALAR_I64_TAGGED;
  }
  if (!nyr_emit_label(b, end_label))
    return -1;

  int merged = nyr_load_local_value(b, result_slot);
  if (merged >= 0 && dynamic_join && b->nyir.len > 0) {
    nyr_inst_t *load = &b->nyir.data[b->nyir.len - 1];
    if (load->op == NYIR_LOAD_LOCAL && load->imm == result_slot)
      load->scalar_class = NYIR_SCALAR_I64_TAGGED;
  }
  if (merged >= 0 && !dynamic_join && b->nyir.len > 0) {
    nyr_scalar_class_t true_class = nyr_value_scalar_class(b, true_val);
    nyr_scalar_class_t false_class = nyr_value_scalar_class(b, false_val);
    nyr_inst_t *load = &b->nyir.data[b->nyir.len - 1];
    if (true_class != NYIR_SCALAR_UNKNOWN && true_class == false_class &&
        load->op == NYIR_LOAD_LOCAL && load->imm == result_slot)
      load->scalar_class = true_class;
  }
  return merged;
}

static bool nyr_lower_stmt(nyr_builder_t *b, const stmt_t *s);
