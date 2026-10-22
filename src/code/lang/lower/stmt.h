/*
 * Statement lowering: AST → NYIR. Translates let/mut bindings, blocks,
 * control flow, and expressions into SSA form.
 * References: Cooper & Torczon, Ch.7; Cytron et al. (TOPLAS 1991) §4–5.
 */
static bool ny_native_stmt_is_stdlib(const stmt_t *s) {
  return s && ny_is_stdlib_tok(s->tok);
}

static int nyr_normalize_return(nyr_builder_t *b, const expr_t *expr,
                                int value);
static bool numeric_init_call(const expr_t *e);
static bool nyr_register_defer(nyr_builder_t *b, const stmt_t *body);

static bool nyr_value_is_tagged_dynamic_call(const nyr_builder_t *b,
                                             int value) {
  if (!b || value < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->dst != value)
      continue;
    if (in->op != NYIR_CALL || !in->symbol)
      return false;
    if (in->scalar_class == NYIR_SCALAR_I64_TAGGED)
      return true;
    /* The ABI record decides first for recorded producers: a tagged-int,
     * tagged-value, or bigint-handle result is already in the dynamic-value
     * domain, while a raw/cstr/pointer/f64 result is a scalar.  Only an
     * unrecorded symbol (or a row without a result class) falls back to the
     * legacy rt_any_* / rt_tag name tests. */
    const ny_native_runtime_abi_decl_t *decl =
        ny_native_runtime_abi_decl(in->symbol);
    if (decl) {
      if (ny_native_runtime_abi_result_is_tagged_int(decl) ||
          ny_native_runtime_abi_result_is_tagged_value(decl) ||
          ny_native_runtime_abi_result_is_bigint(decl))
        return true;
      if (ny_native_runtime_abi_result_is_raw_int(decl) ||
          ny_native_runtime_abi_result_is_raw_word(decl) ||
          ny_native_runtime_abi_result_is_cstr(decl) ||
          ny_native_runtime_abi_result_is_pointer(decl) ||
          (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_F64) != 0)
        return false;
    }
    const stmt_t *callee = nyr_find_user_function(b, in->symbol);
    if (callee &&
        ((callee->as.fn.return_type &&
          ny_native_type_name_is_any(callee->as.fn.return_type)) ||
         (callee->as.fn.return_semantic.resolved &&
          callee->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)))
      return true;
    return strncmp(in->symbol, "rt_any_", 7) == 0 ||
           strcmp(in->symbol, "rt_tag") == 0;
  }
  return false;
}

/* Return-position scalar expressions are already in the native raw-i64 ABI.
 * This is deliberately structural rather than based on `expr_is_any`: a few
 * compatibility intrinsics (notably bshr/bshl) are declared as `any` for the
 * interpreter, while their native lowering returns a raw machine word.  If a
 * raw bitwise tree is classified through that broad declaration, the integer
 * return boundary calls rt_any_to_i64 on an already-raw result and halves odd
 * values. */
static bool numeric_init_call(const expr_t *e);

static bool nyr_expr_is_raw_scalar(const nyr_builder_t *b, const expr_t *expr,
                                   unsigned depth) {
  if (!b || !expr || depth > 24)
    return false;
  /* An unparameterized sequence index already produces a canonical tagged
   * value.  Its stale RAW_INT use-site fact must not make an implicit `any`
   * return tag it a second time after the lexical parameter leaves scope. */
  if (expr->kind == NY_E_INDEX &&
      nyr_expr_is_canonical_dynamic_value((nyr_builder_t *)b, expr))
    return false;
  /* These compatibility intrinsics are declared `any` for the interpreter,
   * but their native lowering uses the raw scalar ABI.  Resolve them before
   * the generic named-call path, which can find their broad stdlib wrapper
   * declaration and classify its return as tagged dynamic. */
  if (expr->kind == NY_E_CALL) {
    const char *leaf = nyr_early_leaf(expr);
    if (leaf && (strcmp(leaf, "bshr") == 0 || strcmp(leaf, "__shr") == 0))
      return true;
  }
  /*
   * A call through a lambda-valued binding follows the ABI of the synthetic
   * function native lowering actually emits, and unannotated lambdas are
   * marked TAGGED_DYNAMIC at collection.  Source-level inference can still
   * stamp the call site itself as RAW_INT from the specialized lambda type;
   * that stale site fact must not classify the tagged body result as a raw
   * scalar, or `def add = fn(x) { x + 1 }` compares its boxed result (85)
   * against 42 and every raw consumer misreads the value.  Compat intrinsics
   * below keep their raw classification because they have no synthetic
   * lambda fact to consult.
   */
  if (expr->kind == NY_E_CALL && expr->as.call.callee &&
      expr->as.call.callee->kind == NY_E_IDENT &&
      expr->as.call.callee->as.ident.name) {
    const nyr_local_t *callable_local =
        nyr_find_expr_local((nyr_builder_t *)b, expr->as.call.callee);
    const expr_t *callable =
        callable_local ? callable_local->callable_expr : NULL;
    if (!callable && !callable_local) {
      const expr_t *global =
          nyr_find_top_level_value(b, expr->as.call.callee->as.ident.name);
      if (global && (global->kind == NY_E_LAMBDA || global->kind == NY_E_FN))
        callable = global;
    }
    ny_native_lambda_entry_t *lambda =
        callable
            ? ny_native_lambda_find(ny_native_lambda_cache_for(b), callable)
            : NULL;
    if (lambda && lambda->fn && !lambda->fn->as.fn.return_type &&
        lambda->fn->as.fn.return_semantic.resolved &&
        lambda->fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)
      return false;
    /* The same mismatch exists for an untyped user function whose inferred
     * return ABI is the canonical tagged value (for example
     * `fn apply(f, v) { f(v) }`): HM stamps the specialized call site as
     * RAW_INT while the body returns a tagged dynamic.  A local ALIAS to a
     * named function (`def fast_ref = fast_add`) resolves through the
     * alias's callable_name because the alias itself is not a function
     * declaration.  Only an explicitly declared return type may keep the
     * raw classification here, because compat intrinsics (`bshr`/`bshl`)
     * declare `any` yet return raw words. */
    const stmt_t *tagged_target = NULL;
    if (!lambda && callable_local && callable_local->callable_name)
      tagged_target = nyr_find_user_function(b, callable_local->callable_name);
    if (!tagged_target && !lambda && !callable_local) {
      const stmt_t *user_fn =
          nyr_find_user_function(b, expr->as.call.callee->as.ident.name);
      if (user_fn && user_fn->kind == NY_S_FUNC &&
          !user_fn->as.fn.return_type &&
          user_fn->as.fn.return_semantic.resolved &&
          user_fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)
        return false;
    }
    if (tagged_target && tagged_target->kind == NY_S_FUNC &&
        !tagged_target->as.fn.return_type &&
        tagged_target->as.fn.return_semantic.resolved &&
        tagged_target->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC)
      return false;
  }
  /*
   * A dynamic lexical/local binding is the representation actually in use
   * inside the current function, so it outranks a resolved expression
   * annotation.  When an untyped parameter shares a name with a top-level
   * binding (`mut x = 5` before `fn sq(x) { x * x }`), inference can leave the
   * parameter's own identifier with the global's RAW_INT fact.  Trusting that
   * stale fact here drops the dynamic parameter to raw arithmetic and
   * multiplies the boxed payload word, so a dynamic binding wins first and
   * only other shapes fall back to the expression annotation below.
   */
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, expr->as.ident.name);
    if (local && (nyr_expr_is_any((nyr_builder_t *)b, expr) ||
                  local->dyn_tag_slot >= 0 ||
                  local->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC))
      return false;
    /* Root bindings have physical module storage rather than a lexical slot.
     * Their identifier can retain a stale RAW_INT semantic fact even when
     * its initializer calls an untyped function whose body returns a tagged
     * dynamic value.  Use the initializer's call ABI before trusting that
     * refinement; otherwise comparisons load the boxed word and compare it
     * with an unboxed literal (`mut result = add(1, 2); result == 3`). */
    if (!local) {
      const stmt_t *global_decl =
          expr->tok.filename
              ? nyr_find_top_level_var_in_source_in_prog(
                    b, b->prog, expr->as.ident.name, expr->tok.filename)
              : NULL;
      if (!global_decl)
        global_decl =
            nyr_find_top_level_var_in_prog(b, b->prog, expr->as.ident.name);
      const expr_t *global =
          expr->tok.filename ? nyr_find_top_level_value_in_source(
                                   b, expr->as.ident.name, expr->tok.filename)
                             : nyr_find_top_level_value(b, expr->as.ident.name);
      if (!global)
        global = nyr_find_top_level_value(b, expr->as.ident.name);
      if (global && global != expr && global->kind == NY_E_CALL &&
          nyr_expr_is_any((nyr_builder_t *)b, global))
        return false;
    }
  }
  /* HM can refine a dynamic operation from its result type (for example
   * `x * 2` inside an untyped closure) even though native lowering still
   * dispatches through rt_any_* and therefore produces a tagged word.  The
   * physical producer wins over that stale scalar refinement at the return
   * boundary. */
  if (expr->kind == NY_E_BINARY && nyr_expr_is_any((nyr_builder_t *)b, expr)) {
    /* Dynamic modulo is evaluated by rt_any_mod, then explicitly unboxed
     * by arithmetic lowering because modulo feeds raw integer comparisons
     * and branches.  Preserve that physical result representation here so
     * `v % 2 == 0` compares raw scalars instead of sending the untagged
     * remainder through rt_any_eq. */
    if (expr->as.binary.op && strcmp(expr->as.binary.op, "%") == 0 &&
        !nyr_expr_is_f64((nyr_builder_t *)b, expr) &&
        !nyr_expr_is_f32((nyr_builder_t *)b, expr))
      return true;
    return false;
  }
  if (expr->kind == NY_E_TERNARY &&
      (nyr_expr_is_any((nyr_builder_t *)b, expr->as.ternary.true_expr) ||
       nyr_expr_is_any((nyr_builder_t *)b, expr->as.ternary.false_expr)))
    return false;
  /* Consult the call ABI before trusting HM's call-site value category.
   * Explicit integer returns are raw at the source function boundary, and
   * raw runtime bridges (including bshr) keep that representation through
   * native lowering even when their compatibility declarations say `any`. */
  if (expr->kind == NY_E_CALL) {
    const char *leaf = nyr_early_leaf(expr);
    if ((leaf && (strcmp(leaf, "bshr") == 0 || strcmp(leaf, "__shr") == 0)) ||
        nyr_expr_has_raw_int_abi(b, expr, 0))
      return true;
  }
  if (expr->semantic.resolved && expr->semantic.rep == NY_SEM_REP_RAW_INT)
    return true;
  if (expr->kind == NY_E_LITERAL)
    return expr->as.literal.kind == NY_LIT_INT && expr->tok.kind != NY_T_NIL;
  if (nyr_expr_is_raw_int(expr))
    return true;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const nyr_local_t *local =
        nyr_find_local((nyr_builder_t *)b, expr->as.ident.name);
    return local && !local->is_any && !local->is_list && !local->is_dict &&
           !local->is_cstr && !local->is_bytes && !local->is_bigint &&
           !local->is_f64 && !local->is_f32 && !local->is_bool &&
           (local->semantic_rep == NY_SEM_REP_RAW_INT ||
            ny_native_type_name_is_int(local->type_name));
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      (strcmp(expr->as.unary.op, "+") == 0 ||
       strcmp(expr->as.unary.op, "-") == 0 ||
       strcmp(expr->as.unary.op, "~") == 0))
    return nyr_expr_is_raw_scalar(b, expr->as.unary.right, depth + 1);
  if (expr->kind != NY_E_BINARY || !expr->as.binary.op)
    return false;
  const char *op = expr->as.binary.op;
  if (strcmp(op, "+") != 0 && strcmp(op, "-") != 0 && strcmp(op, "*") != 0 &&
      strcmp(op, "/") != 0 && strcmp(op, "%") != 0 && strcmp(op, "&") != 0 &&
      strcmp(op, "|") != 0 && strcmp(op, "^") != 0 && strcmp(op, "<<") != 0 &&
      strcmp(op, ">>") != 0)
    return false;
  return nyr_expr_is_raw_scalar(b, expr->as.binary.left, depth + 1) &&
         nyr_expr_is_raw_scalar(b, expr->as.binary.right, depth + 1);
}

/* Return the expression a block contributes as its implicit value. Try/catch
 * joins are lowered as statements, so they do not pass through the ordinary
 * function-tail return normalizer. */
static const expr_t *nyr_stmt_tail_expr(const stmt_t *s) {
  if (!s)
    return NULL;
  if (s->kind == NY_S_EXPR)
    return s->as.expr.expr;
  if (s->kind != NY_S_BLOCK || s->as.block.body.len == 0)
    return NULL;
  const stmt_t *last = s->as.block.body.data[s->as.block.body.len - 1];
  return last && last->kind == NY_S_EXPR ? last->as.expr.expr : NULL;
}

static int nyr_try_normalize_value(nyr_builder_t *b, const expr_t *expr,
                                   int value) {
  if (!b || value < 0 || !expr || !b->return_any)
    return value;
  int out = nyr_normalize_return(b, expr, value);
  if (out < 0)
    return out;
  /* Raw native accessors such as `.len` are memcalls and fall outside the
   * normal literal/binary/identifier return cases. Box scalar results once
   * before they enter an any-valued try/catch join. */
  if ((expr->kind == NY_E_MEMCALL || expr->kind == NY_E_MEMBER) &&
      !nyr_expr_is_any(b, expr) && !nyr_expr_is_cstr(b, expr) &&
      !nyr_expr_is_f64(b, expr) && !nyr_expr_is_f32(b, expr) &&
      !nyr_expr_is_list(b, expr) && !nyr_expr_is_dict(b, expr) &&
      !nyr_expr_is_bigint(b, expr) && !nyr_expr_is_bigfloat(b, expr))
    return nyr_emit_runtime_call(b, "rt_tag", out, -1, -1, 1, 0);
  return out;
}

typedef struct {
  const nyr_builder_t *builder;
  const char *name;
  bool escapes;
  bool counter;
  unsigned declarations;
} nyr_entry_binding_scan_t;

static bool nyr_entry_binding_expr(ny_visitor_t *visitor, expr_t *expr) {
  nyr_entry_binding_scan_t *scan = visitor->ctx;
  if ((expr->kind == NY_E_UNARY && expr->as.unary.op &&
       strcmp(expr->as.unary.op, "&") == 0 &&
       nyr_expr_uses_ident(scan->builder, expr->as.unary.right, scan->name)) ||
      ((expr->kind == NY_E_FN || expr->kind == NY_E_LAMBDA) &&
       nyr_expr_uses_ident(scan->builder, expr, scan->name)))
    scan->escapes = true;
  return !scan->escapes;
}

static bool nyr_entry_binding_stmt(ny_visitor_t *visitor, stmt_t *stmt) {
  nyr_entry_binding_scan_t *scan = visitor->ctx;
  if (scan->counter && stmt->kind == NY_S_VAR) {
    for (size_t i = 0; i < stmt->as.var.names.len; ++i) {
      const char *name = stmt->as.var.names.data[i];
      if (!name || strcmp(name, scan->name) != 0)
        continue;
      if (stmt->as.var.is_decl) {
        if (++scan->declarations != 1)
          scan->escapes = true;
        continue;
      }
      const expr_t *value =
          i < stmt->as.var.exprs.len ? stmt->as.var.exprs.data[i] : NULL;
      if (!value || value->kind != NY_E_BINARY || !value->as.binary.op ||
          strcmp(value->as.binary.op, "+") != 0 || !value->as.binary.left ||
          value->as.binary.left->kind != NY_E_IDENT ||
          !value->as.binary.left->as.ident.name ||
          strcmp(value->as.binary.left->as.ident.name, name) != 0 ||
          !value->as.binary.right ||
          value->as.binary.right->kind != NY_E_LITERAL ||
          value->as.binary.right->as.literal.kind != NY_LIT_INT ||
          value->as.binary.right->as.literal.as.i != 1)
        scan->escapes = true;
    }
  }
  return !scan->escapes && stmt->kind != NY_S_MODULE && stmt->kind != NY_S_FUNC;
}

static bool nyr_entry_binding_can_localize(const nyr_builder_t *b,
                                           const char *name, const char *symbol,
                                           const expr_t *init) {
  /* Bundle construction has finished collecting callees before it lowers
   * rt_main. Keep module storage and captured/address-taken bindings intact.
   * Localize before AST loop construction, rather than only after it, so the
   * existing reserve-once append transform can see private entry bindings. */
  if (!b->cache_owner || !b->profile_name ||
      strcmp(b->profile_name, "rt_main") != 0 || b->scope_depth != 0 ||
      (b->module_name && b->module_name[0]) || strchr(name, '.') || !init)
    return false;
  if (init->kind == NY_E_LIST) {
    if (init->as.list_like.len != 0)
      return false;
  } else if (init->kind != NY_E_LITERAL ||
             init->as.literal.kind != NY_LIT_INT ||
             init->tok.kind == NY_T_NIL || init->as.literal.as.i != 0) {
    return false;
  }
  bool counter = init->kind != NY_E_LIST;
  if (counter) {
    /* The early path only supplies induction slots to append-fill building.
     * Other scalars retain the existing post-lowering promotion policy. */
    bool has_list = false;
    for (size_t i = 0; i < b->local_count; ++i)
      has_list |= b->locals[i].is_list;
    if (!has_list)
      return false;
  }
  if (nyr_inline_callees_reference_symbol(symbol, NULL) ||
      (!counter && !ny_top_mut_scan(b, name, true)))
    return false;
  nyr_entry_binding_scan_t scan = {
      .builder = b, .name = name, .counter = counter};
  ny_visitor_t visitor = {.ctx = &scan,
                          .visit_expr_pre = nyr_entry_binding_expr,
                          .visit_stmt_pre = nyr_entry_binding_stmt};
  for (size_t i = 0; i < b->prog->body.len && !scan.escapes; ++i)
    ny_visit_stmt(&visitor, b->prog->body.data[i]);
  return !scan.escapes;
}

static const char *nyr_global_symbol(const nyr_builder_t *b, const char *name) {
  if (!b || !name)
    return NULL;
  const char *symbol = nyr_globaltab_name(name);
  if (b->module_name && b->module_name[0]) {
    char qualified[512];
    int n =
        snprintf(qualified, sizeof(qualified), "%s.%s", b->module_name, name);
    if (n > 0 && (size_t)n < sizeof(qualified)) {
      symbol = nyr_globaltab_name(qualified);
      if (symbol)
        return symbol;
    }
  }
  /* A bare tail is ambiguous once stdlib modules are loaded: a user binding
   * such as `c` must not silently become core.counter.c.  The main
   * program has no namespace-qualified global to fall back to, so accept
   * only an exact registration there. */
  if (symbol)
    return symbol;
  if (b->profile_name && strcmp(b->profile_name, "rt_main") == 0 &&
      (!b->module_name || !b->module_name[0]))
    return nyr_find_imported_value((const nyr_builder_impl_t *)b, name)
               ? nyr_globaltab_name_tail(name)
               : NULL;
  symbol = nyr_globaltab_name_tail(name);
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
  return n > 0 && (size_t)n < sizeof(qualified) ? nyr_globaltab_name(qualified)
                                                : NULL;
}
/* Find a function's containing module in one traversal.  The previous
 * implementation recursively searched a subtree and then searched the same
 * subtree again with `ny_native_stmt_contains_fn`; imported stdlib trees made
 * that quadratic and turned module lookup into the dominant native compile
 * cost. */
static bool ny_native_find_fn_module_in_stmt(const stmt_t *s, const stmt_t *fn,
                                             const char **module) {
  if (!s || !fn || !module)
    return false;
  if (s == fn)
    return true;
  const ny_stmt_list *body = NULL;
  switch (s->kind) {
  case NY_S_MODULE:
    body = &s->as.module.body;
    break;
  case NY_S_BLOCK:
    body = &s->as.block.body;
    break;
  case NY_S_IMPL:
    body = &s->as.impl.methods;
    break;
  case NY_S_LAYOUT:
    body = &s->as.layout.methods;
    break;
  case NY_S_STRUCT:
    body = &s->as.struc.methods;
    break;
  default:
    return false;
  }
  for (size_t i = 0; i < body->len; ++i) {
    if (!ny_native_find_fn_module_in_stmt(body->data[i], fn, module))
      continue;
    if (!*module && s->kind == NY_S_MODULE)
      *module = s->as.module.name;
    return true;
  }
  return false;
}

static const char *ny_native_fn_module(ny_native_fn_index_t *index,
                                       const program_t *prog,
                                       const stmt_t *fn) {
  if (!prog || !fn)
    return NULL;
  const char *module = NULL;
  bool indexed = ny_native_fn_cache_module(index, prog, fn, &module);
  if (!indexed)
    for (size_t i = 0; i < prog->body.len; ++i)
      if (ny_native_find_fn_module_in_stmt(prog->body.data[i], fn, &module) &&
          module)
        break;
  if (module)
    return module;
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

static bool nyr_expr_uses_ident(const nyr_builder_t *b, const expr_t *e,
                                const char *name);
static bool nyr_stmt_uses_ident(const stmt_t *s, const char *name);

#define NY_NIR_SCAN_DESCEND(EXPR)                                              \
  do {                                                                         \
    if (nyr_expr_uses_ident(b, (EXPR), name))                                  \
      return true;                                                             \
  } while (0)

static bool nyr_expr_uses_ident(const nyr_builder_t *b, const expr_t *e,
                                const char *name) {
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
           nyr_stmt_uses_ident(e->as.comptime_expr.body, name);
  case NY_E_MATCH:
    NY_NIR_SCAN_DESCEND(e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        NY_NIR_SCAN_DESCEND(arm->patterns.data[j]);
      NY_NIR_SCAN_DESCEND(arm->guard);
      if (arm->conseq && nyr_stmt_uses_ident(arm->conseq, name))
        return true;
    }
    return e->as.match.default_conseq &&
           nyr_stmt_uses_ident(e->as.match.default_conseq, name);
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
      return nyr_stmt_uses_ident(e->as.lambda.body, name);
    return false;
  default:
    return false;
  }
}

static bool nyr_stmt_uses_ident(const stmt_t *s, const char *name) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_VAR:
    if (!s->as.var.is_decl)
      for (size_t i = 0; i < s->as.var.names.len; ++i)
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return true;
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (nyr_expr_uses_ident(NULL, s->as.var.exprs.data[i], name))
        return true;
    return false;
  case NY_S_EXPR:
    return nyr_expr_uses_ident(NULL, s->as.expr.expr, name);
  case NY_S_RETURN:
    if (s->as.ret.value)
      return nyr_expr_uses_ident(NULL, s->as.ret.value, name);
    return false;
  case NY_S_BLOCK:
    if (s->as.block.body.data)
      for (size_t i = 0; i < s->as.block.body.len; ++i)
        if (s->as.block.body.data[i] &&
            nyr_stmt_uses_ident(s->as.block.body.data[i], name))
          return true;
    return false;
  case NY_S_IF:
    return nyr_expr_uses_ident(NULL, s->as.iff.test, name) ||
           nyr_stmt_uses_ident(s->as.iff.init, name) ||
           nyr_stmt_uses_ident(s->as.iff.conseq, name) ||
           nyr_stmt_uses_ident(s->as.iff.alt, name);
  case NY_S_GUARD:
    return nyr_expr_uses_ident(NULL, s->as.guard.value, name) ||
           nyr_stmt_uses_ident(s->as.guard.fallback, name);
  case NY_S_WHILE:
    return nyr_stmt_uses_ident(s->as.whl.init, name) ||
           nyr_expr_uses_ident(NULL, s->as.whl.test, name) ||
           nyr_expr_uses_ident(NULL, s->as.whl.invariant, name) ||
           nyr_stmt_uses_ident(s->as.whl.body, name) ||
           nyr_stmt_uses_ident(s->as.whl.update, name);
  case NY_S_FOR:
    return nyr_expr_uses_ident(NULL, s->as.fr.iterable, name) ||
           nyr_expr_uses_ident(NULL, s->as.fr.cond, name) ||
           nyr_stmt_uses_ident(s->as.fr.init, name) ||
           nyr_stmt_uses_ident(s->as.fr.body, name) ||
           nyr_stmt_uses_ident(s->as.fr.update, name);
  case NY_S_TRY:
    return nyr_stmt_uses_ident(s->as.tr.body, name) ||
           nyr_stmt_uses_ident(s->as.tr.handler, name);
  case NY_S_DEFER:
    return nyr_stmt_uses_ident(s->as.de.body, name);
  case NY_S_MATCH:
    if (nyr_expr_uses_ident(NULL, s->as.match.test, name))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (nyr_expr_uses_ident(NULL, arm->patterns.data[j], name))
          return true;
      if (nyr_expr_uses_ident(NULL, arm->guard, name) ||
          nyr_stmt_uses_ident(arm->conseq, name))
        return true;
    }
    return nyr_stmt_uses_ident(s->as.match.default_conseq, name);
  default:
    return false;
  }
}

static bool nyr_stmt_assigns_ident(const stmt_t *s, const char *name) {
  if (!s || !name || !*name)
    return false;
  switch (s->kind) {
  case NY_S_VAR:
    if (!s->as.var.is_decl)
      for (size_t i = 0; i < s->as.var.names.len; ++i)
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return true;
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_assigns_ident(s->as.block.body.data[i], name))
        return true;
    return false;
  case NY_S_IF:
    return nyr_stmt_assigns_ident(s->as.iff.init, name) ||
           nyr_stmt_assigns_ident(s->as.iff.conseq, name) ||
           nyr_stmt_assigns_ident(s->as.iff.alt, name);
  case NY_S_GUARD:
    return nyr_stmt_assigns_ident(s->as.guard.fallback, name);
  case NY_S_WHILE:
    return nyr_stmt_assigns_ident(s->as.whl.init, name) ||
           nyr_stmt_assigns_ident(s->as.whl.body, name) ||
           nyr_stmt_assigns_ident(s->as.whl.update, name);
  case NY_S_FOR:
    return nyr_stmt_assigns_ident(s->as.fr.init, name) ||
           nyr_stmt_assigns_ident(s->as.fr.body, name) ||
           nyr_stmt_assigns_ident(s->as.fr.update, name);
  case NY_S_TRY:
    return nyr_stmt_assigns_ident(s->as.tr.body, name) ||
           nyr_stmt_assigns_ident(s->as.tr.handler, name);
  case NY_S_DEFER:
    return nyr_stmt_assigns_ident(s->as.de.body, name);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (nyr_stmt_assigns_ident(s->as.match.arms.data[i].conseq, name))
        return true;
    return nyr_stmt_assigns_ident(s->as.match.default_conseq, name);
  default:
    return false;
  }
}

#undef NY_NIR_SCAN_DESCEND

static bool nyr_lower_var(nyr_builder_t *b, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_destructure) {
    /*
     * A literal sequence has no observable container identity: lower its
     * elements once, left-to-right, through the ordinary binding path. This
     * also preserves each element's inferred scalar/container ABI instead
     * of forcing every destructured value through a dynamic index accessor.
     */
    const expr_t *source = v->exprs.len == 1 ? v->exprs.data[0] : NULL;
    if (source && (source->kind == NY_E_LIST || source->kind == NY_E_TUPLE) &&
        source->as.list_like.len == v->names.len) {
      stmt_t expanded = *s;
      expanded.as.var = *v;
      expr_t **elements = (expr_t **)alloca(sizeof(*elements) * v->names.len);
      for (size_t i = 0; i < v->names.len; ++i)
        elements[i] = source->as.list_like.data[i];
      expanded.as.var.is_destructure = false;
      expanded.as.var.exprs.data = elements;
      expanded.as.var.exprs.len = v->names.len;
      expanded.as.var.exprs.cap = v->names.len;
      return nyr_lower_var(b, &expanded);
    }
    return nyr_fail(
        b, "native NYIR lower: only simple def/mut bindings are supported");
  }
  if (v->is_del) {
    for (size_t i = 0; i < v->names.len; ++i) {
      const char *name = v->names.data[i];
      if (!name || strcmp(name, "_") == 0)
        continue;
      int nil = nyr_emit_const(b, 0);
      if (nil < 0)
        return false;
      nyr_local_t *local = nyr_find_local(b, name);
      if (local) {
        if (!nyr_store_local_value(b, local->slot, nil))
          return false;
        b->last_value = nil;
        continue;
      }
      const char *sym = nyr_global_symbol(b, name);
      if (sym) {
        int addr = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = 0,
                                                      .symbol = sym});
        if (addr < 0 || !nyr_emit_store_i64(b, addr, nil))
          return false;
        b->last_value = nil;
        continue;
      }
      nyr_fail(b, "native NYIR lower: cannot del unbound local '%s'", name);
      return false;
    }
    return true;
  }
  int *pre_vals = NULL;
  if (v->names.len > 1) {
    pre_vals = (int *)alloca(sizeof(int) * v->names.len);
    for (size_t i = 0; i < v->names.len; ++i) {
      if (i < v->exprs.len && v->exprs.data[i]) {
        pre_vals[i] = nyr_lower_expr(b, v->exprs.data[i]);
        if (pre_vals[i] < 0)
          return false;
        /*
         * Sequential binding: if a later initializer references this name
         * (for example `def p, k = its.get(i), p.get(0)`), bind and store it
         * now so that reference resolves as a local instead of a bogus named
         * data symbol.
         */
        for (size_t j = i + 1;
             v->is_decl && j < v->names.len && j < v->exprs.len; ++j) {
          if (v->exprs.data[j] &&
              nyr_expr_uses_ident(b, v->exprs.data[j], v->names.data[i])) {
            nyr_local_t *dep = nyr_bind_local(b, v->names.data[i]);
            if (dep && !nyr_store_local_value(b, dep->slot, pre_vals[i]))
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
      return nyr_fail(b, "native NYIR lower: local '%s' needs an initializer",
                      name);
    if (i < v->types.len && v->types.data[i]) {
      const char *decl_type = v->types.data[i];
      if (!nyr_check_refine_obligation(b, s->tok, decl_type,
                                       (expr_t *)v->exprs.data[i]))
        return false;
    }
    /* The native lowering path must preserve the source linter contract too.
     * Its compact local table otherwise bypasses scope_bind(), so nested
     * declarations could silently lose W2002 under --warn-all. */
    if (v->is_decl && b->scope_depth > 0 && nyr_find_local(b, name) &&
        ny_diag_should_emit("shadow_local", s->tok, name)) {
      ny_diag_warning_code(s->tok, 2002,
                           "declaration of %s'%s'%s shadows a previous local",
                           clr(NY_CLR_BOLD), name, clr(NY_CLR_RESET));
      ny_diag_hint("this may hide the outer variable — use a different name if "
                   "unintended");
    }
    const expr_t *global_init = v->exprs.data[i];
    const char *global_symbol = NULL;
    /* Top-level bindings are module storage, including mutable bindings.
     * Functions and the entry point must observe the same cell; treating a
     * root `mut` as an entry-frame local disconnects its initializer from
     * accesses emitted in functions and from deferred cleanup. */
    if (!nyr_find_local(b, name) &&
        (!v->is_decl || (b->scope_depth == 0 && b->profile_name &&
                         strcmp(b->profile_name, "rt_main") == 0)))
      global_symbol = nyr_global_symbol(b, name);
    const char *entry_type = i < v->types.len ? v->types.data[i] : NULL;
    if (global_symbol && v->is_decl && v->is_mut &&
        !ny_native_stmt_is_stdlib(s) &&
        (!entry_type || ny_native_type_name_is_int(entry_type) ||
         ny_native_type_name_is_list(entry_type)) &&
        nyr_entry_binding_can_localize(b, name, global_symbol, global_init))
      global_symbol = NULL;
    if (global_symbol) {
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
           strcmp(global_init->as.call.callee->as.ident.name, "core.list") ==
               0) &&
          global_init->as.call.args.len == 1 &&
          !global_init->as.call.args.data[0].name) {
        /* `mut list<int> xs = list(n)` as a global: emit rt_list_new_sized
         * so indexed stores at 0..n-1 succeed immediately. */
        int arg = nyr_lower_expr(b, global_init->as.call.args.data[0].val);
        value = arg < 0 ? -1
                        : nyr_emit_runtime_call(b, "rt_list_new_sized", arg, -1,
                                                -1, 1, 0);
      } else {
        value = pre_vals ? pre_vals[i] : nyr_lower_expr(b, global_init);
      }
      bool global_decl_any = i < v->types.len && v->types.data[i] &&
                             ny_native_type_name_is_any(v->types.data[i]);
      bool raw_bitwise_initializer =
          nyr_raw_bitwise_expression(b, global_init, 0);
      bool global_init_tagged_call = false;
      if (global_init && global_init->kind == NY_E_CALL) {
        const stmt_t *callee = global_init->semantic.canonical_callee_stmt;
        if (!callee && global_init->as.call.callee &&
            global_init->as.call.callee->kind == NY_E_IDENT &&
            global_init->as.call.callee->as.ident.name)
          callee = nyr_find_user_function(
              b, global_init->as.call.callee->as.ident.name);
        global_init_tagged_call =
            callee &&
            ((callee->as.fn.return_type &&
              ny_native_type_name_is_any(callee->as.fn.return_type)) ||
             (callee->as.fn.return_semantic.resolved &&
              callee->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC));
      }
      /* The source wrapper for bor/band/bxor has an `any` signature, while
       * native lowering deliberately returns a raw integer. Do not trust the
       * wrapper declaration as evidence that the produced word is tagged. */
      if (raw_bitwise_initializer)
        global_init_tagged_call = false;
      /* Compatibility bitwise primitives return raw integer payloads even
       * though their public wrapper type is `any`.  When such a value
       * initializes an explicitly dynamic global, store the canonical tagged
       * Ny value so later generic reads do not decode the raw integer again. */
      bool inferred_dynamic_global =
          raw_bitwise_initializer &&
          (i >= v->types.len || !v->types.data[i] || global_decl_any);
      if (value >= 0 && (global_decl_any || inferred_dynamic_global) &&
          global_init && !global_init_tagged_call &&
          global_init->tok.kind != NY_T_NIL &&
          !nyr_value_is_tagged_dynamic_call(b, value) &&
          nyr_value_scalar_class(b, value) != NYIR_SCALAR_I64_TAGGED &&
          !nyr_expr_is_any(b, global_init) &&
          !nyr_expr_is_f64(b, global_init) &&
          !nyr_expr_is_f32(b, global_init) &&
          !nyr_expr_is_bool(b, global_init) &&
          !nyr_expr_is_cstr(b, global_init) &&
          !nyr_expr_is_bytes(b, global_init) &&
          !nyr_expr_is_ptr(b, global_init))
        value = nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_TAGGED);
      if (value >= 0 && global_init && global_init->kind == NY_E_BINARY &&
          nyr_expr_is_any(b, global_init) &&
          !(b->last_arith_raw && b->last_arith_expr == global_init) &&
          !nyr_expr_is_raw_dynamic_read(b, global_init) &&
          global_init->as.binary.op &&
          (strcmp(global_init->as.binary.op, "+") == 0 ||
           strcmp(global_init->as.binary.op, "*") == 0 ||
           strcmp(global_init->as.binary.op, "/") == 0 ||
           strcmp(global_init->as.binary.op, "%") == 0)) {
        const expr_t *target_init = nyr_find_top_level_value(b, name);
        if (target_init && target_init->kind == NY_E_LITERAL &&
            target_init->as.literal.kind == NY_LIT_INT &&
            target_init->tok.kind != NY_T_NIL)
          value =
              nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
      }
      /*
       * Element reads return the tagged dynamic payload.  A global declared
       * with an integer literal is a raw slot: decode once here, otherwise
       * `mut a = 0; a = pair.get(0)` stores the encoded word (23) where the
       * source element is 11.
       */
      if (value >= 0 && global_init && global_init->kind == NY_E_MEMCALL &&
          global_init->as.memcall.name &&
          (strcmp(global_init->as.memcall.name, "get") == 0 ||
           strcmp(global_init->as.memcall.name, "pop") == 0) &&
          nyr_value_scalar_class(b, value) != NYIR_SCALAR_I64_RAW &&
          !nyr_expr_is_f64(b, global_init) &&
          !nyr_expr_is_f32(b, global_init) &&
          !nyr_expr_is_cstr(b, global_init) &&
          !nyr_expr_is_bytes(b, global_init) &&
          !nyr_expr_is_range(b, global_init) &&
          !nyr_expr_is_bool(b, global_init)) {
        const expr_t *target_init = nyr_find_top_level_value(b, name);
        if (target_init && target_init->kind == NY_E_LITERAL &&
            target_init->as.literal.kind == NY_LIT_INT &&
            target_init->tok.kind != NY_T_NIL)
          value =
              nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
      }
      /* A subscript read crosses the same tagged boundary as `.get`, and a
       * global re-assignment must decode it before raw slot storage:
       * `mut cur = 0; cur = next_[cur]` otherwise stores tagged 19 where the
       * element is 9, so later raw arithmetic and prints see the encoding. */
      if (value >= 0 && global_init && global_init->kind == NY_E_INDEX &&
          global_init->as.index.target &&
          !(global_init->semantic.resolved &&
            global_init->semantic.rep == NY_SEM_REP_RAW_INT) &&
          (nyr_expr_is_dyn_list(b, global_init->as.index.target) ||
           nyr_expr_is_dict(b, global_init->as.index.target) ||
           nyr_expr_is_any(b, global_init->as.index.target)) &&
          !nyr_expr_is_f64(b, global_init) &&
          !nyr_expr_is_f32(b, global_init) &&
          !nyr_expr_is_cstr(b, global_init) &&
          !nyr_expr_is_bytes(b, global_init) &&
          !nyr_expr_is_range(b, global_init) &&
          !nyr_expr_is_bool(b, global_init)) {
        const expr_t *target_init = nyr_find_top_level_value(b, name);
        if (target_init && target_init->kind == NY_E_LITERAL &&
            target_init->as.literal.kind == NY_LIT_INT &&
            target_init->tok.kind != NY_T_NIL)
          value =
              nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
      }
      if (value >= 0 && global_init && global_init->kind == NY_E_IDENT) {
        nyr_local_t *source_local =
            nyr_find_local(b, global_init->as.ident.name);
        const expr_t *target_init = nyr_find_top_level_value(b, name);
        /* Global slots retain the representation established by their
         * declaration.  In particular, `mut caught = ""` is a raw string
         * slot even when the catch binding is `any`; unboxing that binding as
         * an integer preserves a tagged string handle and loses its C-string
         * ABI at the next call. */
        bool target_is_string = target_init && nyr_expr_is_cstr(b, target_init);
        if (source_local && source_local->is_any && !target_is_string) {
          if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
            ny_dbg_heuristic_event(
                "native-global-store",
                "function=%s target=%s source=%s source-bool=%s "
                "action=decode-any-local",
                b->current_fn_name ? b->current_fn_name : "<entry>", name,
                global_init->as.ident.name,
                source_local->is_bool ? "yes" : "no");
          value =
              nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
        }
      }
      /* A top-level declaration is an ABI contract just like a local one.
       * Do not infer the global slot only from the initializer: an overloaded
       * expression can retain a conservative dynamic annotation even when
       * `def f64 value = ...` has already fixed the storage and consumer ABI.
       */
      const expr_t *decl_init = nyr_find_top_level_value(b, name);
      const stmt_t *global_decl = nyr_find_top_level_var_stmt(b, name);
      const char *global_decl_type = NULL;
      if (global_decl && global_decl->kind == NY_S_VAR) {
        for (size_t di = 0; di < global_decl->as.var.names.len; ++di) {
          if (!global_decl->as.var.names.data[di] ||
              strcmp(global_decl->as.var.names.data[di], name) != 0)
            continue;
          if (di < global_decl->as.var.types.len)
            global_decl_type = global_decl->as.var.types.data[di];
          break;
        }
      }
      bool global_raw_integer =
          global_decl_type ? ny_native_type_name_is_int(global_decl_type)
                           : decl_init && !global_init_tagged_call &&
                                 !nyr_expr_is_bool(b, decl_init) &&
                                 nyr_expr_is_raw_int(decl_init);
      /* Reassignments keep the global's declared storage ABI.  Inference may
       * stamp `seen + suffix` as f64 after an earlier dynamic operation, but
       * a global introduced by a string literal is still a C-string pointer.
       * A float store would read the pointer from an XMM value slot and
       * corrupt the global in the in-memory native object path. */
      if (!v->is_decl && global_raw_integer && value >= 0 &&
          nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED) {
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "native-global-store",
              "function=%s target=%s target-type=%s source-bool=%s "
              "action=decode-tagged-integer",
              b->current_fn_name ? b->current_fn_name : "<entry>", name,
              global_decl_type ? global_decl_type : "<inferred>",
              nyr_expr_is_bool(b, global_init) ? "yes" : "no");
        value = nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
        if (value < 0)
          return false;
      }
      bool global_is_f64 = !(decl_init && nyr_expr_is_cstr(b, decl_init)) &&
                           ((i < v->types.len && v->types.data[i] &&
                             ny_native_type_name_is_f64(v->types.data[i])) ||
                            nyr_expr_is_f64(b, global_init));
      bool global_is_f32 = !(decl_init && nyr_expr_is_cstr(b, decl_init)) &&
                           ((i < v->types.len && v->types.data[i] &&
                             ny_native_type_name_is_f32(v->types.data[i])) ||
                            (decl_init && nyr_expr_is_f32(b, decl_init)));
      if (global_is_f32 && value >= 0) {
        if (!nyr_expr_is_f32(b, global_init))
          value = nyr_expr_is_f64(b, global_init)
                      ? nyr_emit_f64_to_f32(b, value)
                      : nyr_emit_i64_to_f32(b, value);
        if (value >= 0)
          value = nyr_emit_f32_to_f64(b, value);
      }
      /*
       * A float storage contract must receive float bits. When the
       * initializer's operator lowering kept a dynamic operand in the
       * canonical tagged domain (the rt_any_mul bridge and siblings),
       * decode at the cell boundary instead of storing the tagged handle
       * word under the global's f64/f32 storage class.
       */
      if ((global_is_f64 || global_is_f32) && value >= 0 &&
          nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED) {
        int as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                           NYIR_SCALAR_F64);
        if (as_f64 < 0)
          return false;
        if (global_is_f32) {
          as_f64 = nyr_emit_f64_to_f32(b, as_f64);
          if (as_f64 < 0)
            return false;
        }
        value = as_f64;
      }
      int addr = value < 0 ? -1
                           : nyr_ir_emit(&b->nyir,
                                         (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = 0,
                                                      .symbol = global_symbol});
      if (value < 0 || addr < 0 ||
          !((global_is_f64 || global_is_f32)
                ? nyr_emit_store_f64(b, addr, value)
                : nyr_emit_store_i64(b, addr, value)))
        return false;
      b->last_value = value;
      continue;
    }
    bool is_f64 =
        i < v->types.len && ny_native_type_name_is_f64(v->types.data[i]);
    bool is_f32 =
        i < v->types.len && ny_native_type_name_is_f32(v->types.data[i]);
    const char *declared_type = i < v->types.len ? v->types.data[i] : NULL;
    bool is_cstr = i < v->types.len && v->types.data[i] &&
                   strcmp(v->types.data[i], "str") == 0;
    bool is_any = i < v->types.len && v->types.data[i] &&
                  strcmp(v->types.data[i], "any") == 0;
    nyr_local_t *prior_local =
        !v->is_decl ? nyr_find_local_nearest(b, name) : NULL;
    bool prior_raw_integer_slot =
        prior_local && !prior_local->is_any && !prior_local->is_f64 &&
        !prior_local->is_f32 && !prior_local->is_cstr &&
        !prior_local->is_bytes && !prior_local->is_bigint &&
        !prior_local->is_list && !prior_local->is_dict &&
        prior_local->semantic_rep == NY_SEM_REP_RAW_INT;
    bool prior_dynamic_slot =
        prior_local && (prior_local->is_any ||
                        prior_local->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC);
    bool prior_f64_slot = prior_local && prior_local->is_f64;
    bool prior_f32_slot = prior_local && prior_local->is_f32;
    bool prior_bigint_slot = prior_local && prior_local->is_bigint;
    /* A mutable local keeps one ABI for its entire lifetime.  Once an
     * inferred local has received a dynamic value, later assignments must
     * remain dynamic even when the new expression is statically numeric;
     * otherwise the f64 assignment marks the physical slot as floating and
     * every earlier/later any load is emitted through XMM instead of GPR.
     * This is the generic representation join used by all mutable locals,
     * not a library-specific rule. */
    if (!v->is_decl && prior_dynamic_slot) {
      is_any = true;
      is_f64 = false;
      is_f32 = false;
    }
    /* Mutable float locals keep their established native storage class across
     * assignments, just as dynamic and raw-integer locals do. HM can attach a
     * stale nominal fact (for example `bigint`) to a later compound update;
     * letting that fact rewrite the slot makes subsequent native float
     * consumers decode an F64 register as a tagged value. */
    if (!v->is_decl && !prior_dynamic_slot && prior_f64_slot) {
      is_any = false;
      is_f64 = true;
      is_f32 = false;
    } else if (!v->is_decl && !prior_dynamic_slot && prior_f32_slot) {
      is_any = false;
      is_f64 = false;
      is_f32 = true;
    }
    /* An assignment updates an existing slot; it does not redeclare the
     * container with the inferred type of the RHS.  Keep the declaration's
     * typed-list ABI available while lowering `xs = xs.append(...)`. */
    const char *prior_type_name = prior_local ? prior_local->type_name : NULL;
    bool prior_typed_scalar_list =
        prior_local && prior_local->is_list && !prior_local->is_any &&
        prior_type_name && !ny_native_type_name_is_dyn_list(prior_type_name);
    bool prior_dyn_list = prior_local && prior_local->is_dyn_list;
    /*
     * Resolve a call initializer's callee once: its declared return type
     * classifies the receiving local (list/bytes container ABI) below.
     */
    const stmt_t *init_call_fn = NULL;
    const char *init_call_return_type = NULL;
    if (i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_MEMBER)
      init_call_return_type = nyr_layout_member_type(b, v->exprs.data[i]);
    if (i < v->exprs.len && v->exprs.data[i] &&
        (v->exprs.data[i]->kind == NY_E_CALL ||
         v->exprs.data[i]->kind == NY_E_MEMCALL)) {
      const expr_t *initializer = v->exprs.data[i];
      init_call_return_type = nyr_expr_type_name(b, initializer);
      init_call_fn = initializer->semantic.canonical_callee_stmt;
      if ((!init_call_fn || init_call_fn->kind != NY_S_FUNC) &&
          initializer->semantic.canonical_callee) {
        init_call_fn =
            nyr_find_user_function(b, initializer->semantic.canonical_callee);
        if (!init_call_fn)
          init_call_fn = nyr_find_imported_function(
              b, initializer->semantic.canonical_callee);
      }
      if (initializer->kind == NY_E_CALL) {
        const expr_t *call = initializer;
        if ((!init_call_fn || init_call_fn->kind != NY_S_FUNC) &&
            call->as.call.callee && call->as.call.callee->kind == NY_E_IDENT &&
            call->as.call.callee->as.ident.name) {
          init_call_fn =
              nyr_find_user_function(b, call->as.call.callee->as.ident.name);
          if (!init_call_fn)
            init_call_fn = nyr_find_imported_function(
                b, call->as.call.callee->as.ident.name);
        }
      }
      /* HM can leave a call expression's result type unresolved even when
       * native name resolution found the declaration in the current module.
       * The declaration's explicit result type is authoritative for an
       * inferred local: losing `int` here made iterator `n` slots load as
       * tagged, so a raw length of 3 was decoded as 1 when passed onward. */
      if (init_call_fn && init_call_fn->kind == NY_S_FUNC &&
          init_call_fn->as.fn.return_type &&
          (ny_native_type_name_is_any(init_call_fn->as.fn.return_type) ||
           !init_call_return_type ||
           ny_native_type_name_is_any(init_call_return_type) ||
           nyr_rep_for_type(b, init_call_return_type) == NY_SEM_REP_UNRESOLVED))
        init_call_return_type = init_call_fn->as.fn.return_type;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) &&
          (!declared_type || ny_native_type_name_is_any(declared_type)))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s declared_local=%s call_return=%s "
            "canonical=%s stmt=%s",
            b->current_fn_name ? b->current_fn_name : "<entry>", name,
            declared_type ? declared_type : "<none>",
            init_call_return_type ? init_call_return_type : "<unknown>",
            initializer->semantic.canonical_callee
                ? initializer->semantic.canonical_callee
                : "<none>",
            init_call_fn && init_call_fn->kind == NY_S_FUNC
                ? init_call_fn->as.fn.return_type
                      ? init_call_fn->as.fn.return_type
                      : "<inferred>"
                : "<unresolved>");
    }
    /* A declared call result or layout field type is stronger than HM's
     * generic `any` initializer fallback. Keep the producer's carrier class
     * so later overload, member, and return lowering use that same ABI. */
    bool concrete_call_result =
        init_call_return_type &&
        !ny_native_type_name_is_any(init_call_return_type) &&
        nyr_rep_for_type(b, init_call_return_type) != NY_SEM_REP_UNRESOLVED &&
        nyr_rep_for_type(b, init_call_return_type) != NY_SEM_REP_TAGGED_DYNAMIC;
    /* A resolved callee return rep is the authoritative slot-carrier fact when
     * it does not contradict the declared return type name; the name-derived
     * classification below only fills the gap the semantic layer leaves
     * unresolved. */
    bool init_call_sem_resolved = false;
    ny_sem_rep_t init_call_sem_rep = NY_SEM_REP_UNRESOLVED;
    if (init_call_fn && init_call_fn->kind == NY_S_FUNC &&
        init_call_fn->as.fn.return_semantic.resolved &&
        (!init_call_fn->as.fn.return_type ||
         ny_sem_rep_coherent_with_type(
             init_call_fn->as.fn.return_type,
             init_call_fn->as.fn.return_semantic.rep))) {
      init_call_sem_resolved = true;
      init_call_sem_rep = init_call_fn->as.fn.return_semantic.rep;
    }
    /* BigInt call results are one-word tagged values in native code. HM can
     * stamp an inferred mutable binding as `number` when its initializer is
     * the Z constructor, even though the canonical call contract is bigint.
     * That stale f64 slot stamp survives later bigint assignments and makes
     * comparisons reload the tagged word through an XMM slot. */
    bool bigint_call_result =
        init_call_fn && init_call_fn->kind == NY_S_FUNC &&
        init_call_fn->as.fn.return_type &&
        ny_type_is_bigint(init_call_fn->as.fn.return_type);
    /* `Z(...)` has a concrete BigInt result contract even when HM leaves an
     * inferred binding with its generic `number` fallback.  BigInt's native
     * carrier is a tagged word, so that fallback must not create a raw/f64
     * local or trigger the ordinary tagged-to-integer decode below. */
    bool bigint_result_overrides_inferred_type =
        bigint_call_result &&
        (!declared_type || ny_native_type_name_is_any(declared_type) ||
         ny_type_is(declared_type, "number"));
    if (bigint_result_overrides_inferred_type) {
      is_any = false;
      is_f64 = false;
      is_f32 = false;
      is_cstr = false;
    }
    if ((!declared_type || ny_native_type_name_is_any(declared_type)) &&
        concrete_call_result) {
      ny_sem_rep_t call_rep = init_call_sem_resolved
                                  ? init_call_sem_rep
                                  : nyr_rep_for_type(b, init_call_return_type);
      /* BigInt is nominally aggregate-like but its native one-word carrier is
       * tagged for both immediate integers and heap handles. */
      if (ny_type_is(init_call_return_type, "bigint"))
        call_rep = NY_SEM_REP_TAGGED_DYNAMIC;
      is_any = false;
      is_f64 = call_rep == NY_SEM_REP_F64;
      is_f32 = call_rep == NY_SEM_REP_F32;
      is_cstr = call_rep == NY_SEM_REP_STRING;
    }
    if (!is_f64 && !is_f32 && i < v->exprs.len) {
      is_f64 = nyr_expr_is_f64(b, v->exprs.data[i]);
      if (is_f64) {
        /*
         * A resolved non-scalar nominal type outranks a stale f64 rep
         * stamp.  HM can resolve an impl operator's `self` result to F64
         * when the operator body flows through float helpers, but the
         * carrier (`def sum = a + b` over vec3 dict handles) is a container
         * handle, not a raw f64 register; binding it as f64 makes every
         * load of the local f64-classified and trips the call scalar-class
         * verifier at the next member chain.
         */
        const char *init_type = nyr_expr_type_name(b, v->exprs.data[i]);
        /* An inferred `any` is not a concrete carrier contract.  Preserve
         * the f64/f32 class proven by the lowered producer (for example
         * `dist * dist * dist` after __flt_sqrt); otherwise the binder boxes
         * the IEEE value while stamping the slot as f64, and a later divide
         * reads that boxed handle as its divisor.  Concrete nominal results
         * such as a vec3 handle still override stale scalar annotations. */
        if (init_type && !ny_native_type_name_is_any(init_type) &&
            !ny_native_type_name_is_f64(init_type) &&
            !ny_native_type_name_is_f32(init_type))
          is_f64 = false;
      }
    }
    /* Reassignments preserve a local's established physical ABI.  In
     * particular, an inferred integer min/max can carry a stale `number`
     * result annotation; widening the destination before lowering its RHS
     * makes a self-reference load the raw slot as f64. */
    if (!v->is_decl && prior_raw_integer_slot && i < v->exprs.len &&
        nyr_expr_has_raw_int_abi(b, v->exprs.data[i], 0)) {
      is_any = false;
      is_f64 = false;
      is_f32 = false;
    }
    const char *init_operator_result_type = NULL;
    bool init_operator_result_known = false;
    if (i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_BINARY &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_f64(v->types.data[i]))) {
      const stmt_t *init_operator = nyr_find_operator(b, v->exprs.data[i]);
      const char *operator_return =
          init_operator ? init_operator->as.oper.return_type : NULL;
      bool operator_returns_self =
          operator_return && (strcmp(operator_return, "self") == 0 ||
                              strcmp(operator_return, "?self") == 0);
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s selected_operator=%s return=%s "
            "self_return=%d",
            b->current_fn_name ? b->current_fn_name : "<entry>", name,
            init_operator && init_operator->as.oper.target
                ? init_operator->as.oper.target
                : "<none>",
            operator_return ? operator_return : "<unknown>",
            (int)operator_returns_self);
      if (operator_returns_self)
        init_operator_result_type =
            nyr_expr_type_name(b, v->exprs.data[i]->as.binary.left);
      else
        init_operator_result_type = operator_return;
      if (init_operator_result_type) {
        ny_sem_rep_t operator_rep =
            nyr_rep_for_type(b, init_operator_result_type);
        init_operator_result_known = true;
        if (operator_rep != NY_SEM_REP_F64 && operator_rep != NY_SEM_REP_F32) {
          is_any = operator_rep == NY_SEM_REP_TAGGED_DYNAMIC;
          is_f64 = false;
          is_f32 = false;
        }
      }
    }
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f32 = nyr_expr_is_f32(b, v->exprs.data[i]);
    /* An explicit f32 literal hint is the declared initializer ABI. HM can
     * still leave the containing top-level binding's type slot at its
     * default f64 class, so let the literal representation win for a fresh
     * binding. */
    if (v->is_decl && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_LITERAL &&
        v->exprs.data[i]->as.literal.kind == NY_LIT_FLOAT &&
        v->exprs.data[i]->as.literal.hint == NY_LIT_HINT_F32) {
      is_f32 = true;
      is_f64 = false;
    }
    /* Inferred assignments retain the representation chosen from their
     * concrete operands. Type inference can leave a stale f64 annotation on
     * an inlined f32 calculation (for example `sum = a + b` where `a` and
     * `b` are explicit f32 parameters). An explicitly typed declaration is
     * different: its destination ABI is authoritative and needs a widening
     * conversion when the initializer is f32. */
    if (!v->is_decl && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_BINARY) {
      const expr_t *init = v->exprs.data[i];
      bool operands_f32 = nyr_expr_is_f32(b, init->as.binary.left) &&
                          nyr_expr_is_f32(b, init->as.binary.right);
      bool operands_f64 = nyr_expr_is_f64(b, init->as.binary.left) ||
                          nyr_expr_is_f64(b, init->as.binary.right);
      if (operands_f32 && !operands_f64) {
        is_f64 = false;
        is_f32 = true;
      }
    }
    if (!is_f64 && !is_f32 && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_TERNARY &&
        nyr_expr_has_float_literal(v->exprs.data[i], 0))
      is_f64 = true;
    /* A dynamic sequence accessor returns an already canonical Ny value.
     * Its fallback expression type (often f64) describes a missing element,
     * not the physical result of the accessor. */
    bool canonical_dynamic_init =
        i < v->exprs.len &&
        nyr_expr_is_canonical_dynamic_value(b, v->exprs.data[i]);
    const expr_t *initializer = i < v->exprs.len ? v->exprs.data[i] : NULL;
    bool nil_default_dynamic_get =
        initializer && initializer->kind == NY_E_MEMCALL &&
        initializer->as.memcall.name &&
        strcmp(initializer->as.memcall.name, "get") == 0 &&
        initializer->as.memcall.args.len == 2 &&
        initializer->as.memcall.args.data[1].val &&
        initializer->as.memcall.args.data[1].val->tok.kind == NY_T_NIL &&
        initializer->as.memcall.target &&
        (nyr_expr_is_list(b, initializer->as.memcall.target) ||
         nyr_expr_is_dyn_list(b, initializer->as.memcall.target) ||
         nyr_expr_is_any(b, initializer) ||
         nyr_expr_is_any(b, initializer->as.memcall.target));
    /* A nil-default list read may return either an element or nil, so its
     * inferred float fallback does not fix the slot ABI. Keep it tagged even
     * when later float assignments make the inferred binding look numeric. */
    if (nil_default_dynamic_get &&
        (!declared_type || ny_native_type_name_is_any(declared_type))) {
      is_any = true;
      is_f64 = false;
      is_f32 = false;
    }
    /* A ternary with two proven raw-scalar arms has a scalar join even in a
     * function returning `any`.  The function return ABI must not leak into
     * the temporary's local ABI; return lowering boxes it only if it escapes
     * as the function result. */
    bool ternary_true_raw =
        i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_TERNARY &&
        nyr_expr_is_raw_scalar(b, v->exprs.data[i]->as.ternary.true_expr, 0);
    bool ternary_false_raw =
        i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_TERNARY &&
        nyr_expr_is_raw_scalar(b, v->exprs.data[i]->as.ternary.false_expr, 0);
    bool raw_scalar_ternary = i < v->exprs.len && v->exprs.data[i] &&
                              v->exprs.data[i]->kind == NY_E_TERNARY &&
                              ternary_true_raw && ternary_false_raw &&
                              !nyr_expr_is_f64(b, v->exprs.data[i]) &&
                              !nyr_expr_is_f32(b, v->exprs.data[i]);
    if (i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_TERNARY && getenv("NY_TRACE_ABI") &&
        b->current_fn_name && strstr(b->current_fn_name, "_vec_zip2"))
      fprintf(stderr,
              "[abi-ternary] fn=%s name=%s true_raw=%d false_raw=%d any=%d "
              "canon=%d f64=%d f32=%d\n",
              b->current_fn_name, name ? name : "<unnamed>", ternary_true_raw,
              ternary_false_raw, (int)nyr_expr_is_any(b, v->exprs.data[i]),
              (int)canonical_dynamic_init,
              (int)nyr_expr_is_f64(b, v->exprs.data[i]),
              (int)nyr_expr_is_f32(b, v->exprs.data[i]));
    if (raw_scalar_ternary)
      is_any = false;
    bool declared_raw_scalar =
        is_f64 || is_f32 || is_cstr || raw_scalar_ternary ||
        (declared_type && (strcmp(declared_type, "bool") == 0 ||
                           ny_native_type_name_is_int(declared_type)));
    if (canonical_dynamic_init && !declared_raw_scalar) {
      is_any = true;
      is_f64 = false;
      is_f32 = false;
    }
    /* Type inference can rediscover a float RHS after the initial slot join
     * above. An existing dynamic local keeps its tagged ABI across
     * assignments; the value is boxed when stored rather than narrowing the
     * local to an XMM slot. */
    if (!v->is_decl && prior_local && prior_local->is_any) {
      is_any = true;
      is_f64 = false;
      is_f32 = false;
    }
    if (!is_cstr && i < v->exprs.len)
      is_cstr = nyr_expr_is_cstr(b, v->exprs.data[i]);
    const stmt_t *enum_stmt = NULL;
    const stmt_enum_item_t *enum_item = NULL;
    int64_t enum_value = 0;
    bool is_scalar_enum =
        i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_IDENT &&
        nyr_find_enum_member(b, v->exprs.data[i]->as.ident.name, &enum_stmt,
                             &enum_item, &enum_value) &&
        enum_item && enum_item->fields.len == 0;
    if (!is_any && !is_cstr && i < v->exprs.len &&
        nyr_expr_is_any(b, v->exprs.data[i]) &&
        /* A declared scalar type owns the slot ABI: `mut int y = d["k"]`
         * stores the decoded scalar.  Marking the local any from the
         * dynamic-read initializer made the next compound assignment store
         * a raw sum into an any slot (rendered as true/nil). */
        !declared_raw_scalar &&
        /* Untyped integer literals have a raw scalar NYIR representation;
         * treating every conservative semantic fallback as tagged dynamic
         * sends later `mut total += value` operations through the boxed ABI. */
        (v->is_decl || !prior_local || prior_local->is_any) &&
        !is_scalar_enum &&
        !(v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_LITERAL &&
          v->exprs.data[i]->as.literal.kind == NY_LIT_INT) &&
        /* Typed scalar-buffer reads are raw payloads.  Their conservative
         * `any` semantic is provenance of the accessor, not the physical
         * representation of the result. */
        !nyr_expr_is_raw_dynamic_read(b, v->exprs.data[i]))
      is_any = true;
    bool promoted_top_mut_index =
        i < v->exprs.len && nyr_index_of_promoted_top_mut(b, v->exprs.data[i]);
    if (promoted_top_mut_index &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_any(v->types.data[i])) &&
        !is_f64 && !is_f32 && !is_cstr &&
        !nyr_expr_is_bool(b, v->exprs.data[i]) &&
        !nyr_expr_is_bytes(b, v->exprs.data[i]) &&
        !nyr_expr_is_list(b, v->exprs.data[i]) &&
        !nyr_expr_is_dict(b, v->exprs.data[i]))
      is_any = false;
    if (!is_any && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_DICT)
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
        v->exprs.data[i]->as.index.target &&
        /* A declared scalar type owns the slot ABI even when the read is
         * dynamic (see the guard above). */
        !(i < v->types.len && v->types.data[i] && v->types.data[i][0] &&
          strcmp(v->types.data[i], "any") != 0 &&
          ny_native_type_name_is_int(v->types.data[i]))) {
      const expr_t *index_target = v->exprs.data[i]->as.index.target;
      nyr_local_t *index_local =
          index_target->kind == NY_E_IDENT
              ? nyr_find_local(b, index_target->as.ident.name)
              : NULL;
      /* A descriptor-backed sequence has no raw scalar element ABI unless
       * whole-program analysis proved this top-level append-built list is
       * elem-8 int storage. Preserve the tagged representation on ordinary
       * dynamic reads, but let that proof govern both the returned value and
       * the receiving local's slot. */
      if (!promoted_top_mut_index &&
          (nyr_expr_is_any(b, index_target) ||
           (index_local && index_local->is_dyn_list)))
        is_any = true;
    }
    bool is_bool = i < v->types.len && v->types.data[i] &&
                   strcmp(v->types.data[i], "bool") == 0;
    if (!is_bool && i < v->exprs.len && nyr_expr_is_bool(b, v->exprs.data[i]))
      is_bool = true;
    /* A dynamic sequence accessor returns a tagged word at the expression
     * boundary, but an untyped scalar temporary such as
     * `mut k = perm[0]` is immediately decoded by the native lowering. Keep
     * the temporary's physical ABI raw so later `k -= 1` does not tag the
     * literal and accidentally subtract a Ny tag value. Container-shaped
     * elements and explicit `any` declarations remain dynamic. */
    bool indexed_raw_scalar = false;
    bool raw_scalar_alias = false;
    bool typed_global_int_index =
        i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_INDEX &&
        nyr_index_of_typed_global_int_list(b, v->exprs.data[i]);
    if (i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_INDEX &&
        v->exprs.data[i]->as.index.target &&
        v->exprs.data[i]->as.index.target->kind == NY_E_IDENT &&
        v->exprs.data[i]->as.index.target->as.ident.name &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_any(v->types.data[i])) &&
        !is_f64 && !is_f32 && !is_cstr && !is_bool &&
        !nyr_expr_is_bytes(b, v->exprs.data[i]) &&
        !nyr_expr_is_list(b, v->exprs.data[i]) &&
        !nyr_expr_is_dict(b, v->exprs.data[i])) {
      const nyr_local_t *index_local = nyr_find_local_nearest(
          b, v->exprs.data[i]->as.index.target->as.ident.name);
      if (typed_global_int_index || promoted_top_mut_index ||
          (index_local && index_local->is_list && !index_local->is_dyn_list &&
           index_local->type_name && !index_local->is_any)) {
        indexed_raw_scalar = true;
        is_any = false;
      }
    }
    /* A `.len()` member call returns the raw machine count regardless of the
     * receiver's dynamic representation.  `def n = json.len()` stores that
     * raw word into an untyped binding; recording the producer ABI here keeps
     * later consumers from decoding it as a tagged word (a length of 693
     * rendering as 346 through the any printer). */
    bool raw_len_member = false;
    if (i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_MEMCALL &&
        v->exprs.data[i]->as.memcall.name &&
        strcmp(v->exprs.data[i]->as.memcall.name, "len") == 0 &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_any(v->types.data[i])) &&
        !is_f64 && !is_f32 && !is_cstr && !is_bool) {
      raw_len_member = true;
      is_any = false;
    }
    /* Propagate the physical ABI through scalar aliases.  The semantic AST
     * can still describe `k` as unknown after `k = perm[0]`, but the local
     * table has already recorded that the indexed payload was decoded to a
     * raw integer. Without this join, `mut b = k; b -= 1` re-enters the
     * tagged arithmetic path. */
    if (!is_f64 && !is_f32 && !is_cstr && !is_bool && i < v->exprs.len &&
        v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_IDENT &&
        v->exprs.data[i]->as.ident.name) {
      const nyr_local_t *source_local =
          nyr_find_local_nearest(b, v->exprs.data[i]->as.ident.name);
      if (source_local && !source_local->is_any &&
          source_local->semantic_rep == NY_SEM_REP_RAW_INT) {
        is_any = false;
        raw_scalar_alias = true;
      }
    }
    nyr_local_t *l =
        v->is_decl ? nyr_bind_local_typed(b, name, is_f64, is_f32, is_cstr)
                   : nyr_add_local(b, name);
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        !(declared_type && ny_native_type_name_is_any(declared_type)) &&
        nyr_expr_is_dict(b, v->exprs.data[i])) {
      /* Preserve a statically known dictionary return through an unannotated
       * binding.  Treating `def value = make_dict()` as arbitrary `any`
       * routes `value.get()` through the generic reflection bridge, which
       * loses the tagged result ABI for native dictionaries. */
      l->is_any = false;
      l->is_dict = true;
      l->semantic_rep = NY_SEM_REP_OBJECT;
      if (!l->type_name || !l->type_name[0])
        l->type_name = "dict";
    }
    /* Establish the slot ABI before lowering its initializer.  The indexed
     * load can have a conservative tagged semantic stamp while its proven
     * elem-8 producer is raw; waiting until after STORE_LOCAL lets that stale
     * class become the slot's reaching class and makes a later rt_tag look
     * redundant to NYIR optimization. */
    if (l && !canonical_dynamic_init &&
        (indexed_raw_scalar || raw_scalar_alias || raw_len_member)) {
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    if (l && init_operator_result_known && init_operator_result_type) {
      /* The selected operator declaration determines the initializer's
       * result ABI. This covers `self` and explicit nominal return types such
       * as `vec3`, even when HM retained a stale scalar fact from the body. */
      l->is_any = is_any;
      l->is_f64 = is_f64;
      l->is_f32 = is_f32;
      l->type_name = init_operator_result_type;
      l->semantic_rep = nyr_rep_for_type(b, init_operator_result_type);
      l->is_dict = l->semantic_rep == NY_SEM_REP_AGGREGATE ||
                   l->semantic_rep == NY_SEM_REP_OBJECT;
    }
    /* A mutable local has one physical slot for its whole lifetime.  If a
     * previously raw scalar later receives a dynamic/float/object value,
     * join the slot to the tagged ABI before lowering that assignment.  The
     * old raw scalar is boxed exactly once so the RHS can safely read it in
     * the new representation. */
    const expr_t *assignment_rhs = i < v->exprs.len ? v->exprs.data[i] : NULL;
    /* The primitive bitwise calls below lower to raw NYIR operations inside
     * scalar functions.  Keep an existing raw accumulator raw before
     * lowering their RHS; boxing it here feeds a tagged word into the raw
     * operation even though the result slot is marked raw afterward. */
    bool raw_bitwise_update = false;
    if (prior_local && !prior_local->is_any &&
        prior_local->semantic_rep == NY_SEM_REP_RAW_INT && assignment_rhs &&
        assignment_rhs->kind == NY_E_CALL && b->current_fn_name) {
      const char *bit_leaf = ny_native_call_leaf(assignment_rhs);
      const stmt_t *owner = nyr_find_user_function(b, b->current_fn_name);
      bool raw_owner = owner && owner->as.fn.return_type &&
                       !ny_native_type_name_is_any(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_list(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f64(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f32(owner->as.fn.return_type);
      raw_bitwise_update =
          raw_owner && bit_leaf &&
          (strcmp(bit_leaf, "bor") == 0 || strcmp(bit_leaf, "band") == 0 ||
           strcmp(bit_leaf, "bxor") == 0 || strcmp(bit_leaf, "__or") == 0 ||
           strcmp(bit_leaf, "__and") == 0 || strcmp(bit_leaf, "__xor") == 0 ||
           strcmp(bit_leaf, "bshl") == 0 || strcmp(bit_leaf, "bshr") == 0 ||
           strcmp(bit_leaf, "__shl") == 0 || strcmp(bit_leaf, "__shr") == 0);
    }
    bool raw_compound_scalar_update =
        prior_local && !prior_local->is_any &&
        prior_local->semantic_rep == NY_SEM_REP_RAW_INT && assignment_rhs &&
        assignment_rhs->kind == NY_E_BINARY &&
        !nyr_expr_is_f64(b, assignment_rhs) &&
        !nyr_expr_is_f32(b, assignment_rhs) && assignment_rhs->as.binary.left &&
        assignment_rhs->as.binary.left->kind == NY_E_IDENT &&
        assignment_rhs->as.binary.left->as.ident.name &&
        strcmp(assignment_rhs->as.binary.left->as.ident.name, name) == 0;
    /*
     * A raw integer state update may wrap an addition in a mask or another
     * bitwise operation, so the synthetic `x = x + rhs` recognizer above is
     * too narrow. Preserve the slot ABI for these bitwise trees; lowering
     * unboxes genuinely dynamic operands at their input edges. This covers
     * SHA's `va = (va + a) & 0xffffffff` update without changing ordinary
     * integer-typed call arguments such as string-slice bounds.
     */
    bool raw_existing_int_binary_update =
        prior_local && !prior_local->is_any &&
        prior_local->semantic_rep == NY_SEM_REP_RAW_INT && assignment_rhs &&
        assignment_rhs->kind == NY_E_BINARY &&
        nyr_expr_is_bitwise_tree(b, assignment_rhs, 0) &&
        !nyr_expr_is_f64(b, assignment_rhs) &&
        !nyr_expr_is_f32(b, assignment_rhs) &&
        (!init_operator_result_known ||
         (init_operator_result_type &&
          ny_native_type_name_is_int(init_operator_result_type)));
    bool raw_dynamic_read =
        prior_local && !prior_local->is_any &&
        prior_local->semantic_rep == NY_SEM_REP_RAW_INT && assignment_rhs &&
        ((assignment_rhs->kind == NY_E_INDEX &&
          assignment_rhs->as.index.target &&
          !(assignment_rhs->as.index.target->kind == NY_E_IDENT &&
            nyr_index_read_is_proven_scalar(b, assignment_rhs)) &&
          (nyr_expr_is_any(b, assignment_rhs->as.index.target) ||
           nyr_expr_is_dyn_list(b, assignment_rhs->as.index.target) ||
           nyr_expr_is_dict(b, assignment_rhs->as.index.target))) ||
         (assignment_rhs->kind == NY_E_MEMCALL &&
          assignment_rhs->as.memcall.name &&
          strcmp(assignment_rhs->as.memcall.name, "get") == 0 &&
          assignment_rhs->as.memcall.target &&
          (nyr_expr_is_any(b, assignment_rhs->as.memcall.target) ||
           nyr_expr_is_dyn_list(b, assignment_rhs->as.memcall.target) ||
           nyr_expr_is_dict(b, assignment_rhs->as.memcall.target))));
    if (l && !v->is_decl && prior_local && !prior_local->is_any &&
        !prior_local->is_f64 && !prior_local->is_f32 && !prior_local->is_cstr &&
        !prior_local->is_bytes && !prior_local->is_dict &&
        !prior_local->is_list && !prior_local->is_bool && assignment_rhs &&
        !raw_compound_scalar_update) {
      const expr_t *rhs = assignment_rhs;
      bool dynamic_rhs = nyr_expr_is_any(b, rhs) || nyr_expr_is_f64(b, rhs) ||
                         nyr_expr_is_f32(b, rhs) || nyr_expr_is_cstr(b, rhs) ||
                         nyr_expr_is_bytes(b, rhs) ||
                         nyr_expr_is_dict(b, rhs) || nyr_expr_is_list(b, rhs);
      /* A compound scalar update such as `acc += xs[i]` has a raw
       * accumulator on the left and a dynamic sequence read on the right.
       * The arithmetic lowering decodes that read and returns a raw scalar;
       * promoting the accumulator before lowering the RHS would instead box
       * the old value and make every subsequent update use tagged words. */
      bool raw_dynamic_update =
          rhs->kind == NY_E_BINARY && !nyr_expr_is_f64(b, rhs) &&
          !nyr_expr_is_f32(b, rhs) && prior_local && !prior_local->is_any &&
          !prior_local->is_f64 && !prior_local->is_f32 &&
          !prior_local->is_cstr && !prior_local->is_bytes &&
          !prior_local->is_dict && !prior_local->is_list;
      const stmt_t *raw_call_callee =
          rhs->kind == NY_E_CALL && rhs->semantic.canonical_callee_stmt &&
                  rhs->semantic.canonical_callee_stmt->kind == NY_S_FUNC
              ? rhs->semantic.canonical_callee_stmt
              : NULL;
      bool raw_integer_call_update =
          rhs->kind == NY_E_CALL &&
          (nyr_expr_has_raw_int_abi(b, rhs, 0) ||
           (raw_call_callee &&
            raw_call_callee->as.fn.return_semantic.resolved &&
            raw_call_callee->as.fn.return_semantic.rep ==
                NY_SEM_REP_RAW_INT)) &&
          prior_local && !prior_local->is_any && !prior_local->is_f64 &&
          !prior_local->is_f32 && !prior_local->is_cstr &&
          !prior_local->is_bytes && !prior_local->is_dict &&
          !prior_local->is_list;
      raw_dynamic_update = raw_dynamic_update || raw_integer_call_update;

      if (dynamic_rhs && !raw_dynamic_update && !raw_dynamic_read &&
          !raw_bitwise_update) {
        int old = nyr_load_local_value(b, prior_local->slot);
        int boxed = old < 0
                        ? -1
                        : nyr_emit_runtime_call(b, "rt_tag", old, -1, -1, 1, 0);
        if (boxed < 0 || !nyr_store_local_value(b, prior_local->slot, boxed))
          return false;
        l->is_any = true;
        l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
        is_any = true;
      }
    }
    if (l && (raw_compound_scalar_update || raw_existing_int_binary_update)) {
      /* Compound assignments are represented as `x = x op rhs`.  Preserve
       * the established raw slot ABI across this synthetic assignment even
       * when semantic inference conservatively marks the binary expression
       * dynamic. */
      is_any = false;
      is_f64 = false;
      is_f32 = false;
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    /*
     * A local already bound to the dynamic/tagged representation stays
     * there across reassignments: `mut y = args.get(1, nil)` then `y = 0.0`
     * stores a boxed heap float into an any local.  Flipping the local to
     * is_f64 here re-stamps that boxed (tagged) store as a raw f64 class,
     * and the verifier's local-root merge then poisons every load of the
     * local — including the tagged get result on the other path.
     */
    /*
     * An f64 stamp from a checker guess ("number" for an unknown call)
     * must not survive a non-numeric initializer: the register holds the
     * callee's real result (a Builder handle), and an f64-labeled local
     * made every load of it an f64-classified pointer.
     */
    bool handle_init = l && i < v->exprs.len && v->exprs.data[i] &&
                       v->exprs.data[i]->kind == NY_E_CALL &&
                       !numeric_init_call(v->exprs.data[i]) &&
                       !nyr_expr_is_f64(b, v->exprs.data[i]);

    if (l && is_f64 && !is_any && !l->is_any && !handle_init)
      l->is_f64 = true;
    if (l && is_f32 && !is_any && !l->is_any && !handle_init)
      l->is_f32 = true;
    if (l && is_cstr)
      l->is_cstr = true;
    if (l &&
        ((i < v->types.len && ny_native_type_name_is_bytes(v->types.data[i])) ||
         nyr_expr_is_bytes(b, v->exprs.data[i]) ||
         (init_call_fn && init_call_fn->as.fn.return_type &&
          ny_native_type_name_is_bytes(init_call_fn->as.fn.return_type))))
      l->is_bytes = true;
    if (l && is_bool)
      l->is_bool = true;
    if (l && is_any && !prior_typed_scalar_list) {
      l->is_any = true;
      /*
       * The is_any flag alone is not the classification consumers read:
       * expr_is_any also requires the rep (or a literal `any` type name)
       * to agree.  A canonical dynamic initializer (`comptime{ ... .long }`
       * returning a tagged word) left the rep UNRESOLVED here, so integer
       * compares against raw odd constants matched the tag encoding
       * instead of the value.
       */
      if (nil_default_dynamic_get || l->semantic_rep == NY_SEM_REP_UNRESOLVED)
        l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
      /*
       * expr_is_any reads the flag AND (rep == TAGGED_DYNAMIC or a literal
       * `any` type name).  Later binder stages re-stamp the rep for
       * raw-bridge producers, so keep the literal type name as the stable
       * half of the contract.
       */
      if (!l->type_name || !l->type_name[0])
        l->type_name = "any";
    }
    if (l && i < v->types.len && v->types.data[i] && v->types.data[i][0] &&
        (v->is_decl || !prior_local || !prior_type_name))
      l->type_name = v->types.data[i];
    if (l && concrete_call_result && !prior_bigint_slot &&
        (!declared_type || ny_native_type_name_is_any(declared_type))) {
      ny_sem_rep_t call_rep = init_call_sem_resolved
                                  ? init_call_sem_rep
                                  : nyr_rep_for_type(b, init_call_return_type);
      if (ny_type_is(init_call_return_type, "bigint"))
        call_rep = NY_SEM_REP_TAGGED_DYNAMIC;
      l->type_name = init_call_return_type;
      l->semantic_rep = call_rep;
      l->is_any = call_rep == NY_SEM_REP_TAGGED_DYNAMIC;
      l->is_f64 = call_rep == NY_SEM_REP_F64;
      l->is_f32 = call_rep == NY_SEM_REP_F32;
      l->is_cstr = call_rep == NY_SEM_REP_STRING;
      l->is_list = call_rep == NY_SEM_REP_TYPED_BUFFER;
      l->is_dyn_list = l->is_list && i < v->exprs.len &&
                       nyr_expr_is_dyn_list(b, v->exprs.data[i]);
      l->is_dict =
          call_rep == NY_SEM_REP_AGGREGATE || call_rep == NY_SEM_REP_OBJECT;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s type=%s "
            "decision=use-declared-call-return-carrier rep=%d",
            b->current_fn_name ? b->current_fn_name : "<entry>", name,
            init_call_return_type, (int)call_rep);
    }
    if (ny_trace_enabled("NY_TRACE_OPRESOLVE") && l && name)
      ny_trace_line("OPRESOLVE", "%s name=%s semantic=%s owner=%s",
                    b->current_fn_name ? b->current_fn_name : "?", name,
                    (i < v->types.len && v->types.data[i]) ? v->types.data[i]
                                                           : "-",
                    l->type_name ? l->type_name : "-");
    if (l && i < v->exprs.len && v->exprs.data[i]) {
      const char *inferred = nyr_expr_type_name(b, v->exprs.data[i]);
      if (inferred && ((!l->type_name || !l->type_name[0]) ||
                       (ny_native_type_name_is_dyn_list(l->type_name) &&
                        ny_native_type_name_is_list(inferred) &&
                        !ny_native_type_name_is_dyn_list(inferred))))
        l->type_name = inferred;
    }
    /* An imported function's declared list return type is part of the call
     * ABI. Preserve `list<int>` on an unannotated receiving local instead of
     * collapsing every call result to an unknown descriptor list. */
    if (l && init_call_fn && init_call_fn->as.fn.return_type &&
        ny_native_type_name_is_list(init_call_fn->as.fn.return_type) &&
        !ny_native_type_name_is_dyn_list(init_call_fn->as.fn.return_type) &&
        (!l->type_name || ny_native_type_name_is_dyn_list(l->type_name)))
      l->type_name = init_call_fn->as.fn.return_type;
    /* An unannotated local initialized by a pointer-returning function keeps
     * the callee's pointer ABI. HM may leave the binding tagged/number-like,
     * but that would make subsequent pointer formals receive a dynamic word
     * (the Builder comma-binding path in str._utf8_slice). */
    const char *pointer_return_type = init_call_return_type;
    if (init_call_fn && init_call_fn->kind == NY_S_FUNC &&
        init_call_fn->as.fn.return_type &&
        ny_native_type_name_is_ptr(init_call_fn->as.fn.return_type))
      pointer_return_type = init_call_fn->as.fn.return_type;
    if (l && !declared_type && pointer_return_type &&
        ny_native_type_name_is_ptr(pointer_return_type)) {
      l->is_any = false;
      l->is_f64 = false;
      l->is_f32 = false;
      l->semantic_rep = NY_SEM_REP_POINTER;
      l->type_name = pointer_return_type;
      is_any = false;
      is_f64 = false;
      is_f32 = false;
    }
    if (l && i < v->exprs.len && v->exprs.data[i]) {
      /*
       * A canonical dynamic binding keeps the dynamic classification: the
       * `.long` builtin is declared bigint and every rt_long path returns a
       * heap-allocated TAG_BIGINT handle (never a tagged small int), which
       * the dynamic-value domain already models.  A bigint stamp here would
       * route integer compares through the bigint bridge against that
       * dynamic word instead.
       */
      bool bigint_literal =
          v->exprs.data[i]->kind == NY_E_LITERAL &&
          v->exprs.data[i]->as.literal.kind == NY_LIT_INT &&
          v->exprs.data[i]->tok.kind != NY_T_NIL &&
          (v->exprs.data[i]->as.literal.as.i >= (INT64_C(1) << 62) ||
           v->exprs.data[i]->as.literal.as.i <= -(INT64_C(1) << 62));
      bool bigint_alias = v->exprs.data[i]->kind == NY_E_IDENT &&
                          nyr_expr_is_bigint(b, v->exprs.data[i]);
      /* A wide integer literal is always materialized as a BigInt handle.
       * Unlike a general bigint-returning expression (which may return a
       * tagged small int), its physical class is known even if HM marks the
       * binding dynamic. A name bound to a proven BigInt is equally
       * representation-stable; preserve its class through aliases. */
      l->is_bigint = (!is_any || bigint_literal || bigint_alias) &&
                     nyr_expr_is_bigint(b, v->exprs.data[i]);
    }
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->semantic.resolved) {
      /* A mutable local owns one physical ABI for its lifetime.  Do not let
       * a later dynamic RHS rewrite an already-proven raw scalar slot before
       * that RHS is lowered: compound assignments such as
       * `acc += dict.get(k, 0)` would otherwise feed a raw accumulator into
       * rt_any_add, then store a tagged result back into a raw slot.  Dynamic
       * or as-yet-unknown locals may still refine normally. */
      if (!concrete_call_result &&
          ((v->is_decl && !nil_default_dynamic_get) || !prior_local ||
           prior_local->semantic_rep == NY_SEM_REP_UNRESOLVED))
        l->semantic_rep = v->exprs.data[i]->semantic.rep;
      l->semantic_ownership = v->exprs.data[i]->semantic.ownership;
      l->alias_class = v->exprs.data[i]->semantic.alias_class;
      l->semantic_mutable = v->exprs.data[i]->semantic.mutable_value;
    }
    if (l && nil_default_dynamic_get &&
        (!declared_type || ny_native_type_name_is_any(declared_type))) {
      /* HM can mark the missing-value branch numeric after a later float
       * assignment. The getter still yields a tagged value-or-nil word. */
      l->is_any = true;
      l->is_f64 = false;
      l->is_f32 = false;
      l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
      if (!l->type_name || !l->type_name[0])
        l->type_name = "any";
    }
    /* Preserve the independently proven physical class after copying the
     * expression's contextual semantic stamp.  In an `any`-returning helper,
     * HM may label an int ternary `any` solely from the enclosing return
     * context; its local slot and subsequent typed-parameter uses stay raw. */
    if (l && raw_scalar_ternary) {
      l->is_any = false;
      l->is_f64 = false;
      l->is_f32 = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
      if (!l->type_name || !l->type_name[0])
        l->type_name = "int";
    }
    if (l && init_operator_result_known && init_operator_result_type) {
      /* Keep the selected overload's declared result class after copying the
       * expression's contextual HM stamp above. */
      l->semantic_rep = nyr_rep_for_type(b, init_operator_result_type);
      l->type_name = init_operator_result_type;
      l->is_any = l->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC;
      l->is_f64 = l->semantic_rep == NY_SEM_REP_F64;
      l->is_f32 = l->semantic_rep == NY_SEM_REP_F32;
      l->is_dict = l->semantic_rep == NY_SEM_REP_AGGREGATE ||
                   l->semantic_rep == NY_SEM_REP_OBJECT;
    }
    /* The slot's physical ABI wins over a stale joined semantic rep.  HM
     * stamps f32-parameter arithmetic with the default floating rep (f64),
     * but the store below emits f32 (`movss`) because the operands' raw ABI
     * is f32.  Recording that f64 rep here made the next read classify the
     * local as f64 and load the f32 slot through `movsd`. */
    /*
     * Same handle-init rule as the is_f64 flip above: a checker guess of
     * f64 over a handle-returning call (Builder(...) in a comma binding)
     * stamped the local f64, the inlined handle store kept its own class,
     * and the post-inline union made every load of the builder an
     * f64-classified pointer.
     */
    if (l && is_f32 && !is_any && !handle_init)
      l->semantic_rep = NY_SEM_REP_F32;
    else if (l && is_f64 && !is_any && !handle_init)
      l->semantic_rep = NY_SEM_REP_F64;
    if (l && !canonical_dynamic_init &&
        (indexed_raw_scalar || raw_scalar_alias || raw_len_member)) {
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    /* Compatibility declarations describe bigint-to-int as dynamic, but
     * the native bridge returns a raw i64.  Preserve that physical ABI on an
     * inferred local, including values outside the tagged small-int range. */
    /*
     * A canonical dynamic initializer (a comptime block returning `.long`,
     * for example) speaks the tagged ABI by construction; the raw-bridge
     * overrides below reason about raw producers and must not reclassify
     * it.  Without this guard `rt_long`'s tagged result was stored into a
     * raw-int slot and integer compares matched the tag encoding.
     */
    if (l && !canonical_dynamic_init && i < v->exprs.len &&
        nyr_expr_is_raw_integer_conversion(b, v->exprs.data[i])) {
      l->is_any = false;
      l->is_bigint = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
      is_any = false;
    }
    if (l && !canonical_dynamic_init && raw_dynamic_read) {
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    /* Integer literals are emitted as raw NYIR scalars even when the source
     * declaration is unannotated and HM keeps a conservative dynamic rep.
     * Record the physical slot ABI explicitly so later compound arithmetic
     * can stay raw and normalize only the dynamic operand. */
    if (l && (!declared_type || ny_native_type_name_is_int(declared_type)) &&
        !l->is_bigint && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_LITERAL &&
        v->exprs.data[i]->as.literal.kind == NY_LIT_INT &&
        v->exprs.data[i]->tok.kind != NY_T_NIL) {
      l->is_any = false;
      is_f64 = false;
      is_f32 = false;
      l->is_f64 = false;
      l->is_f32 = false;
      l->scalar_class = NYIR_SCALAR_I64_RAW;
      l->scalar_class_mixed = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
      l->type_name = "int";
    }
    /* A primitive bitwise call with raw operands emits a raw NYIR word,
     * including nested bor/band/bxor calls at top level.  Its source-level
     * `any` annotation must not turn an inferred local into a tagged slot. */
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_any(v->types.data[i])) &&
        nyr_raw_bitwise_value(b, v->exprs.data[i], 0)) {
      is_any = false;
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    /* A sequence index is materialized as a raw slot payload.  Keep that
     * fact on an inferred local so an indirect dynamic callback can box the
     * scalar exactly at its call boundary, while ordinary list writes retain
     * the raw payload. */
    /* Primitive bitwise wrappers are semantically untyped, but a raw integer
     * function can accumulate their result in a mutable scalar. Preserve the
     * producer ABI on the local slot so the next read is not decoded as a
     * tagged value.  Shifts use the same raw adapter as &, |, and ^; leaving
     * them on the conservative any path makes an odd shift result get
     * unboxed twice when it is reused by a later bitwise expression. */
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_CALL && b->current_fn_name) {
      const char *bit_leaf = ny_native_call_leaf(v->exprs.data[i]);
      const stmt_t *owner = nyr_find_user_function(b, b->current_fn_name);
      bool raw_owner = owner && owner->as.fn.return_type &&
                       !ny_native_type_name_is_any(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_list(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f64(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f32(owner->as.fn.return_type);
      if (raw_owner && bit_leaf &&
          (strcmp(bit_leaf, "bor") == 0 || strcmp(bit_leaf, "band") == 0 ||
           strcmp(bit_leaf, "bxor") == 0 || strcmp(bit_leaf, "__or") == 0 ||
           strcmp(bit_leaf, "__and") == 0 || strcmp(bit_leaf, "__xor") == 0 ||
           strcmp(bit_leaf, "bshl") == 0 || strcmp(bit_leaf, "bshr") == 0 ||
           strcmp(bit_leaf, "__shl") == 0 || strcmp(bit_leaf, "__shr") == 0)) {
        is_any = false;
        l->is_any = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    /* The result of a composed mask/rotate expression has the same raw
     * scalar ABI as a primitive bitwise call.  Semantic inference often
     * labels the tree `any` because its leaves come from untyped sequences;
     * that label must not change the physical slot when the containing
     * function is explicitly scalar.  Otherwise the next arithmetic use
     * decodes the already-raw tree result a second time. */
    if (l && i < v->exprs.len && v->exprs.data[i] && b->current_fn_name) {
      const stmt_t *owner = nyr_find_user_function(b, b->current_fn_name);
      bool raw_owner = owner && owner->as.fn.return_type &&
                       !ny_native_type_name_is_any(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_list(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f64(owner->as.fn.return_type) &&
                       !ny_native_type_name_is_f32(owner->as.fn.return_type);
      if (raw_owner && nyr_expr_is_bitwise_tree(b, v->exprs.data[i], 0)) {
        is_any = false;
        l->is_any = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    /* Raw memory loads preserve machine words even when their language
     * declarations use any.  Retain that producer ABI through local slots
     * so offsets and scalar comparisons never decode the word again. */
    if (l && v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_CALL) {
      const char *load_leaf = ny_native_call_leaf(v->exprs.data[i]);
      ny_native_leaf_kind_t load_kind = ny_native_leaf_kind(load_leaf);
      if (load_kind == NY_NATIVE_LEAF_LOAD8 ||
          load_kind == NY_NATIVE_LEAF_LOAD16 ||
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
        v->exprs.data[i]->kind == NY_E_IDENT) {
      const expr_t *init = v->exprs.data[i];
      const nyr_local_t *source_local = nyr_find_expr_local(b, init);
      /* A lexical scalar (including a parameter's synthetic binding) can
       * shadow a library function. Its spelling must never turn the scalar
       * into a callable handle when it is stored in a dynamic container. */
      if (source_local && source_local != l) {
        l->callable_expr = source_local->callable_expr;
        l->callable_name = source_local->callable_name;
      } else if (!source_local &&
                 nyr_find_user_function(b, init->as.ident.name)) {
        l->callable_name = init->as.ident.name;
      }
    }
    if (l && !l->callable_name && i < v->exprs.len && v->exprs.data[i]) {
      const stmt_t *function =
          nyr_namespace_function_value(b, v->exprs.data[i]);
      if (function && function->as.fn.name)
        l->callable_name = function->as.fn.name;
    }
    /*
     * A resolved bigint return type is a heap-handle ABI contract.  Re-assert
     * it after the raw-bridge overrides above: an untyped local bound from
     * `bigint_abs(...)`/`Z(...)` keeps the handle in its slot, and the
     * raw-int classification clobbered by those overrides made the next
     * bitwise/arithmetic use mangle the handle.
     */
    if (l && !canonical_dynamic_init && i < v->exprs.len && v->exprs.data[i]) {
      /*
       * canonical_dynamic_init bindings keep their tagged-any slot: the
       * `.long` builtin is DECLARED bigint but returns a tagged small int
       * when the value fits, so a static bigint classification would send
       * integer compares through the bigint bridge against a tagged word.
       */
      const char *init_type = nyr_expr_type_name(b, v->exprs.data[i]);
      bool bigint_init = init_type && ny_type_is(init_type, "bigint");
      if (!bigint_init && v->exprs.data[i]->kind == NY_E_BINARY) {
        /*
         * The operator result's own type fact can be a stale RAW_INT stamp
         * (HM refines impl `self` results through float helpers), while
         * both operands are proven bigint handles.  The handle domain is
         * then the only sound classification for the slot.
         */
        bigint_init = nyr_expr_is_bigint(b, v->exprs.data[i]->as.binary.left) &&
                      nyr_expr_is_bigint(b, v->exprs.data[i]->as.binary.right);
      }
      if (bigint_init) {
        l->is_any = false;
        l->is_bigint = true;
        /*
         * Publish the handle ABI on the local so later type queries
         * (argument prep, operator dispatch, to_str) resolve it without
         * re-deriving from the initializer.
         */
        if (!l->type_name || !l->type_name[0])
          l->type_name = "bigint";
      }
    }
    /*
     * Symmetric f64 publication: an initializer computed in the f64 domain
     * (`__flt_sqrt(...)`, float arithmetic) leaves an f64-classified
     * register; without the stamp, the first str/any consumer promotes the
     * raw IEEE bits through i64.to.f64 and prints garbage.
     */
    bool binding_has_concrete_nonfloat_type =
        l && l->type_name && !ny_native_type_name_is_any(l->type_name) &&
        !ny_native_type_name_is_f64(l->type_name) &&
        !ny_native_type_name_is_f32(l->type_name) &&
        nyr_rep_for_type(b, l->type_name) != NY_SEM_REP_UNRESOLVED &&
        nyr_rep_for_type(b, l->type_name) != NY_SEM_REP_TAGGED_DYNAMIC;
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        !numeric_init_call(v->exprs.data[i]) && !prior_dynamic_slot &&
        !binding_has_concrete_nonfloat_type &&
        !(init_operator_result_known &&
          nyr_rep_for_type(b, init_operator_result_type) != NY_SEM_REP_F64 &&
          nyr_rep_for_type(b, init_operator_result_type) != NY_SEM_REP_F32) &&
        nyr_expr_is_f64(b, v->exprs.data[i])) {
      l->is_any = false;
      l->is_f64 = true;
      if (!l->type_name || !l->type_name[0])
        l->type_name = "f64";
    }
    if (l && ((i < v->types.len && v->types.data[i] &&
               strcmp(v->types.data[i], "dict") == 0) ||
              nyr_expr_is_dict(b, v->exprs.data[i])))
      l->is_dict = true;
    if (l && i < v->exprs.len && v->exprs.data[i] &&
        v->exprs.data[i]->kind == NY_E_CALL && !l->is_list && !l->is_cstr &&
        !l->is_f64 && !l->is_f32 && !l->is_any)
      l->is_dict = nyr_expr_is_dict(b, v->exprs.data[i]);
    nyr_local_t *source_list_local =
        v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_IDENT
            ? nyr_find_local(b, v->exprs.data[i]->as.ident.name)
            : NULL;
    /* A resolved call declaration is the physical result contract.  The
     * expression's HM carrier can be stale (notably `any`-shaped call sites
     * which retain a typed-buffer use fact); allowing that fallback to mark a
     * concrete scalar result as a list emits a bogus length query on the
     * scalar producer.  Keep the broad expression inference for unresolved
     * calls and non-call initializers only. */
    bool concrete_nonlist_call =
        concrete_call_result && init_call_return_type &&
        !ny_native_type_name_is_list(init_call_return_type);
    bool is_list =
        !concrete_nonlist_call &&
        ((i < v->types.len && ny_native_type_name_is_list(v->types.data[i])) ||
         nyr_expr_is_list(b, v->exprs.data[i]) ||
         (source_list_local && source_list_local->is_list) ||
         (!v->is_decl && prior_local && prior_local->is_list));
    if (init_call_return_type &&
        !ny_native_type_name_is_any(init_call_return_type) &&
        !ny_native_type_name_is_list(init_call_return_type) &&
        nyr_rep_for_type(b, init_call_return_type) != NY_SEM_REP_UNRESOLVED &&
        nyr_rep_for_type(b, init_call_return_type) !=
            NY_SEM_REP_TAGGED_DYNAMIC) {
      /* The lowered call result contract also owns the local's auxiliary
       * list-length state. Clear stale contextual list facts before storing
       * the scalar; otherwise store_local_value queries a length from an
       * f64/pointer result. */
      is_list = false;
      if (l) {
        l->is_list = false;
        l->is_dyn_list = false;
        l->list_len_slot = -1;
      }
    }
    if (l && is_list && source_list_local && source_list_local != l) {
      if (source_list_local->alias_count == 0)
        source_list_local->alias_count = 1;
      if (source_list_local->alias_count < UINT32_MAX)
        source_list_local->alias_count++;
      l->alias_count = source_list_local->alias_count;
    }
    if (!is_list && init_call_fn && init_call_fn->as.fn.return_type &&
        ny_native_type_name_is_list(init_call_fn->as.fn.return_type))
      is_list = true;
    bool proven_list_shape =
        (i < v->types.len && ny_native_type_name_is_list(v->types.data[i])) ||
        (v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_LIST) ||
        (source_list_local && source_list_local->is_list) ||
        (init_call_fn && init_call_fn->as.fn.return_type &&
         ny_native_type_name_is_list(init_call_fn->as.fn.return_type) &&
         !ny_native_type_name_is_dyn_list(init_call_fn->as.fn.return_type));
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
    if (l && is_list && !is_any)
      l->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
    if (l && is_list && proven_list_shape &&
        !(i < v->types.len && v->types.data[i] &&
          ny_native_type_name_is_any(v->types.data[i])))
      l->is_any = false;
    const char *inferred_init_type =
        (i < v->exprs.len && v->exprs.data[i])
            ? nyr_expr_type_name(b, v->exprs.data[i])
            : NULL;
    bool typed_scalar_list =
        prior_typed_scalar_list ||
        (i < v->types.len && v->types.data[i] &&
         ny_native_type_name_is_list(v->types.data[i]) &&
         !ny_native_type_name_is_dyn_list(v->types.data[i])) ||
        (init_call_fn && init_call_fn->as.fn.return_type &&
         ny_native_type_name_is_list(init_call_fn->as.fn.return_type) &&
         !ny_native_type_name_is_dyn_list(init_call_fn->as.fn.return_type)) ||
        (!v->is_mut && inferred_init_type &&
         ny_native_type_name_is_list(inferred_init_type) &&
         !ny_native_type_name_is_dyn_list(inferred_init_type));
    if (l && is_list) {
      l->is_dyn_list = prior_dyn_list ||
                       (!typed_scalar_list &&
                        ((i < v->types.len &&
                          ny_native_type_name_is_dyn_list(v->types.data[i])) ||
                         nyr_expr_is_dyn_list(b, v->exprs.data[i])));
      /* A list-shaped value returned by a dynamic call/member access has no
       * compile-time element stride. Treat it as descriptor-backed so later
       * `.get`/index reads decode raw elem-8 payloads exactly once. Explicit
       * `list<T>` declarations remain on the typed scalar path. */
      if (!typed_scalar_list && v->exprs.data[i] &&
          (v->exprs.data[i]->kind == NY_E_CALL ||
           v->exprs.data[i]->kind == NY_E_MEMCALL))
        l->is_dyn_list = true;
      if (v->is_mut && !typed_scalar_list && v->exprs.data[i] &&
          v->exprs.data[i]->kind == NY_E_LIST)
        l->is_dyn_list = true;
      l->list_literal =
          v->is_mut ? NULL : nyr_resolve_list_literal(b, v->exprs.data[i], 0);
      /* An empty-list declaration is the one shape whose element
       * representation is genuinely undetermined: the first append decides
       * it.  Record that start for mut locals too (their list_literal is
       * deliberately NULL) so a raw-int append can promote the local to
       * the elem-8 facts. */
      if (v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_LIST &&
          v->exprs.data[i]->as.list_like.len == 0)
        l->list_started_empty = true;
      /* `xs = append(xs, <proven-int>)` through the stdlib helper is the
       * common append-built int list shape.  The helper call lowers (and
       * later inlines) with the raw append ABI, so the first such
       * assignment settles the receiver's representation: promote it to
       * the elem-8 scalar facts here, before the statement's indexed
       * consumers lower.  Single-use: the empty-start flag is consumed,
       * and any tagged/dynamic append re-marks the list dynamic. */
      if (!v->is_decl && l->list_started_empty && l->is_dyn_list &&
          v->names.len == 1 && v->exprs.len == 1 && v->exprs.data[i] &&
          v->exprs.data[i]->kind == NY_E_CALL &&
          v->exprs.data[i]->as.call.args.len == 2 &&
          !v->exprs.data[i]->as.call.args.data[0].name &&
          !v->exprs.data[i]->as.call.args.data[1].name) {
        const expr_t *call = v->exprs.data[i];
        const char *leaf = ny_native_call_leaf(call);
        const expr_t *recv = call->as.call.args.data[0].val;
        const expr_t *item = call->as.call.args.data[1].val;
        if (leaf && strcmp(leaf, "append") == 0 && recv &&
            recv->kind == NY_E_IDENT && recv->as.ident.name &&
            strcmp(recv->as.ident.name, v->names.data[i]) == 0 && item &&
            item->kind != NY_E_CALL && item->kind != NY_E_MEMCALL &&
            item->kind != NY_E_LIST && !nyr_expr_is_cstr(b, item) &&
            !nyr_expr_is_f64(b, item) && !nyr_expr_is_f32(b, item) &&
            !nyr_expr_is_any(b, item)) {
          l->is_dyn_list = false;
          l->is_any = false;
          l->type_name = "list<int>";
          l->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
          l->list_started_empty = false;
        }
      }
    }
    if (l)
      l->dict_literal =
          v->is_mut ? NULL : nyr_resolve_dict_literal(b, v->exprs.data[i], 0);
    if (typed_scalar_list)
      l->list_literal = NULL;
    int64_t declared_fin_bound =
        i < v->types.len ? ny_native_resolve_fin_bound(b, v->types.data[i]) : 0;
    expr_t *init = v->exprs.data[i];
    int val = -1;
    bool init_arith_raw = false;
    int64_t saved_list_elem_size = b->current_list_elem_size;
    const char *decl_type = i < v->types.len ? v->types.data[i] : NULL;
    b->current_list_elem_size =
        is_list && decl_type && ny_native_type_name_is_list(decl_type) &&
                !ny_native_type_name_is_dyn_list(decl_type) &&
                v->exprs.data[i] && v->exprs.data[i]->kind == NY_E_LIST &&
                v->exprs.data[i]->as.list_like.len == 0
            ? 8
            : 0;
    if (pre_vals) {
      val = pre_vals[i];
      /*
       * A comma-bound scalar local stores the raw representation (`mut a, b =
       * 0, 0` then `a, b = pair.get(0), pair.get(1)`).  A dynamic right-hand
       * side (any-classified call such as a dict/list `.get`) yields the
       * tagged value; decode once so the raw slot holds the true scalar
       * (otherwise the bound name reads 23 where the source said 11).
       */
      if (val >= 0 && !is_any && v->exprs.data[i] &&
          (prior_raw_integer_slot || v->exprs.data[i]->kind == NY_E_CALL ||
           v->exprs.data[i]->kind == NY_E_MEMCALL) &&
          nyr_value_scalar_class(b, val) != NYIR_SCALAR_I64_RAW &&
          !nyr_expr_is_bigint(b, v->exprs.data[i]) &&
          !nyr_expr_is_f64(b, v->exprs.data[i]) &&
          !nyr_expr_is_f32(b, v->exprs.data[i]) &&
          !nyr_expr_is_cstr(b, v->exprs.data[i]) &&
          !nyr_expr_is_list(b, v->exprs.data[i]) &&
          !nyr_expr_is_dict(b, v->exprs.data[i]) &&
          !nyr_expr_is_bool(b, v->exprs.data[i])) {
        val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1, 0);
        if (val < 0)
          return false;
      }
    } else if (is_f64 && init && init->kind == NY_E_LITERAL &&
               init->as.literal.kind == NY_LIT_FLOAT)
      val = nyr_emit_const_f64(b, init->as.literal.as.f);
    else if (is_f32 && init && init->kind == NY_E_LITERAL &&
             init->as.literal.kind == NY_LIT_FLOAT)
      val = nyr_emit_const_f32(b, init->as.literal.as.f);
    else if (typed_scalar_list && init && init->kind == NY_E_CALL &&
             init->as.call.callee && init->as.call.callee->kind == NY_E_IDENT &&
             init->as.call.callee->as.ident.name &&
             (strcmp(init->as.call.callee->as.ident.name, "list") == 0 ||
              strcmp(init->as.call.callee->as.ident.name, "core.list") == 0) &&
             init->as.call.args.len == 1 && !init->as.call.args.data[0].name) {
      /* Preserve `list(n)`'s contract: n reserves capacity but does not add
       * logical elements. Indexed stores can materialize values through the
       * runtime setter when the destination is preallocated. */
      int arg = nyr_lower_expr(b, init->as.call.args.data[0].val);
      if (arg < 0)
        return false;
      val = nyr_emit_runtime_call(b, "rt_list_new", arg, -1, -1, 1, 0);
    } else if (is_f64 && init && init->kind == NY_E_TERNARY) {
      /* Normalize conditional arms before joining them into the declared
       * machine-f64 slot.  Lowering the ordinary ternary first would merge
       * tagged and raw representations into an ambiguous i64 word. */
      val = nyr_lower_f64_arg(b, init);
    } else {
      bool saved_index_for_any = b->index_for_any_call;
      if (is_any && init && init->kind == NY_E_INDEX)
        b->index_for_any_call = true;
      val = nyr_lower_expr(b, init);
      b->index_for_any_call = saved_index_for_any;
      /*
       * Representation fact captured before any other lowering runs: an
       * integer binary producer leaves a raw i64 even for a dynamic source
       * expression, and the any-slot boxing below must consult this instead
       * of re-deriving it syntactically.  The pointer match proves the fact
       * describes this initializer, not a nested or stale expression.
       */
      init_arith_raw = init && init->kind == NY_E_BINARY &&
                       b->last_arith_expr == init && b->last_arith_raw;
      bool init_arith_tagged = init && init->kind == NY_E_BINARY &&
                               b->last_arith_expr == init &&
                               b->last_arith_tagged;
      bool raw_integer_call_initializer =
          !v->is_decl && prior_local && prior_local->type_name &&
          ny_native_type_name_is_int(prior_local->type_name) && init &&
          init->kind == NY_E_CALL && nyr_expr_has_raw_int_abi(b, init, 0);
      bool inferred_raw_integer_initializer =
          v->is_decl && !declared_type && is_any && init &&
          init->kind == NY_E_CALL && nyr_expr_has_raw_int_abi(b, init, 0);
      if (raw_integer_call_initializer || inferred_raw_integer_initializer) {
        /* A typed raw slot remains raw when an inferred direct call returns a
         * proven raw integer. HM can conservatively stamp that call dynamic;
         * allowing the stale stamp to drive assignment boxing makes the next
         * loop iteration read a tagged word as a scalar. */
        is_any = false;
        is_f64 = false;
        is_f32 = false;
        if (l) {
          l->is_any = false;
          l->is_f64 = false;
          l->is_f32 = false;
          l->semantic_rep = NY_SEM_REP_RAW_INT;
          if (inferred_raw_integer_initializer)
            l->type_name = "int";
        }
      }
      if (init_arith_tagged &&
          !(i < v->types.len && v->types.data[i] && v->types.data[i][0])) {
        /* A dynamic operator's lowered result ABI is authoritative for an
         * inferred local.  In particular, `def v = dynamic_vec / 2.0` may
         * produce a vector handle even though the float literal made the
         * source expression look scalar before operator dispatch. */
        is_any = true;
        is_f64 = false;
        is_f32 = false;
        if (l) {
          l->is_any = true;
          l->is_f64 = false;
          l->is_f32 = false;
          l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
          if (!l->type_name || !l->type_name[0])
            l->type_name = "any";
        }
      }
      /*
       * The integer arithmetic emitter can prove a raw result even when HM
       * left an inferred binding at `any` (notably SHA-256's rotate/xor
       * temporaries).  Apply that producer fact before the dynamic-slot
       * boxing below: boxing the raw word here makes every later local read
       * take the tagged path and corrupts bitwise algorithms.  An explicitly
       * declared `any` still owns the dynamic ABI and is boxed as usual.
       */
      if (init_arith_raw && !init_arith_tagged && !declared_type && l) {
        is_any = false;
        is_f64 = false;
        is_f32 = false;
        l->is_any = false;
        l->is_f64 = false;
        l->is_f32 = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
        if (!l->type_name || !l->type_name[0] ||
            strcmp(l->type_name, "any") == 0)
          l->type_name = "int";
        if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "native-abi",
              "function=%s local=%s producer=i64-raw "
              "decision=keep-inferred-local-raw reason=binary-result-class",
              b->current_fn_name ? b->current_fn_name : "<entry>", name);
      }
      if (val >= 0) {
        if (raw_dynamic_read &&
            !(init && init->semantic.resolved &&
              init->semantic.rep == NY_SEM_REP_RAW_INT) &&
            !nyr_expr_is_f64(b, init) && !nyr_expr_is_f32(b, init) &&
            !nyr_expr_is_cstr(b, init) && !nyr_expr_is_bool(b, init)) {
          val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
          if (val < 0)
            return false;
          is_any = false;
        }
        /* A tagged index read stored into an inferred scalar local must be
         * decoded regardless of the source container's static/list-literal
         * appearance. The lowered value class is the storage ABI authority;
         * explicit `any` locals keep the tagged value and proven scalar-list
         * reads are already raw. */
        /* A top-level append-built elem-8 list was already read through the
         * raw accessor.  Its conservative dynamic-list fact must not decode
         * odd scalar payloads a second time when binding an alias. */
        if (val >= 0 && !is_any &&
            nyr_value_scalar_class(b, val) != NYIR_SCALAR_I64_RAW && init &&
            init->kind == NY_E_INDEX && !promoted_top_mut_index &&
            init->as.index.target &&
            !nyr_index_read_is_proven_scalar(b, init) && !is_f64 && !is_f32 &&
            !is_cstr && !is_bool && !nyr_expr_is_bytes(b, init) &&
            !nyr_expr_is_list(b, init) && !nyr_expr_is_dict(b, init)) {
          val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
          if (val < 0)
            return false;
          if (l) {
            l->is_any = false;
            l->semantic_rep = NY_SEM_REP_RAW_INT;
          }
        }
        /*
         * An untyped callee returns the tagged dynamic ABI, but inference can
         * refine this initializer (and therefore the receiving scalar local)
         * to a raw integer.  Decode exactly once at the store boundary so the
         * local's raw storage holds the true scalar (def r = square(7) then
         * keeps 49, not the tagged encoding 99).
         */
        if (!is_any && !bigint_result_overrides_inferred_type &&
            /* BigInt values are tagged handles even though several native
             * arithmetic paths consume their payload as i64.  The source
             * return contract of bigint_from_str is already tagged; decoding
             * it here stores a truncated payload in the BigInt local. */
            !(declared_type && ny_type_is(declared_type, "bigint")) &&
            !raw_bitwise_update && !nyr_raw_bitwise_value(b, init, 0) && init &&
            init->kind == NY_E_CALL) {
          const stmt_t *init_callee = init->semantic.canonical_callee_stmt;
          if ((!init_callee || init_callee->kind != NY_S_FUNC) &&
              init->as.call.callee &&
              init->as.call.callee->kind == NY_E_IDENT &&
              init->as.call.callee->as.ident.name) {
            /* A lambda-valued binding has no user function under its name;
             * resolve the synthetic function so its TAGGED_DYNAMIC return
             * fact drives the decode below (def r = add_one(41) must keep
             * 42, not the tagged encoding 85). */
            const nyr_local_t *callable_local =
                nyr_find_expr_local(b, init->as.call.callee);
            const expr_t *callable = NULL;
            if (callable_local) {
              if (callable_local->callable_name)
                init_callee =
                    nyr_find_user_function(b, callable_local->callable_name);
              callable = callable_local->callable_expr;
              if (!init_callee && callable) {
                ny_native_lambda_entry_t *lambda = ny_native_lambda_find(
                    ny_native_lambda_cache_for(b), callable);
                if (lambda && lambda->fn)
                  init_callee = lambda->fn;
              }
            } else {
              init_callee = nyr_find_user_function(
                  b, init->as.call.callee->as.ident.name);
              if (!init_callee) {
                const expr_t *global = nyr_find_top_level_value(
                    b, init->as.call.callee->as.ident.name);
                if (global &&
                    (global->kind == NY_E_LAMBDA || global->kind == NY_E_FN))
                  callable = global;
                ny_native_lambda_entry_t *lambda =
                    callable ? ny_native_lambda_find(
                                   ny_native_lambda_cache_for(b), callable)
                             : NULL;
                if (lambda && lambda->fn)
                  init_callee = lambda->fn;
              }
            }
          }
          if (!is_list && !is_f64 && !is_f32 && init_callee &&
              init_callee->kind == NY_S_FUNC &&
              init_callee->as.fn.return_semantic.resolved &&
              init_callee->as.fn.return_semantic.rep ==
                  NY_SEM_REP_TAGGED_DYNAMIC &&
              nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED &&
              /* The expression-level representation is authoritative when
               * lowering has already selected a raw bridge (for example
               * `to_int(any)`).  Decoding that result again changes 1 to 0,
               * and is not a valid ABI conversion.  The raw-scalar check
               * shares that authority: a call site can carry a stale RAW_INT
               * stamp from HM specialization while the callee's body returns
               * the tagged word, and only a genuinely raw producer may skip
               * the decode. */
              (!init->semantic.resolved ||
               init->semantic.rep != NY_SEM_REP_RAW_INT ||
               !nyr_expr_is_raw_scalar(b, init, 0)) &&
              !nyr_expr_is_raw_integer_conversion(b, init) &&
              !nyr_expr_is_raw_int(init) && !nyr_expr_is_f64(b, init) &&
              !nyr_expr_is_f32(b, init) && !nyr_expr_is_bigint(b, init) &&
              !nyr_expr_is_cstr(b, init) && !nyr_expr_is_list(b, init) &&
              !nyr_expr_is_dict(b, init) && !nyr_expr_is_bool(b, init)) {
            val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                        NYIR_SCALAR_I64_RAW);
            if (val < 0)
              return false;
          }
        }
        /*
         * Element reads (.get/.pop on a list, dict, or any receiver) return
         * the tagged dynamic payload.  A scalar local receiving one must
         * store the decoded value, otherwise `a = pair.get(0)` binds the
         * encoded word (23) where the source element is 11.
         */
        if (!is_any && val >= 0 && init && init->kind == NY_E_MEMCALL &&
            init->as.memcall.name &&
            (strcmp(init->as.memcall.name, "get") == 0 ||
             strcmp(init->as.memcall.name, "pop") == 0) &&
            nyr_value_scalar_class(b, val) != NYIR_SCALAR_I64_RAW &&
            !nyr_expr_is_f64(b, init) && !nyr_expr_is_f32(b, init) &&
            !nyr_expr_is_cstr(b, init) && !nyr_expr_is_bytes(b, init) &&
            !nyr_expr_is_range(b, init) && !nyr_expr_is_bool(b, init)) {
          val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
          if (val < 0)
            return false;
        }
        /* A typed tbuf `.get` returns a raw slot even when the destination is
         * explicitly `any`. Cross the dynamic boundary once here; otherwise
         * odd scalar payloads are mistaken for tagged values and nested
         * container/string elements become invalid handles. */
        if (is_any && val >= 0 && init && init->kind == NY_E_MEMCALL &&
            init->as.memcall.name &&
            (strcmp(init->as.memcall.name, "get") == 0 ||
             strcmp(init->as.memcall.name, "pop") == 0) &&
            nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_RAW &&
            !nyr_expr_is_f64(b, init) && !nyr_expr_is_f32(b, init) &&
            !nyr_expr_is_cstr(b, init) && !nyr_expr_is_bytes(b, init) &&
            !nyr_expr_is_bool(b, init)) {
          val = nyr_emit_runtime_call(b, "rt_tag", val, -1, -1, 1, 0);
          if (val < 0)
            return false;
        }
        /*
         * Subscript reads on a dictionary keep the tagged dynamic slot word
         * (mirroring .get).  A scalar local receiving one decodes exactly
         * here, or `def int n = d["k"]` stores the encoded word and later
         * raw arithmetic doubles it.
         */
        if (!is_any && val >= 0 && init && init->kind == NY_E_INDEX &&
            init->as.index.target &&
            (nyr_expr_is_dict(b, init->as.index.target) ||
             (init->as.index.target->kind == NY_E_IDENT &&
                      init->as.index.target->as.ident.name &&
                      nyr_find_local(b, init->as.index.target->as.ident.name)
                  ? nyr_find_local(b, init->as.index.target->as.ident.name)
                        ->is_dict
                  : false)) &&
            !nyr_expr_is_f64(b, init) && !nyr_expr_is_f32(b, init) &&
            !nyr_expr_is_cstr(b, init) && !nyr_expr_is_bool(b, init)) {
          val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
          if (val < 0)
            return false;
        }
        /*
         * Coerce an initializer whose expression type differs from the
         * declared scalar type.  `def f64 y = <f32 expr>` widens via
         * f32->f64; `def f32 y = <f64 expr>` narrows via f64->f32.  A float
         * literal bound to f32 was already emitted as a F32 constant above.
         */
        bool init_is_f64 = nyr_expr_is_f64(b, init);
        bool init_is_f32 = nyr_expr_is_f32(b, init);
        if (is_f64 && init && init->kind == NY_E_LITERAL &&
            init->as.literal.kind == NY_LIT_FLOAT) {
          init_is_f64 = true;
          init_is_f32 = false;
        }
        /*
         * Whole-number float literals lower as integer constants.  Force
         * numeric conversion for these; otherwise the raw int word is
         * stored directly into the f64 slot.
         */
        bool init_whole_float =
            init && init->kind == NY_E_LITERAL &&
            init->as.literal.kind == NY_LIT_FLOAT &&
            init->as.literal.as.f == (double)(int64_t)init->as.literal.as.f;
        if (init_whole_float) {
          init_is_f64 = false;
          init_is_f32 = false;
        }
        /*
         * Operator dispatch is the representation authority for overloaded
         * arithmetic.  When the selected operator declares an f64 return,
         * the lowered call already yields machine f64; converting it as i64
         * would reinterpret the IEEE bits as an integer.
         */
        if (!init_is_f64 && !init_is_f32 && init && init->kind == NY_E_BINARY &&
            init->as.binary.op) {
          const stmt_t *init_op = nyr_find_operator(b, init);
          if (init_op && init_op->as.oper.return_type &&
              ny_native_type_name_is_f64(init_op->as.oper.return_type))
            init_is_f64 = true;
        }
        if (is_f64 && init_is_f32 && !init_is_f64)
          val = nyr_emit_f32_to_f64(b, val);
        else if (is_f64 && !init_is_f64 && !init_is_f32) {
          /*
           * A declared f64 local owns the machine-f64 ABI — but only a
           * NUMERIC initializer may be converted into it.  A handle-valued
           * call (`Builder(count*4+8)` in a comma binding) decoded through
           * rt_any_to_f64 stored garbage in the local, and every later
           * load of the builder read an f64-classified pointer
           * (float-cmp-oracle / linpack builder corruption).
           */
          bool numeric_init =
              init && (init->kind == NY_E_LITERAL ||
                       init->kind == NY_E_BINARY || init->kind == NY_E_UNARY ||
                       (init->kind == NY_E_CALL && init->semantic.resolved &&
                        init->semantic.rep == NY_SEM_REP_F64));
          if (nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED)
            /*
             * The emitted producer stayed in the canonical tagged domain
             * (the rt_any_mul bridge and siblings): decode the dynamic at
             * this boundary instead of SIToFP-converting the handle word.
             */
            val = nyr_emit_runtime_call(b, "rt_any_to_f64", val, -1, -1, 1,
                                        NYIR_SCALAR_F64);
          else if (nyr_expr_is_any(b, init) && numeric_init)
            val = nyr_emit_runtime_call(b, "rt_any_to_f64", val, -1, -1, 1,
                                        NYIR_SCALAR_F64);
          else if (numeric_init || !init)
            val = nyr_emit_i64_to_f64(b, val);
          /* Non-numeric init (handle-returning call): store the word
           * as-is; register classes carry the representation to consumers. */
        } else if (is_f32 && init_is_f64 && !init_is_f32)
          val = nyr_emit_f64_to_f32(b, val);
      }
    }
    b->current_list_elem_size = saved_list_elem_size;
    if (val < 0)
      return false;
    /*
     * Inferred mutable scalar slots retain their raw-i64 ABI after declaration.
     * A later read can still arrive as a tagged integer when source facts are
     * conservative; decode at the assignment boundary instead of storing the
     * tagged word into the established raw slot.
     */
    if (!v->is_decl && prior_raw_integer_slot && !is_any &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED && init &&
        !nyr_expr_is_bigint(b, init) && !nyr_expr_is_f64(b, init) &&
        !nyr_expr_is_f32(b, init) && !nyr_expr_is_cstr(b, init) &&
        !nyr_expr_is_bytes(b, init) && !nyr_expr_is_list(b, init) &&
        !nyr_expr_is_dict(b, init) && !nyr_expr_is_tuple(b, init) &&
        !nyr_expr_is_range(b, init) && !nyr_expr_is_bool(b, init)) {
      val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                  NYIR_SCALAR_I64_RAW);
      if (val < 0)
        return false;
    }
    /* Typed managed-handle locals can be initialized from an `any` branch
     * of a conditional. Decode tagged immediates while preserving handles,
     * matching the conversion already used for these formal carriers at
     * call boundaries. */
    ny_sem_rep_t declared_rep = declared_type
                                    ? nyr_rep_for_type(b, declared_type)
                                    : NY_SEM_REP_UNRESOLVED;
    if (declared_type &&
        (declared_rep == NY_SEM_REP_OBJECT ||
         declared_rep == NY_SEM_REP_AGGREGATE ||
         declared_rep == NY_SEM_REP_TYPED_BUFFER ||
         declared_rep == NY_SEM_REP_POINTER ||
         declared_rep == NY_SEM_REP_CLOSURE) &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED &&
        !(declared_rep == NY_SEM_REP_TYPED_BUFFER &&
          nyr_peek_list_len_fact(b, val) >= 0)) {
      nyr_scalar_class_t target_class =
          ny_native_scalar_class_from_sem_rep(declared_rep);
      val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                  target_class);
      if (val < 0)
        return false;
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s action=decode-tagged-handle target=%s "
            "reason=typed-local-initializer",
            b->current_fn_name ? b->current_fn_name : "<entry>", name,
            nyr_scalar_class_name(target_class));
    }
    /* Inference can give an unannotated local a list-handle ABI from its
     * fallback (`def xs = record.get("items", [])`) even though the dynamic
     * lookup itself returns a tagged handle. Normalize that handle at the
     * local boundary, just as an explicitly declared `list<T>` does; dynamic
     * element layout does not change the physical list-handle carrier. */
    if (!declared_type && is_list && l && l->is_list &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED &&
        nyr_peek_list_len_fact(b, val) < 0) {
      val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                  NYIR_SCALAR_TYPED_BUFFER);
      if (val < 0)
        return false;
    }
    /* A dictionary mutation can return through the dynamic runtime ABI even
     * when its receiver/result establishes an inferred dictionary local.
     * Publish the handle class at the store, not just on later loads: otherwise
     * store-class resolution restores TAGGED after call lowering has already
     * selected the object formal. This is a handle refinement, not integer
     * decoding; the dictionary pointer's bits remain unchanged. */
    if (!declared_type && l && l->is_dict && !is_any &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED) {
      val = nyr_ir_emit(&b->nyir,
                        (nyr_inst_t){.op = NYIR_COPY,
                                     .dst = -1,
                                     .a = val,
                                     .b = -1,
                                     .scalar_class = NYIR_SCALAR_OBJECT});
      if (val < 0)
        return false;
    }
    /* A nominal runtime object can flow through a conditional PHI whose
     * aggregate arm gives the joined temporary the broader aggregate class.
     * The typed declaration is the representation boundary: retain the same
     * machine word while publishing the concrete object carrier to its slot. */
    if (declared_rep == NY_SEM_REP_OBJECT && val >= 0 &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_AGGREGATE) {
      val = nyr_ir_emit(&b->nyir,
                        (nyr_inst_t){.op = NYIR_COPY,
                                     .dst = -1,
                                     .a = val,
                                     .b = -1,
                                     .scalar_class = NYIR_SCALAR_OBJECT});
      if (val < 0)
        return false;
    }
    /* A pointer-typed local can be initialized by a low-level load whose
     * producer is still stamped as raw i64 (for example envp's indexed
     * pointer load). Preserve the declared pointer carrier in SSA so passing
     * it to a dynamic `any` formal is recognized as a handle-word move rather
     * than rejected as an implicit integer boxing conversion. */
    if (declared_rep == NY_SEM_REP_POINTER && val >= 0 &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_RAW) {
      val = nyr_ir_emit(&b->nyir,
                        (nyr_inst_t){.op = NYIR_COPY,
                                     .dst = -1,
                                     .a = val,
                                     .b = -1,
                                     .scalar_class = NYIR_SCALAR_POINTER});
      if (val < 0)
        return false;
    }
    /* Explicit raw integer locals are an ABI boundary. Calls to helpers with
     * dynamic/legacy declarations and arithmetic on `any` fields can still
     * produce a tagged word even when the destination is declared `int`.
     * Decode based on the emitted value class so the local and later raw
     * integer formals agree. */
    if (!is_any && declared_type && ny_native_type_name_is_int(declared_type) &&
        !ny_type_is(declared_type, "bigint") && !nyr_expr_is_bigint(b, init) &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED) {
      val = nyr_emit_runtime_call(b, "rt_any_to_i64", val, -1, -1, 1,
                                  NYIR_SCALAR_I64_RAW);
      if (val < 0)
        return false;
      if (l)
        l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    /* An `any` local owns the canonical dynamic representation. The actual
     * NYIR producer class is authoritative here: an inline intrinsic may have
     * a public `any` type while its native lowering produces a raw float.
     * Boxing based only on the AST annotation misses that case and leaves
     * IEEE bits in a slot that later arithmetic treats as a tagged value. */
    nyr_scalar_class_t init_scalar_class = nyr_value_scalar_class(b, val);
    /* An `any` destination owns the tagged-word slot ABI even when HM
     * propagated an f64 fact from a dynamic accessor's fallback argument.
     * The emitted producer class below decides whether boxing is needed. */
    if (declared_type && ny_native_type_name_is_any(declared_type)) {
      is_any = true;
      is_f64 = false;
      is_f32 = false;
      if (l) {
        l->is_any = true;
        l->is_f64 = false;
        l->is_f32 = false;
        l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
      }
    }
    /*
     * An inferred scalar's physical storage follows its lowered producer.
     * Contextual HM facts can classify an integer-returning helper such as
     * min(3, xs.len) as f64; keeping that fact makes later int arguments
     * decode a raw i64 local as though it held IEEE bits. Declared and
     * explicitly dynamic slots retain their source-level contracts.
     */
    if (!declared_type && !is_any && !is_bool && !is_cstr && !is_list && l &&
        !l->is_dict && !l->is_bigint &&
        (v->is_decl || prior_raw_integer_slot) &&
        init_scalar_class == NYIR_SCALAR_I64_RAW) {
      is_f64 = false;
      is_f32 = false;
      l->is_any = false;
      l->is_f64 = false;
      l->is_f32 = false;
      l->scalar_class = NYIR_SCALAR_I64_RAW;
      l->scalar_class_mixed = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
      if (!l->type_name || !l->type_name[0] ||
          ny_native_type_name_is_f64(l->type_name) ||
          ny_native_type_name_is_f32(l->type_name))
        l->type_name = "int";
    }
    /* An inferred binding follows a concrete handle producer just as it
     * follows a concrete floating producer.  Keeping __malloc, dict, or
     * buffer results in a generic `any` local loses the exact ABI fact and
     * makes later loads falsely tagged; dynamic producers remain tagged.
     * Clear the fallback `any` spelling too: store and argument classification
     * consult that name as well as the representation flag. */
    if (!declared_type && l &&
        (init_scalar_class == NYIR_SCALAR_STRING ||
         init_scalar_class == NYIR_SCALAR_TYPED_BUFFER ||
         init_scalar_class == NYIR_SCALAR_OBJECT ||
         init_scalar_class == NYIR_SCALAR_POINTER ||
         init_scalar_class == NYIR_SCALAR_CLOSURE ||
         init_scalar_class == NYIR_SCALAR_AGGREGATE)) {
      l->is_any = false;
      if (ny_native_type_name_is_any(l->type_name))
        l->type_name = NULL;
      l->semantic_rep =
          init_scalar_class == NYIR_SCALAR_STRING ? NY_SEM_REP_STRING
          : init_scalar_class == NYIR_SCALAR_TYPED_BUFFER
              ? NY_SEM_REP_TYPED_BUFFER
          : init_scalar_class == NYIR_SCALAR_OBJECT  ? NY_SEM_REP_OBJECT
          : init_scalar_class == NYIR_SCALAR_POINTER ? NY_SEM_REP_POINTER
          : init_scalar_class == NYIR_SCALAR_CLOSURE ? NY_SEM_REP_CLOSURE
                                                     : NY_SEM_REP_AGGREGATE;
      is_any = false;
    }
    bool init_is_machine_float = init_scalar_class == NYIR_SCALAR_F64 ||
                                 init_scalar_class == NYIR_SCALAR_F32;
    bool init_returns_f64 =
        ny_native_type_name_is_f64(init_call_return_type) ||
        (init_call_fn && init_call_fn->kind == NY_S_FUNC &&
         ((init_call_fn->as.fn.return_semantic.resolved &&
           init_call_fn->as.fn.return_semantic.rep == NY_SEM_REP_F64 &&
           ny_sem_rep_coherent_with_type(init_call_fn->as.fn.return_type,
                                         NY_SEM_REP_F64)) ||
          ny_native_type_name_is_f64(init_call_fn->as.fn.return_type)));
    bool init_returns_f32 =
        ny_native_type_name_is_f32(init_call_return_type) ||
        (init_call_fn && init_call_fn->kind == NY_S_FUNC &&
         ((init_call_fn->as.fn.return_semantic.resolved &&
           init_call_fn->as.fn.return_semantic.rep == NY_SEM_REP_F32 &&
           ny_sem_rep_coherent_with_type(init_call_fn->as.fn.return_type,
                                         NY_SEM_REP_F32)) ||
          ny_native_type_name_is_f32(init_call_fn->as.fn.return_type)));
    if (!declared_type && init_scalar_class == NYIR_SCALAR_I64_RAW) {
      /* Overloaded scalar helpers can have a conservative float HM return
       * fact even when lowering selected their raw-integer implementation.
       * Do not let that source-level fallback overwrite the producer ABI. */
      init_returns_f64 = false;
      init_returns_f32 = false;
    }
    bool init_is_known_float =
        init_is_machine_float || init_returns_f64 || init_returns_f32;
    /* The emitted producer class is the final authority for a local's
     * physical representation. Stale HM/list facts can otherwise make an
     * inferred f32/f64 call result look like a typed buffer and cause the
     * list-length companion slot to query the floating value as a pointer. */
    if (init_is_known_float && !prior_dynamic_slot) {
      bool result_is_f64 =
          init_returns_f64 ||
          (!init_returns_f32 && init_scalar_class == NYIR_SCALAR_F64);
      is_list = false;
      if (l) {
        l->type_name = result_is_f64 ? "f64" : "f32";
        l->is_any = false;
        l->is_f64 = result_is_f64;
        l->is_f32 = !result_is_f64;
        l->semantic_rep = result_is_f64 ? NY_SEM_REP_F64 : NY_SEM_REP_F32;
        l->is_list = false;
        l->is_dyn_list = false;
        l->list_len_slot = -1;
      }
    }
    /*
     * A machine-float slot contract must receive machine-float bits. When
     * the operator lowering kept a dynamic operand in the canonical tagged
     * domain (the rt_any_mul bridge and siblings), decode at this boundary
     * instead of storing the tagged handle word under the f64/f32 stamp.
     */
    if (init_is_known_float && init_scalar_class == NYIR_SCALAR_I64_TAGGED &&
        val >= 0) {
      int as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", val, -1, -1, 1,
                                         NYIR_SCALAR_F64);
      if (as_f64 < 0)
        return false;
      bool slot_wants_f32 = init_returns_f32 && !init_returns_f64;
      val = slot_wants_f32 ? nyr_emit_f64_to_f32(b, as_f64) : as_f64;
      if (val < 0)
        return false;
      init_scalar_class = slot_wants_f32 ? NYIR_SCALAR_F32 : NYIR_SCALAR_F64;
    }
    bool init_is_dynamic_carrier =
        init_scalar_class == NYIR_SCALAR_I64_TAGGED ||
        init_scalar_class == NYIR_SCALAR_STRING ||
        init_scalar_class == NYIR_SCALAR_TYPED_BUFFER ||
        init_scalar_class == NYIR_SCALAR_OBJECT ||
        init_scalar_class == NYIR_SCALAR_POINTER ||
        init_scalar_class == NYIR_SCALAR_CLOSURE ||
        init_scalar_class == NYIR_SCALAR_AGGREGATE;
    /* An unannotated binding follows the emitted producer ABI when semantic
     * inference retained a broader `any`/unknown result type. The later local
     * loads and arithmetic dispatch must consume the same scalar class. */
    if (!declared_type && !is_any && init_is_machine_float) {
      is_f64 = init_scalar_class == NYIR_SCALAR_F64;
      is_f32 = init_scalar_class == NYIR_SCALAR_F32;
      if (l) {
        l->is_f64 = is_f64;
        l->is_f32 = is_f32;
        l->semantic_rep = is_f64 ? NY_SEM_REP_F64 : NY_SEM_REP_F32;
      }
    }
    if (is_any && init && !init_is_dynamic_carrier &&
        ((!canonical_dynamic_init && nyr_expr_is_f64(b, init)) ||
         init_is_machine_float)) {
      int as_f64 = init_scalar_class == NYIR_SCALAR_F32
                       ? nyr_emit_f32_to_f64(b, val)
                       : val;
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      val = bits < 0 ? -1
                     : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                             1, 0);
      if (val < 0)
        return false;
    }
    /* An explicitly dynamic local owns the tagged value ABI. Native scalar
     * expressions stay raw during evaluation, so normalize them at the `any`
     * boundary: integers with `rt_tag`, booleans with `nyr_box_bool`. Without
     * this boundary, `def any x = 42` stores 42 instead of tagged 85, while
     * raw false is confused with the nil/zero representation. */
    bool init_is_bool = init && nyr_expr_is_bool(b, init);
    if (is_any && init && val >= 0 &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_RAW && init_is_bool) {
      val = nyr_box_bool(b, val);
      if (val < 0)
        return false;
    }
    if (is_any && init && val >= 0 &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_RAW &&
        !init_is_bool &&
        ((!canonical_dynamic_init && nyr_expr_is_raw_int(init)) ||
         nyr_raw_bitwise_value(b, init, 0))) {
      val = nyr_emit_runtime_call(b, "rt_tag", val, -1, -1, 1, 0);
      if (val < 0)
        return false;
    }
    /* Dynamic arithmetic is the same story with the producer's own fact:
     * typing stamps `t1 - t0` TAGGED_DYNAMIC while the integer emitter keeps
     * the raw payload, and the predicates above may classify the node as
     * canonical dynamic, so the raw word used to reach the any slot
     * unboxed (printed as <ptr>, retyped "unknown").  The emitter fact is
     * authoritative for binary nodes: box exactly once when it says raw. */
    if (is_any && init_arith_raw && val >= 0 &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_RAW &&
        !nyr_expr_is_raw_int(init)) {
      val = nyr_emit_runtime_call(b, "rt_tag", val, -1, -1, 1, 0);
      if (val < 0)
        return false;
    }
    /*
     * An explicit nominal declaration owns its slot ABI even when the
     * initializer is dynamically typed. Keep the declared representation on
     * the local so later loads cannot inherit a contextual scalar guess.
     */
    if (l && declared_type && !ny_native_type_name_is_any(declared_type) &&
        (declared_rep == NY_SEM_REP_OBJECT ||
         declared_rep == NY_SEM_REP_AGGREGATE) &&
        strcmp(declared_type, "set") != 0) {
      l->type_name = declared_type;
      l->semantic_rep = declared_rep;
      l->is_any = false;
      l->is_f64 = false;
      l->is_f32 = false;
      is_f64 = false;
      is_f32 = false;
      l->is_dict = declared_rep == NY_SEM_REP_OBJECT ||
                   strcmp(declared_type, "dict") == 0 ||
                   ny_type_vector_dimension(declared_type) > 0;
      l->is_bigint = false;
      is_any = false;
    }
    /* The integer emitter may resolve a dynamically typed binary expression
     * to a raw i64 even when typing left the AST fact TAGGED_DYNAMIC or
     * UNKNOWN. Preserve that physical representation on inferred scalar
     * locals so later calls and global stores make the same boxing decision
     * as the initializer. Explicit any locals were boxed above and retain
     * their tagged ABI. */
    if (l && !is_any && init_arith_raw) {
      l->is_any = false;
      l->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    /* A comptime numeric operation may preserve a BigInt object even when
     * the receiving declaration explicitly owns an `int` slot.  Convert by
     * representation, not through the tagged-int decoder (which would halve
     * an odd raw i64), so small bigint results remain ordinary native ints. */
    if (!is_any && !is_f64 && !is_f32 && i < v->types.len && v->types.data[i] &&
        ny_native_type_name_is_int(v->types.data[i]) && init &&
        nyr_expr_is_bigint(b, init)) {
      /*
       * Some BigInt expressions cross the dynamic-to-scalar boundary while
       * lowering (for example a comptime block around `"ABC".long`).  The
       * producer may already be a raw call result, constant, or raw scalar
       * expression; feeding it to rt_bigint_to_i64_raw again interprets odd
       * payloads as tagged immediates and halves them.
       */
      if (nyr_value_scalar_class(b, val) != NYIR_SCALAR_I64_RAW &&
          !nyr_value_is_untagged_i64_call(b, val) &&
          !nyr_expr_is_raw_scalar(b, init, 0)) {
        val =
            nyr_emit_runtime_call(b, "rt_bigint_to_i64_raw", val, -1, -1, 1, 0);
        if (val < 0)
          return false;
      }
      if (l) {
        l->is_any = false;
        l->is_bigint = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    if (is_any && init && init->kind == NY_E_INDEX && init->as.index.target &&
        init->as.index.target->kind == NY_E_IDENT &&
        init->as.index.target->as.ident.name) {
      nyr_local_t *index_local =
          nyr_find_local(b, init->as.index.target->as.ident.name);
      if (index_local && (index_local->is_dyn_list ||
                          (index_local->type_name &&
                           strcmp(index_local->type_name, "seq") == 0)))
        l->raw_dynamic_index = true;
    }
    /* The canonical sequence bridge yields the dynamic value/len/tag word
     * even when its use-site semantic refines to RAW_INT.  Mark such defs so
     * callback arguments seed a real length and skip the second tag. */
    if (is_any && init && init->kind == NY_E_CALL && init->as.call.callee &&
        init->as.call.callee->as.ident.name) {
      const char *init_sym = ny_native_runtime_symbol_for_expr(
          init->as.call.callee->as.ident.name, NULL, init);
      if (init_sym && strcmp(init_sym, "rt_tbuf_index_any_raw") == 0)
        l->raw_dynamic_index = true;
    }
    /* A C-string local can receive a tagged result from a dynamic operator
     * even when HM labels the expression as `str` (for example, joining a
     * string loaded from an `any` list). Preserve the local's raw string ABI
     * based on the emitted carrier, not only the AST annotation. */
    if (l->is_cstr && init && val >= 0 &&
        nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s value_class=i64-tagged "
            "decision=decode-into-cstr-slot",
            b->current_fn_name ? b->current_fn_name : "<entry>", name);
      val = nyr_emit_runtime_call(b, "rt_any_to_cstr", val, -1, -1, 1, 0);
      if (val < 0)
        return false;
    }
    /* A declared integer local has a raw-i64 storage contract. Earlier
     * semantic joins may have marked its slot dynamic because the initializer
     * came from an any-returning helper; after the conversion above, restore
     * the declared class before recording the store and its later loads. */
    if (declared_type && ny_native_type_name_is_int(declared_type)) {
      is_any = false;
      if (l) {
        l->is_any = false;
        l->is_f64 = false;
        l->is_f32 = false;
        l->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    /* Explicit float declarations own the local's storage ABI. In
     * particular, an f32-returning call widened for `def f64 x = ...` must
     * leave the slot classified as f64 after the semantic-expression joins
     * above. Otherwise LOAD_LOCAL inherits the f32 class from the producer
     * and later ABI checks reject a valid f64 argument/return. */
    if (declared_type && ny_native_type_name_is_f64(declared_type)) {
      is_any = false;
      is_f64 = true;
      is_f32 = false;
      if (l) {
        l->type_name = declared_type;
        l->is_any = false;
        l->is_f64 = true;
        l->is_f32 = false;
        l->semantic_rep = NY_SEM_REP_F64;
      }
    } else if (declared_type && ny_native_type_name_is_f32(declared_type)) {
      is_any = false;
      is_f64 = false;
      is_f32 = true;
      if (l) {
        l->type_name = declared_type;
        l->is_any = false;
        l->is_f64 = false;
        l->is_f32 = true;
        l->semantic_rep = NY_SEM_REP_F32;
      }
    }
    /* Later expression-semantic propagation can restore HM's stale numeric
     * rep after the call-return decision above. Reassert the physical tagged
     * BigInt ABI at the point where this initializer is committed to its
     * local slot. */
    if (l && !v->is_decl && prior_bigint_slot && !declared_type) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s previous_slot=bigint "
            "decision=preserve-mutable-bigint-slot",
            b->current_fn_name ? b->current_fn_name : "<entry>", name);
      l->type_name = "bigint";
      l->is_any = false;
      l->is_f64 = false;
      l->is_f32 = false;
      l->is_cstr = false;
      l->is_bigint = true;
      l->is_dict = false;
      l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
    }
    if (l && bigint_result_overrides_inferred_type) {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "native-abi",
            "function=%s local=%s inferred=%s call_return=bigint "
            "decision=preserve-tagged-bigint-result",
            b->current_fn_name ? b->current_fn_name : "<entry>", name,
            declared_type ? declared_type : "<none>");
      l->type_name = "bigint";
      l->is_any = false;
      l->is_f64 = false;
      l->is_f32 = false;
      l->is_cstr = false;
      l->is_bigint = true;
      l->is_dict = false;
      l->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
    }
    if ((indexed_raw_scalar || raw_scalar_alias || raw_len_member) &&
        val >= 0) {
      /* The expression's cached semantic scalar class may have been stamped
       * tagged before the list-layout proof was available.  Carry the proved
       * physical ABI on the producer itself before STORE_LOCAL records its
       * reaching class; otherwise a later tagged boundary can elide boxing. */
      for (size_t def = b->nyir.len; def > 0; --def) {
        nyr_inst_t *producer = &b->nyir.data[def - 1];
        if (producer->dst == val) {
          producer->scalar_class = NYIR_SCALAR_I64_RAW;
          break;
        }
      }
    }
    if (!nyr_store_local_value(b, l->slot, val))
      return false;
    /* A local list keeps its current logical length in a companion slot for
     * bounds checks and for the extra list argument passed across native
     * calls.  Initializing a list from a call result creates a fresh local
     * slot, so the callee's length fact cannot be assumed to accompany the
     * returned pointer.  Seed it from a known fact when available, otherwise
     * read the length from the actual sequence value. */
    if (l->is_list && l->list_len_slot >= 0) {
      int list_len = nyr_peek_list_len_fact(b, val);
      /* `append(xs, value)` mutates xs and the builtin call lowering updates
       * xs' companion length slot from the same proven post-append value.
       * Reuse that value when the assignment simply writes the returned
       * handle back to the same local. This avoids a redundant runtime length
       * query without carrying facts through arbitrary aliases or calls. */
      bool canonical_stdlib_append =
          init && init->kind == NY_E_CALL &&
          init->semantic.canonical_callee_stmt &&
          init->semantic.canonical_callee_stmt->kind == NY_S_FUNC &&
          ny_is_stdlib_tok(init->semantic.canonical_callee_stmt->tok);
      if (list_len < 0 && init && init->kind == NY_E_CALL &&
          init->as.call.callee && init->as.call.callee->kind == NY_E_IDENT &&
          init->as.call.callee->as.ident.name &&
          (!nyr_user_defined_fn(b, init->as.call.callee->as.ident.name) ||
           canonical_stdlib_append) &&
          (strcmp(init->as.call.callee->as.ident.name, "append") == 0 ||
           strcmp(init->as.call.callee->as.ident.name, "core.append") == 0) &&
          init->as.call.args.len == 2 && !init->as.call.args.data[0].name &&
          !init->as.call.args.data[1].name && init->as.call.args.data[0].val &&
          init->as.call.args.data[0].val->kind == NY_E_IDENT &&
          init->as.call.args.data[0].val->as.ident.name &&
          strcmp(init->as.call.args.data[0].val->as.ident.name, name) == 0)
        list_len = nyr_load_local_value(b, l->list_len_slot);
      if (list_len < 0) {
        /* A list-returning dynamic helper can be tagged at this point even
         * though the destination local is statically a list. rt_len accepts
         * either representation; rt_tbuf_len_raw is the raw-handle ABI. */
        const char *len_fn =
            nyr_value_scalar_class(b, val) == NYIR_SCALAR_I64_TAGGED
                ? "rt_len"
                : "rt_tbuf_len_raw";
        list_len = nyr_emit_runtime_call(b, len_fn, val, -1, -1, 1, 0);
      }
      if (list_len < 0 ||
          !nyr_store_local_value(b, l->list_len_slot, list_len) ||
          !nyr_record_list_len_fact(b, val, list_len))
        return false;
    }
    /*
     * A candidate has one owner: a mutable declaration from a string literal.
     * Any subsequent ordinary identifier read clears this bit.
     */
    l->sb_candidate = v->is_decl && v->is_mut && init &&
                      init->kind == NY_E_LITERAL &&
                      init->as.literal.kind == NY_LIT_STR;
    l->static_string_literal = v->is_decl && !v->is_mut && init &&
                                       init->kind == NY_E_LITERAL &&
                                       init->as.literal.kind == NY_LIT_STR
                                   ? init
                                   : NULL;
    if (declared_fin_bound > 0 && l->fin_bound == 0)
      l->fin_bound = declared_fin_bound;
    b->last_value = val;
  }
  return true;
}

static bool nyr_lower_if(nyr_builder_t *b, const stmt_t *s) {
  if (s->as.iff.init && !nyr_lower_stmt(b, s->as.iff.init))
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
    if (!nyr_lower_cond_branch(b, s->as.iff.test, then_label, end_label) ||
        !nyr_emit_label(b, then_label))
      return false;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, s->as.iff.conseq))
      return false;
    if (!b->emitted_return && !nyr_emit_br(b, end_label))
      return false;
    if (!nyr_emit_label(b, end_label))
      return false;
    b->emitted_return = entry_return;
    b->last_value = entry_last_value;
    return true;
  }
  int then_label = b->next_label++;
  int else_label = b->next_label++;
  int merge_label = b->next_label++;
  if (!nyr_lower_cond_branch(b, s->as.iff.test, then_label, else_label) ||
      !nyr_emit_label(b, then_label))
    return false;

  int result_slot = nyr_temp_slot(b);
  bool has_alt = s->as.iff.alt != NULL;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;

  /*
   * If no else, pre-store entry_last_value as the false-branch result.
   */
  if (!has_alt && !entry_return && entry_last_value >= 0 &&
      !nyr_store_local_value(b, result_slot, entry_last_value))
    return false;

  /*
   * Then branch.
   */
  b->emitted_return = false;
  if (!nyr_lower_scoped_body(b, s->as.iff.conseq))
    return false;
  bool conseq_returns = b->emitted_return;
  if (!conseq_returns) {
    int conseq_val = b->last_value;
    if (conseq_val >= 0 && !nyr_store_local_value(b, result_slot, conseq_val))
      return false;
    /*
     * Jump to the merge point (before end_label) so both branches converge
     * before the shared load.local.
     */
    if (!nyr_emit_br(b, merge_label))
      return false;
  }
  if (!nyr_emit_label(b, else_label))
    return false;

  /*
   * Else branch.
   */
  b->emitted_return = false;
  b->last_value = entry_last_value;
  if (has_alt && !nyr_lower_scoped_body(b, s->as.iff.alt))
    return false;
  bool alt_returns = b->emitted_return;
  if (!alt_returns && has_alt) {
    int alt_val = b->last_value;
    if (alt_val >= 0 && !nyr_store_local_value(b, result_slot, alt_val))
      return false;
  }

  /*
   * Merge point: both branches converge here.
   */
  b->emitted_return =
      entry_return || (has_alt && conseq_returns && alt_returns);
  if (!b->emitted_return) {
    if (!nyr_emit_label(b, merge_label))
      return false;
  }
  int merged = nyr_load_local_value(b, result_slot);
  if (merged < 0)
    return false;
  b->last_value = merged;
  return true;
}
static bool numeric_init_call(const expr_t *e) {
  return e && e->kind == NY_E_CALL && e->semantic.resolved &&
         (e->semantic.rep == NY_SEM_REP_F64 ||
          e->semantic.rep == NY_SEM_REP_F32 ||
          e->semantic.rep == NY_SEM_REP_RAW_INT);
}

static bool nyr_lower_guard(nyr_builder_t *b, const stmt_t *s) {
  if (s->as.guard.name && s->as.guard.value) {
    char from_fn[256];
    const char *type_name = s->as.guard.type_name;
    const char *from_name = NULL;
    if (type_name) {
      while (*type_name == '?')
        type_name++;
      if (type_name[0] == '*')
        type_name++;
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

    bool has_from_fn = from_name && nyr_find_user_function(b, from_name);
    int value = nyr_lower_expr(b, has_from_fn ? &from_call : val_expr);
    if (value < 0)
      return false;
    nyr_local_t *local = nyr_bind_local(b, s->as.guard.name);
    if (!local)
      return false;
    local->slot = b->next_local_slot++;
    if (!nyr_store_local_value(b, local->slot, value))
      return false;
    int pass_label = b->next_label++;
    int fail_label = b->next_label++;
    if (!nyr_emit_br_if(b, value, pass_label) || !nyr_emit_br(b, fail_label) ||
        !nyr_emit_label(b, fail_label))
      return false;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, s->as.guard.fallback))
      return false;
    if (!b->emitted_return && !nyr_emit_br(b, pass_label))
      return false;
    b->emitted_return = false;
    return nyr_emit_label(b, pass_label);
  } else if (s->as.guard.value) {
    int cond = nyr_lower_expr(b, s->as.guard.value);
    if (cond < 0)
      return false;
    int pass_label = b->next_label++;
    int fail_label = b->next_label++;
    if (!nyr_emit_br_if(b, cond, pass_label) || !nyr_emit_br(b, fail_label) ||
        !nyr_emit_label(b, fail_label))
      return false;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, s->as.guard.fallback))
      return false;
    if (!b->emitted_return && !nyr_emit_br(b, pass_label))
      return false;
    b->emitted_return = false;
    return nyr_emit_label(b, pass_label);
  }
  return true;
}

/*
 * Recognize the canonical bounded builder loop
 *
 *   mut xs = []
 *   mut i = 0
 *   while i < N {
 *      xs = append(xs, value(i))
 *      i += 1
 *   }
 *
 * when N is an immutable integer bound and value(i) is a raw scalar that
 * does not observe xs.  The source loop writes every slot exactly once, so a
 * sized scalar tbuf plus direct stores is representation-equivalent while
 * avoiding the append capacity/header protocol on every iteration.  Keeping
 * this proof in statement lowering also means the generated list length is
 * correct before any following consumer sees the value.
 */
static bool nyr_bounded_fill_bound_expr(nyr_builder_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  const nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
  if (local)
    return !local->semantic_mutable && !local->is_any && !local->is_f64 &&
           !local->is_f32 && !local->is_cstr && !local->is_list &&
           !local->is_dict;
  const expr_t *resolved = nyr_find_top_level_value(b, e->as.ident.name);
  return resolved && resolved != e && resolved->kind == NY_E_LITERAL &&
         resolved->as.literal.kind == NY_LIT_INT &&
         resolved->tok.kind != NY_T_NIL;
}

static bool nyr_bounded_fill_i64_expr(const expr_t *e, const char *index_name) {
  if (!e || !index_name)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind == NY_E_IDENT)
    return e->as.ident.name && strcmp(e->as.ident.name, index_name) == 0;
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
       strcmp(e->as.unary.op, "~") == 0))
    return nyr_bounded_fill_i64_expr(e->as.unary.right, index_name);
  if (e->kind != NY_E_BINARY || !e->as.binary.op)
    return false;
  const char *op = e->as.binary.op;
  bool raw_op =
      strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
      strcmp(op, "%") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
      strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
  return raw_op && nyr_bounded_fill_i64_expr(e->as.binary.left, index_name) &&
         nyr_bounded_fill_i64_expr(e->as.binary.right, index_name);
}

static bool nyr_bounded_fill_is_module_entry(const nyr_builder_t *b) {
  return b && b->current_fn_name && strcmp(b->current_fn_name, "rt_main") == 0;
}

static bool nyr_match_bounded_scalar_append(
    nyr_builder_t *b, const stmt_t *list_decl, const stmt_t *index_decl,
    const stmt_t *loop, const char **list_name_out, const char **index_name_out,
    const expr_t **item_out, const expr_t **bound_expr_out) {
  if (!b || !list_decl || !index_decl || !loop || !list_name_out ||
      !index_name_out || !item_out || !bound_expr_out ||
      /* Entry bindings remain observable while module initialization runs. */
      nyr_bounded_fill_is_module_entry(b) || list_decl->kind != NY_S_VAR ||
      index_decl->kind != NY_S_VAR || loop->kind != NY_S_WHILE)
    return false;

  const stmt_var_t *lv = &list_decl->as.var;
  const stmt_var_t *iv = &index_decl->as.var;
  if (!lv->is_decl || !lv->is_mut || lv->names.len != 1 || lv->exprs.len != 1 ||
      !lv->names.data[0] || !lv->exprs.data[0] ||
      lv->exprs.data[0]->kind != NY_E_LIST ||
      lv->exprs.data[0]->as.list_like.len != 0 || !iv->is_decl || !iv->is_mut ||
      iv->names.len != 1 || iv->exprs.len != 1 || !iv->names.data[0] ||
      !iv->exprs.data[0] || iv->exprs.data[0]->kind != NY_E_LITERAL ||
      iv->exprs.data[0]->as.literal.kind != NY_LIT_INT ||
      iv->exprs.data[0]->as.literal.as.i != 0)
    return false;

  const char *list_name = lv->names.data[0];
  const char *index_name = iv->names.data[0];
  const expr_t *test = loop->as.whl.test;
  if (loop->as.whl.init || loop->as.whl.update || !test ||
      test->kind != NY_E_BINARY || !test->as.binary.op ||
      strcmp(test->as.binary.op, "<") != 0 || !test->as.binary.left ||
      test->as.binary.left->kind != NY_E_IDENT ||
      !test->as.binary.left->as.ident.name ||
      strcmp(test->as.binary.left->as.ident.name, index_name) != 0 ||
      !nyr_bounded_fill_bound_expr(b, test->as.binary.right))
    return false;

  const stmt_t *body = loop->as.whl.body;
  if (!body || body->kind != NY_S_BLOCK || body->as.block.body.len != 2)
    return false;
  const stmt_t *append_stmt = body->as.block.body.data[0];
  const stmt_t *inc_stmt = body->as.block.body.data[1];
  if (!append_stmt || append_stmt->kind != NY_S_VAR ||
      append_stmt->as.var.is_decl || append_stmt->as.var.names.len != 1 ||
      append_stmt->as.var.exprs.len != 1 ||
      !append_stmt->as.var.names.data[0] ||
      strcmp(append_stmt->as.var.names.data[0], list_name) != 0 ||
      !append_stmt->as.var.exprs.data[0] ||
      append_stmt->as.var.exprs.data[0]->kind != NY_E_CALL)
    return false;

  const expr_t *append = append_stmt->as.var.exprs.data[0];
  const char *leaf = ny_native_call_leaf(append);
  if (!leaf || strcmp(leaf, "append") != 0 || append->as.call.args.len != 2 ||
      append->as.call.args.data[0].name || append->as.call.args.data[1].name)
    return false;
  const expr_t *recv = append->as.call.args.data[0].val;
  const expr_t *item = append->as.call.args.data[1].val;
  if (!recv || recv->kind != NY_E_IDENT || !recv->as.ident.name ||
      strcmp(recv->as.ident.name, list_name) != 0 || !item ||
      nyr_expr_uses_ident(b, item, list_name) ||
      !nyr_bounded_fill_i64_expr(item, index_name))
    return false;

  if (!inc_stmt || inc_stmt->kind != NY_S_VAR || inc_stmt->as.var.is_decl ||
      inc_stmt->as.var.names.len != 1 || inc_stmt->as.var.exprs.len != 1 ||
      !inc_stmt->as.var.names.data[0] ||
      strcmp(inc_stmt->as.var.names.data[0], index_name) != 0)
    return false;
  const expr_t *inc = inc_stmt->as.var.exprs.data[0];
  if (!inc || inc->kind != NY_E_BINARY || !inc->as.binary.op ||
      strcmp(inc->as.binary.op, "+") != 0 || !inc->as.binary.left ||
      inc->as.binary.left->kind != NY_E_IDENT ||
      !inc->as.binary.left->as.ident.name ||
      strcmp(inc->as.binary.left->as.ident.name, index_name) != 0 ||
      !inc->as.binary.right || inc->as.binary.right->kind != NY_E_LITERAL ||
      inc->as.binary.right->as.literal.kind != NY_LIT_INT ||
      inc->as.binary.right->as.literal.as.i != 1)
    return false;

  *list_name_out = list_name;
  *index_name_out = index_name;
  *item_out = item;
  *bound_expr_out = test->as.binary.right;
  return true;
}

static bool nyr_lower_bounded_scalar_append(nyr_builder_t *b,
                                            const stmt_t *list_decl,
                                            const stmt_t *index_decl,
                                            const stmt_t *loop) {
  const char *list_name = NULL;
  const char *index_name = NULL;
  const expr_t *item = NULL;
  const expr_t *bound_expr = NULL;
  if (!nyr_match_bounded_scalar_append(b, list_decl, index_decl, loop,
                                       &list_name, &index_name, &item,
                                       &bound_expr))
    return false;

  nyr_local_t *list_local = nyr_bind_local(b, list_name);
  nyr_local_t *index_local = nyr_bind_local(b, index_name);
  if (!list_local || !index_local)
    return false;
  list_local->is_list = true;
  list_local->is_dyn_list = false;
  list_local->is_any = false;
  list_local->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
  index_local->is_any = false;
  index_local->semantic_rep = NY_SEM_REP_RAW_INT;

  int bound = nyr_lower_expr(b, bound_expr);
  int list = bound < 0 ? -1
                       : nyr_emit_runtime_call(b, "rt_list_new_sized_uninit",
                                               bound, -1, -1, 1, 0);
  if (list < 0 || !nyr_record_list_len_fact(b, list, bound) ||
      !nyr_store_local_value(b, list_local->slot, list))
    return false;
  int zero = nyr_emit_const(b, 0);
  if (zero < 0 || !nyr_store_local_value(b, index_local->slot, zero))
    return false;

  int entry_last_value = zero;
  bool entry_return = b->emitted_return;
  int head_label = b->next_label++;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  if (!nyr_emit_label(b, head_label))
    return false;
  int index = nyr_load_local_value(b, index_local->slot);
  int cond = index < 0
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = index,
                                                      .b = bound,
                                                      .cmp = NYIR_CMP_LT});
  if (cond < 0 || !nyr_emit_br_if(b, cond, body_label) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, body_label))
    return false;

  int value = nyr_lower_expr(b, item);
  list = nyr_load_local_value(b, list_local->slot);
  index = nyr_load_local_value(b, index_local->slot);
  int shift = nyr_emit_const(b, 3);
  int offset = index < 0 || shift < 0
                   ? -1
                   : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                        .dst = -1,
                                                        .a = index,
                                                        .b = shift});
  int address = list < 0 || offset < 0 ? -1 : nyr_emit_add_i64(b, list, offset);
  if (value < 0 || address < 0 || !nyr_emit_store_i64(b, address, value))
    return false;
  int one = nyr_emit_const(b, 1);
  int next = index < 0 || one < 0 ? -1 : nyr_emit_add_i64(b, index, one);
  if (next < 0 || !nyr_store_local_value(b, index_local->slot, next) ||
      !nyr_emit_br(b, head_label) || !nyr_emit_label(b, end_label))
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  return true;
}

/*
 * Multi-list form of the bounded builder optimization.  This intentionally
 * runs at the while statement because the target lists need not be declared
 * immediately beside the loop (iterator pipelines commonly declare several
 * output lists together).  We only accept loops made entirely from independent
 * self-appends followed by `i = i + 1`; every target must still be an empty,
 * scalar list at this point and no item may observe a target under
 * construction.
 */
typedef struct {
  nyr_local_t *local;
  const char *name;
  const expr_t *item;
} nyr_multi_fill_t;

static bool nyr_reg_is_const_i64(const nyr_func_t *f, int reg, int64_t want) {
  if (!f || reg < 0)
    return false;
  for (size_t i = f->len; i > 0; --i) {
    const nyr_inst_t *in = &f->data[i - 1];
    if (in->dst != reg)
      continue;
    return in->op == NYIR_CONST_I64 && in->imm == want;
  }
  return false;
}

static bool nyr_local_list_len_is_zero(const nyr_builder_t *b,
                                       const nyr_local_t *local) {
  if (!b || !local || !local->is_list || local->list_len_slot < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->op != NYIR_STORE_LOCAL || in->imm != local->list_len_slot)
      continue;
    if (nyr_reg_is_const_i64(&b->nyir, in->a, 0))
      return true;
    const nyr_inst_t *len_call = NULL;
    for (size_t j = b->nyir.len; j > 0; --j)
      if (b->nyir.data[j - 1].dst == in->a) {
        len_call = &b->nyir.data[j - 1];
        break;
      }
    if (!len_call || len_call->op != NYIR_CALL || !len_call->symbol ||
        strcmp(len_call->symbol, "rt_tbuf_len_raw") != 0)
      return false;
    const nyr_inst_t *new_call = NULL;
    for (size_t j = b->nyir.len; j > 0; --j)
      if (b->nyir.data[j - 1].dst == len_call->a) {
        new_call = &b->nyir.data[j - 1];
        break;
      }
    if (!new_call || new_call->op != NYIR_CALL || !new_call->symbol ||
        strcmp(new_call->symbol, "rt_tbuf_new_raw") != 0 ||
        !nyr_reg_is_const_i64(&b->nyir, new_call->a, 0) ||
        !nyr_reg_is_const_i64(&b->nyir, new_call->b, 8))
      return false;
    return true;
  }
  return false;
}

static bool nyr_multi_fill_raw_i64_expr(nyr_builder_t *b, const expr_t *e,
                                        const char *index_name) {
  if (!b || !e || !index_name)
    return false;
  if (nyr_expr_is_f64(b, e) || nyr_expr_is_f32(b, e) ||
      nyr_expr_is_cstr(b, e) || nyr_expr_is_list(b, e) ||
      nyr_expr_is_dict(b, e))
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind == NY_E_IDENT) {
    if (!e->as.ident.name)
      return false;
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    return l && !l->is_any && !l->is_f64 && !l->is_f32 && !l->is_cstr &&
           !l->is_list && !l->is_dict;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
       strcmp(e->as.unary.op, "~") == 0))
    return nyr_multi_fill_raw_i64_expr(b, e->as.unary.right, index_name);
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    const char *op = e->as.binary.op;
    bool raw_op =
        strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "%") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
        strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
    return raw_op &&
           nyr_multi_fill_raw_i64_expr(b, e->as.binary.left, index_name) &&
           nyr_multi_fill_raw_i64_expr(b, e->as.binary.right, index_name);
  }
  if (e->kind == NY_E_INDEX && e->as.index.target && e->as.index.start &&
      e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name) {
    const char *name = e->as.index.target->as.ident.name;
    nyr_local_t *list = nyr_find_local(b, name);
    bool scalar_list = list && list->is_list && !list->is_dyn_list;
    if (!scalar_list && list && list->list_len_slot >= 0 &&
        nyr_top_mut_is_append_int_list(b, name)) {
      list->is_list = true;
      list->is_dyn_list = false;
      list->is_any = false;
      list->type_name = "list<int>";
      list->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
      scalar_list = true;
    }
    return scalar_list &&
           nyr_multi_fill_raw_i64_expr(b, e->as.index.start, index_name);
  }
  return false;
}

/*
 * Bounded append pipeline optimization.
 *
 * This is the continuation form of the empty-list multi-fill transform above:
 * a loop may append one scalar value per iteration to one or more already
 * materialized scalar tbufs, optionally after computing immutable scalar
 * temporaries.  We reserve each target once, write at base_len + i, and commit
 * the final length once after the loop.  The transform is deliberately narrow:
 * i must currently be zero, all non-append statements are immutable scalar
 * declarations that precede the appends, each target is appended exactly once
 * per iteration, and neither a temporary nor an appended value may observe a
 * target list while it is under construction.
 */
typedef struct {
  nyr_local_t *local;
  const char *name;
  const expr_t *item;
  int base_len;
  bool replace_empty_dynamic;
  bool base_is_bound;
} nyr_append_fill_t;

static bool nyr_local_last_const_i64(const nyr_builder_t *b,
                                     const nyr_local_t *local, int64_t *out);

static bool nyr_bounded_fill_bound_nonnegative(nyr_builder_t *b,
                                               const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return e->as.literal.as.i >= 0;
  if (e->kind != NY_E_IDENT || !e->as.ident.name)
    return false;
  const nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
  if (local)
    return false;
  const expr_t *resolved = nyr_find_top_level_value(b, e->as.ident.name);
  return resolved && resolved != e &&
         nyr_bounded_fill_bound_nonnegative(b, resolved);
}

static const nyr_inst_t *nyr_reg_def_inst(const nyr_builder_t *b, int reg) {
  if (!b || reg < 0)
    return NULL;
  for (size_t i = b->nyir.len; i > 0; --i)
    if (b->nyir.data[i - 1].dst == reg)
      return &b->nyir.data[i - 1];
  return NULL;
}

static int nyr_latest_local_store_value(const nyr_builder_t *b, int64_t slot) {
  if (!b || slot < 0)
    return -1;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->op == NYIR_STORE_LOCAL && in->imm == slot)
      return in->a;
  }
  return -1;
}

static bool nyr_simple_values_same(const nyr_builder_t *b, int a, int c) {
  if (!b || a < 0 || c < 0)
    return false;
  if (a == c)
    return true;
  const nyr_inst_t *da = nyr_reg_def_inst(b, a);
  const nyr_inst_t *dc = nyr_reg_def_inst(b, c);
  if (!da || !dc)
    return false;
  if (da->op == NYIR_CONST_I64 && dc->op == NYIR_CONST_I64)
    return da->imm == dc->imm;
  if (da->op == NYIR_LOAD_LOCAL && dc->op == NYIR_LOAD_LOCAL)
    return da->imm == dc->imm;
  return false;
}

static bool nyr_local_value_is_const_i64(const nyr_builder_t *b,
                                         const nyr_local_t *local,
                                         int64_t want) {
  if (!b || !local || local->slot < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->op != NYIR_STORE_LOCAL || in->imm != local->slot)
      continue;
    return nyr_reg_is_const_i64(&b->nyir, in->a, want);
  }
  return false;
}

static bool nyr_local_last_const_i64(const nyr_builder_t *b,
                                     const nyr_local_t *local, int64_t *out) {
  if (!b || !local || !out || local->slot < 0)
    return false;
  for (size_t i = b->nyir.len; i > 0; --i) {
    const nyr_inst_t *in = &b->nyir.data[i - 1];
    if (in->op == NYIR_LABEL || in->op == NYIR_BR || in->op == NYIR_BR_IF ||
        in->op == NYIR_RET)
      return false;
    if (in->op != NYIR_STORE_LOCAL || in->imm != local->slot)
      continue;
    const nyr_inst_t *def = nyr_reg_def_inst(b, in->a);
    if (!def || def->op != NYIR_CONST_I64)
      return false;
    *out = def->imm;
    return true;
  }
  return false;
}

static bool nyr_overwrite_expr_safe(const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL || e->kind == NY_E_IDENT)
    return true;
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
       strcmp(e->as.unary.op, "~") == 0 || strcmp(e->as.unary.op, "!") == 0))
    return nyr_overwrite_expr_safe(e->as.unary.right);
  if (e->kind == NY_E_BINARY && e->as.binary.op && e->as.binary.left &&
      e->as.binary.right) {
    const char *op = e->as.binary.op;
    bool safe_op = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                   strcmp(op, "*") == 0 || strcmp(op, "&") == 0 ||
                   strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
                   strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                   strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;
    return safe_op && nyr_overwrite_expr_safe(e->as.binary.left) &&
           nyr_overwrite_expr_safe(e->as.binary.right);
  }
  return false;
}

static bool nyr_overwrite_expr_raw_i64(nyr_builder_t *b, const expr_t *e) {
  if (!b || !e)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT ||
           e->as.literal.kind == NY_LIT_BOOL;
  if (e->kind == NY_E_IDENT) {
    nyr_local_t *local =
        e->as.ident.name ? nyr_find_local(b, e->as.ident.name) : NULL;
    return local && !local->is_any && !local->is_f64 && !local->is_f32 &&
           !local->is_cstr && !local->is_list && !local->is_dict &&
           !local->is_bigint && !local->is_capture;
  }
  if (e->kind == NY_E_UNARY)
    return nyr_overwrite_expr_safe(e) &&
           nyr_overwrite_expr_raw_i64(b, e->as.unary.right);
  if (e->kind == NY_E_BINARY)
    return nyr_overwrite_expr_safe(e) &&
           nyr_overwrite_expr_raw_i64(b, e->as.binary.left) &&
           nyr_overwrite_expr_raw_i64(b, e->as.binary.right);
  return false;
}

static bool nyr_name_in_set(const char *name, const char *const *names,
                            size_t n) {
  if (!name)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (names[i] && strcmp(name, names[i]) == 0)
      return true;
  return false;
}

static bool nyr_append_fill_raw_i64_expr(nyr_builder_t *b, const expr_t *e,
                                         const char *index_name,
                                         const char *const *temps,
                                         size_t temp_count) {
  if (!b || !e || !index_name)
    return false;
  if (e->kind == NY_E_IDENT && e->as.ident.name &&
      nyr_name_in_set(e->as.ident.name, temps, temp_count))
    return true;
  if (nyr_expr_is_any(b, e) || nyr_expr_is_f64(b, e) || nyr_expr_is_f32(b, e) ||
      nyr_expr_is_cstr(b, e) || nyr_expr_is_list(b, e) ||
      nyr_expr_is_dict(b, e))
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->kind == NY_E_IDENT) {
    if (!e->as.ident.name)
      return false;
    nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    return l && !l->is_any && !l->is_f64 && !l->is_f32 && !l->is_cstr &&
           !l->is_list && !l->is_dict;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      (strcmp(e->as.unary.op, "+") == 0 || strcmp(e->as.unary.op, "-") == 0 ||
       strcmp(e->as.unary.op, "~") == 0))
    return nyr_append_fill_raw_i64_expr(b, e->as.unary.right, index_name, temps,
                                        temp_count);
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    const char *op = e->as.binary.op;
    bool raw_op =
        strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "%") == 0 || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
        strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
    return raw_op &&
           nyr_append_fill_raw_i64_expr(b, e->as.binary.left, index_name, temps,
                                        temp_count) &&
           nyr_append_fill_raw_i64_expr(b, e->as.binary.right, index_name,
                                        temps, temp_count);
  }
  if (e->kind == NY_E_INDEX && e->as.index.target && e->as.index.start &&
      e->as.index.target->kind == NY_E_IDENT &&
      e->as.index.target->as.ident.name) {
    nyr_local_t *list = nyr_find_local(b, e->as.index.target->as.ident.name);
    return list && list->is_list && !list->is_dyn_list &&
           nyr_append_fill_raw_i64_expr(b, e->as.index.start, index_name, temps,
                                        temp_count);
  }
  return false;
}

static bool nyr_try_lower_bounded_append_fill(nyr_builder_t *b, const stmt_t *s,
                                              bool *matched) {
  /* Declarations have already passed normal storage lowering. At module
   * entry, only bindings proven private by nyr_entry_binding_can_localize
   * have local slots for this transform; observable globals stay in cells. */
  if (matched)
    *matched = false;
  if (!b || !s || s->kind != NY_S_WHILE || s->as.whl.init || s->as.whl.update ||
      !s->as.whl.test || s->as.whl.test->kind != NY_E_BINARY ||
      !s->as.whl.test->as.binary.op ||
      strcmp(s->as.whl.test->as.binary.op, "<") != 0)
    return true;

  const expr_t *lhs = s->as.whl.test->as.binary.left;
  const expr_t *bound_expr = s->as.whl.test->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name ||
      !nyr_bounded_fill_bound_expr(b, bound_expr))
    return true;
  const char *index_name = lhs->as.ident.name;
  nyr_local_t *index_local = nyr_find_local(b, index_name);
  if (!index_local || index_local->is_any || index_local->is_f64 ||
      index_local->is_f32 || index_local->is_list || index_local->is_dict ||
      !nyr_local_value_is_const_i64(b, index_local, 0))
    return true;

  const stmt_t *body = s->as.whl.body;
  if (!body || body->kind != NY_S_BLOCK || body->as.block.body.len < 3 ||
      body->as.block.body.len > 25)
    return true;

  size_t last = body->as.block.body.len - 1;
  const stmt_t *inc_stmt = body->as.block.body.data[last];
  if (!inc_stmt || inc_stmt->kind != NY_S_VAR || inc_stmt->as.var.is_decl ||
      inc_stmt->as.var.names.len != 1 || inc_stmt->as.var.exprs.len != 1 ||
      !inc_stmt->as.var.names.data[0] ||
      strcmp(inc_stmt->as.var.names.data[0], index_name) != 0)
    return true;
  const expr_t *inc = inc_stmt->as.var.exprs.data[0];
  if (!inc || inc->kind != NY_E_BINARY || !inc->as.binary.op ||
      strcmp(inc->as.binary.op, "+") != 0 || !inc->as.binary.left ||
      inc->as.binary.left->kind != NY_E_IDENT ||
      !inc->as.binary.left->as.ident.name ||
      strcmp(inc->as.binary.left->as.ident.name, index_name) != 0 ||
      !inc->as.binary.right || inc->as.binary.right->kind != NY_E_LITERAL ||
      inc->as.binary.right->as.literal.kind != NY_LIT_INT ||
      inc->as.binary.right->as.literal.as.i != 1)
    return true;

  const stmt_t *temps_stmt[8] = {0};
  const char *temps[8] = {0};
  size_t temp_count = 0;
  nyr_append_fill_t fills[16] = {{0}};
  size_t fill_count = 0;
  bool saw_append = false;

  for (size_t si = 0; si < last; ++si) {
    const stmt_t *st = body->as.block.body.data[si];
    if (!st || st->kind != NY_S_VAR || st->as.var.names.len != 1 ||
        st->as.var.exprs.len != 1 || !st->as.var.names.data[0] ||
        !st->as.var.exprs.data[0])
      return true;

    if (st->as.var.is_decl) {
      if (saw_append || st->as.var.is_mut || temp_count >= 8)
        return true;
      temps_stmt[temp_count] = st;
      temps[temp_count++] = st->as.var.names.data[0];
      continue;
    }

    saw_append = true;
    if (fill_count >= 16 || st->as.var.exprs.data[0]->kind != NY_E_CALL)
      return true;
    const char *name = st->as.var.names.data[0];
    const expr_t *call = st->as.var.exprs.data[0];
    const char *leaf = ny_native_call_leaf(call);
    if (!leaf || strcmp(leaf, "append") != 0 || call->as.call.args.len != 2 ||
        call->as.call.args.data[0].name || call->as.call.args.data[1].name)
      return true;
    const expr_t *recv = call->as.call.args.data[0].val;
    const expr_t *item = call->as.call.args.data[1].val;
    if (!recv || recv->kind != NY_E_IDENT || !recv->as.ident.name ||
        strcmp(recv->as.ident.name, name) != 0 || !item)
      return true;
    nyr_local_t *local = nyr_find_local(b, name);
    if (!local || !local->is_list || local->list_len_slot < 0)
      return true;
    bool replace_empty_dynamic = local->is_dyn_list;
    if (replace_empty_dynamic && !nyr_local_list_len_is_zero(b, local))
      return true;
    for (size_t j = 0; j < fill_count; ++j)
      if (strcmp(fills[j].name, name) == 0)
        return true;
    fills[fill_count++] =
        (nyr_append_fill_t){.local = local,
                            .name = name,
                            .item = item,
                            .base_len = -1,
                            .replace_empty_dynamic = replace_empty_dynamic,
                            .base_is_bound = false};
  }
  if (!saw_append || fill_count == 0)
    return true;

  /* Temporary expressions and append values must not observe any target. */
  for (size_t ti = 0; ti < temp_count; ++ti) {
    const expr_t *e = temps_stmt[ti]->as.var.exprs.data[0];
    for (size_t j = 0; j < fill_count; ++j)
      if (nyr_expr_uses_ident(b, e, fills[j].name))
        return true;
    if (!nyr_append_fill_raw_i64_expr(b, e, index_name, temps, ti))
      return true;
  }
  for (size_t i = 0; i < fill_count; ++i) {
    for (size_t j = 0; j < fill_count; ++j)
      if (nyr_expr_uses_ident(b, fills[i].item, fills[j].name))
        return true;
    if (!nyr_append_fill_raw_i64_expr(b, fills[i].item, index_name, temps,
                                      temp_count))
      return true;
  }

  int bound = nyr_lower_expr(b, bound_expr);
  if (bound < 0)
    return false;

  /* Reserve once while preserving each target's current logical length.
   * A dynamic empty-list literal can be replaced outright by an 8-byte scalar
   * builder because no element (and therefore no descriptor tag) exists yet. */
  for (size_t i = 0; i < fill_count; ++i) {
    int list = nyr_load_local_value(b, fills[i].local->slot);
    int base = nyr_load_local_value(b, fills[i].local->list_len_slot);
    int prior_len_value =
        nyr_latest_local_store_value(b, fills[i].local->list_len_slot);
    bool base_is_bound = nyr_simple_values_same(b, prior_len_value, bound);
    int reserved = -1;
    if (fills[i].replace_empty_dynamic) {
      int zero = nyr_emit_const(b, 0);
      reserved = bound < 0 ? -1
                           : nyr_emit_runtime_call(b, "rt_list_new_raw", bound,
                                                   -1, -1, 1, 0);
      base = zero;
    } else {
      int capacity =
          list < 0 || base < 0 ? -1 : nyr_emit_add_i64(b, base, bound);
      reserved = capacity < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_tbuf_reserve",
                                                      list, capacity, -1, 2, 0);
    }
    if (reserved < 0 || base < 0 ||
        !nyr_record_list_len_fact(b, reserved, base) ||
        !nyr_store_local_value(b, fills[i].local->slot, reserved))
      return false;
    fills[i].base_is_bound = base_is_bound;
    fills[i].base_len = base;
    fills[i].local->is_dyn_list = false;
    fills[i].local->is_any = false;
    /* The guarded append proof establishes an elem-8 scalar layout even
     * when the source deliberately leaves the list unannotated.  Preserve
     * that inferred contract for all later indexed reads/writes; merely
     * clearing is_dyn_list is not enough because an untyped list otherwise
     * remains on the conservative dynamic accessor path. */
    fills[i].local->type_name = "list<int>";
    fills[i].local->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
  }

  int head_label = b->next_label++;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  if (!nyr_emit_label(b, head_label))
    return false;
  int index = nyr_load_local_value(b, index_local->slot);
  int cond = index < 0
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = index,
                                                      .b = bound,
                                                      .cmp = NYIR_CMP_LT});
  if (cond < 0 || !nyr_emit_br_if(b, cond, body_label) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, body_label))
    return false;

  /* Lower immutable scalar temporaries in source order. */
  for (size_t ti = 0; ti < temp_count; ++ti)
    if (!nyr_lower_stmt(b, temps_stmt[ti]))
      return false;

  for (size_t i = 0; i < fill_count; ++i) {
    int value = nyr_lower_expr(b, fills[i].item);
    int list = nyr_load_local_value(b, fills[i].local->slot);
    index = nyr_load_local_value(b, index_local->slot);
    int pos = index < 0 || fills[i].base_len < 0
                  ? -1
                  : nyr_emit_add_i64(b, fills[i].base_len, index);
    int shift = nyr_emit_const(b, 3);
    int offset = pos < 0 || shift < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = pos,
                                                          .b = shift});
    int address =
        list < 0 || offset < 0 ? -1 : nyr_emit_add_i64(b, list, offset);
    if (value < 0 || address < 0 || !nyr_emit_store_i64(b, address, value))
      return false;
  }

  index = nyr_load_local_value(b, index_local->slot);
  int one = nyr_emit_const(b, 1);
  int next = index < 0 || one < 0 ? -1 : nyr_emit_add_i64(b, index, one);
  if (next < 0 || !nyr_store_local_value(b, index_local->slot, next) ||
      !nyr_emit_br(b, head_label) || !nyr_emit_label(b, end_label))
    return false;

  /* Commit exactly the number of elements actually produced.  For a proven
   * non-negative bound this structured loop has no early exit, so its final
   * induction value equals `bound`.  Keeping that SSA identity improves later
   * BCE (list lengths remain visibly tied to the guard expression). */
  int produced = nyr_load_local_value(b, index_local->slot);
  bool exact_bound = nyr_bounded_fill_bound_nonnegative(b, bound_expr);
  int produced_exact = exact_bound ? bound : produced;
  for (size_t i = 0; i < fill_count; ++i) {
    int final_len = -1;
    if (nyr_reg_is_const_i64(&b->nyir, fills[i].base_len, 0)) {
      final_len = produced_exact;
    } else if (exact_bound && fills[i].base_is_bound) {
      int one_shift = nyr_emit_const(b, 1);
      final_len = one_shift < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                           .dst = -1,
                                                           .a = bound,
                                                           .b = one_shift});
    } else if (produced_exact >= 0 && fills[i].base_len >= 0) {
      final_len = nyr_emit_add_i64(b, fills[i].base_len, produced_exact);
    }
    int list = nyr_load_local_value(b, fills[i].local->slot);
    int updated = final_len < 0 || list < 0
                      ? -1
                      : nyr_emit_runtime_call(b, "rt_list_set_len_raw", list,
                                              final_len, -1, 2, 0);
    if (updated < 0 || !nyr_record_list_len_fact(b, updated, final_len) ||
        !nyr_store_local_value(b, fills[i].local->slot, updated))
      return false;
  }

  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  if (matched)
    *matched = true;
  return true;
}

static bool nyr_try_lower_multi_bounded_fill(nyr_builder_t *b, const stmt_t *s,
                                             bool *matched) {
  if (matched)
    *matched = false;
  if (!b || !s || s->kind != NY_S_WHILE || s->as.whl.init || s->as.whl.update ||
      !s->as.whl.test || s->as.whl.test->kind != NY_E_BINARY ||
      !s->as.whl.test->as.binary.op ||
      strcmp(s->as.whl.test->as.binary.op, "<") != 0)
    return true;
  const expr_t *lhs = s->as.whl.test->as.binary.left;
  const expr_t *bound_expr = s->as.whl.test->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name ||
      !nyr_bounded_fill_bound_expr(b, bound_expr))
    return true;
  const char *index_name = lhs->as.ident.name;
  nyr_local_t *index_local = nyr_find_local(b, index_name);
  if (!index_local || index_local->is_any || index_local->is_f64 ||
      index_local->is_f32 || index_local->is_list || index_local->is_dict)
    return true;

  const stmt_t *body = s->as.whl.body;
  if (!body || body->kind != NY_S_BLOCK || body->as.block.body.len < 2 ||
      body->as.block.body.len > 17)
    return true;
  size_t append_count = body->as.block.body.len - 1;
  nyr_multi_fill_t fills[16] = {{0}};
  if (append_count == 0 || append_count > 16)
    return true;

  for (size_t i = 0; i < append_count; ++i) {
    const stmt_t *st = body->as.block.body.data[i];
    if (!st || st->kind != NY_S_VAR || st->as.var.is_decl ||
        st->as.var.names.len != 1 || st->as.var.exprs.len != 1 ||
        !st->as.var.names.data[0] || !st->as.var.exprs.data[0] ||
        st->as.var.exprs.data[0]->kind != NY_E_CALL)
      return true;
    const char *name = st->as.var.names.data[0];
    const expr_t *call = st->as.var.exprs.data[0];
    const char *leaf = ny_native_call_leaf(call);
    if (!leaf || strcmp(leaf, "append") != 0 || call->as.call.args.len != 2 ||
        call->as.call.args.data[0].name || call->as.call.args.data[1].name)
      return true;
    const expr_t *recv = call->as.call.args.data[0].val;
    const expr_t *item = call->as.call.args.data[1].val;
    if (!recv || recv->kind != NY_E_IDENT || !recv->as.ident.name ||
        strcmp(recv->as.ident.name, name) != 0 || !item)
      return true;
    nyr_local_t *local = nyr_find_local(b, name);
    if (local && local->list_len_slot >= 0 &&
        (!local->is_list || local->is_dyn_list) &&
        nyr_top_mut_is_append_int_list(b, name)) {
      local->is_list = true;
      local->is_dyn_list = false;
      local->is_any = false;
      local->type_name = "list<int>";
      local->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
    }
    if (!local || !local->is_list || !nyr_local_list_len_is_zero(b, local))
      return true;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(fills[j].name, name) == 0)
        return true;
    fills[i] = (nyr_multi_fill_t){local, name, item};
  }

  const stmt_t *inc_stmt = body->as.block.body.data[append_count];
  if (!inc_stmt || inc_stmt->kind != NY_S_VAR || inc_stmt->as.var.is_decl ||
      inc_stmt->as.var.names.len != 1 || inc_stmt->as.var.exprs.len != 1 ||
      !inc_stmt->as.var.names.data[0] ||
      strcmp(inc_stmt->as.var.names.data[0], index_name) != 0)
    return true;
  const expr_t *inc = inc_stmt->as.var.exprs.data[0];
  if (!inc || inc->kind != NY_E_BINARY || !inc->as.binary.op ||
      strcmp(inc->as.binary.op, "+") != 0 || !inc->as.binary.left ||
      inc->as.binary.left->kind != NY_E_IDENT ||
      !inc->as.binary.left->as.ident.name ||
      strcmp(inc->as.binary.left->as.ident.name, index_name) != 0 ||
      !inc->as.binary.right || inc->as.binary.right->kind != NY_E_LITERAL ||
      inc->as.binary.right->as.literal.kind != NY_LIT_INT ||
      inc->as.binary.right->as.literal.as.i != 1)
    return true;

  for (size_t i = 0; i < append_count; ++i) {
    for (size_t j = 0; j < append_count; ++j)
      if (nyr_expr_uses_ident(b, fills[i].item, fills[j].name))
        return true;
    if (!nyr_multi_fill_raw_i64_expr(b, fills[i].item, index_name))
      return true;
  }

  int bound = nyr_lower_expr(b, bound_expr);
  if (bound < 0)
    return false;
  for (size_t i = 0; i < append_count; ++i) {
    int list = nyr_emit_runtime_call(b, "rt_list_new_sized_uninit", bound, -1,
                                     -1, 1, 0);
    if (list < 0 || !nyr_record_list_len_fact(b, list, bound) ||
        !nyr_store_local_value(b, fills[i].local->slot, list))
      return false;
    fills[i].local->is_dyn_list = false;
    fills[i].local->is_any = false;
    /* Multi-fill proves the same fixed-width scalar representation as the
     * single-list transform above. */
    fills[i].local->type_name = "list<int>";
    fills[i].local->semantic_rep = NY_SEM_REP_TYPED_BUFFER;
  }

  int head_label = b->next_label++;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  if (!nyr_emit_label(b, head_label))
    return false;
  int index = nyr_load_local_value(b, index_local->slot);
  int cond = index < 0
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = index,
                                                      .b = bound,
                                                      .cmp = NYIR_CMP_LT});
  if (cond < 0 || !nyr_emit_br_if(b, cond, body_label) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, body_label))
    return false;

  for (size_t i = 0; i < append_count; ++i) {
    int value = nyr_lower_expr(b, fills[i].item);
    int list = nyr_load_local_value(b, fills[i].local->slot);
    index = nyr_load_local_value(b, index_local->slot);
    int shift = nyr_emit_const(b, 3);
    int offset = index < 0 || shift < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                          .dst = -1,
                                                          .a = index,
                                                          .b = shift});
    int address =
        list < 0 || offset < 0 ? -1 : nyr_emit_add_i64(b, list, offset);
    if (value < 0 || address < 0 || !nyr_emit_store_i64(b, address, value))
      return false;
  }

  index = nyr_load_local_value(b, index_local->slot);
  int one = nyr_emit_const(b, 1);
  int next = index < 0 || one < 0 ? -1 : nyr_emit_add_i64(b, index, one);
  if (next < 0 || !nyr_store_local_value(b, index_local->slot, next) ||
      !nyr_emit_br(b, head_label) || !nyr_emit_label(b, end_label))
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  if (matched)
    *matched = true;
  return true;
}

static bool nyr_lower_while(nyr_builder_t *b, const stmt_t *s) {
  if (s->as.whl.init && !nyr_lower_stmt(b, s->as.whl.init))
    return false;
  if (s->as.whl.test && s->as.whl.test->kind == NY_E_LITERAL) {
    if (s->as.whl.test->as.literal.kind == NY_LIT_INT &&
        s->as.whl.test->as.literal.as.i == 0)
      return true;
    if (s->as.whl.test->as.literal.kind == NY_LIT_BOOL &&
        !s->as.whl.test->as.literal.as.b)
      return true;
  }

  bool append_fill_matched = false;
  if (!nyr_try_lower_bounded_append_fill(b, s, &append_fill_matched))
    return false;
  if (append_fill_matched)
    return true;

  bool multi_fill_matched = false;
  if (!nyr_try_lower_multi_bounded_fill(b, s, &multi_fill_matched))
    return false;
  if (multi_fill_matched)
    return true;

  /*
   * Collapse a constant-trip loop when every iteration overwrites the same
   * scalar and neither arm reads its previous value.  The only accepted
   * shape is `while i < bound { if c { x = a } else { x = b }; i += 1 }`.
   * Evaluating the final iteration is equivalent only when the trip count is
   * positive and representable; otherwise the ordinary CFG lowering applies.
   */
  {
    const stmt_t *body = s->as.whl.body;
    if (body && body->kind == NY_S_BLOCK && body->as.block.body.len == 2 &&
        !s->as.whl.update && !s->as.whl.init && s->as.whl.test &&
        s->as.whl.test->kind == NY_E_BINARY && s->as.whl.test->as.binary.op &&
        strcmp(s->as.whl.test->as.binary.op, "<") == 0 &&
        s->as.whl.test->as.binary.left &&
        s->as.whl.test->as.binary.left->kind == NY_E_IDENT &&
        s->as.whl.test->as.binary.left->as.ident.name &&
        s->as.whl.test->as.binary.right &&
        s->as.whl.test->as.binary.right->kind == NY_E_LITERAL &&
        s->as.whl.test->as.binary.right->as.literal.kind == NY_LIT_INT) {
      const char *iv_name = s->as.whl.test->as.binary.left->as.ident.name;
      int64_t bound = s->as.whl.test->as.binary.right->as.literal.as.i;
      const stmt_t *branch = body->as.block.body.data[0];
      const stmt_t *step = body->as.block.body.data[1];
      if (branch && branch->kind == NY_S_IF && !branch->as.iff.init &&
          branch->as.iff.test && step && step->kind == NY_S_VAR &&
          !step->as.var.is_decl && step->as.var.names.len == 1 &&
          step->as.var.exprs.len == 1 && step->as.var.names.data[0] &&
          strcmp(step->as.var.names.data[0], iv_name) == 0) {
        const expr_t *increment = step->as.var.exprs.data[0];
        bool unit_step =
            increment && increment->kind == NY_E_BINARY &&
            increment->as.binary.op &&
            strcmp(increment->as.binary.op, "+") == 0 &&
            increment->as.binary.left &&
            increment->as.binary.left->kind == NY_E_IDENT &&
            increment->as.binary.left->as.ident.name &&
            strcmp(increment->as.binary.left->as.ident.name, iv_name) == 0 &&
            increment->as.binary.right &&
            increment->as.binary.right->kind == NY_E_LITERAL &&
            increment->as.binary.right->as.literal.kind == NY_LIT_INT &&
            increment->as.binary.right->as.literal.as.i == 1;
        const stmt_t *then_s = branch->as.iff.conseq;
        const stmt_t *else_s = branch->as.iff.alt;
        if (then_s && then_s->kind == NY_S_BLOCK &&
            then_s->as.block.body.len == 1)
          then_s = then_s->as.block.body.data[0];
        if (else_s && else_s->kind == NY_S_BLOCK &&
            else_s->as.block.body.len == 1)
          else_s = else_s->as.block.body.data[0];
        bool simple_assign =
            then_s && else_s && then_s->kind == NY_S_VAR &&
            else_s->kind == NY_S_VAR && !then_s->as.var.is_decl &&
            !else_s->as.var.is_decl && then_s->as.var.names.len == 1 &&
            else_s->as.var.names.len == 1 && then_s->as.var.exprs.len == 1 &&
            else_s->as.var.exprs.len == 1 && then_s->as.var.names.data[0] &&
            else_s->as.var.names.data[0] &&
            strcmp(then_s->as.var.names.data[0],
                   else_s->as.var.names.data[0]) == 0 &&
            strcmp(then_s->as.var.names.data[0], iv_name) != 0;
        const char *dst_name =
            simple_assign ? then_s->as.var.names.data[0] : NULL;
        bool expressions_safe =
            simple_assign && nyr_overwrite_expr_safe(branch->as.iff.test) &&
            !nyr_expr_uses_ident(NULL, branch->as.iff.test, iv_name) &&
            !nyr_expr_uses_ident(NULL, branch->as.iff.test, dst_name) &&
            nyr_overwrite_expr_safe(then_s->as.var.exprs.data[0]) &&
            nyr_overwrite_expr_safe(else_s->as.var.exprs.data[0]) &&
            nyr_overwrite_expr_raw_i64(b, branch->as.iff.test) &&
            nyr_overwrite_expr_raw_i64(b, then_s->as.var.exprs.data[0]) &&
            nyr_overwrite_expr_raw_i64(b, else_s->as.var.exprs.data[0]) &&
            !nyr_expr_uses_ident(NULL, then_s->as.var.exprs.data[0],
                                 dst_name) &&
            !nyr_expr_uses_ident(NULL, else_s->as.var.exprs.data[0], dst_name);
        nyr_local_t *iv = nyr_find_local(b, iv_name);
        nyr_local_t *dst = dst_name ? nyr_find_local(b, dst_name) : NULL;
        int64_t initial = 0;
        if (unit_step && expressions_safe && iv && dst && !iv->is_any &&
            !iv->is_f64 && !iv->is_f32 && !iv->is_bigint && !iv->is_capture &&
            !dst->is_any && !dst->is_f64 && !dst->is_f32 && !dst->is_bigint &&
            !dst->is_capture && nyr_local_last_const_i64(b, iv, &initial) &&
            initial < bound && (uint64_t)bound - (uint64_t)initial <= 1024u &&
            bound != INT64_MIN) {
          int64_t last = bound - 1;
          int64_t trip = 0;
          if (!__builtin_sub_overflow(bound, initial, &trip) && trip > 0 &&
              trip <= 1024) {
            int at_last = nyr_emit_const(b, last);
            int at_exit = nyr_emit_const(b, bound);
            if (at_last < 0 || at_exit < 0 ||
                !nyr_store_local_value(b, iv->slot, at_last))
              return false;
            if (!nyr_lower_stmt(b, branch) ||
                !nyr_store_local_value(b, iv->slot, at_exit))
              return false;
            return true;
          }
        }
      }
    }
  }

  int head_label = b->next_label++;
  int update_label = s->as.whl.update ? b->next_label++ : head_label;
  int body_label = b->next_label++;
  int end_label = b->next_label++;
  if (!nyr_emit_label(b, head_label))
    return false;
  if (!nyr_lower_cond_branch(b, s->as.whl.test, body_label, end_label) ||
      !nyr_emit_label(b, body_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  size_t loop_i = b->loop_depth;
  if (!nyr_push_loop(b, head_label, update_label, end_label))
    return false;
  b->emitted_return = false;
  bool body_ok = nyr_lower_scoped_body(b, s->as.whl.body);
  bool body_terminated = b->emitted_return;
  if (body_ok && s->as.whl.update) {
    b->emitted_return = false;
    body_ok = nyr_emit_label(b, update_label) &&
              nyr_lower_scoped_body(b, s->as.whl.update);
    body_terminated = b->emitted_return;
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  if (!body_terminated && !nyr_emit_br(b, head_label))
    return false;
  return nyr_emit_label(b, end_label);
}

static bool nyr_iterable_is_range(const expr_t *iterable, const expr_t **lo,
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
static bool nyr_self_concat_append(const stmt_t *body, nyr_local_t *local,
                                   nyr_builder_t *b, const expr_t **rhs_out) {
  if (!body || !local || !rhs_out)
    return false;
  if (body->kind == NY_S_BLOCK) {
    if (body->as.block.body.len != 1)
      return false;
    body = body->as.block.body.data[0];
  }
  if (!body || body->kind != NY_S_VAR || body->as.var.is_decl ||
      body->as.var.names.len != 1 || body->as.var.exprs.len != 1 ||
      !body->as.var.names.data[0] ||
      strcmp(body->as.var.names.data[0], local->name))
    return false;
  const expr_t *sum = body->as.var.exprs.data[0];
  if (!sum || sum->kind != NY_E_BINARY || !sum->as.binary.op ||
      strcmp(sum->as.binary.op, "+") || !sum->as.binary.left ||
      sum->as.binary.left->kind != NY_E_IDENT ||
      !sum->as.binary.left->as.ident.name ||
      strcmp(sum->as.binary.left->as.ident.name, local->name))
    return false;
  const expr_t *rhs = sum->as.binary.right;
  if (!rhs ||
      ((rhs->kind != NY_E_LITERAL || rhs->as.literal.kind != NY_LIT_STR) &&
       (rhs->kind != NY_E_IDENT || !rhs->as.ident.name ||
        strcmp(rhs->as.ident.name, local->name) == 0 ||
        !nyr_expr_is_cstr(b, rhs))))
    return false;
  *rhs_out = rhs;
  return true;
}

/*
 * The entry function's root `mut` bindings live in module cells rather than
 * in the compact local table.  Give the same exclusive-builder optimization
 * to a root string as to a local string.  Without this path a loop such as
 * `mut s = ""; for ... { s = s + "abc" }` allocates and copies the complete
 * prefix on every iteration, turning a linear fixture into quadratic memory
 * traffic.  The proof is deliberately structural: the loop body must be the
 * sole assignment to the root string and its right hand side must be a
 * literal, so no alias can observe the intermediate builder.
 */
static bool nyr_global_self_concat_append(nyr_builder_t *b, const stmt_t *body,
                                          const char **name_out,
                                          const expr_t **rhs_out,
                                          const char **symbol_out) {
  if (!b || !b->prog || !body || !name_out || !rhs_out || !symbol_out)
    return false;
  if (body->kind == NY_S_BLOCK) {
    if (body->as.block.body.len != 1)
      return false;
    body = body->as.block.body.data[0];
  }
  if (!body || body->kind != NY_S_VAR || body->as.var.is_decl ||
      body->as.var.names.len != 1 || body->as.var.exprs.len != 1 ||
      !body->as.var.names.data[0])
    return false;
  const char *name = body->as.var.names.data[0];
  const expr_t *sum = body->as.var.exprs.data[0];
  if (!sum || sum->kind != NY_E_BINARY || !sum->as.binary.op ||
      strcmp(sum->as.binary.op, "+") != 0 || !sum->as.binary.left ||
      sum->as.binary.left->kind != NY_E_IDENT ||
      !sum->as.binary.left->as.ident.name ||
      strcmp(sum->as.binary.left->as.ident.name, name) != 0 ||
      !sum->as.binary.right ||
      ((sum->as.binary.right->kind != NY_E_LITERAL ||
        sum->as.binary.right->as.literal.kind != NY_LIT_STR) &&
       (sum->as.binary.right->kind != NY_E_IDENT ||
        !sum->as.binary.right->as.ident.name ||
        strcmp(sum->as.binary.right->as.ident.name, name) == 0 ||
        !nyr_expr_is_cstr(b, sum->as.binary.right))))
    return false;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *decl = b->prog->body.data[i];
    if (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_decl ||
        !decl->as.var.is_mut || decl->as.var.names.len != 1 ||
        !decl->as.var.names.data[0] ||
        strcmp(decl->as.var.names.data[0], name) != 0 ||
        decl->as.var.exprs.len != 1 || !decl->as.var.exprs.data[0] ||
        decl->as.var.exprs.data[0]->kind != NY_E_LITERAL ||
        decl->as.var.exprs.data[0]->as.literal.kind != NY_LIT_STR)
      continue;
    const char *symbol = nyr_global_symbol(b, name);
    if (!symbol)
      return false;
    *name_out = name;
    *rhs_out = sum->as.binary.right;
    *symbol_out = symbol;
    return true;
  }
  return false;
}

/* Return a byte length only when the suffix is a literal or an immutable
 * binding initialized directly from one. This lets the builder append path
 * skip strlen without relying on mutable alias facts. */
static bool nyr_cstr_static_literal_len(nyr_builder_t *b, const expr_t *e,
                                        int64_t *len_out) {
  if (!b || !e || !len_out)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_STR) {
    if (e->as.literal.as.s.len > INT64_MAX)
      return false;
    *len_out = (int64_t)e->as.literal.as.s.len;
    return true;
  }
  if (e->kind != NY_E_IDENT || !e->as.ident.name || !b->prog)
    return false;
  /*
   * Resolve the actual local binding before considering a top-level
   * declaration. Immutable direct literals are safe to specialize; mutable
   * or aliased locals are not. This also preserves shadowing: never borrow a
   * same-named top-level literal for a local.
   */
  const nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
  if (local) {
    const expr_t *init = local->static_string_literal;
    if (!init || init->as.literal.as.s.len > INT64_MAX)
      return false;
    *len_out = (int64_t)init->as.literal.as.s.len;
    return true;
  }
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *decl = b->prog->body.data[i];
    if (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_decl ||
        decl->as.var.is_mut || decl->as.var.names.len != 1 ||
        !decl->as.var.names.data[0] ||
        strcmp(decl->as.var.names.data[0], e->as.ident.name) != 0 ||
        decl->as.var.exprs.len != 1 || !decl->as.var.exprs.data[0])
      continue;
    const expr_t *init = decl->as.var.exprs.data[0];
    if (init->kind != NY_E_LITERAL || init->as.literal.kind != NY_LIT_STR ||
        init->as.literal.as.s.len > INT64_MAX)
      return false;
    *len_out = (int64_t)init->as.literal.as.s.len;
    return true;
  }
  return false;
}

static bool nyr_lower_for_range(nyr_builder_t *b, const stmt_t *s) {
  const expr_t *lo_expr = NULL;
  const expr_t *hi_expr = NULL;
  if (!s->as.fr.iter_var ||
      !nyr_iterable_is_range(s->as.fr.iterable, &lo_expr, &hi_expr)) {
    return nyr_fail(
        b,
        "native NYIR lower: for loops currently support `for name in lo..hi` "
        "ranges at %s:%d",
        s->tok.filename ? s->tok.filename : "<source>", s->tok.line);
  }

  nyr_local_t *promoted = NULL;
  const expr_t *append_rhs = NULL;
  const char *promoted_global_name = NULL;
  const char *promoted_global_symbol = NULL;
  bool promoted_global = false;
  int64_t known_suffix_len = 0;
  bool known_suffix = false;
  bool repeat_append = false;
  int64_t repeat_count = 0;
  for (size_t n = b->local_count; n > 0; --n) {
    nyr_local_t *candidate = &b->locals[n - 1];
    if (candidate->is_cstr && candidate->sb_candidate && !candidate->is_sb &&
        nyr_self_concat_append(s->as.fr.body, candidate, b, &append_rhs)) {
      promoted = candidate;
      break;
    }
  }
  if (!promoted && b->profile_name && strcmp(b->profile_name, "rt_main") == 0 &&
      b->scope_depth == 0)
    promoted_global =
        nyr_global_self_concat_append(b, s->as.fr.body, &promoted_global_name,
                                      &append_rhs, &promoted_global_symbol);
  if (promoted || promoted_global) {
    int64_t lo_const = 0, hi_const = 0;
    known_suffix =
        nyr_cstr_static_literal_len(b, append_rhs, &known_suffix_len);
    if (known_suffix && !s->as.fr.iter_index_var &&
        nyr_const_i64_expr(lo_expr, &lo_const, 0) &&
        nyr_const_i64_expr(hi_expr, &hi_const, 0)) {
      __int128 trips =
          hi_const < lo_const ? 0 : (__int128)hi_const - (__int128)lo_const + 1;
      if (trips >= 0 && trips <= INT64_MAX) {
        repeat_append = true;
        repeat_count = (int64_t)trips;
      }
    }
  }
  int promoted_builder_slot = -1;
  if (promoted || promoted_global) {
    int initial;
    if (promoted) {
      initial = nyr_load_local_value(b, promoted->slot);
    } else {
      const expr_t *initial_expr =
          nyr_find_top_level_value(b, promoted_global_name);
      initial = initial_expr ? nyr_lower_expr(b, initial_expr) : -1;
    }
    int builder = initial < 0 ? -1
                              : nyr_emit_runtime_call(b, "rt_cstr_builder_new",
                                                      initial, -1, -1, 1, 0);
    if (builder < 0)
      return false;
    if (promoted) {
      promoted->sb_slot = b->next_local_slot++;
      promoted->is_sb = true;
      promoted->sb_candidate = false;
      if (!nyr_store_local_value(b, promoted->sb_slot, builder))
        return false;
    } else {
      promoted_builder_slot = b->next_local_slot++;
      if (!nyr_store_local_value(b, promoted_builder_slot, builder))
        return false;
    }
    if (repeat_append) {
      int builder_value = promoted
                              ? nyr_load_local_value(b, promoted->sb_slot)
                              : nyr_load_local_value(b, promoted_builder_slot);
      int suffix = nyr_lower_expr(b, append_rhs);
      int suffix_len = nyr_emit_const(b, known_suffix_len);
      int count = nyr_emit_const(b, repeat_count);
      if (builder_value < 0 || suffix < 0 || suffix_len < 0 || count < 0 ||
          nyr_emit_runtime_call4(b, "rt_cstr_builder_repeat_known",
                                 builder_value, suffix, suffix_len, count, -1,
                                 4, 0) < 0)
        return false;
    }
  }

  size_t loop_scope_mark = nyr_scope_mark(b);
  bool ok = true;
  /*
   * The repeat helper has already performed every source-level append.  This
   * specialization proves the induction variable is unobserved, so emitting
   * an otherwise-empty loop would only retain the full trip-count overhead.
   */
  if (!repeat_append) {
    bool discard_iter = strcmp(s->as.fr.iter_var, "_") == 0;
    nyr_local_t *iter =
        discard_iter ? NULL : nyr_bind_local(b, s->as.fr.iter_var);
    if (!iter && !discard_iter)
      return false;
    int iter_slot = iter ? iter->slot : nyr_temp_slot(b);
    if (iter_slot < 0)
      return false;
    /* A range loop induction variable is always a raw integer in NYIR.
     * Record that representation on the local itself so calls made from inside
     * the loop can normalize it correctly at tagged/dynamic ABI boundaries.
     * Without this fact, a typed generated lambda such as `fn(int x){...}`
     * receives raw odd loop values as if they were already tagged and halves
     * them while decoding its parameter. */
    if (iter) {
      iter->type_name = "int";
      iter->semantic_rep = NY_SEM_REP_RAW_INT;
    }
    nyr_local_t *index = NULL;
    if (s->as.fr.iter_index_var) {
      index = nyr_bind_local(b, s->as.fr.iter_index_var);
      if (!index)
        return false;
      index->type_name = "int";
      index->semantic_rep = NY_SEM_REP_RAW_INT;
    }

    int lo = repeat_append ? nyr_emit_const(b, 1) : nyr_lower_expr(b, lo_expr);
    int hi = repeat_append ? nyr_emit_const(b, 0) : nyr_lower_expr(b, hi_expr);
    int hi_slot = nyr_temp_slot(b);
    if (lo < 0 || hi < 0 || !nyr_store_local_value(b, iter_slot, lo) ||
        !nyr_store_local_value(b, hi_slot, hi))
      return false;
    if (index) {
      int zero = nyr_emit_const(b, 0);
      if (zero < 0 || !nyr_store_local_value(b, index->slot, zero))
        return false;
    }

    int head_label = b->next_label++;
    int update_label = b->next_label++;
    int body_label = b->next_label++;
    int end_label = b->next_label++;
    if (!nyr_emit_label(b, head_label))
      return false;

    int cur = nyr_load_local_value(b, iter_slot);
    int end = nyr_load_local_value(b, hi_slot);
    /*
     * `lo..hi` is an inclusive Nytrix range.  Native lowering must keep the
     * terminal iteration; using `<` made every native loop silently omit `hi`,
     * so optimized native output disagreed with both the interpreter and C.
     */
    int in_range = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = cur,
                                                      .b = end,
                                                      .cmp = NYIR_CMP_LE});
    if (in_range < 0 || !nyr_emit_br_if(b, in_range, body_label) ||
        !nyr_emit_br(b, end_label) || !nyr_emit_label(b, body_label))
      return false;

    size_t loop_i = b->loop_depth;
    if (!nyr_push_loop(b, head_label, update_label, end_label))
      return false;
    bool entry_return = b->emitted_return;
    int entry_last_value = b->last_value;
    b->emitted_return = false;
    bool body_ok;
    if (repeat_append) {
      body_ok = true;
    } else if (promoted || promoted_global) {
      int builder = promoted ? nyr_load_local_value(b, promoted->sb_slot)
                             : nyr_load_local_value(b, promoted_builder_slot);
      int suffix = nyr_lower_expr(b, append_rhs);
      int ignored;
      if (builder < 0 || suffix < 0) {
        ignored = -1;
      } else if (known_suffix) {
        int suffix_len = nyr_emit_const(b, known_suffix_len);
        ignored =
            suffix_len < 0
                ? -1
                : nyr_emit_runtime_call(b, "rt_cstr_builder_append_known",
                                        builder, suffix, suffix_len, 3, 0);
      } else {
        ignored = nyr_emit_runtime_call(b, "rt_cstr_builder_append", builder,
                                        suffix, -1, 2, 0);
      }
      body_ok = ignored >= 0;
    } else {
      body_ok = nyr_lower_scoped_body(b, s->as.fr.body);
    }
    b->loop_depth = loop_i;
    if (!body_ok)
      return false;

    b->emitted_return = false;
    if (!nyr_emit_label(b, update_label))
      return false;
    int one = nyr_emit_const(b, 1);
    cur = nyr_load_local_value(b, iter_slot);
    int next = one >= 0 && cur >= 0
                   ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                        .dst = -1,
                                                        .a = cur,
                                                        .b = one})
                   : -1;
    if (next < 0 || !nyr_store_local_value(b, iter_slot, next))
      return false;
    if (index) {
      int old_idx = nyr_load_local_value(b, index->slot);
      int next_idx =
          old_idx >= 0 ? nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                            .dst = -1,
                                                            .a = old_idx,
                                                            .b = one})
                       : -1;
      if (next_idx < 0 || !nyr_store_local_value(b, index->slot, next_idx))
        return false;
    }

    b->emitted_return = entry_return;
    b->last_value = entry_last_value;
    ok = nyr_emit_br(b, head_label) && nyr_emit_label(b, end_label);
    if (!ok)
      return false;
  }
  /*
   * The promoted value becomes an ordinary immutable C string before the
   * first post-loop read, scope exit, or control-flow merge.
   */
  if (ok && promoted) {
    int builder = nyr_load_local_value(b, promoted->sb_slot);
    int value = builder < 0
                    ? -1
                    : nyr_emit_runtime_call(b, "rt_cstr_builder_finalize",
                                            builder, -1, -1, 1, 0);
    if (value < 0 || !nyr_store_local_value(b, promoted->slot, value))
      ok = false;
    /*
     * C-string length metadata must describe the finalized buffer, not the
     * literal initializer retained before promotion.
     */
    if (ok) {
      int length = nyr_emit_runtime_call(b, "rt_cstr_len", value, -1, -1, 1, 0);
      int tag = length < 0 ? -1 : nyr_emit_const(b, 121);
      if (length < 0 || tag < 0 ||
          !nyr_store_local_value(b, promoted->dyn_str_len_slot, length) ||
          !nyr_store_local_value(b, promoted->dyn_tag_slot, tag))
        ok = false;
    }
    promoted->is_sb = false;
    promoted->sb_slot = -1;
  } else if (promoted_global) {
    int builder = nyr_load_local_value(b, promoted_builder_slot);
    int value = builder < 0
                    ? -1
                    : nyr_emit_runtime_call(b, "rt_cstr_builder_finalize",
                                            builder, -1, -1, 1, 0);
    int addr =
        value < 0 ? -1
                  : nyr_ir_emit(&b->nyir,
                                (nyr_inst_t){.op = NYIR_ADDR_SYMBOL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .imm = 0,
                                             .symbol = promoted_global_symbol});
    if (value < 0 || addr < 0 || !nyr_emit_store_i64(b, addr, value))
      ok = false;
  }
  nyr_scope_restore(b, loop_scope_mark);
  return true;
}
static bool nyr_lower_for_list(nyr_builder_t *b, const stmt_t *s) {
  bool is_str = nyr_expr_is_cstr(b, s->as.fr.iterable) ||
                (s->as.fr.iterable && s->as.fr.iterable->kind == NY_E_LITERAL &&
                 s->as.fr.iterable->as.literal.kind == NY_LIT_STR);
  bool is_dyn_list = !is_str && nyr_expr_is_dyn_list(b, s->as.fr.iterable);
  bool is_range = !is_str && nyr_expr_is_range(b, s->as.fr.iterable);
  if (!s->as.fr.iter_var || !s->as.fr.iterable ||
      (!nyr_expr_is_list(b, s->as.fr.iterable) &&
       !nyr_expr_is_dyn_list(b, s->as.fr.iterable) && !is_str && !is_range &&
       !nyr_expr_is_any(b, s->as.fr.iterable))) {
    return nyr_fail(
        b,
        "native NYIR lower: list iteration requires a list expression at %s:%d",
        s->tok.filename ? s->tok.filename : "<source>", s->tok.line);
  }
  int iterable = nyr_lower_expr(b, s->as.fr.iterable);
  int list_slot = nyr_temp_slot(b);
  int index_slot = nyr_temp_slot(b);
  if (iterable < 0 || list_slot < 0 || index_slot < 0 ||
      !nyr_store_local_value(b, list_slot, iterable))
    return false;
  int zero = nyr_emit_const(b, 0);
  if (zero < 0 || !nyr_store_local_value(b, index_slot, zero))
    return false;

  size_t loop_scope_mark = nyr_scope_mark(b);
  nyr_local_t *iter = nyr_bind_local(b, s->as.fr.iter_var);
  nyr_local_t *index = s->as.fr.iter_index_var
                           ? nyr_bind_local(b, s->as.fr.iter_index_var)
                           : NULL;
  if (!iter || (s->as.fr.iter_index_var && !index))
    return false;
  if (is_str) {
    iter->is_cstr = true;
    iter->semantic_rep = NY_SEM_REP_STRING;
  } else if (is_dyn_list) {
    /* Dynamic list elements are descriptor-backed and must cross the loop
     * binding as tagged values, just like an `any` return or callback arg. */
    iter->is_any = true;
    iter->semantic_rep = NY_SEM_REP_TAGGED_DYNAMIC;
  }
  int head_label = b->next_label++;
  int body_label = b->next_label++;
  int update_label = b->next_label++;
  int end_label = b->next_label++;
  if (!nyr_emit_label(b, head_label))
    return false;
  int list_value = nyr_load_local_value(b, list_slot);
  int current = nyr_load_local_value(b, index_slot);
  int length = list_value < 0
                   ? -1
                   : nyr_emit_runtime_call(b,
                                           is_str     ? "rt_cstr_len"
                                           : is_range ? "rt_range_len_raw"
                                                      : "rt_tbuf_len_raw",
                                           list_value, -1, -1, 1, 0);
  int in_range = length < 0 || current < 0
                     ? -1
                     : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                          .dst = -1,
                                                          .a = current,
                                                          .b = length,
                                                          .cmp = NYIR_CMP_LT});
  if (in_range < 0 || !nyr_emit_br_if(b, in_range, body_label) ||
      !nyr_emit_br(b, end_label) || !nyr_emit_label(b, body_label))
    return false;
  int value = nyr_emit_runtime_call(
      b,
      is_str     ? "rt_cstr_get_raw"
      : is_range ? "rt_range_index_read_raw"
                 : (is_dyn_list ? "rt_tbuf_get_any" : "rt_tbuf_get"),
      list_value, current, is_str || is_range ? -1 : nyr_emit_const(b, 0),
      is_str || is_range ? 2 : 3, 0);
  int bound_value = s->as.fr.iter_by_index ? current : value;
  if (value < 0 || bound_value < 0 ||
      !nyr_store_local_value(b, iter->slot, bound_value) ||
      (index && !nyr_store_local_value(b, index->slot, current)))
    return false;
  size_t loop_i = b->loop_depth;
  if (!nyr_push_loop(b, head_label, update_label, end_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok = nyr_lower_scoped_body(b, s->as.fr.body);
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = false;
  if (!nyr_emit_label(b, update_label))
    return false;
  current = nyr_load_local_value(b, index_slot);
  int one = nyr_emit_const(b, 1);
  int next = current < 0 || one < 0
                 ? -1
                 : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                      .dst = -1,
                                                      .a = current,
                                                      .b = one});
  if (next < 0 || !nyr_store_local_value(b, index_slot, next))
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  bool ok = nyr_emit_br(b, head_label) && nyr_emit_label(b, end_label);
  nyr_scope_restore(b, loop_scope_mark);
  return ok;
}

static bool nyr_lower_for(nyr_builder_t *b, const stmt_t *s) {
  if (s->as.fr.init || s->as.fr.cond || s->as.fr.update) {
    /* C-style `for (init; cond; update)` shares the while-loop CFG, but its
     * init binding must remain visible to both the body and update clause. */
    size_t scope_mark = nyr_scope_mark(b);
    if (s->as.fr.init && !nyr_lower_stmt(b, s->as.fr.init))
      return false;
    int head_label = b->next_label++;
    int update_label = b->next_label++;
    int body_label = b->next_label++;
    int end_label = b->next_label++;
    if (!nyr_emit_label(b, head_label))
      return false;
    int cond =
        s->as.fr.cond ? nyr_lower_expr(b, s->as.fr.cond) : nyr_emit_const(b, 1);
    if (cond < 0 || !nyr_emit_br_if(b, cond, body_label) ||
        !nyr_emit_br(b, end_label) || !nyr_emit_label(b, body_label))
      return false;
    size_t loop_i = b->loop_depth;
    if (!nyr_push_loop(b, head_label, update_label, end_label))
      return false;
    bool entry_return = b->emitted_return;
    int entry_last_value = b->last_value;
    b->emitted_return = false;
    bool body_ok = nyr_lower_scoped_body(b, s->as.fr.body);
    b->loop_depth = loop_i;
    if (!body_ok)
      return false;
    b->emitted_return = false;
    if (!nyr_emit_label(b, update_label))
      return false;
    if (s->as.fr.update && !nyr_lower_stmt(b, s->as.fr.update))
      return false;
    if (!b->emitted_return && !nyr_emit_br(b, head_label))
      return false;
    b->emitted_return = entry_return;
    b->last_value = entry_last_value;
    bool ok = nyr_emit_label(b, end_label);
    nyr_scope_restore(b, scope_mark);
    return ok;
  }
  const expr_t *lo = NULL;
  const expr_t *hi = NULL;
  if (nyr_iterable_is_range(s->as.fr.iterable, &lo, &hi))
    return nyr_lower_for_range(b, s);
  return nyr_lower_for_list(b, s);
}

static bool nyr_pattern_is_wildcard(const expr_t *pat) {
  return pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
         strcmp(pat->as.ident.name, "_") == 0;
}

static bool nyr_static_int(const expr_t *expr, int64_t *out) {
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
      return nyr_static_int(body->as.ret.value, out);
    if (body && body->kind == NY_S_EXPR)
      return nyr_static_int(body->as.expr.expr, out);
  }
  return false;
}

/* Return 1 with the selected arm, 2 for the default, or 0 if dynamic. */
static int nyr_static_match_arm(const stmt_t *s, size_t *selected_out) {
  int64_t value = 0;
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test ||
      s->as.match.test->kind != NY_E_COMPTIME ||
      !nyr_static_int(s->as.match.test, &value))
    return 0;
  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    const match_arm_t *arm = &s->as.match.arms.data[i];
    if (arm->guard)
      return 0;
    for (size_t p = 0; p < arm->patterns.len; ++p) {
      const expr_t *pat = arm->patterns.data[p];
      int64_t exact = 0, lo = 0, hi = 0;
      if (nyr_pattern_is_wildcard(pat)) {
        *selected_out = i;
        return 1;
      }
      if (nyr_static_int(pat, &exact) && exact == value) {
        *selected_out = i;
        return 1;
      }
      if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
          strcmp(pat->as.binary.op, "..") == 0 &&
          nyr_static_int(pat->as.binary.left, &lo) &&
          nyr_static_int(pat->as.binary.right, &hi) && value >= lo &&
          value <= hi) {
        *selected_out = i;
        return 1;
      }
    }
  }
  return 2;
}

static int nyr_emit_cmp(nyr_builder_t *b, int a, int rhs, nyr_cmp_t cmp) {
  int v = nyr_ir_emit(
      &b->nyir,
      (nyr_inst_t){
          .op = NYIR_CMP_I64, .dst = -1, .a = a, .b = rhs, .cmp = cmp});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int nyr_emit_bool_binop(nyr_builder_t *b, nyr_op_t op, int a, int rhs) {
  int v = nyr_ir_emit(&b->nyir,
                      (nyr_inst_t){.op = op, .dst = -1, .a = a, .b = rhs});
  if (v < 0)
    nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
  return v;
}

static int nyr_lower_match_pattern(nyr_builder_t *b, int test_value,
                                   const expr_t *pat, bool test_any) {
  if (nyr_pattern_is_wildcard(pat))
    return nyr_emit_const(b, 1);

  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    int lo = nyr_lower_expr(b, pat->as.binary.left);
    int hi = nyr_lower_expr(b, pat->as.binary.right);
    if (lo < 0 || hi < 0)
      return -1;
    if (nyr_expr_is_cstr(b, pat->as.binary.left) ||
        (pat->as.binary.left && pat->as.binary.left->kind == NY_E_LITERAL &&
         pat->as.binary.left->as.literal.kind == NY_LIT_STR)) {
      int cmp_lo =
          nyr_emit_runtime_call(b, "rt_cstr_cmp", test_value, lo, -1, 2, 0);
      int cmp_hi =
          nyr_emit_runtime_call(b, "rt_cstr_cmp", test_value, hi, -1, 2, 0);
      int zero = nyr_emit_const(b, 0);
      int ge = nyr_emit_cmp(b, cmp_lo, zero, NYIR_CMP_GE);
      int le = nyr_emit_cmp(b, cmp_hi, zero, NYIR_CMP_LE);
      return nyr_emit_bool_binop(b, NYIR_AND_I64, ge, le);
    }
    if (nyr_expr_is_f64(b, pat->as.binary.left) ||
        (pat->as.binary.left && pat->as.binary.left->kind == NY_E_LITERAL &&
         pat->as.binary.left->as.literal.kind == NY_LIT_FLOAT)) {
      int ge = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                  .dst = -1,
                                                  .a = test_value,
                                                  .b = lo,
                                                  .cmp = NYIR_CMP_GE});
      int le = nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                  .dst = -1,
                                                  .a = test_value,
                                                  .b = hi,
                                                  .cmp = NYIR_CMP_LE});
      return nyr_emit_bool_binop(b, NYIR_AND_I64, ge, le);
    }
    /* A dynamic match subject carries the tagged NyValue encoding.  Literal
     * arms box their constants before comparing; range arms must untag the
     * subject instead, since ordered compares need the raw integer. */
    int cmp_subject = test_value;
    if (test_any) {
      cmp_subject =
          nyr_emit_runtime_call(b, "rt_untag", test_value, -1, -1, 1, 0);
      if (cmp_subject < 0)
        return -1;
    }
    int ge = nyr_emit_cmp(b, cmp_subject, lo, NYIR_CMP_GE);
    int le = nyr_emit_cmp(b, cmp_subject, hi, NYIR_CMP_LE);
    if (ge < 0 || le < 0)
      return -1;
    return nyr_emit_bool_binop(b, NYIR_AND_I64, ge, le);
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
    return nyr_emit_runtime_call(
        b,
        strcmp(pat->as.call.callee->as.ident.name, "ok") == 0 ? "rt_is_ok"
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
        (nyr_find_enum_member(b, pat_callee_name, &adt_enum, &adt_item,
                              &adt_tag) ||
         (strrchr(pat_callee_name, '.') &&
          nyr_find_enum_member(b, strrchr(pat_callee_name, '.') + 1, &adt_enum,
                               &adt_item, &adt_tag)))) {
      if (adt_item && adt_item->fields.len > 0) {
        int actual_tag =
            nyr_emit_runtime_call(b, "rt_adt_tag", test_value, -1, -1, 1, 0);
        int expected_tag = nyr_emit_const(b, adt_tag);
        return nyr_emit_cmp(b, actual_tag, expected_tag, NYIR_CMP_EQ);
      }
    }
  }

  int rhs = nyr_lower_expr(b, pat);
  if (rhs < 0)
    return -1;
  /* A dynamic match value uses the boxed NyValue encoding. Box proven raw
   * integer patterns, including plain enum tags, before comparing them. */
  if (test_any &&
      (nyr_value_scalar_class(b, rhs) == NYIR_SCALAR_I64_RAW ||
       (pat && pat->kind == NY_E_LITERAL &&
        pat->as.literal.kind == NY_LIT_INT && pat->tok.kind != NY_T_NIL &&
        pat->as.literal.as.i < (INT64_C(1) << 62) &&
        pat->as.literal.as.i > -(INT64_C(1) << 62)))) {
    int one = nyr_emit_const(b, 1);
    int shifted = one < 0
                      ? -1
                      : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_SHL_I64,
                                                           .dst = -1,
                                                           .a = rhs,
                                                           .b = one});
    rhs = shifted < 0 ? -1 : nyr_emit_binop(b, NYIR_OR_I64, shifted, one);
    if (rhs < 0)
      return -1;
  }
  if (nyr_expr_is_cstr(b, pat) ||
      (pat && pat->kind == NY_E_LITERAL && pat->as.literal.kind == NY_LIT_STR))
    return nyr_emit_runtime_call(b, "rt_cstr_eq", test_value, rhs, -1, 2,
                                 NYIR_SCALAR_I64_RAW);
  if (pat && (pat->kind == NY_E_LIST || pat->kind == NY_E_TUPLE ||
              pat->kind == NY_E_DICT || pat->kind == NY_E_SET ||
              nyr_expr_is_list(b, pat) || nyr_expr_is_dyn_list(b, pat))) {
    int equal =
        nyr_emit_runtime_call(b, "rt_any_eq", test_value, rhs, -1, 2, 0);
    int true_imm = nyr_emit_const(b, NY_IMM_TRUE);
    return nyr_emit_cmp(b, equal, true_imm, NYIR_CMP_EQ);
  }
  if (nyr_expr_is_f64(b, pat) || (pat && pat->kind == NY_E_LITERAL &&
                                  pat->as.literal.kind == NY_LIT_FLOAT))
    return nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_F64,
                                              .dst = -1,
                                              .a = test_value,
                                              .b = rhs,
                                              .cmp = NYIR_CMP_EQ});
  return nyr_emit_cmp(b, test_value, rhs, NYIR_CMP_EQ);
}

static const char *nyr_result_payload_type(nyr_builder_t *b,
                                           const expr_t *test) {
  const char *type_name = test ? test->semantic.resolved_type_name : NULL;
  if (!type_name && test)
    type_name = test->semantic.hm_type_name;
  const stmt_t *fn = test ? test->semantic.canonical_callee_stmt : NULL;
  if ((!type_name || !*type_name) && test && test->kind == NY_E_CALL &&
      test->as.call.callee && test->as.call.callee->kind == NY_E_IDENT &&
      test->as.call.callee->as.ident.name)
    fn = fn ? fn
            : nyr_find_user_function(b, test->as.call.callee->as.ident.name);
  if ((!type_name || !*type_name) && fn && fn->kind == NY_S_FUNC)
    type_name = fn->as.fn.return_type;
  if (!type_name || strncmp(type_name, "Result<", 7) != 0)
    return NULL;
  const char *start = type_name + 7;
  unsigned depth = 0;
  const char *end = start;
  for (; *end; ++end) {
    if (*end == '<')
      ++depth;
    else if (*end == '>') {
      if (depth == 0)
        break;
      --depth;
    } else if (*end == ',' && depth == 0) {
      break;
    }
  }
  if (end == start)
    return NULL;
  char *payload_type = ny_strndup(start, (size_t)(end - start));
  if (!payload_type)
    return NULL;
  const char *owned_type = nyr_builder_own_string(b, payload_type);
  free(payload_type);
  return owned_type;
}

static bool nyr_bind_result_pattern(nyr_builder_t *b, int test_value,
                                    const char *payload_type,
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
    int payload = nyr_emit_runtime_call(b, "rt_result_unwrap_raw", test_value,
                                        -1, -1, 1, 0);
    if (payload < 0)
      return false;
    for (size_t i = 0; i < pat->as.call.args.len; ++i) {
      const expr_t *arg = pat->as.call.args.data[i].val;
      if (!arg || arg->kind != NY_E_IDENT || !arg->as.ident.name ||
          strcmp(arg->as.ident.name, "_") == 0)
        continue;
      nyr_local_t *local = nyr_bind_local(b, arg->as.ident.name);
      if (!local)
        return false;
      /* Result constructors preserve scalar payloads raw. Keep that ABI for
       * scalar arms, but retain the instantiated payload type for aggregates
       * so later member/index lowering uses the right representation. */
      if (payload_type) {
        local->type_name = payload_type;
        local->is_any = ny_native_type_name_is_any(payload_type);
        local->is_list = ny_native_type_name_is_list(payload_type);
        local->is_dict = ny_type_is(payload_type, "dict");
        local->is_bytes = ny_native_type_name_is_bytes(payload_type);
        local->is_cstr = ny_native_type_name_is_str(payload_type);
        local->is_bigint = ny_type_is(payload_type, "bigint");
        local->is_f64 = ny_native_type_name_is_f64(payload_type);
        local->is_f32 = ny_native_type_name_is_f32(payload_type);
        local->semantic_rep = ny_semantic_rep_for_type(payload_type);
      } else {
        local->is_any = false;
        local->semantic_rep = NY_SEM_REP_RAW_INT;
      }
      if (arg->semantic.resolved_type_name)
        local->type_name = arg->semantic.resolved_type_name;
      else if (payload_type)
        local->type_name = payload_type;
      if (local->type_name) {
        local->is_list = ny_native_type_name_is_list(local->type_name);
        local->is_dyn_list = ny_native_type_name_is_dyn_list(local->type_name);
        local->is_dict = ny_type_is(local->type_name, "dict");
        local->is_bytes = ny_native_type_name_is_bytes(local->type_name);
        local->is_any = ny_native_type_name_is_any(local->type_name);
        local->is_bigint = ny_type_is(local->type_name, "bigint");
        local->semantic_rep = nyr_rep_for_type(b, local->type_name);
        local->list_len_slot = -1;
      }
      if (!nyr_store_local_value(b, local->slot, payload))
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
    if (pat->as.memcall.target && pat->as.memcall.target->kind == NY_E_IDENT &&
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
      (nyr_find_enum_member(b, pat_callee_name, &adt_enum, &adt_item,
                            &adt_tag) ||
       (strrchr(pat_callee_name, '.') &&
        nyr_find_enum_member(b, strrchr(pat_callee_name, '.') + 1, &adt_enum,
                             &adt_item, &adt_tag)))) {
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
        int off = nyr_emit_const(b, (int64_t)(i * 8));
        int fptr = nyr_emit_binop(b, NYIR_ADD_I64, test_value, off);
        if (fptr < 0)
          return false;
        int val = nyr_ir_emit(
            &b->nyir, (nyr_inst_t){.op = NYIR_LOAD_I64, .dst = -1, .a = fptr});
        if (val < 0)
          return false;
        nyr_local_t *local = nyr_bind_local(b, arg->as.ident.name);
        if (!local || !nyr_store_local_value(b, local->slot, val))
          return false;
      }
      return true;
    }
  }

  return true;
}
static int nyr_lower_match_patterns(nyr_builder_t *b, int test_value,
                                    const match_arm_t *arm, bool test_any) {
  if (!arm || arm->patterns.len == 0)
    return nyr_emit_const(b, 0);
  int combined = -1;
  for (size_t i = 0; i < arm->patterns.len; ++i) {
    int cur =
        nyr_lower_match_pattern(b, test_value, arm->patterns.data[i], test_any);
    if (cur < 0)
      return -1;
    combined =
        combined < 0 ? cur : nyr_emit_bool_binop(b, NYIR_OR_I64, combined, cur);
    if (combined < 0)
      return -1;
  }
  return combined;
}

/*
 * A small, dense integer match can use a
 * comparison tree instead of testing every
 * arm in source order. Keep this
 * deliberately narrow: guarded arms,
 * ranges, duplicate literals, and sparse
 * values retain the general matcher below
 * so first-match semantics and arbitrary
 * pattern evaluation stay exact.
 */
typedef struct {
  int64_t value;
  size_t arm_index;
  int label;
} ny_native_dense_case_t;

static bool nyr_collect_dense_cases(const stmt_t *s,
                                    ny_native_dense_case_t *cases,
                                    size_t *count_out) {
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
    cases[pos] =
        (ny_native_dense_case_t){.value = value, .arm_index = i, .label = -1};
    ++count;
  }

  uint64_t span = (uint64_t)cases[count - 1].value - (uint64_t)cases[0].value;
  if (span > (uint64_t)count * 2u)
    return false;
  *count_out = count;
  return true;
}

static bool nyr_emit_dense_dispatch(nyr_builder_t *b,
                                    const ny_native_dense_case_t *cases,
                                    size_t lo, size_t hi, int test_value,
                                    int default_label) {
  if (lo > hi)
    return nyr_emit_br(b, default_label);
  size_t mid = lo + (hi - lo) / 2;
  int rhs = nyr_emit_const(b, cases[mid].value);
  if (rhs < 0)
    return false;
  if (lo == hi) {
    int equal = nyr_emit_cmp(b, test_value, rhs, NYIR_CMP_EQ);
    return equal >= 0 && nyr_emit_br_if(b, equal, cases[mid].label) &&
           nyr_emit_br(b, default_label);
  }

  int left_label = b->next_label++;
  int right_label = b->next_label++;
  int at_most = nyr_emit_cmp(b, test_value, rhs, NYIR_CMP_LE);
  if (at_most < 0 || !nyr_emit_br_if(b, at_most, left_label) ||
      !nyr_emit_br(b, right_label) || !nyr_emit_label(b, left_label) ||
      !nyr_emit_dense_dispatch(b, cases, lo, mid, test_value, default_label) ||
      !nyr_emit_label(b, right_label) ||
      !nyr_emit_dense_dispatch(b, cases, mid + 1, hi, test_value,
                               default_label))
    return false;
  return true;
}

/* A match's slot records the physical
 * carriers of its arm values. Preserve
 * unanimous raw integer producers before
 * conservative expression inference. */
static void nyr_stamp_match_join(nyr_builder_t *b, int slot, int value) {
  bool found = false;
  for (size_t i = 0; i < b->nyir.len; ++i) {
    const nyr_inst_t *inst = &b->nyir.data[i];
    if (inst->op != NYIR_STORE_LOCAL || inst->imm != slot)
      continue;
    nyr_scalar_class_t cls = nyr_value_scalar_class(b, inst->a);
    if (cls != NYIR_SCALAR_I64_RAW) {
      bool initial_zero = !found && cls == NYIR_SCALAR_UNKNOWN;
      if (initial_zero) {
        initial_zero = false;
        for (size_t j = 0; j < i; ++j)
          if (b->nyir.data[j].dst == inst->a &&
              b->nyir.data[j].op == NYIR_CONST_I64 && b->nyir.data[j].imm == 0)
            initial_zero = true;
      }
      if (!initial_zero)
        return;
    }
    found = true;
  }
  if (found && b->nyir.len && b->nyir.data[b->nyir.len - 1].dst == value)
    b->nyir.data[b->nyir.len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
}

static bool nyr_lower_dense_match(nyr_builder_t *b, const stmt_t *s,
                                  int test_value, int result_slot,
                                  int merge_label, bool entry_return,
                                  bool *lowered_out) {
  ny_native_dense_case_t cases[32];
  size_t count = 0;
  if (!nyr_collect_dense_cases(s, cases, &count)) {
    *lowered_out = false;
    return true;
  }

  int default_label = b->next_label++;
  int lower_label = b->next_label++;
  int dispatch_label = b->next_label++;
  for (size_t i = 0; i < count; ++i)
    cases[i].label = b->next_label++;

  int min_value = nyr_emit_const(b, cases[0].value);
  int max_value = nyr_emit_const(b, cases[count - 1].value);
  int at_least =
      min_value < 0 ? -1 : nyr_emit_cmp(b, test_value, min_value, NYIR_CMP_GE);
  int at_most =
      max_value < 0 ? -1 : nyr_emit_cmp(b, test_value, max_value, NYIR_CMP_LE);
  if (at_least < 0 || at_most < 0 ||
      !nyr_emit_br_if(b, at_least, lower_label) ||
      !nyr_emit_br(b, default_label) || !nyr_emit_label(b, lower_label) ||
      !nyr_emit_br_if(b, at_most, dispatch_label) ||
      !nyr_emit_br(b, default_label) || !nyr_emit_label(b, dispatch_label) ||
      !nyr_emit_dense_dispatch(b, cases, 0, count - 1, test_value,
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
    if (case_index == count || !nyr_emit_label(b, cases[case_index].label))
      return false;
    b->last_value = -1;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, s->as.match.arms.data[arm_index].conseq))
      return false;
    if (!b->emitted_return) {
      all_taken_paths_return = false;
      nyr_scalar_class_t result_class =
          b->last_value >= 0 ? nyr_value_scalar_class(b, b->last_value)
                             : NYIR_SCALAR_UNKNOWN;
      if (b->last_value >= 0 && (b->match_result_any || b->return_any) &&
          (result_class == NYIR_SCALAR_I64_RAW ||
           result_class == NYIR_SCALAR_F64 ||
           result_class == NYIR_SCALAR_F32)) {
        b->last_value = nyr_emit_runtime_call(b, "rt_tag", b->last_value, -1,
                                              -1, 1, NYIR_SCALAR_I64_TAGGED);
        if (b->last_value < 0)
          return false;
      }
      if (b->last_value >= 0 &&
          !nyr_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!nyr_emit_br(b, merge_label))
        return false;
    }
  }

  if (s->as.match.default_conseq) {
    if (!nyr_emit_label(b, default_label))
      return false;
    b->last_value = -1;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, s->as.match.default_conseq))
      return false;
    if (!b->emitted_return) {
      all_taken_paths_return = false;
      nyr_scalar_class_t result_class =
          b->last_value >= 0 ? nyr_value_scalar_class(b, b->last_value)
                             : NYIR_SCALAR_UNKNOWN;
      if (b->last_value >= 0 && (b->match_result_any || b->return_any) &&
          (result_class == NYIR_SCALAR_I64_RAW ||
           result_class == NYIR_SCALAR_F64 ||
           result_class == NYIR_SCALAR_F32)) {
        b->last_value = nyr_emit_runtime_call(b, "rt_tag", b->last_value, -1,
                                              -1, 1, NYIR_SCALAR_I64_TAGGED);
        if (b->last_value < 0)
          return false;
      }
      if (b->last_value >= 0 &&
          !nyr_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!nyr_emit_br(b, merge_label))
        return false;
    }
  } else {
    all_taken_paths_return = false;
    if (!nyr_emit_label(b, default_label) || !nyr_emit_br(b, merge_label))
      return false;
  }

  b->emitted_return = entry_return || all_taken_paths_return;
  if (!b->emitted_return) {
    if (!nyr_emit_label(b, merge_label))
      return false;
    int loaded = nyr_load_local_value(b, result_slot);
    if (loaded < 0)
      return false;
    nyr_stamp_match_join(b, result_slot, loaded);
    b->last_value = loaded;
  }
  *lowered_out = true;
  return true;
}

static bool nyr_match_arm_statically_dead(const expr_t *test,
                                          const match_arm_t *arm) {
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

/* A `case` expression feeding a dynamic
 * consumer must hand it the tagged ABI. Arm
 * bodies whose tail is an unboxed raw
 * scalar (int literal, native scalar
 * arithmetic over scalar operands, or a raw
 * local) are the producers this predicate
 * recognizes; pointers/strings/any-valued
 * tails already carry their ABI value and
 * must not be tagged a second time. */
static bool nyr_arm_tail_is_raw_scalar(nyr_builder_t *b, const stmt_t *body) {
  const stmt_t *s = body;
  if (s && s->kind == NY_S_BLOCK && s->as.block.body.len > 0)
    s = s->as.block.body.data[s->as.block.body.len - 1];
  if (!s)
    return false;
  const expr_t *e = NULL;
  if (s->kind == NY_S_EXPR)
    e = s->as.expr.expr;
  else if (s->kind == NY_S_RETURN)
    e = s->as.ret.value;
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL)
    return true;
  if (e->kind == NY_E_BINARY && e->as.binary.op && !nyr_expr_is_f64(b, e) &&
      !nyr_expr_is_f32(b, e) && !nyr_expr_is_any(b, e) &&
      !nyr_expr_is_bool(b, e))
    return true;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const nyr_local_t *l = nyr_find_local(b, e->as.ident.name);
    return l && !l->is_any && !l->is_cstr && !l->is_bytes && !l->is_dict &&
           !l->is_list && !l->is_bigint && !l->is_f64 && !l->is_f32;
  }
  return false;
}

static bool nyr_lower_match(nyr_builder_t *b, const stmt_t *s) {
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test)
    return nyr_fail(b, "native NYIR lower: "
                       "malformed case/match");

  size_t static_arm = 0;
  int static_selection = nyr_static_match_arm(s, &static_arm);
  if (static_selection == 1)
    return nyr_lower_scoped_body(b, s->as.match.arms.data[static_arm].conseq);
  if (static_selection == 2)
    return !s->as.match.default_conseq ||
           nyr_lower_scoped_body(b, s->as.match.default_conseq);

  int test_value = nyr_lower_expr(b, s->as.match.test);
  if (test_value < 0)
    return false;
  bool test_any =
      nyr_expr_is_any(b, s->as.match.test) ||
      nyr_value_scalar_class(b, test_value) == NYIR_SCALAR_I64_TAGGED;
  const char *result_payload_type =
      nyr_result_payload_type(b, s->as.match.test);

  int result_slot = nyr_temp_slot(b);
  int merge_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  bool all_taken_paths_return = true;
  bool wildcard_default = false;

  int initial = entry_last_value >= 0 ? entry_last_value : nyr_emit_const(b, 0);
  if (initial < 0 || !nyr_store_local_value(b, result_slot, initial))
    return false;
  bool dense_lowered = false;
  /* Dense dispatch compares raw integers. Tagged dynamic subjects must use
   * the general matcher so literal patterns retain their runtime tags. */
  if (!test_any &&
      !nyr_lower_dense_match(b, s, test_value, result_slot, merge_label,
                             entry_return, &dense_lowered))
    return false;
  if (dense_lowered)
    return true;

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    size_t arm_scope = nyr_scope_mark(b);
    if (nyr_match_arm_statically_dead(s->as.match.test, arm)) {
      nyr_scope_restore(b, arm_scope);
      continue;
    }
    int next_label = b->next_label++;
    bool has_wildcard = false;
    for (size_t p = 0; p < arm->patterns.len; ++p)
      has_wildcard |= nyr_pattern_is_wildcard(arm->patterns.data[p]);
    bool unconditional_wildcard = has_wildcard && !arm->guard;
    int pat = nyr_lower_match_patterns(b, test_value, arm, test_any);
    if (pat < 0)
      return false;
    /* Match predicates may be raw NYIR
     * booleans (0/1) or boxed runtime
     * booleans (NY_IMM_FALSE/NY_IMM_TRUE).
     * Branch through an explicit success
     * block so the shared branch normalizer
     * handles both forms without carrying a
     * negated value across the arm
     * boundary. */
    int pat_pass_label = b->next_label++;
    if (unconditional_wildcard) {
      if (!nyr_emit_br(b, pat_pass_label) || !nyr_emit_label(b, pat_pass_label))
        return false;
      wildcard_default = true;
    } else if (!nyr_emit_br_if(b, pat, pat_pass_label) ||
               !nyr_emit_br(b, next_label) ||
               !nyr_emit_label(b, pat_pass_label)) {
      return false;
    }

    if (!nyr_bind_result_pattern(b, test_value, result_payload_type,
                                 arm->patterns.len == 1 ? arm->patterns.data[0]
                                                        : NULL))
      return false;
    if (arm->guard) {
      int guard = nyr_lower_expr(b, arm->guard);
      int guard_pass_label = b->next_label++;
      if (guard < 0 || !nyr_emit_br_if(b, guard, guard_pass_label) ||
          !nyr_emit_br(b, next_label) || !nyr_emit_label(b, guard_pass_label))
        return false;
    }

    b->last_value = -1;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, arm->conseq))
      return false;
    bool arm_returns = b->emitted_return;
    if (!arm_returns) {
      all_taken_paths_return = false;
      nyr_scalar_class_t result_class =
          b->last_value >= 0 ? nyr_value_scalar_class(b, b->last_value)
                             : NYIR_SCALAR_UNKNOWN;
      if (b->last_value >= 0 && (b->match_result_any || b->return_any) &&
          (nyr_arm_tail_is_raw_scalar(b, arm->conseq) ||
           result_class == NYIR_SCALAR_F64 ||
           result_class == NYIR_SCALAR_F32)) {
        int boxed = nyr_emit_runtime_call(b, "rt_tag", b->last_value, -1, -1, 1,
                                          NYIR_SCALAR_I64_TAGGED);
        if (boxed < 0)
          return false;
        b->last_value = boxed;
      }
      if (b->last_value >= 0 &&
          !nyr_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!nyr_emit_br(b, merge_label))
        return false;
    }
    nyr_scope_restore(b, arm_scope);

    if (unconditional_wildcard)
      break;
    if (!nyr_emit_label(b, next_label))
      return false;
  }

  if (s->as.match.default_conseq) {
    b->last_value = -1;
    b->emitted_return = false;
    if (!nyr_lower_scoped_body(b, s->as.match.default_conseq))
      return false;
    bool default_returns = b->emitted_return;
    if (!default_returns) {
      all_taken_paths_return = false;
      nyr_scalar_class_t result_class =
          b->last_value >= 0 ? nyr_value_scalar_class(b, b->last_value)
                             : NYIR_SCALAR_UNKNOWN;
      if (b->last_value >= 0 && (b->match_result_any || b->return_any) &&
          (nyr_arm_tail_is_raw_scalar(b, s->as.match.default_conseq) ||
           result_class == NYIR_SCALAR_F64 ||
           result_class == NYIR_SCALAR_F32)) {
        int boxed = nyr_emit_runtime_call(b, "rt_tag", b->last_value, -1, -1, 1,
                                          NYIR_SCALAR_I64_TAGGED);
        if (boxed < 0)
          return false;
        b->last_value = boxed;
      }
      if (b->last_value >= 0 &&
          !nyr_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!nyr_emit_br(b, merge_label))
        return false;
    }
  } else if (!wildcard_default) {
    all_taken_paths_return = false;
  }

  b->emitted_return = entry_return || all_taken_paths_return;
  if (!b->emitted_return) {
    if (!nyr_emit_label(b, merge_label))
      return false;
    int loaded = nyr_load_local_value(b, result_slot);
    if (loaded < 0)
      return false;
    nyr_stamp_match_join(b, result_slot, loaded);
    b->last_value = loaded;
  }
  return true;
}

/*
 * Convert only a direct self-call in return
 * position and only when all parameters use
 * the scalar ABI.  Argument expressions are
 * evaluated before any parameter slot is
 * overwritten, preserving ordinary call
 * semantics.
 */
static int nyr_lower_tail_return(nyr_builder_t *b, const expr_t *value) {
  if (!b || !b->tail_recur_enabled || b->loop_depth != 0 ||
      b->defer_count != 0 || !value || value->kind != NY_E_CALL ||
      !value->as.call.callee || value->as.call.callee->kind != NY_E_IDENT ||
      !b->current_fn_name ||
      strcmp(value->as.call.callee->as.ident.name, b->current_fn_name) != 0 ||
      value->as.call.args.len != b->tail_param_count)
    return 0;
  size_t count = b->tail_param_count;
  int *args = count ? malloc(count * sizeof(*args)) : NULL;
  if (count && !args) {
    return nyr_fail(b, NY_NATIVE_ALLOC_FAIL), -1;
  }
  for (size_t i = 0; i < count; ++i) {
    args[i] = nyr_lower_expr(b, value->as.call.args.data[i].val);
    if (args[i] < 0) {
      free(args);
      return -1;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (!nyr_store_local_value(b, b->tail_param_slots[i], args[i])) {
      free(args);
      return -1;
    }
  }
  free(args);
  if (!nyr_emit_br(b, b->tail_loop_label))
    return -1;
  b->emitted_return = true;
  b->last_value = -1;
  return 1;
}

/* Prove that an expression lowers to a raw
 * i64 payload in NYIR: integer literals,
 * arithmetic/bitwise trees over proven
 * operands, ternaries whose arms are
 * proven, and idents bound to typed int
 * locals.  Index/get reads are deliberately
 * unproven here because their payload
 * follows the element representation, not
 * the static type. */
static bool nyr_expr_is_proven_raw_int(nyr_builder_t *b, const expr_t *e,
                                       unsigned depth) {
  if (!b || !e || depth > 6)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_INT && e->tok.kind != NY_T_NIL;
  if (e->semantic.resolved) {
    if (e->semantic.rep == NY_SEM_REP_RAW_INT)
      return true;
    if (e->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC ||
        e->semantic.rep == NY_SEM_REP_F64 || e->semantic.rep == NY_SEM_REP_F32)
      return false;
  }
  switch (e->kind) {
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    const nyr_local_t *local = nyr_find_local(b, e->as.ident.name);
    if (!local)
      return false;
    if (local->is_any || local->is_f64 || local->is_f32 || local->is_cstr ||
        local->is_list || local->is_dict)
      return false;
    if (local->semantic_rep == NY_SEM_REP_RAW_INT)
      return true;
    return local->type_name && ny_native_type_name_is_int(local->type_name);
  }
  case NY_E_TERNARY:
    return nyr_expr_is_proven_raw_int(b, e->as.ternary.true_expr, depth + 1) &&
           nyr_expr_is_proven_raw_int(b, e->as.ternary.false_expr, depth + 1);
  case NY_E_BINARY: {
    if (nyr_cmp(e->as.binary.op, NULL))
      return false;
    return nyr_expr_is_proven_raw_int(b, e->as.binary.left, depth + 1) &&
           nyr_expr_is_proven_raw_int(b, e->as.binary.right, depth + 1);
  }
  default:
    return false;
  }
}

static int nyr_normalize_return(nyr_builder_t *b, const expr_t *expr,
                                int value) {
  if (!b || value < 0)
    return value;
  if (!expr) {
    return value;
  }
  /* A tail local can leave scope before its
   * implicit return is normalized. In that
   * case its load may have unknown class
   * even though every write to the slot is
   * machine-f64. Recover that physical fact
   * before choosing the declared return
   * conversion, including when contextual
   * inference made an any return look like
   * a float return. */
  if (expr->kind == NY_E_IDENT && expr->as.ident.name &&
      !nyr_value_is_f64(b, value, 0)) {
    const nyr_local_t *local = nyr_find_local_nearest(b, expr->as.ident.name);
    const nyr_inst_t *value_def = nyr_reg_def_inst(b, value);
    int64_t slot = local ? local->slot
                   : value_def && value_def->op == NYIR_LOAD_LOCAL
                       ? value_def->imm
                       : -1;
    bool saw_store = false;
    bool all_f64_stores = slot >= 0;
    for (size_t si = 0; slot >= 0 && si < b->nyir.len; ++si) {
      const nyr_inst_t *store_inst = &b->nyir.data[si];
      if (store_inst->op != NYIR_STORE_LOCAL || store_inst->imm != slot)
        continue;
      saw_store = true;
      if (store_inst->scalar_class != NYIR_SCALAR_F64 &&
          nyr_value_scalar_class(b, store_inst->a) != NYIR_SCALAR_F64) {
        all_f64_stores = false;
        break;
      }
    }
    if (saw_store && all_f64_stores) {
      value =
          nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                             .dst = -1,
                                             .a = -1,
                                             .b = -1,
                                             .imm = slot,
                                             .scalar_class = NYIR_SCALAR_F64});
      if (value < 0)
        return -1;
    }
  }
  /* Nullable returns use the tagged nil
   * immediate at their ABI boundary. NYIR's
   * ordinary scalar literal lowering keeps
   * nil as raw zero for arithmetic
   * compatibility, but returning that zero
   * from `?T` makes `is_nil`
   * indistinguishable from integer zero. */
  if (expr->tok.kind == NY_T_NIL && b->return_type &&
      (b->return_type[0] == '?' || strchr(b->return_type, '?') != NULL))
    return nyr_emit_const(b, NY_IMM_NIL);
  /* Numeric operators can retain
   * machine-f64 form through a dynamic
   * local even in an integer-returning
   * helper (for example the unboxed
   * accumulator in str.parse_int). Convert
   * at the declared ABI boundary. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      nyr_value_is_f64(b, value, 0))
    return nyr_emit_runtime_call(b, "rt_f64_to_i64", value, -1, -1, 1,
                                 NYIR_SCALAR_I64_RAW);
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_RAW)
    return value;
  /* A call through an untyped/fnptr
   * callable yields the canonical tagged
   * word.  A declared raw-scalar return
   * must unbox it here, or the tagged
   * payload (raw 5 returned as tagged 11)
   * leaks as the function's result and
   * every later comparison shifts by the
   * tag bit. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED) {
    /*
     * `load_layout(...)` of a narrow field
     * is lowered by the dedicated special
     * case into an already-decoded raw
     * machine word (untag + sign-extend).
     * Its generic builtin declaration is
     * `any`, so the TAGGED_DYNAMIC stamp on
     * the call would make this boundary
     * decode the value a second time:
     * `TypeTextureInfo_load_index` returned
     * 1 where the stored field was 3 (odd
     * raw words halve).  The layout special
     * case owns the conversion.
     */
    bool layout_load_call =
        expr->kind == NY_E_CALL && expr->as.call.callee &&
        expr->as.call.callee->kind == NY_E_IDENT &&
        expr->as.call.callee->as.ident.name && expr->as.call.args.len == 3 &&
        strcmp(expr->as.call.callee->as.ident.name, "load_layout") == 0 &&
        expr->as.call.args.data[1].val &&
        expr->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        expr->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR &&
        expr->as.call.args.data[2].val &&
        expr->as.call.args.data[2].val->kind == NY_E_LITERAL &&
        expr->as.call.args.data[2].val->as.literal.kind == NY_LIT_STR;
    if (!layout_load_call)
      return nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
  }
  /* Captured lambda frames store scalar
   * environment slots in the raw NYIR ABI.
   * Calls through a closure are dynamic, so
   * a scalar returned directly from a
   * captured local must be boxed before it
   * leaves the lambda frame. */
  if (b->lambda_entry && b->lambda_entry->capture_count > 0 &&
      expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const nyr_local_t *local = nyr_find_local(b, expr->as.ident.name);
    if (local && local->is_capture && !local->is_any && !local->is_cstr &&
        !local->is_bytes && !local->is_dict && !local->is_list &&
        !local->is_bigint && !local->is_f64 && !local->is_f32)
      return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
  }
  /* A declared bool crosses the
   * user-function ABI as a dynamic boolean.
   * Comparisons and logical operators are
   * intentionally raw 0/1 inside NYIR, but
   * callers pass the result to dynamic
   * runtime predicates and assertions. Box
   * it once at the typed function boundary
   * so native and VM calls agree. */
  if (b->return_type && strcmp(b->return_type, "bool") == 0 &&
      nyr_expr_is_bool(b, expr))
    return nyr_box_bool(b, value);
  /* A typed f64 result uses the machine
   * floating return ABI.  Dynamic helpers
   * return a boxed Ny float, while a raw
   * integer expression is still an integer
   * value; normalize each representation
   * explicitly instead of bitcasting a
   * pointer or tagged word as IEEE bits. */
  if (b->return_type &&
      (ny_native_type_name_is_f64(b->return_type) ||
       nyr_rep_for_type(b, b->return_type) == NY_SEM_REP_F64)) {
    /* `load_layout` is typed by the
     * selected layout field.  Its native
     * lowering returns a real f64 for wide
     * floating fields, but the generic
     * builtin declaration is `any`, so
     * expression inference cannot identify
     * that result on its own.  Preserve the
     * floating ABI here instead of feeding
     * the f64 register through the
     * integer-to-f64 numeric cast. */
    bool layout_f64 = false;
    if (expr && expr->kind == NY_E_CALL && ny_native_call_leaf(expr) &&
        strcmp(ny_native_call_leaf(expr), "load_layout") == 0 &&
        expr->as.call.args.len == 3 && expr->as.call.args.data[1].val &&
        expr->as.call.args.data[1].val->kind == NY_E_LITERAL &&
        expr->as.call.args.data[1].val->as.literal.kind == NY_LIT_STR &&
        expr->as.call.args.data[2].val &&
        expr->as.call.args.data[2].val->kind == NY_E_LITERAL &&
        expr->as.call.args.data[2].val->as.literal.kind == NY_LIT_STR) {
      size_t field_size = 0;
      layout_f64 = nyr_ast_layout_query(
                       b, expr->as.call.args.data[1].val->as.literal.as.s.data,
                       expr->as.call.args.data[2].val->as.literal.as.s.data,
                       &field_size, NULL, NULL) &&
                   field_size >= 8;
    }
    if (layout_f64)
      return value;
    /* The expression's declared type can
     * describe the formal/contextual ABI
     * rather than the bits currently in the
     * NYIR value (notably an integer passed
     * through a `number` parameter).
     * Preserve only a value whose emitted
     * scalar class is actually floating
     * point; otherwise convert the raw
     * integer or tagged dynamic payload
     * below. */
    /* A dynamic accessor can inherit an f64 class from its fallback even
     * though the runtime ABI still returns a tagged value.  The producer's
     * canonical representation takes precedence at this boundary. */
    if (nyr_expr_is_canonical_dynamic_value(b, expr) ||
        nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_TAGGED)
      return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    if (nyr_value_is_f64(b, value, 0))
      return value;
    if (nyr_value_is_f32(b, value, 0))
      return nyr_emit_f32_to_f64(b, value);
    if (nyr_expr_is_any(b, expr))
      return nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                   NYIR_SCALAR_F64);
    return nyr_emit_i64_to_f64(b, value);
  }
  /*
   * A declared f32 return owns the
   * machine-f32 scalar ABI.  Unhinted float
   * literals and dynamic locals reach this
   * boundary classified f64, so the
   * boundary narrows them here instead of
   * shipping an f64 producer under an f32
   * ret contract.
   */
  if (b->return_type &&
      (ny_native_type_name_is_f32(b->return_type) ||
       nyr_rep_for_type(b, b->return_type) == NY_SEM_REP_F32)) {
    if (nyr_value_is_f32(b, value, 0))
      return value;
    if (nyr_value_is_f64(b, value, 0))
      return nyr_emit_f64_to_f32(b, value);
    if (nyr_expr_is_any(b, expr)) {
      int as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                         NYIR_SCALAR_F64);
      return as_f64 < 0 ? -1 : nyr_emit_f64_to_f32(b, as_f64);
    }
    int as_f64 = nyr_emit_i64_to_f64(b, value);
    return as_f64 < 0 ? -1 : nyr_emit_f64_to_f32(b, as_f64);
  }
  if (b->return_any) {
    nyr_scalar_class_t dynamic_return_class = nyr_value_scalar_class(b, value);
    if ((dynamic_return_class != NYIR_SCALAR_F64 &&
         dynamic_return_class != NYIR_SCALAR_F32) &&
        expr->kind == NY_E_IDENT && expr->as.ident.name) {
      const nyr_local_t *local = nyr_find_local_nearest(b, expr->as.ident.name);
      const nyr_inst_t *value_def = nyr_reg_def_inst(b, value);
      int64_t slot = local ? local->slot
                     : value_def && value_def->op == NYIR_LOAD_LOCAL
                         ? value_def->imm
                         : -1;
      bool saw_store = false;
      bool all_f64_stores = slot >= 0;
      for (size_t si = 0; slot >= 0 && si < b->nyir.len; ++si) {
        const nyr_inst_t *store_inst = &b->nyir.data[si];
        if (store_inst->op != NYIR_STORE_LOCAL || store_inst->imm != slot)
          continue;
        saw_store = true;
        if (store_inst->scalar_class != NYIR_SCALAR_F64 &&
            nyr_value_scalar_class(b, store_inst->a) != NYIR_SCALAR_F64) {
          all_f64_stores = false;
          break;
        }
      }
      if (saw_store && all_f64_stores) {
        value = nyr_ir_emit(&b->nyir,
                            (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .imm = slot,
                                         .scalar_class = NYIR_SCALAR_F64});
        if (value < 0)
          return -1;
        dynamic_return_class = NYIR_SCALAR_F64;
      }
    }
    if (nyr_value_is_tagged_dynamic_call(b, value) &&
        dynamic_return_class != NYIR_SCALAR_F64 &&
        dynamic_return_class != NYIR_SCALAR_F32)
      return value;
    /* Lists, tuples, and sets share the
     * typed-buffer machine-word carrier. An
     * `any` return transports that handle
     * unchanged; `rt_tag` is reserved for
     * scalar i64 payloads and corrupts the
     * allocation header pointer. */
    if (nyr_value_scalar_class(b, value) == NYIR_SCALAR_TYPED_BUFFER) {
      const char *return_type = nyr_expr_type_name(b, expr);
      ny_dbg_heuristic_event("native-abi",
                             "function=%s return=%s "
                             "producer=typed-buffer "
                             "decision=preserve-handle "
                             "through=any",
                             b->current_fn_name ? b->current_fn_name
                                                : "<entry>",
                             return_type ? return_type : "collection");
      return value;
    }
    /* String-typed native values may be raw
     * C-string pointers in NYIR. An `any`
     * return promises a canonical dynamic
     * value, so establish managed string
     * provenance here instead of leaking
     * that raw pointer through the return
     * register. Besides fixing callers that
     * pass the result to an untyped runtime
     * builtin, this keeps rt_is_str free to
     * reject arbitrary raw pointers.
     * rt_cstr_to_str is idempotent for
     * already-managed strings.
     */
    if (nyr_expr_is_cstr(b, expr))
      return nyr_emit_runtime_call(b, "rt_cstr_to_str", value, -1, -1, 1, 0);
    if (expr->kind == NY_E_TERNARY &&
        (nyr_expr_is_any(b, expr->as.ternary.true_expr) ||
         nyr_expr_is_canonical_dynamic_value(b, expr->as.ternary.true_expr) ||
         nyr_expr_is_any(b, expr->as.ternary.false_expr) ||
         nyr_expr_is_canonical_dynamic_value(b, expr->as.ternary.false_expr)))
      return value;
    /* Untyped functions expose the dynamic
     * tagged ABI. Literal integers are
     * still emitted as raw NYIR constants,
     * so box only this unambiguous
     * producer; calls and dynamic locals
     * already carry their ABI value. */
    if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_INT &&
        expr->tok.kind != NY_T_NIL)
      return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    /* Container reads use the
     * representation of their element, not
     * the representation of the enclosing
     * `any` function.  A raw integer read
     * must be boxed before returning
     * through the dynamic ABI; otherwise
     * the caller interprets payload 9 as
     * tagged integer 4. */
    if ((expr->kind == NY_E_INDEX ||
         (expr->kind == NY_E_MEMCALL && expr->as.memcall.name &&
          strcmp(expr->as.memcall.name, "get") == 0)) &&
        expr->semantic.resolved && expr->semantic.rep == NY_SEM_REP_RAW_INT &&
        !nyr_expr_is_canonical_dynamic_value(b, expr) &&
        !nyr_expr_is_f64(b, expr))
      return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    /* A proven scalar integer expression
     * leaves the frame as a raw payload
     * too: typed locals, integer
     * arithmetic, and ternaries over ints
     * all lower to raw NYIR words even when
     * the function returns `any`.  Box them
     * at this boundary, or the caller reads
     * payload 7 as tagged integer 3 (the
     * JSON decoder returned halved
     * integers).  A stale TAGGED_DYNAMIC
     * stamp means the payload is already
     * boxed, so those shapes are excluded
     * here; index/get reads have their own
     * branches above because their element
     * proof is the representation, not the
     * static type. */
    {
      /*
       * Contextual inference can stamp an
       * untyped function's arithmetic tail
       * as tagged even though its operator
       * lowered to a raw i64 (for example
       * `fn add(a, b) { a + b }`).  The
       * return ABI is dynamic, so determine
       * whether to box from the produced
       * NYIR value, not the potentially
       * stale expression stamp.
       */
      bool value_is_raw_scalar =
          nyr_value_scalar_class(b, value) == NYIR_SCALAR_I64_RAW;
      bool dynamic_read =
          expr->kind == NY_E_INDEX ||
          (expr->kind == NY_E_MEMCALL && expr->as.memcall.name &&
           strcmp(expr->as.memcall.name, "get") == 0);
      if (value_is_raw_scalar && !dynamic_read && !nyr_expr_is_bool(b, expr) &&
          !nyr_expr_is_bigint(b, expr) && !nyr_expr_is_bigfloat(b, expr) &&
          !nyr_expr_is_f64(b, expr) && !nyr_expr_is_f32(b, expr) &&
          !nyr_expr_is_cstr(b, expr) && !nyr_expr_is_ptr(b, expr) &&
          !nyr_expr_is_list(b, expr) && !nyr_expr_is_set(b, expr) &&
          !nyr_expr_is_dict(b, expr) && !nyr_expr_is_bytes(b, expr))
        return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    }
    if (expr->kind == NY_E_INDEX && expr->as.index.target &&
        expr->as.index.target->kind == NY_E_IDENT &&
        expr->as.index.target->as.ident.name) {
      const nyr_local_t *target_local =
          nyr_find_local(b, expr->as.index.target->as.ident.name);
      if (target_local && target_local->is_list && !target_local->is_any &&
          !nyr_expr_is_f64(b, expr) && !nyr_expr_is_f32(b, expr) &&
          !nyr_expr_is_cstr(b, expr) && !nyr_expr_is_list(b, expr) &&
          !nyr_expr_is_dict(b, expr) && !nyr_expr_is_bool(b, expr)) {
        /* The unparameterized list formal
         * is polymorphic at runtime.  Read
         * through the representation-aware
         * accessor so both raw integer
         * slots and descriptor/string slots
         * survive the any return ABI. */
        int target = nyr_lower_expr(b, expr->as.index.target);
        int index = nyr_lower_expr(b, expr->as.index.start);
        int fallback = nyr_emit_const(b, 0);
        if (target < 0 || index < 0 || fallback < 0)
          return -1;
        return nyr_emit_runtime_call(b, "rt_value_get_index_raw", target, index,
                                     fallback, 3, 0);
      }
    }
    nyr_scalar_class_t producer_class = nyr_value_scalar_class(b, value);
    if (nyr_expr_is_f64(b, expr) || nyr_expr_is_f32(b, expr) ||
        producer_class == NYIR_SCALAR_F64 ||
        producer_class == NYIR_SCALAR_F32) {
      /*
       * The classifier publishes f64 for a
       * float-typed operand even when the
       * emitted chain stayed in the dynamic
       * domain. Normalize the producer by
       * register class before taking its
       * IEEE bits: a tagged dynamic decodes
       * via rt_any_to_f64, a raw int
       * converts numerically, and only a
       * classified float goes straight to
       * rt_f64_bits.
       */
      int as_f64 = value;
      if (producer_class == NYIR_SCALAR_I64_TAGGED) {
        as_f64 = nyr_emit_runtime_call(b, "rt_any_to_f64", value, -1, -1, 1,
                                       NYIR_SCALAR_F64);
      } else if (producer_class == NYIR_SCALAR_I64_RAW) {
        as_f64 = nyr_emit_i64_to_f64(b, value);
      } else if (producer_class == NYIR_SCALAR_F32) {
        as_f64 = nyr_emit_f32_to_f64(b, value);
      }
      if (as_f64 < 0)
        return -1;
      int bits = nyr_emit_runtime_call(b, "rt_f64_bits", as_f64, -1, -1, 1, 0);
      return bits < 0 ? -1
                      : nyr_emit_runtime_call(b, "rt_flt_box_val", bits, -1, -1,
                                              1, 0);
    }
    /* Runtime calls with an explicitly raw
     * integer result are still dynamic
     * values when returned by an untyped
     * function.  Keep the conversion at
     * this ABI boundary so a stdlib wrapper
     * cannot leak a raw word that the
     * caller interprets as a
     * pointer/immediate. */
    if (expr->kind == NY_E_CALL) {
      const char *runtime_symbol = ny_native_runtime_symbol_for_expr(
          expr->as.call.callee && expr->as.call.callee->kind == NY_E_IDENT
              ? expr->as.call.callee->as.ident.name
              : NULL,
          ny_native_call_leaf(expr), expr);
      /* Explicit dynamic-to-raw conversions
       * produce a native scalar even though
       * their source declarations
       * intentionally use the compatibility
       * `any` surface.  An `any` function
       * must re-enter the tagged ABI before
       * returning; otherwise the caller's
       * normal dynamic decode consumes the
       * raw result a second time (for
       * example `to_int` turns 16 into 0).
       */
      /* Record-driven rule: a trailing CALL
       * whose ABI row reports a raw
       * int/word result has already been
       * decoded and must re-enter the
       * tagged ABI here; a row reporting a
       * tagged result is already tagged and
       * keeps the value as-is.  Only an
       * unrecorded symbol falls back to the
       * legacy name list below. */
      const ny_native_runtime_abi_decl_t *row =
          runtime_symbol ? ny_native_runtime_abi_decl(runtime_symbol) : NULL;
      if (row && (ny_native_runtime_abi_result_is_raw_int(row) ||
                  ny_native_runtime_abi_result_is_raw_word(row)))
        return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
      if (runtime_symbol && !row &&
          (strcmp(runtime_symbol, "rt_untag") == 0 ||
           strcmp(runtime_symbol, "rt_any_to_i64") == 0))
        return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
      if (runtime_symbol && !row &&
          strcmp(runtime_symbol, "rt_runtime_tag_raw_value") == 0)
        return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    }
    if (nyr_expr_is_bool(b, expr))
      return nyr_box_bool(b, value);
    /* Raw scalar arithmetic (int-typed
     * operands) lowers to native i64 ops;
     * the untyped ABI contract is a tagged
     * value, so box exactly once here.
     * Dynamic arithmetic (any operands)
     * already returns tagged from the
     * rt_any_* helper and is excluded, as
     * are floats, bools, and bitwise
     * results (raw two's-complement
     * domain).  A dynamic + cstr
     * concatenation resolves semantically
     * to `str`, so it is not `any` here,
     * but its lowering also routes through
     * rt_any_add and returns a canonical
     * dynamic handle; tagging that word a
     * second time corrupts the string into
     * a bogus tagged integer ("map
     * string"). */
    if (expr->kind == NY_E_BINARY && expr->as.binary.op &&
        !nyr_expr_is_f64(b, expr) && !nyr_expr_is_f32(b, expr) &&
        !nyr_expr_is_any(b, expr) && !nyr_expr_is_cstr(b, expr) &&
        !nyr_expr_is_ptr(b, expr) &&
        !nyr_is_bitwise_operator(expr->as.binary.op) &&
        !nyr_expr_is_bool(b, expr) &&
        /* A dynamic operand makes the
         * arithmetic helper produce the
         * canonical tagged result even when
         * HM classifies the whole binary
         * expression as an integer. Inspect
         * the operands before applying a
         * raw-result box; otherwise `fn(v)
         * { v + 1 }` becomes tagged(tagged
         * (2)) at the lambda return
         * boundary. */
        !nyr_expr_is_any(b, expr->as.binary.left) &&
        !nyr_expr_is_any(b, expr->as.binary.right))
      return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
      const nyr_local_t *local = nyr_find_local(b, expr->as.ident.name);
      if (local && !local->is_any && !local->is_cstr && !local->is_bytes &&
          !local->is_dict && !local->is_list && !nyr_expr_is_set(b, expr) &&
          !local->is_bigint && !local->is_f64 && !local->is_f32 &&
          !local->is_bool && local->semantic_rep != NY_SEM_REP_POINTER &&
          !ny_native_type_name_is_ptr(local->type_name))
        return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
      if (local && local->is_bool)
        return nyr_box_bool(b, value);
    }
    if (nyr_expr_is_bool(b, expr))
      return nyr_box_bool(b, value);
    /* Dynamic functions return canonical
     * tagged values.  Native member
     * accessors such as `x.len` produce a
     * raw i64 even when `x` is a
     * polymorphic sequence, so box that
     * scalar exactly once at the return
     * boundary. */
    if ((expr->kind == NY_E_MEMCALL || expr->kind == NY_E_MEMBER) &&
        !nyr_expr_is_any(b, expr) && !nyr_expr_is_cstr(b, expr) &&
        !nyr_expr_is_f64(b, expr) && !nyr_expr_is_f32(b, expr) &&
        !nyr_expr_is_list(b, expr) && !nyr_expr_is_set(b, expr) &&
        !nyr_expr_is_dict(b, expr) && !nyr_expr_is_bigint(b, expr) &&
        !nyr_expr_is_bigfloat(b, expr))
      return nyr_emit_runtime_call(b, "rt_tag", value, -1, -1, 1, 0);
    return value;
  }
  if (b->return_type && !ny_native_type_name_is_int(b->return_type))
    return value;
  /* An unparameterized `list` formal is
   * carried through the expanded
   * dynamic-sequence ABI.  Its indexed read
   * is canonical tagged-any even when the
   * enclosing function promises `int`;
   * decode that value at the function
   * boundary before the caller applies the
   * raw integer ABI.  Typed list<int>
   * locals are deliberately excluded
   * because their elem-8 accessor already
   * returns a raw payload. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      expr->kind == NY_E_INDEX && expr->as.index.target &&
      expr->as.index.target->kind == NY_E_IDENT &&
      expr->as.index.target->as.ident.name) {
    const nyr_local_t *target_local =
        nyr_find_local(b, expr->as.index.target->as.ident.name);
    bool dynamic_sequence =
        target_local && target_local->is_any &&
        ((target_local->type_name &&
          ny_native_type_name_is_list(target_local->type_name)) ||
         target_local->is_dyn_list);
    if (dynamic_sequence)
      return nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
  }
  /* A comptime expression is lowered into
   * native control flow/SSA values; its
   * result already follows the declared
   * scalar ABI.  In particular, `comptime
   * match` lowers table arms to raw i64, so
   * decoding the joined value as a tagged
   * dynamic integer halves odd results. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      expr->kind == NY_E_COMPTIME)
    return value;
  /* An explicitly integer-returning
   * function owns a raw i64 return ABI.  Do
   * not let a compatibility `any`
   * annotation on a nested native intrinsic
   * turn a fully scalar arithmetic/bitwise
   * tree into a tagged decode. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      nyr_expr_is_raw_scalar(b, expr, 0))
    return value;
  /* Arithmetic over dynamic values already
   * returns a raw native scalar from
   * rt_any_*; only an unmodified any local
   * still carries the tagged VM integer
   * encoding at this boundary. */
  bool dynamic = false;
  /* A `case`/match expression may
   * conservatively infer `any`, causing its
   * scalar arms to be boxed at the join. An
   * explicitly integer-returning function
   * owns a raw i64 ABI, so decode that
   * joined value exactly once. Without
   * this, `case c { 48..57 -> c - 48 }`
   * returns 1,3,... for 0,1,... and
   * corrupts consumers such as hexadecimal
   * decoding. */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      expr->kind == NY_E_MATCH && nyr_expr_is_any(b, expr))
    dynamic = true;
  /* Dictionary accessors expose tagged
   * values, unlike typed-buffer reads.
   * Decode that explicit ABI when returning
   * a declared integer.  The subscript
   * spelling mirrors .get:
   * expr_is_raw_dynamic_read already
   * distinguishes dictionary reads
   * (dynamic, decode) from typed-buffer
   * reads (raw slot payload, keep). */
  if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
      ((expr->kind == NY_E_MEMCALL && expr->as.memcall.name &&
        strcmp(expr->as.memcall.name, "get") == 0) ||
       expr->kind == NY_E_INDEX) &&
      !nyr_expr_is_raw_dynamic_read(b, expr))
    dynamic = true;
  /* Dynamic arithmetic produces a canonical
   * Ny value even when the enclosing
   * function has an explicit integer return
   * type.  Decode that value at the typed
   * return boundary; otherwise a dynamic
   * dictionary/list read is decoded to raw
   * operands, re-tagged by rt_any_add, and
   * leaks (for example 85 for the source
   * integer 42) through an `int` return.
   * The same mismatch exists for an UNTYPED
   * function whose inferred return fact
   * resolved to a raw scalar (`fn
   * fast_add(x, y) { x + y }`): the body
   * lowers the untyped parameters through
   * the dynamic triple ABI, so the tail is
   * the tagged rt_any_add result while the
   * signature promises raw — decode exactly
   * once or every caller comparing the
   * result against a literal sees the
   * encoded word (31 for 15). */
  if ((!b->return_type || ny_native_type_name_is_int(b->return_type)) &&
      !b->return_any && expr->kind == NY_E_BINARY &&
      (nyr_expr_is_any(b, expr) ||
       nyr_expr_is_canonical_dynamic_value(b, expr) ||
       nyr_expr_is_any(b, expr->as.binary.left) ||
       nyr_expr_is_any(b, expr->as.binary.right)) &&
      /* Arithmetic lowering can decode a
       * dynamic operand and finish with a
       * raw scalar even when HM keeps the
       * expression's broad `any` fact.
       * Trust the producer fact captured
       * for this exact tail expression so
       * an odd raw result is not untagged a
       * second time here. */
      !(b->last_arith_raw && b->last_arith_expr == expr))
    dynamic = true;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const nyr_local_t *local = nyr_find_local(b, expr->as.ident.name);
    /* A concrete local storage contract
     * outranks a stale expression-level
     * TAGGED_DYNAMIC fact.  Typed-buffer
     * reads (notably i64buf_load) can
     * acquire that stale fact from their
     * generic source annotation while their
     * lowered producer and local slot are
     * raw.  Decoding such a tail again
     * turns odd integers into half values
     * (rotate_left returned 1 instead of
     * 3), and the same mistake corrupts
     * typed crypto words. */
    bool local_raw_scalar = local && !local->is_any && !local->is_f64 &&
                            !local->is_f32 && !local->is_bool &&
                            (local->semantic_rep == NY_SEM_REP_RAW_INT ||
                             ny_native_type_name_is_int(local->type_name));
    if (local_raw_scalar) {
      dynamic = false;
    } else {
      dynamic = dynamic || (local && local->is_any);
      /* A local bound from a case
       * expression carries the tagged
       * result the match join produced
       * (inference falls back to any for
       * `case`, W2101); its declared-int
       * return context must decode exactly
       * once. */
      dynamic = dynamic ||
                (local && local->semantic_rep == NY_SEM_REP_TAGGED_DYNAMIC);
      /* The tail identifier's own semantic
       * is the authority on dynamic
       * provenance only when the local
       * table has no concrete carrier. */
      dynamic = dynamic || (expr->semantic.resolved &&
                            expr->semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC);
    }
  }
  /* Native container reads already return
   * their scalar slot payload in the raw
   * NYIR ABI.  Do not propagate their
   * source-level `any` annotation to a
   * function return: doing so unboxes raw
   * odd values a second time (notably
   * 32-bit words returned from a typed
   * buffer). */
  if (!dynamic)
    return value;
  return nyr_emit_runtime_call(b, "rt_any_to_i64", value, -1, -1, 1, 0);
}

static nyr_label_t *nyr_named_label(nyr_builder_t *b, const char *name) {
  if (!b || !name)
    return NULL;
  for (size_t i = 0; i < b->label_count; ++i)
    if (b->labels[i].name && strcmp(b->labels[i].name, name) == 0)
      return &b->labels[i];
  if (b->label_count == b->label_cap) {
    size_t cap = b->label_cap ? b->label_cap * 2 : 8;
    nyr_label_t *grown = realloc(b->labels, cap * sizeof(*grown));
    if (!grown)
      return NULL;
    b->labels = grown;
    b->label_cap = cap;
  }
  nyr_label_t *entry = &b->labels[b->label_count++];
  *entry = (nyr_label_t){.name = name, .label = b->next_label++};
  return entry;
}

static bool nyr_direct_thread_call(nyr_builder_t *b, const expr_t *e) {
  if (!b || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return false;
  const stmt_t *fn =
      nyr_find_user_function(b, e->as.call.callee->as.ident.name);
  return fn && nyr_fn_has_thread_attr(fn);
}

static bool nyr_is_main_guard(const stmt_t *s) {
  if (!s || s->kind != NY_S_IF || !s->as.iff.test ||
      s->as.iff.test->kind != NY_E_COMPTIME)
    return false;
  const stmt_t *body = s->as.iff.test->as.comptime_expr.body;
  if (body && body->kind == NY_S_BLOCK && body->as.block.body.len == 1)
    body = body->as.block.body.data[0];
  const expr_t *value =
      body && body->kind == NY_S_RETURN ? body->as.ret.value : NULL;
  return value && value->kind == NY_E_CALL && value->as.call.args.len == 0 &&
         value->as.call.callee && value->as.call.callee->kind == NY_E_IDENT &&
         value->as.call.callee->as.ident.name &&
         strcmp(value->as.call.callee->as.ident.name, "__main") == 0;
}

static bool nyr_lower_stmt_impl(nyr_builder_t *b, const stmt_t *s) {
  if (s && s->kind == NY_S_MODULE && b->profile_name &&
      strcmp(b->profile_name, "rt_main") == 0) {
    if (ny_native_stmt_is_stdlib(s))
      return true;
    char init_name[512];
    int n = snprintf(init_name, sizeof(init_name), "%s.__init__",
                     s->as.module.name ? s->as.module.name : "module");
    if (n < 0 || (size_t)n >= sizeof(init_name))
      return nyr_fail(b, "native NYIR: module "
                         "initializer name too long");
    const char *saved_name = b->current_fn_name;
    /* Global initialization is not an entry
     * expression. Its storage carrier must
     * not become the process result (for
     * example, a list address). */
    int saved_last_value = b->last_value;
    bool saved_last_raw = b->last_value_raw_scalar;
    bool saved_last_f64 = b->last_value_f64;
    b->current_fn_name = init_name;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *item = s->as.module.body.data[i];
      if (item && item->kind == NY_S_MODULE) {
        if (!nyr_lower_stmt(b, item)) {
          b->current_fn_name = saved_name;
          return false;
        }
      } else if (item && item->kind == NY_S_VAR && item->as.var.is_mut &&
                 !nyr_lower_stmt(b, item)) {
        b->current_fn_name = saved_name;
        return false;
      }
    }
    b->current_fn_name = saved_name;
    b->last_value = saved_last_value;
    b->last_value_raw_scalar = saved_last_raw;
    b->last_value_f64 = saved_last_f64;
    /* Only the root source's entry guard
     * executes; imported self-tests do not
     * participate in the program entry
     * path. */
    if (s->tok.filename && b->source_file &&
        nyr_same_source_file(s->tok.filename, b->source_file)) {
      const char *saved_module = b->module_name;
      b->module_name = s->as.module.name;
      b->current_fn_name = init_name;
      for (size_t i = 0; i < s->as.module.body.len; ++i) {
        const stmt_t *item = s->as.module.body.data[i];
        if (nyr_is_main_guard(item) && !nyr_lower_stmt(b, item)) {
          b->module_name = saved_module;
          b->current_fn_name = saved_name;
          return false;
        }
        if (b->emitted_return)
          break;
      }
      b->module_name = saved_module;
      b->current_fn_name = saved_name;
    }
    return true;
  }
  if (nyr_ignored_stmt(s) ||
      (s && (s->kind == NY_S_FUNC || s->kind == NY_S_LEMMA)))
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    /* The function body is the root scope.
     * Top-level rt_main declarations
     * intentionally use global storage,
     * while declarations in nested blocks
     * must receive lexical locals even when
     * their spelling matches a global
     * binding. */
    bool nested_scope = s != b->tail_body && !s->as.block.transparent;
    if (nested_scope)
      b->scope_depth++;
    size_t mark = s->as.block.transparent ? b->local_count : nyr_scope_mark(b);
    size_t defer_mark = b->defer_count;
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *item = s->as.block.body.data[i];
      if (i + 2 < s->as.block.body.len &&
          nyr_match_bounded_scalar_append(
              b, item, s->as.block.body.data[i + 1],
              s->as.block.body.data[i + 2], &(const char *){0},
              &(const char *){0}, &(const expr_t *){0}, &(const expr_t *){0})) {
        if (!nyr_lower_bounded_scalar_append(b, item,
                                             s->as.block.body.data[i + 1],
                                             s->as.block.body.data[i + 2])) {
          if (nested_scope)
            b->scope_depth--;
          if (!s->as.block.transparent)
            nyr_scope_restore(b, mark);
          return false;
        }
        i += 2;
        continue;
      }
      if (b->emitted_return && (!item || item->kind != NY_S_LABEL))
        continue;
      if (i + 1 == s->as.block.body.len && s == b->tail_body && item &&
          item->kind == NY_S_EXPR) {
        if (b->return_type && ny_native_type_name_is_int(b->return_type) &&
            nyr_expr_is_bigint(b, item->as.expr.expr))
          return nyr_fail(b, "implicit return value: "
                             "expected int, got bigint");
        int tail = nyr_lower_tail_return(b, item->as.expr.expr);
        if (tail < 0) {
          if (nested_scope)
            b->scope_depth--;
          if (!s->as.block.transparent)
            nyr_scope_restore(b, mark);
          return false;
        }
        if (tail > 0)
          break;
      }
      if (!nyr_lower_stmt(b, item)) {
        if (nested_scope)
          b->scope_depth--;
        if (!s->as.block.transparent)
          nyr_scope_restore(b, mark);
        return false;
      }
      /* Keep scanning so a later named
       * label can reopen a reachable block.
       */
    }
    if (!s->as.block.transparent && !b->capture_defers) {
      /*
       * Defer bodies lower as ordinary
       * statements and would clobber
       * last_value; the block's trailing
       * expression must win for implicit
       * returns.
       */
      int saved_last = b->last_value;
      bool saved_last_raw = b->last_value_raw_scalar;
      bool saved_last_f64 = b->last_value_f64;
      if ((!b->capture_defers || b->defer_capture_mark != defer_mark) &&
          !nyr_emit_defers(b, defer_mark)) {
        b->last_value = saved_last;
        b->last_value_raw_scalar = saved_last_raw;
        b->last_value_f64 = saved_last_f64;
        if (nested_scope)
          b->scope_depth--;
        return false;
      }
      b->last_value = saved_last;
      b->last_value_raw_scalar = saved_last_raw;
      b->last_value_f64 = saved_last_f64;
      nyr_scope_restore(b, mark);
    }
    if (nested_scope)
      b->scope_depth--;
    return true;
  }
  case NY_S_VAR:
    return nyr_lower_var(b, s);
  case NY_S_EXPR: {
    bool prev_thread_detach = b->thread_detach_stmt_call;
    if (nyr_direct_thread_call(b, s->as.expr.expr))
      b->thread_detach_stmt_call = true;
    int v = nyr_lower_expr(b, s->as.expr.expr);
    b->thread_detach_stmt_call = prev_thread_detach;
    if (v < 0)
      return false;
    b->last_value = v;
    /* Scope restoration removes the tail
     * local before build_function
     * normalizes an implicit return.
     * Capture its physical representation
     * while the binding is still visible.
     */
    nyr_scalar_class_t tail_class = nyr_value_scalar_class(b, v);
    b->last_value_raw_scalar = nyr_expr_is_raw_scalar(b, s->as.expr.expr, 0) ||
                               tail_class == NYIR_SCALAR_I64_RAW;
    b->last_value_f64 = nyr_expr_is_f64(b, s->as.expr.expr);
    return true;
  }
  case NY_S_IF:
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME) {
      bool selected = false;
      /* Platform guards must fold even when
       * the module builder carries no
       * options: target_eval_bool falls
       * back to the host OS/arch, which is
       * what the stdlib's #linux/#macos
       * guards rely on.  Without this,
       * probe_linux_library_once returned
       * [0, 0] and alsa.is_available() read
       * false on a Linux host (sound.ny:
       * "audio backend unavailable"). */
      bool eval_ok =
          nyr_target_eval_bool(b->options, s->as.iff.test, &selected);
      if (eval_ok) {
        if (s->as.iff.init && !nyr_lower_stmt(b, s->as.iff.init))
          return false;
        return nyr_lower_stmt(b, selected ? s->as.iff.conseq : s->as.iff.alt);
      }
    }
    return nyr_lower_if(b, s);
  case NY_S_GUARD:
    return nyr_lower_guard(b, s);
  case NY_S_WHILE:
    return nyr_lower_while(b, s);
  case NY_S_FOR:
    return nyr_lower_for(b, s);
  case NY_S_TRY:
    /*
     * Keep native try/catch ABI-compatible
     * with the LLVM path.  The runtime owns
     * the panic stack, while setjmp itself
     * must execute in this native frame so
     * rt_panic can longjmp back here
     * safely.  All values that must survive
     * the call live in NYIR locals/stack
     * slots, matching the normal native
     * calling convention.
     */
    {
      int jmpbuf = nyr_ir_emit(
          &b->nyir, (nyr_inst_t){.op = NYIR_ALLOCA,
                                 .dst = -1,
                                 .a = -1,
                                 .b = -1,
                                 .c = -1,
                                 .imm = (int64_t)sizeof(jmp_buf) + 16});
      int set_env = jmpbuf < 0 ? -1
                               : nyr_emit_runtime_call(b, "rt_set_panic_env",
                                                       jmpbuf, -1, -1, 1, 0);
      int sj = set_env < 0
                   ? -1
                   : nyr_emit_runtime_call(b, "_setjmp", jmpbuf, -1, -1, 1, 0);
      int zero = sj < 0 ? -1 : nyr_emit_const(b, 0);
      int resumed =
          sj < 0 || zero < 0
              ? -1
              : nyr_ir_emit(&b->nyir, (nyr_inst_t){.op = NYIR_CMP_I64,
                                                   .dst = -1,
                                                   .a = sj,
                                                   .b = zero,
                                                   .cmp = NYIR_CMP_EQ});
      int try_label = b->next_label++;
      int catch_label = b->next_label++;
      int end_label = b->next_label++;
      int result_slot = nyr_temp_slot(b);
      if (jmpbuf < 0 || set_env < 0 || sj < 0 || zero < 0 || resumed < 0 ||
          result_slot < 0 || !nyr_emit_br_if(b, resumed, try_label) ||
          !nyr_emit_br(b, catch_label) || !nyr_emit_label(b, try_label))
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
      if (s->as.tr.body && !nyr_lower_stmt(b, s->as.tr.body))
        return false;
      bool body_return = b->emitted_return;
      int body_value = b->last_value;
      const expr_t *body_expr = nyr_stmt_tail_expr(s->as.tr.body);
      size_t captured_count = b->defer_count - try_defer_mark;
      stmt_t **captured_defers = NULL;
      if (captured_count) {
        captured_defers = malloc(captured_count * sizeof(*captured_defers));
        if (!captured_defers) {
          b->capture_defers = saved_capture_defers;
          b->defer_capture_mark = saved_capture_mark;
          return nyr_fail(b, NY_NATIVE_ALLOC_FAIL);
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
        body_value = nyr_try_normalize_value(b, body_expr, body_value);
        if (body_value < 0 ||
            !nyr_store_local_value(b, result_slot, body_value))
          goto try_fail;
        for (size_t i = captured_count; i > 0; --i)
          if (nyr_emit_runtime_call(b, "rt_pop_run_defer", -1, -1, -1, 0, 0) <
              0)
            goto try_fail;
        if (nyr_emit_runtime_call(b, "rt_clear_panic_env", -1, -1, -1, 0, 0) <
                0 ||
            !nyr_emit_br(b, end_label))
          goto try_fail;
      }

      b->emitted_return = false;
      if (!nyr_emit_label(b, catch_label))
        goto try_fail;
      /* rt_panic drains this frame's
       * registrations before longjmp;
       * cleanup bodies must not be lowered
       * a second time in the catch arm. */
      /* Cleanup must run while the panic
       * environment is still active.  A
       * defer may update globals, release
       * an owned value, or itself report a
       * secondary failure; clearing the
       * environment first made the panic
       * path observably skip the original
       * resource cleanup in native code. */
      if (nyr_emit_runtime_call(b, "rt_clear_panic_env", -1, -1, -1, 0, 0) < 0)
        goto try_fail;
      b->last_value = -1;
      int error =
          nyr_emit_runtime_call(b, "rt_get_panic_val", -1, -1, -1, 0, 0);
      if (error < 0)
        return false;
      if (s->as.tr.err && s->as.tr.err[0] && strcmp(s->as.tr.err, "_") != 0) {
        nyr_local_t *err_local = nyr_bind_local(b, s->as.tr.err);
        if (err_local)
          err_local->is_any = true;
        if (!err_local || !nyr_store_local_value(b, err_local->slot, error))
          return false;
      }
      if (s->as.tr.handler && !nyr_lower_stmt(b, s->as.tr.handler))
        return false;
      bool handler_return = b->emitted_return;
      int handler_value = b->last_value;
      const expr_t *handler_expr = nyr_stmt_tail_expr(s->as.tr.handler);
      if (!handler_return) {
        if (handler_value < 0)
          handler_value = zero;
        handler_value = nyr_try_normalize_value(b, handler_expr, handler_value);
        if (handler_value < 0 ||
            !nyr_store_local_value(b, result_slot, handler_value) ||
            !nyr_emit_br(b, end_label))
          return false;
      }
      if (body_return && handler_return)
        b->emitted_return = true;
      else {
        b->emitted_return = entry_return;
        if (!nyr_emit_label(b, end_label))
          return false;
        /* A try statement is an
         * expression-like statement in the
         * AST.  The arms have already
         * written their value to the join
         * slot, so the merge is independent
         * of labels introduced inside
         * either arm. */
        if (!body_return && !handler_return) {
          b->last_value = nyr_load_local_value(b, result_slot);
          if (b->last_value < 0)
            return false;
        } else if (!body_return) {
          b->last_value = nyr_load_local_value(b, result_slot);
          if (b->last_value < 0)
            return false;
        } else if (!handler_return) {
          b->last_value = nyr_load_local_value(b, result_slot);
          if (b->last_value < 0)
            return false;
        }
      }
      nyr_scope_restore(b, try_scope_mark);
      free(captured_defers);
      return true;
    try_fail:
      nyr_scope_restore(b, try_scope_mark);
      free(captured_defers);
      return false;
    }
  case NY_S_MATCH:
    return nyr_lower_match(b, s);
  case NY_S_DEFER:
    return nyr_register_defer(b, s->as.de.body);
  case NY_S_BREAK:
    if (b->loop_depth == 0)
      return nyr_fail(b, "native NYIR lower: "
                         "break outside loop");
    if (!nyr_emit_defers(b, b->loop_frames[b->loop_depth - 1].defer_mark))
      return false;
    b->emitted_return = true;
    return nyr_emit_br(b, b->loop_frames[b->loop_depth - 1].end_label);
  case NY_S_CONTINUE:
    if (b->loop_depth == 0)
      return nyr_fail(b, "native NYIR lower: continue "
                         "outside loop");
    if (!nyr_emit_defers(b, b->loop_frames[b->loop_depth - 1].defer_mark))
      return false;
    b->emitted_return = true;
    return nyr_emit_br(b, b->loop_frames[b->loop_depth - 1].continue_label);
  case NY_S_RETURN: {
    int tail = s->as.ret.value ? nyr_lower_tail_return(b, s->as.ret.value) : 0;
    if (tail < 0)
      return false;
    if (tail > 0)
      return true;
    int v = s->as.ret.value ? nyr_lower_expr(b, s->as.ret.value)
                            : nyr_emit_const(b, 0);
    v = nyr_normalize_return(b, s->as.ret.value, v);
    if (v < 0)
      return false;
    if (!nyr_emit_defers(b, 0))
      return false;
    return nyr_emit_ret(b, v);
  }
  case NY_S_LABEL: {
    nyr_label_t *label = nyr_named_label(b, s->as.label.name);
    if (!label)
      return nyr_fail(b,
                      "native NYIR lower: cannot "
                      "allocate label '%s'",
                      s->as.label.name ? s->as.label.name : "");
    if (label->emitted)
      return nyr_fail(b,
                      "native NYIR lower: duplicate "
                      "label '%s'",
                      s->as.label.name);
    label->emitted = true;
    b->emitted_return = false;
    return nyr_emit_label(b, label->label);
  }
  case NY_S_GOTO: {
    nyr_label_t *label = nyr_named_label(b, s->as.go.name);
    if (!label)
      return nyr_fail(b,
                      "native NYIR lower: cannot "
                      "allocate goto '%s'",
                      s->as.go.name ? s->as.go.name : "");
    if (!nyr_emit_defers(b, 0) || !nyr_emit_br(b, label->label))
      return false;
    b->emitted_return = true;
    b->last_value = -1;
    return true;
  }
  default:
    return nyr_fail(b,
                    "native NYIR lower: statement kind "
                    "%d is not in shared NYIR yet",
                    (int)s->kind);
  }
}

static bool nyr_register_defer(nyr_builder_t *b, const stmt_t *body) {
  if (!b || !body)
    return true;
  ny_native_lambda_entry_t *entry =
      ny_native_lambda_find_defer(ny_native_lambda_cache_for(b), body);
  if (!entry || !entry->expr)
    return nyr_fail(b, "native NYIR: defer closure was "
                       "not collected");
  /* Materialize captures from the active
   * frame. Global cells are deliberately
   * excluded: deferred writes to globals
   * must remain shared with the caller,
   * while lexical locals receive a value
   * snapshot per registration. */
  for (size_t i = 0; i < b->local_count; ++i) {
    nyr_local_t *local = &b->locals[i];
    if (!local->name || !nyr_stmt_uses_ident(body, local->name))
      continue;
    if (!ny_native_lambda_capture_add(entry, local->name))
      return nyr_fail(b, "native NYIR: defer capture "
                         "table is full");
  }
  for (size_t i = 0; i < entry->capture_count; ++i) {
    nyr_local_t *local = nyr_find_local(b, entry->capture_names[i]);
    if (local)
      entry->capture_types[i] = *local;
  }
  int closure = nyr_lower_expr(b, entry->expr);
  if (closure < 0)
    return false;
  int fn = closure;
  int env = nyr_emit_const(b, 0);
  if (entry->capture_count > 0) {
    int zero = nyr_emit_const(b, 0);
    int eight = nyr_emit_const(b, 8);
    int code_addr = zero < 0 ? -1 : nyr_emit_add_i64(b, closure, zero);
    int env_addr = eight < 0 ? -1 : nyr_emit_add_i64(b, closure, eight);
    fn = code_addr < 0 ? -1 : nyr_emit_load_i64(b, code_addr);
    env = env_addr < 0 ? -1 : nyr_emit_load_i64(b, env_addr);
  }
  if (fn < 0 || env < 0 ||
      nyr_emit_runtime_call(b, "rt_push_defer", fn, env, -1, 2, 0) < 0)
    return false;
  return nyr_push_defer(b, (stmt_t *)body);
}

static const char *nyr_trace_function_name;
static size_t nyr_trace_statement_seq;
static unsigned long long nyr_build_rss_kib(void);

static bool nyr_lower_stmt(nyr_builder_t *b, const stmt_t *s) {
  /* The function-level trace gate and filter are already resolved once in
   * nyr_build_function. Avoid re-reading the environment for every nested
   * statement; this wrapper is on the hottest recursive lowering path. */
  const bool trace_stmt = b && s && nyr_trace_function_name;
  const char *trace_name = trace_stmt ? nyr_trace_function_name : "-";
  const size_t trace_seq = trace_stmt ? nyr_trace_statement_seq++ : 0;
  size_t trace_inst_before = trace_stmt ? b->nyir.len : 0;
  if (trace_stmt) {
    fprintf(stderr,
            "[resource] lower-stmt.begin fn=%s "
            "seq=%zu kind=%d at=%s:%d:%d "
            "inst=%zu rss=%lluMiB\n",
            trace_name, trace_seq, s->kind,
            s->tok.filename ? s->tok.filename : "<source>", s->tok.line,
            s->tok.col, b->nyir.len, nyr_build_rss_kib() / 1024ULL);
    fflush(stderr);
  }
  uint32_t prev_ctx = b ? b->current_syntax_ctx : 0;
  if (b && s && s->syntax_ctx != 0)
    b->current_syntax_ctx = s->syntax_ctx;
  bool ok = nyr_lower_stmt_impl(b, s);
  if (!ok && b && b->err && b->err_len > 0 && b->err[0] == '\0') {
    if (s && s->tok.filename) {
      snprintf(b->err, b->err_len,
               "native NYIR: failed to lower "
               "statement kind %d at %s:%d:%d",
               s->kind, s->tok.filename, s->tok.line, s->tok.col);
    } else if (s) {
      snprintf(b->err, b->err_len,
               "native NYIR: failed to lower "
               "statement kind %d at line %d:%d",
               s->kind, s->tok.line, s->tok.col);
    } else {
      snprintf(b->err, b->err_len,
               "native NYIR: failed to "
               "lower null statement");
    }
  }
  if (b)
    b->current_syntax_ctx = prev_ctx;
  if (trace_stmt) {
    fprintf(stderr,
            "[resource] lower-stmt.%s fn=%s "
            "seq=%zu kind=%d inst=%zu->%zu "
            "rss=%lluMiB\n",
            ok ? "complete" : "failed", trace_name, trace_seq, s->kind,
            trace_inst_before, b->nyir.len, nyr_build_rss_kib() / 1024ULL);
    fflush(stderr);
  }
  return ok;
}

/* Branch joins can make a local-store
 * value's float class visible only after
 * the complete function has been lowered.
 * Resolve those still-unknown store facts
 * from the NYIR producer map before the
 * first verifier boundary. */
static bool nyr_resolve_local_store_classes(nyr_func_t *f, char *err,
                                            size_t err_len) {
  nyr_type_map_t types = {0};
  if (!nyr_type_map_init(&types, f, nyr_max_local(f))) {
    if (err && err_len)
      snprintf(err, err_len,
               "native NYIR: cannot build "
               "local-store scalar type map");
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_STORE_LOCAL || in->scalar_class != NYIR_SCALAR_UNKNOWN ||
        in->a < 0 || (size_t)in->a >= types.value_count)
      continue;
    bool f64 = types.value_f64[in->a];
    bool f32 = types.value_f32[in->a];
    if (f64 == f32)
      continue;
    if (in->scalar_class == (f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32))
      continue;
    in->scalar_class = f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_F32;
    if (ny_trace_enabled("NY_TRACE_ABI"))
      ny_trace_line("[abi-local-store]",
                    "resolve inst=%zu value=v%d "
                    "class=%s from "
                    "whole-function producer facts",
                    i, in->a, f64 ? "f64" : "f32");
  }
  nyr_type_map_free(&types);
  return true;
}

/*
 * Shared NYIR optimization + verification
 * step.  After calling this the builder's
 * NYIR is ready for codegen or diagnostics.
 */
static bool nyr_finalize(nyr_builder_t *b, char *err, size_t err_len) {
  nyr_opt_stats_t stats;
  bool trace_context = ny_dbg_is_enabled(NY_DBG_PASS);
  bool optimized;
  if (b->options && (b->options->native_backend == NY_NATIVE_BACKEND_X86_64 ||
                     b->options->native_backend == NY_NATIVE_BACKEND_AARCH64))
    nyr_set_preserve_phis(true);
  if (!nyr_resolve_local_store_classes(&b->nyir, err, err_len))
    goto fail;
  /*
   * Initial lowering is a verifier boundary
   * too: no optimization pass should have
   * to defend itself against malformed
   * values, labels, effects, or CFG
   * structure produced upstream.
   */
  if (!nyr_ir_verify(&b->nyir, err, err_len))
    goto fail;
  ny_native_profile_select(b->profile_name ? b->profile_name : "rt_main");
  if (trace_context)
    ny_dbg_enter_context(b->current_fn_name ? b->current_fn_name
                                            : "<anonymous>");
  const char *trace_fn =
      b->current_fn_name ? b->current_fn_name : "<anonymous>";
  if (ny_trace_enabled("NY_TRACE_O3") && b->opt_level >= 3 &&
      ny_dbg_function_selected(trace_fn))
    optimized = nyr_optimize_debug(&b->nyir, stderr, &stats, b->opt_level);
  else
    optimized = nyr_optimize_with_stats(&b->nyir, &stats, b->opt_level);
  if (trace_context)
    ny_dbg_exit_context(b->current_fn_name ? b->current_fn_name
                                           : "<anonymous>");
  if (!optimized) {
    if (verbose_enabled >= 1)
      fprintf(stderr, "nyir opt FAILED\n");
    if (err && err_len > 0 && err[0] == '\0')
      snprintf(err, err_len,
               "native NYIR: optimization "
               "failed");
    goto fail;
  }
  if (!nyr_ir_verify(&b->nyir, err, err_len))
    goto fail;
  if (verbose_enabled >= 1 && stats.total_time_ms > 0.001) {
    bool grew = stats.after_insts > stats.before_insts;
    size_t delta = grew ? stats.after_insts - stats.before_insts
                        : stats.before_insts - stats.after_insts;
    double pct =
        stats.before_insts > 0 && delta > 0
            ? (grew ? 1.0 : -1.0) * 100.0 * (double)delta / stats.before_insts
            : 0.0;
    if (ny_dbg_json_enabled() || ny_dbg_is_enabled(NY_DBG_HEURISTIC))
      ny_dbg_heuristic_event("NYIR_FINALIZE",
                             "instructions=%zu->%zu delta=%zu "
                             "direction=%s "
                             "percent=%+.1f time_ms=%.2f",
                             stats.before_insts, stats.after_insts, delta,
                             delta == 0 ? "flat"
                             : grew     ? "growth"
                                        : "shrink",
                             pct, stats.total_time_ms);
    else if (verbose_enabled >= 2)
      NY_LOG_V2("NYIR finalize: %zu→%zu "
                "instructions (%s, "
                "%+.1f%%) in %.2f ms\n",
                stats.before_insts, stats.after_insts,
                delta == 0 ? "flat"
                : grew     ? "growth"
                           : "shrink",
                pct, stats.total_time_ms);
    else
      fprintf(stderr,
              "nyir finalize: %zu→%zu insts "
              "(%c%zu, %+.1f%%) in %.2fms\n",
              stats.before_insts, stats.after_insts,
              delta == 0 ? '='
              : grew     ? '+'
                         : '-',
              delta, pct, stats.total_time_ms);
  }
  return true;
fail:
  /*
   * Reduced repro dump on failure.
   * NYTRIX_FAIL_IR=<path> writes the full
   * post-optimization IR of the failing
   * function for offline diagnosis.
   */
  fprintf(stderr,
          "native NYIR repro function=%s "
          "(optimize/verify failed): %s\n",
          b->current_fn_name ? b->current_fn_name : "<unknown>",
          err && err[0] ? err : "unknown error");
  nyr_dump_compact(stderr, &b->nyir, "<failed>");
  {
    const char *fail_ir = getenv("NYTRIX_FAIL_IR");
    if (fail_ir && fail_ir[0]) {
      FILE *ir_out = fopen(fail_ir, "w");
      if (ir_out) {
        nyr_dump(ir_out, &b->nyir,
                 b->current_fn_name ? b->current_fn_name : "<failed>");
        fclose(ir_out);
      }
    }
  }
  return false;
}

static bool nyr_opt_dump(FILE *out, nyr_builder_t *b, const char *name,
                         const ny_options *opt) {
  nyr_opt_stats_t stats;
  if (b->options && (b->options->native_backend == NY_NATIVE_BACKEND_X86_64 ||
                     b->options->native_backend == NY_NATIVE_BACKEND_AARCH64))
    nyr_set_preserve_phis(true);
  if (!nyr_resolve_local_store_classes(&b->nyir, b->err, b->err_len) ||
      !nyr_ir_verify(&b->nyir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      nyr_set_error(b->err, b->err_len,
                    "native NYIR dump: initial "
                    "verifier rejected input");
    return false;
  }
  bool optimized =
      opt && opt->nyr_dump_raw
          ? nyr_optimize_debug(&b->nyir, out, &stats, b->opt_level)
          : nyr_optimize_with_stats(&b->nyir, &stats, b->opt_level);
  if (!optimized || !nyr_ir_verify(&b->nyir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      nyr_set_error(b->err, b->err_len,
                    "native NYIR dump: optimization "
                    "failed");
    return false;
  }
  if (opt && opt->nyr_dump_stats)
    nyr_dump_stats(out, &stats);
  nyr_dump(out, &b->nyir, name);
  if (opt && opt->nyr_dump_cfg)
    nyr_dump_cfg(out, &b->nyir, name);
  return true;
}

/*
 * Lower a single function stmt into a
 * finalized nyr_func_t. Returns true on
 * success; caller must nyr_ir_func_free(out)
 * when done.
 */
static bool nyr_stmt_has_return(const stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_RETURN)
    return true;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_has_return(s->as.block.body.data[i]))
        return true;
  } else if (s->kind == NY_S_IF) {
    return nyr_stmt_has_return(s->as.iff.conseq) ||
           nyr_stmt_has_return(s->as.iff.alt);
  } else if (s->kind == NY_S_WHILE) {
    return nyr_stmt_has_return(s->as.whl.body);
  } else if (s->kind == NY_S_FOR) {
    return nyr_stmt_has_return(s->as.fr.body);
  } else if (s->kind == NY_S_GUARD) {
    return nyr_stmt_has_return(s->as.guard.fallback);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (nyr_stmt_has_return(s->as.match.arms.data[i].conseq))
        return true;
    return nyr_stmt_has_return(s->as.match.default_conseq);
  }
  return false;
}

static bool nyr_stmt_has_control_flow(const stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_IF || s->kind == NY_S_WHILE || s->kind == NY_S_FOR ||
      s->kind == NY_S_GUARD || s->kind == NY_S_MATCH)
    return true;
  if (s->kind == NY_S_BLOCK)
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_has_control_flow(s->as.block.body.data[i]))
        return true;
  return false;
}

/* Expensive compile-stage diagnostics are
 * opt-in and function-filterable so a
 * whole-program trace stays useful instead
 * of emitting a line per stdlib fn. RSS is
 * best-effort on Linux; other platforms
 * report `rss=unavailable`. */
static bool nyr_build_trace_match(const stmt_t *fn) {
  if (!ny_trace_enabled("NY_TRACE_NATIVE_BUILD") || !fn ||
      fn->kind != NY_S_FUNC)
    return false;
  const char *filter = getenv("NY_TRACE_NATIVE_BUILD_FILTER");
  return !filter || !*filter ||
         (fn->as.fn.name && strstr(fn->as.fn.name, filter));
}

static unsigned long long nyr_build_rss_kib(void) {
#if defined(__linux__)
  FILE *status = fopen("/proc/self/status", "r");
  if (!status)
    return 0;
  char line[128];
  unsigned long long kib = 0;
  while (fgets(line, sizeof(line), status))
    if (sscanf(line, "VmRSS: %llu kB", &kib) == 1)
      break;
  fclose(status);
  return kib;
#else
  return 0;
#endif
}

static void nyr_build_trace(const stmt_t *fn, const char *stage,
                            ny_tick_t start, unsigned long long start_rss_kib,
                            const nyr_func_t *ir) {
  const unsigned long long rss = nyr_build_rss_kib();
  const double elapsed_ms = ny_ticks_elapsed_sec(start) * 1000.0;
  char rss_text[64];
  if (rss)
    snprintf(rss_text, sizeof(rss_text), "%lluKiB %+lldKiB", rss,
             (long long)rss - (long long)start_rss_kib);
  else
    snprintf(rss_text, sizeof(rss_text), "unavailable");
  ny_dbg_heuristic_event("NATIVE_BUILD",
                         "function=%s stage=%s elapsed=%.2fms "
                         "ir=%zu inst/%d values rss=%s",
                         fn->as.fn.name ? fn->as.fn.name : "<anonymous>", stage,
                         elapsed_ms, ir ? ir->len : 0, ir ? ir->next_value : 0,
                         rss_text);
}

static void nyr_trace_function_boundary(size_t index, size_t count,
                                        const stmt_t *fn, const char *stage) {
  if (!ny_trace_enabled("NY_TRACE_PHASE_MEMORY"))
    return;
  fprintf(stderr,
          "[resource] "
          "phase=native-nir.function.%s "
          "index=%zu/%zu name=%s "
          "rss=%lluMiB\n",
          stage, index + 1, count,
          fn && fn->kind == NY_S_FUNC && fn->as.fn.name ? fn->as.fn.name
                                                        : "<body>",
          nyr_build_rss_kib() / 1024ULL);
  fflush(stderr);
}

static bool nyr_build_function(const program_t *prog, const stmt_t *fn,
                               const ny_extern_table_t *externs,
                               nyr_func_t *out, char *err, size_t err_len,
                               int opt_level, const ny_options *options,
                               nyr_builder_t *cache_owner) {
  if (!fn || fn->kind != NY_S_FUNC || !out)
    return false;
  const bool build_trace = nyr_build_trace_match(fn);
  const ny_tick_t build_start = build_trace ? ny_ticks_now() : 0;
  const unsigned long long build_start_rss =
      build_trace ? nyr_build_rss_kib() : 0;
  if (build_trace)
    nyr_build_trace(fn, "begin", build_start, build_start_rss, NULL);
  const ny_native_lambda_entry_t *lambda_entry = ny_native_lambda_find_name(
      ny_native_lambda_cache_for(cache_owner), fn->as.fn.name);
  memset(out, 0, sizeof(*out));
  int function_opt_level =
      fn->as.fn.attr_optimize ? fn->as.fn.attr_optimize_level : opt_level;
  /* Broad import surfaces can make hundreds
   * of stdlib helpers reachable. Their
   * per-function O3 cleanup dominates
   * compile time while the hot entrypoint
   * is optimized separately; keep imported
   * library bodies at the lightweight
   * baseline unless they explicitly request
   * a level. */
  if (!fn->as.fn.attr_optimize && ny_is_stdlib_tok(fn->tok) &&
      function_opt_level > 0)
    function_opt_level = 0;
  /* Native NYIR carries raw C-string
   * pointers for stdlib ABI calls.  The
   * legacy stdlib bodies still use
   * boxed-string trace inspection, so
   * tracing those bodies would reinterpret
   * raw addresses and crash while printing
   * a frame.  Keep user/source frames and
   * rt_main instrumented until that ABI
   * boundary is unified. */
  nyr_builder_t b = {
      .last_value = -1,
      .err = err,
      .err_len = err_len,
      .externs = externs,
      .prog = prog,
      .options = options,
      .cache_owner = cache_owner,
      .profile_name = fn->as.fn.name ? fn->as.fn.name : "<fn>",
      .source_file = fn->tok.filename,
      .source_main_file = cache_owner ? cache_owner->source_main_file
                                      : (options ? options->input_file : NULL),
      .module_name =
          ny_native_fn_module(ny_native_fn_index_for(cache_owner), prog, fn),
      .lambda_entry = lambda_entry,
      .closure_env_slot = -1,
      .trace_instrumented =
          nyr_trace_requested(options) &&
          !(fn->tok.module_features & NY_MODULE_FEATURE_STDLIB),
      .opt_level = function_opt_level};

  /* An explicit source return type is
   * authoritative.  In particular,
   * generated captured lambdas may retain a
   * conservative tagged semantic fact from
   * the original expression node, but
   * `fn(...) int { ... }` still has a raw
   * integer return ABI. */
  /* An omitted return annotation is not
   * itself an `any` return.  The typing
   * pass records the inferred semantic
   * representation on the function; use
   * that fact when it is available and keep
   * the conservative dynamic ABI only for
   * genuinely unresolved functions.
   * Treating every omitted annotation as
   * `any` widened otherwise inferred scalar
   * functions to the six-word dynamic ABI,
   * so calls from the LLVM path disagreed
   * with the body lowered from the same
   * typed facts. */
  b.return_any = (fn->as.fn.return_type &&
                  ny_native_type_name_is_any(fn->as.fn.return_type)) ||
                 (fn->as.fn.return_semantic.resolved &&
                  fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC &&
                  ny_sem_rep_coherent_with_type(fn->as.fn.return_type,
                                                NY_SEM_REP_TAGGED_DYNAMIC)) ||
                 (!fn->as.fn.return_type &&
                  (!fn->as.fn.return_semantic.resolved ||
                   fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC));
  b.return_type = fn ? fn->as.fn.return_type : NULL;
  if (!b.return_any &&
      ((fn->as.fn.return_semantic.resolved &&
        fn->as.fn.return_semantic.rep == NY_SEM_REP_F64) ||
       ny_native_type_name_is_f64(fn->as.fn.return_type) ||
       nyr_rep_for_type(&b, fn->as.fn.return_type) == NY_SEM_REP_F64))
    b.return_class = NYIR_SCALAR_F64;
  else if (!b.return_any &&
           ((fn->as.fn.return_semantic.resolved &&
             fn->as.fn.return_semantic.rep == NY_SEM_REP_F32) ||
            ny_native_type_name_is_f32(fn->as.fn.return_type) ||
            nyr_rep_for_type(&b, fn->as.fn.return_type) == NY_SEM_REP_F32))
    b.return_class = NYIR_SCALAR_F32;
  else if (!b.return_any && fn->as.fn.return_type &&
           ny_type_is_bigint(fn->as.fn.return_type))
    b.return_class = NYIR_SCALAR_I64_TAGGED;
  if (!nyr_set_param_types(&b, fn)) {
    nyr_ir_func_free(&b.nyir);
    nyr_builder_dispose(&b);
    return false;
  }
  if (build_trace)
    nyr_build_trace(fn, "parameters", build_start, build_start_rss, &b.nyir);
  /*
   * Incoming ABI arguments and addressable
   * locals share NYIR's local-slot operand.
   * Keep their index ranges disjoint:
   * otherwise an expanded formal (for
   * example `any x, fnptr k`) can alias a
   * metadata local such as x's tag with k's
   * incoming argument, and mem2reg will
   * substitute that tag for k.
   */
  b.next_local_slot = b.nyir.param_count;
  if (lambda_entry && lambda_entry->capture_count > 0) {
    nyr_local_t *env =
        nyr_bind_local_typed(&b, "__ny_env", false, false, false);
    if (!env) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
    env->arg_slot = 0;
    b.closure_env_slot = env->slot;
    int env_arg = nyr_ir_emit(
        &b.nyir,
        (nyr_inst_t){
            .op = NYIR_LOAD_LOCAL, .dst = -1, .a = -1, .b = -1, .imm = 0});
    if (env_arg < 0 || !nyr_store_local_value(&b, env->slot, env_arg)) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
    for (size_t i = 0; i < lambda_entry->capture_count; ++i) {
      nyr_local_t *capture = nyr_bind_local(&b, lambda_entry->capture_names[i]);
      if (!capture) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
      capture->is_capture = true;
      capture->capture_index = i;
      const nyr_local_t *src = &lambda_entry->capture_types[i];
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
        if (fn->as.fn.body->kind == NY_S_BLOCK &&
            fn->as.fn.body->as.block.body.len > 0) {
          stmt_t *last = fn->as.fn.body->as.block.body
                             .data[fn->as.fn.body->as.block.body.len - 1];
          if (last->kind == NY_S_EXPR && last->as.expr.expr)
            rets[nret++] = last->as.expr.expr;
        }
      }
      for (size_t r = 0; r < nret; ++r) {
        int64_t val = 0;
        int d = 0;
        if (rets[r] && nyr_eval_proof_i64(&b, rets[r], 0, &val)) {
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
          nyr_fail(&b,
                   "@proves condition '%s' "
                   "is not proved for a "
                   "returned value range",
                   canon);
          free(canon);
          free(full);
          nyr_ir_func_free(&b.nyir);
          nyr_builder_dispose(&b);
          return false;
        }
      }
      free(canon);
      free(full);
    }
  }
  int arg_index = lambda_entry && lambda_entry->capture_count > 0 ? 1 : 0;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const char *param_name = fn->as.fn.params.data[i].name;
    if (param_name && strcmp(param_name, "_") == 0) {
      /* Discarded formals still occupy their declared ABI words, including
       * the length/tag words of expanded dynamic parameters. */
      arg_index += b.nyir.source_param_widths[i];
      continue;
    }
    const char *param_type = fn->as.fn.params.data[i].type;
    const ny_expr_semantic_t *sem = &fn->as.fn.params.data[i].semantic;
    bool try_dynamic = nyr_param_is_try_dynamic(&b, fn, i);
    bool dynamic_sequence =
        !try_dynamic && nyr_param_uses_dynamic_sequence_abi(fn, i);
    ny_sem_rep_t effective_rep =
        try_dynamic ? NY_SEM_REP_TAGGED_DYNAMIC
                    : (param_type && strstr(param_type, "fnptr")
                           ? NY_SEM_REP_CLOSURE
                           : (param_type && *param_type &&
                                      strcmp(param_type, "set") != 0 &&
                                      (nyr_find_refine_in_prog(&b, b.prog,
                                                               param_type) ||
                                       nyr_find_impl_type(&b, param_type))
                                  ? nyr_rep_for_type(&b, param_type)
                                  : (sem->resolved ? sem->rep
                                                   : NY_SEM_REP_UNRESOLVED)));
    bool is_list = !try_dynamic && !dynamic_sequence &&
                   (ny_native_type_name_is_list(param_type) ||
                    nyr_param_is_inferred_list(fn, i) ||
                    (!param_type && effective_rep == NY_SEM_REP_TYPED_BUFFER) ||
                    (fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1));
    bool is_str =
        !try_dynamic && (ny_native_type_name_is_str(param_type) ||
                         (!param_type && effective_rep == NY_SEM_REP_STRING));
    bool is_f64 =
        ny_native_type_name_is_f64(param_type) ||
        (!try_dynamic && !param_type && effective_rep == NY_SEM_REP_F64) ||
        (!try_dynamic && sem->resolved && sem->rep == NY_SEM_REP_F64 &&
         ny_sem_rep_coherent_with_type(param_type, NY_SEM_REP_F64));
    bool is_f32 =
        ny_native_type_name_is_f32(param_type) ||
        (!try_dynamic && !param_type && effective_rep == NY_SEM_REP_F32) ||
        (!try_dynamic && sem->resolved && sem->rep == NY_SEM_REP_F32 &&
         ny_sem_rep_coherent_with_type(param_type, NY_SEM_REP_F32));
    /* BigInt is a tagged runtime handle (or
     * tagged small int), not an
     * aggregate/dictionary payload. Keep
     * its physical local carrier aligned
     * with the one-word call ABI even
     * though semantic type resolution
     * groups user-defined value types into
     * the aggregate representation. */
    bool is_bigint =
        !try_dynamic && param_type && ny_type_is_bigint(param_type);
    /* Keep the expanded ABI for
     * unresolved/tagged values, but honor a
     * resolved raw_int proof for an omitted
     * scalar annotation.  This second ABI
     * assembly must agree with
     * nyr_set_param_types above; a mismatch
     * here changes both the call arity and
     * the local interpretation.
     */
    bool is_any = try_dynamic || dynamic_sequence ||
                  ny_native_type_name_is_any(param_type) ||
                  (!param_type && !is_list && !is_str && !is_f64 && !is_f32 &&
                   effective_rep != NY_SEM_REP_RAW_INT);
    nyr_local_t *param = nyr_bind_local_typed(&b, fn->as.fn.params.data[i].name,
                                              is_f64, is_f32, is_str);
    if (!param) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
    param->is_any = is_any;
    param->type_name = param_type;
    if (!param->type_name) {
      if (is_f64)
        param->type_name = "f64";
      else if (is_f32)
        param->type_name = "f32";
    }
    param->is_bigint = is_bigint;
    param->semantic_rep = is_any ? NY_SEM_REP_TAGGED_DYNAMIC : effective_rep;
    param->semantic_ownership = sem->ownership;
    param->alias_class = sem->alias_class;
    param->semantic_mutable = sem->mutable_value;
    param->is_dict =
        (!is_bigint && param_type && !ny_native_type_name_is_any(param_type) &&
         (sem->resolved && effective_rep == NY_SEM_REP_OBJECT)) ||
        (param_type && (strcmp(param_type, "dict") == 0 ||
                        ny_type_vector_dimension(param_type) > 0));
    if (ny_dbg_is_enabled(NY_DBG_HEURISTIC) && is_bigint)
      ny_dbg_heuristic_event(
          "native-abi",
          "function=%s param=%s "
          "logical=bigint "
          "decision=tagged-word is_dict=%d "
          "try_dynamic=%d semantic_rep=%d",
          fn->as.fn.name ? fn->as.fn.name : "<anonymous>",
          fn->as.fn.params.data[i].name ? fn->as.fn.params.data[i].name : "?",
          param->is_dict, try_dynamic, (int)param->semantic_rep);
    param->is_list = is_list;
    param->is_dyn_list =
        (is_list && ((fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1) ||
                     ny_native_type_name_is_dyn_list(param_type) ||
                     nyr_function_param_is_dyn_list(cache_owner, prog, fn, i)));
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
    param->fin_bound = ny_native_resolve_fin_bound(&b, param_type);
  }
  b.current_fn_name = fn->as.fn.name;
  b.nyir.param_count = (unsigned)arg_index;
  if (!nyr_emit_trace_enter(&b, fn->as.fn.name, fn->tok.filename,
                            fn->tok.line)) {
    nyr_ir_func_free(&b.nyir);
    nyr_builder_dispose(&b);
    return false;
  }
  /*
   * Function prologue: move expanded
   * arguments from their incoming positions
   * (ld #arg_slot) into the local slots
   * that the body expects.
   */
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    nyr_local_t *param = nyr_find_local(&b, fn->as.fn.params.data[i].name);
    if (!param)
      continue;
    if (param->arg_slot >= 0) {
      nyr_inst_t param_ld = {.op = NYIR_LOAD_LOCAL,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .imm = param->arg_slot};
      param_ld.scalar_class =
          ny_native_scalar_class_from_sem_rep(param->semantic_rep);
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
      int arg_val = nyr_ir_emit(&b.nyir, param_ld);
      if (arg_val >= 0) {
        /* Callable creation records whether
         * the entry uses raw word or tagged
         * arguments. Typed scalar
         * parameters retain their declared
         * word ABI; the runtime adapter
         * performs any required decoding.
         */
        /*
         * List parameters carry their
         * length in the next ABI slot. Do
         * not inspect the pointer as a
         * managed tbuf here: constant
         * pooled arrays have no tbuf
         * header, and the companion slot is
         * authoritative.
         */
        if (param->is_list && param->list_len_arg_slot >= 0) {
          size_t before = b.nyir.len;
          nyr_ir_emit(&b.nyir, (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                                            .dst = -1,
                                            .a = arg_val,
                                            .b = -1,
                                            .imm = param->slot});
          if (b.nyir.len == before) {
            nyr_ir_func_free(&b.nyir);
            nyr_builder_dispose(&b);
            return false;
          }
        } else if (!nyr_store_local_value(&b, param->slot, arg_val)) {
          nyr_ir_func_free(&b.nyir);
          nyr_builder_dispose(&b);
          return false;
        }
      }
    }
    if (param->is_list && param->list_len_arg_slot >= 0) {
      int len_val = nyr_ir_emit(
          &b.nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .imm = param->list_len_arg_slot,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
      if (len_val >= 0)
        nyr_store_local_value(&b, param->list_len_slot, len_val);
    } else if ((param->is_cstr || param->is_any) &&
               param->dyn_str_len_arg_slot >= 0) {
      int len_val = nyr_ir_emit(
          &b.nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .imm = param->dyn_str_len_arg_slot,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
      int tag_val = nyr_ir_emit(
          &b.nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                .dst = -1,
                                .a = -1,
                                .b = -1,
                                .imm = param->dyn_tag_arg_slot,
                                .scalar_class = NYIR_SCALAR_I64_RAW});
      if (len_val >= 0)
        nyr_store_local_value(&b, param->dyn_str_len_slot, len_val);
      if (tag_val >= 0)
        nyr_store_local_value(&b, param->dyn_tag_slot, tag_val);
    }
  }
  if (b.trace_instrumented) {
    for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
      nyr_local_t *param = nyr_find_local(&b, fn->as.fn.params.data[i].name);
      if (!param)
        continue;
      int value = nyr_load_local_value(&b, param->slot);
      if (value < 0)
        continue;
      nyr_scalar_class_t param_class =
          ny_native_scalar_class_from_sem_rep(param->semantic_rep);
      bool param_f64 = param->is_f64 || param_class == NYIR_SCALAR_F64;
      bool param_f32 = param->is_f32 || param_class == NYIR_SCALAR_F32;
      const char *trace_symbol =
          (param_f64 || param_f32) ? "rt_trace_arg_f64"
          : (!param->is_any && !param->is_list && !param->is_cstr &&
             !param_f64 && !param_f32 &&
             param->semantic_rep == NY_SEM_REP_RAW_INT)
              ? "rt_trace_arg_i64"
              : "rt_trace_arg_tagged";
      if (param_f32) {
        value = nyr_emit_f32_to_f64(&b, value);
        if (value < 0)
          continue;
      }
      if (nyr_emit_runtime_call(&b, trace_symbol, value, -1, -1, 1, 0) < 0) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
    }
  }
  b.tail_body = fn->as.fn.body;
  bool scalar_tail = b.current_fn_name && fn->as.fn.body &&
                     !nyr_stmt_has_defer(fn->as.fn.body, 0);
  /* Tail-call lowering is only valid when
   * the recursive call itself is the
   * returned value.  A binary expression
   * such as `1 + tree(depth - 1) +
   * tree(depth - 1)` contains non-tail
   * recursive calls; enabling the
   * parameter-rewrite loop for it corrupts
   * the ordinary call frame and crashes
   * recursive tree benchmarks. */
  if (scalar_tail && fn->as.fn.body->kind == NY_S_BLOCK &&
      fn->as.fn.body->as.block.body.len > 0) {
    const stmt_t *last = fn->as.fn.body->as.block.body
                             .data[fn->as.fn.body->as.block.body.len - 1];
    if (last && last->kind == NY_S_EXPR && last->as.expr.expr &&
        last->as.expr.expr->kind == NY_E_BINARY)
      scalar_tail = false;
  }
  for (size_t i = 0; scalar_tail && i < fn->as.fn.params.len; ++i) {
    const char *type = fn->as.fn.params.data[i].type;
    if (ny_native_type_name_is_list(type) || ny_native_type_name_is_str(type) ||
        ny_native_type_name_is_any(type))
      scalar_tail = false;
  }
  if (scalar_tail) {
    b.tail_param_count = fn->as.fn.params.len;
    if (b.tail_param_count > 0) {
      b.tail_param_slots =
          calloc(b.tail_param_count, sizeof(*b.tail_param_slots));
      if (!b.tail_param_slots) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return nyr_fail(&b, NY_NATIVE_ALLOC_FAIL);
      }
      for (size_t i = 0; i < b.tail_param_count; ++i) {
        nyr_local_t *param = nyr_find_local(&b, fn->as.fn.params.data[i].name);
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
      if (!nyr_emit_label(&b, b.tail_loop_label)) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
    }
  }
  const char *trace_filter = getenv("NY_TRACE_NATIVE_BUILD_FILTER");
  bool trace_body = ny_trace_enabled("NY_TRACE_PHASE_MEMORY") && trace_filter &&
                    *trace_filter && fn->as.fn.name &&
                    strstr(fn->as.fn.name, trace_filter);
  const char *previous_trace_function = nyr_trace_function_name;
  const size_t previous_trace_seq = nyr_trace_statement_seq;
  nyr_trace_function_name = trace_body ? fn->as.fn.name : NULL;
  nyr_trace_statement_seq = 0;
  bool ok = nyr_lower_stmt(&b, fn->as.fn.body);
  nyr_trace_function_name = previous_trace_function;
  nyr_trace_statement_seq = previous_trace_seq;
  if (build_trace)
    nyr_build_trace(fn, ok ? "body-lowered" : "body-failed", build_start,
                    build_start_rss, &b.nyir);
  bool has_implicit_tail =
      fn->as.fn.body && (fn->as.fn.body->kind == NY_S_EXPR ||
                         (fn->as.fn.body->kind == NY_S_BLOCK &&
                          fn->as.fn.body->as.block.body.len > 0 &&
                          fn->as.fn.body->as.block.body
                                  .data[fn->as.fn.body->as.block.body.len - 1]
                                  ->kind == NY_S_EXPR));
  if (ok && !b.emitted_return && !has_implicit_tail &&
      !nyr_stmt_has_return(fn->as.fn.body) &&
      !nyr_stmt_has_control_flow(fn->as.fn.body)) {
    const char *rt = fn->as.fn.return_type;
    const char *dot = rt ? strrchr(rt, '.') : NULL;
    const char *base = dot ? dot + 1 : rt;
    if (base && *base && strcmp(base, "void") != 0 &&
        strcmp(base, "any") != 0 && strcmp(base, "nil") != 0) {
      ok = nyr_fail(&b,
                    "function '%s' falls off the end "
                    "without returning a %s value",
                    fn->as.fn.name ? fn->as.fn.name : "<fn>", base);
    }
  }
  if (ok && !b.emitted_return) {
    /*
     * Capture the implicit return value
     * before defer bodies run: defer
     * statements lower as expression
     * statements and would clobber
     * last_value.
     */
    int ret = b.last_value >= 0 ? b.last_value : nyr_emit_const(&b, 0);
    /* Preserve the implicit expression's
     * producer kind.  Passing NULL here
     * made captured dynamic lambdas
     * conservatively tag an already-tagged
     * `any` arithmetic result a second time
     * (closure 5 became 31/63). */
    const expr_t *implicit_expr = NULL;
    if (fn->as.fn.body && fn->as.fn.body->kind == NY_S_EXPR)
      implicit_expr = fn->as.fn.body->as.expr.expr;
    else if (fn->as.fn.body && fn->as.fn.body->kind == NY_S_BLOCK &&
             fn->as.fn.body->as.block.body.len > 0) {
      const stmt_t *last = fn->as.fn.body->as.block.body
                               .data[fn->as.fn.body->as.block.body.len - 1];
      if (last && last->kind == NY_S_EXPR)
        implicit_expr = last->as.expr.expr;
    }
    bool implicit_dynamic_ternary =
        implicit_expr && implicit_expr->kind == NY_E_TERNARY &&
        (nyr_expr_is_any(&b, implicit_expr->as.ternary.true_expr) ||
         nyr_expr_is_canonical_dynamic_value(
             &b, implicit_expr->as.ternary.true_expr) ||
         nyr_expr_is_any(&b, implicit_expr->as.ternary.false_expr) ||
         nyr_expr_is_canonical_dynamic_value(
             &b, implicit_expr->as.ternary.false_expr));
    /* The tail local's binding has already
     * been removed by block scope
     * restoration. Recover its machine
     * representation from the emitted slot
     * stores so the dynamic return path
     * boxes an existing f64 instead of
     * decoding it as though it were still
     * tagged. */
    if (has_implicit_tail && implicit_expr &&
        implicit_expr->kind == NY_E_IDENT) {
      const nyr_inst_t *value_def = nyr_reg_def_inst(&b, ret);
      int64_t slot =
          value_def && value_def->op == NYIR_LOAD_LOCAL ? value_def->imm : -1;
      bool saw_store = false;
      bool all_f64_stores = slot >= 0;
      for (size_t si = 0; slot >= 0 && si < b.nyir.len; ++si) {
        const nyr_inst_t *store_inst = &b.nyir.data[si];
        if (store_inst->op != NYIR_STORE_LOCAL || store_inst->imm != slot)
          continue;
        saw_store = true;
        if (store_inst->scalar_class != NYIR_SCALAR_F64 &&
            nyr_value_scalar_class(&b, store_inst->a) != NYIR_SCALAR_F64) {
          all_f64_stores = false;
          break;
        }
      }
      if (saw_store && all_f64_stores) {
        ret =
            nyr_ir_emit(&b.nyir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .imm = slot,
                                              .scalar_class = NYIR_SCALAR_F64});
        if (ret < 0)
          ok = false;
        b.last_value_f64 = ret >= 0;
      }
    }
    nyr_scalar_class_t implicit_value_class = nyr_value_scalar_class(&b, ret);
    /* Scope restoration removed the tail
     * local, so normalize_return cannot see
     * its raw slot ABI.  Preserve the fact
     * captured while lowering the
     * expression and box once for an
     * untyped/dynamic result. */
    if (has_implicit_tail && b.return_any &&
        (b.last_value_f64 || implicit_value_class == NYIR_SCALAR_F64 ||
         implicit_value_class == NYIR_SCALAR_F32)) {
      int as_f64 = implicit_value_class == NYIR_SCALAR_F32
                       ? nyr_emit_f32_to_f64(&b, ret)
                       : ret;
      int bits = as_f64 < 0 ? -1
                            : nyr_emit_runtime_call(&b, "rt_f64_bits", as_f64,
                                                    -1, -1, 1, 0);
      ret = bits < 0 ? -1
                     : nyr_emit_runtime_call(&b, "rt_flt_box_val", bits, -1, -1,
                                             1, NYIR_SCALAR_I64_TAGGED);
    } else if (has_implicit_tail && b.last_value_raw_scalar && b.return_any) {
      /* A bool-marked callback adapter owns
       * bool boxing/normalization. Its
       * lambda body must return the raw
       * predicate word; tagging 0/1 here
       * turns a false predicate into tagged
       * integer zero and causes filter
       * callbacks to select the wrong
       * elements. */
      if (!(b.lambda_entry && nyr_expr_is_bool(&b, implicit_expr)) &&
          !implicit_dynamic_ternary &&
          nyr_value_scalar_class(&b, ret) != NYIR_SCALAR_I64_TAGGED &&
          !nyr_value_is_tagged_dynamic_call(&b, ret))
        ret = nyr_emit_runtime_call(&b, "rt_tag", ret, -1, -1, 1, 0);
    } else if (!(has_implicit_tail && b.last_value_f64 && b.return_type &&
                 ny_native_type_name_is_f64(b.return_type)) &&
               !(has_implicit_tail && b.last_value_raw_scalar &&
                 b.return_type && ny_native_type_name_is_int(b.return_type)))
      ret = nyr_normalize_return(&b, implicit_expr, ret);
    ok = ret >= 0 && nyr_emit_defers(&b, 0);
    if (ok)
      ok = nyr_emit_ret(&b, ret);
  }
  if (ok)
    ok = nyr_finalize(&b, err, err_len);
  if (build_trace)
    nyr_build_trace(fn, ok ? "finalized" : "finalize-failed", build_start,
                    build_start_rss, &b.nyir);
  if (ok)
    *out = b.nyir;
  else
    nyr_ir_func_free(&b.nyir);
  nyr_builder_dispose(&b);
  return ok;
}

/*
 * Build extern table from #include and
 * extern top-level statements. Populates
 * the table with NY name → C symbol
 * mappings so the call lowerer can emit
 * correct linker symbols for extern C
 * functions.
 *
 * Like
 * codegen_collect_links()/process_links(),
 * this recurses through the program: extern
 * declarations are not guaranteed to be
 * top-level. The prelude/stdlib and the
 * script wrapper nest user declarations
 * inside NY_S_MODULE/NY_S_BLOCK (and
 * control-flow bodies), so a flat scan of
 * prog->body would silently miss them.
 */
static bool ny_native_target_platform_ident(const ny_options *opt,
                                            const char *name, bool *out) {
  if (!name || !out)
    return false;
  const char *triple = opt ? opt->host_triple : NULL;
  const char *host_os = ny_host_os_name();
  const char *host_arch = ny_host_arch_name();
  bool is_windows =
      triple && (strstr(triple, "windows") || strstr(triple, "mingw") ||
                 strstr(triple, "msvc") || strstr(triple, "win32"));
  bool is_macos =
      triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
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
    case NY_NATIVE_BACKEND_X86_64:
      is_x86_64 = is_x86 = true;
      break;
    case NY_NATIVE_BACKEND_X86:
      is_x86 = true;
      break;
    case NY_NATIVE_BACKEND_AARCH64:
      is_aarch64 = is_arm = true;
      break;
    case NY_NATIVE_BACKEND_ARM:
      is_arm = true;
      break;
    case NY_NATIVE_BACKEND_RISCV:
      is_riscv = true;
      break;
    default:
      break;
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
  bool is_unix =
      !is_windows && (is_linux || is_macos || strcmp(host_os, "unknown") != 0);
  const struct {
    const char *name;
    bool value;
  } values[] = {
      {"linux", is_linux},
      {"LINUX", is_linux},
      {"IS_LINUX", is_linux},
      {"macos", is_macos},
      {"mac", is_macos},
      {"MACOS", is_macos},
      {"IS_MACOS", is_macos},
      {"windows", is_windows},
      {"IS_WINDOWS", is_windows},
      {"unix", is_unix},
      {"posix", is_unix},
      {"UNIX", is_unix},
      {"IS_UNIX", is_unix},
      {"x86_64", is_x86_64},
      {"x64", is_x86_64},
      {"IS_X86_64", is_x86_64},
      {"x86", is_x86},
      {"IS_X86", is_x86},
      {"aarch64", is_aarch64},
      {"arm64", is_aarch64},
      {"IS_AARCH64", is_aarch64},
      {"arm", is_arm},
      {"IS_ARM", is_arm},
      {"riscv", is_riscv},
      {"IS_RISCV", is_riscv},
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
 * Evaluate the platform queries accepted in
 * comptime branch conditions.  This
 * deliberately recognizes only
 * zero-argument host queries: an arbitrary
 * call must remain unknown so extern
 * collection continues to inspect both
 * paths.
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

bool nyr_target_eval_bool(const ny_options *opt, const expr_t *e, bool *out) {
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
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
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
        !nyr_target_eval_bool(opt, e->as.unary.right, &v))
      return false;
    *out = !v;
    return true;
  }
  case NY_E_LOGICAL: {
    bool l = false, r = false;
    if (!e->as.logical.op || !nyr_target_eval_bool(opt, e->as.logical.left, &l))
      return false;
    if (strcmp(e->as.logical.op, "&&") == 0) {
      if (!l) {
        *out = false;
        return true;
      }
      if (!nyr_target_eval_bool(opt, e->as.logical.right, &r))
        return false;
      *out = r;
      return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      if (l) {
        *out = true;
        return true;
      }
      if (!nyr_target_eval_bool(opt, e->as.logical.right, &r))
        return false;
      *out = r;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    if (!e->as.binary.op || (strcmp(e->as.binary.op, "==") != 0 &&
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
      return nyr_target_eval_bool(opt, body->as.ret.value, out);
    if (body->kind == NY_S_EXPR)
      return nyr_target_eval_bool(opt, body->as.expr.expr, out);
    return false;
  }
  default:
    return false;
  }
}

static char *nyr_read_c_include(const char *path, bool is_std, void *userdata) {
  (void)userdata;
  char *src = path ? ny_read_file(path) : NULL;
  if (src || !is_std || !path)
    return src;
  static const char *const roots[] = {"/usr/local/include", "/usr/include",
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

static bool nyr_wasm_classify_aggregate(const ny_parser_t *parser,
                                        const ny_ctype_t *type,
                                        ny_sysv_agg_class_t classes[2]) {
  if (!type || type->aggregate_size == 0)
    return false;
  classes[0] = NY_SYSV_AGG_NONE;
  classes[1] = NY_SYSV_AGG_NONE;
  if (type->aggregate_size > 8) {
    classes[0] = NY_SYSV_AGG_MEMORY;
    return true;
  }
  if (type->aggregate_fields != 1) {
    classes[0] = NY_SYSV_AGG_MEMORY;
    return true;
  }
  if (!ny_native_sysv_classify_aggregate(parser, type, classes))
    return false;
  return classes[1] == NY_SYSV_AGG_NONE &&
         (classes[0] == NY_SYSV_AGG_INTEGER || classes[0] == NY_SYSV_AGG_SSE);
}

static bool nyr_collect_extern(const stmt_t *s, ny_extern_table_t *t,
                               bool aapcs, const ny_options *opt, char *err,
                               size_t err_len) {
  if (!s)
    return true;
  if (s->kind == NY_S_EXTERN) {
    const char *ny_name = s->as.ext.name;
    const char *c_sym = s->as.ext.link_name ? s->as.ext.link_name : ny_name;
    unsigned pc = (unsigned)s->as.ext.params.len;
    if (!ny_extern_table_add(t, ny_name, c_sym, pc, false, 0, NULL, NULL, false,
                             false, false, NULL, NULL)) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "NYIR extern: conflicting or "
                 "duplicate extern '%s' "
                 "(C symbol '%s' conflicts with "
                 "earlier declaration)",
                 ny_name, c_sym);
      ny_diag_error(s->tok,
                    "NYIR extern: conflicting or "
                    "duplicate extern '%s' "
                    "(C symbol '%s' conflicts with "
                    "earlier declaration)",
                    ny_name, c_sym);
      return false;
    }
    return true;
  }
  if (s->kind == NY_S_INCLUDE) {
    const char *prefix = s->as.inc.prefix;
    char *src = ny_read_file(s->as.inc.path);
    if (!src && s->as.inc.path) {
      static const char *const roots[] = {"/usr/local/include", "/usr/include",
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
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", roots[r],
                         s->as.inc.path);
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
        snprintf(err, err_len,
                 "NYIR extern: out of "
                 "memory parsing #include");
      return false;
    }
    ny_parse_init(parser, src, srclen);
    parser->include_read = nyr_read_c_include;
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
    for (;;) {
      const char *stall_pos = parser->tok.start;
      int rc = ny_parse_decl(parser, &decl);
      if (rc <= 0)
        break;
      if (decl.kind != NY_CDECL_FUNC)
        goto extern_next_decl;
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
          goto extern_next_decl;
      } else {
        size_t nn = nlen;
        if (nn >= sizeof(ny_name))
          nn = sizeof(ny_name) - 1;
        memcpy(ny_name, cname, nn);
        ny_name[nn] = '\0';
      }
      /*
       * Compute aggregate return size and
       * per-argument aggregate sizes
       */
      uint32_t ret_agg = 0;
      ny_sysv_agg_class_t ret_agg_classes[2] = {NY_SYSV_AGG_NONE,
                                                NY_SYSV_AGG_NONE};
      if (decl.type.kind == NY_CTYPE_STRUCT ||
          decl.type.kind == NY_CTYPE_UNION) {
        ret_agg = (uint32_t)decl.type.aggregate_size;
      } else if (decl.type.kind == NY_CTYPE_NAMED && decl.type.ptr_depth == 0) {
        /*
         * Named typedef that may be a
         * struct — aggregate_size if
         * present
         */
        ret_agg = (uint32_t)decl.type.aggregate_size;
      }
      bool wasm = opt && opt->native_backend == NY_NATIVE_BACKEND_WASM;
      if (ret_agg > 0 && !(wasm    ? nyr_wasm_classify_aggregate(
                                         parser, &decl.type, ret_agg_classes)
                           : aapcs ? ny_native_aapcs_classify_aggregate(
                                         parser, &decl.type, ret_agg_classes)
                                   : ny_native_sysv_classify_aggregate(
                                         parser, &decl.type, ret_agg_classes)))
        ret_agg_classes[0] = NY_SYSV_AGG_UNSUPPORTED;
      uint32_t arg_agg[NY_C_MAX_PARAMS] = {0};
      bool param_f64[NY_C_MAX_PARAMS] = {0};
      bool param_f32[NY_C_MAX_PARAMS] = {0};
      for (unsigned pi = 0; pi < decl.param_count && pi < NY_C_MAX_PARAMS;
           pi++) {
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
          ny_sysv_agg_class_t classes[2] = {NY_SYSV_AGG_NONE, NY_SYSV_AGG_NONE};
          if (arg_agg[pi] > NYIR_ARG_AGG_SIZE_MASK ||
              !(wasm ? nyr_wasm_classify_aggregate(parser, pt, classes)
                : aapcs
                    ? ny_native_aapcs_classify_aggregate(parser, pt, classes)
                    : ny_native_sysv_classify_aggregate(parser, pt, classes))) {
            classes[0] = NY_SYSV_AGG_UNSUPPORTED;
            classes[1] = NY_SYSV_AGG_NONE;
          }
          arg_agg[pi] = (arg_agg[pi] & NYIR_ARG_AGG_SIZE_MASK) |
                        ((uint32_t)classes[0] << NYIR_ARG_AGG_CLASS0_SHIFT) |
                        ((uint32_t)classes[1] << NYIR_ARG_AGG_CLASS1_SHIFT);
        }
      }
      char *ny_name_dup = ny_strdup(ny_name);
      char *c_sym = ny_strdup(cname);
      if (!ny_name_dup || !c_sym ||
          !ny_extern_table_add(
              t, ny_name_dup, c_sym, decl.param_count, true, ret_agg,
              ret_agg_classes, arg_agg, ny_native_c_type_is_f64(&decl.type),
              ny_native_c_type_is_f32(&decl.type),
              ny_native_c_type_is_signed_i64(&decl.type, parser->abi),
              param_f64, param_f32)) {
        free(ny_name_dup);
        free(c_sym);
        ny_parse_cleanup(parser);
        free(parser);
        free(src);
        if (err && err_len > 0)
          snprintf(err, err_len,
                   "NYIR extern: table "
                   "full from #include");
        return false;
      }
      if (!prefix || !prefix[0]) {
        char default_name[512];
        int nn = snprintf(default_name, sizeof(default_name), "c.%s", cname);
        char *default_name_dup = nn > 0 && (size_t)nn < sizeof(default_name)
                                     ? ny_strdup(default_name)
                                     : NULL;
        char *default_c_sym = ny_strdup(cname);
        if (!default_name_dup || !default_c_sym ||
            !ny_extern_table_add(
                t, default_name_dup, default_c_sym, decl.param_count, true,
                ret_agg, ret_agg_classes, arg_agg,
                ny_native_c_type_is_f64(&decl.type),
                ny_native_c_type_is_f32(&decl.type),
                ny_native_c_type_is_signed_i64(&decl.type, parser->abi),
                param_f64, param_f32)) {
          free(default_name_dup);
          free(default_c_sym);
          ny_parse_cleanup(parser);
          free(parser);
          free(src);
          if (err && err_len > 0)
            snprintf(err, err_len,
                     "NYIR extern: table full "
                     "from default C namespace");
          return false;
        }
      }
    extern_next_decl:
      if (parser->tok.kind != NY_CTOK_EOF && parser->tok.start == stall_pos)
        parser->tok = ny_lex_next(&parser->lx);
    }
    if (!ny_native_c_layout_collect_parser(&t->layouts, parser)) {
      ny_parse_cleanup(parser);
      free(parser);
      free(src);
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "NYIR extern: C layout table "
                 "full or out of memory");
      return false;
    }
    for (unsigned i = 0; i < parser->define_count; ++i) {
      char *define_name = ny_native_c_token_dup(parser->define_names[i]);
      if (!define_name)
        continue;
      bool define_ok = ny_native_c_define_add(&t->defines, define_name,
                                              parser->define_values[i]);
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
          snprintf(err, err_len,
                   "NYIR extern: C define table "
                   "full or out of memory");
        return false;
      }
    }
    ny_parse_cleanup(parser);
    free(parser);
    free(src);
    return true;
  }
  /*
   * Recurse through container statements,
   * mirroring process_links().
   */
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!nyr_collect_extern(s->as.module.body.data[i], t, aapcs, opt, err,
                              err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!nyr_collect_extern(s->as.block.body.data[i], t, aapcs, opt, err,
                              err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_IF) {
    bool selected = false;
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME &&
        nyr_target_eval_bool(opt, s->as.iff.test, &selected)) {
      const stmt_t *branch = selected ? s->as.iff.conseq : s->as.iff.alt;
      return !branch || nyr_collect_extern(branch, t, aapcs, opt, err, err_len);
    }
    if (s->as.iff.conseq &&
        !nyr_collect_extern(s->as.iff.conseq, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.iff.alt &&
        !nyr_collect_extern(s->as.iff.alt, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body &&
        !nyr_collect_extern(s->as.whl.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.whl.update &&
        !nyr_collect_extern(s->as.whl.update, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.whl.init &&
        !nyr_collect_extern(s->as.whl.init, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init &&
        !nyr_collect_extern(s->as.fr.init, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.fr.body &&
        !nyr_collect_extern(s->as.fr.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.fr.update &&
        !nyr_collect_extern(s->as.fr.update, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_TRY) {
    if (s->as.tr.body &&
        !nyr_collect_extern(s->as.tr.body, t, aapcs, opt, err, err_len))
      return false;
    if (s->as.tr.handler &&
        !nyr_collect_extern(s->as.tr.handler, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_DEFER) {
    if (s->as.de.body &&
        !nyr_collect_extern(s->as.de.body, t, aapcs, opt, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (s->as.match.arms.data[i].conseq &&
          !nyr_collect_extern(s->as.match.arms.data[i].conseq, t, aapcs, opt,
                              err, err_len))
        return false;
    if (s->as.match.default_conseq &&
        !nyr_collect_extern(s->as.match.default_conseq, t, aapcs, opt, err,
                            err_len))
      return false;
    return true;
  }
  return true;
}

static bool nyr_build_extern_table(const program_t *prog, ny_extern_table_t *t,
                                   bool aapcs, const ny_options *opt, char *err,
                                   size_t err_len) {
  if (!t)
    return false;
  ny_extern_table_init(t);
  if (!prog)
    return true;
  const bool emit_std_module =
      opt && opt->emit_module && strcmp(opt->emit_module, "std") == 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    /* Imported stdlib modules are
     * declarations, not source-level link
     * manifests for the current native
     * unit.  Walking every nested #include
     * in core.os here repeatedly reparses
     * the system headers and defeats the
     * reachable-function pruning below.
     * User/source-file includes remain
     * fully collected; runtime-backed
     * stdlib calls are represented by their
     * rt_* lowering symbols instead. */
    if (ny_native_stmt_is_stdlib(s) && !emit_std_module)
      continue;
    if (!nyr_collect_extern(s, t, aapcs, opt, err, err_len))
      return false;
  }
  return true;
}

/*
 * Per-pass oracle callback state.  The IR
 * layer calls this after every successful
 * verifier checkpoint while optimizing
 * rt_main.
 */
typedef struct {
  const ny_options *opt;
  nyr_func_t *rt_main;
  nyr_func_t *funcs;
  const char **names;
  size_t count;
} ny_native_oracle_ctx_t;

static bool ny_native_per_pass_oracle_cb(const nyr_func_t *f,
                                         const char *pass_name,
                                         void *userdata) {
  ny_native_oracle_ctx_t *ctx = (ny_native_oracle_ctx_t *)userdata;
  /* The callback is globally installed
   * while collected helper functions are
   * finalized too.  Only rt_main has the
   * program-result contract; comparing a
   * helper's return value against the
   * top-level expected result produces a
   * false failure at the first const-fold
   * checkpoint. */
  if (!ctx || !f || f != ctx->rt_main)
    return true;
  char err[512] = {0};
  bool ok = nyr_native_result_oracle_for_nir(
      (nyr_func_t *)f, ctx->funcs, ctx->names, ctx->count, ctx->opt,
      ny_pipeline_runtime_services_get(), err, sizeof(err));
  if (!ok) {
    fprintf(stderr,
            "native NYIR: per-pass oracle "
            "failed after %s: %s\n",
            pass_name ? pass_name : "pass",
            err[0] ? err : NY_NATIVE_UNKNOWN_ERR);
  }
  return ok;
}

/*
 * Lower the top-level program statements
 * into a finalized nyr_func_t for rt_main.
 * Returns true on success; caller must
 * nyr_ir_func_free(out).
 */
static bool nyr_build_rt_main(const program_t *prog, nyr_func_t *out,
                              const ny_extern_table_t *externs, char *err,
                              size_t err_len, int opt_level,
                              const ny_options *options,
                              nyr_builder_t *cache_owner) {
  if (!out)
    return false;
  const char *source_file = options ? options->input_file : NULL;
  /* Test runners and embedded callers can feed the root program through
   * stdin, leaving input_file unset.  In that case the first non-module
   * statement still carries the root file's source location; imported
   * module bodies are separate NY_S_MODULE nodes and must not establish the
   * main-file identity. */
  if (!source_file || !*source_file) {
    for (size_t i = 0; prog && i < prog->body.len; ++i) {
      const stmt_t *stmt = prog->body.data[i];
      if (stmt && stmt->kind != NY_S_MODULE && stmt->tok.filename &&
          *stmt->tok.filename) {
        source_file = stmt->tok.filename;
        break;
      }
    }
  }
  memset(out, 0, sizeof(*out));
  nyr_builder_t b = {.last_value = -1,
                     .err = err,
                     .err_len = err_len,
                     .externs = externs,
                     .prog = prog,
                     .options = options,
                     .cache_owner = cache_owner,
                     .profile_name = "rt_main",
                     .current_fn_name = "rt_main",
                     .source_file = source_file,
                     .source_main_file = source_file,
                     .trace_instrumented = nyr_trace_requested(options),
                     .opt_level = opt_level};
  if (!nyr_emit_trace_enter(&b, "rt_main", NULL, 1)) {
    nyr_ir_func_free(&b.nyir);
    nyr_builder_dispose(&b);
    return false;
  }
  const bool trace_lower = ny_dbg_verbosity() >= 5;
  const clock_t lower_started = trace_lower ? clock() : (clock_t)0;
  size_t user_stmt_count = 0;
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    const stmt_t *top_stmt = prog->body.data[i];
    if (trace_lower && top_stmt && !ny_native_stmt_is_stdlib(top_stmt))
      ++user_stmt_count;
    if (i + 2 < prog->body.len &&
        nyr_match_bounded_scalar_append(
            &b, prog->body.data[i], prog->body.data[i + 1],
            prog->body.data[i + 2], &(const char *){0}, &(const char *){0},
            &(const expr_t *){0}, &(const expr_t *){0})) {
      if (trace_lower) {
        for (size_t j = 1; j < 3; ++j) {
          const stmt_t *matched_stmt = prog->body.data[i + j];
          if (matched_stmt && !ny_native_stmt_is_stdlib(matched_stmt))
            ++user_stmt_count;
        }
      }
      if (!nyr_lower_bounded_scalar_append(&b, prog->body.data[i],
                                           prog->body.data[i + 1],
                                           prog->body.data[i + 2])) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
      i += 2;
      continue;
    }
    if (!nyr_lower_stmt(&b, prog->body.data[i])) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (trace_lower) {
    double cpu_ms =
        1000.0 * (double)(clock() - lower_started) / (double)CLOCKS_PER_SEC;
    ny_dbg_heuristic_event(
        "native-lower",
        "function=rt_main source-stmts=%zu insts=%zu cpu_ms=%.3f",
        user_stmt_count, b.nyir.len, cpu_ms);
  }
  if (!b.emitted_return) {
    /*
     * A user-defined main is the documented
     * executable entrypoint: top-level
     * statements run first, then rt_main
     * calls main and returns its value.
     * Deciding on `last_value < 0` alone
     * let any trailing top-level statement
     * (including a plain `def`) suppress
     * the call, so main never ran.
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
    /* The implicit native entry has no source-level arguments.  A
     * parameterized main must only run through an explicit call in the
     * program; emitting a zero-argument call here gives its lowered ABI
     * an invalid arity (dynamic/list parameters expand to multiple words). */
    if (main_fn && main_fn->as.fn.params.len == 0) {
      int call_val = nyr_ir_emit(
          &b.nyir,
          (nyr_inst_t){.op = NYIR_CALL,
                       .dst = -1,
                       .a = -1,
                       .b = -1,
                       .c = -1,
                       .imm = 0,
                       .flags = 0,
                       .scalar_class =
                           main_fn->as.fn.return_semantic.resolved
                               ? ny_native_scalar_class_from_sem_rep(
                                     main_fn->as.fn.return_semantic.rep)
                               : NYIR_SCALAR_UNKNOWN,
                       .symbol = "main"});
      if (call_val < 0) {
        nyr_fail(&b, "native NYIR: failed to "
                     "emit call to main");
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
      b.last_value = call_val;
    } else if (b.last_value < 0) {
      /* A module-only compilation unit has
       * no executable expression.  It is
       * still a valid native artifact; make
       * its entry a no-op so incremental
       * provider modules can be compiled
       * independently. */
      b.last_value = nyr_emit_const(&b, 0);
      if (b.last_value < 0) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
    }
    int ret_val = b.last_value;
    /* The native result oracle calls
     * rt_main through a type-specific
     * harness and compares the raw IEEE
     * result bits. Preserve the top-level
     * floating ABI for that diagnostic
     * route; ordinary executables still
     * return an integer process status. */
    if (options && (options->native_result_oracle || options->emit_only) &&
        ret_val >= 0 && nyr_value_is_f64(&b, ret_val, 0))
      b.return_class = NYIR_SCALAR_F64;
    else if (options && (options->native_result_oracle || options->emit_only) &&
             ret_val >= 0 && nyr_value_is_f32(&b, ret_val, 0))
      b.return_class = NYIR_SCALAR_F32;
    /* Ordinary execution uses the script's
     * tail value as its status; keep that
     * conversion out of object-link
     * fixtures (which inspect the typed
     * return ABI) and the oracle route
     * (which compares the raw float bits).
     */
    if ((!options || (!options->native_result_oracle && !options->emit_only)) &&
        ret_val >= 0 && nyr_value_is_f64(&b, ret_val, 0)) {
      int as_int =
          nyr_emit_runtime_call(&b, "rt_f64_to_i64", ret_val, -1, -1, 1, 0);
      ret_val = as_int < 0 ? 0 : as_int;
    }
    if ((!options || (!options->native_result_oracle && !options->emit_only)) &&
        ret_val >= 0 &&
        nyr_value_scalar_class(&b, ret_val) == NYIR_SCALAR_I64_TAGGED) {
      ret_val = nyr_emit_runtime_call(&b, "rt_any_to_i64", ret_val, -1, -1, 1,
                                      NYIR_SCALAR_I64_RAW);
      if (ret_val < 0) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        return false;
      }
    }
    if (!nyr_emit_defers(&b, 0)) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
    if (!nyr_emit_ret(&b, ret_val)) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
  }
  bool ok = nyr_finalize(&b, err, err_len);
  if (ok)
    *out = b.nyir;
  else
    nyr_ir_func_free(&b.nyir);
  nyr_builder_dispose(&b);
  return ok;
}

typedef struct {
  const char *filename;
  const char *name;
  const expr_t *init;
  bool occupied;
} ny_dynlist_global_init_entry_t;

typedef struct {
  const program_t *prog;
  const ny_options *opt;
  const stmt_t **funcs;
  size_t count;
  size_t max_funcs;
  bool overflow;
  size_t *seen_fn_slots;
  size_t *seen_name_slots;
  size_t seen_slot_cap;
  /* Function whose body is currently
   * scanned; resolves module-local
   * callbacks.
   */
  const stmt_t *scope_fn;
  /* Top-level statement currently being
   * scanned; scopes dyn-list inference. */
  const stmt_t *query_scope;
  /* Compilation-scoped memo owner shared by
   * all transient reachability probes.
   * Without this, each probe callocs its
   * own top-val/user/attached caches and
   * owned strings, leaking ~GiBs across
   * whole-stdlib scans (std.bc RSS). */
  struct nyr_builder_impl_t *shared_cache;
  /* Library precompile (--emit-module):
   * export-rooted dependencies use the
   * syntax walk, skipping O(n^2)
   * attached/operator/type discovery. */
  bool is_precompile;
  /* Dynamic-list prewarm only needs call
   * sites in user code. Imported stdlib
   * bodies have already been
   * analyzed/compiled and walking them here
   * repeatedly dominates tiny native-only
   * programs. */
  bool is_dynlist_prewarm;
  /* A reachable function can read a module
   * global whose initializer contains
   * additional function references. Track
   * those initializers to bound the
   * recursive dependency walk. */
  const expr_t *scanned_global_initializers[2048];
  size_t scanned_global_initializer_count;
  bool scanning_global_initializer;
  ny_dynlist_global_init_entry_t *dynlist_global_inits;
  size_t dynlist_global_inits_cap;
} ny_native_fn_collector_t;

static bool ny_native_collector_index_init(ny_native_fn_collector_t *col) {
  if (!col || col->seen_slot_cap)
    return col && col->seen_slot_cap;
  if (col->max_funcs == 0 || col->max_funcs > SIZE_MAX / 2)
    return false;
  size_t capacity = 64;
  while (capacity < col->max_funcs * 2) {
    if (capacity > SIZE_MAX / 2)
      return false;
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*col->seen_fn_slots))
    return false;
  size_t *fn_slots = calloc(capacity, sizeof(*fn_slots));
  size_t *name_slots = calloc(capacity, sizeof(*name_slots));
  if (!fn_slots || !name_slots) {
    free(fn_slots);
    free(name_slots);
    return false;
  }
  col->seen_fn_slots = fn_slots;
  col->seen_name_slots = name_slots;
  col->seen_slot_cap = capacity;
  return true;
}

static size_t ny_native_collector_fn_slot(const stmt_t *fn, size_t mask) {
  uintptr_t p = (uintptr_t)fn;
  uint64_t h = (uint64_t)p * UINT64_C(0x9E3779B97F4A7C15);
  return (size_t)((h >> 32) & mask);
}

static size_t ny_native_collector_name_slot(const char *name, size_t mask) {
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    h = (h ^ *p) * 16777619u;
  return (size_t)h & mask;
}

static bool ny_native_collector_has_fn(const ny_native_fn_collector_t *col,
                                       const stmt_t *fn) {
  if (!col || !col->seen_fn_slots || !fn)
    return false;
  size_t mask = col->seen_slot_cap - 1;
  size_t slot = ny_native_collector_fn_slot(fn, mask);
  while (col->seen_fn_slots[slot]) {
    size_t index = col->seen_fn_slots[slot] - 1;
    if (col->funcs[index] == fn)
      return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

static void ny_native_collector_record_fn(ny_native_fn_collector_t *col,
                                          const stmt_t *fn, size_t index) {
  if (!col || !col->seen_fn_slots || !fn)
    return;
  size_t mask = col->seen_slot_cap - 1;
  size_t slot = ny_native_collector_fn_slot(fn, mask);
  while (col->seen_fn_slots[slot])
    slot = (slot + 1) & mask;
  col->seen_fn_slots[slot] = index + 1;
}

static bool ny_native_collector_has_name(const ny_native_fn_collector_t *col,
                                         const char *name) {
  if (!col || !col->seen_name_slots || !name)
    return false;
  size_t mask = col->seen_slot_cap - 1;
  size_t slot = ny_native_collector_name_slot(name, mask);
  while (col->seen_name_slots[slot]) {
    size_t index = col->seen_name_slots[slot] - 1;
    const stmt_t *fn = col->funcs[index];
    if (fn && fn->as.fn.name && strcmp(fn->as.fn.name, name) == 0)
      return true;
    slot = (slot + 1) & mask;
  }
  return false;
}

static void ny_native_collector_record_name(ny_native_fn_collector_t *col,
                                            const char *name, size_t index) {
  if (!col || !col->seen_name_slots || !name)
    return;
  size_t mask = col->seen_slot_cap - 1;
  size_t slot = ny_native_collector_name_slot(name, mask);
  while (col->seen_name_slots[slot])
    slot = (slot + 1) & mask;
  col->seen_name_slots[slot] = index + 1;
}

static void ny_native_collector_add_fn(ny_native_fn_collector_t *col,
                                       const stmt_t *fn) {
  if (!col || !col->funcs || col->max_funcs == 0 || !fn ||
      fn->kind != NY_S_FUNC || !fn->as.fn.name)
    return;
  if (ny_native_collector_index_init(col) &&
      ny_native_collector_has_fn(col, fn))
    return;
  for (size_t i = 0; !col->seen_fn_slots && i < col->count; ++i)
    if (col->funcs[i] == fn)
      return;
  if (col->count < col->max_funcs) {
    size_t index = col->count++;
    col->funcs[index] = fn;
    ny_native_collector_record_fn(col, fn, index);
    ny_native_collector_record_name(col, fn->as.fn.name, index);
  } else
    col->overflow = true;
}

/* An emitted module's exports and attached methods are roots even when no
 * entry-point expression calls them. Private helpers are added transitively
 * from those roots, so consumers get the full callable API without compiling
 * every unreachable implementation detail or self-test. */
static bool ny_native_emit_module_matches(const char *wanted,
                                          const char *module) {
  if (!wanted || !*wanted || !module || !*module)
    return false;
  if (strcmp(wanted, module) == 0)
    return true;
  if ((strcmp(wanted, "std") == 0 || strcmp(wanted, "lib") == 0) &&
      strncmp(module, wanted, strlen(wanted)) == 0 &&
      module[strlen(wanted)] == '.')
    return true;
  if (strcmp(wanted, "core") == 0 && strncmp(module, "core.", 5) == 0)
    return true;
  return false;
}

static bool ny_native_module_exports_function(const stmt_t *module,
                                              const stmt_t *fn) {
  if (!module || module->kind != NY_S_MODULE || !fn || fn->kind != NY_S_FUNC ||
      !fn->as.fn.name)
    return false;
  /* A module without an export list exposes its declarations, just as
   * ordinary imports do. These functions must survive library emission even
   * when no other library function happens to call them. */
  if (module->as.module.export_all || !module_has_export_list(module))
    return true;
  const char *leaf = ny_name_leaf(fn->as.fn.name);
  if (!leaf)
    return false;
  for (size_t i = 0; i < module->as.module.body.len; ++i) {
    const stmt_t *stmt = module->as.module.body.data[i];
    if (!stmt || stmt->kind != NY_S_EXPORT || stmt->as.exprt.is_internal)
      continue;
    for (size_t j = 0; j < stmt->as.exprt.names.len; ++j)
      if (stmt->as.exprt.names.data[j] &&
          strcmp(stmt->as.exprt.names.data[j], leaf) == 0)
        return true;
  }
  return false;
}

static void ny_native_seed_module_functions(ny_native_fn_collector_t *col,
                                            const stmt_t *module,
                                            const stmt_t *stmt,
                                            unsigned depth) {
  if (!col || !module || !stmt || depth > 64)
    return;
  switch (stmt->kind) {
  case NY_S_FUNC:
    if (ny_native_module_exports_function(module, stmt))
      ny_native_collector_add_fn(col, stmt);
    return;
  case NY_S_MODULE:
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      ny_native_seed_module_functions(col, module, stmt->as.block.body.data[i],
                                      depth + 1);
    return;
  case NY_S_IF: {
    bool selected = false;
    if (stmt->as.iff.test &&
        nyr_target_eval_bool(NULL, stmt->as.iff.test, &selected)) {
      ny_native_seed_module_functions(
          col, module, selected ? stmt->as.iff.conseq : stmt->as.iff.alt,
          depth + 1);
    } else {
      ny_native_seed_module_functions(col, module, stmt->as.iff.conseq,
                                      depth + 1);
      ny_native_seed_module_functions(col, module, stmt->as.iff.alt, depth + 1);
    }
    return;
  }
  default:
    return;
  }
}

static void ny_native_seed_module_methods(ny_native_fn_collector_t *col,
                                          const stmt_t *stmt, unsigned depth) {
  if (!stmt || depth > 64)
    return;
  const ny_stmt_list *methods = NULL;
  switch (stmt->kind) {
  case NY_S_IMPL:
    methods = &stmt->as.impl.methods;
    break;
  case NY_S_LAYOUT:
    methods = &stmt->as.layout.methods;
    break;
  case NY_S_STRUCT:
    methods = &stmt->as.struc.methods;
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < stmt->as.module.body.len; ++i)
      ny_native_seed_module_methods(col, stmt->as.module.body.data[i],
                                    depth + 1);
    return;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      ny_native_seed_module_methods(col, stmt->as.block.body.data[i],
                                    depth + 1);
    return;
  case NY_S_IF: {
    bool selected = false;
    if (stmt->as.iff.test &&
        nyr_target_eval_bool(NULL, stmt->as.iff.test, &selected)) {
      ny_native_seed_module_methods(
          col, selected ? stmt->as.iff.conseq : stmt->as.iff.alt, depth + 1);
    } else {
      ny_native_seed_module_methods(col, stmt->as.iff.conseq, depth + 1);
      ny_native_seed_module_methods(col, stmt->as.iff.alt, depth + 1);
    }
    return;
  }
  default:
    return;
  }
  for (size_t i = 0; methods && i < methods->len; ++i)
    ny_native_collector_add_fn(col, methods->data[i]);
}

static void ny_native_seed_emitted_module(const program_t *prog,
                                          const ny_options *opt,
                                          ny_native_fn_collector_t *col) {
  if (!prog || !opt || !opt->emit_module || !*opt->emit_module || !col)
    return;
  ny_native_fn_index_t *index = ny_native_fn_index_for(col->shared_cache);
  for (size_t mi = 0; mi < index->modules.count; ++mi) {
    const stmt_t *module_stmt = index->modules.modules[mi];
    const char *module = module_stmt && module_stmt->kind == NY_S_MODULE
                             ? module_stmt->as.module.name
                             : NULL;
    bool stdlib_module = module_stmt && module_stmt->kind == NY_S_MODULE &&
                         ny_is_stdlib_tok(module_stmt->tok);
    bool emit_stdlib_module = stdlib_module && opt->emit_module &&
                              (strcmp(opt->emit_module, "std") == 0 ||
                               strcmp(opt->emit_module, "lib") == 0);
    if (!emit_stdlib_module &&
        !ny_native_emit_module_matches(opt->emit_module, module))
      continue;
    for (size_t si = 0; si < module_stmt->as.module.body.len; ++si)
      ny_native_seed_module_functions(col, module_stmt,
                                      module_stmt->as.module.body.data[si], 0);
    ny_native_seed_module_methods(col, module_stmt, 0);
  }
}

static const char *ny_native_emitted_fn_name(ny_native_fn_index_t *index,
                                             const program_t *prog,
                                             const ny_options *opt,
                                             const stmt_t *fn) {
  if (!fn || !fn->as.fn.name)
    return fn && fn->as.fn.name ? fn->as.fn.name : "<fn>";
  /* Calls to declarations inside a module use their canonical module name,
   * including private helpers and impl methods.  Always emit the definition
   * under that same name; previously only --emit-module builds prefixed
   * names, leaving ordinary native builds with unresolved calls such as
   * `core.os.parallel._normalize_parallel_mode` and
   * `core.math.big.bigint.ne`. */
  (void)opt;
  const char *module = ny_native_fn_module(index, prog, fn);
  size_t module_len = module ? strlen(module) : 0;
  size_t fn_len = strlen(fn->as.fn.name);
  if (!module || !*module ||
      (fn_len > module_len &&
       strncmp(fn->as.fn.name, module, module_len) == 0 &&
       fn->as.fn.name[module_len] == '.'))
    return fn->as.fn.name;
  static char names[4096][1024];
  static size_t next_name;
  char *out = names[next_name++ % 4096];
  int n = snprintf(out, 1024, "%s.%s", module, fn->as.fn.name);
  return n > 0 && n < 1024 ? out : fn->as.fn.name;
}
static const expr_t *
nyr_find_var_init_in_stmt(const stmt_t *s, const char *name, unsigned depth) {
  if (!s || !name || !*name || depth > 32)
    return NULL;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], name) == 0 && i < s->as.var.exprs.len)
        return s->as.var.exprs.data[i];
    return NULL;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found =
          nyr_find_var_init_in_stmt(s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const expr_t *found =
          nyr_find_var_init_in_stmt(s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_FUNC:
    return nyr_find_var_init_in_stmt(s->as.fn.body, name, depth + 1);
  case NY_S_IF: {
    const expr_t *found =
        nyr_find_var_init_in_stmt(s->as.iff.conseq, name, depth + 1);
    return found ? found
                 : nyr_find_var_init_in_stmt(s->as.iff.alt, name, depth + 1);
  }
  case NY_S_WHILE:
    return nyr_find_var_init_in_stmt(s->as.whl.body, name, depth + 1);
  case NY_S_FOR:
    return nyr_find_var_init_in_stmt(s->as.fr.body, name, depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const expr_t *found = nyr_find_var_init_in_stmt(
          s->as.match.arms.data[i].conseq, name, depth + 1);
      if (found)
        return found;
    }
    return nyr_find_var_init_in_stmt(s->as.match.default_conseq, name,
                                     depth + 1);
  default:
    return NULL;
  }
}

/* A local or formal name shadows a
 * same-named top-level function during
 * reachability collection just as it does
 * during lowering.  Without this check,
 * ordinary parameters such as `kind` and
 * locals such as `bits` can accidentally
 * root unrelated stdlib methods with those
 * names. */
static bool
ny_native_collector_ident_is_local(const ny_native_fn_collector_t *col,
                                   const char *name) {
  if (!col || !col->scope_fn || col->scope_fn->kind != NY_S_FUNC || !name ||
      !*name)
    return false;
  for (size_t i = 0; i < col->scope_fn->as.fn.params.len; ++i) {
    const char *param = col->scope_fn->as.fn.params.data[i].name;
    if (param && strcmp(param, name) == 0)
      return true;
  }
  return nyr_find_var_init_in_stmt(col->scope_fn->as.fn.body, name, 0) != NULL;
}

/* Reachability scanning runs before
 * function lowering has populated lexical
 * local facts.  Recover a receiver's
 * initializer when a member call targets a
 * local, then let the same attached-method
 * resolver used by lowering infer its owner
 * type.  This keeps method reachability
 * aligned with the lowering decision
 * without prebuilding every method that
 * shares a leaf name. */
static const stmt_t *
ny_native_collector_attached_method(ny_native_fn_collector_t *col,
                                    const expr_t *target, const char *method) {
  if (!col || !target || !method || !*method)
    return NULL;
  nyr_builder_impl_t probe = {
      .prog = col->prog,
      .options = col->opt,
      .module_name =
          col->scope_fn
              ? ny_native_fn_module(ny_native_fn_index_for(col->shared_cache),
                                    col->prog, col->scope_fn)
              : NULL,
      /* Type probes used only by
       * reachability must not populate the
       * shared resolver cache under the
       * active function's source key. Their
       * empty lexical-local set is
       * intentionally incomplete; isolating
       * the cache prevents an ambiguous
       * leaf such as `value` from poisoning
       * the real lowering lookup for an
       * attached method. */
      .source_file = NULL,
      .cache_owner = col->shared_cache,
      /* Attached-method reachability must
       * see declared parameter types too.
       * Without the active function, `set
       * items` has no initializer to
       * recover and its `items.add(...)`
       * call is emitted but omitted from
       * the JIT function bundle. */
      .current_fn_name = col->scope_fn && col->scope_fn->kind == NY_S_FUNC
                             ? col->scope_fn->as.fn.name
                             : NULL};
  const stmt_t *attached = nyr_find_attached_method(&probe, target, method);
  if (attached || target->kind != NY_E_IDENT || !target->as.ident.name)
    return attached;
  const expr_t *init = NULL;
  if (col->scope_fn)
    init = nyr_find_var_init_in_stmt(col->scope_fn, target->as.ident.name, 0);
  else
    init = nyr_find_top_level_value(&probe, target->as.ident.name);
  return init ? nyr_find_attached_method(&probe, init, method) : NULL;
}

static const char *ny_native_collector_expr_type(ny_native_fn_collector_t *col,
                                                 const expr_t *expr) {
  if (!col || !expr)
    return NULL;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name && col->scope_fn &&
      col->scope_fn->kind == NY_S_FUNC) {
    for (size_t i = 0; i < col->scope_fn->as.fn.params.len; ++i) {
      const param_t *param = &col->scope_fn->as.fn.params.data[i];
      if (param->name && strcmp(param->name, expr->as.ident.name) == 0 &&
          param->type && *param->type)
        return param->type;
    }
  }
  nyr_builder_impl_t probe = {
      .prog = col->prog,
      .options = col->opt,
      .module_name =
          col->scope_fn
              ? ny_native_fn_module(ny_native_fn_index_for(col->shared_cache),
                                    col->prog, col->scope_fn)
              : NULL,
      .source_file = NULL,
      .cache_owner = col->shared_cache};
  const char *type = nyr_expr_type_name(&probe, expr);
  if (type || expr->kind != NY_E_IDENT || !expr->as.ident.name)
    return type;
  const expr_t *init =
      col->scope_fn
          ? nyr_find_var_init_in_stmt(col->scope_fn, expr->as.ident.name, 0)
          : nyr_find_top_level_value(&probe, expr->as.ident.name);
  return init ? nyr_expr_type_name(&probe, init) : NULL;
}

static void ny_native_add_reachable_fn(ny_native_fn_collector_t *col,
                                       const char *name);

static void ny_native_collector_add_typed_member(ny_native_fn_collector_t *col,
                                                 const expr_t *target,
                                                 const char *method) {
  if (!col || !target || !method || !*method)
    return;
  const char *owner = ny_native_collector_expr_type(col, target);
  if (!owner || !*owner || strcmp(owner, "any") == 0)
    return;
  const char *module =
      col->scope_fn
          ? ny_native_fn_module(ny_native_fn_index_for(col->shared_cache),
                                col->prog, col->scope_fn)
          : NULL;
  char qualified[512];
  int n = strchr(owner, '.') || !module
              ? snprintf(qualified, sizeof(qualified), "%s.%s", owner, method)
              : snprintf(qualified, sizeof(qualified), "%s.%s.%s", module,
                         owner, method);
  if (n > 0 && (size_t)n < sizeof(qualified))
    ny_native_add_reachable_fn(col, qualified);
}

/*
 * Owner-less operator reachability: collect
 * the target body of every `operator <op>`
 * declaration in any impl, regardless of
 * operand-type annotations. Used when the
 * receiver-type probe cannot type the left
 * operand (an intermediate call
 * expression); the lowering's own dispatch
 * stays strictly typed, so over-collection
 * here only costs compile time bounded by
 * impls declaring the same operator symbol.
 */
static const stmt_t *ny_native_impl_method_for_target(const stmt_t *impl,
                                                      const char *target) {
  if (!impl || impl->kind != NY_S_IMPL || !target)
    return NULL;
  const char *leaf = strrchr(target, '.');
  leaf = leaf ? leaf + 1 : target;
  for (size_t i = 0; i < impl->as.impl.methods.len; ++i) {
    const stmt_t *m = impl->as.impl.methods.data[i];
    if (!m || m->kind != NY_S_FUNC || !m->as.fn.name)
      continue;
    const char *mleaf = strrchr(m->as.fn.name, '.');
    mleaf = mleaf ? mleaf + 1 : m->as.fn.name;
    if (strcmp(mleaf, leaf) == 0)
      return m;
  }
  return NULL;
}

static const stmt_t *
ny_native_find_impl_method_for_target(const stmt_t *s, const char *owner,
                                      const char *target,
                                      ny_native_operator_walk_t *walk) {
  if (!s || !target)
    return NULL;
  if (s->kind == NY_S_IMPL &&
      (!owner || (s->as.impl.type_name &&
                  ny_native_type_alias_eq(s->as.impl.type_name, owner)))) {
    const stmt_t *m = ny_native_impl_method_for_target(s, target);
    if (m)
      return m;
    return NULL;
  }
  if (s->kind == NY_S_MODULE) {
    if (!ny_native_operator_walk_mark_module(walk, s))
      return NULL;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const stmt_t *m = ny_native_find_impl_method_for_target(
          s->as.module.body.data[i], owner, target, walk);
      if (m)
        return m;
    }
  }
  return NULL;
}

static void ny_native_collect_impl_op_bodies(ny_native_fn_collector_t *col,
                                             const stmt_t *s, const char *op) {
  if (!col || !s || !op)
    return;
  if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *m = s->as.impl.methods.data[i];
      if (!m || m->kind != NY_S_OPERATOR || !m->as.oper.op ||
          strcmp(m->as.oper.op, op) != 0 || !m->as.oper.target)
        continue;
      const stmt_t *target_fn =
          ny_native_impl_method_for_target(s, m->as.oper.target);
      if (target_fn)
        ny_native_collector_add_fn(col, target_fn);
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_collect_impl_op_bodies(col, s->as.module.body.data[i], op);
    return;
  }
}

/*
 * Collect single-parameter impl methods matching a member name, for member
 * reads that static resolution cannot pin to one body (dynamic receivers
 * later narrowed by flow typing). Mirrors ny_native_collect_impl_op_bodies.
 */
static void ny_native_collect_impl_method_bodies(ny_native_fn_collector_t *col,
                                                 const stmt_t *s,
                                                 const char *method) {
  if (!col || !s || !method || !*method)
    return;
  if (s->kind == NY_S_IMPL) {
    for (size_t i = 0; i < s->as.impl.methods.len; ++i) {
      const stmt_t *m = s->as.impl.methods.data[i];
      const char *leaf;
      if (!m || m->kind != NY_S_FUNC || !m->as.fn.name)
        continue;
      if (m->as.fn.params.len != 1)
        continue;
      leaf = strrchr(m->as.fn.name, '.');
      leaf = leaf ? leaf + 1 : m->as.fn.name;
      if (strcmp(leaf, method) == 0)
        ny_native_collector_add_fn(col, m);
    }
    return;
  }
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_collect_impl_method_bodies(col, s->as.module.body.data[i],
                                           method);
    return;
  }
}

static const stmt_t *ny_native_collector_operator(ny_native_fn_collector_t *col,
                                                  const expr_t *expr) {
  if (!col || !expr || expr->kind != NY_E_BINARY || !expr->as.binary.op)
    return NULL;
  /* Shared typing has already selected the
   * unique top-level operator declaration
   * for this expression. Preserve that
   * identity for reachability too: the
   * legacy AST walk below only searches
   * operators nested in impls, so a valid
   * module-exported operator could lower to
   * a qualified call whose target body was
   * never added to the native function
   * bundle. */
  const stmt_t *selected = expr->semantic.canonical_callee_stmt;
  if (selected && selected->kind == NY_S_OPERATOR && selected->as.oper.target &&
      *selected->as.oper.target) {
    const stmt_t *fn =
        ny_native_operator_target_function(col->shared_cache, selected);
    if (fn) {
      ny_native_collector_add_fn(col, fn);
      return fn;
    }
  }
  const char *owner = ny_native_collector_expr_type(col, expr->as.binary.left);
  const char *right = ny_native_collector_expr_type(col, expr->as.binary.right);
  if (!owner) {
    /*
     * The receiver type probe can fail on
     * intermediate expressions (a call like
     * `Z(2) != Z(3)`), but the impl method
     * that operator dispatch will select at
     * lowering time must still be
     * collected, or the emitted canonical
     * call strandss the VM with an
     * unresolved target. Walk every impl
     * declaring this operator symbol and
     * collect its target bodies;
     * over-collection here is bounded by
     * the impls declaring the operator and
     * costs no ABI decisions (the
     * lowering's own strict dispatch is
     * unchanged).
     */
    const char *op = expr->as.binary.op;
    for (size_t i = 0; i < col->prog->body.len; ++i)
      ny_native_collect_impl_op_bodies(col, col->prog->body.data[i], op);
    return NULL;
  }
  ny_native_operator_walk_t walk = {0};
  for (size_t i = 0; i < col->prog->body.len; ++i) {
    const stmt_t *op = ny_native_find_impl_operator_in_stmt(
        col->prog->body.data[i], owner, expr->as.binary.op, right, &walk);
    if (!op || !op->as.oper.target)
      continue;
    const stmt_t *method = ny_native_collector_attached_method(
        col, expr->as.binary.left, op->as.oper.target);
    if (method) {
      free(walk.modules);
      return method;
    }
    /*
     * The receiver-side attached-method
     * probe can miss an imported impl whose
     * receiver is an intermediate call
     * expression. The declaring impl owns
     * the target method body directly —
     * resolve it there (the global table
     * indexes methods under their AST
     * spelling, not the qualified operator
     * target) or the emitted canonical call
     * strandss the VM with an unresolved
     * target.
     */
    {
      ny_native_operator_walk_t owner_walk = {0};
      for (size_t k = 0; k < col->prog->body.len; ++k) {
        const stmt_t *target_fn = ny_native_find_impl_method_for_target(
            col->prog->body.data[k], owner, op->as.oper.target, &owner_walk);
        if (target_fn) {
          free(owner_walk.modules);
          free(walk.modules);
          ny_native_collector_add_fn(col, target_fn);
          return target_fn;
        }
      }
      free(owner_walk.modules);
    }
  }
  free(walk.modules);
  return NULL;
}
static bool nyr_expr_has_dyn_list_shape(const program_t *prog, const expr_t *e,
                                        unsigned depth);

static const expr_t *nyr_find_global_var_init(const stmt_t *s, const char *name,
                                              const char *filename,
                                              unsigned depth) {
  if (!s || !name || !*name || depth > 32)
    return NULL;
  switch (s->kind) {
  case NY_S_VAR:
    if (filename && s->tok.filename && strcmp(filename, s->tok.filename) != 0)
      return NULL;
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], name) == 0 && i < s->as.var.exprs.len)
        return s->as.var.exprs.data[i];
    return NULL;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const expr_t *found = nyr_find_global_var_init(s->as.block.body.data[i],
                                                     name, filename, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const expr_t *found = nyr_find_global_var_init(s->as.module.body.data[i],
                                                     name, filename, depth + 1);
      if (found)
        return found;
    }
    return NULL;
  default:
    return NULL;
  }
}

static size_t nyr_dynlist_global_init_hash(const char *filename,
                                           const char *name) {
  size_t hash = (size_t)1469598103934665603ULL;
  const char *parts[] = {filename ? filename : "", name ? name : ""};
  for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p) {
    for (const unsigned char *s = (const unsigned char *)parts[p]; *s; ++s) {
      hash ^= *s;
      hash *= (size_t)1099511628211ULL;
    }
    hash ^= (unsigned char)0xff;
    hash *= (size_t)1099511628211ULL;
  }
  return hash;
}

static const expr_t *nyr_find_global_var_init_in_program(const program_t *prog,
                                                         const char *name,
                                                         const char *filename) {
  if (!prog)
    return NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const expr_t *init =
        nyr_find_global_var_init(prog->body.data[i], name, filename, 0);
    if (init)
      return init;
  }
  return NULL;
}

static const expr_t *
nyr_find_global_var_init_cached(ny_native_fn_collector_t *col,
                                const program_t *prog, const char *name,
                                const char *filename) {
  if (!col || !col->dynlist_global_inits || !col->dynlist_global_inits_cap ||
      (col->dynlist_global_inits_cap & (col->dynlist_global_inits_cap - 1)) !=
          0)
    return nyr_find_global_var_init_in_program(prog, name, filename);
  size_t mask = col->dynlist_global_inits_cap - 1;
  size_t slot = nyr_dynlist_global_init_hash(filename, name) & mask;
  for (size_t probe = 0; probe < col->dynlist_global_inits_cap; ++probe) {
    ny_dynlist_global_init_entry_t *entry =
        &col->dynlist_global_inits[(slot + probe) & mask];
    if (!entry->occupied) {
      const expr_t *init =
          nyr_find_global_var_init_in_program(prog, name, filename);
      *entry = (ny_dynlist_global_init_entry_t){
          .filename = filename, .name = name, .init = init, .occupied = true};
      return init;
    }
    if ((!filename && !entry->filename) ||
        (filename && entry->filename &&
         strcmp(filename, entry->filename) == 0)) {
      if (name && entry->name && strcmp(name, entry->name) == 0)
        return entry->init;
    }
  }
  return nyr_find_global_var_init_in_program(prog, name, filename);
}

static bool nyr_expr_has_dyn_list_shape_in_scope(const program_t *prog,
                                                 ny_native_fn_collector_t *col,
                                                 const stmt_t *scope,
                                                 const expr_t *e,
                                                 unsigned depth) {
  if (!e || depth > 32)
    return false;
  if (e->kind == NY_E_IDENT && prog) {
    const expr_t *init = nyr_find_var_init_in_stmt(scope, e->as.ident.name, 0);
    if (init && e->tok.filename && init->tok.filename &&
        strcmp(e->tok.filename, init->tok.filename) != 0)
      init = NULL;
    if (!init) {
      init = nyr_find_global_var_init_cached(col, prog, e->as.ident.name,
                                             e->tok.filename);
    }
    if (init && init != e)
      return nyr_expr_has_dyn_list_shape_in_scope(prog, col, scope, init,
                                                  depth + 1);
    return false;
  }
  return nyr_expr_has_dyn_list_shape(prog, e, depth);
}

static bool nyr_expr_has_dyn_list_shape(const program_t *prog, const expr_t *e,
                                        unsigned depth) {
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
      const expr_t *init = nyr_find_var_init_in_stmt(
          prog->body.data[i], e->as.ident.name, depth + 1);
      if (init && nyr_expr_has_dyn_list_shape(prog, init, depth + 1))
        return true;
    }
  }
  return false;
}

static void ny_native_add_reachable_fn(ny_native_fn_collector_t *col,
                                       const char *name);
static bool ny_native_add_semantic_callee(ny_native_fn_collector_t *col,
                                          const expr_t *expr);
static bool nyr_qualified_expr(const expr_t *e, char *out, size_t out_len);
static bool ny_native_scan_qualified_call(ny_native_fn_collector_t *col,
                                          const expr_t *target,
                                          const char *method) {
  if (!col || !target || !method || !*method)
    return false;
  char qualified[512];
  if (!nyr_qualified_expr(target, qualified, sizeof(qualified)))
    return false;
  size_t used = strlen(qualified);
  if (used + 1 + strlen(method) >= sizeof(qualified))
    return false;
  qualified[used++] = '.';
  memcpy(qualified + used, method, strlen(method) + 1);
  const char *dot = strchr(qualified, '.');
  if (!dot || dot == qualified)
    return false;
  char alias[256];
  size_t alias_len = (size_t)(dot - qualified);
  if (alias_len >= sizeof(alias))
    return false;
  memcpy(alias, qualified, alias_len);
  alias[alias_len] = '\0';
  nyr_builder_impl_t probe = {
      .prog = col->prog,
      .options = col->opt,
      .module_name =
          col->scope_fn
              ? ny_native_fn_module(ny_native_fn_index_for(col->shared_cache),
                                    col->prog, col->scope_fn)
              : NULL,
      .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL,
      .cache_owner = col->shared_cache};
  probe.source_file = target->tok.filename;
  const stmt_t *declared = nyr_find_user_function(&probe, qualified);
  if (declared && !ny_is_stdlib_tok(declared->tok)) {
    ny_native_collector_add_fn(col, declared);
    return true;
  }
  const char *module = nyr_resolve_use_alias(&probe, alias);
  if (!module) {
    /* The first lookup also finds stdlib declarations. Reuse that result
     * when the spelling is not an import alias instead of walking the same
     * function index and source scopes a second time. */
    const stmt_t *fn =
        declared ? declared : nyr_find_user_function(&probe, qualified);
    if (!fn)
      return false;
    ny_native_collector_add_fn(col, fn);
    return true;
  }
  char canonical[512];
  int n = snprintf(canonical, sizeof(canonical), "%s%s", module, dot);
  if (n < 0 || (size_t)n >= sizeof(canonical))
    return false;
  const stmt_t *target_fn = nyr_find_user_function(&probe, canonical);
  if (target_fn && target_fn->kind == NY_S_FUNC &&
      !target_fn->as.fn.is_extern && !target_fn->as.fn.link_name) {
    expr_t resolved = {0};
    resolved.semantic.canonical_callee = canonical;
    resolved.semantic.canonical_callee_stmt = (stmt_t *)target_fn;
    if (ny_native_add_semantic_callee(col, &resolved))
      return true;
  }
  ny_native_add_reachable_fn(col, canonical);
  return true;
}
static void ny_native_scan_expr_for_calls(const expr_t *e,
                                          ny_native_fn_collector_t *col);
static void ny_native_scan_stmt_for_calls(const stmt_t *s,
                                          ny_native_fn_collector_t *col);
static void ny_native_dynlist_note_call(nyr_builder_impl_t *cache_owner,
                                        const char *callee_name,
                                        size_t arg_index);

static bool nyr_stdlib_call_lowered_inline(const char *name) {
  if (!name || !*name)
    return false;
  const char *leaf = strrchr(name, '.');
  leaf = leaf ? leaf + 1 : name;
  if (strcmp(leaf, "to_str") == 0 || strcmp(leaf, "__to_str") == 0 ||
      strcmp(leaf, "abs") == 0 || strcmp(leaf, "lerp") == 0 ||
      strcmp(leaf, "min") == 0 || strcmp(leaf, "max") == 0 ||
      strcmp(leaf, "prove") == 0 || strcmp(leaf, "static_assert") == 0 ||
      strcmp(leaf, "memchr") == 0 || strcmp(leaf, "memcmp") == 0 ||
      strcmp(leaf, "memcpy") == 0 || strcmp(leaf, "memset") == 0 ||
      strcmp(leaf, "strchr") == 0 || strcmp(leaf, "strcmp") == 0)
    return true;
  return false;
}

/* Single-flow buffer for the
 * NY_TRACE_NATIVE_REACHABLE report: names
 * are appended here per reachable function
 * and flushed as one wrapped line at the
 * collector summary (compact wide output
 * instead of one line per fn). */
static char ny_reach_flow[16384];
static void ny_native_add_reachable_fn(ny_native_fn_collector_t *col,
                                       const char *name) {
  if (!col || !col->funcs || col->max_funcs == 0 || !name || !*name)
    return;
  if (ny_native_collector_index_init(col) &&
      ny_native_collector_has_name(col, name)) {
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE_MISSES")) {
      const char *from = col->scope_fn && col->scope_fn->as.fn.name
                             ? col->scope_fn->as.fn.name
                             : "<entry-roots>";
      ny_trace_line("NATIVE_REACHABLE_MISSES",
                    "native-reach-skip %s -> %s: already collected by name",
                    from, name);
    }
    return;
  }
  for (size_t i = 0; !col->seen_name_slots && i < col->count; ++i) {
    if (col->funcs[i] && col->funcs[i]->as.fn.name &&
        strcmp(col->funcs[i]->as.fn.name, name) == 0)
      return;
  }
  nyr_builder_impl_t probe = {
      .prog = col->prog,
      .options = col->opt,
      .module_name =
          col->scope_fn
              ? ny_native_fn_module(ny_native_fn_index_for(col->shared_cache),
                                    col->prog, col->scope_fn)
              : NULL,
      .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL,
      .cache_owner = col->shared_cache};
  const stmt_t *fn = nyr_find_user_function(&probe, name);
  if (!fn) {
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE_MISSES")) {
      const char *from = col->scope_fn && col->scope_fn->as.fn.name
                             ? col->scope_fn->as.fn.name
                             : "<entry-roots>";
      ny_trace_line(
          "NATIVE_REACHABLE_MISSES",
          "native-reach-skip %s -> %s: unresolved in module=%s source=%s", from,
          name, probe.module_name ? probe.module_name : "<none>",
          probe.source_file ? probe.source_file : "<none>");
    }
    return;
  }
  if (fn->as.fn.is_extern || fn->as.fn.link_name) {
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE_MISSES")) {
      const char *from = col->scope_fn && col->scope_fn->as.fn.name
                             ? col->scope_fn->as.fn.name
                             : "<entry-roots>";
      ny_trace_line("NATIVE_REACHABLE_MISSES",
                    "native-reach-skip %s -> %s: external declaration", from,
                    name);
    }
    return;
  }
  /* Public names with a raw runtime bridge
   * do not need their stdlib bodies. A
   * source function is different: it
   * deliberately shadows that bridge and
   * must be collected under the normal
   * native-function symbol.  Inert
   * TUI/audio stubs are the exception in
   * the other direction: the stdlib defines
   * the real implementation, so its body
   * must be collected instead of silently
   * calling the no-op bridge. */
  const char *fn_module = ny_native_fn_module(
      ny_native_fn_index_for(col->shared_cache), col->prog, fn);
  const char *fn_leaf = ny_name_leaf(fn->as.fn.name);
  bool core_runtime_module =
      fn_module && (strcmp(fn_module, "core") == 0 ||
                    strncmp(fn_module, "core.", sizeof("core.") - 1) == 0);
  bool boxed_float_converter = fn_module &&
                               strcmp(fn_module, "core.math.float") == 0 &&
                               fn_leaf && strcmp(fn_leaf, "float") == 0;
  if (strcmp(name, "core.math.float.floor") == 0 ||
      strcmp(name, "core.math.float.ceil") == 0 ||
      strcmp(name, "core.math.float.round") == 0)
    ny_native_add_reachable_fn(col, "core.math.float.float");
  if (nyr_runtime_symbol(name) && ny_is_stdlib_tok(fn->tok) &&
      core_runtime_module && !boxed_float_converter &&
      !ny_native_runtime_stub_yields_to_source(nyr_runtime_symbol(name)))
    return;
  /* `float(i)` inside the boxed core.math.float floor/ceil implementations
   * is an ordinary call to the module's boxed converter.  The frontend also
   * recognizes `float` as a native scalar leaf, so its dependency edge is
   * absent from the syntax walk even though these bodies call it directly.
   * Keep the boxed converter body reachable with those callers. */
  if (fn_module && strcmp(fn_module, "core.math.float") == 0 &&
      fn->as.fn.name &&
      (strcmp(fn->as.fn.name, "floor") == 0 ||
       strcmp(fn->as.fn.name, "ceil") == 0 ||
       strcmp(fn->as.fn.name, "round") == 0))
    ny_native_add_reachable_fn(col, "core.math.float.float");
  /*
   * Native leaf / builtin-alloc names
   * (print, addr_of, borrow, float, malloc,
   * free, ...) are lowered directly to
   * runtime calls in the shared-NYIR path,
   * so their stdlib bodies must not be
   * force-built: those bodies use NY_E_LIST
   * / NY_E_MEMBER constructs the shared
   * lowerer does not support yet.  `zalloc`
   * is retained because modules also pass
   * it as a function value.
   */
  const char *leaf = ny_name_leaf(name);
  if (ny_is_stdlib_tok(fn->tok) && core_runtime_module &&
      ((!boxed_float_converter &&
        ny_native_leaf_kind(leaf) != NY_NATIVE_LEAF_NONE) ||
       (ny_builtin_alloc_kind(leaf) != NY_BUILTIN_ALLOC_NONE &&
        strcmp(leaf, "zalloc") != 0) ||
       nyr_stdlib_call_lowered_inline(name))) {
    return;
  }
  if (ny_native_collector_index_init(col) &&
      ny_native_collector_has_name(col, fn->as.fn.name)) {
    return;
  }
  for (size_t i = 0; !col->seen_name_slots && i < col->count; ++i) {
    if (col->funcs[i] == fn ||
        (col->funcs[i]->as.fn.name && fn->as.fn.name &&
         strcmp(col->funcs[i]->as.fn.name, fn->as.fn.name) == 0)) {
      return;
    }
  }
  if (col->count < col->max_funcs) {
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE_"
                         "EDGES")) {
      const char *from = col->scope_fn && col->scope_fn->as.fn.name
                             ? col->scope_fn->as.fn.name
                             : "<entry-roots>";
      ny_trace_line("NATIVE_REACHABLE_EDGES", "native-reach %s -> %s", from,
                    name);
    }
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE")) {
      /* Compact single-flow report: buffer
       * the names here and print one
       * wrapped line at the collector
       * summary instead of one line per
       * function (hundreds of lines for a
       * hello-world at -vvvvv). */
      size_t len = strlen(ny_reach_flow);
      if (len < sizeof(ny_reach_flow) - 64) {
        if (len)
          ny_reach_flow[len++] = ' ';
        for (const char *p2 = name; *p2 && len < sizeof(ny_reach_flow) - 2;
             ++p2)
          ny_reach_flow[len++] = *p2;
        ny_reach_flow[len] = '\0';
      }
    }
    size_t index = col->count++;
    col->funcs[index] = fn;
    ny_native_collector_record_fn(col, fn, index);
    ny_native_collector_record_name(col, fn->as.fn.name, index);
  } else
    col->overflow = true;
  /* The scoped resolver above selects the
   * body that lowering will call. Rooting
   * every stdlib declaration with the same
   * unqualified spelling defeats
   * reachability for common helpers (and
   * can pull large unrelated math/crypto
   * dependency graphs into an otherwise
   * small program). Canonical semantic
   * callees and attached methods are added
   * explicitly at their call sites;
   * unresolved dynamic calls remain on
   * their generic path. */
}

/* A resolved call already identifies its declaration. Keep that identity
 * through reachability collection: suffix lookup by the canonical name can
 * select a same-leaf attached method instead of the imported free function. */
static bool ny_native_add_semantic_callee(ny_native_fn_collector_t *col,
                                          const expr_t *expr) {
  const stmt_t *fn =
      expr && expr->semantic.canonical_callee_stmt &&
              expr->semantic.canonical_callee_stmt->kind == NY_S_FUNC
          ? expr->semantic.canonical_callee_stmt
          : NULL;
  if (!fn)
    return false;
  if (fn->as.fn.is_extern || fn->as.fn.link_name)
    return true;

  const char *name = expr->semantic.canonical_callee;
  if (ny_is_stdlib_tok(fn->tok)) {
    const char *module = ny_native_fn_module(
        ny_native_fn_index_for(col->shared_cache), col->prog, fn);
    bool core_runtime_module =
        module && (strcmp(module, "core") == 0 ||
                   strncmp(module, "core.", sizeof("core.") - 1) == 0);
    const char *runtime = name ? nyr_runtime_symbol(name) : NULL;
    if (runtime && core_runtime_module &&
        !ny_native_runtime_stub_yields_to_source(runtime))
      return true;
    const char *leaf = ny_name_leaf(fn->as.fn.name);
    bool boxed_float_converter = module &&
                                 strcmp(module, "core.math.float") == 0 &&
                                 leaf && strcmp(leaf, "float") == 0;
    if (core_runtime_module &&
        ((!boxed_float_converter && leaf &&
          ny_native_leaf_kind(leaf) != NY_NATIVE_LEAF_NONE) ||
         (leaf && ny_builtin_alloc_kind(leaf) != NY_BUILTIN_ALLOC_NONE &&
          strcmp(leaf, "zalloc") != 0) ||
         (name && nyr_stdlib_call_lowered_inline(name))))
      return true;
  }
  ny_native_collector_add_fn(col, fn);
  if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE_EDGES")) {
    const char *from = col->scope_fn && col->scope_fn->as.fn.name
                           ? col->scope_fn->as.fn.name
                           : "<entry-roots>";
    ny_trace_line("NATIVE_REACHABLE_EDGES", "native-reach %s -> %s", from,
                  name ? name : fn->as.fn.name);
  }
  return true;
}

/* Synthetic lambda functions are emitted before the enclosing function is
 * lowered.  Discover lexical locals here, while the enclosing function is
 * available, so the lambda's hidden environment ABI is fixed before codegen.
 * The closure materialization path still completes the capture list for
 * unusual contexts, but that late discovery cannot change an already-built
 * lambda's parameter layout. */
static void ny_native_lambda_capture_locals(const stmt_t *s,
                                            const expr_t *lambda_expr,
                                            ny_native_lambda_entry_t *entry,
                                            ny_native_fn_collector_t *col,
                                            unsigned depth) {
  if (!s || !lambda_expr || !entry || !col || depth > 64)
    return;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name || ny_native_lambda_param_named(lambda_expr, name) ||
          !nyr_stmt_uses_ident(lambda_expr->as.lambda.body, name))
        continue;
      /* Do not capture a same-named declaration that appears after the
       * closure.  AST nodes from one function share the source file and carry
       * stable line/column positions. */
      if (s->tok.filename && lambda_expr->tok.filename &&
          strcmp(s->tok.filename, lambda_expr->tok.filename) == 0 &&
          (s->tok.line > lambda_expr->tok.line ||
           (s->tok.line == lambda_expr->tok.line &&
            s->tok.col > lambda_expr->tok.col)))
        continue;
      if (!ny_native_lambda_capture_add(entry, name))
        return;
      size_t ci = 0;
      while (ci < entry->capture_count &&
             strcmp(entry->capture_names[ci], name) != 0)
        ++ci;
      if (ci >= entry->capture_count || i >= s->as.var.exprs.len)
        continue;
      const expr_t *init = s->as.var.exprs.data[i];
      if (!init)
        continue;
      nyr_builder_impl_t probe = {
          .prog = col->prog,
          .options = col->opt,
          .module_name = col->scope_fn
                             ? ny_native_fn_module(
                                   ny_native_fn_index_for(col->shared_cache),
                                   col->prog, col->scope_fn)
                             : NULL,
          .source_file = s->tok.filename,
          .cache_owner = col->shared_cache};
      nyr_local_t *dst = &entry->capture_types[ci];
      memset(dst, 0, sizeof(*dst));
      dst->is_list = nyr_expr_is_list(&probe, init);
      dst->is_dyn_list = nyr_expr_is_dyn_list(&probe, init);
      dst->is_cstr = nyr_expr_is_cstr(&probe, (expr_t *)init);
      dst->is_bytes = nyr_expr_is_bytes(&probe, init);
      dst->is_dict = nyr_expr_is_dict(&probe, init);
      dst->is_any = nyr_expr_is_any(&probe, (expr_t *)init);
      dst->is_f64 = nyr_expr_is_f64(&probe, init);
      dst->is_f32 = nyr_expr_is_f32(&probe, init);
      dst->is_bool = nyr_expr_is_bool(&probe, (expr_t *)init);
      if (init->semantic.resolved) {
        dst->semantic_rep = init->semantic.rep;
        dst->semantic_ownership = init->semantic.ownership;
        dst->alias_class = init->semantic.alias_class;
        dst->semantic_mutable = init->semantic.mutable_value;
      } else if ((init->kind == NY_E_LITERAL &&
                  init->as.literal.kind == NY_LIT_INT &&
                  init->tok.kind != NY_T_NIL) ||
                 nyr_expr_is_raw_int(init)) {
        dst->semantic_rep = NY_SEM_REP_RAW_INT;
      }
    }
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_lambda_capture_locals(s->as.block.body.data[i], lambda_expr,
                                      entry, col, depth + 1);
    break;
  case NY_S_IF:
    ny_native_lambda_capture_locals(s->as.iff.init, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.iff.conseq, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.iff.alt, lambda_expr, entry, col,
                                    depth + 1);
    break;
  case NY_S_WHILE:
    ny_native_lambda_capture_locals(s->as.whl.init, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.whl.body, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.whl.update, lambda_expr, entry, col,
                                    depth + 1);
    break;
  case NY_S_FOR:
    ny_native_lambda_capture_locals(s->as.fr.init, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.fr.body, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.fr.update, lambda_expr, entry, col,
                                    depth + 1);
    break;
  case NY_S_TRY:
    ny_native_lambda_capture_locals(s->as.tr.body, lambda_expr, entry, col,
                                    depth + 1);
    ny_native_lambda_capture_locals(s->as.tr.handler, lambda_expr, entry, col,
                                    depth + 1);
    break;
  case NY_S_GUARD:
    ny_native_lambda_capture_locals(s->as.guard.fallback, lambda_expr, entry,
                                    col, depth + 1);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      ny_native_lambda_capture_locals(s->as.match.arms.data[i].conseq,
                                      lambda_expr, entry, col, depth + 1);
    ny_native_lambda_capture_locals(s->as.match.default_conseq, lambda_expr,
                                    entry, col, depth + 1);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_lambda_capture_locals(s->as.module.body.data[i], lambda_expr,
                                      entry, col, depth + 1);
    break;
  default:
    break;
  }
}

static void ny_native_scan_expr_for_calls(const expr_t *e,
                                          ny_native_fn_collector_t *col) {
  if (!e || !col)
    return;
  /* Precompiles must still follow identifier references to function values
   * and module-global initializers through the scoped, cycle-bounded path
   * below. The remaining fast paths only need a syntax walk. Prewarm records
   * call argument shapes;
   * running reachability and operator
   * overload discovery here repeats
   * whole-program work for every expression
   * and can dominate compilation for
   * ordinary function bodies. */
  if ((col->is_precompile && e->kind != NY_E_IDENT) ||
      col->is_dynlist_prewarm) {
    switch (e->kind) {
    case NY_E_CALL:
      if (col->is_precompile) {
        const stmt_t *resolved = e->semantic.canonical_callee_stmt;
        if (resolved && resolved->kind == NY_S_FUNC &&
            !resolved->as.fn.is_extern && !resolved->as.fn.link_name) {
          ny_native_collector_add_fn(col, resolved);
        } else if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
                   e->as.call.callee->as.ident.name) {
          ny_native_add_reachable_fn(col, e->as.call.callee->as.ident.name);
        } else if (e->as.call.callee &&
                   e->as.call.callee->kind == NY_E_MEMBER) {
          bool qualified = ny_native_scan_qualified_call(
              col, e->as.call.callee->as.member.target,
              e->as.call.callee->as.member.name);
          if (!qualified && e->semantic.canonical_callee)
            ny_native_add_reachable_fn(col, e->semantic.canonical_callee);
        }
      }
      if (col->is_dynlist_prewarm && e->as.call.callee &&
          e->as.call.callee->kind == NY_E_IDENT &&
          e->as.call.callee->as.ident.name) {
        for (size_t ai = 0; ai < e->as.call.args.len; ++ai)
          if (nyr_expr_has_dyn_list_shape_in_scope(
                  col->prog, col, col->query_scope,
                  e->as.call.args.data[ai].val, 0))
            ny_native_dynlist_note_call(col->shared_cache,
                                        e->as.call.callee->as.ident.name, ai);
      }
      if (!col->is_dynlist_prewarm && e->as.call.callee &&
          (e->as.call.callee->kind == NY_E_FN ||
           e->as.call.callee->kind == NY_E_LAMBDA)) {
        stmt_t *lambda = ny_native_lambda_create(
            ny_native_lambda_cache_for(col->shared_cache), e->as.call.callee);
        if (lambda)
          ny_native_collector_add_fn(col, lambda);
      }
      if (e->as.call.callee)
        ny_native_scan_expr_for_calls(e->as.call.callee, col);
      for (size_t i = 0; i < e->as.call.args.len; ++i) {
        const expr_t *arg = e->as.call.args.data[i].val;
        if (!col->is_dynlist_prewarm && arg && arg->semantic.resolved &&
            arg->semantic.rep == NY_SEM_REP_CLOSURE) {
          if (arg->kind == NY_E_FN || arg->kind == NY_E_LAMBDA) {
            stmt_t *lambda = ny_native_lambda_create(
                ny_native_lambda_cache_for(col->shared_cache), arg);
            if (lambda)
              ny_native_collector_add_fn(col, lambda);
          }
        }
        ny_native_scan_expr_for_calls(arg, col);
      }
      break;
    case NY_E_MEMCALL:
      if (col->is_precompile) {
        const stmt_t *resolved = e->semantic.canonical_callee_stmt;
        if (resolved && resolved->kind == NY_S_FUNC &&
            !resolved->as.fn.is_extern && !resolved->as.fn.link_name) {
          ny_native_collector_add_fn(col, resolved);
        } else if (e->as.memcall.target && e->as.memcall.name) {
          bool qualified = ny_native_scan_qualified_call(
              col, e->as.memcall.target, e->as.memcall.name);
          if (!qualified) {
            /* Module artifacts seed every attached method in the selected
             * module set. Re-resolving each unannotated method receiver here
             * performs repeated local type inference across the entire
             * stdlib without adding any new roots. */
            const bool methods_are_seeded =
                col->opt && col->opt->emit_module && *col->opt->emit_module;
            if (!methods_are_seeded) {
              const stmt_t *attached = ny_native_collector_attached_method(
                  col, e->as.memcall.target, e->as.memcall.name);
              if (attached)
                ny_native_collector_add_fn(col, attached);
            }
            if (!qualified && e->semantic.canonical_callee)
              ny_native_add_reachable_fn(col, e->semantic.canonical_callee);
          }
        }
      }
      ny_native_scan_expr_for_calls(e->as.memcall.target, col);
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        ny_native_scan_expr_for_calls(e->as.memcall.args.data[i].val, col);
      break;
    case NY_E_MEMBER:
      ny_native_scan_expr_for_calls(e->as.member.target, col);
      break;
    case NY_E_UNARY:
      ny_native_scan_expr_for_calls(e->as.unary.right, col);
      break;
    case NY_E_BINARY:
    case NY_E_LOGICAL:
      ny_native_scan_expr_for_calls(
          e->kind == NY_E_BINARY ? e->as.binary.left : e->as.logical.left, col);
      ny_native_scan_expr_for_calls(
          e->kind == NY_E_BINARY ? e->as.binary.right : e->as.logical.right,
          col);
      break;
    case NY_E_TERNARY:
      ny_native_scan_expr_for_calls(e->as.ternary.cond, col);
      ny_native_scan_expr_for_calls(e->as.ternary.true_expr, col);
      ny_native_scan_expr_for_calls(e->as.ternary.false_expr, col);
      break;
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
      stmt_t match_stmt = {
          .kind = NY_S_MATCH, .tok = e->tok, .as = {.match = e->as.match}};
      size_t static_arm = 0;
      int static_selection = nyr_static_match_arm(&match_stmt, &static_arm);
      if (static_selection == 1) {
        ny_native_scan_stmt_for_calls(e->as.match.arms.data[static_arm].conseq,
                                      col);
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
      if (!col->is_dynlist_prewarm) {
        stmt_t *lambda = ny_native_lambda_create(
            ny_native_lambda_cache_for(col->shared_cache), e);
        if (lambda)
          ny_native_collector_add_fn(col, lambda);
      }
      ny_native_scan_stmt_for_calls(e->as.lambda.body, col);
      break;
    }
    case NY_E_COMPTIME:
      ny_native_scan_stmt_for_calls(e->as.comptime_expr.body, col);
      break;
    case NY_E_FSTRING:
      for (size_t fi = 0; fi < e->as.fstring.parts.len; ++fi)
        if (e->as.fstring.parts.data[fi].kind == NY_FSP_EXPR)
          ny_native_scan_expr_for_calls(e->as.fstring.parts.data[fi].as.e, col);
      break;
    default:
      break;
    }
    return;
  }
  switch (e->kind) {
  case NY_E_IDENT:
    /*
     * A function value referenced as data
     * (type constructor, stored closure,
     * callback, resolved sysconst) is still
     * reachable code: emit it so the object
     * encoder can resolve the
     * ADDR_SYMBOL/CALL reloc it produces.
     */
    if (e->as.ident.name && col->funcs && col->max_funcs > 0) {
      bool local_binding =
          ny_native_collector_ident_is_local(col, e->as.ident.name);
      nyr_builder_impl_t source_probe = {.prog = col->prog,
                                         .options = col->opt,
                                         .source_file = e->tok.filename,
                                         .cache_owner = col->shared_cache};
      if (!local_binding) {
        const stmt_t *source_fn =
            nyr_find_user_function(&source_probe, e->as.ident.name);
        if (source_fn && !ny_is_stdlib_tok(source_fn->tok) &&
            !source_fn->as.fn.is_extern && !source_fn->as.fn.link_name)
          ny_native_collector_add_fn(col, source_fn);
      }
      /* A callback may be stored in an
       * `any` dictionary before it is
       * called. In that case semantic
       * analysis does not always retain the
       * closure representation, but the
       * identifier still names a real
       * program function and its
       * ADDR_SYMBOL must keep the body
       * reachable. */
      bool is_fn = false;
      if (e->semantic.resolved) {
        is_fn = (e->semantic.rep == NY_SEM_REP_CLOSURE);
        /*
         * A bare function-value ident can
         * carry a stale/conservative
         * semantic rep (module-level value
         * tables leak across modules and a
         * handler named _induction resolves
         * to an object rep).  When the name
         * still resolves to a real program
         * function, the body must stay
         * reachable: the emitted
         * ADDR_SYMBOL references it. Stdlib
         * definitions are collected here
         * too — calls already pull stdlib
         * helpers in, and excluding them
         * left window/cocoa callbacks
         * (registered as values from
         * collected stdlib bodies) with
         * emitted references but no
         * definitions.
         */
        if (!is_fn && e->as.ident.name) {
          bool known = false;
          const stmt_t *cached = ny_native_fn_resolve_cache_get(
              col->shared_cache, e->as.ident.name, NULL, &known);
          if (!known)
            cached = ny_native_fn_cache_lookup_exact_with_builder(
                col->shared_cache, e->as.ident.name);
          is_fn = cached != NULL;
        }
      } else {
        bool known = false;
        const stmt_t *cached = ny_native_fn_resolve_cache_get(
            col->shared_cache, e->as.ident.name, NULL, &known);
        if (known) {
          is_fn = (cached != NULL);
        } else {
          const stmt_t *fn = ny_native_fn_cache_lookup_exact_with_builder(
              col->shared_cache, e->as.ident.name);
          ny_native_fn_resolve_cache_put(col->shared_cache, e->as.ident.name,
                                         NULL, fn);
          is_fn = fn != NULL;
        }
      }
      if (is_fn && !local_binding) {
        ny_native_add_reachable_fn(col, e->as.ident.name);
      } else if (!is_fn && !local_binding && e->as.ident.name) {
        /*
         * A bare handler ident inside a
         * module (standard_registry passing
         * _induction to register) can carry
         * a conservative object semantic
         * rep while the lowerer still
         * resolves it through the
         * module-aware user-function finder
         * and emits the qualified ny_fn_
         * symbol.  Use the same resolver
         * here so the referenced body is
         * collected.
         */
        nyr_builder_impl_t probe = {
            .prog = col->prog,
            .options = col->opt,
            .module_name = col->scope_fn
                               ? ny_native_fn_module(
                                     ny_native_fn_index_for(col->shared_cache),
                                     col->prog, col->scope_fn)
                               : NULL,
            .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL,
            .cache_owner = col->shared_cache};
        const stmt_t *target = nyr_find_user_function(&probe, e->as.ident.name);
        if (target && target->kind == NY_S_FUNC && target->as.fn.name &&
            !target->as.fn.is_extern && !target->as.fn.link_name)
          ny_native_add_reachable_fn(col, target->as.fn.name);

        /* A referenced global's initializer
         * is executable code as well. It
         * may call a private helper that
         * has no reference in a function
         * body, so follow only the binding
         * named at this use site. */
        const char *module = probe.module_name;
        const expr_t *initializer =
            probe.source_file ? nyr_find_top_level_value_in_source(
                                    &probe, e->as.ident.name, probe.source_file)
                              : NULL;
        if (!initializer && module) {
          char qualified[512];
          int qn = snprintf(qualified, sizeof(qualified), "%s.%s", module,
                            e->as.ident.name);
          if (qn > 0 && (size_t)qn < sizeof(qualified) && probe.source_file)
            initializer = nyr_find_top_level_value_in_source(&probe, qualified,
                                                             probe.source_file);
          if (!initializer && qn > 0 && (size_t)qn < sizeof(qualified))
            initializer = nyr_find_imported_value(&probe, qualified);
        }
        if (!initializer)
          initializer = nyr_find_imported_value(&probe, e->as.ident.name);
        bool scanned = false;
        if (initializer && initializer != e) {
          for (size_t i = 0; i < col->scanned_global_initializer_count; ++i)
            if (col->scanned_global_initializers[i] == initializer) {
              scanned = true;
              break;
            }
          if (!scanned && col->scanned_global_initializer_count <
                              sizeof(col->scanned_global_initializers) /
                                  sizeof(col->scanned_global_initializers[0])) {
            col->scanned_global_initializers
                [col->scanned_global_initializer_count++] = initializer;
            bool was_scanning_initializer = col->scanning_global_initializer;
            col->scanning_global_initializer = true;
            ny_native_scan_expr_for_calls(initializer, col);
            col->scanning_global_initializer = was_scanning_initializer;
          }
        }
      }
    }
    if (col->scanning_global_initializer && e->as.ident.name) {
      nyr_builder_impl_t probe = {.prog = col->prog,
                                  .options = col->opt,
                                  .source_file = e->tok.filename,
                                  .cache_owner = col->shared_cache};
      const expr_t *initializer =
          probe.source_file ? nyr_find_top_level_value_in_source(
                                  &probe, e->as.ident.name, probe.source_file)
                            : NULL;
      if (!initializer)
        initializer = nyr_find_imported_value(&probe, e->as.ident.name);
      bool scanned = false;
      if (initializer && initializer != e) {
        for (size_t i = 0; i < col->scanned_global_initializer_count; ++i)
          if (col->scanned_global_initializers[i] == initializer) {
            scanned = true;
            break;
          }
        if (!scanned && col->scanned_global_initializer_count <
                            sizeof(col->scanned_global_initializers) /
                                sizeof(col->scanned_global_initializers[0])) {
          col->scanned_global_initializers
              [col->scanned_global_initializer_count++] = initializer;
          ny_native_scan_expr_for_calls(initializer, col);
        }
      }
    }
    break;
  case NY_E_CALL: {
    if (col->scope_fn && col->scope_fn->as.fn.name &&
        strcmp(col->scope_fn->as.fn.name, "create_window") == 0 &&
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name &&
        strcmp(e->as.call.callee->as.ident.name, "_x11_open_display") == 0 &&
        ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE")) {
      const stmt_t *resolved = e->semantic.canonical_callee_stmt;
      ny_trace_line("NATIVE_REACHABLE",
                    "native-call-scan %s -> %s canonical=%s declaration=%s",
                    col->scope_fn->as.fn.name, e->as.call.callee->as.ident.name,
                    e->semantic.canonical_callee ? e->semantic.canonical_callee
                                                 : "<none>",
                    resolved && resolved->kind == NY_S_FUNC &&
                            resolved->as.fn.name
                        ? resolved->as.fn.name
                        : "<none>");
    }
    if (getenv("NY_DEBUG_CALL_SCAN") && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.name &&
        strcmp(e->as.call.callee->as.member.name, "add") == 0)
      fprintf(stderr,
              "scan call member add "
              "canonical=%s kind=%d\n",
              e->semantic.canonical_callee ? e->semantic.canonical_callee
                                           : "<null>",
              (int)e->semantic.member_call_kind);
    /* The semantic resolver may retain a
     * canonical attached-method symbol
     * while leaving the surface call kind
     * as NONE (the parser represents
     * `value.method(args)` as an ordinary
     * call whose callee is a member).
     * Reachability must follow the
     * canonical symbol independently of
     * that presentation detail, or the
     * later lowerer can emit a valid method
     * call whose body was never collected.
     */
    const char *semantic_callee = e->semantic.canonical_callee;
    bool semantic_decl_collected = ny_native_add_semantic_callee(col, e);
    if (semantic_callee && !semantic_decl_collected) {
      ny_native_add_reachable_fn(col, semantic_callee);
      if (strncmp(semantic_callee, "core.", sizeof("core.") - 1) == 0)
        ny_native_add_reachable_fn(col, semantic_callee + sizeof("core.") - 1);
      else if (strchr(semantic_callee, '.')) {
        char qualified[512];
        int qn =
            snprintf(qualified, sizeof(qualified), "core.%s", semantic_callee);
        if (qn > 0 && (size_t)qn < sizeof(qualified))
          ny_native_add_reachable_fn(col, qualified);
      }
      /* Attached methods can be indexed
       * under their short impl name while
       * the semantic call carries the
       * module-qualified spelling.  Resolve
       * the declaration here so collection
       * and emission use the same body. */
      nyr_builder_impl_t probe = {.prog = col->prog,
                                  .options = col->opt,
                                  .module_name = NULL,
                                  .source_file = NULL,
                                  .cache_owner = col->shared_cache};
      const stmt_t *target = nyr_find_user_function(&probe, semantic_callee);
      if (target && target->kind == NY_S_FUNC && target->as.fn.name)
        ny_native_add_reachable_fn(col, target->as.fn.name);
    }
    if (e->as.call.callee) {
      if (e->as.call.callee->kind == NY_E_IDENT) {
        const char *callee_name = e->as.call.callee->as.ident.name;
        if (callee_name) {
          /*
           * Pre-pass collection: record
           * every (callee, arg-index) whose
           * argument has a dynamic-list
           * shape so per-parameter queries
           * never need to rescan the
           * program.
           */
          if (!col->funcs) {
            for (size_t ai = 0; ai < e->as.call.args.len; ++ai)
              if (nyr_expr_has_dyn_list_shape_in_scope(
                      col->prog, col, col->query_scope,
                      e->as.call.args.data[ai].val, 0))
                ny_native_dynlist_note_call(col->shared_cache, callee_name, ai);
          }
        }
        if (callee_name) {
          ny_native_add_reachable_fn(col, callee_name);
          const char *dot = strrchr(callee_name, '.');
          if (dot && dot > callee_name) {
            char candidate[256];
            snprintf(candidate, sizeof(candidate), "core.%s", dot + 1);
            ny_native_add_reachable_fn(col, candidate);
          }
        }
      } else if (e->as.call.callee->kind == NY_E_MEMBER) {
        nyr_builder_impl_t probe = {
            .prog = col->prog,
            .options = col->opt,
            .module_name = col->scope_fn
                               ? ny_native_fn_module(
                                     ny_native_fn_index_for(col->shared_cache),
                                     col->prog, col->scope_fn)
                               : NULL,
            .source_file = col->scope_fn ? col->scope_fn->tok.filename : NULL,
            .cache_owner = col->shared_cache};
        const char *owner =
            nyr_expr_type_name(&probe, e->as.call.callee->as.member.target);
        const char *mname = e->as.call.callee->as.member.name;
        if (owner && mname) {
          char qual[512];
          int n = snprintf(qual, sizeof(qual), "%s.%s", owner, mname);
          if (n > 0 && (size_t)n < sizeof(qual))
            ny_native_add_reachable_fn(col, qual);
        }
        const stmt_t *attached = ny_native_collector_attached_method(
            col, e->as.call.callee->as.member.target,
            e->as.call.callee->as.member.name);
        if (attached)
          ny_native_collector_add_fn(col, attached);
        if (!semantic_decl_collected) {
          bool qualified = ny_native_scan_qualified_call(
              col, e->as.call.callee->as.member.target,
              e->as.call.callee->as.member.name);
          if (!qualified && !semantic_callee)
            ny_native_add_reachable_fn(col, e->as.call.callee->as.member.name);
        }
      } else if (e->as.call.callee->kind == NY_E_FN ||
                 e->as.call.callee->kind == NY_E_LAMBDA) {
        stmt_t *lambda = ny_native_lambda_create(
            ny_native_lambda_cache_for(col->shared_cache), e->as.call.callee);
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
          stmt_t *lambda = ny_native_lambda_create(
              ny_native_lambda_cache_for(col->shared_cache), arg);
          if (lambda)
            ny_native_collector_add_fn(col, lambda);
        }
      }
      ny_native_scan_expr_for_calls(arg, col);
    }
    break;
  }
  case NY_E_MEMCALL:
    if (e->semantic.canonical_callee) {
      ny_native_add_reachable_fn(col, e->semantic.canonical_callee);
      if (strncmp(e->semantic.canonical_callee, "core.", sizeof("core.") - 1) ==
          0)
        ny_native_add_reachable_fn(col, e->semantic.canonical_callee +
                                            sizeof("core.") - 1);
    }
    if (e->as.memcall.name) {
      bool qualified = ny_native_scan_qualified_call(col, e->as.memcall.target,
                                                     e->as.memcall.name);
      bool runtime_method = strcmp(e->as.memcall.name, "append") == 0 ||
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
    /*
     * The call site may dispatch a receiver
     * method through the attached-method
     * lookup with an owner-qualified
     * spelling (tuple literals are
     * list-backed, so `(1, 2, 3).first()`
     * emits list.first while the semantic
     * canonical names the tuple overload).
     * Collect the same attached-method
     * target the emitter can choose so the
     * body exists under the emitted symbol.
     */
    if (e->as.memcall.target && e->as.memcall.name &&
        !ny_native_scan_qualified_call(col, e->as.memcall.target,
                                       e->as.memcall.name)) {
      const stmt_t *attached = ny_native_collector_attached_method(
          col, e->as.memcall.target, e->as.memcall.name);
      if (attached)
        ny_native_collector_add_fn(col, attached);
      /* The general `any` implementation
       * may be returned even when the
       * receiver has a concrete declared
       * type. Lowering can select the
       * concrete owner implementation from
       * its local facts, so retain that
       * same owner-qualified body as well.
       */
      ny_native_collector_add_typed_member(col, e->as.memcall.target,
                                           e->as.memcall.name);
    }
    ny_native_scan_expr_for_calls(e->as.memcall.target, col);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_native_scan_expr_for_calls(e->as.memcall.args.data[i].val, col);
    break;
  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "value") == 0 &&
        !e->semantic.canonical_callee_stmt) {
      /*
       * Narrow fan-out 2026-10-11: `value` reads that lack a resolved body
       * lower to single-argument attached calls once flow typing narrows
       * the receiver. Collect same-leaf single-parameter bodies so those
       * late-synthesized calls link. Bounded: only impl methods named
       * exactly `value` taking exactly one parameter.
       */
      for (size_t i = 0; i < col->prog->body.len; ++i)
        ny_native_collect_impl_method_bodies(col, col->prog->body.data[i],
                                             "value");
    }
    if (e->semantic.canonical_callee)
      ny_native_add_reachable_fn(col, e->semantic.canonical_callee);
    /* A method used as a property is
     * emitted as a zero-argument call by
     * the native lowerer (for example
     * `field.degree`).  Follow the exact
     * attached declaration selected for
     * this receiver; leaf-name collection
     * alone can miss module-qualified impl
     * methods and leave an unresolved LLVM
     * call. */
    if (e->semantic.canonical_callee_stmt &&
        e->semantic.canonical_callee_stmt->kind == NY_S_FUNC)
      ny_native_collector_add_fn(col, e->semantic.canonical_callee_stmt);
    if (e->as.member.name) {
      const stmt_t *attached = ny_native_collector_attached_method(
          col, e->as.member.target, e->as.member.name);
      if (attached) {
        ny_native_collector_add_fn(col, attached);
      } else {
        ny_native_collector_add_typed_member(col, e->as.member.target,
                                             e->as.member.name);
      }
    }
    if (e->as.member.name && !ny_native_scan_qualified_call(
                                 col, e->as.member.target, e->as.member.name))
      /* Type/impl member dispatch often has
       * a local receiver, so it cannot be
       * reconstructed as an alias-qualified
       * expression by this syntax pass.
       * Resolve the method leaf and let the
       * bounded scoped fan-out in
       * ny_native_add_reachable_fn retain
       * ambiguous impl bodies. */
      ny_native_add_reachable_fn(col, e->as.member.name);
    ny_native_scan_expr_for_calls(e->as.member.target, col);
    break;
  case NY_E_UNARY:
    ny_native_scan_expr_for_calls(e->as.unary.right, col);
    break;
  case NY_E_BINARY:
    if (e->as.binary.op) {
      if (strcmp(e->as.binary.op, "**") == 0)
        ny_native_add_reachable_fn(col, "core.pow");
      const stmt_t *method = ny_native_collector_operator(col, e);
      if (method)
        ny_native_collector_add_fn(col, method);
      /* Root the selected overload above.
       * Rooting every same-owner operator
       * from every loaded module pulled
       * unrelated stdlib routines into each
       * native compilation unit. Unknown or
       * dynamic operators remain on the
       * generic lowering path; uncertainty
       * is not a reachability proof. */
    }
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
    stmt_t match_stmt = {
        .kind = NY_S_MATCH, .tok = e->tok, .as = {.match = e->as.match}};
    size_t static_arm = 0;
    int static_selection = nyr_static_match_arm(&match_stmt, &static_arm);
    if (static_selection == 1) {
      ny_native_scan_stmt_for_calls(e->as.match.arms.data[static_arm].conseq,
                                    col);
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
    stmt_t *lambda = ny_native_lambda_create(
        ny_native_lambda_cache_for(col->shared_cache), e);
    if (lambda) {
      ny_native_lambda_entry_t *entry = ny_native_lambda_find(
          ny_native_lambda_cache_for(col->shared_cache), e);
      /* Function parameters are the common
       * enclosing captures.  Record them
       * during reachability so the lambda
       * body is built with its hidden env
       * parameter before function lowering
       * starts. */
      if (entry && col->scope_fn) {
        for (size_t i = 0; i < col->scope_fn->as.fn.params.len; ++i) {
          const char *name = col->scope_fn->as.fn.params.data[i].name;
          if (!name || ny_native_lambda_param_named(e, name) ||
              !nyr_stmt_uses_ident(e->as.lambda.body, name))
            continue;
          if (!ny_native_lambda_capture_add(entry, name))
            break;
          size_t ci = 0;
          while (ci < entry->capture_count &&
                 strcmp(entry->capture_names[ci], name) != 0)
            ++ci;
          if (ci >= entry->capture_count)
            continue;
          nyr_param_abi_t param_abi;
          if (!nyr_param_abi(col->shared_cache, col->scope_fn, i, &param_abi))
            continue;
          const param_t *param = &col->scope_fn->as.fn.params.data[i];
          nyr_local_t *dst = &entry->capture_types[ci];
          memset(dst, 0, sizeof(*dst));
          dst->is_list = param_abi.is_list;
          dst->is_dyn_list = param_abi.dynamic_sequence;
          dst->is_cstr = param_abi.is_str;
          dst->is_bytes = ny_native_type_name_is_bytes(param->type);
          dst->is_dict = param->type && strcmp(param->type, "dict") == 0;
          dst->is_any = param_abi.is_any;
          dst->is_f64 = param_abi.value_type == NYIR_PARAM_F64;
          dst->is_f32 = param_abi.value_type == NYIR_PARAM_F32;
          dst->is_bool = ny_type_name_is_bool(param->type);
          dst->semantic_rep =
              param->semantic.resolved ? param->semantic.rep : param_abi.rep;
          dst->semantic_ownership = param->semantic.ownership;
          dst->alias_class = param->semantic.alias_class;
          dst->semantic_mutable = param->semantic.mutable_value;
        }
        ny_native_lambda_capture_locals(col->scope_fn->as.fn.body, e, entry,
                                        col, 0);
      }
      /*
       * Top-level `def`/`mut` bindings live
       * in rt_main locals, not in the
       * global symbol table.  Discover
       * those captures during reachability,
       * before generated lambda functions
       * are lowered.  Waiting until the
       * closure value is materialized in
       * rt_main is too late: the synthetic
       * lambda would already have been
       * compiled without its hidden env
       * parameter, so the closure
       * trampoline shifts every real
       * argument by one slot (the classic
       * `range(4).map(fn(i){ i + base })`
       * failure).
       */
      if (entry && !col->scope_fn && col->prog) {
        nyr_builder_impl_t probe = {
            .prog = col->prog,
            .options = col->opt,
            .source_file = col->query_scope ? col->query_scope->tok.filename
                                            : e->tok.filename,
            .cache_owner = col->shared_cache};
        for (size_t si = 0; si < col->prog->body.len; ++si) {
          const stmt_t *decl = col->prog->body.data[si];
          if (!decl || decl->kind != NY_S_VAR)
            continue;
          for (size_t vi = 0; vi < decl->as.var.names.len; ++vi) {
            const char *name = decl->as.var.names.data[vi];
            if (!name || !*name || ny_native_lambda_param_named(e, name) ||
                /* Root `mut` bindings are
                 * module cells, not lexical
                 * values. Capturing their
                 * current payload would
                 * turn a shared global into
                 * a stale closure snapshot
                 * (and cannot represent
                 * non-local container
                 * values reliably).  Let
                 * the lambda resolve the
                 * registered global symbol
                 * at its use site. */
                /* Both the initial `mut`
                 * binding and later root
                 * assignments are emitted
                 * as module-cell
                 * operations. */
                decl->as.var.is_mut || !decl->as.var.is_decl ||
                !nyr_stmt_uses_ident(e->as.lambda.body, name))
              continue;
            if (!ny_native_lambda_capture_add(entry, name))
              break;
            size_t ci = 0;
            while (ci < entry->capture_count &&
                   strcmp(entry->capture_names[ci], name) != 0)
              ++ci;
            if (ci >= entry->capture_count)
              continue;
            const expr_t *init = vi < decl->as.var.exprs.len
                                     ? decl->as.var.exprs.data[vi]
                                     : nyr_find_top_level_value(&probe, name);
            if (!init)
              continue;
            nyr_local_t *dst = &entry->capture_types[ci];
            memset(dst, 0, sizeof(*dst));
            dst->is_list = nyr_expr_is_list(&probe, init);
            dst->is_dyn_list = nyr_expr_is_dyn_list(&probe, init);
            dst->is_cstr = nyr_expr_is_cstr(&probe, (expr_t *)init);
            dst->is_bytes = nyr_expr_is_bytes(&probe, init);
            dst->is_dict = nyr_expr_is_dict(&probe, init);
            dst->is_any = nyr_expr_is_any(&probe, (expr_t *)init);
            dst->is_f64 = nyr_expr_is_f64(&probe, init);
            dst->is_f32 = nyr_expr_is_f32(&probe, init);
            dst->is_bool = nyr_expr_is_bool(&probe, (expr_t *)init);
            if (init->semantic.resolved) {
              dst->semantic_rep = init->semantic.rep;
              dst->semantic_ownership = init->semantic.ownership;
              dst->alias_class = init->semantic.alias_class;
              dst->semantic_mutable = init->semantic.mutable_value;
            } else if ((init->kind == NY_E_LITERAL &&
                        init->as.literal.kind == NY_LIT_INT &&
                        init->tok.kind != NY_T_NIL) ||
                       nyr_expr_is_raw_int(init)) {
              dst->semantic_rep = NY_SEM_REP_RAW_INT;
            }
          }
        }
      }
      ny_native_collector_add_fn(col, lambda);
    }
    ny_native_scan_stmt_for_calls(e->as.lambda.body, col);
    break;
  }
  case NY_E_COMPTIME:
    /* Comptime blocks can contain ordinary
     * calls and lambdas whose code is still
     * emitted by the native backend.
     * Reachability must descend into the
     * block before synthetic lambda
     * functions are lowered; otherwise a
     * `comptime{ xs.map(fn(v){ ... }) }`
     * silently loses its callback. */
    ny_native_scan_stmt_for_calls(e->as.comptime_expr.body, col);
    break;
  case NY_E_FSTRING:
    /*
     * Callees referenced only inside an
     * f-string interpolation are real
     * calls: assert_type formats
     * {_type_spec_to_str(spec)} and
     * {type_name(x)}, and both bodies must
     * stay reachable for linking.
     */
    for (size_t fi = 0; fi < e->as.fstring.parts.len; ++fi)
      if (e->as.fstring.parts.data[fi].kind == NY_FSP_EXPR)
        ny_native_scan_expr_for_calls(e->as.fstring.parts.data[fi].as.e, col);
    break;
  default:
    break;
  }
}
/*
 * Memoization for dyn-list parameter
 * queries. Each query rescans the ENTIRE
 * program AST, and the query fires once per
 * list-typed parameter of every lowered
 * function: O(functions x params x program)
 * node visits. Real modules
 * (core.os.ui.render alone is ~3k lines with
 * hundreds of functions) turned that into
 * minutes of pure scanning.
 *
 * The cache is populated by ONE
 * whole-program pre-pass
 * (ny_native_dynlist_prewarm) that records
 * every callee/argument-index pair whose
 * call-site argument has a dynamic-list
 * shape; lookups are then O(1). Cleared
 * with the other session-lifetime native
 * pools.
 */
static uint64_t ny_native_dynlist_key_hash(const char *name, unsigned param) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    hash = (hash ^ *p) * UINT64_C(1099511628211);
  for (unsigned shift = 0; shift < sizeof(param) * 8; shift += 8)
    hash = (hash ^ (uint8_t)(param >> shift)) * UINT64_C(1099511628211);
  return hash;
}

static size_t ny_native_dynlist_cache_find(const nyr_builder_impl_t *owner,
                                           const char *name, size_t param) {
  if (!owner || !name || param > (size_t)(unsigned)param)
    return SIZE_MAX;
  unsigned key_param = (unsigned)param;
  if (owner->dynlist_name_slots && owner->dynlist_name_slot_cap) {
    size_t slot = (size_t)ny_native_dynlist_key_hash(name, key_param) &
                  (owner->dynlist_name_slot_cap - 1);
    while (owner->dynlist_name_slots[slot]) {
      size_t index = owner->dynlist_name_slots[slot] - 1;
      const ny_dynlist_cache_ent_t *entry = &owner->dynlist_cache[index];
      if (entry->param == key_param && strcmp(entry->name, name) == 0)
        return index;
      slot = (slot + 1) & (owner->dynlist_name_slot_cap - 1);
    }
    return SIZE_MAX;
  }
  for (size_t i = 0; i < owner->dynlist_cache_len; ++i)
    if (owner->dynlist_cache[i].param == key_param &&
        strcmp(owner->dynlist_cache[i].name, name) == 0)
      return i;
  return SIZE_MAX;
}

static bool ny_native_dynlist_name_index_reserve(nyr_builder_impl_t *owner,
                                                 size_t required) {
  size_t cap = owner->dynlist_name_slot_cap;
  if (cap && required < cap - cap / 3)
    return true;
  if (!cap)
    cap = 128;
  while (required >= cap - cap / 3) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  if (cap > SIZE_MAX / sizeof(*owner->dynlist_name_slots))
    return false;
  size_t *slots = calloc(cap, sizeof(*slots));
  if (!slots)
    return false;
  for (size_t i = 0; i < owner->dynlist_cache_len; ++i) {
    const ny_dynlist_cache_ent_t *entry = &owner->dynlist_cache[i];
    size_t slot =
        (size_t)ny_native_dynlist_key_hash(entry->name, entry->param) &
        (cap - 1);
    while (slots[slot])
      slot = (slot + 1) & (cap - 1);
    slots[slot] = i + 1;
  }
  free(owner->dynlist_name_slots);
  owner->dynlist_name_slots = slots;
  owner->dynlist_name_slot_cap = cap;
  return true;
}

static void ny_native_dynlist_name_index_insert(nyr_builder_impl_t *owner,
                                                size_t index) {
  const ny_dynlist_cache_ent_t *entry = &owner->dynlist_cache[index];
  if (!owner->dynlist_name_slots || !entry->name[0])
    return;
  size_t slot = (size_t)ny_native_dynlist_key_hash(entry->name, entry->param) &
                (owner->dynlist_name_slot_cap - 1);
  while (owner->dynlist_name_slots[slot])
    slot = (slot + 1) & (owner->dynlist_name_slot_cap - 1);
  owner->dynlist_name_slots[slot] = index + 1;
}

static bool
nyr_function_param_is_dyn_list(const nyr_builder_impl_t *cache_owner,
                               const program_t *prog, const stmt_t *fn,
                               size_t param_index) {
  (void)prog;
  if (!fn || fn->kind != NY_S_FUNC || !fn->as.fn.name)
    return false;
  const char *qname = fn->as.fn.name;
  if (strlen(qname) >= NY_DYNLIST_NAME)
    return false;
  const nyr_builder_impl_t *owner = cache_owner && cache_owner->cache_owner
                                        ? cache_owner->cache_owner
                                        : cache_owner;
  size_t index = ny_native_dynlist_cache_find(owner, qname, param_index);
  return index != SIZE_MAX && owner->dynlist_cache[index].dyn_list;
}

static void ny_native_dynlist_note_call(nyr_builder_impl_t *cache_owner,
                                        const char *callee_name,
                                        size_t arg_index) {
  nyr_builder_impl_t *owner = cache_owner && cache_owner->cache_owner
                                  ? cache_owner->cache_owner
                                  : cache_owner;
  if (!owner || !callee_name || strlen(callee_name) >= NY_DYNLIST_NAME ||
      arg_index > (size_t)(unsigned)arg_index)
    return;
  unsigned key_param = (unsigned)arg_index;
  if (ny_native_dynlist_cache_find(owner, callee_name, key_param) != SIZE_MAX ||
      owner->dynlist_cache_len >= NY_DYNLIST_CACHE_MAX)
    return;
  if (!ny_native_dynlist_name_index_reserve(owner,
                                            owner->dynlist_cache_len + 1)) {
    free(owner->dynlist_name_slots);
    owner->dynlist_name_slots = NULL;
    owner->dynlist_name_slot_cap = 0;
  }
  if (owner->dynlist_cache_len == owner->dynlist_cache_cap) {
    size_t next = owner->dynlist_cache_cap ? owner->dynlist_cache_cap * 2 : 64;
    if (next > NY_DYNLIST_CACHE_MAX)
      next = NY_DYNLIST_CACHE_MAX;
    ny_dynlist_cache_ent_t *grown =
        realloc(owner->dynlist_cache, next * sizeof(*grown));
    if (!grown)
      return;
    owner->dynlist_cache = grown;
    owner->dynlist_cache_cap = next;
  }
  size_t index = owner->dynlist_cache_len++;
  ny_dynlist_cache_ent_t *entry = &owner->dynlist_cache[index];
  snprintf(entry->name, sizeof(entry->name), "%s", callee_name);
  entry->param = key_param;
  entry->dyn_list = true;
  ny_native_dynlist_name_index_insert(owner, index);
}

/*
 * A deferred body is a value created at the
 * registration point.  Mutable module
 * bindings normally remain shared for
 * ordinary closures, but defer uses the
 * value captured at registration just like
 * the source-level defer contract. Discover
 * only mutable bindings from the same
 * module as the enclosing function; this
 * avoids turning unrelated imported globals
 * with the same leaf name into captures.
 */
static void ny_native_defer_capture_globals(const program_t *prog,
                                            const stmt_t *s, const stmt_t *body,
                                            ny_native_lambda_entry_t *entry,
                                            const char *module_name,
                                            unsigned depth) {
  if (!prog || !s || !body || !entry || depth > 64)
    return;
  switch (s->kind) {
  case NY_S_VAR:
    if (!s->as.var.is_mut)
      return;
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name || !nyr_stmt_uses_ident(body, name) ||
          nyr_stmt_assigns_ident(body, name))
        continue;
      if (!ny_native_lambda_capture_add(entry, name))
        return;
      size_t ci = 0;
      while (ci < entry->capture_count &&
             strcmp(entry->capture_names[ci], name) != 0)
        ++ci;
      if (ci >= entry->capture_count)
        continue;
      const expr_t *init =
          i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      if (!init)
        continue;
      nyr_builder_t probe = {.prog = prog,
                             .module_name = module_name,
                             .source_file = s->tok.filename};
      nyr_local_t *dst = &entry->capture_types[ci];
      memset(dst, 0, sizeof(*dst));
      dst->is_list = nyr_expr_is_list(&probe, init);
      dst->is_dyn_list = nyr_expr_is_dyn_list(&probe, init);
      dst->is_cstr = nyr_expr_is_cstr(&probe, (expr_t *)init);
      dst->is_bytes = nyr_expr_is_bytes(&probe, init);
      dst->is_dict = nyr_expr_is_dict(&probe, init);
      dst->is_any = nyr_expr_is_any(&probe, (expr_t *)init);
      dst->is_f64 = nyr_expr_is_f64(&probe, init);
      dst->is_f32 = nyr_expr_is_f32(&probe, init);
      dst->is_bool = nyr_expr_is_bool(&probe, (expr_t *)init);
      if (init->semantic.resolved) {
        dst->semantic_rep = init->semantic.rep;
        dst->semantic_ownership = init->semantic.ownership;
        dst->alias_class = init->semantic.alias_class;
        dst->semantic_mutable = init->semantic.mutable_value;
      }
    }
    return;
  case NY_S_MODULE: {
    const char *child = s->as.module.name && *s->as.module.name
                            ? s->as.module.name
                            : module_name;
    if ((!module_name && child) ||
        (module_name && (!child || strcmp(module_name, child) != 0)))
      return;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      ny_native_defer_capture_globals(prog, s->as.module.body.data[i], body,
                                      entry, child, depth + 1);
    return;
  }
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      ny_native_defer_capture_globals(prog, s->as.block.body.data[i], body,
                                      entry, module_name, depth + 1);
    return;
  case NY_S_IF:
    ny_native_defer_capture_globals(prog, s->as.iff.conseq, body, entry,
                                    module_name, depth + 1);
    ny_native_defer_capture_globals(prog, s->as.iff.alt, body, entry,
                                    module_name, depth + 1);
    return;
  default:
    return;
  }
}

static void ny_native_scan_stmt_for_calls(const stmt_t *s,
                                          ny_native_fn_collector_t *col) {
  if (!s || !col)
    return;
  /* The general reachability walk must
   * enter stdlib bodies to retain their
   * dependencies. The one-shot dynamic-list
   * prewarm is different: its cache answers
   * questions about source call sites, so
   * stdlib implementation bodies are
   * irrelevant and can be very large. Use
   * registered token features rather than
   * filename or qualified-name guesses. */
  if (col->is_dynlist_prewarm && ny_native_stmt_is_stdlib(s) &&
      !(col->opt && col->opt->emit_module && *col->opt->emit_module))
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
        nyr_target_eval_bool(col->opt, s->as.iff.test, &selected)) {
      ny_native_scan_stmt_for_calls(selected ? s->as.iff.conseq : s->as.iff.alt,
                                    col);
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
    if (s->as.de.body) {
      ny_native_lambda_entry_t *entry = ny_native_defer_lambda_create(
          ny_native_lambda_cache_for(col->shared_cache), s->as.de.body);
      if (entry && col->scope_fn) {
        for (size_t i = 0; i < col->scope_fn->as.fn.params.len; ++i) {
          const char *name = col->scope_fn->as.fn.params.data[i].name;
          if (name && nyr_stmt_uses_ident(s->as.de.body, name) &&
              !ny_native_lambda_capture_add(entry, name))
            break;
        }
      }
      if (entry && col->prog) {
        const char *module =
            col->scope_fn
                ? ny_native_fn_module(ny_native_fn_index_for(col->shared_cache),
                                      col->prog, col->scope_fn)
                : NULL;
        for (size_t i = 0; i < col->prog->body.len; ++i)
          ny_native_defer_capture_globals(col->prog, col->prog->body.data[i],
                                          s->as.de.body, entry, module, 0);
      }
      if (entry)
        ny_native_collector_add_fn(col, entry->fn);
    }
    ny_native_scan_stmt_for_calls(s->as.de.body, col);
    break;
  case NY_S_MATCH: {
    size_t static_arm = 0;
    int static_selection = nyr_static_match_arm(s, &static_arm);
    if (static_selection == 1) {
      ny_native_scan_stmt_for_calls(s->as.match.arms.data[static_arm].conseq,
                                    col);
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
    /* Nested declarations are executable
     * code when their enclosing function
     * calls them.  The body scanner reaches
     * this node while walking the parent,
     * so register it explicitly instead of
     * only scanning its defaults/body. */
    if (!col->is_dynlist_prewarm && s->as.fn.name && !ny_is_stdlib_tok(s->tok))
      ny_native_add_reachable_fn(col, s->as.fn.name);
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      ny_native_scan_expr_for_calls(s->as.fn.params.data[i].def, col);
    ny_native_scan_stmt_for_calls(s->as.fn.body, col);
    break;
  default:
    break;
  }
}

static size_t
ny_native_collect_reachable_fns(const program_t *prog, const ny_options *opt,
                                const stmt_t **out_funcs, size_t max_funcs,
                                nyr_builder_impl_t *shared_owner) {
  if (!prog || !out_funcs || max_funcs == 0 || !shared_owner)
    return 0;
  /* The caller owns this context through
   * function and rt_main lowering. */
  shared_owner->prog = prog;
  shared_owner->options = opt;
  shared_owner->cache_owner = shared_owner;
  ny_native_fn_collector_t col = {
      .prog = prog,
      .opt = opt,
      .funcs = out_funcs,
      .max_funcs = max_funcs,
      .shared_cache = shared_owner,
  };
  ny_native_seed_emitted_module(prog, opt, &col);
  /* Library precompiles root exports above, then follow their function and
   * initializer dependencies without repeated attached/operator/type walks.
   * Those whole-program queries dominated std.bc time and RSS. */
  if (opt && opt->emit_module && *opt->emit_module)
    col.is_precompile = true;
  /* The expanded program contains test/main
   * blocks from imported packages. Seed the
   * call graph only from the root source
   * block; imported-module bodies become
   * reachable when an actually called
   * function is scanned. */
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
   * Seed only from executable top-level
   * statements below.  Pre-seeding every
   * user function consumes the bounded
   * native function/object tables with dead
   * helpers before the actual entry-point
   * call graph is traversed.
   */

  /*
   * 2. Scan top-level user statements in
   * prog->body for calls.  Skipping
   * stdlib-token statements is what keeps
   * the whole standard library out of the
   * native build: prog->body contains every
   * stdlib module body, and scanning them
   * here would force-build the full stdlib
   * (borrow of unary, store.local
   * dominance, ... fail in the shared-NYIR
   * lowerer).  Stdlib helpers are pulled in
   * only when user code actually calls
   * them.
   */
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_BLOCK && root_source && s->tok.filename &&
        !nyr_same_source_file(s->tok.filename, root_source))
      continue;
    /* Impl methods are executable functions
     * too, but their declarations are
     * nested under NY_S_IMPL rather than
     * appearing as top-level NY_S_FUNC
     * nodes.  Seed source impl methods so
     * operator/attached dispatch cannot
     * leave a referenced method as an
     * unresolved JIT declaration. */
    if ((s->kind == NY_S_IMPL || s->kind == NY_S_LAYOUT ||
         s->kind == NY_S_STRUCT) &&
        !ny_native_stmt_is_stdlib(s)) {
      const ny_stmt_list *methods = s->kind == NY_S_IMPL ? &s->as.impl.methods
                                    : s->kind == NY_S_LAYOUT
                                        ? &s->as.layout.methods
                                        : &s->as.struc.methods;
      for (size_t j = 0; j < methods->len; ++j) {
        const stmt_t *m = methods->data[j];
        if (m && m->kind == NY_S_FUNC && m->as.fn.name)
          ny_native_add_reachable_fn(&col, m->as.fn.name);
      }
    }
    /* Imported user modules contribute
     * declarations, not executable
     * top-level statements to the root.
     * Scanning every function body in each
     * module here over-approximates the
     * call graph (and can pull hundreds of
     * dead helpers into native lowering);
     * reachable function bodies are scanned
     * by the worklist below instead. */
    if (s->kind == NY_S_MODULE && !ny_native_stmt_is_stdlib(s) &&
        nyr_same_source_file(s->tok.filename, root_source)) {
      for (size_t j = 0; j < s->as.module.body.len; ++j)
        if (nyr_is_main_guard(s->as.module.body.data[j]))
          ny_native_scan_stmt_for_calls(s->as.module.body.data[j], &col);
    }
    if (s->kind != NY_S_FUNC && s->kind != NY_S_MODULE &&
        !ny_native_stmt_is_stdlib(s)) {
      ny_native_scan_stmt_for_calls(s, &col);
    }
  }

  /*
   * 1b. Seed the entry point function so
   * that programs consisting entirely of
   * function definitions (no top-level
   * statements) still have their call graph
   * traversed.  Without this, `fn main() {
   * ... }` programs produce zero reachable
   * functions, leaving the inline callee
   * table empty and forcing every call
   * through the slow ABI path.
   */
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
        !ny_is_stdlib_tok(s->tok)) {
      ny_native_add_reachable_fn(&col, s->as.fn.name);
    }
  }

  /*
   * 3. Transitive worklist loop over
   * collected functions
   */
  size_t idx = 0;
  while (idx < col.count) {
    const stmt_t *fn = col.funcs[idx++];
    col.scope_fn = fn;
    ny_native_scan_stmt_for_calls(fn->as.fn.body, &col);
  }

  size_t reachable_count = col.count;
  free(col.seen_fn_slots);
  free(col.seen_name_slots);
  return col.overflow ? SIZE_MAX : reachable_count;
}

/*
 * Interprocedural effect inference for
 * user-function calls.
 *
 * The runtime-symbol table
 * (nyr_call_effect_summary) classifies
 * known rt_* helpers but reports
 * CALL|FFI|UNKNOWN_SIDE_EFFECT for every
 * user function, which makes LICM (and DCE)
 * treat all user calls as opaque. After all
 * reachable function bodies are built and
 * optimized, we compute the true observable
 * effect set of each body -- including what
 * its own callees do, iterated to a fixed
 * point for recursion -- and tag the CALL
 * instructions with that summary
 * (NYIR_INST_F_EFFECTS_KNOWN).  A function
 * whose body (and transitively its callees)
 * only reads its own frame and computes
 * arithmetic is then hoistable out of
 * loops, which is exactly the gap that made
 * recursive pure helpers like gcbench's
 * tree_count stay inside hot loops while C
 * hoists them.
 *
 * Only caller-observable effects are
 * tracked: a function's own
 * READ_LOCAL/WRITE_LOCAL (private frame)
 * and CONTROL (internal branches) are
 * invisible to the caller and stripped from
 * the summary.
 */

typedef struct {
  const char *name;
  size_t function_index;
} ny_native_call_effect_index_entry_t;

typedef struct {
  ny_native_call_effect_index_entry_t *entries;
  size_t len;
} ny_native_call_effect_index_t;

static int ny_native_call_effect_index_compare(const void *left,
                                               const void *right) {
  const ny_native_call_effect_index_entry_t *a = left;
  const ny_native_call_effect_index_entry_t *b = right;
  int names = strcmp(a->name, b->name);
  if (names)
    return names;
  return a->function_index < b->function_index   ? -1
         : a->function_index > b->function_index ? 1
                                                 : 0;
}

static bool
ny_native_call_effect_index_build(ny_native_call_effect_index_t *index,
                                  const char **names, size_t count) {
  if (!index || !names || count == 0)
    return false;
  index->entries = malloc(count * sizeof(*index->entries));
  if (!index->entries)
    return false;
  for (size_t i = 0; i < count; ++i) {
    if (!names[i])
      continue;
    index->entries[index->len++] = (ny_native_call_effect_index_entry_t){
        .name = names[i], .function_index = i};
  }
  qsort(index->entries, index->len, sizeof(*index->entries),
        ny_native_call_effect_index_compare);
  return true;
}

static bool
ny_native_call_effect_index_find(const ny_native_call_effect_index_t *index,
                                 const char *name, size_t *function_index) {
  if (!index || !index->entries || !name)
    return false;
  size_t lo = 0, hi = index->len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int cmp = strcmp(index->entries[mid].name, name);
    if (cmp < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == index->len || strcmp(index->entries[lo].name, name) != 0)
    return false;
  if (function_index)
    *function_index = index->entries[lo].function_index;
  return true;
}

static unsigned
nyr_observable_effects(const nyr_inst_t *in, const char **names, size_t count,
                       const ny_native_call_effect_index_t *index,
                       const unsigned *summaries) {
  if (!in)
    return 0;
  unsigned e;
  if (in->op == NYIR_CALL) {
    size_t callee_index;
    if (in->symbol &&
        ny_native_call_effect_index_find(index, in->symbol, &callee_index))
      return NYIR_EFFECT_CALL | summaries[callee_index];
    /* Keep the conservative compatibility
     * path if index allocation failed. */
    if (in->symbol && names && count) {
      for (size_t i = 0; i < count; ++i) {
        if (names[i] && strcmp(names[i], in->symbol) == 0)
          return summaries[i]; /* already includes
                                  NYIR_EFFECT_CALL */
      }
    }
    e = nyr_call_effect_summary(in);
  } else {
    e = in->effects | nyr_inst_effects(in);
  }
  e &=
      ~(NYIR_EFFECT_READ_LOCAL | NYIR_EFFECT_WRITE_LOCAL | NYIR_EFFECT_CONTROL);
  return e;
}

static void nyr_compute_call_effects_fixed_point(
    nyr_func_t *funcs, const char **names, size_t count,
    const ny_native_call_effect_index_t *index, unsigned *summaries) {
  for (size_t i = 0; i < count; ++i)
    summaries[i] = 0;
  /*
   * Gauss-Seidel fixed point over the call
   * graph.  Summaries only grow (OR), so
   * starting from zero and iterating to a
   * fixpoint is sound and terminates;
   * recursion keeps a self-call's
   * contribution fixed after the first pass
   * because its summary is already folded
   * in.
   */
  size_t max_iters = count + 2;
  for (size_t iter = 0; iter < max_iters; ++iter) {
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
      unsigned s = 0;
      for (size_t j = 0; j < funcs[i].len; ++j)
        s |= nyr_observable_effects(&funcs[i].data[j], names, count, index,
                                    summaries);
      if (s != summaries[i]) {
        summaries[i] = s;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
}

static void nyr_compute_call_effects(nyr_func_t *funcs, const char **names,
                                     size_t count,
                                     const ny_native_call_effect_index_t *index,
                                     unsigned *summaries) {
  if (!funcs || !names || !summaries || count == 0)
    return;
  /* Without the name index we cannot build
   * reverse edges, so preserve the complete
   * compatibility fixed point instead of
   * under-propagating effects. */
  if (!index || !index->entries) {
    nyr_compute_call_effects_fixed_point(funcs, names, count, index, summaries);
    return;
  }
  if (count == SIZE_MAX || count + 1 > SIZE_MAX / sizeof(size_t)) {
    nyr_compute_call_effects_fixed_point(funcs, names, count, index, summaries);
    return;
  }

  /*
   * Infer direct effects once and build the
   * reverse call graph. The former global
   * fixed-point loop rescanned every
   * instruction in every function on every
   * iteration, even when only one leaf
   * summary had changed. A worklist only
   * revisits callers whose callee's summary
   * grows.
   */
  size_t *caller_offsets = calloc(count + 1, sizeof(*caller_offsets));
  if (!caller_offsets) {
    nyr_compute_call_effects_fixed_point(funcs, names, count, index, summaries);
    return;
  }
  size_t edge_count = 0;
  bool graph_ok = true;
  for (size_t i = 0; i < count; ++i)
    summaries[i] = 0;
  for (size_t i = 0; i < count && graph_ok; ++i) {
    unsigned direct = 0;
    for (size_t j = 0; j < funcs[i].len; ++j) {
      const nyr_inst_t *in = &funcs[i].data[j];
      size_t callee;
      if (in->op == NYIR_CALL && in->symbol &&
          ny_native_call_effect_index_find(index, in->symbol, &callee)) {
        if (caller_offsets[callee + 1] == SIZE_MAX || edge_count == SIZE_MAX) {
          graph_ok = false;
          break;
        }
        caller_offsets[callee + 1]++;
        edge_count++;
      } else {
        direct |= nyr_observable_effects(in, names, count, index, summaries);
      }
    }
    summaries[i] = direct;
  }
  if (!graph_ok || edge_count > SIZE_MAX / sizeof(size_t)) {
    free(caller_offsets);
    nyr_compute_call_effects_fixed_point(funcs, names, count, index, summaries);
    return;
  }
  for (size_t i = 1; i <= count; ++i) {
    if (caller_offsets[i] > SIZE_MAX - caller_offsets[i - 1]) {
      graph_ok = false;
      break;
    }
    caller_offsets[i] += caller_offsets[i - 1];
  }
  size_t *callers =
      graph_ok && edge_count ? malloc(edge_count * sizeof(*callers)) : NULL;
  size_t *cursor = graph_ok ? malloc(count * sizeof(*cursor)) : NULL;
  size_t *queue = graph_ok ? malloc(count * sizeof(*queue)) : NULL;
  bool *queued = graph_ok ? calloc(count, sizeof(*queued)) : NULL;
  if (!graph_ok || (edge_count && !callers) || !cursor || !queue || !queued) {
    free(caller_offsets);
    free(callers);
    free(cursor);
    free(queue);
    free(queued);
    nyr_compute_call_effects_fixed_point(funcs, names, count, index, summaries);
    return;
  }
  memcpy(cursor, caller_offsets, count * sizeof(*cursor));
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = 0; j < funcs[i].len; ++j) {
      const nyr_inst_t *in = &funcs[i].data[j];
      size_t callee;
      if (in->op == NYIR_CALL && in->symbol &&
          ny_native_call_effect_index_find(index, in->symbol, &callee))
        callers[cursor[callee]++] = i;
    }
  }

  size_t head = 0, tail = 0, pending = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!summaries[i])
      continue;
    queue[tail] = i;
    tail = (tail + 1) % count;
    queued[i] = true;
    pending++;
  }
  while (pending) {
    size_t callee = queue[head];
    head = (head + 1) % count;
    pending--;
    queued[callee] = false;
    unsigned effects = summaries[callee];
    for (size_t e = caller_offsets[callee]; e < caller_offsets[callee + 1];
         ++e) {
      size_t caller = callers[e];
      unsigned joined = summaries[caller] | effects;
      if (joined == summaries[caller])
        continue;
      summaries[caller] = joined;
      if (!queued[caller]) {
        queue[tail] = caller;
        tail = (tail + 1) % count;
        queued[caller] = true;
        pending++;
      }
    }
  }
  free(caller_offsets);
  free(callers);
  free(cursor);
  free(queue);
  free(queued);
}

/*
 * Attach the inferred summary to calls of
 * user functions whose effects are provably
 * pure (no writes, allocation, IO, threads,
 * FFI, or unknown behavior).  Such calls
 * become LICM candidates and result-unused
 * DCE candidates, matching how C compilers
 * treat pure helpers.
 */
static bool nyr_patch_call_effects(nyr_func_t *f, const char **names,
                                   size_t count,
                                   const ny_native_call_effect_index_t *index,
                                   const unsigned *summaries) {
  if (!f || !names || !summaries || count == 0)
    return false;
  bool has_tagged_call = false;
  for (size_t j = 0; j < f->len; ++j) {
    nyr_inst_t *in = &f->data[j];
    if (in->op != NYIR_CALL || !in->symbol)
      continue;
    size_t k;
    bool found = ny_native_call_effect_index_find(index, in->symbol, &k);
    if (!found) {
      for (k = 0; k < count; ++k)
        if (names[k] && strcmp(names[k], in->symbol) == 0) {
          found = true;
          break;
        }
    }
    if (found) {
      unsigned s = summaries[k];
      const unsigned cse_safe = NYIR_EFFECT_CALL | NYIR_EFFECT_READ_LOCAL |
                                NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP;
      /* Compact classification report:
       * per-site lines defer to -vvvvvv;
       * below that, unique callees and site
       * counts summarize once. */
      bool tagged = (s & ~cse_safe) == 0;
      if (tagged) {
        in->flags |= NYIR_INST_F_EFFECTS_KNOWN;
        in->effects = NYIR_EFFECT_CALL | s;
        has_tagged_call = true;
      }
    }
  }
  return has_tagged_call;
}
static bool nyr_has_phi(const nyr_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_PHI)
      return true;
  return false;
}

bool nyr_build_ir(const program_t *prog, const ny_options *opt,
                  nyr_func_t *rt_main_out, nyr_func_t *funcs_out,
                  size_t *func_count, const char **func_names_out,
                  size_t max_funcs, char *err, size_t err_len) {
  if (!prog || !rt_main_out)
    return false;
  bool trace_stage_timing = ny_env_enabled("NYTRIX_TRACE_NYIR_EMIT_TIMING");
  ny_tick_t native_stage_start = trace_stage_timing ? ny_ticks_now() : 0;
#define NY_NATIVE_STAGE(label)                                                 \
  do {                                                                         \
    if (ny_trace_enabled("NY_NATIVE_TRACE"))                                   \
      ny_trace_line("STAGE", "%s", label);                                     \
    if (ny_trace_enabled("NY_TRACE_PHASE_MEMORY")) {                           \
      unsigned long long rss_kib = nyr_build_rss_kib();                        \
      fprintf(stderr,                                                          \
              "[resource] phase=native-nir.%s "                                \
              "rss=%lluMiB\n",                                                 \
              label, rss_kib / 1024ULL);                                       \
      fflush(stderr);                                                          \
    }                                                                          \
  } while (0)
#define NY_NATIVE_STAGE_TIMED(label)                                           \
  do {                                                                         \
    NY_NATIVE_STAGE(label);                                                    \
    if (trace_stage_timing) {                                                  \
      ny_tick_t stage_end = ny_ticks_now();                                    \
      fprintf(stderr, "[nyir-build-timing] stage=%s elapsed=%.3f s\n", label,  \
              ny_ticks_elapsed_sec(native_stage_start));                       \
      native_stage_start = stage_end;                                          \
    }                                                                          \
  } while (0)
  NY_NATIVE_STAGE_TIMED("begin");
  /* Create the shared owner before building
   * either function/module index; every
   * later lowering helper observes this
   * immutable compilation snapshot.
   */
  nyr_builder_impl_t compilation_cache = {0};
  compilation_cache.prog = prog;
  compilation_cache.options = opt;
  compilation_cache.source_main_file = opt ? opt->input_file : NULL;
  if (!compilation_cache.source_main_file ||
      !*compilation_cache.source_main_file) {
    for (size_t i = 0; i < prog->body.len; ++i) {
      const stmt_t *stmt = prog->body.data[i];
      if (stmt && stmt->kind != NY_S_MODULE && stmt->tok.filename &&
          *stmt->tok.filename) {
        compilation_cache.source_main_file = stmt->tok.filename;
        break;
      }
    }
  }
  compilation_cache.cache_owner = &compilation_cache;
  compilation_cache.resolve_alias_stats_enabled =
      ny_trace_enabled("NYTRIX_TRACE_IMPORTS");
  compilation_cache.layout_stats_enabled =
      ny_trace_enabled("NYTRIX_TRACE_LAYOUTS");
  ny_native_fn_index_t *fn_index = ny_native_fn_index_for(&compilation_cache);
  if (!fn_index) {
    nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
    return false;
  }
  ny_native_fn_cache_build(fn_index, prog);
  if (fn_index->build_failed) {
    nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
    nyr_builder_dispose(&compilation_cache);
    return false;
  }
  NY_NATIVE_STAGE_TIMED("function-cache");
  nyr_strtab_clear();
  nyr_consttab_clear();
  nyr_globaltab_clear();
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_native_register_const_defs(&compilation_cache, prog, prog->body.data[i],
                                  NULL, opt);
  NY_NATIVE_STAGE_TIMED("const-table");
  ny_native_profile_clear();
  int opt_level = opt ? opt->opt_level : 1;
  /*
   * Native tiers select real NYIR
   * optimization pipelines.  Previously the
   * tier name only changed the report label
   * and budgets, so cache entries and
   * emitted code were identical across
   * explicit tiers.
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
  nyr_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyr_set_pass_controls(opt ? opt->nyr_disable_pass : NULL,
                        opt ? opt->nyr_stop_after : NULL);
  nyr_set_verify_each_pass(opt && opt->nyr_ir_verify);
  nyr_set_rollback_on_verify_fail(opt && opt->nyr_rollback_on_verify_fail);
  nyr_set_global_inst_cap(opt ? opt->nyr_global_inst_cap : 0);
  nyr_set_pass_growth_factor(opt ? opt->nyr_pass_growth_factor : 0.0);
  nyr_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_dbg_init_from_verbosity(opt ? opt->verbosity : 0);
  nyr_set_track_value(opt ? opt->nyr_track_value : -1);
  char profile_err[256] = {0};
  if (opt && opt->native_profile_use_path &&
      !ny_native_profile_load_path(opt->native_profile_use_path, profile_err,
                                   sizeof(profile_err))) {
    nyr_set_error(err, err_len, "native PGO: %s", profile_err);
    return false;
  }
  memset(rt_main_out, 0, sizeof(*rt_main_out));
  if (func_count)
    *func_count = 0;
  if (func_names_out)
    memset((void *)func_names_out, 0, max_funcs * sizeof(*func_names_out));

  /*
   * Build extern table from #include and
   * extern statements.
   */
  ny_extern_table_t externs;
  bool aapcs = opt && (opt->native_backend == NY_NATIVE_BACKEND_AARCH64 ||
                       opt->native_abi == NY_NATIVE_ABI_AAPCS);
  char extern_err[256] = {0};
  if (!nyr_build_extern_table(prog, &externs, aapcs, opt, extern_err,
                              sizeof(extern_err))) {
    nyr_set_error(err, err_len, "NYIR extern: %s", extern_err);
    ny_extern_table_free(&externs);
    ny_native_profile_clear();
    nyr_builder_dispose(&compilation_cache);
    return false;
  }
  NY_NATIVE_STAGE_TIMED("extern-table");

  /* All function and rt_main builders share
   * one owner for immutable lowering query
   * results. The owner is scoped to this
   * whole-program construction. */
  /*
   * Collect and build reachable non-extern
   * Nytrix functions found anywhere in the
   * program.  This lets rt_main call stdlib
   * helpers like key_down that live inside
   * module bodies without exceeding
   * max_funcs capacity.
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
      nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
      ny_extern_table_free(&externs);
      ny_native_profile_clear();
      nyr_builder_dispose(&compilation_cache);
      return false;
    }
    all_fn_count = ny_native_collect_reachable_fns(
        prog, opt, all_fns, max_funcs, &compilation_cache);
    if (all_fn_count == SIZE_MAX) {
      free(all_fns);
      free(func_names);
      nyr_set_error(err, err_len,
                    "native reachable function limit exceeded (%zu)",
                    max_funcs);
      ny_extern_table_free(&externs);
      ny_native_profile_clear();
      nyr_builder_dispose(&compilation_cache);
      return false;
    }
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE"))
      fprintf(stderr, "native reachable collection complete: %zu/%zu\n",
              all_fn_count, max_funcs);
    NY_NATIVE_STAGE_TIMED("reachable-functions");
    ny_type_pipeline_persist_native_reachable_facts(
        (program_t *)prog, all_fns, all_fn_count,
        all_fn_count && all_fns[0] ? all_fns[0]->tok.filename : NULL);
    /* Prewarm only functions that will be lowered. Scanning every function
     * in a bundled stdlib repeats the same AST walk for dead private helpers
     * and self-tests; module artifacts already have an export-rooted,
     * transitive function set from the collector above. */
    ny_native_fn_collector_t warm = {.prog = prog,
                                     .opt = opt,
                                     .shared_cache = &compilation_cache,
                                     .is_dynlist_prewarm = true,
                                     .dynlist_global_inits_cap = 32768};
    warm.dynlist_global_inits = calloc(warm.dynlist_global_inits_cap,
                                       sizeof(*warm.dynlist_global_inits));
    NY_NATIVE_STAGE_TIMED("dynamic-list-prewarm.begin");
    for (size_t i = 0; i < all_fn_count; ++i) {
      const stmt_t *fn = all_fns[i];
      if (!fn || fn->kind != NY_S_FUNC)
        continue;
      warm.query_scope = fn;
      for (size_t p = 0; p < fn->as.fn.params.len; ++p)
        ny_native_scan_expr_for_calls(fn->as.fn.params.data[p].def, &warm);
      ny_native_scan_stmt_for_calls(fn->as.fn.body, &warm);
    }
    for (size_t i = 0; i < prog->body.len; ++i) {
      const stmt_t *stmt = prog->body.data[i];
      if (!stmt || stmt->kind == NY_S_FUNC || stmt->kind == NY_S_MODULE ||
          ny_native_stmt_is_stdlib(stmt))
        continue;
      warm.query_scope = stmt;
      ny_native_scan_stmt_for_calls(stmt, &warm);
    }
    free(warm.dynlist_global_inits);
    NY_NATIVE_STAGE_TIMED("dynamic-list-prewarm");
    if (ny_trace_enabled("NY_TRACE_NATIVE_REACHABLE")) {
      if (ny_dbg_json_enabled() || ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("NATIVE_REACHABLE", "functions=%zu capacity=%zu",
                               all_fn_count, max_funcs);
      else {
        fprintf(stderr,
                "native reachable functions: "
                "%zu (capacity %zu)\n",
                all_fn_count, max_funcs);
        /* One wrapped flow of the reachable
         * names (wide, not tall). */
        if (ny_reach_flow[0]) {
          const char *p2 = ny_reach_flow;
          int flow_col = 2;
          fputs("  ", stderr);
          while (*p2) {
            if (flow_col >= 96 && *p2 == ' ') {
              fputc('\n', stderr);
              fputs("  ", stderr);
              flow_col = 2;
              p2++;
              continue;
            }
            fputc(*p2, stderr);
            flow_col++;
            p2++;
          }
          fputc('\n', stderr);
          ny_reach_flow[0] = '\0';
        }
      }
    }
    size_t count = 0;
    ny_native_target_info_t abi_target = {0};
    nyr_target_request_t abi_request = {0};
    bool have_abi_target = ny_options_target_request(opt, &abi_request) &&
                           nyr_target_info_init(&abi_target, &abi_request);
    for (size_t i = 0; i < all_fn_count; ++i) {
      const stmt_t *s = all_fns[i];
      if (!s)
        continue;
      if (ny_dbg_is_enabled(NY_DBG_ABI) && s->kind == NY_S_FUNC) {
        const char *abi_name = s->as.fn.name ? s->as.fn.name : "<anonymous>";
        size_t gp_index = 0, fp_index = 0, ordinal = 0, stack_index = 0;
        for (size_t p = 0; p < s->as.fn.params.len; ++p) {
          const param_t *param = &s->as.fn.params.data[p];
          ny_sem_rep_t rep = param->semantic.resolved
                                 ? param->semantic.rep
                                 : ny_semantic_rep_for_type(param->type);
          char arg_name[192], detail[192];
          snprintf(arg_name, sizeof(arg_name), "%s arg#%zu %s", abi_name, p + 1,
                   param->name ? param->name : "?");
          nyr_scalar_class_t sc = ny_native_scalar_class_from_sem_rep(rep);
          bool is_fp = sc == NYIR_SCALAR_F32 || sc == NYIR_SCALAR_F64;
          const char *reg = NULL;
          char location[96] = {0}, abi_class[64];
          if (have_abi_target) {
            if (abi_target.abi == NY_NATIVE_ABI_WIN64) {
              size_t pos = ordinal++;
              if (pos < abi_target.gp_arg_reg_count &&
                  pos < abi_target.fp_arg_reg_count)
                reg = is_fp ? abi_target.fp_arg_regs[pos]
                            : abi_target.gp_arg_regs[pos];
              else if (pos >= 4)
                snprintf(location, sizeof(location), "STACK+%zu",
                         abi_target.shadow_space_bytes + (pos - 4) * 8);
            } else if (is_fp) {
              if (fp_index < abi_target.fp_arg_reg_count)
                reg = abi_target.fp_arg_regs[fp_index++];
              else
                snprintf(location, sizeof(location), "STACK+%zu",
                         stack_index++ * 8);
            } else {
              if (gp_index < abi_target.gp_arg_reg_count)
                reg = abi_target.gp_arg_regs[gp_index++];
              else
                snprintf(location, sizeof(location), "STACK+%zu",
                         stack_index++ * 8);
            }
          }
          if (reg)
            snprintf(location, sizeof(location), "%s", reg);
          else if (!location[0])
            snprintf(location, sizeof(location), "%s",
                     opt && opt->native_backend == NY_NATIVE_BACKEND_LLVM
                         ? "LLVM-managed"
                         : "target unavailable");
          snprintf(abi_class, sizeof(abi_class), "%s %s",
                   reg ? (is_fp ? "SSE" : "GPR")
                       : (have_abi_target ? "STACK" : "ABI"),
                   location);
          snprintf(detail, sizeof(detail),
                   "logical type=%s; value "
                   "class=%s; location=%s",
                   param->type ? param->type : "inferred",
                   nyr_scalar_class_name(sc), location);
          ny_dbg_abi_classify(arg_name, abi_class, detail);
        }
        char ret_name[192], ret_detail[192];
        snprintf(ret_name, sizeof(ret_name), "%s return", abi_name);
        snprintf(ret_detail, sizeof(ret_detail), "logical result type=%s",
                 s->as.fn.return_type ? s->as.fn.return_type : "inferred");
        nyr_scalar_class_t ret_class = ny_native_scalar_class_from_sem_rep(
            ny_semantic_rep_for_type(s->as.fn.return_type));
        bool ret_fp =
            ret_class == NYIR_SCALAR_F32 || ret_class == NYIR_SCALAR_F64;
        const char *ret_reg = have_abi_target
                                  ? (ret_fp ? abi_target.fp_arg_regs[0]
                                            : abi_target.gp_arg_regs[0])
                                  : NULL;
        char ret_abi[64], ret_location[64];
        snprintf(ret_location, sizeof(ret_location), "%s",
                 ret_reg ? ret_reg
                 : opt && opt->native_backend == NY_NATIVE_BACKEND_LLVM
                     ? "LLVM-managed"
                     : "target unavailable");
        snprintf(ret_abi, sizeof(ret_abi), "%s %s", ret_fp ? "SSE" : "GPR",
                 ret_location);
        size_t detail_len = strlen(ret_detail);
        snprintf(ret_detail + detail_len, sizeof(ret_detail) - detail_len,
                 "; location=%s", ret_location);
        ny_dbg_abi_classify(ret_name, ret_abi, ret_detail);
      }
      char local_err[256] = {0};
      nyr_trace_function_boundary(i, all_fn_count, s, "begin");
      if (!nyr_build_function(prog, s, &externs, &funcs_out[count], local_err,
                              sizeof(local_err), opt_level, opt,
                              &compilation_cache)) {
        nyr_trace_function_boundary(i, all_fn_count, s, "failed");
        /*
         * Lowering failure: free
         * already-built functions.
         */
        for (size_t j = 0; j < count; ++j)
          nyr_ir_func_free(&funcs_out[j]);
        *func_count = 0;
        if (err && err_len > 0) {
          if (local_err[0])
            nyr_set_error(err, err_len, "%s", local_err);
          else
            nyr_set_error(
                err, err_len,
                "native NYIR lower: function '%s' failed without a diagnostic",
                s->as.fn.name ? s->as.fn.name : "<anonymous>");
        }
        if (!opt || (!opt->native_dump_ir_path && !opt->nyr_dump_bin_path)) {
          free(all_fns);
          free(func_names);
          ny_native_profile_clear();
          nyr_builder_dispose(&compilation_cache);
          return false;
        }
      } else {
        nyr_trace_function_boundary(i, all_fn_count, s, "complete");
        const char *emitted_name = ny_native_emitted_fn_name(
            ny_native_fn_index_for(&compilation_cache), prog, opt, s);
        if (getenv("NYTRIX_DEBUG_POOLNAME") && emitted_name &&
            (strstr(emitted_name, "zmod.value") ||
             strstr(emitted_name, "strip") ||
             strstr(emitted_name, "zmod.same"))) {
          const char *rawname = (s && s->kind == NY_S_FUNC && s->as.fn.name)
                                    ? s->as.fn.name
                                    : "(nonfunc)";
          fprintf(stderr, "poolname-dbg: emitted=[%s] rawfn=[%s]\n",
                  emitted_name, rawname);
        }
        /* Scope the debug channels to this
         * function: stdlib detail defers to
         * -vvvvvv while the IR is being
         * built, not just at emission. */
        ny_dbg_lower_fn(emitted_name);
        ny_dbg_scope_stdlib(ny_is_stdlib_tok(s->tok));
        func_names[count] =
            nyr_builder_own_string(&compilation_cache, emitted_name);
        if (!func_names[count]) {
          for (size_t j = 0; j <= count; ++j)
            nyr_ir_func_free(&funcs_out[j]);
          *func_count = 0;
          free(all_fns);
          free(func_names);
          ny_native_profile_clear();
          ny_extern_table_free(&externs);
          nyr_builder_dispose(&compilation_cache);
          nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
          return false;
        }
        const char *ny_dump_fn = getenv("NY_DUMP_FN");
        if (ny_dump_fn && func_names[count] &&
            strstr(func_names[count], ny_dump_fn))
          nyr_dump(stderr, &funcs_out[count], func_names[count]);
        if (getenv("NY_LIST_FNS"))
          fprintf(stderr, "[built] %s\n", func_names[count]);
        count++;
      }
    }
    NY_NATIVE_STAGE_TIMED("function-lowering");
    *func_count = count;
  }

  /*
   * Offer user functions as inlining
   * candidates for rt_main. Library
   * precompiles (--emit-module) keep every
   * body out-of-line: consumers inline at
   * use time, and holding all bodies for
   * cross-function inlining doubles peak
   * RSS on whole-stdlib bitcode (std.bc
   * retention path).
   */
  nyr_inline_callee_t *inline_callees = NULL;
  size_t inline_count = 0;
  bool want_inline = funcs_out && func_count && *func_count > 0 &&
                     (!opt || !opt->emit_module || !*opt->emit_module);
  if (want_inline) {
    inline_callees = calloc(*func_count, sizeof(*inline_callees));
    if (!inline_callees) {
      nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
      for (size_t j = 0; j < *func_count; ++j)
        nyr_ir_func_free(&funcs_out[j]);
      *func_count = 0;
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      nyr_builder_dispose(&compilation_cache);
      return false;
    }
    for (size_t i = 0; i < *func_count; ++i) {
      if (!func_names[i])
        continue;
      /* Keep explicit noinline contracts consistent with LLVM. In
       * particular, borrow-returning identity helpers carry semantic facts
       * on their call sites; replacing the call with its body would erase the
       * place where NYIR records those facts. */
      if (i < all_fn_count && all_fns[i] && all_fns[i]->kind == NY_S_FUNC &&
          all_fns[i]->as.fn.attr_noinline)
        continue;
      /*
       * Bodies with any-to-scalar decodes
       * (`rt_any_to_i64`/`rt_any_to_f64`)
       * are inline candidates like any
       * other.  The inliner substitutes the
       * call's own operand values for the
       * callee formals, so a cloned decode
       * consumes exactly the boundary state
       * a real call would have delivered —
       * boxed where the caller boxes for an
       * `any` formal, raw where the caller
       * passes the raw ABI.  Decodes were
       * formerly excluded here pending that
       * guarantee; operand-identity
       * substitution provides it.
       */
      inline_callees[inline_count].name = func_names[i];
      inline_callees[inline_count].func = &funcs_out[i];
      inline_count++;
    }
    nyr_set_inline_callees(inline_callees, inline_count);
    /*
     * Bound cross-function inlining work without disabling it for the whole
     * program once stdlib loading pushes the function count over the limit.
     * User functions are lowered before their imported dependencies, so this
     * preserves a bounded prefix of the same order while keeping hot user
     * helpers eligible in large programs.
     */
    size_t inline_func_count = *func_count < 512 ? *func_count : 512;
    if (ny_trace_enabled("NY_TRACE_LOWER") && ny_dbg_function_selected(NULL))
      if (ny_dbg_verbosity() >= 6)
        fprintf(stderr, "[build_nir] "
                        "mem2reg/cleanup sweep\n");
    /*
     * Clean and promote all raw function
     * bodies to SSA before offering them to
     * the inliner. This removes raw
     * parameter stores/loads so small leaf
     * functions like add(a, b) and mul(a,
     * b) become immediate inlining
     * candidates.
     */
    for (size_t i = 0; i < *func_count; ++i) {
      (void)nyr_mem2reg(&funcs_out[i]);
      (void)nyr_copy_prop(&funcs_out[i]);
      (void)nyr_dce(&funcs_out[i]);
      (void)nyr_compact(&funcs_out[i]);
      nyr_refresh_metadata(&funcs_out[i]);
    }
    for (int pass = 0; pass < 3; ++pass) {
      if (ny_trace_enabled("NY_TRACE_LOWER") && ny_dbg_function_selected(NULL))
        if (ny_dbg_verbosity() >= 6)
          fprintf(stderr,
                  "[build_nir] "
                  "inline_small pass %d\n",
                  pass);
      for (size_t i = 0; i < inline_func_count; ++i) {
        if (nyr_inline_small(&funcs_out[i])) {
          (void)nyr_mem2reg(&funcs_out[i]);
          (void)nyr_copy_prop(&funcs_out[i]);
          (void)nyr_dce(&funcs_out[i]);
          (void)nyr_compact(&funcs_out[i]);
          nyr_refresh_metadata(&funcs_out[i]);
        }
      }
    }
    /*
     * Inline larger pure wrappers after
     * leaf calls are folded.  This second
     * sweep is what turns a call chain into
     * one direct arithmetic body; the
     * general inliner still enforces its
     * existing size, ABI, and CFG guards.
     */
    for (size_t i = 0; i < inline_func_count; ++i) {
      const char *trace_fn =
          func_names && func_names[i] ? func_names[i] : "<anonymous>";
      bool trace_context = ny_dbg_verbosity() >= 6;
      if (trace_context)
        ny_dbg_enter_context(trace_fn);
      if (ny_trace_enabled("NY_TRACE_LOWER") &&
          ny_dbg_function_selected(trace_fn))
        if (ny_dbg_verbosity() >= 6)
          fprintf(stderr,
                  "[build_nir] "
                  "inline_general fn %zu\n",
                  i);
      bool inline_ok = nyr_inline_general(&funcs_out[i]);
      if (trace_context)
        ny_dbg_exit_context(trace_fn);
      if (inline_ok) {
        nyr_refresh_metadata(&funcs_out[i]);
        continue;
      }
      for (size_t j = 0; j < *func_count; ++j)
        nyr_ir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyr_set_inline_callees(NULL, 0);
      free(inline_callees);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
      nyr_builder_dispose(&compilation_cache);
      return false;
    }
    /*
     * Function bodies were initially
     * finalized before the callee registry
     * was available, so post-build inlining
     * can expose new loops, bounds checks,
     * scalar-replacement opportunities, and
     * vectorizable arithmetic after the
     * original O2 pipeline has already run.
     * Re-run the normal optimizer now while
     * the monomorphic callee table is live.
     * This makes optimization ordering
     * intentional instead of leaving
     * inlined wrapper bodies in a
     * half-optimized state when rt_main
     * later clones them into hot loops.
     */
    for (size_t i = 0; i < inline_func_count; ++i) {
      if (ny_trace_enabled("NY_TRACE_LOWER") &&
          ny_dbg_function_selected(func_names && func_names[i] ? func_names[i]
                                                               : "<anonymous>"))
        if (ny_dbg_verbosity() >= 6)
          fprintf(stderr,
                  "[build_nir] post-inline "
                  "optimize fn %zu\n",
                  i);
      char class_err[256] = {0};
      if (!nyr_resolve_local_store_classes(&funcs_out[i], class_err,
                                           sizeof(class_err))) {
        nyr_set_error(err, err_len, "%s", class_err);
        for (size_t j = 0; j < *func_count; ++j)
          nyr_ir_func_free(&funcs_out[j]);
        *func_count = 0;
        nyr_set_inline_callees(NULL, 0);
        free(inline_callees);
        free(all_fns);
        free(func_names);
        ny_native_profile_clear();
        ny_extern_table_free(&externs);
        nyr_builder_dispose(&compilation_cache);
        return false;
      }
      const char *trace_fn =
          func_names && func_names[i] ? func_names[i] : "<anonymous>";
      /* Scope the optimization-pass detail
       * to this function: stdlib defers to
       * -vvvvvv here too, not only at build
       * time. */
      ny_dbg_lower_fn(trace_fn);
      ny_dbg_scope_stdlib(strchr(trace_fn, '.') != NULL);
      bool trace_context = ny_dbg_is_enabled(NY_DBG_PASS);
      if (trace_context)
        ny_dbg_enter_context(trace_fn);
      /* Stdlib functions defer their
       * per-pass IR dumps to -vvvvvv: the
       * raw dump sink is the direct-fprintf
       * channel the tier gates can't see.
       */
      bool fn_suppressed = ny_dbg_suppressed_by_scope();
      bool function_selected = ny_dbg_function_selected(trace_fn);
      nyr_opt_stats_t function_stats = {0};
      bool opt_ok = false;
      if (opt && opt->nyr_ir_verify) {
        opt_ok = nyr_optimize_debug(
            &funcs_out[i],
            (fn_suppressed || !function_selected) ? NULL : stderr,
            &function_stats, opt_level);
      } else if (opt && opt->nyr_pass_stats) {
        opt_ok =
            nyr_optimize_with_stats(&funcs_out[i], &function_stats, opt_level);
      } else {
        opt_ok = nyr_ir_optimize(&funcs_out[i], opt_level);
      }
      /* Zero-information filter: a run
       * where nothing changed prints at
       * -vvvvvv; at -vvvvv only functions
       * whose NYIR actually moved report.
       */
      bool stats_changed =
          function_stats.before_insts != function_stats.after_insts;
      if (opt_ok && opt && opt->nyr_pass_stats && !opt->nyr_ir_verify &&
          (ny_dbg_verbosity() >= 6 || stats_changed) && function_selected)
        nyr_dump_stats(stderr, &function_stats);
      const char *dump_optimized_fn = getenv("NY_DUMP_FN");
      if (opt_ok && dump_optimized_fn && strstr(trace_fn, dump_optimized_fn))
        nyr_dump(stderr, &funcs_out[i],
                 "optimized function "
                 "before inline");
      if (trace_context)
        ny_dbg_exit_context(trace_fn);
      if (opt_ok) {
        nyr_refresh_metadata(&funcs_out[i]);
        continue;
      }
      const char *dump_failed_fn = getenv("NY_DUMP_FN");
      if (dump_failed_fn && strstr(trace_fn, dump_failed_fn))
        nyr_dump(stderr, &funcs_out[i],
                 "failed function after "
                 "NYIR optimize");
      char failed_name[256];
      snprintf(failed_name, sizeof(failed_name), "%s", trace_fn);
      for (size_t j = 0; j < *func_count; ++j)
        nyr_ir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyr_set_inline_callees(NULL, 0);
      free(inline_callees);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      nyr_set_error(err, err_len,
                    "native NYIR optimization failed "
                    "in '%s'",
                    failed_name);
      nyr_builder_dispose(&compilation_cache);
      return false;
    }
  }

  /*
   * Bodies are now final (dead allocations,
   * stores, and wrappers have been
   * eliminated), so their observable effect
   * sets are trustworthy.  Infer them and
   * tag user-function calls so LICM can
   * hoist provably pure calls (recursive
   * helpers included) out of rt_main's hot
   * loops. Skipped for library precompiles
   * (--emit-module): no rt_main hot loop
   * exists and the fixed-point over
   * thousands of stdlib bodies dominates
   * time/RSS.
   */
  unsigned *call_effect_summaries = NULL;
  ny_native_call_effect_index_t call_effect_index = {0};
  bool want_effects = funcs_out && func_count && *func_count > 0 &&
                      (!opt || !opt->emit_module || !*opt->emit_module);
  if (want_effects) {
    call_effect_summaries = calloc(*func_count, sizeof(*call_effect_summaries));
    if (!call_effect_summaries) {
      nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
      for (size_t j = 0; j < *func_count; ++j)
        nyr_ir_func_free(&funcs_out[j]);
      *func_count = 0;
      nyr_set_inline_callees(NULL, 0);
      free(inline_callees);
      free(all_fns);
      free(func_names);
      ny_native_profile_clear();
      ny_extern_table_free(&externs);
      nyr_builder_dispose(&compilation_cache);
      return false;
    }
    (void)ny_native_call_effect_index_build(&call_effect_index, func_names,
                                            *func_count);
    nyr_compute_call_effects(funcs_out, func_names, *func_count,
                             &call_effect_index, call_effect_summaries);
    /*
     * Tag inside the collected bodies too:
     * any call inlined from them into
     * rt_main later keeps its trusted
     * effect summary.
     */
    for (size_t i = 0; i < *func_count; ++i) {
      bool has_tagged_call =
          nyr_patch_call_effects(&funcs_out[i], func_names, *func_count,
                                 &call_effect_index, call_effect_summaries);
      if (!has_tagged_call)
        continue;
      (void)nyr_cse(&funcs_out[i]);
      (void)nyr_copy_prop(&funcs_out[i]);
      (void)nyr_dce(&funcs_out[i]);
      (void)nyr_compact(&funcs_out[i]);
      nyr_refresh_metadata(&funcs_out[i]);
    }
  }

  /*
   * Build rt_main with extern table.  When
   * requested, run the VM/native result
   * oracle after every optimization pass on
   * rt_main so any pass-level bug is caught
   * before final codegen.
   */
  ny_native_oracle_ctx_t oracle_ctx = {opt, rt_main_out, funcs_out,
                                       (const char **)func_names,
                                       func_count ? *func_count : 0};
  if (ny_trace_enabled("NY_TRACE_LOWER"))
    ny_trace_line("LOWER", "building rt_main");
  if (opt && opt->native_oracle_per_pass && funcs_out && func_count)
    nyr_set_per_pass_oracle(true, ny_native_per_pass_oracle_cb, &oracle_ctx);
  bool ok = nyr_build_rt_main(prog, rt_main_out, &externs, err, err_len,
                              opt_level, opt, &compilation_cache);
  NY_NATIVE_STAGE_TIMED("rt-main-lowering");
  /*
   * Tag rt_main's direct calls to pure user
   * functions before the first optimization
   * pass, so loop-invariant pure calls
   * (e.g. tree_count(4)) are hoisted by
   * LICM instead of re-executed every
   * iteration.
   */
  if (ok && call_effect_summaries && func_count && *func_count > 0)
    nyr_patch_call_effects(rt_main_out, func_names, *func_count,
                           &call_effect_index, call_effect_summaries);
  if (ok && !nyr_promote_private_entry_globals(rt_main_out)) {
    nyr_set_error(err, err_len,
                  "failed to localize private entry "
                  "globals");
    ok = false;
  }
  /* Expose induction and reduction
   * recurrences before the root optimizer's
   * early memory-aware vectorization pass.
   * Entry-only globals just localized above
   * are now ordinary private slots, so
   * immediate promotion is safe. */
  if (ok && !nyr_mem2reg(rt_main_out)) {
    nyr_set_error(err, err_len,
                  "failed to promote private entry "
                  "locals");
    ok = false;
  }
  /* `NY_DUMP_FN=rt_main` also covers the
   * synthesized entry function.  It is the
   * most important body for source-only
   * repros, but historically the filter was
   * applied only to collected user
   * functions. */
  const char *dump_fn = getenv("NY_DUMP_FN");
  bool dump_rt_main = dump_fn && strstr("rt_main", dump_fn) != NULL;
  if (ok && dump_rt_main)
    nyr_dump(stderr, rt_main_out, "rt_main before registry optimize");
  if (ok && !nyr_promote_private_entry_globals(rt_main_out)) {
    nyr_set_error(err, err_len, "failed to localize private rt_main globals");
    ok = false;
  }
  /*
   * Run the full optimization pipeline on
   * rt_main to enable vectorization, loop
   * optimizations, and other transforms
   * before inlining.
   */
  if (ok) {
    bool trace_context = ny_dbg_is_enabled(NY_DBG_PASS);
    if (trace_context)
      ny_dbg_enter_context("rt_main registry optimize");
    nyr_opt_stats_t rt_main_stats = {0};
    bool function_selected = ny_dbg_function_selected("rt_main");
    if (opt && opt->nyr_ir_verify) {
      ok = nyr_optimize_debug(rt_main_out, function_selected ? stderr : NULL,
                              &rt_main_stats, opt_level);
    } else if (opt && opt->nyr_pass_stats) {
      ok = nyr_optimize_with_stats(rt_main_out, &rt_main_stats, opt_level);
    } else {
      ok = nyr_ir_optimize(rt_main_out, opt_level);
    }
    if (ok && opt && opt->nyr_pass_stats && !opt->nyr_ir_verify &&
        (ny_dbg_verbosity() >= 6 ||
         rt_main_stats.before_insts != rt_main_stats.after_insts) &&
        function_selected)
      nyr_dump_stats(stderr, &rt_main_stats);
    if (ok && dump_rt_main)
      nyr_dump(stderr, rt_main_out,
               "rt_main after registry "
               "optimize");
    if (trace_context)
      ny_dbg_exit_context("rt_main registry optimize");
  }
  /*
   * The general inliner deliberately
   * rejects callees that still carry local
   * stores.  The small pure-body splice can
   * remap those private slots safely, so
   * give rt_main the same leaf-wrapper
   * sweep already used for collected user
   * functions.  This is the hot call-chain
   * path: it removes the rt_main -> wrapper
   * call without changing the externally
   * visible ABI.
   */
  if (ok && inline_count > 0) {
    for (int pass = 0; pass < 3; ++pass) {
      if (!nyr_inline_small(rt_main_out))
        break;
      (void)nyr_copy_prop(rt_main_out);
      (void)nyr_dce(rt_main_out);
      nyr_refresh_metadata(rt_main_out);
      if (dump_rt_main)
        nyr_dump(stderr, rt_main_out,
                 "rt_main after "
                 "leaf-wrapper inline");
    }
    /*
     * Keep the general inliner off synthetic rt_main. Cloning CFG-bearing
     * callees here can miscompile loop-carried PHIs in the entry function.
     * Retain the small-wrapper sweep, which does not clone CFGs.
     */
    for (int pass = 0; pass < 3; ++pass) {
      if (!nyr_inline_small(rt_main_out))
        break;
      (void)nyr_copy_prop(rt_main_out);
      (void)nyr_dce(rt_main_out);
      nyr_refresh_metadata(rt_main_out);
    }
    if (dump_rt_main)
      nyr_dump(stderr, rt_main_out,
               "rt_main before "
               "post-inline optimize");
    bool trace_context = ny_dbg_is_enabled(NY_DBG_PASS);
    if (trace_context)
      ny_dbg_enter_context("rt_main post-inline optimize");
    char class_err[256] = {0};
    if (!nyr_resolve_local_store_classes(rt_main_out, class_err,
                                         sizeof(class_err))) {
      nyr_set_error(err, err_len, "%s", class_err);
      ok = false;
    }
    nyr_opt_stats_t post_inline_stats = {0};
    bool optimized = false;
    bool function_selected = ny_dbg_function_selected("rt_main");
    if (ok && opt && opt->nyr_ir_verify) {
      optimized =
          nyr_optimize_debug(rt_main_out, function_selected ? stderr : NULL,
                             &post_inline_stats, opt_level);
    } else if (ok && opt && opt->nyr_pass_stats) {
      optimized =
          nyr_optimize_with_stats(rt_main_out, &post_inline_stats, opt_level);
    } else if (ok) {
      optimized = nyr_ir_optimize(rt_main_out, opt_level);
    }
    if (optimized && opt && opt->nyr_pass_stats && !opt->nyr_ir_verify &&
        (ny_dbg_verbosity() >= 6 ||
         post_inline_stats.before_insts != post_inline_stats.after_insts) &&
        function_selected)
      nyr_dump_stats(stderr, &post_inline_stats);
    if (trace_context)
      ny_dbg_exit_context("rt_main post-inline optimize");
    if (!optimized) {
      if (dump_rt_main)
        nyr_dump(stderr, rt_main_out,
                 "rt_main post-inline "
                 "optimization failed");
      nyr_set_error(err, err_len,
                    "native NYIR: post-inline "
                    "rt_main optimization failed");
      ok = false;
    } else {
      /*
       * rt_main is optimized once before
       * this explicit wrapper sweep.  A
       * second full pipeline is
       * intentional: inlining can expose
       * entire counted loops and
       * managed-buffer accesses that the
       * first pass could not see through a
       * call boundary.  Scalar-only cleanup
       * left those hot regions unvectorized
       * and with stale BCE/LICM
       * opportunities.
       */
      nyr_refresh_metadata(rt_main_out);
      if (dump_rt_main)
        nyr_dump(stderr, rt_main_out,
                 "rt_main after "
                 "post-inline optimize");
    }
  }
  /* Keep the pass oracle installed through
   * rt_main's registry and post-inline
   * optimization pipelines. Disabling it
   * immediately after initial lowering
   * silently skipped the two pipelines
   * where most entry-function rewrites run.
   */
  nyr_set_per_pass_oracle(false, NULL, NULL);
  nyr_set_inline_callees(NULL, 0);
  free(inline_callees);
  nyr_set_tv_seed(0);

  /*
   * Inlining passes (including mem2reg) may
   * have introduced PHIs in user functions
   * and rt_main.  For backends that don't
   * preserve PHIs (i386, portable, etc.),
   * eliminate them now before codegen.
   */
  if (!nyr_get_preserve_phis()) {
    for (size_t i = 0; funcs_out && i < *func_count; ++i) {
      if (!nyr_phi_elim(&funcs_out[i]) || nyr_has_phi(&funcs_out[i])) {
        if (err && err_len > 0 && err[0] == '\0')
          snprintf(err, err_len,
                   "native NYIR: phi "
                   "elimination could not "
                   "lower PHI in %s",
                   func_names && func_names[i] ? func_names[i] : "unknown_fn");
        ok = false;
        break;
      }
      nyr_refresh_metadata(&funcs_out[i]);
    }
    if (ok && rt_main_out) {
      if (!nyr_phi_elim(rt_main_out) || nyr_has_phi(rt_main_out)) {
        if (err && err_len > 0 && err[0] == '\0')
          snprintf(err, err_len,
                   "native NYIR: phi "
                   "elimination could not "
                   "lower PHI in rt_main");
        ok = false;
      }
      nyr_refresh_metadata(rt_main_out);
    }
  }

  /* PHI elimination runs after the normal
   * optimizer/verifier pipeline and may
   * introduce local stores whose
   * scalar-class metadata is not carried
   * from the incoming edge. Restore the
   * agreed slot facts at the shared build
   * boundary so every backend sees the same
   * final ABI classes. */
  for (size_t i = 0; ok && funcs_out && i < *func_count; ++i)
    nyr_reconcile_local_scalar_classes(&funcs_out[i]);
  if (ok && rt_main_out)
    nyr_reconcile_local_scalar_classes(rt_main_out);

  /* Function bodies may change again during
   * cross-function inlining and class
   * reconciliation. The earlier dump alone
   * is not the encoder input. */
  const char *final_dump_fn = getenv("NY_DUMP_FN");
  for (size_t i = 0; ok && final_dump_fn && funcs_out && i < *func_count; ++i) {
    if (!func_names[i] || !strstr(func_names[i], final_dump_fn))
      continue;
    char dump_name[384];
    snprintf(dump_name, sizeof(dump_name), "%s final NYIR", func_names[i]);
    nyr_dump(stderr, &funcs_out[i], dump_name);
  }

  free(call_effect_summaries);
  free(call_effect_index.entries);
  /* The temporary compilation owner goes
   * away on return. Keep the public
   * function-name views with their NYIR
   * bodies, which callers free together. */
  if (func_names_out) {
    for (size_t i = 0; funcs_out && i < *func_count; ++i) {
      func_names_out[i] =
          nyr_func_own_symbol_copy(&funcs_out[i], func_names[i]);
      if (!func_names_out[i]) {
        for (size_t j = 0; j < *func_count; ++j)
          nyr_ir_func_free(&funcs_out[j]);
        *func_count = 0;
        nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
        ok = false;
        break;
      }
    }
  }
  ny_extern_table_free(&externs);
  ny_native_profile_clear();
  free(all_fns);
  free(func_names);
  nyr_builder_dispose(&compilation_cache);
#undef NY_NATIVE_STAGE_TIMED
  return ok;
}

bool nyr_dump_function(FILE *out, const program_t *prog, const stmt_t *fn,
                       char *err, size_t err_len, const ny_options *opt) {
  if (!fn || fn->kind != NY_S_FUNC)
    return true;
  nyr_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyr_set_pass_controls(opt ? opt->nyr_disable_pass : NULL,
                        opt ? opt->nyr_stop_after : NULL);
  nyr_set_verify_each_pass(opt && opt->nyr_ir_verify);
  nyr_set_rollback_on_verify_fail(opt && opt->nyr_rollback_on_verify_fail);
  nyr_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_dbg_init_from_verbosity(opt ? opt->verbosity : 0);
  nyr_set_track_value(opt ? opt->nyr_track_value : -1);
  nyr_builder_impl_t b = {.last_value = -1,
                          .err = err,
                          .err_len = err_len,
                          .prog = prog,
                          .options = opt,
                          .cache_owner = &b,
                          .current_fn_name = fn->as.fn.name,
                          .source_file = fn->tok.filename,
                          .module_name = NULL,
                          .opt_level = opt ? opt->opt_level : 1};
  ny_native_fn_index_t *fn_index = ny_native_fn_index_for(&b);
  if (!fn_index) {
    nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
    nyr_builder_dispose(&b);
    return false;
  }
  ny_native_fn_cache_build(fn_index, prog);
  if (fn_index->build_failed) {
    nyr_set_error(err, err_len, NY_NATIVE_ALLOC_FAIL);
    nyr_builder_dispose(&b);
    return false;
  }
  b.module_name = ny_native_fn_module(fn_index, prog, fn);
  b.return_type = fn ? fn->as.fn.return_type : NULL;
  b.return_any =
      (fn->as.fn.return_type &&
       ny_native_type_name_is_any(fn->as.fn.return_type)) ||
      (fn->as.fn.return_semantic.resolved &&
       fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC &&
       ny_sem_rep_coherent_with_type(fn->as.fn.return_type,
                                     NY_SEM_REP_TAGGED_DYNAMIC)) ||
      (!fn->as.fn.return_type && fn->as.fn.return_semantic.resolved &&
       fn->as.fn.return_semantic.rep == NY_SEM_REP_TAGGED_DYNAMIC);
  if (ny_native_type_name_is_f64(fn->as.fn.return_type) ||
      nyr_rep_for_type(&b, fn->as.fn.return_type) == NY_SEM_REP_F64 ||
      (!fn->as.fn.return_type && fn->as.fn.return_semantic.resolved &&
       fn->as.fn.return_semantic.rep == NY_SEM_REP_F64 && !b.return_any))
    b.return_class = NYIR_SCALAR_F64;
  else if (ny_native_type_name_is_f32(fn->as.fn.return_type) ||
           nyr_rep_for_type(&b, fn->as.fn.return_type) == NY_SEM_REP_F32 ||
           (!fn->as.fn.return_type && fn->as.fn.return_semantic.resolved &&
            fn->as.fn.return_semantic.rep == NY_SEM_REP_F32 && !b.return_any))
    b.return_class = NYIR_SCALAR_F32;
  else if (!b.return_any && fn->as.fn.return_type &&
           ny_type_is_bigint(fn->as.fn.return_type))
    b.return_class = NYIR_SCALAR_I64_TAGGED;
  if (!nyr_set_param_types(&b, fn)) {
    nyr_ir_func_free(&b.nyir);
    nyr_builder_dispose(&b);
    return false;
  }
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    const char *param_name = fn->as.fn.params.data[i].name;
    if (param_name && strcmp(param_name, "_") == 0)
      continue;
    const ny_expr_semantic_t *sem = &fn->as.fn.params.data[i].semantic;
    bool try_dynamic = nyr_param_is_try_dynamic(&b, fn, i);
    bool dynamic_sequence =
        !try_dynamic && nyr_param_uses_dynamic_sequence_abi(fn, i);
    ny_sem_rep_t effective_rep =
        try_dynamic
            ? NY_SEM_REP_TAGGED_DYNAMIC
            : (fn->as.fn.params.data[i].type &&
                       strstr(fn->as.fn.params.data[i].type, "fnptr")
                   ? NY_SEM_REP_CLOSURE
                   : (fn->as.fn.params.data[i].type &&
                              *fn->as.fn.params.data[i].type &&
                              strcmp(fn->as.fn.params.data[i].type, "set") !=
                                  0 &&
                              (nyr_find_refine_in_prog(
                                   &b, b.prog, fn->as.fn.params.data[i].type) ||
                               nyr_find_impl_type(
                                   &b, fn->as.fn.params.data[i].type))
                          ? nyr_rep_for_type(&b, fn->as.fn.params.data[i].type)
                          : (sem->resolved ? sem->rep
                                           : NY_SEM_REP_UNRESOLVED)));
    bool is_list =
        !try_dynamic && !dynamic_sequence &&
        (ny_native_type_name_is_list(fn->as.fn.params.data[i].type) ||
         nyr_param_is_inferred_list(fn, i) ||
         (!fn->as.fn.params.data[i].type &&
          effective_rep == NY_SEM_REP_TYPED_BUFFER) ||
         (fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1));
    bool is_str = !try_dynamic &&
                  (ny_native_type_name_is_str(fn->as.fn.params.data[i].type) ||
                   (!fn->as.fn.params.data[i].type &&
                    effective_rep == NY_SEM_REP_STRING));
    bool is_f64 =
        ny_native_type_name_is_f64(fn->as.fn.params.data[i].type) ||
        (!try_dynamic && !fn->as.fn.params.data[i].type && sem->resolved &&
         sem->rep == NY_SEM_REP_F64) ||
        (!try_dynamic && sem->resolved && sem->rep == NY_SEM_REP_F64 &&
         ny_sem_rep_coherent_with_type(fn->as.fn.params.data[i].type,
                                       NY_SEM_REP_F64));
    bool is_f32 =
        ny_native_type_name_is_f32(fn->as.fn.params.data[i].type) ||
        (!try_dynamic && !fn->as.fn.params.data[i].type && sem->resolved &&
         sem->rep == NY_SEM_REP_F32) ||
        (!try_dynamic && sem->resolved && sem->rep == NY_SEM_REP_F32 &&
         ny_sem_rep_coherent_with_type(fn->as.fn.params.data[i].type,
                                       NY_SEM_REP_F32));
    bool is_bigint = !try_dynamic && fn->as.fn.params.data[i].type &&
                     ny_type_is_bigint(fn->as.fn.params.data[i].type);
    bool is_any = try_dynamic || dynamic_sequence ||
                  ny_native_type_name_is_any(fn->as.fn.params.data[i].type) ||
                  (!fn->as.fn.params.data[i].type && !is_list && !is_str &&
                   !is_f64 && !is_f32 && effective_rep != NY_SEM_REP_RAW_INT);
    nyr_local_t *param = nyr_bind_local_typed(
        &b, fn->as.fn.params.data[i].name, is_f64, is_f32,
        fn->as.fn.params.data[i].type &&
            strcmp(fn->as.fn.params.data[i].type, "str") == 0);
    if (!param) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      return false;
    }
    param->semantic_rep = is_any ? NY_SEM_REP_TAGGED_DYNAMIC : effective_rep;
    param->semantic_ownership = sem->ownership;
    param->alias_class = sem->alias_class;
    param->semantic_mutable = sem->mutable_value;
    /* Keep the local representation
     * identical to the ABI assembled by
     * nyr_set_param_types above.  An
     * unannotated scalar whose HM type is
     * still unresolved is a dynamic value
     * at the function boundary; treating
     * its local as raw while reserving the
     * extra value/tag slots lets the body
     * consume the first metadata word as an
     * integer. */
    param->is_any = try_dynamic || dynamic_sequence ||
                    ny_native_type_name_is_any(fn->as.fn.params.data[i].type) ||
                    (!fn->as.fn.params.data[i].type && !is_list && !is_str &&
                     !is_f64 && !is_f32 && effective_rep != NY_SEM_REP_RAW_INT);
    param->type_name = fn->as.fn.params.data[i].type;
    param->is_bigint = is_bigint;
    param->is_dict =
        (!is_bigint && fn->as.fn.params.data[i].type &&
         !ny_native_type_name_is_any(fn->as.fn.params.data[i].type) &&
         (sem->resolved && effective_rep == NY_SEM_REP_OBJECT)) ||
        (fn->as.fn.params.data[i].type &&
         (strcmp(fn->as.fn.params.data[i].type, "dict") == 0 ||
          ny_type_vector_dimension(fn->as.fn.params.data[i].type) > 0));
    param->is_list =
        !try_dynamic && !dynamic_sequence &&
        ((sem->resolved && sem->rep == NY_SEM_REP_TYPED_BUFFER) ||
         ny_native_type_name_is_list(fn->as.fn.params.data[i].type) ||
         nyr_param_is_inferred_list(fn, i) ||
         (fn->as.fn.is_variadic && i == fn->as.fn.params.len - 1));
    param->is_dyn_list =
        param->is_list &&
        ((fn->as.fn.is_variadic && i + 1 == fn->as.fn.params.len) ||
         ny_native_type_name_is_dyn_list(fn->as.fn.params.data[i].type) ||
         nyr_function_param_is_dyn_list(b.cache_owner, b.prog, fn, i));
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
  bool ok = nyr_lower_stmt(&b, fn->as.fn.body);
  bool has_implicit_tail =
      fn->as.fn.body && (fn->as.fn.body->kind == NY_S_EXPR ||
                         (fn->as.fn.body->kind == NY_S_BLOCK &&
                          fn->as.fn.body->as.block.body.len > 0 &&
                          fn->as.fn.body->as.block.body
                                  .data[fn->as.fn.body->as.block.body.len - 1]
                                  ->kind == NY_S_EXPR));
  if (ok && !b.emitted_return && !has_implicit_tail &&
      !nyr_stmt_has_return(fn->as.fn.body) &&
      !nyr_stmt_has_control_flow(fn->as.fn.body)) {
    const char *rt = fn->as.fn.return_type;
    const char *dot = rt ? strrchr(rt, '.') : NULL;
    const char *base = dot ? dot + 1 : rt;
    if (base && *base && strcmp(base, "void") != 0 &&
        strcmp(base, "any") != 0 && strcmp(base, "nil") != 0) {
      ok = nyr_fail(&b,
                    "function '%s' falls off the end "
                    "without returning a %s value",
                    fn->as.fn.name ? fn->as.fn.name : "<fn>", base);
    }
  }
  if (ok && !b.emitted_return) {
    /*
     * Capture the implicit return value
     * before defer bodies run: defer
     * statements lower as expression
     * statements and would clobber
     * last_value.
     */
    int ret = b.last_value >= 0 ? b.last_value : nyr_emit_const(&b, 0);
    ok = ret >= 0 && nyr_emit_defers(&b, 0);
    if (ok)
      ok = nyr_emit_ret(&b, ret);
  }
  if (ok)
    ok = nyr_opt_dump(out, &b, fn->as.fn.name ? fn->as.fn.name : "<fn>", opt);
  nyr_ir_func_free(&b.nyir);
  nyr_builder_dispose(&b);
  return ok;
}

bool nyr_dump_rt_main(FILE *out, const program_t *prog, char *err,
                      size_t err_len, const ny_options *opt) {
  nyr_set_cf_mem2reg_enabled(!opt || opt->native_enable_cf_mem2reg);
  nyr_set_pass_controls(opt ? opt->nyr_disable_pass : NULL,
                        opt ? opt->nyr_stop_after : NULL);
  nyr_set_verify_each_pass(opt && opt->nyr_ir_verify);
  nyr_set_rollback_on_verify_fail(opt && opt->nyr_rollback_on_verify_fail);
  nyr_set_global_inst_cap(opt ? opt->nyr_global_inst_cap : 0);
  nyr_set_pass_growth_factor(opt ? opt->nyr_pass_growth_factor : 0.0);
  nyr_set_tv_seed(opt ? opt->native_tv_seed_trials : 0);
  ny_dbg_init_from_verbosity(opt ? opt->verbosity : 0);
  nyr_set_track_value(opt ? opt->nyr_track_value : -1);
  ny_extern_table_t externs;
  bool aapcs = opt && (opt->native_backend == NY_NATIVE_BACKEND_AARCH64 ||
                       opt->native_abi == NY_NATIVE_ABI_AAPCS);
  if (!nyr_build_extern_table(prog, &externs, aapcs, opt, err, err_len)) {
    ny_extern_table_free(&externs);
    return false;
  }
  nyr_builder_impl_t b = {.last_value = -1,
                          .err = err,
                          .err_len = err_len,
                          .externs = &externs,
                          .prog = prog,
                          .options = opt,
                          .profile_name = "rt_main",
                          .current_fn_name = "rt_main",
                          .opt_level = opt ? opt->opt_level : 1};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!nyr_lower_stmt(&b, prog->body.data[i])) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      b.last_value = nyr_emit_const(&b, 0);
      if (b.last_value < 0) {
        nyr_ir_func_free(&b.nyir);
        nyr_builder_dispose(&b);
        ny_extern_table_free(&externs);
        return false;
      }
    }
    if (!nyr_emit_ret(&b, b.last_value)) {
      nyr_ir_func_free(&b.nyir);
      nyr_builder_dispose(&b);
      ny_extern_table_free(&externs);
      return false;
    }
  }
  bool ok = nyr_opt_dump(out, &b, "rt_main", opt);
  nyr_ir_func_free(&b.nyir);
  nyr_builder_dispose(&b);
  ny_extern_table_free(&externs);
  NY_NATIVE_STAGE("done");
#undef NY_NATIVE_STAGE
  return ok;
}

size_t nyr_local_count(const nyr_func_t *f);
bool nyr_ensure_parent_dir_for_path(const char *path);

static bool nyr_write_u16le(FILE *out, uint16_t value) {
  unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
  return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

static bool nyr_write_u32le(FILE *out, uint32_t value) {
  unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8),
                            (unsigned char)(value >> 16),
                            (unsigned char)(value >> 24)};
  return fwrite(bytes, 1, sizeof(bytes), out) == sizeof(bytes);
}

static bool nyr_write_binary_blob(FILE *out, const nyr_func_t *nyir,
                                  const char *name, char *err, size_t err_len) {
  FILE *tmp = tmpfile();
  if (!tmp)
    return nyr_set_error(err, err_len,
                         "native NYIR bundle: failed "
                         "to create temporary blob"),
           false;
  bool ok = nyr_dump_binary(tmp, nyir, name);
  long bytes =
      ok && fflush(tmp) == 0 && fseek(tmp, 0, SEEK_END) == 0 ? ftell(tmp) : -1;
  if (!ok || bytes < 0 || (unsigned long)bytes > UINT32_MAX ||
      fseek(tmp, 0, SEEK_SET) != 0 || !nyr_write_u32le(out, (uint32_t)bytes)) {
    fclose(tmp);
    return nyr_set_error(err, err_len,
                         "native NYIR bundle: failed "
                         "to encode %s",
                         name),
           false;
  }
  unsigned char buffer[4096];
  size_t left = (size_t)bytes;
  while (left > 0) {
    size_t want = left < sizeof(buffer) ? left : sizeof(buffer);
    size_t got = fread(buffer, 1, want, tmp);
    if (got != want || fwrite(buffer, 1, got, out) != got) {
      fclose(tmp);
      return nyr_set_error(err, err_len,
                           "native NYIR bundle: "
                           "failed to write %s",
                           name),
             false;
    }
    left -= got;
  }
  fclose(tmp);
  return true;
}

bool nyr_dump_program_binary(FILE *out, const program_t *prog,
                             const ny_options *opt, char *err, size_t err_len) {
  if (!out || !prog)
    return nyr_set_error(err, err_len,
                         "native NYIR bundle: "
                         "missing program output"),
           false;
  enum { NYR_BUNDLE_VERSION = 1 };
  nyr_func_t rt_main = {0};
  size_t wanted = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      wanted++;
  }
  if (wanted > NYR_BUNDLE_MAX_FUNCS)
    return nyr_set_error(err, err_len,
                         "native NYIR bundle: %zu "
                         "functions exceed bundle "
                         "limit %u",
                         wanted, NYR_BUNDLE_MAX_FUNCS),
           false;
  nyr_func_t *funcs = wanted ? calloc(wanted, sizeof(*funcs)) : NULL;
  const char **names = wanted ? calloc(wanted, sizeof(*names)) : NULL;
  if (wanted && (!funcs || !names)) {
    free(funcs);
    free(names);
    return nyr_set_error(err, err_len, NY_NATIVE_BUNDLE_OOM), false;
  }
  size_t named = 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[named++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }
  size_t count = 0;
  bool ok = nyr_build_ir(prog, opt, &rt_main, funcs, &count, names, wanted, err,
                         err_len);
  if (!ok || count != wanted) {
    if (ok)
      nyr_set_error(err, err_len,
                    "native NYIR bundle: every user "
                    "function must lower "
                    "before serialization");
    ok = false;
    goto done;
  }
  if (fwrite("NYIP", 1, 4, out) != 4 ||
      !nyr_write_u16le(out, NYR_BUNDLE_VERSION) || !nyr_write_u16le(out, 0) ||
      !nyr_write_u32le(out, (uint32_t)(count + 1)) ||
      !nyr_write_binary_blob(out, &rt_main, "rt_main", err, err_len)) {
    ok = false;
    goto done;
  }
  for (size_t i = 0; i < count; ++i)
    if (!nyr_write_binary_blob(out, &funcs[i], names[i], err, err_len)) {
      ok = false;
      goto done;
    }
done:
  nyr_ir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    nyr_ir_func_free(&funcs[i]);
  free(funcs);
  free(names);
  return ok;
}

bool nyr_write_ir_metadata_report(const program_t *prog, const ny_options *opt,
                                  char *err, size_t err_len) {
  if (!opt || !opt->nyr_metadata_report)
    return true;
  FILE *out = stderr;
  if (opt->nyr_metadata_report_path && opt->nyr_metadata_report_path[0]) {
    nyr_ensure_parent_dir_for_path(opt->nyr_metadata_report_path);
    out = fopen(opt->nyr_metadata_report_path, "wb");
    if (!out) {
      nyr_set_error(err, err_len,
                    "native NYIR metadata: failed to "
                    "open %s: %s",
                    opt->nyr_metadata_report_path, strerror(errno));
      return false;
    }
  }

  if (opt->nyr_metadata_bin_path && opt->nyr_metadata_bin_path[0]) {
    FILE *in = fopen(opt->nyr_metadata_bin_path, "rb");
    if (!in) {
      if (out != stderr)
        fclose(out);
      nyr_set_error(err, err_len,
                    "native NYIR metadata: failed to "
                    "open %s: %s",
                    opt->nyr_metadata_bin_path, strerror(errno));
      return false;
    }
    nyr_func_t f = {0};
    char name[128] = {0};
    char local_err[512] = {0};
    bool ok = nyr_load_binary(in, &f, name, sizeof(name), local_err,
                              sizeof(local_err));
    fclose(in);
    if (ok) {
      nyr_metadata_summary_t summary = {0};
      ok = nyr_metadata_summary(&f, &summary, local_err, sizeof(local_err));
      if (ok) {
        fprintf(out,
                "nyir metadata report "
                "functions=1 source=binary "
                "path=%s\n",
                opt->nyr_metadata_bin_path);
        nyr_metadata_summary_dump(out, name[0] ? name : "rt_main", &summary);
      }
    }
    nyr_ir_func_free(&f);
    if (out != stderr)
      fclose(out);
    if (!ok) {
      nyr_set_error(err, err_len, "%s",
                    local_err[0] ? local_err
                                 : "native NYIR binary "
                                   "metadata failed");
      return false;
    }
    if (err && err_len > 0)
      err[0] = '\0';
    return true;
  }

  nyr_func_t rt_main = {0};
  size_t func_cap = NYR_BUNDLE_MAX_FUNCS;
  nyr_func_t *funcs = calloc(func_cap, sizeof(*funcs));
  const char **names = calloc(func_cap, sizeof(*names));
  size_t func_count = 0;
  char local_err[512] = {0};
  if (!funcs || !names) {
    free(funcs);
    free(names);
    if (out != stderr)
      fclose(out);
    nyr_set_error(err, err_len,
                  "native metadata function pool "
                  "allocation failed");
    return false;
  }
  bool ok = nyr_build_ir(prog, opt, &rt_main, funcs, &func_count, names,
                         func_cap, local_err, sizeof(local_err));

  if (!ok) {
    if (out != stderr)
      fclose(out);
    nyr_set_error(err, err_len, "%s",
                  local_err[0] ? local_err : "native NYIR build failed");
    return false;
  }

  fprintf(out,
          "nyir metadata report "
          "functions=%zu\n",
          func_count + 1);
  for (size_t i = 0; i < func_count; ++i) {
    nyr_metadata_summary_t summary = {0};
    if (!nyr_metadata_summary(&funcs[i], &summary, local_err,
                              sizeof(local_err))) {
      ok = false;
      break;
    }
    nyr_metadata_summary_dump(out, names[i] ? names[i] : "<fn>", &summary);
  }
  if (ok) {
    nyr_metadata_summary_t summary = {0};
    if (nyr_metadata_summary(&rt_main, &summary, local_err, sizeof(local_err)))
      nyr_metadata_summary_dump(out, "rt_main", &summary);
    else
      ok = false;
  }

  nyr_ir_func_free(&rt_main);
  for (size_t i = 0; i < func_count; ++i)
    nyr_ir_func_free(&funcs[i]);
  free(funcs);
  free(names);
  if (out != stderr)
    fclose(out);
  if (!ok) {
    nyr_set_error(err, err_len, "%s",
                  local_err[0] ? local_err
                               : "native NYIR "
                                 "metadata failed");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
