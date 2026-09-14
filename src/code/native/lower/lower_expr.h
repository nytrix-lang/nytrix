static bool ny_native_nir_negative_literal_i64(const expr_t *e, int64_t *out) {
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

static int ny_native_nir_lower_expr_impl(ny_native_nir_builder_t *b,
                                         const expr_t *e) {
  if (!e) {
    ny_native_nir_fail(b, "native NYIR lower: missing expression");
    return -1;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL)
      return ny_native_nir_emit_const(b, e->as.literal.as.b ? 1 : 0);
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_F32)
                 ? ny_native_nir_emit_const_f32(b, e->as.literal.as.f)
                 : ny_native_nir_emit_const_f64(b, e->as.literal.as.f);
    if (e->tok.kind == NY_T_NIL)
      return ny_native_nir_emit_const(b, NY_IMM_NIL);
    if (e->as.literal.kind == NY_LIT_STR) {
      /*
       * C-string pointer via interned .Lnystr.N (appended into the object).
       */
      const char *s = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
      size_t slen = e->as.literal.as.s.len;
      const char *sym = ny_native_strtab_intern(s, slen, NULL, 0);
      if (!sym) {
        ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
        return -1;
      }
      int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = sym});
      if (addr < 0)
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      int length = ny_native_nir_emit_const(b, (int64_t)slen);
      int tag = ny_native_nir_emit_const(b, TAG_STR_CONST);
      if (length >= 0 && tag >= 0) {
        ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_STR_LEN,
                                      length);
        ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_TAG, tag);
      }
      return addr;
    }
    if (e->as.literal.kind != NY_LIT_INT) {
      ny_native_nir_fail(b, "native NYIR lower: only int/bool/f64/nil/string "
                            "literals are supported");
      return -1;
    }
    int64_t int_lit = e->as.literal.as.i;
    /*
     * The tagged-int encoding (v << 1 | 1) cannot hold |v| >= 2^62, so a
     * decimal literal at that magnitude is a bigint: materialize it as a
     * bigint object instead of a raw i64, so type(x) == "bigint" and
     * to_str(x) round-trip the decimal digits.
     */
    if (int_lit >= (INT64_C(1) << 62) || int_lit <= -(INT64_C(1) << 62)) {
      int raw = ny_native_nir_emit_const(b, int_lit);
      return raw < 0 ? -1
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_bigint_from_i64_raw", raw, -1, -1, 1, 0);
    }
    return ny_native_nir_emit_const(b, int_lit);
  case NY_E_EMBED: {
    const char *path = e->as.embed.path;
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
      ny_native_nir_fail(b, "native NYIR lower: failed to open embed file '%s'",
                         path ? path : "");
      return -1;
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
    const char *sym = ny_native_strtab_intern(data, len, NULL, 0);
    free(data);
    if (!sym)
      return -1;
    int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .symbol = sym});
    int length = ny_native_nir_emit_const(b, (int64_t)len);
    int tag = ny_native_nir_emit_const(b, TAG_STR_CONST);
    if (addr < 0 || length < 0 || tag < 0)
      return -1;
    ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_STR_LEN,
                                  length);
    ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_TAG, tag);
    return addr;
  }
  case NY_E_IDENT: {
    ny_native_nir_local_t *l =
        ny_native_nir_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
    /* Enum members are source-level constants and must win over short-name
     * globals collected from imported `#main` blocks (for example `Red` from
     * an unrelated module). Resolve the exact enum declaration before any
     * tail/global fallback can capture the identifier. */
    if (!l) {
      const stmt_t *enum_stmt = NULL;
      const stmt_enum_item_t *enum_item = NULL;
      int64_t enum_value = 0;
      if (ny_native_nir_find_enum_member(b, e->as.ident.name, &enum_stmt,
                                         &enum_item, &enum_value) &&
          enum_item && enum_item->fields.len == 0)
        return ny_native_nir_emit_const(b, enum_value);
    }
    if (!l && b->current_fn_name) {
      const char *dot = strrchr(b->current_fn_name, '.');
      if (dot && dot[1]) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                         (int)(dot - b->current_fn_name), b->current_fn_name,
                         e->as.ident.name);
        if (n > 0 && (size_t)n < sizeof(qualified)) {
          const char *global_name = ny_native_globaltab_name(qualified);
          if (global_name) {
            int addr =
                nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .imm = 0,
                                                  .symbol = global_name});
            if (addr < 0)
              return -1;
            int value = ny_native_nir_global_is_f64(b, global_name, e)
                            ? ny_native_nir_emit_load_f64(b, addr)
                            : ny_native_nir_emit_load_i64(b, addr);
            ny_native_nir_record_global_dyn_tag(b, global_name, value);
            return value;
          }
        }
      }
    }
    if (!l && b->module_name && b->module_name[0]) {
      const stmt_t *owner = NULL;
      for (size_t i = 0; i < b->prog->body.len && !owner; ++i)
        owner = ny_native_nir_find_module(b->prog->body.data[i],
                                          b->module_name, 0);
      const expr_t *local_global =
          ny_native_nir_find_module_member_in(owner, e->as.ident.name, 0);
      if (local_global && local_global != e && b->resolve_depth < 64) {
        ++b->resolve_depth;
        int r = ny_native_nir_lower_expr(b, local_global);
        --b->resolve_depth;
        return r;
      }
      char qualified[512];
      int n = snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name,
                       e->as.ident.name);
      const char *global_name = n > 0 && (size_t)n < sizeof(qualified)
                                    ? ny_native_globaltab_name(qualified)
                                    : NULL;
      if (global_name) {
        int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = global_name});
        if (addr < 0)
          return -1;
        int value = ny_native_nir_global_is_f64(b, global_name, e)
                        ? ny_native_nir_emit_load_f64(b, addr)
                        : ny_native_nir_emit_load_i64(b, addr);
        ny_native_nir_record_global_dyn_tag(b, global_name, value);
        return value;
      }
    }
    /* A same-source function definition shadows foreign globals and
     * top-level values when referenced as a value (notably a named callback
     * such as `filter(p)`).  Imported `#main`/self-test blocks register
     * short bindings that would otherwise capture the reference and emit
     * the foreign initializer with out-of-module lambdas. */
    if (!l && e->as.ident.name &&
        ny_native_nir_user_defined_fn(b, e->as.ident.name)) {
      char function_symbol[512];
      int symbol_len = snprintf(function_symbol, sizeof(function_symbol),
                                "ny_fn_%s", e->as.ident.name);
      if (symbol_len < 0 || (size_t)symbol_len >= sizeof(function_symbol)) {
        ny_native_nir_fail(b, "native NYIR lower: function symbol too long");
        return -1;
      }
      int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = function_symbol});
      if (addr < 0) {
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      return addr;
    }
    /* Prefer an exact global registration over a qualified tail match.  The
     * main program can legitimately define a name such as `d` while a
     * loaded stdlib module also exports `module.d`; resolving the tail first
     * silently redirects the user's local/global reference to the module
     * object. */
    if (!l && ny_native_globaltab_has(e->as.ident.name)) {
      /* A bare global registration can originate from an imported #main
       * block.  In the root program that short name must not capture an
       * unresolved read (or an enum member) from the user's source.  Keep
       * exact bare globals for the current module and for declarations whose
       * token belongs to the source currently being lowered. */
      const expr_t *source_global =
          b->source_file ? ny_native_nir_find_top_level_value_in_source(
                               b, e->as.ident.name, b->source_file)
                         : NULL;
      bool allow_bare_global =
          (b->module_name && b->module_name[0]) || source_global != NULL;
      if (!allow_bare_global)
        goto skip_exact_global;
      const char *global_name = ny_native_globaltab_name(e->as.ident.name);
      int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = global_name});
      if (addr < 0)
        return -1;
      int value = ny_native_nir_global_is_f64(b, global_name, e)
                      ? ny_native_nir_emit_load_f64(b, addr)
                      : ny_native_nir_emit_load_i64(b, addr);
      ny_native_nir_record_global_dyn_tag(b, global_name, value);
      return value;
    }
  skip_exact_global:
    if (!l && (b->module_name && b->module_name[0])) {
      const char *global_name = ny_native_globaltab_name_tail(e->as.ident.name);
      if (global_name) {
        int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = global_name});
        if (addr < 0)
          return -1;
        int value = ny_native_nir_global_is_f64(b, global_name, e)
                        ? ny_native_nir_emit_load_f64(b, addr)
                        : ny_native_nir_emit_load_i64(b, addr);
        ny_native_nir_record_global_dyn_tag(b, global_name, value);
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
              ? ny_native_nir_find_top_level_value(b, e->as.ident.name)
              : (b->source_file ? ny_native_nir_find_top_level_value_in_source(
                                      b, e->as.ident.name, b->source_file)
                                : NULL);
      if (!global && (!b->module_name || !b->module_name[0]))
        global = ny_native_nir_find_imported_value(b, e->as.ident.name);
      /*
       * Stdlib module defs are stored with a qualified name
       * (std.math.big.BF_PRECISION); a bare intra-module reference only
       * carries the leaf.  Resolve against the current module's namespace
       * first (derived from the qualified current_fn_name) so module-local
       * constants (both compile-time and runtime-computed defs) resolve.
       */
      if (!global && b->current_fn_name) {
        const char *dot = strrchr(b->current_fn_name, '.');
        if (dot && dot != b->current_fn_name && dot[1]) {
          char qualified[512];
          int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                           (int)(dot - b->current_fn_name), b->current_fn_name,
                           e->as.ident.name);
          if (n > 0 && (size_t)n < sizeof(qualified))
            global = ny_native_nir_find_top_level_value(b, qualified);
        }
      }
      if (global && global != e && global->kind != NY_E_IDENT) {
        if (b->resolve_depth >= 64) {
          global = NULL; /* over-deep/cyclic def chain: stop recursing */
        } else {
          ++b->resolve_depth;
          int r = ny_native_nir_lower_expr(b, global);
          --b->resolve_depth;
          return r;
        }
      }
    }
    if (!l) {
      const char *platform =
          ny_native_nir_platform_string(b->options, e->as.ident.name);
      if (platform) {
        const char *sym =
            ny_native_strtab_intern(platform, strlen(platform), NULL, 0);
        int addr =
            sym ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                    .dst = -1,
                                                    .a = -1,
                                                    .b = -1,
                                                    .imm = 0,
                                                    .symbol = sym})
                : -1;
        int length = ny_native_nir_emit_const(b, (int64_t)strlen(platform));
        int tag = ny_native_nir_emit_const(b, 121);
        if (addr < 0 || length < 0 || tag < 0)
          return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
        if (!ny_native_nir_record_dyn_fact(
                b, addr, NY_NATIVE_NIR_FACT_DYN_STR_LEN, length) ||
            !ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_TAG,
                                           tag))
          return -1;
        return addr;
      }
    }
    if (!l) {
      if (b->module_name && b->module_name[0]) {
        char qualified[512];
        int qn = snprintf(qualified, sizeof(qualified), "%s.%s",
                         b->module_name, e->as.ident.name);
        int64_t qualified_constant = 0;
        if (qn > 0 && (size_t)qn < sizeof(qualified) &&
            ny_native_consttab_get(qualified, &qualified_constant)) {
          return ny_native_nir_emit_const(b, qualified_constant);
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
        return ny_native_nir_emit_const(b, imported_constant);
      if (ny_native_consttab_get_tail(e->as.ident.name, &imported_constant)) {
        return ny_native_nir_emit_const(b, imported_constant);
      }
      const stmt_t *adt_enum = NULL;
      const stmt_enum_item_t *adt_item = NULL;
      int64_t adt_tag = 0;
      if (ny_native_nir_find_enum_member(b, e->as.ident.name, &adt_enum,
                                         &adt_item, &adt_tag)) {
        if (adt_item && adt_item->fields.len == 0)
          return ny_native_nir_emit_const(b, adt_tag);
      }
    }
    /* The tuple type can occur as value-level type evidence in generated
     * stdlib code.  It is a runtime tag, not a linkable global. */
    if (!l && e->as.ident.name && strcmp(e->as.ident.name, "tuple") == 0)
      return ny_native_nir_emit_const(
          b,
          rt_runtime_tag_raw_name(e->as.ident.name, strlen(e->as.ident.name)));
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
      const char *sym = ny_native_strtab_intern(text, n, NULL, 0);
      int addr = sym ? nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                         .dst = -1,
                                                         .a = -1,
                                                         .b = -1,
                                                         .symbol = sym})
                     : -1;
      return addr < 0 ? -1 : addr;
    }
    if (!l) {
      char function_symbol[512];
      const stmt_t *referenced_fn =
          ny_native_nir_find_user_function(b, e->as.ident.name);
      if (!referenced_fn)
        referenced_fn =
            ny_native_nir_find_imported_function(b, e->as.ident.name);
      const char *leaf = ny_native_leaf_name(e->as.ident.name);
      if (leaf && strcmp(leaf, "zalloc") == 0) {
        int addr =
            nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .imm = 0,
                                              .symbol = "rt_zalloc_raw"});
        if (addr < 0)
          ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return addr;
      }
      bool compiled_fn =
          referenced_fn && !referenced_fn->as.fn.is_extern &&
          !referenced_fn->as.fn.link_name &&
          ny_native_runtime_symbol_for_expr(e->as.ident.name, leaf, e) == NULL &&
          (!leaf || ny_native_runtime_symbol_for_expr(NULL, leaf, e) == NULL) &&
          (!leaf || ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_NONE) &&
          (!leaf || ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE);
      bool user_function =
          ny_native_nir_user_defined_fn(b, e->as.ident.name) || compiled_fn;
      /* std.math exports these immutable scalar constants.  When a root
       * program imports the module with `use std.math`, the expanded module
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
          return ny_native_nir_emit_const_f64(b, constant);
      }
      if (!referenced_fn && !user_function &&
          ny_native_runtime_symbol_for_expr(e->as.ident.name, leaf, e) == NULL &&
          (!leaf || ny_native_runtime_symbol_for_expr(NULL, leaf, e) == NULL) &&
          (!leaf || ny_native_leaf_kind(leaf) == NY_NATIVE_LEAF_NONE) &&
          (!leaf || ny_builtin_alloc_kind(leaf) == NY_BUILTIN_ALLOC_NONE)) {
        ny_native_nir_fail(b, "undefined symbol '%s'", e->as.ident.name);
        return -1;
      }
      const char *symbol = e->as.ident.name;
      if (compiled_fn && referenced_fn->as.fn.name)
        symbol = referenced_fn->as.fn.name;
      if (e->semantic.canonical_callee &&
          strncmp(e->semantic.canonical_callee, "std.core.", 9) == 0)
        symbol = e->semantic.canonical_callee;
      if (user_function) {
        int n = snprintf(function_symbol, sizeof(function_symbol), "ny_fn_%s",
                         symbol);
        if (n < 0 || (size_t)n >= sizeof(function_symbol)) {
          ny_native_nir_fail(b, "native NYIR lower: function symbol too long");
          return -1;
        }
        symbol = function_symbol;
      }
      int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = symbol});
      if (addr < 0) {
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      /*
       * A top-level def constant registered in the consttab is stored as an
       * 8-byte .data definition: a value-context reference loads it rather
       * than returning the symbol's address (R3).
       */
      if (ny_native_consttab_has(e->as.ident.name))
        return ny_native_nir_emit_load_i64(b, addr);
      return addr;
    }
    /*
     * Any ordinary read can create or observe an alias; only the exact
     * loop-local promotion path below may consume a candidate without this.
     */
    if (!l) {
      ny_native_nir_fail(b, "undefined symbol '%s'", e->as.ident.name);
      return -1;
    }
    l->sb_candidate = false;
    int val = ny_native_nir_load_local_value(b, l->slot);
    if (val >= 0) {
      if (l->list_len_slot >= 0) {
        int llen = ny_native_nir_load_local_value(b, l->list_len_slot);
        if (llen >= 0)
          ny_native_nir_record_list_len_fact(b, val, llen);
      }
      if (l->dyn_str_len_slot >= 0) {
        int slen = ny_native_nir_load_local_value(b, l->dyn_str_len_slot);
        if (slen >= 0)
          ny_native_nir_record_dyn_fact(b, val, NY_NATIVE_NIR_FACT_DYN_STR_LEN,
                                        slen);
      }
      if (l->dyn_tag_slot >= 0) {
        int stag = ny_native_nir_load_local_value(b, l->dyn_tag_slot);
        if (stag >= 0)
          ny_native_nir_record_dyn_fact(b, val, NY_NATIVE_NIR_FACT_DYN_TAG,
                                        stag);
      }
    }
    return val;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right) {
      ny_native_nir_fail(b, "native NYIR lower: malformed unary");
      return -1;
    }
    /* `async f(a, b)` is a task constructor, not an ordinary unary
     * operation.  Keep the callback address and its raw native arguments in
     * a heap vector: the scheduler copies that vector before the lowering
     * frame can disappear.  The high bit on argc selects the raw callback
     * dispatcher (native function addresses must never go through the tagged
     * callable dispatcher). */
    if (strcmp(e->as.unary.op, "async") == 0) {
      const expr_t *task = e->as.unary.right;
      const expr_t *callee = task;
      const ny_call_arg_list *call_args = NULL;
      size_t argc = 0;
      if (task->kind == NY_E_CALL) {
        callee = task->as.call.callee;
        call_args = &task->as.call.args;
        argc = call_args->len;
      } else if (task->kind == NY_E_MEMCALL) {
        ny_native_nir_fail(b, "native NYIR lower: async member calls should be "
                              "wrapped in a function or lambda");
        return -1;
      }
      if (!callee || argc > 15) {
        ny_native_nir_fail(b, "native NYIR lower: async requires a callable "
                              "with at most 15 arguments");
        return -1;
      }
      int fn = ny_native_nir_lower_expr(b, callee);
      if (fn < 0)
        return -1;
      int argv = ny_native_nir_emit_const(b, 0);
      if (argc > 0) {
        int bytes =
            ny_native_nir_emit_const(b, (int64_t)(argc * sizeof(int64_t)));
        argv = bytes < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b, "rt_zalloc_raw", bytes, -1, -1, 1, 0);
        if (argv < 0)
          return -1;
        for (size_t i = 0; i < argc; ++i) {
          if (call_args->data[i].name) {
            ny_native_nir_fail(b, "native NYIR lower: async call syntax does "
                                  "not support named arguments");
            return -1;
          }
          int arg = ny_native_nir_lower_expr(b, call_args->data[i].val);
          int index = ny_native_nir_emit_const(b, (int64_t)i);
          if (arg < 0 || index < 0 ||
              ny_native_nir_emit_runtime_call(b, "rt_store64_idx", argv, index,
                                              arg, 3, 0) < 0)
            return -1;
        }
      }
      /* The scheduler calls native code directly, but its result may already
       * be in the tagged any ABI.  Carry the declared return representation
       * separately from the raw-code-pointer marker. */
      const stmt_t *callback = NULL;
      if (callee->kind == NY_E_FN || callee->kind == NY_E_LAMBDA) {
        ny_native_lambda_entry_t *entry = ny_native_lambda_find(callee);
        callback = entry ? entry->fn : NULL;
      } else if (callee->kind == NY_E_IDENT && callee->as.ident.name) {
        callback = ny_native_nir_find_user_function(b, callee->as.ident.name);
      }
      bool tagged_result = callback && callback->kind == NY_S_FUNC &&
          (ny_native_type_name_is_any(callback->as.fn.return_type) ||
           (!callback->as.fn.return_type &&
            callback->as.fn.return_semantic.resolved &&
            callback->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC));
      uint64_t argc_abi = UINT64_C(1) << 63;
      if (tagged_result)
        argc_abi |= UINT64_C(1) << 62;
      argc_abi |= ((uint64_t)argc << 1) | 1;
      int argc_value = ny_native_nir_emit_const(b, (int64_t)argc_abi);
      if (argc_value < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_async_task_new", fn,
                                             argc_value, argv, 3, 0);
    }
    int rv = ny_native_nir_lower_expr(b, e->as.unary.right);
    if (rv < 0)
      return -1;
    if (strcmp(e->as.unary.op, "await") == 0)
      return ny_native_nir_emit_runtime_call(b, "rt_async_await_blocking", rv,
                                             -1, -1, 1, 0);
    if (strcmp(e->as.unary.op, "+") == 0)
      return rv;
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (ny_native_nir_expr_is_bigint(b, e->as.unary.right))
        return ny_native_nir_emit_runtime_call(b, "rt_bigint_neg_raw", rv,
                                               -1, -1, 1, 0);
      /* Keep values in the native scalar domain only while they fit the
       * language's signed small-int range. */
      int64_t known = 0;
      if (ny_native_nir_eval_proof_i64(b, e->as.unary.right, 0, &known) &&
          known != INT64_MIN && known <= (INT64_C(1) << 62) - 1 &&
          known >= -(INT64_C(1) << 62) + 1)
        return ny_native_nir_emit_const(b, -known);
      if (ny_native_nir_expr_is_f32(b, e->as.unary.right)) {
        int zero = ny_native_nir_emit_const_f32(b, 0.0f);
        if (zero < 0)
          return -1;
        return nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SUB_F32, .dst = -1, .a = zero, .b = rv});
      }
      if (ny_native_nir_expr_is_f64(b, e->as.unary.right)) {
        int zero = ny_native_nir_emit_const_f64(b, 0.0);
        if (zero < 0)
          return -1;
        return nyir_emit(
            &b->nyir,
            (nyir_inst_t){.op = NYIR_SUB_F64, .dst = -1, .a = zero, .b = rv});
      }
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_SUB_I64, .dst = -1, .a = zero, .b = rv});
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      if (ny_native_nir_expr_is_f64(b, e->as.unary.right)) {
        int zero = ny_native_nir_emit_const_f64(b, 0.0);
        if (zero < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_F64,
                                                 .dst = -1,
                                                 .a = rv,
                                                 .b = zero,
                                                 .cmp = NYIR_CMP_EQ});
      }
      if (e->as.unary.right->kind == NY_E_IDENT &&
          e->as.unary.right->as.ident.name) {
        const ny_native_nir_local_t *local =
            ny_native_nir_find_local(b, e->as.unary.right->as.ident.name);
        if (local && local->is_any) {
          rv = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", rv,
                                               -1, -1, 1, 0);
          if (rv < 0)
            return -1;
        }
      }
      /* Native predicates return boxed NyValue booleans (NY_IMM_FALSE=2), while
       * comparisons and raw integer expressions use 0/1 or untagged ints.
       * `!` is a truthiness operation: 0 represents nil/false/int 0, and 2
       * represents boxed false. Raw 1 represents true/int 1 and must never be
       * treated as falsy. */
      int zero = ny_native_nir_emit_const(b, 0);
      int false_imm = ny_native_nir_emit_const(b, NY_IMM_FALSE);
      if (zero < 0 || false_imm < 0)
        return -1;
      int is_nil = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, rv, zero);
      int is_false = ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, rv, false_imm);
      if (is_nil < 0 || is_false < 0)
        return -1;
      return ny_native_nir_emit_binop(b, NYIR_OR_I64, is_nil, is_false);
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      int mask = ny_native_nir_emit_const(b, -1);
      if (mask < 0)
        return -1;
      return nyir_emit(
          &b->nyir,
          (nyir_inst_t){.op = NYIR_XOR_I64, .dst = -1, .a = rv, .b = mask});
    }
    ny_native_nir_fail(b, "native NYIR lower: unsupported unary operator '%s'",
                       e->as.unary.op);
    return -1;
  }
  case NY_E_BINARY:
    return ny_native_nir_lower_binary(b, e);
  case NY_E_LOGICAL: {
    if (!e->as.logical.op || (strcmp(e->as.logical.op, "&&") != 0 &&
                              strcmp(e->as.logical.op, "||") != 0)) {
      ny_native_nir_fail(b,
                         "native NYIR lower: unsupported logical operator '%s'",
                         e->as.logical.op ? e->as.logical.op : "(null)");
      return -1;
    }
    return ny_native_nir_lower_logical(b, e->as.logical.left,
                                       e->as.logical.right,
                                       strcmp(e->as.logical.op, "||") == 0);
  }
  case NY_E_TERNARY:
    return ny_native_nir_lower_ternary(b, e->as.ternary.cond,
                                       e->as.ternary.true_expr,
                                       e->as.ternary.false_expr);
  case NY_E_COMPTIME: {
    const stmt_t *body = e->as.comptime_expr.body;
    if (body) {
      if (body->kind == NY_S_BLOCK) {
        int comptime_last_value = -1;
        for (size_t i = 0; i < body->as.block.body.len; ++i) {
          const stmt_t *item = body->as.block.body.data[i];
          if (!item)
            continue;
          if (item->kind == NY_S_RETURN && item->as.ret.value)
            return ny_native_nir_lower_expr(b, item->as.ret.value);
          if (item->kind == NY_S_EXPR && item->as.expr.expr) {
            int v = ny_native_nir_lower_expr(b, item->as.expr.expr);
            if (v >= 0)
              comptime_last_value = v;
          } else {
            if (!ny_native_nir_lower_stmt(b, item))
              return -1;
          }
        }
        if (comptime_last_value >= 0)
          return comptime_last_value;
        return ny_native_nir_emit_const(b, 0);
      }
      if (body->kind == NY_S_RETURN && body->as.ret.value)
        return ny_native_nir_lower_expr(b, body->as.ret.value);
      if (body->kind == NY_S_EXPR && body->as.expr.expr)
        return ny_native_nir_lower_expr(b, body->as.expr.expr);
      if (!ny_native_nir_lower_stmt(b, body))
        return -1;
      return b->last_value >= 0 ? b->last_value
                                : ny_native_nir_emit_const(b, 0);
    }
    bool value = false;
    if (ny_native_target_eval_bool(NULL, e, &value))
      return ny_native_nir_emit_const(b, value ? 1 : 0);
    return ny_native_nir_emit_const(b, 0);
  }
  case NY_E_QUOTE: {
    uint32_t prev_ctx = b->current_syntax_ctx;
    if (e->as.quote.syntax_ctx != 0)
      b->current_syntax_ctx = e->as.quote.syntax_ctx;
    int res = -1;
    if (e->as.quote.expr) {
      res = ny_native_nir_lower_expr(b, e->as.quote.expr);
    } else if (e->as.quote.body) {
      const stmt_t *body = e->as.quote.body;
      if (body->kind == NY_S_BLOCK) {
        int last_v = -1;
        for (size_t i = 0; i < body->as.block.body.len; ++i) {
          const stmt_t *item = body->as.block.body.data[i];
          if (!item)
            continue;
          if (item->kind == NY_S_RETURN && item->as.ret.value) {
            res = ny_native_nir_lower_expr(b, item->as.ret.value);
            break;
          }
          if (item->kind == NY_S_EXPR && item->as.expr.expr) {
            int v = ny_native_nir_lower_expr(b, item->as.expr.expr);
            if (v >= 0)
              last_v = v;
          } else {
            if (!ny_native_nir_lower_stmt(b, item)) {
              res = -1;
              break;
            }
          }
        }
        if (res < 0)
          res = last_v >= 0 ? last_v : ny_native_nir_emit_const(b, 0);
      } else if (body->kind == NY_S_RETURN && body->as.ret.value) {
        res = ny_native_nir_lower_expr(b, body->as.ret.value);
      } else if (body->kind == NY_S_EXPR && body->as.expr.expr) {
        res = ny_native_nir_lower_expr(b, body->as.expr.expr);
      } else {
        if (!ny_native_nir_lower_stmt(b, body))
          res = -1;
        else
          res = b->last_value >= 0 ? b->last_value : ny_native_nir_emit_const(b, 0);
      }
    } else {
      res = ny_native_nir_emit_const(b, 0);
    }
    b->current_syntax_ctx = prev_ctx;
    return res;
  }
  case NY_E_SPLICE: {
    return e->as.splice.expr ? ny_native_nir_lower_expr(b, e->as.splice.expr)
                             : ny_native_nir_emit_const(b, 0);
  }
  case NY_E_MATCH: {
    stmt_t match = {.kind = NY_S_MATCH, .tok = e->tok};
    match.as.match = e->as.match;
    bool saved_match_any = b->match_result_any;
    b->match_result_any = ny_native_nir_expr_is_any(b, e);
    if (!ny_native_nir_lower_match(b, &match))
      return -1;
    b->match_result_any = saved_match_any;
    /* Match arms already lower through the common value ABI.  Re-tagging the
     * joined result here corrupts pointer/string arms by treating their
     * address as an integer; dynamic consumers decode the arm representation
     * at their own boundary instead. */
    return b->last_value;
  }
  case NY_E_CALL:
    return ny_native_nir_lower_call(b, e);
  case NY_E_SIZEOF: {
    const char *type_name = e->as.szof.is_type ? e->as.szof.type_name : NULL;
    if (!type_name && e->as.szof.target &&
        e->as.szof.target->kind == NY_E_IDENT)
      type_name = e->as.szof.target->as.ident.name;
    size_t size = 0;
    size_t align = 0;
    bool resolved =
        type_name && ny_native_nir_primitive_layout(type_name, &size, &align);
    if (!resolved)
      resolved = type_name && ny_native_nir_ast_layout_query(
                                  b, type_name, NULL, &size, &align, NULL);
    if (!resolved) {
      ny_native_nir_fail(b,
                         "native NYIR lower: sizeof cannot resolve type '%s'",
                         type_name ? type_name : "");
      return -1;
    }
    (void)align;
    return ny_native_nir_emit_const(b, (int64_t)size);
  }
  case NY_E_DEREF: {
    int addr = ny_native_nir_lower_expr(b, e->as.deref.target);
    if (addr < 0)
      return -1;
    int v = nyir_emit(
        &b->nyir,
        (nyir_inst_t){
            .op = NYIR_LOAD_I64, .dst = -1, .a = addr, .b = -1, .c = -1});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: deref load failed");
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
        return ny_native_nir_emit_const(b, 0);
      if (strstr(tpl, "lea") && strstr(tpl, "ret") && b->local_count >= 2) {
        int left = ny_native_nir_load_local_value(b, b->locals[0].slot);
        int right = left < 0
                        ? -1
                        : ny_native_nir_load_local_value(b, b->locals[1].slot);
        return left < 0 || right < 0
                   ? -1
                   : ny_native_asm_emit_binop(b, NYIR_ADD_I64, left, right);
      }
    }
    if (e->as.as_asm.args.len == 1 && strcmp(cons, "=r,r") == 0 &&
        (strcmp(tpl, "mov $1, $0") == 0 || strcmp(tpl, "mov %1, %0") == 0 ||
         strcmp(tpl, "movq $1, $0") == 0 || strcmp(tpl, "movl $1, $0") == 0 ||
         strcmp(tpl, "mov $1,$0") == 0 || strcmp(tpl, "mov %1,%0") == 0))
      return ny_native_nir_lower_expr(b, e->as.as_asm.args.data[0]);
    if (e->as.as_asm.args.len == 2 &&
        (strcmp(tpl, "addq $1, $0") == 0 || strcmp(tpl, "add %1, %0") == 0 ||
         strcmp(tpl, "or $0, $1, $2") == 0)) {
      int a = ny_native_nir_lower_expr(b, e->as.as_asm.args.data[0]);
      int c =
          a < 0 ? -1 : ny_native_nir_lower_expr(b, e->as.as_asm.args.data[1]);
      if (a < 0 || c < 0)
        return -1;
      return ny_native_asm_emit_binop(
          b, strncmp(tpl, "or ", 3) == 0 ? NYIR_OR_I64 : NYIR_ADD_I64, a, c);
    }
    if (e->as.as_asm.args.len == 2 && strstr(tpl, "lea") && strstr(tpl, "$1") &&
        strstr(tpl, "$2")) {
      int a = ny_native_nir_lower_expr(b, e->as.as_asm.args.data[0]);
      int c =
          a < 0 ? -1 : ny_native_nir_lower_expr(b, e->as.as_asm.args.data[1]);
      return a < 0 || c < 0 ? -1
                            : ny_native_asm_emit_binop(b, NYIR_ADD_I64, a, c);
    }
    return ny_native_nir_lower_aarch64_asm(b, e);
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
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        return -1;
      }
      size_t off = 0;
      for (size_t i = 0; i < parts->len; ++i) {
        const char *data =
            parts->data[i].as.s.data ? parts->data[i].as.s.data : "";
        memcpy(joined + off, data, parts->data[i].as.s.len);
        off += parts->data[i].as.s.len;
      }
      joined[off] = '\0';
      const char *sym = ny_native_strtab_intern(joined, off, NULL, 0);
      free(joined);
      if (!sym) {
        ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
        return -1;
      }
      int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                   .dst = -1,
                                                   .a = -1,
                                                   .b = -1,
                                                   .imm = 0,
                                                   .symbol = sym});
      if (addr < 0)
        ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return addr;
    }
    int acc = -1;
    for (size_t i = 0; i < parts->len; ++i) {
      int part_val = -1;
      if (parts->data[i].kind == NY_FSP_STR) {
        const char *s =
            parts->data[i].as.s.data ? parts->data[i].as.s.data : "";
        const char *sym =
            ny_native_strtab_intern(s, parts->data[i].as.s.len, NULL, 0);
        if (!sym) {
          ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
          return -1;
        }
        part_val = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                     .dst = -1,
                                                     .a = -1,
                                                     .b = -1,
                                                     .imm = 0,
                                                     .symbol = sym});
      } else {
        const expr_t *sub = parts->data[i].as.e;
        part_val = ny_native_nir_lower_expr(b, sub);
        if (part_val < 0)
          return -1;
        if (ny_native_nir_expr_is_cstr(b, sub)) {
          /*
           * already a runtime C string — use directly
           */
        } else if (ny_native_nir_expr_is_any(b, sub)) {
          part_val = ny_native_nir_emit_runtime_call(b, "rt_any_to_cstr",
                                                     part_val, -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else if (ny_native_nir_expr_is_bool(b, sub)) {
          part_val = ny_native_nir_emit_runtime_call(
              b, "rt_bool_to_cstr", part_val, -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else {
          /*
           * Raw i64 (matches print's is_cstr ? cstr : i64 convention).
           */
          part_val = ny_native_nir_emit_runtime_call(b, "rt_i64_to_cstr_raw",
                                                     part_val, -1, -1, 1, 0);
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
      acc = ny_native_nir_emit_runtime_call(b, "rt_cstr_concat", acc,
                                            part_val, -1, 2, 0);
      if (acc < 0)
        return -1;
    }
    return acc;
  }
  case NY_E_DICT: {
    size_t count = e->as.dict.pairs.len;
    int capacity =
        ny_native_nir_emit_const(b, (int64_t)(count < 4 ? 8 : count * 2));
    if (capacity < 0)
      return -1;
    int dict = ny_native_nir_emit_runtime_call(b, "rt_dict_new_raw",
                                               capacity, -1, -1, 1, 0);
    if (dict < 0)
      return -1;
    int dict_slot = b->next_local_slot++;
    if (!ny_native_nir_store_local_value(b, dict_slot, dict))
      return -1;
    for (size_t i = 0; i < count; ++i) {
      const expr_t *key = e->as.dict.pairs.data[i].key;
      const expr_t *value = e->as.dict.pairs.data[i].value;
      int key_reg = ny_native_nir_lower_dict_key(b, key);
      int value_reg = ny_native_nir_lower_expr(b, value);
      if (key_reg < 0 || value_reg < 0)
        return -1;
      /* Dict slots are dynamic `any` values.  Native boolean expressions use
       * raw 0/1 for branch arithmetic, but stored dynamic booleans use the
       * canonical Ny immediates. */
      if (value->kind == NY_E_LITERAL &&
          value->as.literal.kind == NY_LIT_BOOL) {
        value_reg = ny_native_nir_emit_const(
            b, value->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE);
        if (value_reg < 0)
          return -1;
      }
      /* Dictionary slots use the public dynamic-value ABI.  Keep literal
       * construction consistent with indexed/set calls by boxing typed f64
       * values before handing them to the integer-shaped runtime bridge. */
      if (ny_native_nir_expr_is_f64(b, value)) {
        int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value_reg,
                                                   -1, -1, 1, 0);
        value_reg = bits < 0
                        ? -1
                        : ny_native_nir_emit_runtime_call(
                              b, "rt_flt_box_val", bits, -1, -1, 1, 0);
        if (value_reg < 0)
          return -1;
      }
      /* Dynamic integer parameters arrive in the legacy tagged ABI, while
       * native dictionary payloads are raw scalar slots.  Normalize only
       * values proven dynamic; raw integer literals must remain untouched
       * because odd raw values are valid native integers too. */
      bool is_bool_val = ny_native_nir_expr_is_bool(b, value);
      bool integer_literal = value && value->kind == NY_E_LITERAL &&
                             value->as.literal.kind == NY_LIT_INT &&
                             value->tok.kind != NY_T_NIL;
      bool raw_integer = integer_literal ||
                         (!is_bool_val && value && value->semantic.resolved &&
                          value->semantic.rep == NY_SEM_REP_RAW_INT);
      if (value->kind == NY_E_IDENT && value->as.ident.name) {
        const ny_native_nir_local_t *value_local =
            ny_native_nir_find_local(b, value->as.ident.name);
        if (value_local && value_local->is_bool)
          is_bool_val = true;
        raw_integer = raw_integer ||
                      (!is_bool_val && value_local &&
                       value_local->semantic_rep == NY_SEM_REP_RAW_INT);
      }
      /* Dictionary values are public `any` values.  Box every value proven
       * raw-integer before storing it, including odd integers from typed
       * locals.  Leaving those raw makes a later dynamic read indistinguish-
       * able from a tagged integer and corrupts equality/callback results. */
      if (is_bool_val) {
        value_reg = ny_native_nir_box_bool(b, value_reg);
        if (value_reg < 0)
          return -1;
      } else if (raw_integer && !ny_native_nir_expr_is_cstr(b, value)) {
        value_reg = ny_native_nir_emit_runtime_call(b, "rt_tag", value_reg, -1,
                                                    -1, 1, 0);
        if (value_reg < 0)
          return -1;
      }
      /* A proven-dynamic value already carries the canonical dynamic word
       * and is stored as-is.  Unboxing it through rt_any_to_i64 turned int 0
       * into a raw 0 payload, which the getter must read back as nil (raw 0
       * is reserved), so {"a": any_param} returned nil for the int 0 case. */
      int saved_dict = ny_native_nir_load_local_value(b, dict_slot);
      bool key_is_string =
          ny_native_nir_expr_is_cstr(b, key) ||
          (key->kind == NY_E_LITERAL && key->as.literal.kind == NY_LIT_STR);
      int set_result = saved_dict < 0
                           ? -1
                           : ny_native_nir_emit_runtime_call(
                                 b,
                                 key_is_string ? "rt_native_dict_set_str_compact"
                                               : "rt_native_dict_set_nir_i64",
                                 saved_dict, key_reg, value_reg, 3, 0);
      if (set_result < 0)
        return -1;
      /* Setters mutate the dictionary in place.  Keep the allocator result
       * as the expression value; relying on a setter's return register made
       * optimized machine calls lose the live dictionary pointer. */
    }
    return ny_native_nir_load_local_value(b, dict_slot);
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
      ny_native_nir_fail(
          b,
          "native NYIR lower: list literal must have at most 65536 elements");
      return -1;
    }
    if (count == 0) {
      int n = ny_native_nir_emit_const(b, 0);
      int width = ny_native_nir_emit_const(
          b, b->current_list_elem_size > 0
                 ? b->current_list_elem_size
                 : (b->force_dynamic_list ? 24 : 8));
      int base = n < 0 || width < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_tbuf_new_raw",
                                                       n, width, -1, 2, 0);
      int length = n < 0 ? -1 : ny_native_nir_emit_const(b, 0);
      if (base < 0 || length < 0 ||
          !ny_native_nir_record_list_len_fact(b, base, length))
        return -1;
      return base;
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
        if (!ny_native_nir_expr_is_f64(b, element)) {
          all_f64 = false;
        }
        /* A list whose elements are proven raw scalar integers needs neither
         * descriptor slots nor per-element runtime tag discovery.  Besides
         * avoiding needless metadata, this preserves scalar-replacement for
         * transient literals such as `[i]` in hot loops. */
        if (ny_native_nir_expr_is_any(b, element) ||
            ny_native_nir_expr_is_cstr(b, element) ||
            ny_native_nir_expr_is_dict(b, element) ||
            ny_native_nir_expr_is_list(b, element) ||
            ny_native_nir_expr_is_f64(b, element) ||
            ny_native_nir_expr_is_f32(b, element))
          all_raw_i64 = false;
      }
      if (all_f64) {
        int n = ny_native_nir_emit_const(b, (int64_t)count);
        /* A float list crosses `any` boundaries (camera vectors, nested
         * lists, callbacks), so retain an explicit descriptor tag instead of
         * exposing IEEE bits as an integer elem-8 payload. */
        int width = ny_native_nir_emit_const(b, 24);
        int base = n < 0 || width < 0
                       ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_tbuf_new_raw", n, width, -1, 2, 0);
        if (base < 0)
          return -1;
        for (size_t i = 0; i < count; ++i) {
          int value = ny_native_nir_lower_expr(b, e->as.list_like.data[i]);
          int off = ny_native_nir_emit_const(b, (int64_t)(i * 24));
          int slot = value < 0 || off < 0
                         ? -1
                         : ny_native_nir_emit_add_i64(b, base, off);
          int tag = slot < 0 ? -1 : ny_native_nir_emit_const(b, TAG_FLOAT);
          int tag_addr = slot < 0 || tag < 0
                             ? -1
                             : ny_native_nir_emit_add_i64(
                                   b, slot, ny_native_nir_emit_const(b, 16));
          if (slot < 0 || tag_addr < 0 ||
              !ny_native_nir_emit_store_f64(b, slot, value) ||
              !ny_native_nir_emit_store_i64(b, tag_addr, tag))
            return -1;
        }
        int list_len = ny_native_nir_emit_const(b, (int64_t)count);
        if (list_len < 0 ||
            !ny_native_nir_record_list_len_fact(b, base, list_len))
          return -1;
        if (count <= (size_t)(INT64_MAX / 24))
          ny_native_nir_record_alloc_fact(b, base, (int64_t)(count * 24));
        return base;
      }
      /* A non-literal scalar may be raw zero.  Keep such transient lists in
       * descriptor storage so their dynamic consumer can retain integer-vs-
       * nil provenance. */
      for (size_t i = 0; i < count; ++i) {
        const expr_t *el = e->as.list_like.data[i];
        if (!el || el->kind != NY_E_LITERAL) {
          all_raw_i64 = false;
          break;
        }
      }
      if (all_raw_i64) {
        int n = ny_native_nir_emit_const(b, (int64_t)count);
        int width = ny_native_nir_emit_const(b, 8);
        int base = n < 0 || width < 0
                       ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_tbuf_new_raw", n, width, -1, 2, 0);
        if (base < 0)
          return -1;
        for (size_t i = 0; i < count; ++i) {
          int value = ny_native_nir_lower_expr(b, e->as.list_like.data[i]);
          int off = ny_native_nir_emit_const(b, (int64_t)(i * 8));
          int slot = value < 0 || off < 0
                         ? -1
                         : ny_native_nir_emit_add_i64(b, base, off);
          if (slot < 0 || !ny_native_nir_emit_store_i64(b, slot, value))
            return -1;
        }
        int list_len = ny_native_nir_emit_const(b, (int64_t)count);
        if (list_len < 0 ||
            !ny_native_nir_record_list_len_fact(b, base, list_len))
          return -1;
        if (count <= (size_t)(INT64_MAX / 8))
          ny_native_nir_record_alloc_fact(b, base, (int64_t)(count * 8));
        return base;
      }
      int n = ny_native_nir_emit_const(b, (int64_t)count);
      int width = ny_native_nir_emit_const(b, 24);
      int base = n < 0 || width < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(b, "rt_tbuf_new_raw",
                                                       n, width, -1, 2, 0);
      if (base < 0)
        return -1;
      for (size_t i = 0; i < count; ++i) {
        const expr_t *el = e->as.list_like.data[i];
        bool saved_force_dynamic = b->force_dynamic_list;
        if (el->kind == NY_E_LIST || el->kind == NY_E_TUPLE)
          b->force_dynamic_list = true;
        int value = ny_native_nir_lower_expr(b, el);
        b->force_dynamic_list = saved_force_dynamic;
        int index = ny_native_nir_emit_const(b, (int64_t)i);
        int stride = ny_native_nir_emit_const(b, 24);
        int off = value < 0 || index < 0 || stride < 0
                      ? -1
                      : ny_native_nir_push_val(b, NYIR_MUL_I64, index, stride,
                                               0, NULL);
        int slot = off < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, off);
        if (slot < 0)
          return -1;
        bool is_string = ny_native_nir_expr_is_cstr(b, el);
        bool is_dict = ny_native_nir_expr_is_dict(b, el);
        bool is_list_elem = ny_native_nir_expr_is_list(b, el);
        bool is_f64 = ny_native_nir_expr_is_f64(b, el);
        /* Native integer expressions such as from_int() already produce a
         * raw payload.  Do not classify an odd payload with rt_value_tag:
         * the descriptor reader would then untag it a second time. */
        bool raw_integer_expr = el->semantic.resolved &&
                                el->semantic.rep == NY_SEM_REP_RAW_INT;
        if (el->kind == NY_E_IDENT && el->as.ident.name) {
          const ny_native_nir_local_t *element_local =
              ny_native_nir_find_local(b, el->as.ident.name);
          raw_integer_expr =
              raw_integer_expr ||
              (element_local &&
               element_local->semantic_rep == NY_SEM_REP_RAW_INT);
        }
        /* A pooled C-string literal stored raw is indistinguishable from any
         * other .rodata pointer once it leaves the descriptor (is_str probes
         * the bytes before the pointer for a TAG header).  Materialize a
         * managed handle at construction so downstream is_str/iteration see
         * a real string ([(1, 2), range(2), "ab"] flattened lost the split). */
        if (is_string && el->kind == NY_E_LITERAL) {
          value = ny_native_nir_emit_runtime_call(b, "rt_cstr_to_str", value,
                                                  -1, -1, 1, 0);
          if (value < 0)
            return -1;
        }
        /* Inferred `any` parameters arrive as value/length/tag triples and
         * their value slot is normally the tagged VM representation.  Native
         * descriptor lists store raw payloads, so unbox dynamic integer-like
         * elements before writing slot 0.  Keep strings and float values on
         * their dedicated paths. */
        bool dynamic_value = ny_native_nir_expr_is_any(b, el);
        if (el->kind == NY_E_IDENT && el->as.ident.name) {
          const ny_native_nir_local_t *value_local =
              ny_native_nir_find_local(b, el->as.ident.name);
          dynamic_value = dynamic_value || (value_local && value_local->is_any);
        }
        bool unboxed_dynamic = false;
        if (dynamic_value && !is_string && !is_f64) {
          value = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64",
                                                  value, -1, -1, 1, 0);
          unboxed_dynamic = value >= 0;
          if (value < 0)
            return -1;
        }
        int tag = -1;
        if (unboxed_dynamic) {
          /*
           * The payload was unboxed to a raw machine word, so its slot tag
           * must classify the raw word at runtime — the static
           * expression-level classification can be wrong here (an untyped
           * parameter inferred as a list tagged a scalar 1 as TAG_LIST, so
           * mapcat's fn1(1) dispatched the raw word and println read 0).
           * rt_raw_word_tag still returns the container tag for genuine
           * handles, which rt_any_to_i64 passes through unchanged.
           */
          tag = ny_native_nir_emit_runtime_call(b, "rt_raw_word_tag", value,
                                                -1, -1, 1, 0);
        } else if (is_string)
          tag = ny_native_nir_emit_const(b, 121);
        else if (is_dict)
          tag = ny_native_nir_emit_const(b, 101);
        else if (is_list_elem || el->kind == NY_E_LIST ||
                 el->kind == NY_E_TUPLE)
          tag = ny_native_nir_emit_const(b, 100);
        else if (raw_integer_expr && !is_string && !is_f64 &&
                 !dynamic_value)
          tag = ny_native_nir_emit_const(b, 3);
        else if (count == 1 && !is_string && !is_f64 && !dynamic_value &&
                 el->kind == NY_E_IDENT) {
          /* A statically scalar local is already a raw integer payload.  The
           * generic tag bridge performs a heap/tag probe on every iteration;
           * descriptor reads return slot 0 unchanged for integer elements, so
           * the fixed integer tag is both sufficient and substantially cheaper. */
          tag = ny_native_nir_emit_const(b, 3);
        }
        if (tag < 0 && el->kind == NY_E_LITERAL &&
            el->as.literal.kind == NY_LIT_INT && el->tok.kind != NY_T_NIL) {
          /*
           * A literal slot payload is the raw machine word by construction.
           * rt_value_tag classifies odd words as tagged-int (tag 1), which
           * tells raw consumers to untag — [ptr, 17] then handed the int
           * param 8 (untag 17) instead of 17 at thread-arg reads.
           */
          tag = ny_native_nir_emit_const(b, 3);
        }
        if (tag < 0)
          tag = ny_native_nir_emit_runtime_call(b,
                                                (!is_string && !is_dict &&
                                                 !is_list_elem && !is_f64 &&
                                                 el->kind != NY_E_LITERAL)
                                                    ? "rt_tag_or_raw_int"
                                                    : "rt_value_tag",
                                                value, -1, -1, 1, 0);
        int length = is_string ? ny_native_nir_peek_dyn_fact(
                                     b, value, NY_NATIVE_NIR_FACT_DYN_STR_LEN)
                               : ny_native_nir_emit_const(b, 0);
        if (length < 0)
          length = ny_native_nir_emit_const(b, 0);
        int payload = value;
        if (payload < 0 || length < 0 || tag < 0 ||
            !(is_f64 ? ny_native_nir_emit_store_f64(b, slot, payload)
                     : ny_native_nir_emit_store_i64(b, slot, payload)))
          return -1;
        int len_slot =
            ny_native_nir_emit_add_i64(b, slot, ny_native_nir_emit_const(b, 8));
        int tag_slot = ny_native_nir_emit_add_i64(
            b, slot, ny_native_nir_emit_const(b, 16));
        if (len_slot < 0 || tag_slot < 0 ||
            !ny_native_nir_emit_store_i64(b, len_slot, length) ||
            !ny_native_nir_emit_store_i64(b, tag_slot, tag))
          return -1;
      }
      int list_len = ny_native_nir_emit_const(b, (int64_t)count);
      if (list_len < 0 ||
          !ny_native_nir_record_list_len_fact(b, base, list_len))
        return -1;
      ny_native_nir_record_alloc_fact(b, base, (int64_t)(count * 24));
      return base;
    }
    /*
     * Constant list literal → pooled .data array.  The value is the array
     * address, so indexing lowers to address math + load.
     */
    ny_native_array_elem_t *values = calloc(count, sizeof(*values));
    if (!values) {
      ny_native_nir_fail(b,
                         "native NYIR lower: constant list allocation failed");
      return -1;
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
        ny_native_nir_fail(b, "native NYIR lower: only constant list literals "
                              "are supported in shared NYIR");
        free(values);
        return -1;
      }
      if (el->as.literal.kind == NY_LIT_INT) {
        if (string_elements) {
          ny_native_nir_fail(b,
                             "native NYIR lower: heterogeneous constant list "
                             "elements at %s:%d",
                             e->tok.filename ? e->tok.filename : "<source>",
                             e->tok.line);
          free(values);
          return -1;
        }
        saw_int_word = true;
        int64_t value = el->as.literal.as.i;
        if (negate) {
          if (value == INT64_MIN) {
            ny_native_nir_fail(
                b, "native NYIR lower: negated list integer overflows i64");
            free(values);
            return -1;
          }
          value = -value;
        }
        values[i].value = value;
        values[i].tag = 3;
      } else if (el->as.literal.kind == NY_LIT_FLOAT) {
        if (string_elements) {
          ny_native_nir_fail(b,
                             "native NYIR lower: heterogeneous constant list "
                             "elements at %s:%d",
                             e->tok.filename ? e->tok.filename : "<source>",
                             e->tok.line);
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
          ny_native_nir_fail(b, "native NYIR lower: heterogeneous constant "
                                "list elements are not supported");
          free(values);
          return -1;
        }
        string_elements = true;
        values[i].str = el->as.literal.as.s.data;
        values[i].str_len = el->as.literal.as.s.len;
        values[i].tag = TAG_STR_CONST;
      } else {
        ny_native_nir_fail(
            b, "native NYIR lower: unsupported constant list element");
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
    const char *arr_sym =
        ny_native_arraytab_intern(values, count, stride, NULL, 0);
    free(values);
    if (!arr_sym)
      return -1;
    int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .symbol = arr_sym});
    if (addr < 0)
      return -1;
    int length = ny_native_nir_emit_const(b, (int64_t)count);
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, addr, length))
      return -1;
    if (count <= INT64_MAX / stride)
      ny_native_nir_record_alloc_fact(b, addr, (int64_t)(count * stride));
    return addr;
  }
  case NY_E_INDEX: {
    /* Slice syntax is represented as an index node with a stop/step.  A
     * string is a raw C-string at this lowering boundary, so lower only
     * actual slices here.  The previous `!stop` condition classified plain
     * `s[i]` as `s[i:]`, returning a suffix instead of one character. */
    if (e->as.index.target &&
        ny_native_nir_expr_is_cstr(b, e->as.index.target) &&
        (e->as.index.stop || e->as.index.step) &&
        (!e->as.index.step ||
         (e->as.index.step->kind == NY_E_LITERAL &&
          e->as.index.step->as.literal.kind == NY_LIT_INT &&
          e->as.index.step->as.literal.as.i == 1))) {
      int string = ny_native_nir_lower_expr(b, e->as.index.target);
      int start = e->as.index.start
                      ? ny_native_nir_lower_expr(b, e->as.index.start)
                      : ny_native_nir_emit_const(b, 0);
      int stop = e->as.index.stop
                     ? ny_native_nir_lower_expr(b, e->as.index.stop)
                     : (string < 0 ? -1
                                    : ny_native_nir_emit_runtime_call(
                                          b, "rt_cstr_len", string, -1, -1,
                                          1, 0));
      if (string < 0 || start < 0 || stop < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_cstr_slice", string,
                                             start, stop, 3, 0);
    }
    bool target_is_dict = ny_native_nir_expr_is_dict(b, e->as.index.target);
    if (!target_is_dict && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT) {
      ny_native_nir_local_t *tl =
          ny_native_nir_find_local(b, e->as.index.target->as.ident.name);
      if (tl && tl->is_dict)
        target_is_dict = true;
    }
    bool target_is_seq =
        !target_is_dict &&
        (ny_native_nir_expr_is_list(b, e->as.index.target) ||
         ny_native_nir_expr_is_bytes(b, e->as.index.target) ||
         ny_native_nir_expr_is_cstr(b, e->as.index.target) ||
         ny_native_nir_expr_is_range(b, e->as.index.target) ||
         (ny_native_nir_expr_is_dyn_list(b, e->as.index.target) &&
          !ny_native_nir_expr_is_any(b, e->as.index.target)));
    if (!target_is_seq && !target_is_dict && e->as.index.target &&
        e->as.index.target->kind == NY_E_IDENT) {
      ny_native_nir_local_t *tl =
          ny_native_nir_find_local(b, e->as.index.target->as.ident.name);
      if (tl && (tl->is_list || tl->is_cstr || tl->is_bytes))
        target_is_seq = true;
    }
    bool non_int_index = false;
    if (e->as.index.start) {
      if (ny_native_nir_expr_is_cstr(b, e->as.index.start) ||
          ny_native_nir_expr_is_bool(b, e->as.index.start) ||
          (e->as.index.start->kind == NY_E_LITERAL &&
           (e->as.index.start->as.literal.kind == NY_LIT_STR ||
            e->as.index.start->as.literal.kind == NY_LIT_BOOL ||
            e->as.index.start->as.literal.kind == NY_LIT_FLOAT ||
            e->as.index.start->tok.kind == NY_T_NIL))) {
        non_int_index = true;
      }
    }
    if (target_is_seq && non_int_index) {
      return ny_native_nir_emit_runtime_call(b, "rt_index_key_error", -1,
                                             -1, -1, 0, 0);
    }
    if (e->as.index.target &&
        !ny_native_nir_expr_is_bytes(b, e->as.index.target) &&
        ny_native_nir_expr_is_cstr(b, e->as.index.target) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int str = ny_native_nir_lower_expr(b, e->as.index.target);
      int index = ny_native_nir_lower_expr(b, e->as.index.start);
      if (str < 0 || index < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_cstr_index_read_raw",
                                             str, index, -1, 2, 0);
    }
    if (e->as.index.target &&
        ny_native_nir_expr_is_bytes(b, e->as.index.target) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int bytes = ny_native_nir_lower_expr(b, e->as.index.target);
      int index = ny_native_nir_lower_expr(b, e->as.index.start);
      if (bytes < 0 || index < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_bytes_index_read_raw",
                                             bytes, index, -1, 2, 0);
    }
    if (e->as.index.target &&
        ny_native_nir_expr_is_range(b, e->as.index.target) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int range = ny_native_nir_lower_expr(b, e->as.index.target);
      int index = ny_native_nir_lower_expr(b, e->as.index.start);
      if (range < 0 || index < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_range_index_read_raw",
                                             range, index, -1, 2, 0);
    }
    /* An element loaded from a dynamic outer sequence can itself be a raw
     * typed list.  Reusing the outer dynamic stride (24-byte value/len/tag)
     * for the inner access reads the wrong slot; lower the outer expression
     * first and let the typed-buffer accessor use its actual 8-byte stride. */
    if (e->as.index.target && e->as.index.target->kind == NY_E_INDEX &&
        e->as.index.target->as.index.target &&
        (ny_native_nir_expr_is_list(b, e->as.index.target->as.index.target) ||
         ny_native_nir_expr_is_dyn_list(b,
                                        e->as.index.target->as.index.target)) &&
        e->as.index.start && !e->as.index.stop && !e->as.index.step) {
      int inner = ny_native_nir_lower_expr(b, e->as.index.target);
      int index = ny_native_nir_lower_expr(b, e->as.index.start);
      if (inner < 0 || index < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_index_read_raw",
                                             inner, index, -1, 2, 0);
    }
    if (!target_is_seq && e->as.index.target &&
        (e->as.index.target->kind == NY_E_IDENT ||
         ny_native_nir_expr_is_dict(b, e->as.index.target) ||
         ny_native_nir_expr_is_cstr(b, e->as.index.start))) {
      bool is_dict = ny_native_nir_expr_is_dict(b, e->as.index.target);
      if (e->as.index.target->kind == NY_E_IDENT) {
        ny_native_nir_local_t *dict_local =
            ny_native_nir_find_local(b, e->as.index.target->as.ident.name);
        if (dict_local && dict_local->is_dict)
          is_dict = true;
      }
      if (ny_native_nir_expr_is_cstr(b, e->as.index.start))
        is_dict = true;
      if (is_dict && e->as.index.start && !e->as.index.stop &&
          !e->as.index.step) {
        int dict = ny_native_nir_lower_expr(b, e->as.index.target);
        int key = ny_native_nir_lower_dict_key(b, e->as.index.start);
        /* Dictionary indexing is dynamic; a missing key's language default
         * is integer zero, not the nil immediate used by the raw bridge. */
        int fallback = ny_native_nir_emit_const(b, rt_tag_v(0));
        if (dict < 0 || key < 0 || fallback < 0)
          return -1;
        bool string_key = ny_native_nir_expr_is_cstr(b, e->as.index.start) ||
                          (e->as.index.start->kind == NY_E_LITERAL &&
                           e->as.index.start->as.literal.kind == NY_LIT_STR);
        int value = ny_native_nir_emit_runtime_call(
            b, string_key ? "rt_dict_get_str_raw" : "rt_value_get_tagged",
            dict, key, fallback, 3, 0);
        /* The bridge exposes a canonical dynamic value.  A dictionary literal
         * with a proven scalar result, however, is consumed by raw NYIR
         * arithmetic/comparisons; decode exactly at that typed boundary. */
        if (value >= 0 && e->semantic.resolved &&
            e->semantic.rep == NY_SEM_REP_RAW_INT)
          value = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64",
                                                  value, -1, -1, 1, 0);
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
      return ny_native_nir_lower_expr(b,
                                      e->as.index.target->as.list_like.data[0]);
    }
    const expr_t *literal_target =
        ny_native_nir_resolve_list_literal(b, e->as.index.target, 0);
    if (literal_target &&
        literal_target->kind == NY_E_LIST &&
        literal_target->as.list_like.len == 1 && e->as.index.start &&
        e->as.index.start->kind == NY_E_LITERAL &&
        e->as.index.start->as.literal.kind == NY_LIT_INT &&
        e->as.index.start->as.literal.as.i == 0)
      return ny_native_nir_lower_expr(b, literal_target->as.list_like.data[0]);
    bool dynamic_elements =
        literal_target ? ny_native_nir_expr_is_dyn_list(b, literal_target)
                       : ny_native_nir_expr_is_dyn_list(b, e->as.index.target);
    bool unknown_target = false;
    /* An unparameterized list formal can receive either 8-byte scalar slots
     * or 24-byte dynamic descriptors.  Its ABI deliberately carries no
     * compile-time stride, so do not guess one from the formal type; the
     * typed-buffer accessor reads the runtime header and handles both forms. */
    bool runtime_tbuf_access = false;
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT) {
      ny_native_nir_local_t *l =
          ny_native_nir_find_local(b, e->as.index.target->as.ident.name);
      /* A global list can be rebound after its initializer, including from
       * another flattened module.  Without a local proof of immutability,
       * read its length and stride from the runtime descriptor. */
      bool global_list = !l && ny_native_nir_find_top_level_value(
                                   b, e->as.index.target->as.ident.name);
      bool sequence_formal =
          l && l->type_name && strcmp(l->type_name, "seq") == 0;
      if (global_list || (l && (l->is_dyn_list || sequence_formal))) {
        dynamic_elements = true;
        runtime_tbuf_access = true;
      } else if (l && l->is_list)
        dynamic_elements = false;
      else if (l)
        unknown_target = true;
    }
    if (!dynamic_elements && !literal_target && e->as.index.target &&
        e->as.index.target->kind != NY_E_IDENT &&
        !ny_native_nir_expr_is_list(b, e->as.index.target)) {
      dynamic_elements = true;
    }
    int base = ny_native_nir_lower_expr(b, e->as.index.target);
    int idx = ny_native_nir_lower_expr(b, e->as.index.start);
    if (base < 0 || idx < 0)
      return -1;
    bool typed_static_target = false;
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name) {
      const ny_native_nir_local_t *target_local =
          ny_native_nir_find_local(b, e->as.index.target->as.ident.name);
      typed_static_target =
          target_local && target_local->is_list && !target_local->is_any;
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
    bool unknown_call_receiver =
        e->as.index.target &&
        (e->as.index.target->kind == NY_E_CALL ||
         e->as.index.target->kind == NY_E_MEMCALL) &&
        !typed_static_target;
    bool dynamic_index_result = b->index_for_any_call || runtime_tbuf_access ||
                                unknown_call_receiver;
    if (dynamic_index_result) {
      if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
          !b->index_for_any_call) {
        ny_native_nir_local_t *target_l =
            ny_native_nir_find_local(b, e->as.index.target->as.ident.name);
        if (target_l && target_l->is_list) {
          int neg24 = ny_native_nir_emit_const(b, -24);
          int neg16 = ny_native_nir_emit_const(b, -16);
          int eight = ny_native_nir_emit_const(b, 8);
          int len_addr =
              neg24 < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, neg24);
          int count =
              len_addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, len_addr);
          int esz_addr =
              neg16 < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, neg16);
          int elem_sz =
              esz_addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, esz_addr);
          int zero = ny_native_nir_emit_const(b, 0);
          int ge_zero =
              (zero < 0 || count < 0)
                  ? -1
                  : ny_native_nir_emit_cmp_i64(b, NYIR_CMP_GE, idx, zero);
          int lt_count =
              (ge_zero < 0)
                  ? -1
                  : ny_native_nir_emit_cmp_i64(b, NYIR_CMP_LT, idx, count);
          int is_e8 =
              (eight < 0 || elem_sz < 0)
                  ? -1
                  : ny_native_nir_emit_cmp_i64(b, NYIR_CMP_EQ, elem_sz, eight);
          int in_b = (lt_count < 0 || is_e8 < 0)
                         ? -1
                         : ny_native_nir_emit_binop(b, NYIR_AND_I64, ge_zero,
                                                    lt_count);
          int can_fast = (in_b < 0)
                             ? -1
                             : ny_native_nir_emit_binop(b, NYIR_AND_I64, in_b,
                                                        is_e8);
          if (can_fast >= 0) {
            int fast_lab = b->next_label++;
            int slow_lab = b->next_label++;
            int end_lab = b->next_label++;
            int res_slot = ny_native_nir_temp_slot(b);
            if (fast_lab >= 0 && slow_lab >= 0 && end_lab >= 0 &&
                res_slot >= 0) {
              if (!ny_native_nir_emit_br_if(b, can_fast, fast_lab) ||
                  !ny_native_nir_emit_br(b, slow_lab))
                return -1;

              if (!ny_native_nir_emit_label(b, fast_lab))
                return -1;
              int width8 = ny_native_nir_emit_const(b, 8);
              int off8 = ny_native_nir_push_val(b, NYIR_MUL_I64, idx, width8,
                                                0, NULL);
              int addr =
                  off8 < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, off8);
              if (addr < 0)
                return -1;
              bool is_f64 = ny_native_nir_expr_is_f64(b, e->as.index.target);
              int fast_val = is_f64 ? ny_native_nir_emit_load_f64(b, addr)
                                    : ny_native_nir_emit_load_i64(b, addr);
              if (fast_val < 0 ||
                  !ny_native_nir_store_local_value(b, res_slot, fast_val) ||
                  !ny_native_nir_emit_br(b, end_lab))
                return -1;

              if (!ny_native_nir_emit_label(b, slow_lab))
                return -1;
              int slow_val = ny_native_nir_emit_runtime_call(
                  b, "rt_tbuf_index_any_raw", base, idx, -1, 2, 0);
              if (slow_val < 0)
                return -1;
              if (typed_static_target) {
                slow_val = ny_native_nir_emit_runtime_call(
                    b, "rt_any_to_i64", slow_val, -1, -1, 1, 0);
              }
              if (slow_val < 0 ||
                  !ny_native_nir_store_local_value(b, res_slot, slow_val) ||
                  !ny_native_nir_emit_br(b, end_lab))
                return -1;

              if (!ny_native_nir_emit_label(b, end_lab))
                return -1;
              return ny_native_nir_load_local_value(b, res_slot);
            }
          }
        }
      }
      int value = ny_native_nir_emit_runtime_call(b, "rt_tbuf_index_any_raw",
                                                  base, idx, -1, 2, 0);
      if (value < 0)
        return -1;
      if (typed_static_target)
        return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", value, -1,
                                               -1, 1, 0);
      /*
       * An unknown call-result receiver crosses the tagged dynamic boundary
       * (canonical value out), but a proven-scalar consumer must not see the
       * tagged word: decode exactly here, mirroring the dictionary literal
       * rule above (f(9)[0] printed the tagged encoding instead of 9).
       */
      if (e->semantic.resolved && e->semantic.rep == NY_SEM_REP_RAW_INT)
        return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", value, -1,
                                               -1, 1, 0);
      return value;
    }
    /* Native list/tuple values use the data pointer as their base.  The
     * ordinary byte-offset path is correct for non-negative indices, but a
     * negative index would address the typed-buffer header.  Route negative
     * literals and containers with unknown element stride through the
     * representation-aware accessor. */
    if (runtime_tbuf_access) {
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_index_read_raw",
                                             base, idx, -1, 2, 0);
    }
    int64_t negative_index = 0;
    bool is_negative =
        ny_native_nir_negative_literal_i64(e->as.index.start, &negative_index);
    if (is_negative || unknown_target) {
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_index_read_raw",
                                             base, idx, -1, 2, 0);
    }
    int width = ny_native_nir_emit_const(b, dynamic_elements ? 24 : 8);
    int off = width < 0 ? -1
                        : ny_native_nir_push_val(b, NYIR_MUL_I64, idx, width, 0,
                                                 NULL);
    if (off < 0)
      return -1;
    int64_t byte_len = ny_native_nir_peek_alloc_fact(b, base);
    int length = ny_native_nir_peek_list_len_fact(b, base);
    if (literal_target && literal_target->kind == NY_E_LIST) {
      if (length < 0) {
        length = ny_native_nir_emit_const(
            b, (int64_t)literal_target->as.list_like.len);
      }
      if (byte_len <= 0) {
        byte_len = (int64_t)(literal_target->as.list_like.len *
                             (dynamic_elements ? 24 : 8));
      }
    }
    int dynamic_byte_len = -1;
    if (length >= 0) {
      dynamic_byte_len =
          ny_native_nir_push_val(b, NYIR_MUL_I64, length, width, 0, NULL);
      if (dynamic_byte_len < 0)
        return -1;
    }
    if (dynamic_byte_len < 0 && byte_len <= 0) {
      const ny_native_nir_local_t *target_l =
          (e->as.index.target && e->as.index.target->kind == NY_E_IDENT)
              ? ny_native_nir_find_local(b, e->as.index.target->as.ident.name)
              : NULL;
      if (target_l && target_l->is_list && !dynamic_elements &&
          !unknown_target) {
        int neg24 = ny_native_nir_emit_const(b, -24);
        int len_addr =
            neg24 < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, neg24);
        int count =
            len_addr < 0 ? -1 : ny_native_nir_emit_load_i64(b, len_addr);
        int zero = ny_native_nir_emit_const(b, 0);
        int ge_zero =
            (zero < 0 || count < 0)
                ? -1
                : ny_native_nir_emit_cmp_i64(b, NYIR_CMP_GE, idx, zero);
        int lt_count =
            (ge_zero < 0)
                ? -1
                : ny_native_nir_emit_cmp_i64(b, NYIR_CMP_LT, idx, count);
        int in_bounds =
            (lt_count < 0)
                ? -1
                : ny_native_nir_emit_binop(b, NYIR_AND_I64, ge_zero, lt_count);
        if (in_bounds >= 0) {
          int fast_lab = b->next_label++;
          int slow_lab = b->next_label++;
          int end_lab = b->next_label++;
          int res_slot = ny_native_nir_temp_slot(b);
          if (fast_lab >= 0 && slow_lab >= 0 && end_lab >= 0 && res_slot >= 0) {
            if (!ny_native_nir_emit_br_if(b, in_bounds, fast_lab) ||
                !ny_native_nir_emit_br(b, slow_lab))
              return -1;

            if (!ny_native_nir_emit_label(b, fast_lab))
              return -1;
            int addr = off < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, off);
            if (addr < 0)
              return -1;
            bool is_f64 = ny_native_nir_expr_is_f64(b, e->as.index.target);
            int fast_val = is_f64 ? ny_native_nir_emit_load_f64(b, addr)
                                  : ny_native_nir_emit_load_i64(b, addr);
            if (fast_val < 0 ||
                !ny_native_nir_store_local_value(b, res_slot, fast_val) ||
                !ny_native_nir_emit_br(b, end_lab))
              return -1;

            if (!ny_native_nir_emit_label(b, slow_lab))
              return -1;
            int slow_val = ny_native_nir_emit_runtime_call(
                b, "rt_tbuf_index_read_raw", base, idx, -1, 2, 0);
            if (slow_val < 0 ||
                !ny_native_nir_store_local_value(b, res_slot, slow_val) ||
                !ny_native_nir_emit_br(b, end_lab))
              return -1;

            if (!ny_native_nir_emit_label(b, end_lab))
              return -1;
            return ny_native_nir_load_local_value(b, res_slot);
          }
        }
      }
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_index_read_raw",
                                             base, idx, -1, 2, 0);
    }
    if (!ny_native_nir_emit_bounds_check_value(b, base, off, dynamic_byte_len,
                                               byte_len))
      return -1;
    int addr = off < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, off);
    if (addr < 0)
      return -1;
    bool is_f64 = ny_native_nir_expr_is_f64(b, e->as.index.target);
    if (dynamic_elements) {
      int payload = ny_native_nir_emit_load_i64(b, addr);
      int o8 = ny_native_nir_emit_const(b, 8);
      int o16 = ny_native_nir_emit_const(b, 16);
      int a8 = o8 < 0 ? -1 : ny_native_nir_emit_add_i64(b, addr, o8);
      int a16 = o16 < 0 ? -1 : ny_native_nir_emit_add_i64(b, addr, o16);
      int len = a8 < 0 ? -1 : ny_native_nir_emit_load_i64(b, a8);
      int tag = a16 < 0 ? -1 : ny_native_nir_emit_load_i64(b, a16);
      if (payload < 0 || len < 0 || tag < 0)
        return -1;
      if (!ny_native_nir_record_dyn_fact(b, payload,
                                         NY_NATIVE_NIR_FACT_DYN_STR_LEN, len) ||
          !ny_native_nir_record_dyn_fact(b, payload, NY_NATIVE_NIR_FACT_DYN_TAG,
                                         tag))
        return -1;
      return payload;
    }
    return is_f64 ? ny_native_nir_emit_load_f64(b, addr)
                  : ny_native_nir_emit_load_i64(b, addr);
  }
  case NY_E_MEMCALL: {
    /* `module_alias.fn(...)` is a namespace call, never a receiver call.
     * Semantic dispatch can annotate the same syntax as an attached method;
     * canonicalize it before the dynamic-call path adds the alias as an ABI
     * argument and emits an unresolved `lea module_alias`. */
    if (e->as.memcall.target && e->as.memcall.name &&
        e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name) {
      const char *alias = e->as.memcall.target->as.ident.name;
      const char *module = ny_native_nir_resolve_use_alias(b, alias);
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
                         e->as.memcall.name);
        if (n <= 0 || (size_t)n >= sizeof(canonical)) {
          ny_native_nir_fail(b,
                             "native NYIR lower: module call name is too long");
          return -1;
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
        return ny_native_nir_lower_expr(b, &call);
      }
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "get") == 0 &&
        (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2) &&
        ny_native_nir_expr_is_list(b, e->as.memcall.target)) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int index = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      int fallback =
          e->as.memcall.args.len == 2
              ? ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val)
              : ny_native_nir_emit_const(b, 0);
      if (list < 0 || index < 0 || fallback < 0)
        return -1;
      /* A typed scalar list's `get` result is consumed through the native
       * scalar ABI in ordinary expressions (including print/equality).  The
       * dynamic-list variant carries per-element tags and must use the any
       * accessor; tagging a raw odd scalar here turns 3 into 7. */
      const char *get_runtime = ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target)
                                    ? "rt_tbuf_get_any"
                                    : "rt_tbuf_get";
      int value = ny_native_nir_emit_runtime_call(b, get_runtime, list, index,
                                                  fallback, 3, 0);
      bool wants_f64 = ny_native_nir_expr_is_f64(b, e) ||
                       (e->as.memcall.args.len == 2 &&
                        ny_native_nir_expr_is_f64(
                            b, e->as.memcall.args.data[1].val));
      if (wants_f64) {
        return ny_native_nir_emit_runtime_call(b, "rt_any_to_f64", value,
                                               -1, -1, 1,
                                               NYIR_INST_F_RET_F64);
      }
      if (!ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target))
        return value;
      int tag = value < 0 ? -1
                          : ny_native_nir_emit_runtime_call(
                                b, "rt_tbuf_tag", list, index, -1, 2, 0);
      if (value < 0 || tag < 0 ||
          !ny_native_nir_record_dyn_fact(b, value, NY_NATIVE_NIR_FACT_DYN_TAG,
                                         tag))
        return -1;
      return value;
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "extend") == 0 &&
        e->as.memcall.args.len == 1 &&
        ny_native_nir_expr_is_list(b, e->as.memcall.target) &&
        ny_native_nir_expr_is_list(b, e->as.memcall.args.data[0].val)) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int other = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      if (list < 0 || other < 0)
        return -1;
      int out = ny_native_nir_emit_runtime_call(b, "rt_tbuf_extend",
                                                list, other, -1, 2, 0);
      return out < 0 ? -1 : out;
    }
    ny_native_nir_local_t *append_local =
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
                e->as.memcall.target->as.ident.name
            ? ny_native_nir_find_local(b, e->as.memcall.target->as.ident.name)
            : NULL;
    if (e->as.memcall.target &&
        ((append_local && append_local->is_list) ||
         e->as.memcall.target->kind == NY_E_CALL ||
         e->as.memcall.target->kind == NY_E_MEMCALL) &&
        e->as.memcall.name && strcmp(e->as.memcall.name, "append") == 0 &&
        e->as.memcall.args.len == 1) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      const expr_t *append_value = e->as.memcall.args.data[0].val;
      int value = ny_native_nir_lower_expr(b, append_value);
      if (list < 0 || value < 0)
        return -1;
      int is_string = ny_native_nir_emit_const(
          b, ny_native_nir_expr_is_cstr(b, append_value) ? 1 : 0);
      const ny_native_nir_local_t *append_value_local =
          append_value && append_value->kind == NY_E_IDENT
              ? ny_native_nir_find_local(b, append_value->as.ident.name)
              : NULL;
      const char *append_symbol =
           ((append_value_local && append_value_local->is_any) ||
           ny_native_nir_expr_is_any(b, append_value) ||
           (append_local && append_local->is_dyn_list)) &&
           !ny_native_nir_expr_is_raw_dynamic_read(b, append_value)
              ? "rt_tbuf_append_tagged"
              : "rt_tbuf_append_raw";
      if (strcmp(append_symbol, "rt_tbuf_append_tagged") == 0) {
        bool is_bool_val = ny_native_nir_expr_is_bool(b, append_value);
        if (append_value_local && append_value_local->is_bool)
          is_bool_val = true;
        if (is_bool_val) {
          value = ny_native_nir_box_bool(b, value);
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
            value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1,
                                                     -1, 1, 0);
          } else if (ny_native_nir_expr_is_f64(b, append_value) ||
                     ny_native_nir_expr_is_f32(b, append_value)) {
            int bits = ny_native_nir_emit_runtime_call(
                b, "rt_f64_bits", value, -1, -1, 1, 0);
            value = bits < 0
                        ? -1
                        : ny_native_nir_emit_runtime_call(
                              b, "rt_flt_box_val", bits, -1, -1, 1, 0);
          }
        }
        if (value < 0)
          return -1;
      }
      int out = is_string < 0
                    ? -1
                    : ny_native_nir_emit_runtime_call(b, append_symbol, list,
                                                      value, is_string, 3, 0);
      int length = ny_native_nir_emit_known_list_append_len(b, list, out);
      if (out < 0 || length < 0 ||
          !ny_native_nir_record_list_len_fact(b, out, length))
        return -1;
      if (e->as.memcall.target->kind == NY_E_IDENT) {
        ny_native_nir_local_t *target_local =
            ny_native_nir_find_local(b, e->as.memcall.target->as.ident.name);
        if (target_local) {
          target_local->is_list = true;
          if (target_local->list_len_slot < 0)
            target_local->list_len_slot = b->next_local_slot++;
          if (!ny_native_nir_store_local_value(b, target_local->list_len_slot,
                                               length))
            return -1;
          if (!ny_native_nir_store_local_value(b, target_local->slot, out))
            return -1;
        }
      }
      return out;
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "pop") == 0 && e->as.memcall.args.len == 0) {
      const expr_t *target_expr = e->as.memcall.target;
      ny_native_nir_local_t *pop_local =
          target_expr->kind == NY_E_IDENT
              ? ny_native_nir_find_local(b, target_expr->as.ident.name)
              : NULL;
      if ((pop_local && pop_local->is_list) ||
          ny_native_nir_expr_is_dyn_list(b, target_expr)) {
        int list = ny_native_nir_lower_expr(b, target_expr);
        int out = list < 0 ? -1
                           : ny_native_nir_emit_runtime_call(
                                 b, "rt_tbuf_pop_raw", list, -1, -1, 1, 0);
        if (list < 0 || out < 0)
          return -1;
        int length = ny_native_nir_emit_known_list_pop_len(b, list);
        if (length < 0)
          length = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw",
                                                   list, -1, -1, 1, 0);
        if (length < 0 || !ny_native_nir_record_list_len_fact(b, list, length))
          return -1;
        if (pop_local) {
          if (pop_local->list_len_slot < 0)
            pop_local->list_len_slot = b->next_local_slot++;
          if (!ny_native_nir_store_local_value(b, pop_local->list_len_slot,
                                               length))
            return -1;
        }
        return out;
      }
    }
    if (!e->as.memcall.target || !e->as.memcall.name) {
      ny_native_nir_fail(
          b, "native NYIR lower: member call has no target or method");
      return -1;
    }
    bool target_is_ident = e->as.memcall.target->kind == NY_E_IDENT &&
                           e->as.memcall.target->as.ident.name;
    ny_native_nir_local_t *local =
        target_is_ident
            ? ny_native_nir_find_local(b, e->as.memcall.target->as.ident.name)
            : NULL;
    const char *method = e->as.memcall.name;
    const char *dynamic_target_ident = e->as.memcall.target->kind == NY_E_IDENT
                                           ? e->as.memcall.target->as.ident.name
                                           : NULL;
    bool dynamic_target_is_module =
        dynamic_target_ident && !local &&
        ny_native_nir_resolve_use_alias(b, dynamic_target_ident) != NULL;
    if (strcmp(method, "get") == 0 && e->as.memcall.args.len >= 1 &&
        e->as.memcall.args.len <= 2 && !dynamic_target_is_module &&
        ny_native_nir_expr_is_bytes(b, e->as.memcall.target)) {
      int value = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int key =
          (ny_native_nir_expr_is_dict(b, e->as.memcall.target) ||
           (local && local->is_dict))
              ? ny_native_nir_lower_dict_key(b, e->as.memcall.args.data[0].val)
              : ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      int fallback =
          e->as.memcall.args.len == 2
              ? ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val)
              : ny_native_nir_emit_const(b, 0);
      return value < 0 || key < 0 || fallback < 0
                 ? -1
                 : ny_native_nir_emit_runtime_call(b, "rt_bytes_get_raw",
                                                   value, key, fallback, 3, 0);
    }
    if (strcmp(method, "get") == 0 && e->as.memcall.args.len >= 1 &&
        e->as.memcall.args.len <= 2 && !dynamic_target_is_module) {
      int value = ny_native_nir_lower_expr(b, e->as.memcall.target);
      bool target_is_list =
          ny_native_nir_expr_is_list(b, e->as.memcall.target) ||
          ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target) ||
          (local && local->is_list);
      if (!target_is_list && !local && e->as.memcall.target->kind == NY_E_IDENT &&
          e->as.memcall.target->as.ident.name) {
        const expr_t *global = ny_native_nir_find_top_level_value(
            b, e->as.memcall.target->as.ident.name);
        target_is_list = global && global != e->as.memcall.target &&
                         ny_native_nir_expr_is_list(b, global);
      }
      bool dynamic_target =
          e->as.memcall.target->kind == NY_E_CALL ||
          e->as.memcall.target->kind == NY_E_MEMCALL ||
          /* An `any` local may hold a native tbuf even when flow inference
           * conservatively marks its current value as list-like.  The
           * generic tagged value-get entry point still consumes a tagged index
           * at that boundary, so retain the dynamic ABI for every any local. */
          (local && local->is_any);
      bool dict_like_target =
          !ny_native_nir_expr_is_bytes(b, e->as.memcall.target) &&
          !target_is_list &&
          !ny_native_nir_expr_is_range(b, e->as.memcall.target) &&
          !dynamic_target;
      int key =
          dict_like_target
              ? ny_native_nir_lower_dict_key(b, e->as.memcall.args.data[0].val)
              : ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      /* `rt_value_get_tagged` accepts the legacy tagged index ABI for dynamic
       * values, while NYIR lowers a raw native integer index.  Tag only this
       * dynamic boundary; typed tbuf/list access must keep its raw index. */
      const expr_t *key_arg = e->as.memcall.args.data[0].val;
      bool key_is_lit_int = key_arg && key_arg->kind == NY_E_LITERAL &&
                            key_arg->as.literal.kind == NY_LIT_INT &&
                            key_arg->tok.kind != NY_T_NIL;
      bool key_is_raw_int =
          key_is_lit_int ||
          (key_arg && key_arg->semantic.rep == NY_SEM_REP_RAW_INT);
      if (!key_is_raw_int && key_arg && key_arg->kind == NY_E_IDENT) {
        ny_native_nir_local_t *kl =
            ny_native_nir_find_local(b, key_arg->as.ident.name);
        if (kl && kl->semantic_rep == NY_SEM_REP_RAW_INT)
          key_is_raw_int = true;
      }
      if (dynamic_target && key_is_raw_int &&
          !ny_native_nir_expr_is_any(b, key_arg) &&
          !ny_native_nir_expr_is_cstr(b, key_arg)) {
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
          e->as.memcall.args.len == 2
              ? ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val)
              : ny_native_nir_emit_const(b, 0);
      if (e->as.memcall.args.len == 2 &&
          ny_native_nir_expr_is_f64(b, e->as.memcall.args.data[1].val)) {
        int bits = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", fallback,
                                                   -1, -1, 1, 0);
        fallback = bits < 0
                       ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_flt_box_val", bits, -1, -1, 1, 0);
      }
      if (value < 0 || key < 0 || fallback < 0)
        return -1;
      /* Typed list/bytes sequence methods expose raw slot payloads. Using
       * the dynamic tagged accessor here misclassifies an odd raw byte such
       * as 3 as tagged(1), so keep the index and result on the raw path. */
      if (target_is_list && !dynamic_target) {
        return ny_native_nir_emit_runtime_call(b, "rt_tbuf_index_read_raw",
                                               value, key, fallback, 3, 0);
      }
      int got = ny_native_nir_emit_runtime_call(
          b,
          (dict_like_target &&
           !ny_native_nir_expr_is_cstr(b, e->as.memcall.target))
              ? ny_native_nir_dict_get_symbol(
                    b, e->as.memcall.args.data[0].val)
          : ny_native_nir_expr_is_cstr(b, e->as.memcall.args.data[0].val)
              ? "rt_dict_get_str_raw"
              : "rt_value_get_tagged",
          value, key, fallback, 3, 0);
      bool wants_f64 = e->as.memcall.args.len == 2 &&
                       ny_native_nir_expr_is_f64(
                           b, e->as.memcall.args.data[1].val);
      if (got >= 0 && !wants_f64 && e->as.memcall.args.len == 2 &&
          e->as.memcall.args.data[1].val &&
          e->as.memcall.args.data[1].val->kind == NY_E_LITERAL &&
          e->as.memcall.args.data[1].val->as.literal.kind == NY_LIT_INT &&
          e->as.memcall.args.data[1].val->tok.kind != NY_T_NIL &&
          /* String-key dictionary reads return the stored raw payload from
           * the native dictionary bridge.  Applying any_to_i64 here would
           * interpret an odd raw value such as 7 as tagged(3). */
          !ny_native_nir_expr_is_cstr(b, key_arg) &&
          (target_is_list || dynamic_target ||
           !ny_native_nir_expr_is_dict(b, e->as.memcall.target)))
        return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", got, -1,
                                               -1, 1, 0);
      return wants_f64
                 ? ny_native_nir_emit_runtime_call(b, "rt_any_to_f64", got,
                                                   -1, -1, 1,
                                                   NYIR_INST_F_RET_F64)
                 : got;
    }
    if (strcmp(method, "slice") == 0 &&
        (e->as.memcall.args.len == 2 ||
         (e->as.memcall.args.len == 3 && e->as.memcall.args.data[2].val &&
          e->as.memcall.args.data[2].val->kind == NY_E_LITERAL &&
          e->as.memcall.args.data[2].val->as.literal.kind == NY_LIT_INT &&
          e->as.memcall.args.data[2].val->as.literal.as.i == 1)) &&
        ny_native_nir_expr_is_list(b, e->as.memcall.target)) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int start = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      int stop = ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val);
      int out = list < 0 || start < 0 || stop < 0
                    ? -1
                    : ny_native_nir_emit_runtime_call(b, "rt_tbuf_slice",
                                                      list, start, stop, 3, 0);
      if (out < 0)
        return -1;
      int length = ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", out,
                                                   -1, -1, 1, 0);
      return length < 0 || !ny_native_nir_record_list_len_fact(b, out, length)
                 ? -1
                 : out;
    }
    /*
     * The language resolves methods on `any` through its attached interface.
     * Native lowering represents that interface as the same ordinary function
     * call: receiver first, then the source arguments. Do not speculate that
     * an `any` value is a dictionary.
     */
    if (strcmp(method, "contains") == 0 && e->as.memcall.args.len == 1 &&
        !dynamic_target_is_module) {
      int container = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int item = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      return container < 0 || item < 0
                 ? -1
                 : ny_native_nir_emit_runtime_call(b, "rt_contains_raw",
                                                   container, item, -1, 2, 0);
    }
    if ((e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT ||
         e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED) &&
        !ny_native_nir_user_defined_fn(b, method) &&
        !(local && local->is_dict) &&
        !ny_native_nir_expr_is_dict(b, e->as.memcall.target)) {
      if (e->as.memcall.args.len + 1 > NYIR_CALL_MAX_ARGS) {
        ny_native_nir_fail(b,
                           "native NYIR lower: dynamic method call exceeds "
                           "the maximum supported argument count (%d)",
                           NYIR_CALL_MAX_ARGS);
        return -1;
      }
      const char *canonical = e->semantic.canonical_callee;
      if (!canonical && e->semantic.canonical_callee_stmt)
        canonical = e->semantic.canonical_callee_stmt->as.fn.name;
      const stmt_t *attached_method = ny_native_nir_find_attached_method(
          b, e->as.memcall.target, method);
      /* Keep the exact declaration selected by attached-method lookup.  The
       * owner.method spelling is only a source-level convenience and can
       * disagree with the module-qualified symbol collected by lowering. */
      if (attached_method && attached_method->as.fn.name)
        canonical = attached_method->as.fn.name;
      if (canonical && strncmp(canonical, "std.core.", 9) == 0) {
        const stmt_t *fn = ny_native_nir_find_user_function(b, canonical);
        if (fn && fn->as.fn.name &&
            strncmp(fn->as.fn.name, "std.core.", 9) == 0)
          canonical = fn->as.fn.name;
      }
      if (!canonical) {
        ny_native_nir_fail(b,
                           "native NYIR lower: semantic member target is "
                           "missing for '%s'",
                           method);
        return -1;
      }
      /*
       * A module namespace reference spelled `alias.method(...)` (for
       * example `it.map` where `it` aliases `std.core.iter`) is resolved to
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
        namespace_target =
            (!ny_native_nir_find_local(b, m_rcv) &&
             ny_native_nir_resolve_use_alias(b, m_rcv) != NULL) ||
            (!ny_native_nir_find_local(b, m_rcv) &&
             !ny_native_nir_find_user_function(b, m_rcv) &&
             !ny_native_nir_find_top_level_value(b, m_rcv));
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
      return ny_native_nir_lower_expr(b, &call);
    }
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        (ny_native_nir_expr_is_bytes(b, e->as.memcall.target) ||
         (local && local->is_bytes))) {
      int target = ny_native_nir_lower_expr(b, e->as.memcall.target);
      if (target < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_len", target, -1, -1,
                                             1, 0);
    }
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        ny_native_nir_expr_is_cstr(b, e->as.memcall.target)) {
      int target = ny_native_nir_lower_expr(b, e->as.memcall.target);
      if (target < 0)
        return -1;
      int dynamic_length = ny_native_nir_peek_dyn_fact(
          b, target, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
      if (dynamic_length >= 0)
        return dynamic_length;
      return ny_native_nir_emit_runtime_call(b, "rt_cstr_len", target,
                                             -1, -1, 1, 0);
    }
    if (e->as.memcall.target && strcmp(method, "append") == 0 &&
        e->as.memcall.args.len == 1) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      const expr_t *append_value = e->as.memcall.args.data[0].val;
      int value = ny_native_nir_lower_expr(b, append_value);
      if (list < 0 || value < 0)
        return -1;
      int out = ny_native_nir_emit_runtime_call(b, "rt_append", list, value, -1,
                                                2, 0);
      int length = ny_native_nir_emit_known_list_append_len(b, list, out);
      if (out < 0 || length < 0 ||
          !ny_native_nir_record_list_len_fact(b, out, length))
        return -1;
      if (e->as.memcall.target->kind == NY_E_IDENT) {
        ny_native_nir_local_t *target_local =
            ny_native_nir_find_local(b, e->as.memcall.target->as.ident.name);
        if (target_local) {
          target_local->is_list = true;
          if (target_local->list_len_slot < 0)
            target_local->list_len_slot = b->next_local_slot++;
          if (!ny_native_nir_store_local_value(b, target_local->list_len_slot,
                                               length))
            return -1;
          if (!ny_native_nir_store_local_value(b, target_local->slot, out))
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
        !ny_native_nir_expr_is_bytes(b, e->as.memcall.target) &&
        (!(local && local->is_bytes)) &&
        ((local && local->is_list) ||
         ny_native_nir_expr_is_list(b, e->as.memcall.target) ||
         ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target))) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      if (list < 0)
        return -1;
      int known_len = ny_native_nir_peek_list_len_fact(b, list);
      if (known_len >= 0)
        return known_len;
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", list, -1,
                                             -1, 1, 0);
    }
    /* Unknown identifier receivers keep the strict dynamic ABI.  Their `.len`
     * operation must reject scalar values instead of silently returning zero. */
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        !ny_native_nir_expr_is_list(b, e->as.memcall.target) &&
        !ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target) &&
        !ny_native_nir_expr_is_bytes(b, e->as.memcall.target) &&
        !ny_native_nir_expr_is_cstr(b, e->as.memcall.target) &&
        !ny_native_nir_expr_is_range(b, e->as.memcall.target)) {
      int value = ny_native_nir_lower_expr(b, e->as.memcall.target);
      return value < 0 ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_len_strict", value, -1, -1, 1, 0);
    }
    /* Untyped `any` receivers keep the dynamic ABI.  Their `.len` operation
     * still needs to understand native tbuf/list/string handles, but must not
     * force the function parameter itself into a list ABI (callbacks are
     * invoked through a two-argument dynamic call path). */
    if (strcmp(method, "len") == 0 && e->as.memcall.args.len == 0 &&
        ((local && local->is_any) ||
         ny_native_nir_expr_is_any(b, e->as.memcall.target))) {
      int value = ny_native_nir_lower_expr(b, e->as.memcall.target);
      return value < 0 ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_len_strict", value, -1, -1, 1, 0);
    }
    if (!ny_native_nir_expr_is_bytes(b, e->as.memcall.target) &&
        (ny_native_nir_expr_is_list(b, e->as.memcall.target) ||
         (local && local->is_list && !local->is_any)) &&
        strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int index = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      int value = ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val);
      if (list < 0 || index < 0 || value < 0)
        return -1;
      if (ny_native_nir_expr_is_f64(b, e->as.memcall.args.data[1].val)) {
        value = ny_native_nir_emit_runtime_call(b, "rt_f64_bits", value,
                                                -1, -1, 1, 0);
        if (value < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_f64_bits",
                                               list, index, value, 3, 0);
      }
      return ny_native_nir_emit_runtime_call(b, "rt_tbuf_set_i64_raw", list,
                                             index, value, 3, 0);
    }
    if (ny_native_nir_expr_is_bytes(b, e->as.memcall.target) &&
        strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
      int bytes = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int index = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      int value = ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val);
      if (bytes < 0 || index < 0 || value < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_bytes_set_raw", bytes,
                                             index, value, 3, 0);
    }
    const char *target_ident =
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT
            ? e->as.memcall.target->as.ident.name
            : NULL;
    /* A local receiver shadows a module alias with the same spelling.  In
     * particular, `io.set(...)` in net helpers is a dictionary mutation on
     * the `io` parameter, not a call to a hypothetical `std.core.io.set`.
     */
    bool is_module_namespace =
        target_ident && !local &&
        ny_native_nir_resolve_use_alias(b, target_ident) != NULL;
    bool is_bytes = ny_native_nir_expr_is_bytes(b, e->as.memcall.target);
    bool is_dict =
        !is_bytes && (ny_native_nir_expr_is_dict(b, e->as.memcall.target) ||
                      (local && local->is_dict));
    if (!is_dict && !is_module_namespace && strcmp(method, "contains") == 0 &&
        e->as.memcall.args.len == 1 && e->as.memcall.args.data[0].val) {
      int container = ny_native_nir_lower_expr(b, e->as.memcall.target);
      int item = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      return container < 0 || item < 0
                 ? -1
                 : ny_native_nir_emit_runtime_call(b, "rt_contains_raw",
                                                   container, item, -1, 2, 0);
    }
    const stmt_t *user_method_fn = ny_native_nir_find_user_function(b, method);
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
      int dict = ny_native_nir_lower_expr(b, e->as.memcall.target);
      if (dict < 0)
        return -1;
      if (strcmp(method, "keys") == 0) {
        if (e->as.memcall.args.len != 0) {
          ny_native_nir_fail(b,
                             "native NYIR lower: dict.keys takes no arguments");
          return -1;
        }
        return ny_native_nir_emit_runtime_call(b, "rt_dict_keys_raw", dict,
                                               -1, -1, 1, 0);
      }
      if (strcmp(method, "values") == 0 &&
          (is_dict || ny_native_nir_expr_is_dict(b, e->as.memcall.target))) {
        if (e->as.memcall.args.len != 0) {
          ny_native_nir_fail(
              b, "native NYIR lower: dict.values takes no arguments");
          return -1;
        }
        return ny_native_nir_emit_runtime_call(b, "rt_dict_values_raw", dict,
                                               -1, -1, 1, 0);
      }
      if (strcmp(method, "items") == 0 &&
          (is_dict || ny_native_nir_expr_is_dict(b, e->as.memcall.target))) {
        if (e->as.memcall.args.len != 0) {
          ny_native_nir_fail(
              b, "native NYIR lower: dict.items takes no arguments");
          return -1;
        }
        return ny_native_nir_emit_runtime_call(b, "rt_dict_items_raw", dict,
                                               -1, -1, 1, 0);
      }
      if (strcmp(method, "len") == 0) {
        if (e->as.memcall.args.len != 0) {
          ny_native_nir_fail(b,
                             "native NYIR lower: dict.len takes no arguments");
          return -1;
        }
        if (ny_native_nir_expr_is_list(b, e->as.memcall.target) ||
            ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target))
          return ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", dict,
                                                 -1, -1, 1, 0);
        if (ny_native_nir_expr_is_dict(b, e->as.memcall.target))
          return ny_native_nir_emit_runtime_call(b, "rt_dict_len_raw", dict,
                                                 -1, -1, 1, 0);
        return ny_native_nir_emit_runtime_call(b, "rt_len", dict, -1, -1,
                                               1, 0);
      }
      if ((strcmp(method, "has") == 0 || strcmp(method, "contains") == 0 ||
           strcmp(method, "exists") == 0) &&
          e->as.memcall.args.len == 1) {
        int key =
            ny_native_nir_lower_dict_key(b, e->as.memcall.args.data[0].val);
        if (key < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_cstr(b, e->as.memcall.args.data[0].val)
                ? "rt_dict_has_str_raw"
                : "rt_dict_has_raw",
            dict, key, -1, 2, 0);
      }
      if ((strcmp(method, "delete") == 0 || strcmp(method, "remove") == 0) &&
          e->as.memcall.args.len == 1) {
        int key =
            ny_native_nir_lower_dict_key(b, e->as.memcall.args.data[0].val);
        if (key < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b,
            ny_native_nir_expr_is_cstr(b, e->as.memcall.args.data[0].val)
                ? "rt_dict_delete_str_raw"
                : "rt_dict_delete_raw",
            dict, key, -1, 2, 0);
      }
      if (strcmp(method, "merge") == 0 && e->as.memcall.args.len == 1) {
        int other = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
        if (other < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(b, "rt_dict_merge_raw", dict,
                                               other, -1, 2, 0);
      }
      if (strcmp(method, "get") == 0 &&
          (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
        int key =
            ny_native_nir_lower_dict_key(b, e->as.memcall.args.data[0].val);
        int fallback =
            e->as.memcall.args.len == 2
                ? ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val)
                : ny_native_nir_emit_const(b, 0);
        if (key < 0 || fallback < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b, ny_native_nir_dict_get_symbol(b, e->as.memcall.args.data[0].val),
            dict, key, fallback, 3, 0);
      }
      if (strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
        int key =
            ny_native_nir_lower_dict_key(b, e->as.memcall.args.data[0].val);
        int value = ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val);
        if (key < 0 || value < 0)
          return -1;
        /* Native boolean expressions use raw 0/1 for control flow, but
         * dictionary slots are dynamic values.  Preserve the canonical Ny
         * bool immediates at this container boundary. */
        const expr_t *value_expr = e->as.memcall.args.data[1].val;
        bool is_bool_val = ny_native_nir_expr_is_bool(b, value_expr);
        const ny_native_nir_local_t *value_local = NULL;
        if (value_expr && value_expr->kind == NY_E_IDENT &&
            value_expr->as.ident.name) {
          value_local =
              ny_native_nir_find_local(b, value_expr->as.ident.name);
          if (value_local && value_local->is_bool)
            is_bool_val = true;
        }
        if (is_bool_val) {
          value = ny_native_nir_box_bool(b, value);
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
              !ny_native_nir_expr_is_f64(b, value_expr) &&
              !ny_native_nir_expr_is_f32(b, value_expr) &&
              !ny_native_nir_expr_is_bool(b, value_expr) &&
              !ny_native_nir_expr_is_any(b, value_expr))
            raw_dict_integer = true;
          if (value_local && !is_bool_val &&
              value_local->semantic_rep == NY_SEM_REP_RAW_INT)
            raw_dict_integer = true;
          if (raw_dict_integer) {
            value = ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1,
                                                    -1, 1, 0);
            if (value < 0)
              return -1;
          }
        }
        return ny_native_nir_emit_runtime_call(
            b, ny_native_nir_dict_set_symbol(b, e->as.memcall.args.data[0].val),
            dict, key, value, 3, 0);
      }
      ny_native_nir_fail(b, "native NYIR lower: unsupported dict.%s arity",
                         method);
      return -1;
    }
    char qualified[512];
    if (!ny_native_nir_qualified_expr(e->as.memcall.target, qualified,
                                      sizeof(qualified))) {
      const stmt_t *method_fn =
          ny_native_nir_find_user_function(b, e->as.memcall.name);
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
        return ny_native_nir_lower_expr(b, &call);
      }
      ny_native_nir_fail(
          b,
          "native NYIR lower: unsupported member-call target at %s:%d "
          "(rep=%d resolved=%d call=%d callee=%s)",
          e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
          (int)e->as.memcall.target->semantic.rep,
          e->as.memcall.target->semantic.resolved ? 1 : 0,
          (int)e->semantic.member_call_kind,
          e->semantic.canonical_callee ? e->semantic.canonical_callee
                                       : "<none>");
      return -1;
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
        const char *module = ny_native_nir_resolve_use_alias(b, alias);
        if (module) {
          char canonical[512];
          int cn = snprintf(canonical, sizeof(canonical), "%s%s", module, dot);
          if (cn < 0 || (size_t)cn >= sizeof(canonical)) {
            ny_native_nir_fail(
                b, "native NYIR lower: canonical call name is too long");
            return -1;
          }
          memcpy(qualified, canonical, (size_t)cn + 1);
        }
      }
    }
    if (n < 0 || (size_t)n >= sizeof(qualified) - strlen(qualified)) {
      ny_native_nir_fail(b,
                         "native NYIR lower: qualified call name is too long");
      return -1;
    }
    if (e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        strcmp(e->as.memcall.target->as.ident.name, "c") == 0 &&
        strcmp(e->as.memcall.name, "sqrt") == 0 &&
        e->as.memcall.args.len == 1) {
      int arg = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
      if (arg < 0)
        return -1;
      if (ny_native_nir_expr_is_f64(b, e->as.memcall.args.data[0].val))
        return ny_native_nir_emit_runtime_call(b, "rt_sqrt_f64", arg, -1,
                                               -1, 1, NYIR_INST_F_RET_F64);
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
      return ny_native_nir_lower_expr(b, &call);
    }
    const stmt_t *qualified_fn = ny_native_nir_find_user_function(b, qualified);
    if (qualified_fn || is_module_namespace) {
      expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
      callee.as.ident.name = qualified_fn && qualified_fn->as.fn.name
                                 ? qualified_fn->as.fn.name
                                 : qualified;
      expr_t call = *e;
      call.kind = NY_E_CALL;
      call.semantic.canonical_callee = callee.as.ident.name;
      call.semantic.canonical_callee_stmt = (stmt_t *)qualified_fn;
      call.semantic.member_call_kind = NY_SEM_CALL_NONE;
      call.as.call.callee = &callee;
      call.as.call.args = e->as.memcall.args;
      return ny_native_nir_lower_expr(b, &call);
    }
    const stmt_t *method_fn =
        ny_native_nir_find_user_function(b, e->as.memcall.name);
    const stmt_t *target_callee = e->semantic.canonical_callee_stmt;
    if (!target_callee && e->semantic.canonical_callee)
      target_callee =
          ny_native_nir_find_user_function(b, e->semantic.canonical_callee);
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
      return ny_native_nir_lower_expr(b, &call);
    }
    expr_t callee = {.kind = NY_E_IDENT, .tok = e->tok};
    callee.as.ident.name = qualified;
    expr_t call = *e;
    call.kind = NY_E_CALL;
    call.as.call.callee = &callee;
    call.as.call.args = e->as.memcall.args;
    return ny_native_nir_lower_expr(b, &call);
  }
  case NY_E_MEMBER: {
    if (!e->as.member.name || !e->as.member.target) {
      ny_native_nir_fail(b, "native NYIR lower: malformed member access");
      return -1;
    }
    /* Semantic attached-call facts normally redirect properties through
     * their canonical function.  `.long` has a representation-independent
     * raw runtime ABI, so lower it before that redirection; otherwise the
     * generated wrapper can preserve the receiver instead of performing the
     * conversion when the receiver is a compact native list. */
    if (strcmp(e->as.member.name, "long") == 0) {
      int target = ny_native_nir_lower_expr(b, e->as.member.target);
      if (target < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_long", target, -1, -1, 1,
                                             0);
    }
    if (strcmp(e->as.member.name, "len") == 0) {
      if (e->as.member.target->kind == NY_E_LIST ||
          e->as.member.target->kind == NY_E_TUPLE)
        return ny_native_nir_emit_const(
            b, (int64_t)e->as.member.target->as.list_like.len);
      int target = ny_native_nir_lower_expr(b, e->as.member.target);
      if (target < 0)
        return -1;
      /* Typed/native strings use the raw C-string ABI.  The generic length
       * helper intentionally also understands buffers and tagged containers,
       * but its object probe can reject a headerless string constant that has
       * crossed a typed boundary. */
      if (ny_native_nir_expr_is_cstr(b, e->as.member.target))
        return ny_native_nir_emit_runtime_call(b, "rt_cstr_len", target,
                                               -1, -1, 1, 0);
      if (ny_native_nir_expr_is_any(b, e->as.member.target) ||
          (e->as.member.target->kind == NY_E_IDENT &&
           !ny_native_nir_expr_is_list(b, e->as.member.target) &&
           !ny_native_nir_expr_is_bytes(b, e->as.member.target) &&
           !ny_native_nir_expr_is_range(b, e->as.member.target)))
        return ny_native_nir_emit_runtime_call(b, "rt_len_strict", target,
                                               -1, -1, 1, 0);
      return ny_native_nir_emit_runtime_call(b, "rt_len", target, -1, -1,
                                             1, 0);
    }
    /* A typed vector is a dynamic dictionary with a stable field contract.
     * Read its coordinates directly from that representation instead of
     * routing through an unqualified helper call; the latter loses the
     * receiver's value/length/tag facts across imported module boundaries.
     * This is driven by the resolved type, not by a stdlib symbol spelling. */
    const char *target_type = ny_native_nir_expr_type_name(b, e->as.member.target);
    if ((!target_type || strcmp(target_type, "any") == 0) &&
        e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name) {
      const ny_native_nir_local_t *target_local = ny_native_nir_find_local(
          b, e->as.member.target->as.ident.name);
      if (target_local)
        target_type = target_local->type_name;
      if (!target_type) {
        const expr_t *global = ny_native_nir_find_top_level_value(
            b, e->as.member.target->as.ident.name);
        if (global && global != e->as.member.target)
          target_type = ny_native_nir_expr_type_name(b, global);
      }
    }
    if ((!target_type || strcmp(target_type, "any") == 0) &&
        e->as.member.target->kind == NY_E_CALL &&
        e->as.member.target->as.call.callee &&
        e->as.member.target->as.call.callee->kind == NY_E_IDENT &&
        e->as.member.target->as.call.callee->as.ident.name && b->prog) {
      const char *ctor_name =
          e->as.member.target->as.call.callee->as.ident.name;
      const stmt_t *ctor_layout = NULL;
      for (size_t li = 0; b->prog && li < b->prog->body.len && !ctor_layout;
           ++li)
        ctor_layout = ny_native_nir_find_layout_stmt(b->prog->body.data[li],
                                                     ctor_name);
      if (ctor_layout && (ctor_layout->kind == NY_S_STRUCT ||
                          ctor_layout->kind == NY_S_LAYOUT))
        target_type = ctor_layout->kind == NY_S_STRUCT
                          ? ctor_layout->as.struc.name
                          : ctor_layout->as.layout.name;
    }
    bool typed_vector = target_type &&
                        (strcmp(target_type, "vec2") == 0 ||
                         strcmp(target_type, "vec3") == 0 ||
                         strcmp(target_type, "vec4") == 0 ||
                         strcmp(target_type, "Vector2") == 0 ||
                         strcmp(target_type, "Vector3") == 0 ||
                         strcmp(target_type, "Vector4") == 0);
    if (typed_vector && (strcmp(e->as.member.name, "x") == 0 ||
                         strcmp(e->as.member.name, "y") == 0 ||
                         strcmp(e->as.member.name, "z") == 0 ||
                         strcmp(e->as.member.name, "w") == 0)) {
      int target = ny_native_nir_lower_expr(b, e->as.member.target);
      int key = ny_native_nir_emit_const_cstr(b, e->as.member.name);
      int fallback = ny_native_nir_emit_const(b, 0);
      if (target < 0 || key < 0 || fallback < 0)
        return -1;
      int boxed = ny_native_nir_emit_runtime_call(
          b, "rt_dict_get_str_raw", target, key, fallback, 3, 0);
      return boxed < 0 ? -1 : ny_native_nir_emit_runtime_call(
                                 b, "rt_any_to_f64", boxed, -1, -1, 1,
                                 NYIR_INST_F_RET_F64);
    }
    /* User-defined layouts are raw, owned records rather than dictionaries.
     * Their constructor returns the data pointer, so resolve a member from
     * the declaration and load it at the declared byte offset.  Falling
     * through to rt_dict_get_str_raw made a valid `Vec2(...).x` look like a
     * dynamic record and discarded the field value. */
    if (target_type && e->as.member.name) {
      const stmt_t *layout = NULL;
      if (b->prog) {
        for (size_t li = 0; li < b->prog->body.len && !layout; ++li)
          layout = ny_native_nir_find_layout_stmt(b->prog->body.data[li],
                                                  target_type);
      }
      if (layout &&
          (layout->kind == NY_S_STRUCT || layout->kind == NY_S_LAYOUT)) {
        const ny_layout_field_list *fields =
            layout->kind == NY_S_STRUCT ? &layout->as.struc.fields
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
          if (!ny_native_nir_ast_layout_query(
                  b, target_type, field->name, &field_size, &field_align,
                  &field_offset)) {
            ny_native_nir_fail(b, "native member '%s.%s' has no layout",
                               target_type, field->name);
            return -1;
          }
          int target = ny_native_nir_lower_expr(b, e->as.member.target);
          int offset = ny_native_nir_emit_const(b, (int64_t)field_offset);
          int address = target < 0 || offset < 0
                            ? -1
                            : ny_native_nir_emit_add_i64(b, target, offset);
          if (address < 0)
            return -1;
          if (ny_native_type_name_is_f64(field->type_name))
            return ny_native_nir_emit_load_f64(b, address);
          if (ny_native_type_name_is_f32(field->type_name))
            return ny_native_nir_emit_runtime_call(
                b, "rt_load32_f64", target, offset, -1, 2,
                NYIR_INST_F_RET_F64);
          if (field_size <= 4) {
            int loaded = ny_native_nir_emit_runtime_call(
                b, field_size == 1 ? "rt_load8_idx"
                   : field_size == 2 ? "rt_load16_idx"
                                     : "rt_load32_idx",
                target, offset, -1, 2, 0);
            return loaded < 0
                       ? -1
                       : ny_native_nir_emit_runtime_call(b, "rt_any_to_i64",
                                                         loaded, -1, -1, 1, 0);
          }
          return ny_native_nir_emit_load_i64(b, address);
        }
      }
    }
    /* An unresolved/dynamic receiver may still be a vector dictionary.  The
     * attached `x/y/z/w` helpers index through the dynamic container ABI and
     * can lose the third field; direct field lookup preserves the boxed value
     * and also remains correct for ordinary record dictionaries. */
    bool dynamic_record = !target_type || strcmp(target_type, "any") == 0 ||
                          ny_native_nir_expr_is_any(b, e->as.member.target);
    if (dynamic_record && (strcmp(e->as.member.name, "x") == 0 ||
                           strcmp(e->as.member.name, "y") == 0 ||
                           strcmp(e->as.member.name, "z") == 0 ||
                           strcmp(e->as.member.name, "w") == 0)) {
      int target = ny_native_nir_lower_expr(b, e->as.member.target);
      int key = ny_native_nir_emit_const_cstr(b, e->as.member.name);
      int fallback = ny_native_nir_emit_const(b, 0);
      if (target < 0 || key < 0 || fallback < 0)
        return -1;
      return ny_native_nir_emit_runtime_call(b, "rt_dict_get_str_raw", target,
                                             key, fallback, 3, 0);
    }
    if (e->semantic.member_call_kind == NY_SEM_CALL_DYNAMIC_CONTRACT ||
        e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED ||
        ny_native_nir_find_attached_method(b, e->as.member.target,
                                           e->as.member.name) != NULL) {
      const char *canonical = e->semantic.canonical_callee;
      if (!canonical && e->semantic.canonical_callee_stmt)
        canonical = e->semantic.canonical_callee_stmt->as.fn.name;
      const stmt_t *attached_method = ny_native_nir_find_attached_method(
          b, e->as.member.target, e->as.member.name);
      if (attached_method && attached_method->as.fn.name)
        canonical = attached_method->as.fn.name;
      if (!canonical) {
        ny_native_nir_fail(
            b,
            "native NYIR lower: semantic property target is missing for '%s'",
            e->as.member.name);
        return -1;
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
      return ny_native_nir_lower_expr(b, &call);
    }
    char mem_qname[128] = {0};
    if (e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name && e->as.member.name) {
      snprintf(mem_qname, sizeof(mem_qname), "%s.%s",
               e->as.member.target->as.ident.name, e->as.member.name);
      const stmt_t *adt_enum = NULL;
      const stmt_enum_item_t *adt_item = NULL;
      int64_t adt_tag = 0;
      if (ny_native_nir_find_enum_member(b, mem_qname, &adt_enum, &adt_item,
                                         &adt_tag)) {
        if (adt_item && adt_item->fields.len == 0) {
          return ny_native_nir_emit_const(b, adt_tag);
        }
      }
    }
    /* Resolve the fully qualified module member before consulting the
     * process-wide leaf-name constant table.  Re-export facades commonly
     * repeat names (for example vk.compute.FLAG = compute.FLAG); a tail-only
     * lookup can select an unrelated or not-yet-folded definition. */
    const expr_t *resolved_member = ny_native_nir_resolve_member_expr(b, e);
    if (resolved_member && resolved_member != e)
      return ny_native_nir_lower_expr(b, resolved_member);
    /* Complete stdlib collection can omit the facade module from the direct
     * module table while retaining its root `use` declaration.  Resolve the
     * exported leaf through the import graph before falling back to the
     * process-wide tail constant table; otherwise a valid integer flag is
     * lowered as a missing dictionary member (zero). */
    const expr_t *imported_member =
        ny_native_nir_find_imported_value(b, e->as.member.name);
    if (imported_member && imported_member != e)
      return ny_native_nir_lower_expr(b, imported_member);
    int64_t constant = 0;
    bool constant_found = ny_native_consttab_get(e->as.member.name, &constant);
    if (!constant_found)
      constant_found =
          ny_native_consttab_get_tail(e->as.member.name, &constant);
    if (ny_trace_enabled("NY_TRACE_CONSTTAB"))
      fprintf(stderr, "native const lookup %s found=%d value=%lld\n",
              e->as.member.name, constant_found ? 1 : 0, (long long)constant);
    if (constant_found)
      return ny_native_nir_emit_const(b, constant);
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
        return ny_native_nir_emit_const(b, (int64_t)list->as.list_like.len);
      int target = ny_native_nir_lower_expr(b, e->as.member.target);
      /* Only use the compile-time length fact for non-identifier expressions.
       * Any identifier binding may alias a mutated list; always read from
       * memory to avoid stale cached lengths after clear()/append(). */
      bool target_is_var = (e->as.member.target->kind == NY_E_IDENT);
      int length =
          target_is_var ? -1 : ny_native_nir_peek_list_len_fact(b, target);
      if (target >= 0 && length >= 0)
        return length;
      if (target >= 0 &&
          (ny_native_nir_expr_is_bytes(b, e->as.member.target) ||
           (e->as.member.target->kind == NY_E_IDENT &&
            ny_native_nir_find_local(b, e->as.member.target->as.ident.name) &&
            ny_native_nir_find_local(b, e->as.member.target->as.ident.name)
                ->is_bytes)))
        return ny_native_nir_emit_runtime_call(b, "rt_len", target, -1,
                                               -1, 1, 0);
      if (target >= 0 && ny_native_nir_expr_is_cstr(b, e->as.member.target)) {
        int dynamic_length = ny_native_nir_peek_dyn_fact(
            b, target, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
        if (dynamic_length >= 0)
          return dynamic_length;
        return ny_native_nir_emit_runtime_call(b, "rt_cstr_len", target,
                                               -1, -1, 1, 0);
      }
      if (target >= 0 && ny_native_nir_expr_is_list(b, e->as.member.target))
        return ny_native_nir_emit_runtime_call(b, "rt_tbuf_len_raw", target,
                                               -1, -1, 1, 0);
      if (target >= 0)
        return ny_native_nir_emit_runtime_call(b, "rt_len", target, -1,
                                               -1, 1, 0);
    }
    const stmt_t *method_fn =
        ny_native_nir_find_user_function(b, e->as.member.name);
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
      return ny_native_nir_lower_expr(b, &call);
    }
    int target_val = ny_native_nir_lower_expr(b, e->as.member.target);
    if (target_val >= 0) {
      int key_const = ny_native_nir_emit_const_cstr(b, e->as.member.name);
      int fallback = ny_native_nir_emit_const(b, 0);
      if (key_const >= 0 && fallback >= 0) {
        int value = ny_native_nir_emit_runtime_call(
            b, "rt_dict_get_str_raw", target_val, key_const, fallback, 3, 0);
        /* Dictionary fields are stored in the canonical tagged-value ABI.
         * A property expression is a value boundary, so decode a tagged
         * scalar exactly once while leaving handles (strings, buffers,
         * nested objects) unchanged. */
        return value < 0 ? -1
                         : ny_native_nir_emit_runtime_call(
                               b, "rt_any_to_i64", value, -1, -1, 1, 0);
      }
    }
    ny_native_nir_fail(
        b,
        "native NYIR lower: member access '%s' has unsupported semantic "
        "representation %d (resolved=%d, call=%d, callee=%s) in %s at %s:%d",
        e->as.member.name, (int)e->as.member.target->semantic.rep,
        e->as.member.target->semantic.resolved ? 1 : 0,
        (int)e->semantic.member_call_kind,
        e->semantic.canonical_callee ? e->semantic.canonical_callee : "<none>",
        b->current_fn_name ? b->current_fn_name : "<unknown>",
        e->tok.filename ? e->tok.filename : "<source>", e->tok.line);
    return -1;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    ny_native_lambda_entry_t *lambda = ny_native_lambda_find(e);
    if (!lambda || !lambda->name) {
      ny_native_nir_fail(b, "native NYIR lower: closure was not collected");
      return -1;
    }
    /* Address-taking uses the same linkage name as ordinary function values.
     * Keep this in shared lowering so assembly, object, and LLVM emission
     * consume one symbol instead of repairing generated names independently. */
    char function_symbol[512];
    int symbol_len = snprintf(function_symbol, sizeof(function_symbol),
                              "ny_fn_%s", lambda->name);
    if (symbol_len < 0 || (size_t)symbol_len >= sizeof(function_symbol)) {
      ny_native_nir_fail(b, "native NYIR lower: function symbol too long");
      return -1;
    }
    /* Capture only names that are locals of the enclosing native frame.  The
     * collector may have seen the lambda before this frame was lowered, so
     * complete the precise capture set at the value-creation site. */
    for (size_t i = 0; i < b->local_count; ++i) {
      ny_native_nir_local_t *outer = &b->locals[i];
      if (!outer->name || ny_native_lambda_param_named(e, outer->name) ||
          !ny_native_nir_stmt_uses_ident(e->as.lambda.body, outer->name))
        continue;
      if (!ny_native_lambda_capture_add(lambda, outer->name))
        return ny_native_nir_fail(b,
                                  "native NYIR: closure capture table is full");
    }
    if (lambda->capture_count > 0) {
      int count =
          ny_native_nir_emit_const(b, (int64_t)lambda->capture_count * 8);
      int env = count < 0 ? -1
                          : ny_native_nir_emit_runtime_call(
                                b, "rt_malloc", count, -1, -1, 1, 0);
      if (env < 0)
        return -1;
      for (size_t i = 0; i < lambda->capture_count; ++i) {
        ny_native_nir_local_t *outer =
            ny_native_nir_find_local(b, lambda->capture_names[i]);
        if (outer) {
          lambda->capture_types[i] = *outer;
        } else {
          memset(&lambda->capture_types[i], 0,
                 sizeof(lambda->capture_types[i]));
          const expr_t *top =
              ny_native_nir_find_top_level_value(b, lambda->capture_names[i]);
          if (top) {
            lambda->capture_types[i].is_list =
                ny_native_nir_expr_is_list(b, top);
            lambda->capture_types[i].is_dyn_list =
                ny_native_nir_expr_is_dyn_list(b, top);
            lambda->capture_types[i].is_cstr =
                ny_native_nir_expr_is_cstr(b, (expr_t *)top);
            lambda->capture_types[i].is_bytes =
                ny_native_nir_expr_is_bytes(b, top);
            lambda->capture_types[i].is_dict =
                ny_native_nir_expr_is_dict(b, top);
            lambda->capture_types[i].is_any =
                ny_native_nir_expr_is_any(b, (expr_t *)top);
            lambda->capture_types[i].is_f64 = ny_native_nir_expr_is_f64(b, top);
          }
        }
        int value = outer ? ny_native_nir_load_local_value(b, outer->slot) : -1;
        int off = value < 0 ? -1 : ny_native_nir_emit_const(b, (int64_t)i * 8);
        int addr =
            value < 0 || off < 0 ? -1 : ny_native_nir_emit_add_i64(b, env, off);
        if (addr < 0 || !ny_native_nir_emit_store_i64(b, addr, value))
          return -1;
      }
      int cls_size = ny_native_nir_emit_const(b, 16);
      int closure = cls_size < 0 ? -1
                                 : ny_native_nir_emit_runtime_call(
                                       b, "rt_malloc", cls_size, -1, -1, 1, 0);
      int code =
          closure < 0
              ? -1
              : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                  .dst = -1,
                                                  .a = -1,
                                                  .b = -1,
                                                  .imm = 0,
                                                  .symbol = function_symbol});
      int tag_off = closure < 0 ? -1 : ny_native_nir_emit_const(b, -8);
      int tag_addr = closure < 0 || tag_off < 0
                         ? -1
                         : ny_native_nir_emit_add_i64(b, closure, tag_off);
      int env_off = closure < 0 ? -1 : ny_native_nir_emit_const(b, 8);
      int env_addr = closure < 0 || env_off < 0
                         ? -1
                         : ny_native_nir_emit_add_i64(b, closure, env_off);
      int tag = closure < 0 ? -1 : ny_native_nir_emit_const(b, TAG_CLOSURE);
      if (closure < 0 || code < 0 || tag_addr < 0 || env_addr < 0 || tag < 0 ||
          !ny_native_nir_emit_store_i64(b, tag_addr, tag) ||
          !ny_native_nir_emit_store_i64(b, closure, code) ||
          !ny_native_nir_emit_store_i64(b, env_addr, env))
        return -1;
      return closure;
    }
    int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .imm = 0,
                                                 .symbol = function_symbol});
    if (addr < 0)
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
    return addr;
  }
  default:
    ny_native_nir_fail(
        b,
        "native NYIR lower: expression kind %d ('%.*s') at %s:%d in %s is not "
        "in shared NYIR yet",
        (int)e->kind, (int)e->tok.len, e->tok.lexeme ? e->tok.lexeme : "",
        e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
        b->current_fn_name ? b->current_fn_name : "<unknown>");
    return -1;
  }
}

