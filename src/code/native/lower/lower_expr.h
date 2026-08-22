static int ny_native_nir_lower_expr(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e) {
    ny_native_nir_fail(b, "native NYIR lower: missing expression");
    return -1;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL)
      return ny_native_nir_emit_const(b, e->as.literal.as.b ? 1 : 0);
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return ny_native_nir_emit_const_f64(b, e->as.literal.as.f);
    if (e->tok.kind == NY_T_NIL)
      return ny_native_nir_emit_const(b, 0);
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
      int tag = ny_native_nir_emit_const(b, 121);
      if (length >= 0 && tag >= 0) {
        ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_STR_LEN, length);
        ny_native_nir_record_dyn_fact(b, addr, NY_NATIVE_NIR_FACT_DYN_TAG, tag);
      }
      return addr;
    }
    if (e->as.literal.kind != NY_LIT_INT) {
      ny_native_nir_fail(
          b, "native NYIR lower: only int/bool/f64/nil/string literals are supported");
      return -1;
    }
    return ny_native_nir_emit_const(b, e->as.literal.as.i);
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (!l) {
      const expr_t *global =
          ny_native_nir_find_top_level_value(b, e->as.ident.name);
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
                           (int)(dot - b->current_fn_name),
                           b->current_fn_name, e->as.ident.name);
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
      char function_symbol[512];
      const char *symbol = e->as.ident.name;
      bool user_function =
          ny_native_nir_user_defined_fn(b, e->as.ident.name);
      if (user_function) {
        int n = snprintf(function_symbol, sizeof(function_symbol), "ny_fn_%s",
                         e->as.ident.name);
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
    l->sb_candidate = false;
    return ny_native_nir_load_local_value(b, l->slot);
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right) {
      ny_native_nir_fail(b, "native NYIR lower: malformed unary");
      return -1;
    }
    int rv = ny_native_nir_lower_expr(b, e->as.unary.right);
    if (rv < 0)
      return -1;
    if (strcmp(e->as.unary.op, "+") == 0)
      return rv;
    if (strcmp(e->as.unary.op, "-") == 0) {
      if (ny_native_nir_expr_is_f32(b, e->as.unary.right)) {
        int zero = ny_native_nir_emit_const_f32(b, 0.0f);
        if (zero < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_F32,
                                                    .dst = -1, .a = zero,
                                                    .b = rv});
      }
      if (ny_native_nir_expr_is_f64(b, e->as.unary.right)) {
        int zero = ny_native_nir_emit_const_f64(b, 0.0);
        if (zero < 0)
          return -1;
        return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_F64,
                                                    .dst = -1, .a = zero,
                                                    .b = rv});
      }
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_SUB_I64,
                                                  .dst = -1, .a = zero,
                                                  .b = rv});
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
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = zero,
                                                  .cmp = NYIR_CMP_EQ});
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      int mask = ny_native_nir_emit_const(b, -1);
      if (mask < 0)
        return -1;
      return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_XOR_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = mask});
    }
    ny_native_nir_fail(b, "native NYIR lower: unsupported unary operator '%s'",
                       e->as.unary.op);
    return -1;
  }
  case NY_E_BINARY:
    return ny_native_nir_lower_binary(b, e);
  case NY_E_LOGICAL: {
    if (!e->as.logical.op ||
        (strcmp(e->as.logical.op, "&&") != 0 &&
         strcmp(e->as.logical.op, "||") != 0)) {
      ny_native_nir_fail(b, "native NYIR lower: unsupported logical operator '%s'",
                         e->as.logical.op ? e->as.logical.op : "(null)");
      return -1;
    }
    return ny_native_nir_lower_logical(
        b, e->as.logical.left, e->as.logical.right,
        strcmp(e->as.logical.op, "||") == 0);
  }
  case NY_E_TERNARY:
    return ny_native_nir_lower_ternary(b, e->as.ternary.cond,
                                       e->as.ternary.true_expr,
                                       e->as.ternary.false_expr);
  case NY_E_COMPTIME: {
    bool value = false;
    if (ny_native_target_eval_bool(NULL, e, &value))
      return ny_native_nir_emit_const(b, value ? 1 : 0);
    ny_native_nir_fail(b, "native NYIR lower: unsupported compile-time expression");
    return -1;
  }
  case NY_E_MATCH: {
    stmt_t match = {.kind = NY_S_MATCH, .tok = e->tok};
    match.as.match = e->as.match;
    if (!ny_native_nir_lower_match(b, &match))
      return -1;
    return b->last_value;
  }
  case NY_E_CALL:
    return ny_native_nir_lower_call(b, e);
  case NY_E_DEREF: {
    int addr = ny_native_nir_lower_expr(b, e->as.deref.target);
    if (addr < 0)
      return -1;
    int v = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                 .dst = -1,
                                                 .a = addr,
                                                 .b = -1,
                                                 .c = -1});
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
          strcmp(tpl, "xorl %eax,%eax") == 0 || strcmp(tpl, "xor %%eax,%%eax") == 0 ||
          strcmp(tpl, "xor %rax,%rax") == 0 || strcmp(tpl, "xorq %rax,%rax") == 0)
        return ny_native_nir_emit_const(b, 0);
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
      int c = a < 0 ? -1 : ny_native_nir_lower_expr(b, e->as.as_asm.args.data[1]);
      if (a < 0 || c < 0)
        return -1;
      return ny_native_asm_emit_binop(b,
          strncmp(tpl, "or ", 3) == 0 ? NYIR_OR_I64 : NYIR_ADD_I64, a, c);
    }
    return ny_native_nir_lower_aarch64_asm(b, e);
  }
  case NY_E_FSTRING: {
    /*
     * F-string: concatenate static parts + interpolated values into a
     * runtime C string via rt_native_cstr_concat.  Static-only f-strings
     * fold into a single interned literal.
     * NOTE: concat chains allocate intermediate C strings via
     * rt_native_cstr_concat and only the final pointer survives to the
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
        const char *data = parts->data[i].as.s.data
                               ? parts->data[i].as.s.data
                               : "";
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
        const char *s = parts->data[i].as.s.data
                            ? parts->data[i].as.s.data
                            : "";
        const char *sym =
            ny_native_strtab_intern(s, parts->data[i].as.s.len, NULL, 0);
        if (!sym) {
          ny_native_nir_fail(b, "native NYIR lower: string table full or OOM");
          return -1;
        }
        part_val = nyir_emit(&b->nyir,
                             (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
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
          part_val = ny_native_nir_emit_runtime_call(
              b, "rt_native_any_to_cstr", part_val, -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else if (ny_native_nir_expr_is_bool(b, sub)) {
          part_val = ny_native_nir_emit_runtime_call(
              b, "rt_native_bool_to_cstr", part_val, -1, -1, 1, 0);
          if (part_val < 0)
            return -1;
        } else {
          /*
           * Raw i64 (matches print's is_cstr ? cstr : i64 convention).
           */
          part_val = ny_native_nir_emit_runtime_call(
              b, "rt_native_i64_to_cstr", part_val, -1, -1, 1, 0);
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
      acc = ny_native_nir_emit_runtime_call(b, "rt_native_cstr_concat", acc,
                                            part_val, -1, 2, 0);
      if (acc < 0)
        return -1;
    }
    return acc;
  }
  case NY_E_DICT: {
    size_t count = e->as.dict.pairs.len;
    int capacity = ny_native_nir_emit_const(
        b, (int64_t)(count < 4 ? 8 : count * 2));
    if (capacity < 0)
      return -1;
    int dict = ny_native_nir_emit_runtime_call(
        b, "rt_native_dict_new", capacity, -1, -1, 1, 0);
    if (dict < 0)
      return -1;
    for (size_t i = 0; i < count; ++i) {
      const expr_t *key = e->as.dict.pairs.data[i].key;
      const expr_t *value = e->as.dict.pairs.data[i].value;
      int key_reg = ny_native_nir_lower_expr(b, key);
      int value_reg = ny_native_nir_lower_expr(b, value);
      if (key_reg < 0 || value_reg < 0)
        return -1;
      dict = ny_native_nir_emit_runtime_call(
          b, "rt_native_dict_set", dict, key_reg, value_reg, 3, 0);
      if (dict < 0)
        return -1;
    }
    return dict;
  }
  case NY_E_LIST: {
    /*
     * Constant list literal → pooled .data array.  The value is the array
     * address, so indexing lowers to address math + load.  Non-constant
     * list construction is not supported by shared NYIR yet.
     */
    size_t count = e->as.list_like.len;
    if (count > 128) {
      ny_native_nir_fail(
          b, "native NYIR lower: list literal must have at most 128 elements");
      return -1;
    }
    if (count == 0) {
      int n = ny_native_nir_emit_const(b, 0);
      int width = ny_native_nir_emit_const(
          b, b->current_list_elem_size > 0 ? b->current_list_elem_size : 24);
      int base = n < 0 || width < 0
                     ? -1
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_native_tbuf_new", n, width, -1, 2, 0);
      int length = n < 0 ? -1 : ny_native_nir_emit_const(b, 0);
      if (base < 0 || length < 0 ||
          !ny_native_nir_record_list_len_fact(b, base, length))
        return -1;
      return base;
    }
    bool all_constant = true;
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
    }
    if (!all_constant) {
      bool all_f64 = true;
      for (size_t i = 0; i < count; ++i) {
        if (!ny_native_nir_expr_is_f64(b, e->as.list_like.data[i])) {
          all_f64 = false;
          break;
        }
      }
      if (all_f64) {
        int n = ny_native_nir_emit_const(b, (int64_t)count);
        int width = ny_native_nir_emit_const(b, 8);
        int base = n < 0 || width < 0
                       ? -1
                       : ny_native_nir_emit_runtime_call(
                             b, "rt_native_tbuf_new", n, width, -1, 2, 0);
        if (base < 0)
          return -1;
        for (size_t i = 0; i < count; ++i) {
          int value = ny_native_nir_lower_expr(b, e->as.list_like.data[i]);
          int off = ny_native_nir_emit_const(b, (int64_t)(i * 8));
          int slot = value < 0 || off < 0
                         ? -1
                         : ny_native_nir_emit_add_i64(b, base, off);
          if (slot < 0 || !ny_native_nir_emit_store_f64(b, slot, value))
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
                     : ny_native_nir_emit_runtime_call(
                           b, "rt_native_tbuf_new", n, width, -1, 2, 0);
      if (base < 0)
        return -1;
      for (size_t i = 0; i < count; ++i) {
        const expr_t *el = e->as.list_like.data[i];
        int value = ny_native_nir_lower_expr(b, el);
        int index = ny_native_nir_emit_const(b, (int64_t)i);
        int stride = ny_native_nir_emit_const(b, 24);
        int off = value < 0 || index < 0 || stride < 0
                      ? -1
                      : ny_native_nir_push_val(b, NYIR_MUL_I64, index, stride,
                                               0, NULL);
        int slot = off < 0 ? -1 : ny_native_nir_emit_add_i64(b, base, off);
        if (slot < 0)
          return -1;
        bool is_string = ny_native_nir_expr_is_cstr(b, el) ||
                         ny_native_nir_expr_is_any(b, el);
        bool is_f64 = ny_native_nir_expr_is_f64(b, el);
        int tag = ny_native_nir_emit_const(b, is_string ? 121 : 3);
        int length = is_string
                         ? ny_native_nir_peek_dyn_fact(
                               b, value, NY_NATIVE_NIR_FACT_DYN_STR_LEN)
                         : ny_native_nir_emit_const(b, 0);
        if (length < 0)
          length = ny_native_nir_emit_const(b, 0);
        int payload = is_string
                          ? ny_native_nir_push_val(b, NYIR_SUB_I64, value, slot,
                                                   0, NULL)
                          : value;
        if (payload < 0 || length < 0 || tag < 0 ||
            !(is_f64 ? ny_native_nir_emit_store_f64(b, slot, payload)
                     : ny_native_nir_emit_store_i64(b, slot, payload)))
          return -1;
        int len_slot = ny_native_nir_emit_add_i64(
            b, slot, ny_native_nir_emit_const(b, 8));
        int tag_slot = ny_native_nir_emit_add_i64(
            b, slot, ny_native_nir_emit_const(b, 16));
        if (len_slot < 0 || tag_slot < 0 ||
            !ny_native_nir_emit_store_i64(b, len_slot, length) ||
            !ny_native_nir_emit_store_i64(b, tag_slot, tag))
          return -1;
      }
      int list_len = ny_native_nir_emit_const(b, (int64_t)count);
      if (list_len < 0 || !ny_native_nir_record_list_len_fact(b, base, list_len))
        return -1;
      ny_native_nir_record_alloc_fact(b, base, (int64_t)(count * 24));
      return base;
    }
    /*
     * Constant list literal → pooled .data array.  The value is the array
     * address, so indexing lowers to address math + load.
     */
    ny_native_array_elem_t values[128] = {0};
    bool string_elements = false;
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
        ny_native_nir_fail(
            b, "native NYIR lower: only constant list literals are supported in shared NYIR");
        return -1;
      }
      if (el->as.literal.kind == NY_LIT_INT) {
        int64_t value = el->as.literal.as.i;
        if (negate) {
          if (value == INT64_MIN) {
            ny_native_nir_fail(
                b, "native NYIR lower: negated list integer overflows i64");
            return -1;
          }
          value = -value;
        }
        values[i].value = value;
      } else if (el->as.literal.kind == NY_LIT_FLOAT) {
        double value = el->as.literal.as.f;
        values[i].value = ny_native_f64_bits(negate ? -value : value);
      } else if (!negate && el->as.literal.kind == NY_LIT_BOOL)
        values[i].value = el->as.literal.as.b ? 1 : 0;
      else if (!negate && el->as.literal.kind == NY_LIT_STR) {
        if (i && !string_elements) { ny_native_nir_fail(b, "native NYIR lower: heterogeneous constant list elements are not supported"); return -1; }
        string_elements = true; values[i].str = el->as.literal.as.s.data; values[i].str_len = el->as.literal.as.s.len;
      } else {
        ny_native_nir_fail(b, "native NYIR lower: unsupported constant list element");
        return -1;
      }
    }
    /*
     * Only homogeneous string lists use dynamic descriptors.  Scalar integer,
     * boolean, and f64 lists remain packed at one 8-byte word per element;
     * widening them to 24 bytes based on a missing f64 inference would make
     * indexing and bounds calculations disagree with the emitted storage.
     */
    size_t stride = string_elements ? 24 : 8;
    const char *sym = ny_native_arraytab_intern(values, count, stride, NULL, 0);
    if (!sym) {
      ny_native_nir_fail(b, "native NYIR lower: constant array table full or OOM");
      return -1;
    }
    int addr = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .imm = 0,
                                                 .symbol = sym});
    if (addr < 0) {
      ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
      return -1;
    }
    int length = ny_native_nir_emit_const(b, (int64_t)count);
    if (length < 0 || !ny_native_nir_record_list_len_fact(b, addr, length))
      return -1;
    if (count <= INT64_MAX / stride)
      ny_native_nir_record_alloc_fact(b, addr, (int64_t)(count * stride));
    return addr;
  }
  case NY_E_INDEX: {
    if (!e->as.index.target || !e->as.index.start || e->as.index.stop ||
        e->as.index.step) {
      ny_native_nir_fail(
          b,
          "native NYIR lower: only single-element list indexing is supported at %s:%d in %s",
          e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
          b->current_fn_name ? b->current_fn_name : "<unknown>");
      return -1;
    }
    /*
     * A one-element temporary indexed by its only constant index does not
     * escape the expression.  Lower the element directly instead of
     * allocating a 24-byte tagged tbuf and immediately loading it back.
     * Keep this strictly syntactic: dynamic indices and larger literals
     * retain the normal bounds-checked representation.
     */
    const expr_t *direct_target =
        ny_native_nir_resolve_list_literal(b, e->as.index.target, 0);
    if (direct_target && direct_target->kind == NY_E_LIST &&
        direct_target->as.list_like.len == 1 &&
        e->as.index.start->kind == NY_E_LITERAL &&
        e->as.index.start->as.literal.kind == NY_LIT_INT &&
        e->as.index.start->as.literal.as.i == 0) {
      if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT)
        return ny_native_nir_lower_expr(b, e->as.index.target);
      return ny_native_nir_lower_expr(b, direct_target->as.list_like.data[0]);
    }
    const expr_t *literal_target =
        ny_native_nir_resolve_list_literal(b, e->as.index.target, 0);
    bool dynamic_elements =
        literal_target
            ? ny_native_nir_expr_is_dyn_list(b, literal_target)
            : ny_native_nir_expr_is_dyn_list(b, e->as.index.target);
    int base = ny_native_nir_lower_expr(b, e->as.index.target);
    int idx = ny_native_nir_lower_expr(b, e->as.index.start);
    if (base < 0 || idx < 0)
      return -1;
    int width = ny_native_nir_emit_const(b, dynamic_elements ? 24 : 8);
    int off = width < 0
                  ? -1
                  : ny_native_nir_push_val(b, NYIR_MUL_I64, idx, width, 0, NULL);
    if (off < 0)
      return -1;
    int64_t byte_len = ny_native_nir_peek_alloc_fact(b, base);
    int length = ny_native_nir_peek_list_len_fact(b, base);
    int dynamic_byte_len = -1;
    if (length >= 0) {
      dynamic_byte_len = ny_native_nir_push_val(
          b, NYIR_MUL_I64, length, width, 0, NULL);
      if (dynamic_byte_len < 0)
        return -1;
    }
    if (!ny_native_nir_emit_bounds_check_value(
            b, base, off, dynamic_byte_len, byte_len))
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
      if (!ny_native_nir_record_dyn_fact(
              b, payload, NY_NATIVE_NIR_FACT_DYN_STR_LEN, len) ||
          !ny_native_nir_record_dyn_fact(
              b, payload, NY_NATIVE_NIR_FACT_DYN_TAG, tag))
        return -1;
      return payload;
    }
    return is_f64 ? ny_native_nir_emit_load_f64(b, addr)
                  : ny_native_nir_emit_load_i64(b, addr);
  }
  case NY_E_MEMCALL: {
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "get") == 0 &&
        e->as.memcall.args.len == 1 &&
        ny_native_nir_expr_is_list(b, e->as.memcall.target) &&
        !ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target)) {
      expr_t index = {.kind = NY_E_INDEX, .tok = e->tok};
      index.as.index.target = e->as.memcall.target;
      index.as.index.start = e->as.memcall.args.data[0].val;
      index.as.index.stop = NULL;
      index.as.index.step = NULL;
      return ny_native_nir_lower_expr(b, &index);
    }
    ny_native_nir_local_t *append_local =
        e->as.memcall.target &&
                e->as.memcall.target->kind == NY_E_IDENT &&
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
      bool value_is_string = ny_native_nir_expr_is_cstr(b, append_value);
      if (list < 0 || value < 0)
        return -1;
      bool scalar_list =
          ny_native_nir_expr_is_list(b, e->as.memcall.target) &&
          !ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target);
      int out = scalar_list
                    ? ny_native_nir_emit_runtime_call(
                          b, "rt_native_tbuf_append_i64", list, value, -1, 2,
                          0)
                    : ny_native_nir_emit_runtime_call(
                          b, "rt_native_tbuf_append", list, value,
                          ny_native_nir_emit_const(b, value_is_string ? 1 : 0),
                          3, 0);
      int length = ny_native_nir_emit_known_list_append_len(b, list, out);
      if (out < 0 || length < 0 ||
          !ny_native_nir_record_list_len_fact(b, out, length))
        return -1;
      return out;
    }
    if (e->as.memcall.target && e->as.memcall.name &&
        strcmp(e->as.memcall.name, "pop") == 0 &&
        e->as.memcall.args.len == 0) {
      const expr_t *target_expr = e->as.memcall.target;
      ny_native_nir_local_t *pop_local =
          target_expr->kind == NY_E_IDENT
              ? ny_native_nir_find_local(b, target_expr->as.ident.name)
              : NULL;
      if ((pop_local && pop_local->is_list) ||
          ny_native_nir_expr_is_dyn_list(b, target_expr)) {
        int list = ny_native_nir_lower_expr(b, target_expr);
        int out = list < 0
                      ? -1
                      : ny_native_nir_emit_runtime_call(
                            b, "rt_native_tbuf_pop", list, -1, -1, 1, 0);
        if (list < 0 || out < 0)
          return -1;
        int length = ny_native_nir_emit_known_list_pop_len(b, list);
        if (length < 0)
          length = ny_native_nir_emit_runtime_call(
              b, "rt_native_tbuf_len", list, -1, -1, 1, 0);
        if (length < 0 ||
            !ny_native_nir_record_list_len_fact(b, list, length))
          return -1;
        if (pop_local) {
          if (pop_local->list_len_slot < 0)
            pop_local->list_len_slot = b->next_local_slot++;
          if (!ny_native_nir_store_local_value(
                  b, pop_local->list_len_slot, length))
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
    bool target_is_ident =
        e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name;
    ny_native_nir_local_t *local =
        target_is_ident
            ? ny_native_nir_find_local(b, e->as.memcall.target->as.ident.name)
            : NULL;
    const char *method = e->as.memcall.name;
    if (strcmp(method, "len") == 0 &&
        e->as.memcall.args.len == 0 &&
        ny_native_nir_expr_is_cstr(b, e->as.memcall.target)) {
      int target = ny_native_nir_lower_expr(b, e->as.memcall.target);
      if (target < 0)
        return -1;
      int dynamic_length =
          ny_native_nir_peek_dyn_fact(b, target,
                                      NY_NATIVE_NIR_FACT_DYN_STR_LEN);
      if (dynamic_length >= 0)
        return dynamic_length;
      return ny_native_nir_emit_runtime_call(
          b, "rt_native_cstr_len", target, -1, -1, 1, 0);
    }
    if (e->as.memcall.target &&
        ((local && local->is_list) ||
         ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target) ||
         ny_native_nir_expr_is_any(b, e->as.memcall.target) ||
         e->as.memcall.target->kind == NY_E_CALL ||
         e->as.memcall.target->kind == NY_E_MEMCALL) &&
        strcmp(method, "append") == 0 &&
        e->as.memcall.args.len == 1) {
      int list = ny_native_nir_lower_expr(b, e->as.memcall.target);
      const expr_t *append_value = e->as.memcall.args.data[0].val;
      int value = ny_native_nir_lower_expr(b, append_value);
      bool value_is_string = ny_native_nir_expr_is_cstr(b, append_value);
      if (list < 0 || value < 0)
        return -1;
      bool scalar_list =
          ny_native_nir_expr_is_list(b, e->as.memcall.target) &&
          !ny_native_nir_expr_is_dyn_list(b, e->as.memcall.target);
      int out = scalar_list
                    ? ny_native_nir_emit_runtime_call(
                          b, "rt_native_tbuf_append_i64", list, value, -1, 2,
                          0)
                    : ny_native_nir_emit_runtime_call(
                          b, "rt_native_tbuf_append", list, value,
                          ny_native_nir_emit_const(b, value_is_string ? 1 : 0),
                          3, 0);
      int length = ny_native_nir_emit_known_list_append_len(b, list, out);
      if (out < 0 || length < 0 ||
          !ny_native_nir_record_list_len_fact(b, out, length))
        return -1;
      return out;
    }
    if ((local || ny_native_nir_expr_is_any(b, e->as.memcall.target) ||
         e->as.memcall.target->kind == NY_E_CALL ||
         e->as.memcall.target->kind == NY_E_MEMCALL ||
         e->as.memcall.target->kind == NY_E_MEMBER) &&
        (strcmp(method, "get") == 0 ||
         strcmp(method, "set") == 0 ||
         strcmp(method, "has") == 0 ||
         strcmp(method, "contains") == 0 ||
         strcmp(method, "exists") == 0 ||
         strcmp(method, "len") == 0 ||
         strcmp(method, "delete") == 0 ||
         strcmp(method, "remove") == 0)) {
      int dict = ny_native_nir_lower_expr(b, e->as.memcall.target);
      if (dict < 0)
        return -1;
      if (strcmp(method, "len") == 0) {
        if (e->as.memcall.args.len != 0) {
          ny_native_nir_fail(b, "native NYIR lower: dict.len takes no arguments");
          return -1;
        }
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_dict_len", dict, -1, -1, 1, 0);
      }
      if ((strcmp(method, "has") == 0 || strcmp(method, "contains") == 0 ||
           strcmp(method, "exists") == 0) &&
          e->as.memcall.args.len == 1) {
        int key = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
        if (key < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_dict_has", dict, key, -1, 2, 0);
      }
      if ((strcmp(method, "delete") == 0 || strcmp(method, "remove") == 0) &&
          e->as.memcall.args.len == 1) {
        int key = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
        if (key < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_dict_delete", dict, key, -1, 2, 0);
      }
      if (strcmp(method, "get") == 0 &&
          (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
        int key = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
        int fallback = e->as.memcall.args.len == 2
                           ? ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val)
                           : ny_native_nir_emit_const(b, 0);
        if (key < 0 || fallback < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_dict_get", dict, key, fallback, 3, 0);
      }
      if (strcmp(method, "set") == 0 && e->as.memcall.args.len == 2) {
        int key = ny_native_nir_lower_expr(b, e->as.memcall.args.data[0].val);
        int value = ny_native_nir_lower_expr(b, e->as.memcall.args.data[1].val);
        if (key < 0 || value < 0)
          return -1;
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_dict_set", dict, key, value, 3, 0);
      }
      ny_native_nir_fail(b, "native NYIR lower: unsupported dict.%s arity",
                         method);
      return -1;
    }
    char qualified[512];
    if (!ny_native_nir_qualified_expr(e->as.memcall.target, qualified,
                                      sizeof(qualified))) {
      ny_native_nir_fail(
          b, "native NYIR lower: unsupported member-call target at %s:%d",
          e->tok.filename ? e->tok.filename : "<source>", e->tok.line);
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
            ny_native_nir_fail(b, "native NYIR lower: canonical call name is too long");
            return -1;
          }
          memcpy(qualified, canonical, (size_t)cn + 1);
        }
      }
    }
    if (n < 0 || (size_t)n >= sizeof(qualified) - strlen(qualified)) {
      ny_native_nir_fail(b, "native NYIR lower: qualified call name is too long");
      return -1;
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
    if (strcmp(e->as.member.name, "len") == 0) {
      /*
       * A mutable identifier may still resolve to its declaration initializer
       * in the top-level value table.  Folding `x.len` through that table
       * would report the initializer length after `x = append(x, value)`.
       * Only fold direct list literals here; identifiers use their current
       * native value.
       */
      const expr_t *list =
          e->as.member.target->kind == NY_E_LIST
              ? e->as.member.target
              : NULL;
      if (list)
        return ny_native_nir_emit_const(b, (int64_t)list->as.list_like.len);
      int target = ny_native_nir_lower_expr(b, e->as.member.target);
      int length = ny_native_nir_peek_list_len_fact(b, target);
      if (target >= 0 && length >= 0)
        return length;
      int dynamic_length =
          ny_native_nir_peek_dyn_fact(b, target, NY_NATIVE_NIR_FACT_DYN_STR_LEN);
      if (target >= 0 && dynamic_length >= 0)
        return dynamic_length;
      if (target >= 0 && ny_native_nir_expr_is_cstr(b, e->as.member.target))
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_cstr_len", target, -1, -1, 1, 0);
      if (target >= 0 &&
          (ny_native_nir_expr_is_list(b, e->as.member.target) ||
           e->as.member.target->kind == NY_E_IDENT ||
           e->as.member.target->kind == NY_E_INDEX ||
           e->as.member.target->kind == NY_E_CALL ||
           e->as.member.target->kind == NY_E_MEMCALL))
        return ny_native_nir_emit_runtime_call(
            b, "rt_native_tbuf_len", target, -1, -1, 1, 0);
    }
    const expr_t *val = ny_native_nir_resolve_member_expr(b, e);
    if (val && val != e)
      return ny_native_nir_lower_expr(b, val);
    ny_native_nir_fail(
        b, "native NYIR lower: member access '%s' is not a constant or module value in %s",
        e->as.member.name,
        b->current_fn_name ? b->current_fn_name : "<unknown>");
    return -1;
  }
  default:
    ny_native_nir_fail(
        b, "native NYIR lower: expression kind %d ('%.*s') at %s:%d in %s is not in shared NYIR yet",
        (int)e->kind, (int)e->tok.len,
        e->tok.lexeme ? e->tok.lexeme : "",
        e->tok.filename ? e->tok.filename : "<source>", e->tok.line,
        b->current_fn_name ? b->current_fn_name : "<unknown>");
    return -1;
  }
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left,
                                       const expr_t *right,
                                       bool is_or) {
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

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s);
