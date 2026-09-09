/*
 * Statement lowering: AST → NYIR. Translates let/mut bindings, blocks,
 * control flow, and expressions into SSA form. References: Cooper &
 * Torczon, Ch.7; Cytron et al. (TOPLAS 1991) §4–5.
 */
static bool ny_native_stmt_is_stdlib(const stmt_t *s) {
  if (!s)
    return false;
  if (ny_is_stdlib_tok(s->tok))
    return true;
  return s->kind == NY_S_MODULE && s->as.module.name &&
         (strncmp(s->as.module.name, "std.", 4) == 0 ||
          strcmp(s->as.module.name, "std") == 0);
}

static const char *ny_native_nir_global_symbol(
    const ny_native_nir_builder_t *b, const char *name) {
  if (!b || !name)
    return NULL;
  const char *symbol = ny_native_globaltab_name(name);
  if (b->module_name && b->module_name[0]) {
    char qualified[512];
    int n = snprintf(qualified, sizeof(qualified), "%s.%s",
                     b->module_name, name);
    if (n > 0 && (size_t)n < sizeof(qualified)) {
      symbol = ny_native_globaltab_name(qualified);
      if (symbol)
        return symbol;
    }
  }
  /* A bare tail is ambiguous once stdlib modules are loaded: a user binding
   * such as `c` must not silently become std.core.counter.c.  The main
   * program has no namespace-qualified global to fall back to, so accept
   * only an exact registration there. */
  if (symbol)
    return symbol;
  if (b->profile_name && strcmp(b->profile_name, "rt_main") == 0 &&
      (!b->module_name || !b->module_name[0]))
    return NULL;
  symbol = ny_native_globaltab_name_tail(name);
  if (symbol)
    return symbol;
  if (!b->current_fn_name)
    return NULL;
  const char *dot = strrchr(b->current_fn_name, '.');
  if (!dot || !dot[1])
    return NULL;
  char qualified[512];
  int n = snprintf(qualified, sizeof(qualified), "%.*s.%s",
                   (int)(dot - b->current_fn_name), b->current_fn_name, name);
  return n > 0 && (size_t)n < sizeof(qualified)
             ? ny_native_globaltab_name(qualified)
             : NULL;
}
static const char *ny_native_fn_module_in_stmt(const stmt_t *s,
                                               const stmt_t *fn) {
  if (!s || !fn)
    return NULL;
  if (s == fn)
    return NULL;
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found =
          ny_native_fn_module_in_stmt(s->as.module.body.data[i], fn);
      if (found || s->as.module.body.data[i] == fn)
        return s->as.module.name;
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found =
          ny_native_fn_module_in_stmt(s->as.block.body.data[i], fn);
      if (found)
        return found;
    }
  }
  return NULL;
}

static const char *ny_native_fn_module(const program_t *prog,
                                       const stmt_t *fn) {
  if (!prog || !fn)
    return NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const char *found = ny_native_fn_module_in_stmt(prog->body.data[i], fn);
    if (found)
      return found;
  }
  if (fn->kind == NY_S_FUNC && fn->as.fn.name) {
    const char *dot = strrchr(fn->as.fn.name, '.');
    if (dot && dot > fn->as.fn.name) {
      static char mod_buf[8][256];
      static size_t mod_idx = 0;
      size_t len = (size_t)(dot - fn->as.fn.name);
      if (len < sizeof(mod_buf[0])) {
        char *buf = mod_buf[mod_idx++ % 8];
        memcpy(buf, fn->as.fn.name, len);
        buf[len] = '\0';
        return buf;
      }
    }
  }
  return NULL;
}


static bool ny_native_nir_expr_uses_ident(const ny_native_nir_builder_t *b,
                                          const expr_t *e, const char *name);
static bool ny_native_nir_stmt_uses_ident(const stmt_t *s, const char *name);

#define NY_NIR_SCAN_DESCEND(EXPR)                                              \
  do {                                                                         \
    if (ny_native_nir_expr_uses_ident(b, (EXPR), name))                        \
      return true;                                                             \
  } while (0)

static bool ny_native_nir_expr_uses_ident(const ny_native_nir_builder_t *b,
                                          const expr_t *e, const char *name) {
  (void)b;
  if (!e || !name || !*name)
    return false;
  switch (e->kind) {
  case NY_E_IDENT:
    return e->as.ident.name && strcmp(e->as.ident.name, name) == 0;
  case NY_E_CALL:
    if (e->as.call.callee)
      NY_NIR_SCAN_DESCEND(e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      NY_NIR_SCAN_DESCEND(e->as.call.args.data[i].val);
    return false;
  case NY_E_MEMCALL:
    if (e->as.memcall.target)
      NY_NIR_SCAN_DESCEND(e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      NY_NIR_SCAN_DESCEND(e->as.memcall.args.data[i].val);
    return false;
  case NY_E_MEMBER:
    if (e->as.member.target)
      NY_NIR_SCAN_DESCEND(e->as.member.target);
    return false;
  case NY_E_UNARY:
    NY_NIR_SCAN_DESCEND(e->as.unary.right);
    return false;
  case NY_E_BINARY:
    NY_NIR_SCAN_DESCEND(e->as.binary.left);
    NY_NIR_SCAN_DESCEND(e->as.binary.right);
    return false;
  case NY_E_LOGICAL:
    NY_NIR_SCAN_DESCEND(e->as.logical.left);
    NY_NIR_SCAN_DESCEND(e->as.logical.right);
    return false;
  case NY_E_TERNARY:
    NY_NIR_SCAN_DESCEND(e->as.ternary.cond);
    NY_NIR_SCAN_DESCEND(e->as.ternary.true_expr);
    NY_NIR_SCAN_DESCEND(e->as.ternary.false_expr);
    return false;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      NY_NIR_SCAN_DESCEND(e->as.list_like.data[i]);
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      NY_NIR_SCAN_DESCEND(e->as.dict.pairs.data[i].key);
      NY_NIR_SCAN_DESCEND(e->as.dict.pairs.data[i].value);
    }
    return false;
  case NY_E_INDEX:
    NY_NIR_SCAN_DESCEND(e->as.index.target);
    NY_NIR_SCAN_DESCEND(e->as.index.start);
    NY_NIR_SCAN_DESCEND(e->as.index.stop);
    NY_NIR_SCAN_DESCEND(e->as.index.step);
    return false;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        NY_NIR_SCAN_DESCEND(e->as.fstring.parts.data[i].as.e);
    return false;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      NY_NIR_SCAN_DESCEND(e->as.as_asm.args.data[i]);
    return false;
  case NY_E_COMPTIME:
    return e->as.comptime_expr.body &&
           ny_native_nir_stmt_uses_ident(e->as.comptime_expr.body, name);
  case NY_E_MATCH:
    NY_NIR_SCAN_DESCEND(e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        NY_NIR_SCAN_DESCEND(arm->patterns.data[j]);
      NY_NIR_SCAN_DESCEND(arm->guard);
      if (arm->conseq && ny_native_nir_stmt_uses_ident(arm->conseq, name))
        return true;
    }
    return e->as.match.default_conseq &&
           ny_native_nir_stmt_uses_ident(e->as.match.default_conseq, name);
  case NY_E_PTR_TYPE:
    NY_NIR_SCAN_DESCEND(e->as.ptr_type.target);
    return false;
  case NY_E_DEREF:
    NY_NIR_SCAN_DESCEND(e->as.deref.target);
    return false;
  case NY_E_SIZEOF:
    NY_NIR_SCAN_DESCEND(e->as.szof.target);
    return false;
  case NY_E_TRY:
    NY_NIR_SCAN_DESCEND(e->as.try_expr.target);
    return false;
  case NY_E_LAMBDA:
  case NY_E_FN:
    if (e->as.lambda.body)
      return ny_native_nir_stmt_uses_ident(e->as.lambda.body, name);
    return false;
  default:
    return false;
  }
}

static bool ny_native_nir_stmt_uses_ident(const stmt_t *s, const char *name) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_EXPR:
    return ny_native_nir_expr_uses_ident(NULL, s->as.expr.expr, name);
  case NY_S_RETURN:
    if (s->as.ret.value)
      return ny_native_nir_expr_uses_ident(NULL, s->as.ret.value, name);
    return false;
  case NY_S_BLOCK:
    if (s->as.block.body.data)
      for (size_t i = 0; i < s->as.block.body.len; ++i)
        if (s->as.block.body.data[i] &&
            ny_native_nir_stmt_uses_ident(s->as.block.body.data[i], name))
          return true;
    return false;
  default:
    return false;
  }
}

#undef NY_NIR_SCAN_DESCEND

