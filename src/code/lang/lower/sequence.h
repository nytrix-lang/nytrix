/*
 * Native lowering: infer dynamic sequence and try/catch parameter ABIs.
 */
static bool nyr_stmt_iterates_param(const stmt_t *s, const char *name,
                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.iterable && s->as.fr.iterable->kind == NY_E_IDENT &&
        s->as.fr.iterable->as.ident.name &&
        strcmp(s->as.fr.iterable->as.ident.name, name) == 0)
      return true;
    return nyr_stmt_iterates_param(s->as.fr.body, name, depth + 1);
  }
  if (s->kind == NY_S_BLOCK || s->kind == NY_S_MODULE) {
    const ny_stmt_list *body =
        s->kind == NY_S_BLOCK ? &s->as.block.body : &s->as.module.body;
    for (size_t i = 0; i < body->len; ++i)
      if (nyr_stmt_iterates_param(body->data[i], name, depth + 1))
        return true;
  }
  return false;
}

/*
 * Infer a list ABI from operations that are specific to sequence mutation or
 * traversal. Generic `get`, `set`, and `contains` are deliberately excluded:
 * dictionaries expose the same surface.
 */
static bool nyr_expr_uses_list_surface(const expr_t *e, const char *name,
                                       unsigned depth) {
  if (!e || !name || depth > 64)
    return false;
  switch (e->kind) {
  case NY_E_MEMCALL: {
    const char *method = e->as.memcall.name;
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        strcmp(e->as.memcall.target->as.ident.name, name) == 0 && method &&
        (strcmp(method, "append") == 0 || strcmp(method, "extend") == 0 ||
         strcmp(method, "push") == 0 || strcmp(method, "pop") == 0 ||
         strcmp(method, "slice") == 0 || strcmp(method, "clear") == 0 ||
         strcmp(method, "sort") == 0 || strcmp(method, "sorted") == 0 ||
         strcmp(method, "clone") == 0 || strcmp(method, "insert") == 0))
      return true;
    if (nyr_expr_uses_list_surface(e->as.memcall.target, name, depth + 1))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (nyr_expr_uses_list_surface(e->as.memcall.args.data[i].val, name,
                                     depth + 1))
        return true;
    return false;
  }
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.target->kind == NY_E_IDENT &&
        e->as.call.callee->as.member.target->as.ident.name &&
        strcmp(e->as.call.callee->as.member.target->as.ident.name, name) == 0 &&
        e->as.call.callee->as.member.name &&
        (strcmp(e->as.call.callee->as.member.name, "append") == 0 ||
         strcmp(e->as.call.callee->as.member.name, "push") == 0 ||
         strcmp(e->as.call.callee->as.member.name, "pop") == 0 ||
         strcmp(e->as.call.callee->as.member.name, "slice") == 0))
      return true;
    if (nyr_expr_uses_list_surface(e->as.call.callee, name, depth + 1))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (nyr_expr_uses_list_surface(e->as.call.args.data[i].val, name,
                                     depth + 1))
        return true;
    return false;
  case NY_E_BINARY:
    return nyr_expr_uses_list_surface(e->as.binary.left, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.binary.right, name, depth + 1);
  case NY_E_UNARY:
    return nyr_expr_uses_list_surface(e->as.unary.right, name, depth + 1);
  case NY_E_INDEX:
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        e->as.index.target->as.ident.name &&
        strcmp(e->as.index.target->as.ident.name, name) == 0)
      return true;
    return nyr_expr_uses_list_surface(e->as.index.target, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.index.start, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.index.stop, name, depth + 1) ||
           nyr_expr_uses_list_surface(e->as.index.step, name, depth + 1);
  case NY_E_MEMBER:
    return nyr_expr_uses_list_surface(e->as.member.target, name, depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (nyr_expr_uses_list_surface(e->as.list_like.data[i], name, depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool nyr_stmt_uses_list_surface(const stmt_t *s, const char *name,
                                       unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_EXPR:
    return nyr_expr_uses_list_surface(s->as.expr.expr, name, depth + 1);
  case NY_S_RETURN:
    return nyr_expr_uses_list_surface(s->as.ret.value, name, depth + 1);
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (nyr_expr_uses_list_surface(s->as.var.exprs.data[i], name, depth + 1))
        return true;
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_uses_list_surface(s->as.block.body.data[i], name, depth + 1))
        return true;
    return false;
  case NY_S_WHILE:
    return nyr_expr_uses_list_surface(s->as.whl.test, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.whl.body, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.whl.update, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.whl.init, name, depth + 1);
  case NY_S_IF:
    return nyr_expr_uses_list_surface(s->as.iff.test, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.iff.conseq, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.iff.alt, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.iff.init, name, depth + 1);
  case NY_S_FOR:
    return nyr_expr_uses_list_surface(s->as.fr.iterable, name, depth + 1) ||
           nyr_expr_uses_list_surface(s->as.fr.cond, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.fr.init, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.fr.update, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.fr.body, name, depth + 1);
  case NY_S_GUARD:
    return nyr_expr_uses_list_surface(s->as.guard.value, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.guard.fallback, name, depth + 1);
  case NY_S_TRY:
    return nyr_stmt_uses_list_surface(s->as.tr.body, name, depth + 1) ||
           nyr_stmt_uses_list_surface(s->as.tr.handler, name, depth + 1);
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (nyr_stmt_uses_list_surface(s->as.module.body.data[i], name,
                                     depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool nyr_param_is_inferred_list(const stmt_t *fn, size_t index) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len)
    return false;
  const char *type = fn->as.fn.params.data[index].type;
  const char *name = fn->as.fn.params.data[index].name;
  bool unparameterized =
      !type || strcmp(type, "list") == 0 || strcmp(type, "seq") == 0;
  return name && unparameterized &&
         (nyr_stmt_iterates_param(fn->as.fn.body, name, 0) ||
          nyr_stmt_uses_list_surface(fn->as.fn.body, name, 0));
}

static bool nyr_param_uses_dynamic_sequence_abi(const stmt_t *fn,
                                                size_t index) {
  return fn && fn->kind == NY_S_FUNC && index < fn->as.fn.params.len &&
         !(fn->as.fn.is_variadic && index + 1 == fn->as.fn.params.len) &&
         (!fn->as.fn.params.data[index].type ||
          strcmp(fn->as.fn.params.data[index].type, "list") == 0 ||
          strcmp(fn->as.fn.params.data[index].type, "seq") == 0) &&
         nyr_param_is_inferred_list(fn, index);
}

static bool nyr_stmt_try_uses_ident(const stmt_t *s, const char *name,
                                    unsigned depth) {
  if (!s || !name || depth > 64)
    return false;
  switch (s->kind) {
  case NY_S_TRY:
    if (s->as.tr.body && nyr_stmt_uses_ident(s->as.tr.body, name))
      return true;
    return nyr_stmt_try_uses_ident(s->as.tr.handler, name, depth + 1);
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (nyr_stmt_try_uses_ident(s->as.block.body.data[i], name, depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return nyr_stmt_try_uses_ident(s->as.iff.init, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.iff.conseq, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.iff.alt, name, depth + 1);
  case NY_S_WHILE:
    return nyr_stmt_try_uses_ident(s->as.whl.init, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.whl.body, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.whl.update, name, depth + 1);
  case NY_S_FOR:
    return nyr_stmt_try_uses_ident(s->as.fr.init, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.fr.body, name, depth + 1) ||
           nyr_stmt_try_uses_ident(s->as.fr.update, name, depth + 1);
  case NY_S_GUARD:
    return nyr_stmt_try_uses_ident(s->as.guard.fallback, name, depth + 1);
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (nyr_stmt_try_uses_ident(s->as.module.body.data[i], name, depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

#define NY_TRY_DYNAMIC_CACHE_MAX 4096u

static bool nyr_param_is_try_dynamic(const nyr_builder_impl_t *b,
                                     const stmt_t *fn, size_t index) {
  if (!fn || fn->kind != NY_S_FUNC || index >= fn->as.fn.params.len ||
      fn->as.fn.params.data[index].type)
    return false;

  uintptr_t key = ((uintptr_t)fn >> 4) ^ (index * UINT64_C(0x9e3779b97f4a7c15));
  size_t slot = (size_t)(key % NY_TRY_DYNAMIC_CACHE_MAX);
  nyr_builder_impl_t *owner =
      b && b->cache_owner ? b->cache_owner : (nyr_builder_t *)b;
  if (!owner)
    goto uncached;
  owner->cache_owner = owner;
  if (!owner->try_dynamic_cache) {
    owner->try_dynamic_cache =
        calloc(NY_TRY_DYNAMIC_CACHE_MAX, sizeof(*owner->try_dynamic_cache));
    if (owner->try_dynamic_cache)
      owner->try_dynamic_cache_cap = NY_TRY_DYNAMIC_CACHE_MAX;
  }
  if (!owner->try_dynamic_cache)
    goto uncached;
  for (size_t probe = 0; probe < NY_TRY_DYNAMIC_CACHE_MAX; ++probe) {
    ny_try_dynamic_cache_ent_t *ent =
        &owner->try_dynamic_cache[(slot + probe) % NY_TRY_DYNAMIC_CACHE_MAX];
    if (ent->fn == fn && ent->index == index)
      return ent->value;
    if (!ent->fn) {
      const char *name = fn->as.fn.params.data[index].name;
      ent->fn = fn;
      ent->index = index;
      ent->value = name && nyr_stmt_try_uses_ident(fn->as.fn.body, name, 0);
      return ent->value;
    }
  }

uncached:;
  const char *name = fn->as.fn.params.data[index].name;
  return name && nyr_stmt_try_uses_ident(fn->as.fn.body, name, 0);
}