static int ny_native_nir_lower_expr(ny_native_nir_builder_t *b,
                                    const expr_t *e) {
  if (!b)
    return -1;
  size_t debug_start = b ? b->nyir.len : 0;
  ny_expr_semantic_t semantic = e ? e->semantic : (ny_expr_semantic_t){0};
  if (e && !semantic.resolved && e->kind == NY_E_IDENT && e->as.ident.name) {
    ny_native_nir_local_t *local =
        ny_native_nir_find_local_ctx(b, e->as.ident.name, e->as.ident.syntax_ctx);
    if (local && local->semantic_rep != NY_SEM_REP_UNKNOWN) {
      semantic.rep = local->semantic_rep;
      semantic.ownership = local->semantic_ownership;
      semantic.alias_class = local->alias_class;
      semantic.mutable_value = local->semantic_mutable;
      semantic.resolved = true;
    } else {
      const expr_t *global =
          ny_native_nir_find_top_level_value(b, e->as.ident.name);
      if (!global && b->current_fn_name) {
        const char *dot = strrchr(b->current_fn_name, '.');
        if (dot && dot != b->current_fn_name) {
          char qualified[512];
          int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                           (int)(dot - b->current_fn_name), b->current_fn_name,
                           e->as.ident.name);
          if (n > 0 && (size_t)n < sizeof(qualified))
            global = ny_native_nir_find_top_level_value(b, qualified);
        }
      }
      if (global && global->semantic.resolved)
        semantic = global->semantic;
    }
  } else if (e && !semantic.resolved && e->kind == NY_E_CALL &&
             e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name) {
    const stmt_t *callee_fn =
        ny_native_nir_find_user_function(b, e->as.call.callee->as.ident.name);
    if (callee_fn && callee_fn->kind == NY_S_FUNC) {
      if (callee_fn->as.fn.return_semantic.resolved) {
        semantic = callee_fn->as.fn.return_semantic;
      } else if (callee_fn->as.fn.return_type) {
        const char *rt = callee_fn->as.fn.return_type;
        if (ny_native_type_name_is_f64(rt))
          semantic.rep = NY_SEM_REP_F64;
        else if (ny_native_type_name_is_f32(rt))
          semantic.rep = NY_SEM_REP_F32;
        else if (ny_native_type_name_is_str(rt))
          semantic.rep = NY_SEM_REP_STRING;
        else if (ny_native_type_name_is_list(rt))
          semantic.rep = NY_SEM_REP_TYPED_BUFFER;
        else if (ny_native_type_name_is_any(rt))
          semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
        else
          semantic.rep = NY_SEM_REP_RAW_INT;
        semantic.resolved = true;
      }
    }
  }
  if (!semantic.resolved) {
    semantic.rep = NY_SEM_REP_TAGGED_DYNAMIC;
    semantic.ownership = NY_SEM_OWN_UNKNOWN;
    semantic.resolved = true;
  }
  int value = ny_native_nir_lower_expr_impl(b, e);
  /* Keep source provenance on the NYIR itself.  The lowering helpers emit
   * several instructions per expression, so annotate the whole newly-emitted
   * range while retaining precise locations already assigned by nested
   * expressions.  This is consumed by verifier/dump diagnostics and is the
   * source map used by native debug backends. */
  if (b && e && e->tok.line > 0) {
    const char *file = e->tok.filename ? e->tok.filename : b->source_file;
    for (size_t i = debug_start; i < b->nyir.len; ++i) {
      nyir_inst_t *inst = &b->nyir.data[i];
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
    nyir_inst_t *inst = &b->nyir.data[i - 1];
    if (inst->dst != value)
      continue;
    inst->semantic_rep = (uint8_t)semantic.rep;
    inst->semantic_ownership = (uint8_t)semantic.ownership;
    inst->semantic_mutable = semantic.mutable_value;
    inst->alias_class = semantic.alias_class;
    break;
  }
  return value;
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left, const expr_t *right,
                                       bool is_or) {
  if (left && left->kind == NY_E_LITERAL) {
    if (left->as.literal.kind == NY_LIT_INT) {
      int64_t val = left->as.literal.as.i;
      if (is_or) {
        if (val != 0)
          return ny_native_nir_emit_const(b, 1);
        return ny_native_nir_lower_expr(b, right);
      } else {
        if (val == 0)
          return ny_native_nir_emit_const(b, 0);
        return ny_native_nir_lower_expr(b, right);
      }
    } else if (left->as.literal.kind == NY_LIT_BOOL) {
      bool val = left->as.literal.as.b;
      if (is_or) {
        if (val)
          return ny_native_nir_emit_const(b, 1);
        return ny_native_nir_lower_expr(b, right);
      } else {
        if (!val)
          return ny_native_nir_emit_const(b, 0);
        return ny_native_nir_lower_expr(b, right);
      }
    }
  }

  int result_slot = ny_native_nir_temp_slot(b);
  int zero = ny_native_nir_emit_const(b, 0);
  int one = ny_native_nir_emit_const(b, 1);
  if (zero < 0 || one < 0)
    return -1;
  int true_label = b->next_label++;
  int end_label = b->next_label++;

  if (!ny_native_nir_store_local_value(b, result_slot, zero))
    return -1;

  int lhs = ny_native_nir_lower_expr(b, left);
  if (lhs < 0)
    return -1;

  if (is_or) {
    if (!ny_native_nir_emit_br_if(b, lhs, true_label))
      return -1;
  } else {
    int lhs_zero = ny_native_nir_emit_is_zero(b, lhs);
    if (lhs_zero < 0 || !ny_native_nir_emit_br_if(b, lhs_zero, end_label))
      return -1;
  }

  int rhs = ny_native_nir_lower_expr(b, right);
  if (rhs < 0)
    return -1;

  if (is_or) {
    if (!ny_native_nir_emit_br_if(b, rhs, true_label) ||
        !ny_native_nir_emit_br(b, end_label))
      return -1;
  } else {
    int rhs_zero = ny_native_nir_emit_is_zero(b, rhs);
    if (rhs_zero < 0 || !ny_native_nir_emit_br_if(b, rhs_zero, end_label))
      return -1;
  }

  if (!ny_native_nir_emit_label(b, true_label) ||
      !ny_native_nir_store_local_value(b, result_slot, one) ||
      !ny_native_nir_emit_br(b, end_label))
    return -1;
  if (!ny_native_nir_emit_label(b, end_label))
    return -1;
  return ny_native_nir_load_local_value(b, result_slot);
}