static bool ny_native_nir_lower_var(ny_native_nir_builder_t *b, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_destructure)
    return ny_native_nir_fail(b,
                              "native NYIR lower: only simple def/mut bindings are supported");
  if (v->is_del) {
    for (size_t i = 0; i < v->names.len; ++i) {
      const char *name = v->names.data[i];
      if (!name || strcmp(name, "_") == 0)
        continue;
      int nil = ny_native_nir_emit_const(b, 0);
      if (nil < 0)
        return false;
      ny_native_nir_local_t *local = ny_native_nir_find_local(b, name);
      if (local) {
        if (!ny_native_nir_store_local_value(b, local->slot, nil))
          return false;
        b->last_value = nil;
        continue;
      }
      const char *sym = ny_native_nir_global_symbol(b, name);
      if (sym) {
        int addr = nyir_emit(&b->nyir,
                             (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                           .dst = -1, .a = -1, .b = -1,
                                           .imm = 0, .symbol = sym});
        if (addr < 0 || !ny_native_nir_emit_store_i64(b, addr, nil))
          return false;
        b->last_value = nil;
        continue;
      }
      ny_native_nir_fail(b, "native NYIR lower: cannot del unbound local '%s'",
                         name);
      return false;
    }
    return true;
  }
  int *pre_vals = NULL;
  if (v->names.len > 1) {
    pre_vals = (int *)alloca(sizeof(int) * v->names.len);
    for (size_t i = 0; i < v->names.len; ++i) {
      if (i < v->exprs.len && v->exprs.data[i]) {
        pre_vals[i] = ny_native_nir_lower_expr(b, v->exprs.data[i]);
        if (pre_vals[i] < 0) return false;
        /*
         * Sequential binding: if a later initializer references this name
         * (for example `def p, k = its.get(i), p.get(0)`), bind and store it
         * now so that reference resolves as a local instead of a bogus named
         * data symbol.
         */
        for (size_t j = i + 1;
             v->is_decl && j < v->names.len && j < v->exprs.len; ++j) {
          if (v->exprs.data[j] &&
              ny_native_nir_expr_uses_ident(b, v->exprs.data[j],
                                            v->names.data[i])) {
            ny_native_nir_local_t *dep =
                ny_native_nir_bind_local(b, v->names.data[i]);
            if (dep && !ny_native_nir_store_local_value(b, dep->slot,
                                                        pre_vals[i]))
              return false;
            break;
          }
        }
      } else {
        pre_vals[i] = -1;
      }
    }
  }
  for (size_t i = 0; i < v->names.len; ++i) {
    const char *name = v->names.data[i];
    if (!name || strcmp(name, "_") == 0)
      continue;
    if (i >= v->exprs.len || !v->exprs.data[i])
      return ny_native_nir_fail(
          b, "native NYIR lower: local '%s' needs an initializer", name);
    /* The native lowering path must preserve the source linter contract too.
     * Its compact local table otherwise bypasses scope_bind(), so nested
     * declarations could silently lose W2002 under --warn-all. */
    if (v->is_decl && b->scope_depth > 0 &&
        ny_native_nir_find_local(b, name) &&
        ny_diag_should_emit("shadow_local", s->tok, name)) {
      ny_diag_warning_code(
          s->tok, 2002, "declaration of %s'%s'%s shadows a previous local",
          clr(NY_CLR_BOLD), name, clr(NY_CLR_RESET));
      ny_diag_hint("this may hide the outer variable — use a different name if unintended");
    }
    const expr_t *global_init = v->exprs.data[i];
    const char *global_symbol = NULL;
    if (!ny_native_nir_find_local(b, name) &&
        (!v->is_decl || (b->scope_depth == 0 && b->profile_name &&
                         strcmp(b->profile_name, "rt_main") == 0)))
      global_symbol = ny_native_nir_global_symbol(b, name);
    if (global_symbol) {
      ny_native_nir_local_t *source_list_local =
          global_init && global_init->kind == NY_E_IDENT
              ? ny_native_nir_find_local(b, global_init->as.ident.name)
              : NULL;
      bool is_list =
          (i < v->types.len && ny_native_type_name_is_list(v->types.data[i])) ||
          ny_native_nir_expr_is_list(b, global_init) ||
          (source_list_local && source_list_local->is_list);
      bool typed_global_list =
          i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_list(v->types.data[i]) &&
          !ny_native_type_name_is_dyn_list(v->types.data[i]);
      int value;
      if (!pre_vals && typed_global_list && global_init &&
          global_init->kind == NY_E_CALL && global_init->as.call.callee &&
          global_init->as.call.callee->kind == NY_E_IDENT &&
          global_init->as.call.callee->as.ident.name &&
          (strcmp(global_init->as.call.callee->as.ident.name, "list") == 0 ||
           strcmp(global_init->as.call.callee->as.ident.name, "std.core.list") == 0) &&
          global_init->as.call.args.len == 1 &&
          !global_init->as.call.args.data[0].name) {
        /* `mut list<int> xs = list(n)` as a global: emit rt_list_new_sized
         * so indexed stores at 0..n-1 succeed immediately. */
        int arg = ny_native_nir_lower_expr(b, global_init->as.call.args.data[0].val);
        value = arg < 0 ? -1
                        : ny_native_nir_emit_runtime_call(b, "rt_list_new_sized",
                                                          arg, -1, -1, 1, 0);
      } else {
        value = pre_vals ? pre_vals[i] : ny_native_nir_lower_expr(b, global_init);
      }
      if (value >= 0 && global_init && global_init->kind == NY_E_BINARY &&
          ny_native_nir_expr_is_any(b, global_init) &&
          !ny_native_nir_expr_is_raw_dynamic_read(b, global_init) &&
          global_init->as.binary.op &&
          (strcmp(global_init->as.binary.op, "+") == 0 ||
           strcmp(global_init->as.binary.op, "*") == 0 ||
           strcmp(global_init->as.binary.op, "/") == 0 ||
           strcmp(global_init->as.binary.op, "%") == 0)) {
        const expr_t *target_init = ny_native_nir_find_top_level_value(b, name);
        if (target_init && target_init->kind == NY_E_LITERAL &&
            target_init->as.literal.kind == NY_LIT_INT &&
            target_init->tok.kind != NY_T_NIL)
          value = ny_native_nir_emit_runtime_call(
              b, "rt_any_to_i64", value, -1, -1, 1, 0);
      }
      if (value >= 0 && global_init && global_init->kind == NY_E_IDENT) {
        ny_native_nir_local_t *source_local =
            ny_native_nir_find_local(b, global_init->as.ident.name);
        const expr_t *target_init = ny_native_nir_find_top_level_value(
            b, name);
        /* Global slots retain the representation established by their
         * declaration.  In particular, `mut caught = ""` is a raw string
         * slot even when the catch binding is `any`; unboxing that binding as
         * an integer preserves a tagged string handle and loses its C-string
         * ABI at the next call. */
        bool target_is_string =
            target_init && ny_native_nir_expr_is_cstr(b, target_init);
        if (source_local && source_local->is_any && !target_is_string)
          value = ny_native_nir_emit_runtime_call(
              b, "rt_any_to_i64", value, -1, -1, 1, 0);
      }
      bool global_is_f64 = ny_native_nir_expr_is_f64(b, global_init);
      if (is_list && global_init && global_init->kind == NY_E_IDENT &&
          (!global_init->as.ident.name || strcmp(global_init->as.ident.name, name) != 0)) {
        value = ny_native_nir_emit_runtime_call(
            b, "rt_tbuf_clone_raw", value, -1, -1, 1, 0);
      }
      int addr = value < 0
                     ? -1
                     : nyir_emit(&b->nyir,
                                 (nyir_inst_t){.op = NYIR_ADDR_SYMBOL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = 0,
                                               .symbol = global_symbol});
      if (value < 0 || addr < 0 ||
          !(global_is_f64 ? ny_native_nir_emit_store_f64(b, addr, value)
                          : ny_native_nir_emit_store_i64(b, addr, value)))
        return false;
      b->last_value = value;
      continue;
    }
    bool is_f64 = i < v->types.len && ny_native_type_name_is_f64(v->types.data[i]);
    bool is_f32 = i < v->types.len && ny_native_type_name_is_f32(v->types.data[i]);
    bool is_cstr = i < v->types.len && v->types.data[i] &&
                   strcmp(v->types.data[i], "str") == 0;
    bool is_any = i < v->types.len && v->types.data[i] &&
                  strcmp(v->types.data[i], "any") == 0;
    ny_native_nir_local_t *prior_local =
        !v->is_decl ? ny_native_nir_find_local(b, name) : NULL;
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f64 = ny_native_nir_expr_is_f64(b, v->exprs.data[i]);
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f32 = ny_native_nir_expr_is_f32(b, v->exprs.data[i]);
    if (!is_cstr && i < v->exprs.len)
      is_cstr = ny_native_nir_expr_is_cstr(b, v->exprs.data[i]);
    const stmt_t *enum_stmt = NULL;
    const stmt_enum_item_t *enum_item = NULL;
    int64_t enum_value = 0;
    bool is_scalar_enum =
        i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_IDENT &&
        ny_native_nir_find_enum_member(b, v->exprs.data[i]->as.ident.name,
                                       &enum_stmt, &enum_item, &enum_value) &&
        enum_item && enum_item->fields.len == 0;
    if (!is_any && !is_cstr && i < v->exprs.len &&
        ny_native_nir_expr_is_any(b, v->exprs.data[i]) &&
        /* Untyped integer literals have a raw scalar NYIR representation;
         * treating every conservative semantic fallback as tagged dynamic
         * sends later `mut total += value` operations through the boxed ABI. */
        (v->is_decl || !prior_local || prior_local->is_any) &&
        !is_scalar_enum &&
        !(v->exprs.data[i] &&
          v->exprs.data[i]->kind == NY_E_LITERAL &&
          v->exprs.data[i]->as.literal.kind == NY_LIT_INT))
      is_any = true;
    if (!is_any && i < v->exprs.len &&
        v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_DICT)
      is_any = true;
    /* Indexing an `any` value yields a tagged dynamic payload even though the
     * native accessor itself returns a raw slot.  Preserve that provenance on
     * the receiving local so a later consumer (notably `to_str(item)` in the
     * runtime type registry) uses the any formatter instead of rendering a
     * string pointer as a decimal i64.  Typed/untyped list locals are kept on
     * their scalar path; only an explicitly any-typed receiver crosses this
     * boundary. */
    if (!is_any && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_INDEX &&
        v->exprs.data[i]->as.index.target) {
      const expr_t *index_target = v->exprs.data[i]->as.index.target;
      ny_native_nir_local_t *index_local =
          index_target->kind == NY_E_IDENT
              ? ny_native_nir_find_local(b, index_target->as.ident.name)
              : NULL;
      /* A list returned through an untyped helper (`list`/`_clone_*`) has
       * UNKNOWN element semantics, so its indexed value is still dynamic.
       * Lists already carrying a TAGGED_DYNAMIC fact (e.g. fannkuch's
       * mutable scalar builder) remain raw integers to avoid re-tagging hot
       * arithmetic. */
      if (ny_native_nir_expr_is_any(b, index_target) ||
          (index_local && index_local->is_dyn_list &&
           index_local->semantic_rep == NY_SEM_REP_UNKNOWN))
        is_any = true;
    }
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
    if (l && ((i < v->types.len &&
               ny_native_type_name_is_bytes(v->types.data[i])) ||
              ny_native_nir_expr_is_bytes(b, v->exprs.data[i])))
      l->is_bytes = true;
    if (l && is_bool)
      l->is_bool = true;
    if (l && is_any)
      l->is_any = true;
    if (l && i < v->types.len && v->types.data[i] &&
        v->types.data[i][0])
      l->type_name = v->types.data[i];
    if (l && i < v->exprs.len && v->exprs.data[i])
      l->is_bigint = ny_native_nir_expr_is_bigint(b, v->exprs.data[i]);
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->semantic.resolved) {
      l->semantic_rep = v->exprs.data[i]->semantic.rep;
      l->semantic_ownership = v->exprs.data[i]->semantic.ownership;
      l->alias_class = v->exprs.data[i]->semantic.alias_class;
      l->semantic_mutable = v->exprs.data[i]->semantic.mutable_value;
    }
    /* A sequence index is materialized as a raw slot payload.  Keep that
     * fact on an inferred local so an indirect dynamic callback can box the
     * scalar exactly at its call boundary, while ordinary list writes retain
     * the raw payload. */
    /* Primitive bitwise wrappers are semantically untyped, but a raw integer
     * function can accumulate their result in a mutable scalar. Preserve the
     * producer ABI on the local slot so the next read is not decoded as a
     * tagged value. */
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_CALL && b->current_fn_name) {
      const char *bit_leaf = ny_native_call_leaf(v->exprs.data[i]);
      const stmt_t *owner =
          ny_native_nir_find_user_function(b, b->current_fn_name);
      bool raw_owner = owner && owner->as.fn.return_type &&
                       !ny_native_type_name_is_any(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_list(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f64(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f32(owner->as.fn.return_type);
      if (raw_owner && bit_leaf &&
          (strcmp(bit_leaf, "bor") == 0 || strcmp(bit_leaf, "band") == 0 ||
           strcmp(bit_leaf, "bxor") == 0 || strcmp(bit_leaf, "__or") == 0 ||
           strcmp(bit_leaf, "__and") == 0 || strcmp(bit_leaf, "__xor") == 0)) {
        l->is_any = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    /* Raw memory loads preserve machine words even when their language
     * declarations use any.  Retain that producer ABI through local slots
     * so offsets and scalar comparisons never decode the word again. */
    if (l && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_CALL) {
      const char *load_leaf = ny_native_call_leaf(v->exprs.data[i]);
      ny_native_leaf_kind_t load_kind = ny_native_leaf_kind(load_leaf);
      if (load_kind == NY_NATIVE_LEAF_LOAD8 ||
          load_kind == NY_NATIVE_LEAF_LOAD64 ||
          load_kind == NY_NATIVE_LEAF_LOAD64_IDX) {
        l->is_any = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    /* Nullary enum members have a proven raw integer ABI even when semantic
     * inference conservatively annotates the identifier as dynamic.  Keep
     * the local slot and its consumers on that representation; otherwise a
     * later `r == 0` tags only the literal and compares unlike encodings. */
    if (l && is_scalar_enum) {
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        (v->exprs.data[i]->kind == NY_E_FN ||
         v->exprs.data[i]->kind == NY_E_LAMBDA))
      l->callable_expr = v->exprs.data[i];
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_IDENT &&
        ny_native_nir_find_user_function(b, v->exprs.data[i]->as.ident.name))
      l->callable_name = v->exprs.data[i]->as.ident.name;
    if (l && ((i < v->types.len && v->types.data[i] &&
               strcmp(v->types.data[i], "dict") == 0) ||
              ny_native_nir_expr_is_dict(b, v->exprs.data[i])))
      l->is_dict = true;
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_CALL && !l->is_list &&
        !l->is_cstr && !l->is_f64 && !l->is_f32 && !l->is_any)
      l->is_dict = ny_native_nir_expr_is_dict(b, v->exprs.data[i]);
    ny_native_nir_local_t *source_list_local =
        v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_IDENT
            ? ny_native_nir_find_local(b, v->exprs.data[i]->as.ident.name)
            : NULL;
    bool is_list =
        (i < v->types.len && ny_native_type_name_is_list(v->types.data[i])) ||
        ny_native_nir_expr_is_list(b, v->exprs.data[i]) ||
        (source_list_local && source_list_local->is_list);
    bool proven_list_shape =
        (i < v->types.len && ny_native_type_name_is_list(v->types.data[i])) ||
        (v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_LIST) ||
        (source_list_local && source_list_local->is_list);
    /* Indexing produces an element, not a copy of the indexed container.
     * In particular, an untyped list may hold integers, and promoting every
     * `xs[i]` assignment to a list here makes scalar temporaries acquire the
     * list ABI (emitting clone/len calls for values such as fannkuch's `k`).
     * A list-of-lists declaration carries its shape through the declared type
     * and therefore does not need this broad inference fallback. */
    if (l && is_list && !l->is_list) {
      l->is_list = true;
      l->list_len_slot = b->next_local_slot++;
    }
    if (l && is_list && proven_list_shape &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_any(v->types.data[i])))
      l->is_any = false;
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
      /* A list-shaped value returned by a dynamic call/member access has no
       * compile-time element stride. Treat it as descriptor-backed so later
       * `.get`/index reads decode raw elem-8 payloads exactly once. Explicit
       * `list<T>` declarations remain on the typed scalar path. */
      if (v->exprs.data[i] &&
          (v->exprs.data[i]->kind == NY_E_CALL ||
           v->exprs.data[i]->kind == NY_E_MEMCALL))
        l->is_dyn_list = true;
      l->list_literal =
          v->is_mut ? NULL : ny_native_nir_resolve_list_literal(b, v->exprs.data[i], 0);
    }
    if (typed_scalar_list)
      l->list_literal = NULL;
    int64_t declared_fin_bound =
        i < v->types.len ? ny_native_resolve_fin_bound(b, v->types.data[i]) : 0;
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
    if (pre_vals) {
      val = pre_vals[i];
    } else if (is_f32 && init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_FLOAT)
      val = ny_native_nir_emit_const_f32(b, init->as.literal.as.f);
else if (v->is_decl && !v->is_mut && !is_list && init &&
         init->kind == NY_E_LIST &&
         init->as.list_like.len == 1) {
      /*
       * `def y = [v]` unpacks a single-element list into scalar y.  A
       * list-typed declaration must receive the list itself: unwrapping the
       * element here turned `def list xs = [1]` into the raw int 1.
       */
      val = ny_native_nir_lower_expr(b, init->as.list_like.data[0]);
    } else if (typed_scalar_list && init && init->kind == NY_E_CALL &&
               init->as.call.callee &&
               init->as.call.callee->kind == NY_E_IDENT &&
               init->as.call.callee->as.ident.name &&
               (strcmp(init->as.call.callee->as.ident.name, "list") == 0 ||
                strcmp(init->as.call.callee->as.ident.name, "std.core.list") == 0) &&
               init->as.call.args.len == 1 && !init->as.call.args.data[0].name) {
      /* `mut list<int> xs = list(n)` — a typed scalar-integer list pre-sized
       * to n elements.  rt_list_new zeros the length so indexed writes to
       * 0..n-1 silently fail; rt_list_new_sized keeps length=n so random-
       * access stores work immediately.  This is the only call site that needs
       * random-access rather than append semantics. */
      int arg = ny_native_nir_lower_expr(b, init->as.call.args.data[0].val);
      if (arg < 0)
        return false;
      val = ny_native_nir_emit_runtime_call(b, "rt_list_new_sized", arg, -1, -1, 1, 0);
    } else {
      bool saved_index_for_any = b->index_for_any_call;
      if (is_any && init && init->kind == NY_E_INDEX)
        b->index_for_any_call = true;
      val = ny_native_nir_lower_expr(b, init);
      b->index_for_any_call = saved_index_for_any;
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
    if (is_any && init && init->kind == NY_E_INDEX &&
        init->as.index.target &&
        init->as.index.target->kind == NY_E_IDENT &&
        init->as.index.target->as.ident.name) {
      ny_native_nir_local_t *index_local = ny_native_nir_find_local(
          b, init->as.index.target->as.ident.name);
      if (index_local &&
          (index_local->is_dyn_list ||
           (index_local->type_name &&
            strcmp(index_local->type_name, "seq") == 0)))
        l->raw_dynamic_index = true;
    }
    if (is_any && init && init->kind == NY_E_INDEX &&
        ny_native_nir_expr_is_dyn_list(b, init->as.index.target)) {
      val = ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", val, -1,
                                             -1, 1, 0);
      if (val < 0)
        return false;
    }
    /* A mutable string local may receive an error/value from an `any`
     * binding (the common `catch e { caught = e }` pattern).  Keep the local's
     * raw C-string ABI by decoding that dynamic value exactly once; storing
     * the tagged pointer directly makes later `str_contains`/`strlen` calls
     * dereference the tag word as an address. */
    if (l->is_cstr && init && !ny_native_nir_expr_is_cstr(b, init) &&
        ny_native_nir_expr_is_any(b, init)) {
      val = ny_native_nir_emit_runtime_call(
          b, "rt_any_to_cstr", val, -1, -1, 1, 0);
      if (val < 0)
        return false;
    }
    if (is_list && init && init->kind == NY_E_IDENT &&
        (!init->as.ident.name || strcmp(init->as.ident.name, name) != 0)) {
      val = ny_native_nir_emit_runtime_call(
          b, "rt_tbuf_clone_raw", val, -1, -1, 1, 0);
      if (val < 0)
        return false;
    }
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
static bool ny_native_nir_lower_guard(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  if (s->as.guard.name && s->as.guard.value) {
    char from_fn[256];
    const char *type_name = s->as.guard.type_name;
    const char *from_name = NULL;
    if (type_name) {
      while (*type_name == '?') type_name++;
      if (type_name[0] == '*') type_name++;
      snprintf(from_fn, sizeof(from_fn), "%s_from", type_name);
      from_name = from_fn;
    }
    expr_t *val_expr = s->as.guard.value;
    call_arg_t arg = {.val = val_expr};
    expr_t callee = {.kind = NY_E_IDENT, .tok = s->tok};
    callee.as.ident.name = from_name;
    expr_t from_call = {.kind = NY_E_CALL, .tok = s->tok};
    from_call.as.call.callee = &callee;
    from_call.as.call.args.data = &arg;
    from_call.as.call.args.len = 1;
    from_call.as.call.args.cap = 1;

    bool has_from_fn = from_name && ny_native_nir_find_user_function(b, from_name);
    int value = ny_native_nir_lower_expr(b, has_from_fn ? &from_call : val_expr);
    if (value < 0)
      return false;
    ny_native_nir_local_t *local = ny_native_nir_bind_local(b, s->as.guard.name);
    if (!local)
      return false;
    local->slot = b->next_local_slot++;
    if (!ny_native_nir_store_local_value(b, local->slot, value))
      return false;
    int pass_label = b->next_label++;
    int fail_label = b->next_label++;
    if (!ny_native_nir_emit_br_if(b, value, pass_label) ||
        !ny_native_nir_emit_br(b, fail_label) ||
        !ny_native_nir_emit_label(b, fail_label))
      return false;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(b, s->as.guard.fallback))
      return false;
    if (!b->emitted_return && !ny_native_nir_emit_br(b, pass_label))
      return false;
    b->emitted_return = false;
    return ny_native_nir_emit_label(b, pass_label);
  } else if (s->as.guard.value) {
    int cond = ny_native_nir_lower_expr(b, s->as.guard.value);
    if (cond < 0)
      return false;
    int pass_label = b->next_label++;
    int fail_label = b->next_label++;
    if (!ny_native_nir_emit_br_if(b, cond, pass_label) ||
        !ny_native_nir_emit_br(b, fail_label) ||
        !ny_native_nir_emit_label(b, fail_label))
      return false;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(b, s->as.guard.fallback))
      return false;
    if (!b->emitted_return && !ny_native_nir_emit_br(b, pass_label))
      return false;
    b->emitted_return = false;
    return ny_native_nir_emit_label(b, pass_label);
  }
  return true;
}

static bool ny_native_nir_lower_while(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  if (s->as.whl.init && !ny_native_nir_lower_stmt(b, s->as.whl.init))
    return false;
  if (s->as.whl.test && s->as.whl.test->kind == NY_E_LITERAL) {
    if (s->as.whl.test->as.literal.kind == NY_LIT_INT &&
        s->as.whl.test->as.literal.as.i == 0)
      return true;
    if (s->as.whl.test->as.literal.kind == NY_LIT_BOOL &&
        !s->as.whl.test->as.literal.as.b)
      return true;
  }

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
  bool body_terminated = b->emitted_return;
  if (body_ok && s->as.whl.update) {
    b->emitted_return = false;
    body_ok = ny_native_nir_emit_label(b, update_label) &&
              ny_native_nir_lower_scoped_body(b, s->as.whl.update);
    body_terminated = b->emitted_return;
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  if (!body_terminated && !ny_native_nir_emit_br(b, head_label))
    return false;
  return ny_native_nir_emit_label(b, end_label);
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
        b,
        "native NYIR lower: for loops currently support `for name in lo..hi` "
        "ranges at %s:%d",
        s->tok.filename ? s->tok.filename : "<source>", s->tok.line);
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
        b, "rt_cstr_builder_new", initial, -1, -1, 1, 0);
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
        ny_native_nir_emit_runtime_call(b, "rt_cstr_builder_append",
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
        b, "rt_cstr_builder_finalize", builder, -1, -1, 1, 0);
    if (value < 0 || !ny_native_nir_store_local_value(b, promoted->slot, value))
      ok = false;
    /*
     * C-string length metadata must describe the finalized buffer, not the
     * literal initializer retained before promotion.
     */
    if (ok) {
      int length = ny_native_nir_emit_runtime_call(
          b, "rt_cstr_len", value, -1, -1, 1, 0);
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
static bool ny_native_nir_lower_for_list(ny_native_nir_builder_t *b,
                                         const stmt_t *s) {
  bool is_str = ny_native_nir_expr_is_cstr(b, s->as.fr.iterable) ||
                (s->as.fr.iterable && s->as.fr.iterable->kind == NY_E_LITERAL &&
                 s->as.fr.iterable->as.literal.kind == NY_LIT_STR);
  if (!s->as.fr.iter_var || !s->as.fr.iterable ||
      (!ny_native_nir_expr_is_list(b, s->as.fr.iterable) &&
       !ny_native_nir_expr_is_dyn_list(b, s->as.fr.iterable) &&
       !is_str &&
       !ny_native_nir_expr_is_any(b, s->as.fr.iterable))) {
    return ny_native_nir_fail(
        b, "native NYIR lower: list iteration requires a list expression at %s:%d",
        s->tok.filename ? s->tok.filename : "<source>", s->tok.line);
  }
  int iterable = ny_native_nir_lower_expr(b, s->as.fr.iterable);
  int list_slot = ny_native_nir_temp_slot(b);
  int index_slot = ny_native_nir_temp_slot(b);
  if (iterable < 0 || list_slot < 0 || index_slot < 0 ||
      !ny_native_nir_store_local_value(b, list_slot, iterable))
    return false;
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0 || !ny_native_nir_store_local_value(b, index_slot, zero))
    return false;

  size_t loop_scope_mark = ny_native_nir_scope_mark(b);
  ny_native_nir_local_t *iter =
      ny_native_nir_bind_local(b, s->as.fr.iter_var);
  ny_native_nir_local_t *index = s->as.fr.iter_index_var
                                     ? ny_native_nir_bind_local(
                                           b, s->as.fr.iter_index_var)
                                     : NULL;
  if (!iter || (s->as.fr.iter_index_var && !index))
    return false;
  if (is_str) {
    iter->is_cstr = true;
    iter->semantic_rep = NY_SEM_REP_STRING;
  }
  int head_label = b->next_label++;
  int body_label = b->next_label++;
  int update_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int list_value = ny_native_nir_load_local_value(b, list_slot);
  int current = ny_native_nir_load_local_value(b, index_slot);
  int length = list_value < 0
                   ? -1
                   : ny_native_nir_emit_runtime_call(
                         b, is_str ? "rt_cstr_len" : "rt_tbuf_len_raw",
                         list_value, -1, -1, 1, 0);
  int in_range =
      length < 0 || current < 0
          ? -1
          : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64,
                                               .dst = -1,
                                               .a = current,
                                               .b = length,
                                               .cmp = NYIR_CMP_LT});
  if (in_range < 0 || !ny_native_nir_emit_br_if(b, in_range, body_label) ||
      !ny_native_nir_emit_br(b, end_label) ||
      !ny_native_nir_emit_label(b, body_label))
    return false;
  int value = ny_native_nir_emit_runtime_call(
      b, is_str ? "rt_cstr_get_raw" : "rt_tbuf_get",
      list_value, current,
      is_str ? -1 : ny_native_nir_emit_const(b, 0),
      is_str ? 2 : 3, 0);
  int bound_value = s->as.fr.iter_by_index ? current : value;
  if (value < 0 || bound_value < 0 ||
      !ny_native_nir_store_local_value(b, iter->slot, bound_value) ||
      (index && !ny_native_nir_store_local_value(b, index->slot, current)))
    return false;
  size_t loop_i = b->loop_depth;
  if (!ny_native_nir_push_loop(b, head_label, update_label, end_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_scoped_body(b, s->as.fr.body);
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = false;
  if (!ny_native_nir_emit_label(b, update_label))
    return false;
  current = ny_native_nir_load_local_value(b, index_slot);
  int one = ny_native_nir_emit_const(b, 1);
  int next = current < 0 || one < 0
                 ? -1
                 : nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_ADD_I64,
                                                      .dst = -1,
                                                      .a = current,
                                                      .b = one});
  if (next < 0 || !ny_native_nir_store_local_value(b, index_slot, next))
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  bool ok = ny_native_nir_emit_br(b, head_label) &&
            ny_native_nir_emit_label(b, end_label);
  ny_native_nir_scope_restore(b, loop_scope_mark);
  return ok;
}


static bool ny_native_nir_lower_for(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.fr.init || s->as.fr.cond || s->as.fr.update) {
    /* C-style `for (init; cond; update)` shares the while-loop CFG, but its
     * init binding must remain visible to both the body and update clause. */
    size_t scope_mark = ny_native_nir_scope_mark(b);
    if (s->as.fr.init && !ny_native_nir_lower_stmt(b, s->as.fr.init))
      return false;
    int head_label = b->next_label++;
    int update_label = b->next_label++;
    int body_label = b->next_label++;
    int end_label = b->next_label++;
    if (!ny_native_nir_emit_label(b, head_label))
      return false;
    int cond = s->as.fr.cond ? ny_native_nir_lower_expr(b, s->as.fr.cond)
                             : ny_native_nir_emit_const(b, 1);
    if (cond < 0 || !ny_native_nir_emit_br_if(b, cond, body_label) ||
        !ny_native_nir_emit_br(b, end_label) ||
        !ny_native_nir_emit_label(b, body_label))
      return false;
    size_t loop_i = b->loop_depth;
    if (!ny_native_nir_push_loop(b, head_label, update_label, end_label))
      return false;
    bool entry_return = b->emitted_return;
    int entry_last_value = b->last_value;
    b->emitted_return = false;
    bool body_ok = ny_native_nir_lower_scoped_body(b, s->as.fr.body);
    b->loop_depth = loop_i;
    if (!body_ok)
      return false;
    b->emitted_return = false;
    if (!ny_native_nir_emit_label(b, update_label))
      return false;
    if (s->as.fr.update && !ny_native_nir_lower_stmt(b, s->as.fr.update))
      return false;
    if (!b->emitted_return && !ny_native_nir_emit_br(b, head_label))
      return false;
    b->emitted_return = entry_return;
    b->last_value = entry_last_value;
    bool ok = ny_native_nir_emit_label(b, end_label);
    ny_native_nir_scope_restore(b, scope_mark);
    return ok;
  }
  const expr_t *lo = NULL;
  const expr_t *hi = NULL;
  if (ny_native_nir_iterable_is_range(s->as.fr.iterable, &lo, &hi))
    return ny_native_nir_lower_for_range(b, s);
  return ny_native_nir_lower_for_list(b, s);
}

static bool ny_native_nir_pattern_is_wildcard(const expr_t *pat) {
  return pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
         strcmp(pat->as.ident.name, "_") == 0;
}

static bool ny_native_nir_static_int(const expr_t *expr, int64_t *out) {
  if (!expr || !out)
    return false;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_INT) {
    *out = expr->as.literal.as.i;
    return true;
  }
  if (expr->kind == NY_E_COMPTIME) {
    const stmt_t *body = expr->as.comptime_expr.body;
    if (body && body->kind == NY_S_BLOCK && body->as.block.body.len == 1)
      body = body->as.block.body.data[0];
    if (body && body->kind == NY_S_RETURN)
      return ny_native_nir_static_int(body->as.ret.value, out);
    if (body && body->kind == NY_S_EXPR)
      return ny_native_nir_static_int(body->as.expr.expr, out);
  }
  return false;
}

/* Return 1 with the selected arm, 2 for the default, or 0 if dynamic. */
static int ny_native_nir_static_match_arm(const stmt_t *s,
                                          size_t *selected_out) {
  int64_t value = 0;
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test ||
      s->as.match.test->kind != NY_E_COMPTIME ||
      !ny_native_nir_static_int(s->as.match.test, &value))
    return 0;
  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    const match_arm_t *arm = &s->as.match.arms.data[i];
    if (arm->guard)
      return 0;
    for (size_t p = 0; p < arm->patterns.len; ++p) {
      const expr_t *pat = arm->patterns.data[p];
      int64_t exact = 0, lo = 0, hi = 0;
      if (ny_native_nir_pattern_is_wildcard(pat)) {
        *selected_out = i;
        return 1;
      }
      if (ny_native_nir_static_int(pat, &exact) && exact == value) {
        *selected_out = i;
        return 1;
      }
      if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
          strcmp(pat->as.binary.op, "..") == 0 &&
          ny_native_nir_static_int(pat->as.binary.left, &lo) &&
          ny_native_nir_static_int(pat->as.binary.right, &hi) &&
          value >= lo && value <= hi) {
        *selected_out = i;
        return 1;
      }
    }
  }
  return 2;
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
                                             const expr_t *pat,
                                             bool test_any) {
  if (ny_native_nir_pattern_is_wildcard(pat))
    return ny_native_nir_emit_const(b, 1);

  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    int lo = ny_native_nir_lower_expr(b, pat->as.binary.left);
    int hi = ny_native_nir_lower_expr(b, pat->as.binary.right);
    if (lo < 0 || hi < 0)
      return -1;
    if (ny_native_nir_expr_is_cstr(b, pat->as.binary.left) ||
        (pat->as.binary.left && pat->as.binary.left->kind == NY_E_LITERAL &&
         pat->as.binary.left->as.literal.kind == NY_LIT_STR)) {
      int cmp_lo = ny_native_nir_emit_runtime_call(
            b, "rt_cstr_cmp", test_value, lo, -1, 2, 0);
      int cmp_hi = ny_native_nir_emit_runtime_call(
            b, "rt_cstr_cmp", test_value, hi, -1, 2, 0);
      int zero = ny_native_nir_emit_const(b, 0);
      int ge = ny_native_nir_emit_cmp(b, cmp_lo, zero, NYIR_CMP_GE);
      int le = ny_native_nir_emit_cmp(b, cmp_hi, zero, NYIR_CMP_LE);
      return ny_native_nir_emit_bool_binop(b, NYIR_AND_I64, ge, le);
    }
    if (ny_native_nir_expr_is_f64(b, pat->as.binary.left) ||
        (pat->as.binary.left && pat->as.binary.left->kind == NY_E_LITERAL &&
         pat->as.binary.left->as.literal.kind == NY_LIT_FLOAT)) {
      int ge = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_F64,
                                                 .dst = -1,
                                                 .a = test_value,
                                                 .b = lo,
                                                 .cmp = NYIR_CMP_GE});
      int le = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_F64,
                                                 .dst = -1,
                                                 .a = test_value,
                                                 .b = hi,
                                                 .cmp = NYIR_CMP_LE});
      return ny_native_nir_emit_bool_binop(b, NYIR_AND_I64, ge, le);
    }
    int ge = ny_native_nir_emit_cmp(b, test_value, lo, NYIR_CMP_GE);
    int le = ny_native_nir_emit_cmp(b, test_value, hi, NYIR_CMP_LE);
    if (ge < 0 || le < 0)
      return -1;
    return ny_native_nir_emit_bool_binop(b, NYIR_AND_I64, ge, le);
  }

  /*
   * Result patterns are predicates over the tagged result value. Their
   * payload identifiers are bindings, not ordinary function arguments.
   */
  if (pat && pat->kind == NY_E_CALL && pat->as.call.callee &&
      pat->as.call.callee->kind == NY_E_IDENT &&
      pat->as.call.callee->as.ident.name &&
      (strcmp(pat->as.call.callee->as.ident.name, "ok") == 0 ||
       strcmp(pat->as.call.callee->as.ident.name, "err") == 0))
    return ny_native_nir_emit_runtime_call(
        b, strcmp(pat->as.call.callee->as.ident.name, "ok") == 0
               ? "rt_is_ok"
               : "rt_is_err",
        test_value, -1, -1, 1, 0);

  if (pat && (pat->kind == NY_E_CALL || pat->kind == NY_E_MEMCALL)) {
    const char *pat_callee_name = NULL;
    char pat_name_buf[128] = {0};
    if (pat->kind == NY_E_CALL && pat->as.call.callee) {
      if (pat->as.call.callee->kind == NY_E_IDENT) {
        pat_callee_name = pat->as.call.callee->as.ident.name;
      } else if (pat->as.call.callee->kind == NY_E_MEMBER &&
                 pat->as.call.callee->as.member.target &&
                 pat->as.call.callee->as.member.target->kind == NY_E_IDENT) {
        snprintf(pat_name_buf, sizeof(pat_name_buf), "%s.%s",
                 pat->as.call.callee->as.member.target->as.ident.name,
                 pat->as.call.callee->as.member.name);
        pat_callee_name = pat_name_buf;
      }
    } else if (pat->kind == NY_E_MEMCALL && pat->as.memcall.name) {
      if (pat->as.memcall.target &&
          pat->as.memcall.target->kind == NY_E_IDENT &&
          pat->as.memcall.target->as.ident.name) {
        snprintf(pat_name_buf, sizeof(pat_name_buf), "%s.%s",
                 pat->as.memcall.target->as.ident.name, pat->as.memcall.name);
        pat_callee_name = pat_name_buf;
      } else {
        pat_callee_name = pat->as.memcall.name;
      }
    }
    const stmt_t *adt_enum = NULL;
    const stmt_enum_item_t *adt_item = NULL;
    int64_t adt_tag = 0;
    if (pat_callee_name &&
        (ny_native_nir_find_enum_member(b, pat_callee_name, &adt_enum, &adt_item, &adt_tag) ||
         (strrchr(pat_callee_name, '.') && ny_native_nir_find_enum_member(b, strrchr(pat_callee_name, '.') + 1, &adt_enum, &adt_item, &adt_tag)))) {
      if (adt_item && adt_item->fields.len > 0) {
        int actual_tag = ny_native_nir_emit_runtime_call(
            b, "rt_adt_tag", test_value, -1, -1, 1, 0);
        int expected_tag = ny_native_nir_emit_const(b, adt_tag);
        return ny_native_nir_emit_cmp(b, actual_tag, expected_tag, NYIR_CMP_EQ);
      }
    }
  }

  int rhs = ny_native_nir_lower_expr(b, pat);
  if (rhs < 0)
    return -1;
  /* A dynamic match value uses the boxed NyValue encoding.  Literal integer
   * patterns are parsed as raw NYIR integers, so box small literals before
   * comparing them with an `any` test value. */
  if (test_any && pat && pat->kind == NY_E_LITERAL &&
      pat->as.literal.kind == NY_LIT_INT && pat->tok.kind != NY_T_NIL &&
      pat->as.literal.as.i < (INT64_C(1) << 62) &&
      pat->as.literal.as.i > -(INT64_C(1) << 62)) {
    int one = ny_native_nir_emit_const(b, 1);
    int shifted = one < 0 ? -1 : nyir_emit(
        &b->nyir, (nyir_inst_t){.op = NYIR_SHL_I64, .dst = -1,
                                .a = rhs, .b = one});
    rhs = shifted < 0 ? -1 : ny_native_nir_emit_binop(b, NYIR_OR_I64,
                                                       shifted, one);
    if (rhs < 0)
      return -1;
  }
  if (ny_native_nir_expr_is_cstr(b, pat) ||
      (pat && pat->kind == NY_E_LITERAL && pat->as.literal.kind == NY_LIT_STR))
    return ny_native_nir_emit_runtime_call(
        b, "rt_cstr_eq", test_value, rhs, -1, 2, 0);
  if (pat && (pat->kind == NY_E_LIST || pat->kind == NY_E_TUPLE ||
              pat->kind == NY_E_DICT || pat->kind == NY_E_SET ||
              ny_native_nir_expr_is_list(b, pat) ||
              ny_native_nir_expr_is_dyn_list(b, pat))) {
    int equal = ny_native_nir_emit_runtime_call(
        b, "rt_any_eq", test_value, rhs, -1, 2, 0);
    int true_imm = ny_native_nir_emit_const(b, NY_IMM_TRUE);
    return ny_native_nir_emit_cmp(b, equal, true_imm, NYIR_CMP_EQ);
  }
  if (ny_native_nir_expr_is_f64(b, pat) ||
      (pat && pat->kind == NY_E_LITERAL && pat->as.literal.kind == NY_LIT_FLOAT))
    return nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_CMP_F64,
                                             .dst = -1,
                                             .a = test_value,
                                             .b = rhs,
                                             .cmp = NYIR_CMP_EQ});
  return ny_native_nir_emit_cmp(b, test_value, rhs, NYIR_CMP_EQ);
}