static int ny_native_nir_lower_ternary(ny_native_nir_builder_t *b,
                                       const expr_t *cond,
                                       const expr_t *true_expr,
                                       const expr_t *false_expr) {
  if (!cond || !true_expr || !false_expr) {
    ny_native_nir_fail(b, "native NYIR lower: malformed ternary expression");
    return -1;
  }
  if (cond->kind == NY_E_LITERAL) {
    if (cond->as.literal.kind == NY_LIT_INT) {
      return cond->as.literal.as.i != 0
                 ? ny_native_nir_lower_expr(b, true_expr)
                 : ny_native_nir_lower_expr(b, false_expr);
    }
    if (cond->as.literal.kind == NY_LIT_BOOL) {
      return cond->as.literal.as.b ? ny_native_nir_lower_expr(b, true_expr)
                                   : ny_native_nir_lower_expr(b, false_expr);
    }
  }
  int result_slot = ny_native_nir_temp_slot(b);
  int true_label = b->next_label++;
  int else_label = b->next_label++;
  int end_label = b->next_label++;

  int cond_val = ny_native_nir_lower_expr(b, cond);
  if (cond_val < 0)
    return -1;
  if (!ny_native_nir_emit_br_if(b, cond_val, true_label) ||
      !ny_native_nir_emit_br(b, else_label) ||
      !ny_native_nir_emit_label(b, true_label))
    return -1;

  int true_val = ny_native_nir_lower_expr(b, true_expr);
  if (true_val < 0 ||
      !ny_native_nir_store_local_value(b, result_slot, true_val) ||
      !ny_native_nir_emit_br(b, end_label))
    return -1;

  if (!ny_native_nir_emit_label(b, else_label))
    return -1;
  int false_val = ny_native_nir_lower_expr(b, false_expr);
  if (false_val < 0 ||
      !ny_native_nir_store_local_value(b, result_slot, false_val) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;

  return ny_native_nir_load_local_value(b, result_slot);
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b,
                                     const stmt_t *s);