static bool ny_native_nir_bind_result_pattern(ny_native_nir_builder_t *b,
                                              int test_value,
                                              const expr_t *pat) {
  if (!b || !pat || (pat->kind != NY_E_CALL && pat->kind != NY_E_MEMCALL))
    return true;

  if (pat->kind == NY_E_CALL && pat->as.call.callee &&
      pat->as.call.callee->kind == NY_E_IDENT &&
      pat->as.call.callee->as.ident.name &&
      (strcmp(pat->as.call.callee->as.ident.name, "ok") == 0 ||
       strcmp(pat->as.call.callee->as.ident.name, "err") == 0)) {
    if (pat->as.call.args.len == 0)
      return true;
    int payload = ny_native_nir_emit_runtime_call(
        b, "rt_unwrap", test_value, -1, -1, 1, 0);
    if (payload < 0)
      return false;
    for (size_t i = 0; i < pat->as.call.args.len; ++i) {
      const expr_t *arg = pat->as.call.args.data[i].val;
      if (!arg || arg->kind != NY_E_IDENT || !arg->as.ident.name ||
          strcmp(arg->as.ident.name, "_") == 0)
        continue;
      ny_native_nir_local_t *local =
          ny_native_nir_bind_local(b, arg->as.ident.name);
      if (!local || !ny_native_nir_store_local_value(b, local->slot, payload))
        return false;
    }
    return true;
  }

  const char *pat_callee_name = NULL;
  char pat_name_buf[128] = {0};
  if (pat->kind == NY_E_CALL && pat->as.call.callee) {
    if (pat->as.call.callee->kind == NY_E_IDENT) {
      pat_callee_name = pat->as.call.callee->as.ident.name;
    } else if (pat->as.call.callee->kind == NY_E_MEMBER &&
               pat->as.call.callee->as.member.target &&
               pat->as.call.callee->as.member.target->kind == NY_E_IDENT) {
      snprintf(pat_name_buf, sizeof(pat_name_buf), "%s.%s",
               pat->as.call.callee->as.member.target->as.ident.name,
               pat->as.call.callee->as.member.name);
      pat_callee_name = pat_name_buf;
    }
  } else if (pat->kind == NY_E_MEMCALL && pat->as.memcall.name) {
    if (pat->as.memcall.target &&
        pat->as.memcall.target->kind == NY_E_IDENT &&
        pat->as.memcall.target->as.ident.name) {
      snprintf(pat_name_buf, sizeof(pat_name_buf), "%s.%s",
               pat->as.memcall.target->as.ident.name,
               pat->as.memcall.name);
      pat_callee_name = pat_name_buf;
    } else {
      pat_callee_name = pat->as.memcall.name;
    }
  }
  const stmt_t *adt_enum = NULL;
  const stmt_enum_item_t *adt_item = NULL;
  int64_t adt_tag = 0;
  if (pat_callee_name &&
      (ny_native_nir_find_enum_member(b, pat_callee_name, &adt_enum, &adt_item, &adt_tag) ||
       (strrchr(pat_callee_name, '.') && ny_native_nir_find_enum_member(b, strrchr(pat_callee_name, '.') + 1, &adt_enum, &adt_item, &adt_tag)))) {
    size_t pat_args = pat->kind == NY_E_CALL ? pat->as.call.args.len
                                             : pat->as.memcall.args.len;
    if (adt_item && adt_item->fields.len > 0) {
      for (size_t i = 0; i < pat_args && i < adt_item->fields.len; ++i) {
        const expr_t *arg = pat->kind == NY_E_CALL
                                ? pat->as.call.args.data[i].val
                                : pat->as.memcall.args.data[i].val;
        if (!arg || arg->kind != NY_E_IDENT || !arg->as.ident.name ||
            strcmp(arg->as.ident.name, "_") == 0)
          continue;
        int off = ny_native_nir_emit_const(b, (int64_t)(i * 8));
        int fptr = ny_native_nir_emit_binop(b, NYIR_ADD_I64, test_value, off);
        if (fptr < 0) return false;
        int val = nyir_emit(&b->nyir, (nyir_inst_t){.op = NYIR_LOAD_I64,
                                                   .dst = -1,
                                                   .a = fptr});
        if (val < 0) return false;
        ny_native_nir_local_t *local =
            ny_native_nir_bind_local(b, arg->as.ident.name);
        if (!local || !ny_native_nir_store_local_value(b, local->slot, val))
          return false;
      }
      return true;
    }
  }

  return true;
}
static int ny_native_nir_lower_match_patterns(ny_native_nir_builder_t *b,
                                              int test_value,
                                              const match_arm_t *arm,
                                              bool test_any) {
  if (!arm || arm->patterns.len == 0)
    return ny_native_nir_emit_const(b, 0);
  int combined = -1;
  for (size_t i = 0; i < arm->patterns.len; ++i) {
    int cur = ny_native_nir_lower_match_pattern(b, test_value,
                                                arm->patterns.data[i], test_any);
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

/*
 * A small, dense integer match can use a comparison tree instead of testing
 * every arm in source order. Keep this deliberately narrow: guarded arms,
 * ranges, duplicate literals, and sparse values retain the general matcher
 * below so first-match semantics and arbitrary pattern evaluation stay exact.
 */
typedef struct {
  int64_t value;
  size_t arm_index;
  int label;
} ny_native_dense_case_t;

static bool ny_native_nir_collect_dense_cases(
    const stmt_t *s, ny_native_dense_case_t *cases, size_t *count_out) {
  if (!s || !cases || !count_out || s->kind != NY_S_MATCH ||
      s->as.match.arms.len < 3 || s->as.match.arms.len > 32)
    return false;

  size_t count = 0;
  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    const match_arm_t *arm = &s->as.match.arms.data[i];
    if (arm->guard || arm->patterns.len != 1)
      return false;
    const expr_t *pat = arm->patterns.data[0];
    if (!pat || pat->kind != NY_E_LITERAL ||
        pat->as.literal.kind != NY_LIT_INT || pat->tok.kind == NY_T_NIL)
      return false;

    int64_t value = pat->as.literal.as.i;
    size_t pos = count;
    while (pos > 0 && cases[pos - 1].value > value) {
      cases[pos] = cases[pos - 1];
      --pos;
    }
    if ((pos > 0 && cases[pos - 1].value == value) ||
        (pos < count && cases[pos].value == value))
      return false;
    cases[pos] = (ny_native_dense_case_t){
        .value = value, .arm_index = i, .label = -1};
    ++count;
  }

  uint64_t span = (uint64_t)cases[count - 1].value -
                  (uint64_t)cases[0].value;
  if (span > (uint64_t)count * 2u)
    return false;
  *count_out = count;
  return true;
}

static bool ny_native_nir_emit_dense_dispatch(
    ny_native_nir_builder_t *b, const ny_native_dense_case_t *cases,
    size_t lo, size_t hi, int test_value, int default_label) {
  if (lo > hi)
    return ny_native_nir_emit_br(b, default_label);
  size_t mid = lo + (hi - lo) / 2;
  int rhs = ny_native_nir_emit_const(b, cases[mid].value);
  if (rhs < 0)
    return false;
  if (lo == hi) {
    int equal = ny_native_nir_emit_cmp(b, test_value, rhs, NYIR_CMP_EQ);
    return equal >= 0 && ny_native_nir_emit_br_if(b, equal, cases[mid].label) &&
           ny_native_nir_emit_br(b, default_label);
  }

  int left_label = b->next_label++;
  int right_label = b->next_label++;
  int at_most = ny_native_nir_emit_cmp(b, test_value, rhs, NYIR_CMP_LE);
  if (at_most < 0 || !ny_native_nir_emit_br_if(b, at_most, left_label) ||
      !ny_native_nir_emit_br(b, right_label) ||
      !ny_native_nir_emit_label(b, left_label) ||
      !ny_native_nir_emit_dense_dispatch(b, cases, lo, mid, test_value,
                                         default_label) ||
      !ny_native_nir_emit_label(b, right_label) ||
      !ny_native_nir_emit_dense_dispatch(b, cases, mid + 1, hi, test_value,
                                         default_label))
    return false;
  return true;
}

static bool ny_native_nir_lower_dense_match(ny_native_nir_builder_t *b,
                                            const stmt_t *s, int test_value,
                                            int result_slot, int merge_label,
                                            bool entry_return,
                                            bool *lowered_out) {
  ny_native_dense_case_t cases[32];
  size_t count = 0;
  if (!ny_native_nir_collect_dense_cases(s, cases, &count)) {
    *lowered_out = false;
    return true;
  }

  int default_label = b->next_label++;
  int lower_label = b->next_label++;
  int dispatch_label = b->next_label++;
  for (size_t i = 0; i < count; ++i)
    cases[i].label = b->next_label++;

  int min_value = ny_native_nir_emit_const(b, cases[0].value);
  int max_value = ny_native_nir_emit_const(b, cases[count - 1].value);
  int at_least = min_value < 0 ? -1
                               : ny_native_nir_emit_cmp(b, test_value, min_value,
                                                        NYIR_CMP_GE);
  int at_most = max_value < 0 ? -1
                              : ny_native_nir_emit_cmp(b, test_value, max_value,
                                                       NYIR_CMP_LE);
  if (at_least < 0 || at_most < 0 ||
      !ny_native_nir_emit_br_if(b, at_least, lower_label) ||
      !ny_native_nir_emit_br(b, default_label) ||
      !ny_native_nir_emit_label(b, lower_label) ||
      !ny_native_nir_emit_br_if(b, at_most, dispatch_label) ||
      !ny_native_nir_emit_br(b, default_label) ||
      !ny_native_nir_emit_label(b, dispatch_label) ||
      !ny_native_nir_emit_dense_dispatch(b, cases, 0, count - 1, test_value,
                                         default_label))
    return false;

  bool all_taken_paths_return = true;
  for (size_t arm_index = 0; arm_index < s->as.match.arms.len; ++arm_index) {
    size_t case_index = count;
    for (size_t i = 0; i < count; ++i) {
      if (cases[i].arm_index == arm_index) {
        case_index = i;
        break;
      }
    }
    if (case_index == count ||
        !ny_native_nir_emit_label(b, cases[case_index].label))
      return false;
    b->last_value = -1;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(
            b, s->as.match.arms.data[arm_index].conseq))
      return false;
    if (!b->emitted_return) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }
  }

  if (s->as.match.default_conseq) {
    if (!ny_native_nir_emit_label(b, default_label))
      return false;
    b->last_value = -1;
    b->emitted_return = false;
    if (!ny_native_nir_lower_scoped_body(b, s->as.match.default_conseq))
      return false;
    if (!b->emitted_return) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }
  } else {
    all_taken_paths_return = false;
    if (!ny_native_nir_emit_label(b, default_label) ||
        !ny_native_nir_emit_br(b, merge_label))
      return false;
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
  *lowered_out = true;
  return true;
}

static bool ny_native_nir_match_arm_statically_dead(
    const expr_t *test, const match_arm_t *arm) {
  if (!test || test->kind != NY_E_LITERAL || !arm || arm->patterns.len == 0)
    return false;
  if (test->as.literal.kind == NY_LIT_INT) {
    int64_t tv = test->as.literal.as.i;
    for (size_t i = 0; i < arm->patterns.len; ++i) {
      const expr_t *p = arm->patterns.data[i];
      if (!p || p->kind != NY_E_LITERAL || p->as.literal.kind != NY_LIT_INT)
        return false;
      if (p->as.literal.as.i == tv)
        return false;
    }
    return true;
  }
  return false;
}

static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test)
    return ny_native_nir_fail(b, "native NYIR lower: malformed case/match");

  size_t static_arm = 0;
  int static_selection = ny_native_nir_static_match_arm(s, &static_arm);
  if (static_selection == 1)
    return ny_native_nir_lower_scoped_body(
        b, s->as.match.arms.data[static_arm].conseq);
  if (static_selection == 2)
    return !s->as.match.default_conseq ||
           ny_native_nir_lower_scoped_body(b, s->as.match.default_conseq);

  int test_value = ny_native_nir_lower_expr(b, s->as.match.test);
  if (test_value < 0)
    return false;
  bool test_any = ny_native_nir_expr_is_any(b, s->as.match.test);

  int result_slot = ny_native_nir_temp_slot(b);
  int merge_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  bool all_taken_paths_return = true;

  int initial = entry_last_value >= 0 ? entry_last_value
                                      : ny_native_nir_emit_const(b, 0);
  if (initial < 0 || !ny_native_nir_store_local_value(b, result_slot, initial))
    return false;
  bool dense_lowered = false;
  if (!ny_native_nir_lower_dense_match(
          b, s, test_value, result_slot, merge_label, entry_return,
          &dense_lowered))
    return false;
  if (dense_lowered)
    return true;

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    size_t arm_scope = ny_native_nir_scope_mark(b);
    if (ny_native_nir_match_arm_statically_dead(s->as.match.test, arm)) {
      ny_native_nir_scope_restore(b, arm_scope);
      continue;
    }
    int next_label = b->next_label++;
    int pat = ny_native_nir_lower_match_patterns(b, test_value, arm, test_any);
    if (pat < 0)
      return false;
    int pat_false = ny_native_nir_emit_is_zero(b, pat);
    if (pat_false < 0 || !ny_native_nir_emit_br_if(b, pat_false, next_label))
      return false;

    if (!ny_native_nir_bind_result_pattern(
            b, test_value,
            arm->patterns.len == 1 ? arm->patterns.data[0] : NULL))
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
    ny_native_nir_scope_restore(b, arm_scope);

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

static int ny_native_nir_normalize_return(ny_native_nir_builder_t *b,
                                          const expr_t *expr, int value) {
  if (!b || value < 0)
    return value;
  if (!expr) {
    return value;
  }
  /* Nullable returns use the tagged nil immediate at their ABI boundary.
   * NYIR's ordinary scalar literal lowering keeps nil as raw zero for
   * arithmetic compatibility, but returning that zero from `?T` makes
   * `is_nil` indistinguishable from integer zero. */
  if (expr->tok.kind == NY_T_NIL && b->return_type &&
      (b->return_type[0] == '?' || strchr(b->return_type, '?') != NULL))
    return ny_native_nir_emit_const(b, NY_IMM_NIL);
  /* Captured lambda frames store scalar environment slots in the raw NYIR
   * ABI.  Calls through a closure are dynamic, so a scalar returned directly
   * from a captured local must be boxed before it leaves the lambda frame. */
  if (b->lambda_entry && b->lambda_entry->capture_count > 0 &&
      expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const ny_native_nir_local_t *local =
        ny_native_nir_find_local(b, expr->as.ident.name);
    if (local && local->is_capture && !local->is_any && !local->is_cstr &&
        !local->is_bytes && !local->is_dict && !local->is_list &&
        !local->is_bigint && !local->is_f64 && !local->is_f32)
      return ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1, 1,
                                             0);
  }
  if (b->return_any) {
    /* Untyped functions expose the dynamic tagged ABI. Literal integers are
     * still emitted as raw NYIR constants, so box only this unambiguous
     * producer; calls and dynamic locals already carry their ABI value. */
    if (expr->kind == NY_E_LITERAL &&
        expr->as.literal.kind == NY_LIT_INT &&
        expr->tok.kind != NY_T_NIL)
      return ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
      const ny_native_nir_local_t *local =
          ny_native_nir_find_local(b, expr->as.ident.name);
      if (local && local->is_capture && !local->is_any &&
          !local->is_cstr && !local->is_bytes && !local->is_dict &&
          !local->is_list && !local->is_bigint && !local->is_f64 &&
          !local->is_f32)
        return ny_native_nir_emit_runtime_call(b, "rt_tag", value, -1, -1, 1,
                                               0);
    }
    return value;
  }
  if (b->return_type && !ny_native_type_name_is_int(b->return_type))
    return value;
  /* Arithmetic over dynamic values already returns a raw native scalar from
   * rt_any_*; only an unmodified any local still carries the tagged
   * VM integer encoding at this boundary. */
  bool dynamic = false;
  /* Dictionary accessors expose tagged values, unlike typed-buffer reads.
   * Decode that explicit ABI when returning a declared integer. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      expr->kind == NY_E_MEMCALL && expr->as.memcall.name &&
      strcmp(expr->as.memcall.name, "get") == 0 &&
      !ny_native_nir_expr_is_raw_dynamic_read(b, expr))
    dynamic = true;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const ny_native_nir_local_t *local =
        ny_native_nir_find_local(b, expr->as.ident.name);
    dynamic = dynamic || (local && local->is_any);
  }
  /* Native container reads already return their scalar slot payload in the
   * raw NYIR ABI.  Do not propagate their source-level `any` annotation to a
   * function return: doing so unboxes raw odd values a second time (notably
   * 32-bit words returned from a typed buffer). */
  if (!dynamic)
    return value;
  return ny_native_nir_emit_runtime_call(b, "rt_any_to_i64", value,
                                         -1, -1, 1, 0);
}

static ny_native_nir_label_t *ny_native_nir_named_label(
    ny_native_nir_builder_t *b, const char *name) {
  if (!b || !name) return NULL;
  for (size_t i = 0; i < b->label_count; ++i)
    if (b->labels[i].name && strcmp(b->labels[i].name, name) == 0)
      return &b->labels[i];
  if (b->label_count == b->label_cap) {
    size_t cap = b->label_cap ? b->label_cap * 2 : 8;
    ny_native_nir_label_t *grown = realloc(b->labels, cap * sizeof(*grown));
    if (!grown) return NULL;
    b->labels = grown;
    b->label_cap = cap;
  }
  ny_native_nir_label_t *entry = &b->labels[b->label_count++];
  *entry = (ny_native_nir_label_t){.name = name, .label = b->next_label++};
  return entry;
}

static bool ny_native_nir_direct_thread_call(ny_native_nir_builder_t *b,
                                             const expr_t *e) {
  if (!b || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return false;
  const stmt_t *fn = ny_native_nir_find_user_function(
      b, e->as.call.callee->as.ident.name);
  return fn && ny_native_nir_fn_has_thread_attr(fn);
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s && s->kind == NY_S_MODULE && b->profile_name &&
      strcmp(b->profile_name, "rt_main") == 0) {
    if (ny_native_stmt_is_stdlib(s))
      return true;
    char init_name[512];
    int n = snprintf(init_name, sizeof(init_name), "%s.__init__",
                     s->as.module.name ? s->as.module.name : "module");
    if (n < 0 || (size_t)n >= sizeof(init_name))
      return ny_native_nir_fail(b, "native NYIR: module initializer name too long");
    const char *saved_name = b->current_fn_name;
    b->current_fn_name = init_name;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *item = s->as.module.body.data[i];
      if (item && item->kind == NY_S_MODULE) {
        if (!ny_native_nir_lower_stmt(b, item)) {
          b->current_fn_name = saved_name;
          return false;
        }
      } else if (item && item->kind == NY_S_VAR && item->as.var.is_mut &&
                 !ny_native_nir_lower_stmt(b, item)) {
        b->current_fn_name = saved_name;
        return false;
      }
    }
    b->current_fn_name = saved_name;
    return true;
  }
  if (ny_native_nir_ignored_stmt(s) ||
      (s && (s->kind == NY_S_FUNC || s->kind == NY_S_LEMMA)))
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    /* The function body is the root scope.  Top-level rt_main declarations
     * intentionally use global storage, while declarations in nested blocks
     * must receive lexical locals even when their spelling matches a global
     * binding. */
    bool nested_scope = s != b->tail_body && !s->as.block.transparent;
    if (nested_scope)
      b->scope_depth++;
    size_t mark = s->as.block.transparent ? b->local_count
                                           : ny_native_nir_scope_mark(b);
    size_t defer_mark = b->defer_count;
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *item = s->as.block.body.data[i];
      if (b->emitted_return && (!item || item->kind != NY_S_LABEL))
        continue;
      if (i + 1 == s->as.block.body.len && s == b->tail_body &&
          item && item->kind == NY_S_EXPR) {
        int tail = ny_native_nir_lower_tail_return(b, item->as.expr.expr);
        if (tail < 0) {
          if (nested_scope)
            b->scope_depth--;
          if (!s->as.block.transparent)
            ny_native_nir_scope_restore(b, mark);
          return false;
        }
        if (tail > 0)
          break;
      }
      if (!ny_native_nir_lower_stmt(b, item)) {
        if (nested_scope)
          b->scope_depth--;
        if (!s->as.block.transparent)
          ny_native_nir_scope_restore(b, mark);
        return false;
      }
      /* Keep scanning so a later named label can reopen a reachable block. */
    }
    if (!s->as.block.transparent && !b->capture_defers) {
      /*
       * Defer bodies lower as ordinary statements and would clobber
       * last_value; the block's trailing expression must win for
       * implicit returns.
       */
      int saved_last = b->last_value;
      if ((!b->capture_defers || b->defer_capture_mark != defer_mark) &&
          !ny_native_nir_emit_defers(b, defer_mark)) {
        b->last_value = saved_last;
        if (nested_scope)
          b->scope_depth--;
        return false;
      }
      b->last_value = saved_last;
      ny_native_nir_scope_restore(b, mark);
    }
    if (nested_scope)
      b->scope_depth--;
    return true;
  }
  case NY_S_VAR:
    return ny_native_nir_lower_var(b, s);
  case NY_S_EXPR: {
    bool prev_thread_detach = b->thread_detach_stmt_call;
    if (ny_native_nir_direct_thread_call(b, s->as.expr.expr))
      b->thread_detach_stmt_call = true;
    int v = ny_native_nir_lower_expr(b, s->as.expr.expr);
    b->thread_detach_stmt_call = prev_thread_detach;
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
  case NY_S_GUARD:
    return ny_native_nir_lower_guard(b, s);
  case NY_S_WHILE:
    return ny_native_nir_lower_while(b, s);
  case NY_S_FOR:
    return ny_native_nir_lower_for(b, s);
  case NY_S_TRY:
    /*
     * Keep native try/catch ABI-compatible with the LLVM path.  The runtime
     * owns the panic stack, while setjmp itself must execute in this native
     * frame so rt_panic can longjmp back here safely.  All values that must
     * survive the call live in NYIR locals/stack slots, matching the normal
     * native calling convention.
     */
    {
      int jmpbuf = nyir_emit(&b->nyir, (nyir_inst_t){
          .op = NYIR_ALLOCA, .dst = -1, .a = -1, .b = -1, .c = -1,
          .imm = (int64_t)sizeof(jmp_buf) + 16});
      int set_env = jmpbuf < 0 ? -1 : ny_native_nir_emit_runtime_call(
          b, "rt_set_panic_env", jmpbuf, -1, -1, 1, 0);
      int sj = set_env < 0 ? -1 : ny_native_nir_emit_runtime_call(
          b, "_setjmp", jmpbuf, -1, -1, 1, 0);
      int zero = sj < 0 ? -1 : ny_native_nir_emit_const(b, 0);
      int resumed = sj < 0 || zero < 0 ? -1 : nyir_emit(
          &b->nyir, (nyir_inst_t){.op = NYIR_CMP_I64, .dst = -1,
                                  .a = sj, .b = zero, .cmp = NYIR_CMP_EQ});
      int try_label = b->next_label++;
      int catch_label = b->next_label++;
      int end_label = b->next_label++;
      int result_slot = ny_native_nir_temp_slot(b);
      if (jmpbuf < 0 || set_env < 0 || sj < 0 || zero < 0 || resumed < 0 ||
          result_slot < 0 || !ny_native_nir_emit_br_if(b, resumed, try_label) ||
          !ny_native_nir_emit_br(b, catch_label) ||
          !ny_native_nir_emit_label(b, try_label))
        return false;

      bool entry_return = b->emitted_return;
      size_t try_scope_mark = b->local_count;
      size_t try_defer_mark = b->defer_count;
      bool saved_capture_defers = b->capture_defers;
      size_t saved_capture_mark = b->defer_capture_mark;
      b->capture_defers = true;
      b->defer_capture_mark = try_defer_mark;
      b->emitted_return = false;
      b->last_value = -1;
      if (s->as.tr.body && !ny_native_nir_lower_stmt(b, s->as.tr.body))
        return false;
      bool body_return = b->emitted_return;
      int body_value = b->last_value;
      size_t captured_count = b->defer_count - try_defer_mark;
      stmt_t **captured_defers = NULL;
      if (captured_count) {
        captured_defers = malloc(captured_count * sizeof(*captured_defers));
        if (!captured_defers) {
          b->capture_defers = saved_capture_defers;
          b->defer_capture_mark = saved_capture_mark;
          return ny_native_nir_fail(b, NY_NATIVE_ALLOC_FAIL);
        }
        memcpy(captured_defers, b->defers + try_defer_mark,
               captured_count * sizeof(*captured_defers));
      }
      b->defer_count = try_defer_mark;
      b->capture_defers = saved_capture_defers;
      b->defer_capture_mark = saved_capture_mark;
      if (!body_return) {
        if (body_value < 0)
          body_value = zero;
        if (body_value < 0 ||
            !ny_native_nir_store_local_value(b, result_slot, body_value))
          goto try_fail;
        for (size_t i = captured_count; i > 0; --i) {
          if (!captured_defers[i - 1] ||
              !ny_native_nir_lower_stmt(b, captured_defers[i - 1]))
            goto try_fail;
        }
        if (ny_native_nir_emit_runtime_call(b, "rt_clear_panic_env", -1, -1,
                                            -1, 0, 0) < 0 ||
            !ny_native_nir_emit_br(b, end_label))
          goto try_fail;
      }

      b->emitted_return = false;
      if (!ny_native_nir_emit_label(b, catch_label))
        goto try_fail;
      for (size_t i = captured_count; i > 0; --i) {
        if (!captured_defers[i - 1] ||
            !ny_native_nir_lower_stmt(b, captured_defers[i - 1]))
          goto try_fail;
      }
      /* Cleanup must run while the panic environment is still active.  A
       * defer may update globals, release an owned value, or itself report a
       * secondary failure; clearing the environment first made the panic
       * path observably skip the original resource cleanup in native code. */
      if (ny_native_nir_emit_runtime_call(b, "rt_clear_panic_env", -1, -1,
                                          -1, 0, 0) < 0)
        goto try_fail;
      b->last_value = -1;
      int error = ny_native_nir_emit_runtime_call(
          b, "rt_get_panic_val", -1, -1, -1, 0, 0);
      if (error < 0)
        return false;
      if (s->as.tr.err && s->as.tr.err[0] &&
          strcmp(s->as.tr.err, "_") != 0) {
        ny_native_nir_local_t *err_local =
            ny_native_nir_bind_local(b, s->as.tr.err);
        if (err_local)
          err_local->is_any = true;
        if (!err_local || !ny_native_nir_store_local_value(b, err_local->slot,
                                                            error))
          return false;
      }
      if (s->as.tr.handler &&
          !ny_native_nir_lower_stmt(b, s->as.tr.handler))
        return false;
      bool handler_return = b->emitted_return;
      int handler_value = b->last_value;
      if (!handler_return) {
        if (handler_value < 0)
          handler_value = zero;
        if (handler_value < 0 ||
            !ny_native_nir_store_local_value(b, result_slot, handler_value) ||
            !ny_native_nir_emit_br(b, end_label))
          return false;
      }
      if (body_return && handler_return)
        b->emitted_return = true;
      else {
        b->emitted_return = entry_return;
        if (!ny_native_nir_emit_label(b, end_label))
          return false;
        /* A try statement is an expression-like statement in the AST.  The
         * arms have already written their value to the join slot, so the
         * merge is independent of labels introduced inside either arm. */
        if (!body_return && !handler_return) {
          b->last_value = ny_native_nir_load_local_value(b, result_slot);
          if (b->last_value < 0)
            return false;
        } else if (!body_return) {
          b->last_value = ny_native_nir_load_local_value(b, result_slot);
          if (b->last_value < 0)
            return false;
        } else if (!handler_return) {
          b->last_value = ny_native_nir_load_local_value(b, result_slot);
          if (b->last_value < 0)
            return false;
        }
      }
      ny_native_nir_scope_restore(b, try_scope_mark);
      free(captured_defers);
      return true;
try_fail:
      ny_native_nir_scope_restore(b, try_scope_mark);
      free(captured_defers);
      return false;
    }
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
    v = ny_native_nir_normalize_return(b, s->as.ret.value, v);
    if (v < 0)
      return false;
    if (!ny_native_nir_emit_defers(b, 0))
      return false;
    return ny_native_nir_emit_ret(b, v);
  }
  case NY_S_LABEL: {
    ny_native_nir_label_t *label = ny_native_nir_named_label(b, s->as.label.name);
    if (!label)
      return ny_native_nir_fail(b, "native NYIR lower: cannot allocate label '%s'",
                                s->as.label.name ? s->as.label.name : "");
    if (label->emitted)
      return ny_native_nir_fail(b, "native NYIR lower: duplicate label '%s'",
                                s->as.label.name);
    label->emitted = true;
    b->emitted_return = false;
    return ny_native_nir_emit_label(b, label->label);
  }
  case NY_S_GOTO: {
    ny_native_nir_label_t *label = ny_native_nir_named_label(b, s->as.go.name);
    if (!label)
      return ny_native_nir_fail(b, "native NYIR lower: cannot allocate goto '%s'",
                                s->as.go.name ? s->as.go.name : "");
    if (!ny_native_nir_emit_defers(b, 0) ||
        !ny_native_nir_emit_br(b, label->label))
      return false;
    b->emitted_return = true;
    b->last_value = -1;
    return true;
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
  if (ny_trace_enabled("NY_TRACE_O3") && b->opt_level >= 3)
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
  nyir_dump_compact(stderr, &b->nyir, "<failed>");
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
static bool ny_native_nir_stmt_has_return(const stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_RETURN)
    return true;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (ny_native_nir_stmt_has_return(s->as.block.body.data[i]))
        return true;
  } else if (s->kind == NY_S_IF) {
    return ny_native_nir_stmt_has_return(s->as.iff.conseq) ||
           ny_native_nir_stmt_has_return(s->as.iff.alt);
  } else if (s->kind == NY_S_WHILE) {
    return ny_native_nir_stmt_has_return(s->as.whl.body);
  } else if (s->kind == NY_S_FOR) {
    return ny_native_nir_stmt_has_return(s->as.fr.body);
  } else if (s->kind == NY_S_GUARD) {
    return ny_native_nir_stmt_has_return(s->as.guard.fallback);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (ny_native_nir_stmt_has_return(s->as.match.arms.data[i].conseq))
        return true;
    return ny_native_nir_stmt_has_return(s->as.match.default_conseq);
  }
  return false;
}

static bool ny_native_nir_stmt_has_control_flow(const stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_IF || s->kind == NY_S_WHILE ||
      s->kind == NY_S_FOR || s->kind == NY_S_GUARD || s->kind == NY_S_MATCH)
    return true;
  if (s->kind == NY_S_BLOCK)
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (ny_native_nir_stmt_has_control_flow(s->as.block.body.data[i]))
        return true;
  return false;
}

static bool ny_native_nir_build_function(const program_t *prog, const stmt_t *fn,
                                        const ny_extern_table_t *externs,
                                        nyir_func_t *out, char *err,
                                        size_t err_len, int opt_level,
                                        const ny_options *options) {
  if (!fn || fn->kind != NY_S_FUNC || !out)
    return false;
  const ny_native_lambda_entry_t *lambda_entry =
      ny_native_lambda_find_name(fn->as.fn.name);
  memset(out, 0, sizeof(*out));
  int function_opt_level = fn->as.fn.attr_optimize
                               ? fn->as.fn.attr_optimize_level
                               : opt_level;
  /* Broad import surfaces can make hundreds of stdlib helpers reachable.
   * Their per-function O3 cleanup dominates compile time while the hot
   * entrypoint is optimized separately; keep imported library bodies at the
   * lightweight baseline unless they explicitly request a level. */
  if (!fn->as.fn.attr_optimize && ny_is_stdlib_tok(fn->tok) &&
      function_opt_level > 0)
    function_opt_level = 0;
  /* Native NYIR carries raw C-string pointers for stdlib ABI calls.  The
   * legacy stdlib bodies still use boxed-string trace inspection, so tracing
   * those bodies would reinterpret raw addresses and crash while printing a
   * frame.  Keep user/source frames and rt_main instrumented until that ABI
   * boundary is unified. */
  ny_native_nir_builder_t b = {
      .last_value = -1,
      .err = err,
      .err_len = err_len,
      .externs = externs,
      .prog = prog,
      .options = options,
      .profile_name = fn->as.fn.name ? fn->as.fn.name : "<fn>",
      .source_file = fn->tok.filename,
      .module_name = ny_native_fn_module(prog, fn),
      .lambda_entry = lambda_entry,
      .closure_env_slot = -1,
      .trace_instrumented = ny_native_nir_trace_requested(options) &&
                            (!fn->tok.filename ||
                             !strstr(fn->tok.filename, "/lib/core/")),
      .opt_level = function_opt_level};
  /* An explicit source return type is authoritative.  In particular,
   * generated captured lambdas may retain a conservative tagged semantic
   * fact from the original expression node, but `fn(...) int { ... }` still
   * has a raw integer return ABI. */
  b.return_any =
      (fn->as.fn.return_type && ny_native_type_name_is_any(fn->as.fn.return_type)) ||
      (!fn->as.fn.return_type && fn->as.fn.return_semantic.resolved &&
       fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC);
  b.return_type = fn ? fn->as.fn.return_type : NULL;
  if ((fn->as.fn.return_semantic.resolved &&
       fn->as.fn.return_semantic.rep == NY_SEM_REP_F64) ||
      ny_native_type_name_is_f64(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F64;
  else if ((fn->as.fn.return_semantic.resolved &&
            fn->as.fn.return_semantic.rep == NY_SEM_REP_F32) ||
           ny_native_type_name_is_f32(fn->as.fn.return_type))
    b.return_flags = NYIR_INST_F_RET_F32;
  if (!ny_native_nir_set_param_types(&b, fn)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  if (lambda_entry && lambda_entry->capture_count > 0) {
    ny_native_nir_local_t *env =
        ny_native_nir_bind_local_typed(&b, "__ny_env", false, false, false);
    if (!env) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    env->arg_slot = 0;
    b.closure_env_slot = env->slot;
    int env_arg = nyir_emit(&b.nyir, (nyir_inst_t){
        .op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = 0});
    if (env_arg < 0 || !ny_native_nir_store_local_value(&b, env->slot, env_arg)) {
      nyir_func_free(&b.nyir);
      ny_native_nir_builder_dispose(&b);
      return false;
    }
    for (size_t i = 0; i < lambda_entry->capture_count; ++i) {
      ny_native_nir_local_t *capture = ny_native_nir_bind_local(&b,
                                                   lambda_entry->capture_names[i]);
      if (!capture) {
        nyir_func_free(&b.nyir);
        ny_native_nir_builder_dispose(&b);
        return false;
      }
      capture->is_capture = true;
      capture->capture_index = i;
      const ny_native_nir_local_t *src = &lambda_entry->capture_types[i];
      capture->is_list = src->is_list;
      capture->is_dyn_list = src->is_dyn_list;
      capture->is_cstr = src->is_cstr;
      capture->is_bytes = src->is_bytes;
      capture->is_dict = src->is_dict;
      capture->is_any = src->is_any;
      capture->is_f64 = src->is_f64;
      capture->is_f32 = src->is_f32;
      capture->is_bool = src->is_bool;
      capture->semantic_rep = src->semantic_rep;
      capture->semantic_ownership = src->semantic_ownership;
      capture->alias_class = src->alias_class;
      capture->semantic_mutable = src->semantic_mutable;
      capture->fin_bound = src->fin_bound;
    }
  }
  if (fn->as.fn.attr_proves) {
    char *full = ny_proof_type_from_expr(fn->as.fn.attr_proves);
    if (full) {
      const char *inner = full;
      if (strncmp(inner, "proof<", 6) == 0)
        inner += 6;
      size_t ilen = strlen(inner);
      char *canon = (ilen > 0 && inner[ilen - 1] == '>')
                        ? ny_strndup(inner, ilen - 1)
                        : ny_strdup(inner);
      const expr_t *rets[16] = {0};
      size_t nret = 0;
      if (fn->as.fn.body) {
        if (fn->as.fn.body->kind == NY_S_BLOCK && fn->as.fn.body->as.block.body.len > 0) {
          stmt_t *last = fn->as.fn.body->as.block.body.data[fn->as.fn.body->as.block.body.len - 1];
          if (last->kind == NY_S_EXPR && last->as.expr.expr)
            rets[nret++] = last->as.expr.expr;
        }
      }
      for (size_t r = 0; r < nret; ++r) {
        int64_t val = 0;
        int d = 0;
        if (rets[r] && ny_native_nir_eval_proof_i64(&b, rets[r], 0, &val)) {
          char replaced[512] = {0};
          char *hit = strstr(canon, "name:result");
          if (hit) {
            size_t prefix_len = (size_t)(hit - canon);
            snprintf(replaced, sizeof(replaced), "%.*sint:%lld%s",
                     (int)prefix_len, canon, (long long)val,
                     hit + strlen("name:result"));
            d = ny_proof_canon_decide(replaced, NULL, NULL, 0);
          } else {
            hit = strstr(canon, "result");
            if (hit) {
              size_t prefix_len = (size_t)(hit - canon);
              snprintf(replaced, sizeof(replaced), "%.*s%lld%s",
                       (int)prefix_len, canon, (long long)val,
                       hit + strlen("result"));
              d = ny_proof_canon_decide(replaced, NULL, NULL, 0);
            }
          }
        }
        if (d != 1) {
          ny_native_nir_fail(&b,
              "@proves condition '%s' is not proved for a returned value range",
              canon);
          free(canon);
          free(full);
          nyir_func_free(&b.nyir);
          ny_native_nir_builder_dispose(&b);
          return false;
        }
      }
      free(canon);
      free(full);
    }
  }
  int arg_index = lambda_entry && lambda_entry->capture_count > 0 ? 1 : 0;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const char *param_type = fn->as.fn.params.data[i].type;
    const ny_expr_semantic_t *sem = &fn->as.fn.params.data[i].semantic;
    bool is_list = ny_native_type_name_is_list(param_type) ||
                   ny_native_nir_param_is_inferred_list(fn, i) ||
                   (!param_type && sem->resolved && sem->rep == NY_SEM_REP_TYPED_BUFFER) ||
                   (fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1);
    bool is_str = ny_native_type_name_is_str(param_type) ||
                  (!param_type && sem->resolved && sem->rep == NY_SEM_REP_STRING);
    bool is_any = ny_native_type_name_is_any(param_type) ||
                  (!param_type && sem->resolved && sem->rep == NY_SEM_REP_TAGGED_DYNAMIC) ||
                  (!param_type && !sem->resolved);
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
    param->type_name = param_type;
    param->is_bigint = param_type && strcmp(param_type, "bigint") == 0;
    param->semantic_rep = sem->resolved ? sem->rep : param->semantic_rep;
    param->semantic_ownership = sem->ownership;
    param->alias_class = sem->alias_class;
    param->semantic_mutable = sem->mutable_value;
    param->is_dict = (sem->resolved && sem->rep == NY_SEM_REP_OBJECT) ||
                     (param_type && (strcmp(param_type, "dict") == 0 ||
                                     strcmp(param_type, "vec2") == 0 ||
                                     strcmp(param_type, "vec3") == 0 ||
                                     strcmp(param_type, "vec4") == 0));
    param->is_list = is_list;
    param->is_dyn_list =
        (is_list &&
         (ny_native_type_name_is_dyn_list(param_type) ||
          ny_native_nir_function_param_is_dyn_list(prog, fn, i)));
    param->is_cstr = is_str;
    param->is_bytes = ny_native_type_name_is_bytes(param_type);
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
        ny_native_resolve_fin_bound(&b, param_type);
  }
  b.current_fn_name = fn->as.fn.name;
  b.nyir.param_count = (unsigned)arg_index;
  if (!ny_native_nir_emit_trace_enter(&b, fn->as.fn.name, fn->tok.filename,
                                      fn->tok.line)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  /*
   * Function prologue: move expanded arguments from their incoming
   * positions (ld #arg_slot) into the local slots that the body expects.
   */
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    ny_native_nir_local_t *param = ny_native_nir_find_local(&b, fn->as.fn.params.data[i].name);
    if (!param)
      continue;
    if (param->arg_slot >= 0) {
      nyir_inst_t param_ld = {.op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = param->arg_slot};
      if (param->semantic_rep)
        param_ld.semantic_rep = (uint8_t)param->semantic_rep;
      if (param->semantic_ownership)
        param_ld.semantic_ownership = (uint8_t)param->semantic_ownership;
      if (param->alias_class)
        param_ld.alias_class = param->alias_class;
      param_ld.semantic_mutable = param->semantic_mutable;
      if (param->fin_bound > 0) {
        param_ld.range.has_min = true;
        param_ld.range.has_max = true;
        param_ld.range.min = 0;
        param_ld.range.max = param->fin_bound - 1;
      }
      int arg_val = nyir_emit(&b.nyir, param_ld);
      if (arg_val >= 0) {
        /* Captured closures enter through their closure trampoline with a
         * tagged dynamic value.  Zero-capture functions instead use the raw
         * callback adapter, so decoding those here would turn raw 1 into 0. */
        if (lambda_entry && lambda_entry->capture_count > 0 && fn->as.fn.name &&
            strncmp(fn->as.fn.name, "__ny_lambda_", 12) == 0 &&
            !param->is_any && !param->is_list && !param->is_cstr &&
            !param->is_f64 && !param->is_f32) {
          arg_val = ny_native_nir_emit_runtime_call(
              &b, "rt_any_to_i64", arg_val, -1, -1, 1, 0);
        }
        /*
         * List parameters carry their length in the next ABI slot. Do not
         * inspect the pointer as a managed tbuf here: constant pooled arrays
         * have no tbuf header, and the companion slot is authoritative.
         */
        if (param->is_list && param->list_len_arg_slot >= 0) {
          size_t before = b.nyir.len;
          nyir_emit(&b.nyir, (nyir_inst_t){.op = NYIR_STORE_LOCAL, .dst = -1,
                                            .a = arg_val, .b = -1,
                                            .imm = param->slot});
          if (b.nyir.len == before) {
            nyir_func_free(&b.nyir);
            ny_native_nir_builder_dispose(&b);
            return false;
          }
        } else if (!ny_native_nir_store_local_value(&b, param->slot, arg_val)) {
          nyir_func_free(&b.nyir);
          ny_native_nir_builder_dispose(&b);
          return false;
        }
      }
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
  /* Tail-call lowering is only valid when the recursive call itself is the
   * returned value.  A binary expression such as
   * `1 + tree(depth - 1) + tree(depth - 1)` contains non-tail recursive
   * calls; enabling the parameter-rewrite loop for it corrupts the ordinary
   * call frame and crashes recursive tree benchmarks. */
  if (scalar_tail && fn->as.fn.body->kind == NY_S_BLOCK &&
      fn->as.fn.body->as.block.body.len > 0) {
    const stmt_t *last = fn->as.fn.body->as.block.body.data[
        fn->as.fn.body->as.block.body.len - 1];
    if (last && last->kind == NY_S_EXPR && last->as.expr.expr &&
        last->as.expr.expr->kind == NY_E_BINARY)
      scalar_tail = false;
  }
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
  bool has_implicit_tail =
      fn->as.fn.body &&
      (fn->as.fn.body->kind == NY_S_EXPR ||
       (fn->as.fn.body->kind == NY_S_BLOCK &&
        fn->as.fn.body->as.block.body.len > 0 &&
        fn->as.fn.body->as.block.body.data[
            fn->as.fn.body->as.block.body.len - 1]->kind == NY_S_EXPR));
  if (ok && !b.emitted_return && !has_implicit_tail &&
      !ny_native_nir_stmt_has_return(fn->as.fn.body) &&
      !ny_native_nir_stmt_has_control_flow(fn->as.fn.body)) {
    const char *rt = fn->as.fn.return_type;
    const char *dot = rt ? strrchr(rt, '.') : NULL;
    const char *base = dot ? dot + 1 : rt;
    if (base && *base && strcmp(base, "void") != 0 &&
        strcmp(base, "any") != 0 && strcmp(base, "nil") != 0) {
      ok = ny_native_nir_fail(
          &b, "function '%s' falls off the end without returning a %s value",
          fn->as.fn.name ? fn->as.fn.name : "<fn>", base);
    }
  }
  if (ok && !b.emitted_return) {
    /*
     * Capture the implicit return value before defer bodies run: defer
     * statements lower as expression statements and would clobber
     * last_value.
     */
    int ret = b.last_value >= 0 ? b.last_value
                                : ny_native_nir_emit_const(&b, 0);
    /* Preserve the implicit expression's producer kind.  Passing NULL here
     * made captured dynamic lambdas conservatively tag an already-tagged
     * `any` arithmetic result a second time (closure 5 became 31/63). */
    const expr_t *implicit_expr = NULL;
    if (fn->as.fn.body && fn->as.fn.body->kind == NY_S_EXPR)
      implicit_expr = fn->as.fn.body->as.expr.expr;
    else if (fn->as.fn.body && fn->as.fn.body->kind == NY_S_BLOCK &&
             fn->as.fn.body->as.block.body.len > 0) {
      const stmt_t *last = fn->as.fn.body->as.block.body.data[
          fn->as.fn.body->as.block.body.len - 1];
      if (last && last->kind == NY_S_EXPR)
        implicit_expr = last->as.expr.expr;
    }
    ret = ny_native_nir_normalize_return(&b, implicit_expr, ret);
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
  case NY_E_CALL:
    if (e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name &&
        strcmp(e->as.call.callee->as.ident.name, "__main") == 0 &&
        e->as.call.args.len == 0) {
      *out = true;
      return true;
    }
    return false;
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

static char *ny_native_nir_read_c_include(const char *path, bool is_std,
                                          void *userdata) {
  (void)userdata;
  char *src = path ? ny_read_file(path) : NULL;
  if (src || !is_std || !path)
    return src;
  static const char *const roots[] = {
      "/usr/local/include", "/usr/include",
#if defined(__x86_64__)
      "/usr/include/x86_64-linux-gnu",
#elif defined(__aarch64__)
      "/usr/include/aarch64-linux-gnu",
#elif defined(__i386__)
      "/usr/include/i386-linux-gnu",
#endif
      NULL};
  char fullpath[4096];
  for (size_t i = 0; roots[i]; ++i) {
    int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", roots[i], path);
    if (n > 0 && (size_t)n < sizeof(fullpath)) {
      src = ny_read_file(fullpath);
      if (src)
        return src;
    }
  }
  return NULL;
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
    if (!ny_extern_table_add(t, ny_name, c_sym, pc, false, 0, NULL, NULL,
                             false, false, NULL, NULL)) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "NYIR extern: conflicting or duplicate extern '%s' "
                 "(C symbol '%s' conflicts with earlier declaration)",
                 ny_name, c_sym);
      ny_diag_error(s->tok,
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
    if (!src && s->as.inc.path) {
      static const char *const roots[] = {
          "/usr/local/include", "/usr/include",
#if defined(__x86_64__)
          "/usr/include/x86_64-linux-gnu",
#elif defined(__aarch64__)
          "/usr/include/aarch64-linux-gnu",
#elif defined(__i386__)
          "/usr/include/i386-linux-gnu",
#endif
          NULL};
      char fullpath[4096];
      for (size_t r = 0; !src && roots[r]; ++r) {
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", roots[r], s->as.inc.path);
        if (n > 0 && (size_t)n < sizeof(fullpath))
          src = ny_read_file(fullpath);
      }
    }
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
    parser->include_read = ny_native_nir_read_c_include;
    parser->source_file = s->as.inc.path;
    if (s->as.inc.path) {
      static char source_dir[4096];
      const char *slash = strrchr(s->as.inc.path, '/');
      if (slash) {
        size_t dlen = (size_t)(slash - s->as.inc.path);
        if (dlen < sizeof(source_dir)) {
          memcpy(source_dir, s->as.inc.path, dlen);
          source_dir[dlen] = '\0';
          parser->source_dir = source_dir;
        }
      }
    }
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
      bool param_f64[NY_C_MAX_PARAMS] = {0};
      bool param_f32[NY_C_MAX_PARAMS] = {0};
      for (unsigned pi = 0; pi < decl.param_count && pi < NY_C_MAX_PARAMS; pi++) {
        const ny_ctype_t *pt = &decl.params[pi];
        param_f64[pi] = ny_native_c_type_is_f64(pt);
        param_f32[pi] = ny_native_c_type_is_f32(pt);
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
                               ret_agg, ret_agg_classes, arg_agg,
                               ny_native_c_type_is_f64(&decl.type),
                               ny_native_c_type_is_f32(&decl.type),
                               param_f64, param_f32)) {
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
                                 ret_agg_classes, arg_agg,
                                 ny_native_c_type_is_f64(&decl.type),
                                 ny_native_c_type_is_f32(&decl.type),
                                 param_f64, param_f32)) {
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
    if (!ny_native_c_layout_collect_parser(&t->layouts, parser)) {
      ny_parse_cleanup(parser);
      free(parser);
      free(src);
      if (err && err_len > 0)
        snprintf(err, err_len, "NYIR extern: C layout table full or out of memory");
      return false;
    }
    for (unsigned i = 0; i < parser->define_count; ++i) {
      char *define_name = ny_native_c_token_dup(parser->define_names[i]);
      if (!define_name)
        continue;
      bool define_ok = ny_native_c_define_add(
          &t->defines, define_name, parser->define_values[i]);
      if (define_ok && prefix && prefix[0]) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "%s.%s", prefix,
                         define_name);
        define_ok = n > 0 && (size_t)n < sizeof(qualified) &&
                    ny_native_c_define_add(&t->defines, qualified,
                                           parser->define_values[i]);
      }
      if (define_ok && (!prefix || !prefix[0])) {
        char qualified[512];
        int n = snprintf(qualified, sizeof(qualified), "c.%s", define_name);
        define_ok = n > 0 && (size_t)n < sizeof(qualified) &&
                    ny_native_c_define_add(&t->defines, qualified,
                                           parser->define_values[i]);
      }
      free(define_name);
      if (!define_ok) {
        ny_parse_cleanup(parser);
        free(parser);
        free(src);
        if (err && err_len > 0)
          snprintf(err, err_len, "NYIR extern: C define table full or out of memory");
        return false;
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
    /* Imported stdlib modules are declarations, not source-level link
     * manifests for the current native unit.  Walking every nested #include
     * in std.os here repeatedly reparses the system headers and defeats the
     * reachable-function pruning below.  User/source-file includes remain
     * fully collected; runtime-backed stdlib calls are represented by their
     * rt_* lowering symbols instead. */
    if (ny_native_stmt_is_stdlib(s))
      continue;
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
  nyir_func_t *rt_main;
  nyir_func_t *funcs;
  const char **names;
  size_t count;
} ny_native_oracle_ctx_t;

static bool ny_native_per_pass_oracle_cb(const nyir_func_t *f,
                                         const char *pass_name,
                                         void *userdata) {
  ny_native_oracle_ctx_t *ctx = (ny_native_oracle_ctx_t *)userdata;
  /* The callback is globally installed while collected helper functions are
   * finalized too.  Only rt_main has the program-result contract; comparing
   * a helper's return value against the top-level expected result produces a
   * false failure at the first const-fold checkpoint. */
  if (!ctx || !f || f != ctx->rt_main)
    return true;
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
                               .source_file = options ? options->input_file : NULL,
                               .trace_instrumented = ny_native_nir_trace_requested(options),
                               .opt_level = opt_level};
  if (!ny_native_nir_emit_trace_enter(&b, "rt_main", NULL, 1)) {
    nyir_func_free(&b.nyir);
    ny_native_nir_builder_dispose(&b);
    return false;
  }
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (ny_trace_enabled("NY_TRACE_LOWER"))
      fprintf(stderr, "[lower rt_main stmt %zu] kind=%d\n", i,
              (int)prog->body.data[i]->kind);
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
        /* A module-only compilation unit has no executable expression.  It
         * is still a valid native artifact; make its entry a no-op so
         * incremental provider modules can be compiled independently. */
        b.last_value = ny_native_nir_emit_const(&b, 0);
        if (b.last_value < 0) {
          nyir_func_free(&b.nyir);
          ny_native_nir_builder_dispose(&b);
          return false;
        }
      } else {
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
  /* Function whose body is currently scanned; resolves module-local callbacks. */
  const stmt_t *scope_fn;
  /* Top-level statement currently being scanned; scopes dyn-list inference. */
  const stmt_t *query_scope;
} ny_native_fn_collector_t;

static void ny_native_collector_add_fn(ny_native_fn_collector_t *col,
                                       const stmt_t *fn) {
  if (!col || !col->funcs || col->max_funcs == 0 || !fn || fn->kind != NY_S_FUNC || !fn->as.fn.name)
    return;
  for (size_t i = 0; i < col->count; ++i)
    if (col->funcs[i] == fn)
      return;
  if (col->count < col->max_funcs)
    col->funcs[col->count++] = fn;
}
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
  ny_native_nir_builder_t probe = {
      .prog = col->prog,
      .options = col->opt,
      .module_name = col->scope_fn
                         ? ny_native_fn_module(col->prog, col->scope_fn)
                         : NULL,
      .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL};
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
static void ny_native_dynlist_note_call(const char *callee_name,
                                        size_t arg_index);


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
      strcmp(name, "std.os.ui.render.get_frame_time") == 0 ||
      strcmp(name, "std.os.ui.render.set_ortho_2d") == 0 ||
      strcmp(name, "std.os.ui.render.draw_rect") == 0 ||
      strcmp(name, "std.os.ui.render.draw_circle") == 0 ||
      strcmp(name, "std.os.ui.render.draw_text_centered") == 0 ||
      strcmp(name, "std.os.ui.render.end_frame") == 0 ||
      strcmp(name, "std.os.ui.window.set_should_close") == 0 ||
      strcmp(name, "std.os.ui.window.input.key_down") == 0 ||
      strcmp(leaf, "str_replace") == 0 ||
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
  if (!col || !col->funcs || col->max_funcs == 0 || !name || !*name)
    return;
  for (size_t i = 0; i < col->count; ++i) {
    if (col->funcs[i] && col->funcs[i]->as.fn.name &&
        strcmp(col->funcs[i]->as.fn.name, name) == 0)
      return;
  }
  ny_native_nir_builder_t probe = {
      .prog = col->prog,
      .options = col->opt,
      .module_name = col->scope_fn
                         ? ny_native_fn_module(col->prog, col->scope_fn)
                         : NULL,
      .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL};
  const stmt_t *fn = ny_native_nir_find_user_function(&probe, name);
  if (!fn || fn->as.fn.is_extern || fn->as.fn.link_name)
    return;
  /* Public names with a raw runtime bridge do not need their stdlib bodies.
   * A source function is different: it deliberately shadows that bridge and
   * must be collected under the normal native-function symbol. */
  if (ny_native_runtime_symbol(name) && ny_is_stdlib_tok(fn->tok))
    return;
  /*
   * Native leaf / builtin-alloc names (print, addr_of, borrow, float,
   * malloc, free, ...) are lowered directly to runtime calls in the
   * shared-NYIR path, so their stdlib bodies must not be force-built:
   * those bodies use NY_E_LIST / NY_E_MEMBER constructs the shared lowerer
   * does not support yet.  `zalloc` is retained because modules also pass it
   * as a function value.
   */
  const char *leaf = ny_native_leaf_name(name);
  if (ny_is_stdlib_tok(fn->tok) &&
      (ny_native_leaf_kind(leaf) != NY_NATIVE_LEAF_NONE ||
       (ny_builtin_alloc_kind(leaf) != NY_BUILTIN_ALLOC_NONE &&
        strcmp(leaf, "zalloc") != 0) ||
       ny_native_nir_stdlib_call_lowered_inline(name)))
    return;
  for (size_t i = 0; i < col->count; ++i) {
    if (col->funcs[i] == fn ||
        (col->funcs[i]->as.fn.name && fn->as.fn.name &&
         strcmp(col->funcs[i]->as.fn.name, fn->as.fn.name) == 0))
      return;
  }
  if (col->count < col->max_funcs) {
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE"))
      fprintf(stderr, "native reachable: %s\n", name);
    col->funcs[col->count++] = fn;
  }
  /* Semantic resolution can choose a module-specific overload for an
   * unqualified stdlib leaf while the syntax-only collector sees an earlier
   * wildcard import.  Only retain an exact-name duplicate here.  Leaf-name
   * fan-out is unsafe: unrelated methods such as io.set and ring.evaluate
   * can otherwise be pulled into every native unit merely because they share
   * a spelling with a call in the source. */
  if (col->scope_fn && !strchr(name, '.') && ny_native_fn_cache.prog == col->prog) {
    unsigned extra = 0;
    for (size_t i = 0; i < ny_native_fn_cache.count && extra < 16; ++i) {
      const stmt_t *candidate = ny_native_fn_cache.funcs[i];
      if (!candidate || candidate == fn || !candidate->as.fn.name ||
          !ny_is_stdlib_tok(candidate->tok) ||
          strcmp(candidate->as.fn.name, name) != 0)
        continue;
      if (ny_native_runtime_symbol(candidate->as.fn.name) ||
          ny_native_nir_stdlib_call_lowered_inline(candidate->as.fn.name) ||
          ny_native_leaf_kind(ny_native_leaf_name(candidate->as.fn.name)) !=
              NY_NATIVE_LEAF_NONE)
        continue;
      ny_native_add_reachable_fn(col, candidate->as.fn.name);
      ++extra;
    }
  }
}

static void ny_native_scan_expr_for_calls(const expr_t *e,
                                          ny_native_fn_collector_t *col) {
  if (!e || !col)
    return;
  switch (e->kind) {
  case NY_E_IDENT:
    /*
     * A function value referenced as data (type constructor, stored
     * closure, callback, resolved sysconst) is still reachable code:
     * emit it so the object encoder can resolve the ADDR_SYMBOL/CALL
     * reloc it produces.
     */
    if (e->as.ident.name && col->funcs && col->max_funcs > 0) {
      /* A callback may be stored in an `any` dictionary before it is called.
       * In that case semantic analysis does not always retain the closure
       * representation, but the identifier still names a real program
       * function and its ADDR_SYMBOL must keep the body reachable. */
      bool is_fn = false;
      if (e->semantic.resolved) {
        is_fn = (e->semantic.rep == NY_SEM_REP_CLOSURE);
      } else {
        bool known = false;
        const stmt_t *cached =
            ny_native_fn_resolve_cache_get(e->as.ident.name, NULL, &known);
        if (known) {
          is_fn = (cached != NULL);
        } else {
          const stmt_t *fn = ny_native_fn_cache_lookup_exact(e->as.ident.name);
          ny_native_fn_resolve_cache_put(e->as.ident.name, NULL, fn);
          is_fn = (fn != NULL);
        }
      }
      /* A named `use` import can be explicitly widened to `any` before it
       * is stored as a callback.  Its identifier then no longer advertises a
       * closure representation, but the import resolver still knows the
       * canonical source function whose address is being materialized. */
      ny_native_nir_builder_t probe = {
          .prog = col->prog,
          .options = col->opt,
          .module_name = col->scope_fn
                             ? ny_native_fn_module(col->prog, col->scope_fn)
                             : NULL,
          .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL};
      const char *imported =
          ny_native_nir_resolve_use_alias(&probe, e->as.ident.name);
      if (imported && strcmp(imported, e->as.ident.name) != 0)
        ny_native_add_reachable_fn(col, imported);
      const stmt_t *sibling =
          ny_native_nir_find_user_function(&probe, e->as.ident.name);
      if (sibling && sibling->kind == NY_S_FUNC && sibling->as.fn.name)
        ny_native_add_reachable_fn(col, sibling->as.fn.name);
      if (is_fn)
        ny_native_add_reachable_fn(col, e->as.ident.name);
    }
    break;
  case NY_E_CALL: {
    const char *semantic_callee =
        e->semantic.member_call_kind != NY_SEM_CALL_NONE
            ? e->semantic.canonical_callee
            : NULL;
    if (semantic_callee) {
      ny_native_add_reachable_fn(col, semantic_callee);
    }
    if (e->as.call.callee) {
      if (e->as.call.callee->kind == NY_E_IDENT) {
        const char *callee_name = e->as.call.callee->as.ident.name;
        if (callee_name) {
          /*
           * Pre-pass collection: record every (callee, arg-index) whose
           * argument has a dynamic-list shape so per-parameter queries
           * never need to rescan the program.
           */
          if (!col->funcs) {
            for (size_t ai = 0; ai < e->as.call.args.len; ++ai)
              if (ny_native_nir_expr_has_dyn_list_shape_in_scope(
                      col->prog, col->query_scope,
                      e->as.call.args.data[ai].val, 0))
                ny_native_dynlist_note_call(callee_name, ai);
          }
        }
        if (callee_name) {
          ny_native_add_reachable_fn(col, callee_name);
          const char *dot = strrchr(callee_name, '.');
          if (dot && dot > callee_name) {
            char candidate[256];
            snprintf(candidate, sizeof(candidate), "std.core.%s", dot + 1);
            ny_native_add_reachable_fn(col, candidate);
          }
        }
      } else if (e->as.call.callee->kind == NY_E_MEMBER) {
        if (!semantic_callee) {
          bool qualified = ny_native_scan_qualified_call(
              col, e->as.call.callee->as.member.target,
              e->as.call.callee->as.member.name);
          if (!qualified)
            ny_native_add_reachable_fn(col,
                                       e->as.call.callee->as.member.name);
        }
      } else if (e->as.call.callee->kind == NY_E_FN ||
                 e->as.call.callee->kind == NY_E_LAMBDA) {
        stmt_t *lambda = ny_native_lambda_create(e->as.call.callee);
        if (lambda)
          ny_native_collector_add_fn(col, lambda);
      }
      ny_native_scan_expr_for_calls(e->as.call.callee, col);
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      const expr_t *arg = e->as.call.args.data[i].val;
      if (arg && arg->semantic.resolved &&
          arg->semantic.rep == NY_SEM_REP_CLOSURE) {
        if (arg->kind == NY_E_IDENT && arg->as.ident.name) {
          ny_native_add_reachable_fn(col, arg->as.ident.name);
        } else if (arg->kind == NY_E_FN || arg->kind == NY_E_LAMBDA) {
          stmt_t *lambda = ny_native_lambda_create(arg);
          if (lambda)
            ny_native_collector_add_fn(col, lambda);
        }
      }
      ny_native_scan_expr_for_calls(arg, col);
    }
    break;
  }
  case NY_E_MEMCALL:
    if (e->semantic.canonical_callee)
      ny_native_add_reachable_fn(col, e->semantic.canonical_callee);
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
      if (!e->semantic.canonical_callee && !qualified && !runtime_method)
        ny_native_add_reachable_fn(col, e->as.memcall.name);
    }
    ny_native_scan_expr_for_calls(e->as.memcall.target, col);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_native_scan_expr_for_calls(e->as.memcall.args.data[i].val, col);
    break;
  case NY_E_MEMBER:
    if (e->semantic.canonical_callee)
      ny_native_add_reachable_fn(col, e->semantic.canonical_callee);
    if (e->as.member.name &&
        !ny_native_scan_qualified_call(col, e->as.member.target,
                                       e->as.member.name))
      /* Type/impl member dispatch often has a local receiver, so it cannot
       * be reconstructed as an alias-qualified expression by this syntax
       * pass.  Resolve the method leaf and let the bounded scoped fan-out in
       * ny_native_add_reachable_fn retain ambiguous impl bodies. */
      ny_native_add_reachable_fn(col, e->as.member.name);
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
  case NY_E_TERNARY: {
    const expr_t *branches[] = {e->as.ternary.true_expr,
                                e->as.ternary.false_expr};
    for (size_t i = 0; i < sizeof(branches) / sizeof(branches[0]); ++i)
      if (branches[i] && branches[i]->kind == NY_E_IDENT &&
          branches[i]->as.ident.name)
        ny_native_add_reachable_fn(col, branches[i]->as.ident.name);
    ny_native_scan_expr_for_calls(e->as.ternary.cond, col);
    ny_native_scan_expr_for_calls(e->as.ternary.true_expr, col);
    ny_native_scan_expr_for_calls(e->as.ternary.false_expr, col);
    break;
  }
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
  case NY_E_MATCH: {
    stmt_t match_stmt = {.kind = NY_S_MATCH, .tok = e->tok, .as = {.match = e->as.match}};
    size_t static_arm = 0;
    int static_selection = ny_native_nir_static_match_arm(&match_stmt, &static_arm);
    if (static_selection == 1) {
      ny_native_scan_stmt_for_calls(
          e->as.match.arms.data[static_arm].conseq, col);
      break;
    }
    if (static_selection == 2) {
      ny_native_scan_stmt_for_calls(e->as.match.default_conseq, col);
      break;
    }
    ny_native_scan_expr_for_calls(e->as.match.test, col);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      ny_native_scan_expr_for_calls(e->as.match.arms.data[i].guard, col);
      ny_native_scan_stmt_for_calls(e->as.match.arms.data[i].conseq, col);
    }
    ny_native_scan_stmt_for_calls(e->as.match.default_conseq, col);
    break;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    stmt_t *lambda = ny_native_lambda_create(e);
    if (lambda) {
      ny_native_lambda_entry_t *entry = ny_native_lambda_find(e);
      /* Function parameters are the common enclosing captures.  Record them
       * during reachability so the lambda body is built with its hidden env
       * parameter before function lowering starts. */
      if (entry && col->scope_fn) {
        for (size_t i = 0; i < col->scope_fn->as.fn.params.len; ++i) {
          const char *name = col->scope_fn->as.fn.params.data[i].name;
          if (name && !ny_native_lambda_param_named(e, name) &&
              ny_native_nir_stmt_uses_ident(e->as.lambda.body, name) &&
              !ny_native_lambda_capture_add(entry, name))
            break;
        }
      }
      ny_native_collector_add_fn(col, lambda);
    }
    ny_native_scan_stmt_for_calls(e->as.lambda.body, col);
    break;
  }
  default:
    break;
  }
}
/*
 * Memoization for dyn-list parameter queries. Each query rescans the
 * ENTIRE program AST, and the query fires once per list-typed parameter
 * of every lowered function: O(functions x params x program) node
 * visits. Real modules (std.os.ui.render alone is ~3k lines with
 * hundreds of functions) turned that into minutes of pure scanning.
 *
 * The cache is populated by ONE whole-program pre-pass
 * (ny_native_dynlist_prewarm) that records every callee/argument-index
 * pair whose call-site argument has a dynamic-list shape; lookups are
 * then O(1). Cleared with the other session-lifetime native pools.
 */
enum { NY_DYNLIST_CACHE_MAX = 8192, NY_DYNLIST_NAME = 512 };
typedef struct {
  char name[NY_DYNLIST_NAME];
  unsigned param;
  bool dyn_list;
} ny_dynlist_cache_ent_t;
static ny_dynlist_cache_ent_t ny_dynlist_cache[NY_DYNLIST_CACHE_MAX];
static size_t ny_dynlist_cache_len;

static void ny_native_dynlist_cache_clear(void) { ny_dynlist_cache_len = 0; }

static bool ny_native_nir_function_param_is_dyn_list(const program_t *prog,
                                                     const stmt_t *fn,
                                                     size_t param_index) {
  (void)prog;
  if (!fn || fn->kind != NY_S_FUNC || !fn->as.fn.name)
    return false;
  const char *qname = fn->as.fn.name;
  if (strlen(qname) >= NY_DYNLIST_NAME)
    return false;
  for (size_t i = 0; i < ny_dynlist_cache_len; ++i) {
    if (ny_dynlist_cache[i].param == param_index &&
        strcmp(ny_dynlist_cache[i].name, qname) == 0)
      return ny_dynlist_cache[i].dyn_list;
  }
  return false;
}

static void ny_native_dynlist_note_call(const char *callee_name,
                                        size_t arg_index) {
  if (!callee_name || strlen(callee_name) >= NY_DYNLIST_NAME)
    return;
  for (size_t i = 0; i < ny_dynlist_cache_len; ++i)
    if (ny_dynlist_cache[i].param == arg_index &&
        strcmp(ny_dynlist_cache[i].name, callee_name) == 0)
      return;
  if (ny_dynlist_cache_len >= NY_DYNLIST_CACHE_MAX)
    return;
  ny_dynlist_cache_ent_t *e = &ny_dynlist_cache[ny_dynlist_cache_len++];
  snprintf(e->name, sizeof(e->name), "%s", callee_name);
  e->param = arg_index;
  e->dyn_list = true;
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
    ny_native_scan_stmt_for_calls(s->as.iff.init, col);
    ny_native_scan_expr_for_calls(s->as.iff.test, col);
    ny_native_scan_stmt_for_calls(s->as.iff.conseq, col);
    ny_native_scan_stmt_for_calls(s->as.iff.alt, col);
    break;
  }
  case NY_S_WHILE:
    ny_native_scan_stmt_for_calls(s->as.whl.init, col);
    ny_native_scan_expr_for_calls(s->as.whl.test, col);
    ny_native_scan_expr_for_calls(s->as.whl.invariant, col);
    ny_native_scan_stmt_for_calls(s->as.whl.body, col);
    ny_native_scan_stmt_for_calls(s->as.whl.update, col);
    break;
  case NY_S_FOR:
    ny_native_scan_expr_for_calls(s->as.fr.iterable, col);
    ny_native_scan_expr_for_calls(s->as.fr.cond, col);
    ny_native_scan_stmt_for_calls(s->as.fr.init, col);
    ny_native_scan_stmt_for_calls(s->as.fr.update, col);
    ny_native_scan_stmt_for_calls(s->as.fr.body, col);
    break;
  case NY_S_TRY:
    ny_native_scan_stmt_for_calls(s->as.tr.body, col);
    ny_native_scan_stmt_for_calls(s->as.tr.handler, col);
    break;
  case NY_S_GUARD:
    if (s->as.guard.type_name) {
      const char *type = s->as.guard.type_name;
      while (*type == '?' || *type == '*')
        ++type;
      char converter[256];
      int n = snprintf(converter, sizeof(converter), "%s_from", type);
      if (n > 0 && (size_t)n < sizeof(converter))
        ny_native_add_reachable_fn(col, converter);
    }
    ny_native_scan_expr_for_calls(s->as.guard.value, col);
    ny_native_scan_stmt_for_calls(s->as.guard.fallback, col);
    break;
  case NY_S_DEFER:
    ny_native_scan_stmt_for_calls(s->as.de.body, col);
    break;
  case NY_S_MATCH:
    {
      size_t static_arm = 0;
      int static_selection = ny_native_nir_static_match_arm(s, &static_arm);
      if (static_selection == 1) {
        ny_native_scan_stmt_for_calls(
            s->as.match.arms.data[static_arm].conseq, col);
        break;
      }
      if (static_selection == 2) {
        ny_native_scan_stmt_for_calls(s->as.match.default_conseq, col);
        break;
      }
    }
    ny_native_scan_expr_for_calls(s->as.match.test, col);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
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
    /* Nested declarations are executable code when their enclosing
     * function calls them.  The body scanner reaches this node while walking
     * the parent, so register it explicitly instead of only scanning its
     * defaults/body. */
    if (s->as.fn.name && !ny_is_stdlib_tok(s->tok))
      ny_native_add_reachable_fn(col, s->as.fn.name);
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      ny_native_scan_expr_for_calls(s->as.fn.params.data[i].def, col);
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
  /* The expanded program contains test/main blocks from imported packages.
   * Seed the call graph only from the root source block; imported-module
   * bodies become reachable when an actually called function is scanned. */
  const char *root_source = NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
        strcmp(s->as.fn.name, "main") == 0 && !ny_native_stmt_is_stdlib(s)) {
      root_source = s->tok.filename;
      break;
    }
  }
  for (size_t i = 0; i < prog->body.len && !root_source; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->tok.filename && !ny_native_stmt_is_stdlib(s))
      root_source = s->tok.filename;
  }

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
    if (!s)
      continue;
    if (s->kind == NY_S_BLOCK && root_source && s->tok.filename &&
        !ny_native_nir_same_source_file(s->tok.filename, root_source))
      continue;
    /* Impl methods are executable functions too, but their declarations are
     * nested under NY_S_IMPL rather than appearing as top-level NY_S_FUNC
     * nodes.  Seed source impl methods so operator/attached dispatch cannot
     * leave a referenced method as an unresolved JIT declaration. */
    if ((s->kind == NY_S_IMPL || s->kind == NY_S_LAYOUT ||
         s->kind == NY_S_STRUCT) && !ny_native_stmt_is_stdlib(s)) {
      const ny_stmt_list *methods =
          s->kind == NY_S_IMPL   ? &s->as.impl.methods
          : s->kind == NY_S_LAYOUT ? &s->as.layout.methods
                                   : &s->as.struc.methods;
      for (size_t j = 0; j < methods->len; ++j) {
        const stmt_t *m = methods->data[j];
        if (m && m->kind == NY_S_FUNC && m->as.fn.name)
          ny_native_add_reachable_fn(&col, m->as.fn.name);
      }
    }
    /* Imported user modules contribute declarations, not executable top-level
     * statements to the root.  Scanning every function body in each module
     * here over-approximates the call graph (and can pull hundreds of dead
     * helpers into native lowering); reachable function bodies are scanned by
     * the worklist below instead. */
    if (s->kind != NY_S_FUNC && s->kind != NY_S_MODULE &&
        !ny_native_stmt_is_stdlib(s)) {
      ny_native_scan_stmt_for_calls(s, &col);
    }
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
    if (s && s->kind == NY_S_FUNC && s->as.fn.name && !ny_is_stdlib_tok(s->tok)) {
      ny_native_add_reachable_fn(&col, s->as.fn.name);
    }
  }

  /*
   * 3. Transitive worklist loop over collected functions
   */
  size_t idx = 0;
  while (idx < col.count) {
    const stmt_t *fn = col.funcs[idx++];
    col.scope_fn = fn;
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
        if (ny_trace_enabled("NY_TRACE_PURE_CALLS"))
          fprintf(stderr, "pure-call: %s tagged effects=0x%x\n",
                  in->symbol, (unsigned)in->effects);
      } else if (ny_trace_enabled("NY_TRACE_PURE_CALLS")) {
        fprintf(stderr, "pure-call: %s NOT tagged effects=0x%x\n",
                in->symbol, (unsigned)s);
      }
      break;
    }
  }
}
static bool ny_native_nir_has_phi(const nyir_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI)
      return true;
  return false;
}

bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         nyir_func_t *rt_main_out,
                         nyir_func_t *funcs_out, size_t *func_count,
                         const char **func_names_out, size_t max_funcs,
                         char *err, size_t err_len) {
  if (!prog || !rt_main_out)
    return false;
#define NY_NATIVE_STAGE(label)                                                  \
  do {                                                                          \
    if (ny_trace_enabled("NY_NATIVE_TRACE"))                                    \
      fprintf(stderr, "[native-stage] %s\n", label);                            \
  } while (0)
  NY_NATIVE_STAGE("begin");
  ny_native_fn_cache_build(prog);
  NY_NATIVE_STAGE("function-cache");
  ny_native_strtab_clear();
  ny_native_lambda_clear();
  ny_native_consttab_clear();
  ny_native_globaltab_clear();
  ny_native_dynlist_cache_clear();
  {
    /*
     * One whole-program pass records every (callee, arg-index) with a
     * dynamic-list argument; function_param_is_dyn_list then answers
     * from this table in O(1) instead of rescanning the AST per
     * list-typed parameter.
     */
    ny_native_fn_collector_t warm = {.prog = prog};
    for (size_t i = 0; i < prog->body.len; ++i) {
    if (ny_native_stmt_is_stdlib(prog->body.data[i]))
      continue;
    warm.query_scope = prog->body.data[i];
    ny_native_scan_stmt_for_calls(prog->body.data[i], &warm);
    }
  }
  NY_NATIVE_STAGE("dynamic-list-prewarm");
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_register_const_defs(prog, prog->body.data[i], NULL, opt);
  NY_NATIVE_STAGE("const-table");
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
  NY_NATIVE_STAGE("extern-table");

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
    NY_NATIVE_STAGE("reachable-functions");
    ny_type_pipeline_persist_native_reachable_facts(
        (program_t *)prog, all_fns, all_fn_count,
        all_fn_count && all_fns[0] ? all_fns[0]->tok.filename : NULL);
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE"))
      fprintf(stderr, "native reachable functions: %zu (capacity %zu)\n", all_fn_count,
              max_funcs);
    size_t count = 0;
    for (size_t i = 0; i < all_fn_count; ++i) {
      const stmt_t *s = all_fns[i];
      if (!s)
        continue;
      char local_err[256] = {0};
      if (!ny_native_nir_build_function(prog, s, &externs, &funcs_out[count], local_err,
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
        const char *ny_dump_fn = getenv("NY_DUMP_FN");
        if (ny_dump_fn && func_names[count] &&
            strstr(func_names[count], ny_dump_fn))
          nyir_dump(stderr, &funcs_out[count], func_names[count]);
        if (getenv("NY_LIST_FNS"))
          fprintf(stderr, "[built] %s\n", func_names[count]);
        count++;
      }
    }
    NY_NATIVE_STAGE("function-lowering");
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
    if (ny_trace_enabled("NY_TRACE_LOWER"))
      fprintf(stderr, "[build_nir] mem2reg/cleanup sweep\n");
    /*
     * Clean and promote all raw function bodies to SSA before offering them
     * to the inliner. This removes raw parameter stores/loads so small leaf
     * functions like add(a, b) and mul(a, b) become immediate inlining candidates.
     */
    for (size_t i = 0; i < *func_count; ++i) {
      (void)nyir_mem2reg(&funcs_out[i]);
      (void)nyir_copy_prop(&funcs_out[i]);
      (void)nyir_dce(&funcs_out[i]);
      (void)nyir_compact(&funcs_out[i]);
      nyir_refresh_metadata(&funcs_out[i]);
    }
    for (int pass = 0; pass < 3; ++pass) {
      if (ny_trace_enabled("NY_TRACE_LOWER"))
        fprintf(stderr, "[build_nir] inline_small pass %d\n", pass);
      for (size_t i = 0; i < *func_count && *func_count <= 256; ++i) {
        if (nyir_inline_small(&funcs_out[i])) {
          (void)nyir_mem2reg(&funcs_out[i]);
          (void)nyir_copy_prop(&funcs_out[i]);
          (void)nyir_dce(&funcs_out[i]);
          (void)nyir_compact(&funcs_out[i]);
      nyir_refresh_metadata(&funcs_out[i]);
    }
  }
    }
    /*
     * Inline larger pure wrappers after leaf calls are folded.  This second
     * sweep is what turns a call chain into one direct arithmetic body; the
     * general inliner still enforces its existing size, ABI, and CFG guards.
     */
      for (size_t i = 0; i < *func_count && *func_count <= 256; ++i) {
      if (ny_trace_enabled("NY_TRACE_LOWER"))
        fprintf(stderr, "[build_nir] inline_general fn %zu\n", i);
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
    for (size_t i = 0; i < *func_count && *func_count <= 256; ++i) {
      if (ny_trace_enabled("NY_TRACE_LOWER"))
        fprintf(stderr, "[build_nir] post-inline optimize fn %zu\n", i);
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
  ny_native_oracle_ctx_t oracle_ctx = {opt, rt_main_out, funcs_out,
                                       (const char **)func_names,
                                       func_count ? *func_count : 0};
  if (ny_trace_enabled("NY_TRACE_LOWER"))
    fprintf(stderr, "[build_nir] calling build_rt_main\n");
  if (opt && opt->native_oracle_per_pass && funcs_out && func_count)
    nyir_set_per_pass_oracle(true, ny_native_per_pass_oracle_cb, &oracle_ctx);
  bool ok = ny_native_nir_build_rt_main(prog, rt_main_out, &externs, err, err_len,
                                        opt_level, opt);
  NY_NATIVE_STAGE("rt-main-lowering");
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
    for (int pass = 0; pass < 3; ++pass) {
      if (!nyir_inline_small(rt_main_out))
        break;
      (void)nyir_copy_prop(rt_main_out);
      (void)nyir_dce(rt_main_out);
      nyir_refresh_metadata(rt_main_out);
    }
    (void)nyir_inline_general(rt_main_out);
    for (int pass = 0; pass < 3; ++pass) {
      if (!nyir_inline_small(rt_main_out))
        break;
      (void)nyir_copy_prop(rt_main_out);
      (void)nyir_dce(rt_main_out);
      nyir_refresh_metadata(rt_main_out);
    }
    if (!nyir_optimize(rt_main_out, opt_level)) {
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
      if (!nyir_phi_elim(&funcs_out[i]) ||
          ny_native_nir_has_phi(&funcs_out[i])) {
        if (err && err_len > 0 && err[0] == '\0')
          snprintf(err, err_len,
                   "native NYIR: phi elimination could not lower PHI in %s",
                   func_names && func_names[i] ? func_names[i] : "unknown_fn");
        ok = false;
        break;
      }
      nyir_refresh_metadata(&funcs_out[i]);
    }
    if (ok && rt_main_out) {
      if (!nyir_phi_elim(rt_main_out) || ny_native_nir_has_phi(rt_main_out)) {
        if (err && err_len > 0 && err[0] == '\0')
          snprintf(err, err_len,
                   "native NYIR: phi elimination could not lower PHI in rt_main");
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
  ny_native_nir_builder_t b = {
      .last_value = -1,
      .err = err,
      .err_len = err_len,
      .prog = prog,
      .options = opt,
      .current_fn_name = fn->as.fn.name,
      .source_file = fn->tok.filename,
      .module_name = ny_native_fn_module(prog, fn),
      .opt_level = opt ? opt->opt_level : 1};
  b.return_type = fn ? fn->as.fn.return_type : NULL;
  b.return_any =
      (fn->as.fn.return_type && ny_native_type_name_is_any(fn->as.fn.return_type)) ||
      (!fn->as.fn.return_type && fn->as.fn.return_semantic.resolved &&
       fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC);
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
    const ny_expr_semantic_t *sem = &fn->as.fn.params.data[i].semantic;
    param->semantic_rep = sem->resolved ? sem->rep : param->semantic_rep;
    param->semantic_ownership = sem->ownership;
    param->alias_class = sem->alias_class;
    param->semantic_mutable = sem->mutable_value;
    param->is_any = (sem->resolved && sem->rep == NY_SEM_REP_TAGGED_DYNAMIC) ||
                    ny_native_type_name_is_any(fn->as.fn.params.data[i].type);
    param->type_name = fn->as.fn.params.data[i].type;
    param->is_bigint = fn->as.fn.params.data[i].type &&
                       strcmp(fn->as.fn.params.data[i].type, "bigint") == 0;
    param->is_dict = (sem->resolved && sem->rep == NY_SEM_REP_OBJECT) ||
                     (fn->as.fn.params.data[i].type &&
                      (strcmp(fn->as.fn.params.data[i].type, "dict") == 0 ||
                       strcmp(fn->as.fn.params.data[i].type, "vec2") == 0 ||
                       strcmp(fn->as.fn.params.data[i].type, "vec3") == 0 ||
                       strcmp(fn->as.fn.params.data[i].type, "vec4") == 0));
    param->is_list = (sem->resolved && sem->rep == NY_SEM_REP_TYPED_BUFFER) ||
        ny_native_type_name_is_list(fn->as.fn.params.data[i].type) ||
        ny_native_nir_param_is_inferred_list(fn, i) ||
        (fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1);
    param->is_dyn_list =
        param->is_list &&
        (ny_native_type_name_is_dyn_list(fn->as.fn.params.data[i].type) ||
         ny_native_nir_function_param_is_dyn_list(b.prog, fn, i));
    if (param->is_list)
      param->list_len_slot = b.next_local_slot++;
    else if (param->is_cstr || param->is_any) {
      param->dyn_str_len_slot = b.next_local_slot++;
      param->dyn_tag_slot = b.next_local_slot++;
    }
    param->fin_bound =
        ny_native_resolve_fin_bound(&b, fn->as.fn.params.data[i].type);
  }
  b.current_fn_name = fn->as.fn.name;
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  bool has_implicit_tail =
      fn->as.fn.body &&
      (fn->as.fn.body->kind == NY_S_EXPR ||
       (fn->as.fn.body->kind == NY_S_BLOCK &&
        fn->as.fn.body->as.block.body.len > 0 &&
        fn->as.fn.body->as.block.body.data[
            fn->as.fn.body->as.block.body.len - 1]->kind == NY_S_EXPR));
  if (ok && !b.emitted_return && !has_implicit_tail &&
      !ny_native_nir_stmt_has_return(fn->as.fn.body) &&
      !ny_native_nir_stmt_has_control_flow(fn->as.fn.body)) {
    const char *rt = fn->as.fn.return_type;
    const char *dot = rt ? strrchr(rt, '.') : NULL;
    const char *base = dot ? dot + 1 : rt;
    if (base && *base && strcmp(base, "void") != 0 &&
        strcmp(base, "any") != 0 && strcmp(base, "nil") != 0) {
      ok = ny_native_nir_fail(
          &b, "function '%s' falls off the end without returning a %s value",
          fn->as.fn.name ? fn->as.fn.name : "<fn>", base);
    }
  }
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
      b.last_value = ny_native_nir_emit_const(&b, 0);
      if (b.last_value < 0) {
        nyir_func_free(&b.nyir);
        ny_native_nir_builder_dispose(&b);
        ny_extern_table_free(&externs);
        return false;
      }
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
  NY_NATIVE_STAGE("done");
#undef NY_NATIVE_STAGE
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
  size_t func_cap = NY_NATIVE_NIR_BUNDLE_MAX_FUNCS;
  nyir_func_t *funcs = calloc(func_cap, sizeof(*funcs));
  const char **names = calloc(func_cap, sizeof(*names));
  size_t func_count = 0;
  char local_err[512] = {0};
  if (!funcs || !names) {
    free(funcs);
    free(names);
    if (out != stderr)
      fclose(out);
    ny_native_set_err(err, err_len,
                      "native metadata function pool allocation failed");
    return false;
  }
  bool ok = ny_native_build_nir(
      prog, opt, &rt_main, funcs, &func_count, names,
      func_cap, local_err, sizeof(local_err));

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
  free(funcs);
  free(names);
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
