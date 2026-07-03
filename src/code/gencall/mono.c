static int ny_mono_env_int(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  char *end = NULL;
  long n = strtol(v, &end, 10);
  if (end == v)
    return fallback;
  if (n < 0)
    return 0;
  if (n > 1000000)
    return 1000000;
  return (int)n;
}

static bool ny_mono_enabled(codegen_t *cg) {
  const char *forced = getenv("NYTRIX_MONO_TYPES");
  if (!forced || !*forced)
    forced = getenv("NYTRIX_ENABLE_MONOMORPHIZATION");
  if (forced && *forced)
    return ny_env_truthy(forced);
  if (ny_env_enabled("NYTRIX_DISABLE_MONO_TYPES") ||
      ny_env_enabled("NYTRIX_DISABLE_MONOMORPHIZATION"))
    return false;
  if (ny_env_enabled("NYTRIX_MONO_LIST_ARGS"))
    return true;
  return ny_codegen_speed_profile_enabled(cg);
}

static bool ny_mono_trace_enabled(void) {
  return ny_env_enabled("NYTRIX_MONO_TRACE") ||
         ny_env_enabled("NYTRIX_TRACE_MONO_TYPES");
}

static LLVMValueRef ny_try_inline_simple_raw_int_call(codegen_t *cg,
                                                      scope *scopes,
                                                      size_t depth, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return NULL;
  if (!ny_codegen_speed_profile_enabled(cg) &&
      !ny_env_enabled("NYTRIX_SIMPLE_RAW_INT_CALL_FAST"))
    return NULL;
  if (!ny_is_proven_int(cg, scopes, depth, e, NULL))
    return NULL;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!ny_build_mono_raw_int_expr(cg, scopes, depth, e, &raw, &ok) || !raw ||
      !ok)
    return NULL;
  if (!LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return NULL;
  cg->mono_inline_body_uses++;
  return ny_tag_int(cg, raw);
}

static const char *ny_mono_type_name(uint8_t kind) {
  switch ((ny_mono_type_kind_t)kind) {
  case NY_MONO_TYPE_INT:
    return "int";
  case NY_MONO_TYPE_F64:
    return "f64";
  case NY_MONO_TYPE_LIST:
    return "list";
  case NY_MONO_TYPE_F64_LIST:
    return "list";
  default:
    return NULL;
  }
}

static bool ny_mono_type_is_raw_scalar(uint8_t kind) {
  return kind == NY_MONO_TYPE_INT || kind == NY_MONO_TYPE_F64;
}

static char ny_mono_type_suffix(uint8_t kind) {
  switch ((ny_mono_type_kind_t)kind) {
  case NY_MONO_TYPE_INT:
    return 'i';
  case NY_MONO_TYPE_F64:
    return 'd';
  case NY_MONO_TYPE_LIST:
    return 'l';
  case NY_MONO_TYPE_F64_LIST:
    return 'q';
  default:
    return 'x';
  }
}

static ny_mono_type_kind_t ny_mono_expr_kind(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *expr) {
  if (!expr)
    return NY_MONO_TYPE_NONE;
  if (ny_is_proven_int(cg, scopes, depth, expr, NULL))
    return NY_MONO_TYPE_INT;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_FLOAT)
    return NY_MONO_TYPE_F64;
  const char *ty = infer_expr_type(cg, scopes, depth, expr);
  if (ny_type_is(ty, "f64"))
    return NY_MONO_TYPE_F64;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t name_len = (size_t)expr->tok.len;
    if (name_len == 0)
      name_len = strlen(expr->as.ident.name);
    binding *b = ny_gencall_lookup_binding(
        cg, scopes, depth, expr->as.ident.name, name_len, expr->as.ident.hash);
    if (b && b->is_f64_list_storage)
      return NY_MONO_TYPE_F64_LIST;
  }
  if (ny_type_is(ty, "list") ||
      ny_expr_is_direct_list_storage(cg, scopes, depth, expr))
    return NY_MONO_TYPE_LIST;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t name_len = (size_t)expr->tok.len;
    if (name_len == 0)
      name_len = strlen(expr->as.ident.name);
    binding *b = ny_gencall_lookup_binding(
        cg, scopes, depth, expr->as.ident.name, name_len, expr->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct))
      return NY_MONO_TYPE_F64;
  }
  return NY_MONO_TYPE_NONE;
}

static size_t ny_mono_expr_cost(expr_t *e);

static size_t ny_mono_stmt_cost(stmt_t *s) {
  if (!s)
    return 0;
  size_t cost = 1;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      cost += ny_mono_stmt_cost(s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      cost += ny_mono_expr_cost(s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    cost += ny_mono_expr_cost(s->as.expr.expr);
    break;
  case NY_S_IF:
    cost += ny_mono_expr_cost(s->as.iff.test);
    cost += ny_mono_stmt_cost(s->as.iff.init);
    cost += ny_mono_stmt_cost(s->as.iff.conseq);
    cost += ny_mono_stmt_cost(s->as.iff.alt);
    break;
  case NY_S_GUARD:
    cost += ny_mono_expr_cost(s->as.guard.value);
    cost += ny_mono_stmt_cost(s->as.guard.fallback);
    break;
  case NY_S_WHILE:
    cost += ny_mono_expr_cost(s->as.whl.test);
    cost += ny_mono_stmt_cost(s->as.whl.init);
    cost += ny_mono_stmt_cost(s->as.whl.body);
    cost += ny_mono_stmt_cost(s->as.whl.update);
    break;
  case NY_S_FOR:
    cost += ny_mono_stmt_cost(s->as.fr.init);
    cost += ny_mono_expr_cost(s->as.fr.cond);
    cost += ny_mono_expr_cost(s->as.fr.iterable);
    cost += ny_mono_stmt_cost(s->as.fr.body);
    cost += ny_mono_stmt_cost(s->as.fr.update);
    break;
  case NY_S_MATCH:
    cost += ny_mono_expr_cost(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      for (size_t j = 0; j < s->as.match.arms.data[i].patterns.len; j++)
        cost += ny_mono_expr_cost(s->as.match.arms.data[i].patterns.data[j]);
      cost += ny_mono_expr_cost(s->as.match.arms.data[i].guard);
      cost += ny_mono_stmt_cost(s->as.match.arms.data[i].conseq);
    }
    cost += ny_mono_stmt_cost(s->as.match.default_conseq);
    break;
  case NY_S_RETURN:
    cost += ny_mono_expr_cost(s->as.ret.value);
    break;
  default:
    break;
  }
  return cost;
}

static size_t ny_mono_expr_cost(expr_t *e) {
  if (!e)
    return 0;
  size_t cost = 1;
  switch (e->kind) {
  case NY_E_UNARY:
    cost += ny_mono_expr_cost(e->as.unary.right);
    break;
  case NY_E_BINARY:
    cost += ny_mono_expr_cost(e->as.binary.left) +
            ny_mono_expr_cost(e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    cost += ny_mono_expr_cost(e->as.logical.left) +
            ny_mono_expr_cost(e->as.logical.right);
    break;
  case NY_E_TERNARY:
    cost += ny_mono_expr_cost(e->as.ternary.cond) +
            ny_mono_expr_cost(e->as.ternary.true_expr) +
            ny_mono_expr_cost(e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    cost += ny_mono_expr_cost(e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; i++)
      cost += ny_mono_expr_cost(e->as.call.args.data[i].val);
    break;
  case NY_E_MEMCALL:
    cost += ny_mono_expr_cost(e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; i++)
      cost += ny_mono_expr_cost(e->as.memcall.args.data[i].val);
    break;
  case NY_E_INDEX:
    cost += ny_mono_expr_cost(e->as.index.target) +
            ny_mono_expr_cost(e->as.index.start) +
            ny_mono_expr_cost(e->as.index.stop) +
            ny_mono_expr_cost(e->as.index.step);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++)
      cost += ny_mono_expr_cost(e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      cost += ny_mono_expr_cost(e->as.dict.pairs.data[i].key);
      cost += ny_mono_expr_cost(e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        cost += ny_mono_expr_cost(e->as.fstring.parts.data[i].as.e);
    break;
  case NY_E_MATCH:
    cost += ny_mono_stmt_cost((stmt_t *)e->as.match.default_conseq);
    break;
  case NY_E_MEMBER:
    cost += ny_mono_expr_cost(e->as.member.target);
    break;
  case NY_E_PTR_TYPE:
    cost += ny_mono_expr_cost(e->as.ptr_type.target);
    break;
  case NY_E_DEREF:
    cost += ny_mono_expr_cost(e->as.deref.target);
    break;
  case NY_E_SIZEOF:
    cost += ny_mono_expr_cost(e->as.szof.target);
    break;
  case NY_E_TRY:
    cost += ny_mono_expr_cost(e->as.try_expr.target);
    break;
  default:
    break;
  }
  return cost;
}

static bool ny_mono_expr_refs_self(expr_t *e, const char *base_name,
                                   const char *tail_name);

static bool ny_mono_stmt_refs_self(stmt_t *s, const char *base_name,
                                   const char *tail_name) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      if (ny_mono_stmt_refs_self(s->as.block.body.data[i], base_name,
                                 tail_name))
        return true;
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      if (ny_mono_expr_refs_self(s->as.var.exprs.data[i], base_name, tail_name))
        return true;
    return false;
  case NY_S_EXPR:
    return ny_mono_expr_refs_self(s->as.expr.expr, base_name, tail_name);
  case NY_S_IF:
    return ny_mono_expr_refs_self(s->as.iff.test, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.iff.init, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.iff.conseq, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.iff.alt, base_name, tail_name);
  case NY_S_WHILE:
    return ny_mono_expr_refs_self(s->as.whl.test, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.whl.init, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.whl.body, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.whl.update, base_name, tail_name);
  case NY_S_FOR:
    return ny_mono_stmt_refs_self(s->as.fr.init, base_name, tail_name) ||
           ny_mono_expr_refs_self(s->as.fr.cond, base_name, tail_name) ||
           ny_mono_expr_refs_self(s->as.fr.iterable, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.fr.body, base_name, tail_name) ||
           ny_mono_stmt_refs_self(s->as.fr.update, base_name, tail_name);
  case NY_S_RETURN:
    return ny_mono_expr_refs_self(s->as.ret.value, base_name, tail_name);
  default:
    return false;
  }
}

static bool ny_mono_expr_refs_self(expr_t *e, const char *base_name,
                                   const char *tail_name) {
  if (!e)
    return false;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *n = e->as.call.callee->as.ident.name;
    if (n && ((base_name && strcmp(n, base_name) == 0) ||
              (tail_name && strcmp(n, tail_name) == 0)))
      return true;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    return ny_mono_expr_refs_self(e->as.unary.right, base_name, tail_name);
  case NY_E_BINARY:
    return ny_mono_expr_refs_self(e->as.binary.left, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.binary.right, base_name, tail_name);
  case NY_E_LOGICAL:
    return ny_mono_expr_refs_self(e->as.logical.left, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.logical.right, base_name, tail_name);
  case NY_E_TERNARY:
    return ny_mono_expr_refs_self(e->as.ternary.cond, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.ternary.true_expr, base_name,
                                  tail_name) ||
           ny_mono_expr_refs_self(e->as.ternary.false_expr, base_name,
                                  tail_name);
  case NY_E_CALL:
    if (ny_mono_expr_refs_self(e->as.call.callee, base_name, tail_name))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; i++)
      if (ny_mono_expr_refs_self(e->as.call.args.data[i].val, base_name,
                                 tail_name))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (ny_mono_expr_refs_self(e->as.memcall.target, base_name, tail_name))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; i++)
      if (ny_mono_expr_refs_self(e->as.memcall.args.data[i].val, base_name,
                                 tail_name))
        return true;
    return false;
  case NY_E_INDEX:
    return ny_mono_expr_refs_self(e->as.index.target, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.index.start, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.index.stop, base_name, tail_name) ||
           ny_mono_expr_refs_self(e->as.index.step, base_name, tail_name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++)
      if (ny_mono_expr_refs_self(e->as.list_like.data[i], base_name, tail_name))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++)
      if (ny_mono_expr_refs_self(e->as.dict.pairs.data[i].key, base_name,
                                 tail_name) ||
          ny_mono_expr_refs_self(e->as.dict.pairs.data[i].value, base_name,
                                 tail_name))
        return true;
    return false;
  case NY_E_MEMBER:
    return ny_mono_expr_refs_self(e->as.member.target, base_name, tail_name);
  default:
    return false;
  }
}

static bool ny_mono_stmt_has_unsupported(stmt_t *s) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_TRY:
  case NY_S_DEFER:
  case NY_S_GOTO:
  case NY_S_LABEL:
  case NY_S_FUNC:
  case NY_S_EXTERN:
  case NY_S_MODULE:
  case NY_S_MACRO:
  case NY_S_INCLUDE:
    return true;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      if (ny_mono_stmt_has_unsupported(s->as.block.body.data[i]))
        return true;
    return false;
  case NY_S_IF:
    return ny_mono_stmt_has_unsupported(s->as.iff.init) ||
           ny_mono_stmt_has_unsupported(s->as.iff.conseq) ||
           ny_mono_stmt_has_unsupported(s->as.iff.alt);
  case NY_S_WHILE:
    if (!ny_env_enabled("NYTRIX_MONO_IMPERATIVE"))
      return true;
    return ny_mono_stmt_has_unsupported(s->as.whl.init) ||
           ny_mono_stmt_has_unsupported(s->as.whl.body) ||
           ny_mono_stmt_has_unsupported(s->as.whl.update);
  case NY_S_FOR:
    if (!ny_env_enabled("NYTRIX_MONO_IMPERATIVE"))
      return true;
    return ny_mono_stmt_has_unsupported(s->as.fr.init) ||
           ny_mono_stmt_has_unsupported(s->as.fr.body) ||
           ny_mono_stmt_has_unsupported(s->as.fr.update);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      if (ny_mono_stmt_has_unsupported(s->as.match.arms.data[i].conseq))
        return true;
    return ny_mono_stmt_has_unsupported(s->as.match.default_conseq);
  default:
    return false;
  }
}

static uint64_t ny_mono_key_hash(const char *base_name, const uint8_t *types,
                                 int arity) {
  uint64_t h = ny_hash64_cstr(base_name ? base_name : "");
  h = ny_hash64_u64(h, (uint64_t)(uint32_t)arity);
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++)
    h = ny_hash64_u64(h, (uint64_t)types[i] + ((uint64_t)i << 8));
  return h;
}

static bool ny_mono_types_equal(const uint8_t *a, const uint8_t *b, int arity) {
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++)
    if (a[i] != b[i])
      return false;
  return true;
}

static uint64_t ny_mono_key_hash_with_list_lens(
    uint64_t h, const bool *list_len_known, const int64_t *list_len_raw,
    int arity) {
  bool any_known = false;
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++) {
    if (list_len_known && list_len_known[i]) {
      any_known = true;
      break;
    }
  }
  if (!any_known)
    return h;
  h = ny_hash64_u64(h, UINT64_C(0x4e594d4f4e4f4c4c));
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++) {
    h = ny_hash64_u64(h, (uint64_t)(list_len_known && list_len_known[i]));
    if (list_len_known && list_len_known[i] && list_len_raw)
      h = ny_hash64_u64(h, (uint64_t)list_len_raw[i]);
  }
  return h;
}

static bool ny_mono_list_lens_equal(const ny_mono_specialization_t *spec,
                                    const bool *list_len_known,
                                    const int64_t *list_len_raw, int arity) {
  if (!spec)
    return false;
  for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++) {
    bool known = list_len_known && list_len_known[i];
    if (spec->arg_list_len_min_known[i] != known)
      return false;
    if (known && list_len_raw &&
        spec->arg_list_len_min_raw[i] != list_len_raw[i])
      return false;
  }
  return true;
}

static fun_sig *ny_mono_lookup_existing(codegen_t *cg, stmt_t *base_stmt,
                                        uint64_t key_hash, const uint8_t *types,
                                        const bool *list_len_known,
                                        const int64_t *list_len_raw,
                                        int arity) {
  if (!cg || !base_stmt)
    return NULL;
  for (size_t i = 0; i < cg->mono_specs.len; i++) {
    ny_mono_specialization_t *spec = &cg->mono_specs.data[i];
    if (spec->base_stmt == base_stmt && spec->key_hash == key_hash &&
        spec->arity == arity &&
        ny_mono_types_equal(spec->types, types, arity) &&
        ny_mono_list_lens_equal(spec, list_len_known, list_len_raw, arity)) {
      return lookup_fun_exact(cg, spec->specialized_name);
    }
  }
  return NULL;
}

static size_t ny_mono_count_for_base(codegen_t *cg, stmt_t *base_stmt) {
  size_t n = 0;
  if (!cg || !base_stmt)
    return 0;
  for (size_t i = 0; i < cg->mono_specs.len; i++)
    if (cg->mono_specs.data[i].base_stmt == base_stmt)
      n++;
  return n;
}

static const char *ny_mono_make_name(codegen_t *cg, const char *base_name,
                                     const uint8_t *types, int arity,
                                     uint64_t key_hash) {
  if (!cg || !base_name)
    return NULL;
  char type_buf[NY_MONO_MAX_ARITY + 1];
  int n = arity < NY_MONO_MAX_ARITY ? arity : NY_MONO_MAX_ARITY;
  for (int i = 0; i < n; i++)
    type_buf[i] = ny_mono_type_suffix(types[i]);
  type_buf[n] = '\0';
  char hash_buf[32];
  snprintf(hash_buf, sizeof(hash_buf), "%08llx",
           (unsigned long long)(key_hash & 0xffffffffu));
  size_t len = strlen(base_name) + strlen("__ny_mono_") + strlen(type_buf) + 1 +
               strlen(hash_buf) + 1;
  char *out = arena_alloc(cg->arena, len);
  snprintf(out, len, "%s__ny_mono_%s_%s", base_name, type_buf, hash_buf);
  return out;
}

static ny_mono_type_kind_t ny_mono_static_expr_kind(expr_t *e,
                                                    const char **names,
                                                    const uint8_t *types,
                                                    int arity) {
  if (!e)
    return NY_MONO_TYPE_NONE;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT)
      return NY_MONO_TYPE_INT;
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return NY_MONO_TYPE_F64;
    return NY_MONO_TYPE_NONE;
  case NY_E_IDENT:
    if (!e->as.ident.name)
      return NY_MONO_TYPE_NONE;
    for (int i = 0; i < arity && i < NY_MONO_MAX_ARITY; i++)
      if (names[i] && strcmp(names[i], e->as.ident.name) == 0)
        return (ny_mono_type_kind_t)types[i];
    return NY_MONO_TYPE_NONE;
  case NY_E_UNARY:
    return ny_mono_static_expr_kind(e->as.unary.right, names, types, arity);
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    if (!op)
      return NY_MONO_TYPE_NONE;
    bool arith = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                 strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                 strcmp(op, "%") == 0 || strcmp(op, "^") == 0;
    bool bit = strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
               strcmp(op, "^^") == 0 || strcmp(op, "<<") == 0 ||
               strcmp(op, ">>") == 0;
    if (!arith && !bit)
      return NY_MONO_TYPE_NONE;
    ny_mono_type_kind_t l =
        ny_mono_static_expr_kind(e->as.binary.left, names, types, arity);
    ny_mono_type_kind_t r =
        ny_mono_static_expr_kind(e->as.binary.right, names, types, arity);
    if (l == NY_MONO_TYPE_NONE || r == NY_MONO_TYPE_NONE)
      return NY_MONO_TYPE_NONE;
    if (l == NY_MONO_TYPE_F64 || r == NY_MONO_TYPE_F64)
      return arith ? NY_MONO_TYPE_F64 : NY_MONO_TYPE_NONE;
    return NY_MONO_TYPE_INT;
  }
  default:
    return NY_MONO_TYPE_NONE;
  }
}

static bool ny_mono_return_kind_walk(stmt_t *s, const char **names,
                                     const uint8_t *types, int arity,
                                     ny_mono_type_kind_t *out,
                                     bool *saw_return) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      if (!ny_mono_return_kind_walk(s->as.block.body.data[i], names, types,
                                    arity, out, saw_return))
        return false;
    return true;
  case NY_S_IF:
    return ny_mono_return_kind_walk(s->as.iff.conseq, names, types, arity, out,
                                    saw_return) &&
           ny_mono_return_kind_walk(s->as.iff.alt, names, types, arity, out,
                                    saw_return);
  case NY_S_WHILE:
    return ny_mono_return_kind_walk(s->as.whl.body, names, types, arity, out,
                                    saw_return) &&
           ny_mono_return_kind_walk(s->as.whl.update, names, types, arity, out,
                                    saw_return);
  case NY_S_FOR:
    return ny_mono_return_kind_walk(s->as.fr.body, names, types, arity, out,
                                    saw_return) &&
           ny_mono_return_kind_walk(s->as.fr.update, names, types, arity, out,
                                    saw_return);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      if (!ny_mono_return_kind_walk(s->as.match.arms.data[i].conseq, names,
                                    types, arity, out, saw_return))
        return false;
    return ny_mono_return_kind_walk(s->as.match.default_conseq, names, types,
                                    arity, out, saw_return);
  case NY_S_RETURN: {
    ny_mono_type_kind_t k =
        ny_mono_static_expr_kind(s->as.ret.value, names, types, arity);
    if (k == NY_MONO_TYPE_NONE)
      return false;
    if (!*saw_return) {
      *out = k;
      *saw_return = true;
      return true;
    }
    if (*out == k)
      return true;
    if ((*out == NY_MONO_TYPE_INT && k == NY_MONO_TYPE_F64) ||
        (*out == NY_MONO_TYPE_F64 && k == NY_MONO_TYPE_INT)) {
      *out = NY_MONO_TYPE_F64;
      return true;
    }
    return false;
  }
  case NY_S_EXPR: {
    ny_mono_type_kind_t k =
        ny_mono_static_expr_kind(s->as.expr.expr, names, types, arity);
    if (k == NY_MONO_TYPE_NONE)
      return false;
    if (!*saw_return) {
      *out = k;
      *saw_return = true;
      return true;
    }
    if (*out == k)
      return true;
    if ((*out == NY_MONO_TYPE_INT && k == NY_MONO_TYPE_F64) ||
        (*out == NY_MONO_TYPE_F64 && k == NY_MONO_TYPE_INT)) {
      *out = NY_MONO_TYPE_F64;
      return true;
    }
    return false;
  }
  default:
    return true;
  }
}

static bool ny_mono_is_single_return_shape(stmt_t *s) {
  if (!s)
    return false;
  if (s->kind == NY_S_RETURN || s->kind == NY_S_EXPR)
    return true;
  if (s->kind == NY_S_BLOCK && s->as.block.body.len == 1)
    return ny_mono_is_single_return_shape(s->as.block.body.data[0]);
  return false;
}

static ny_mono_type_kind_t
ny_mono_infer_return_kind(stmt_t *clone, const uint8_t *types, int arity) {
  if (!clone || clone->as.fn.return_type || arity <= 0)
    return NY_MONO_TYPE_NONE;
  if (!ny_mono_is_single_return_shape(clone->as.fn.body))
    return NY_MONO_TYPE_NONE;
  const char *names[NY_MONO_MAX_ARITY] = {0};
  for (int i = 0;
       i < arity && i < NY_MONO_MAX_ARITY && i < (int)clone->as.fn.params.len;
       i++)
    names[i] = clone->as.fn.params.data[i].name;
  ny_mono_type_kind_t ret = NY_MONO_TYPE_NONE;
  bool saw_return = false;
  if (!ny_mono_return_kind_walk(clone->as.fn.body, names, types, arity, &ret,
                                &saw_return) ||
      !saw_return)
    return NY_MONO_TYPE_NONE;
  return ret;
}

static const char *ny_mono_infer_return_type(stmt_t *clone,
                                             const uint8_t *types, int arity) {
  if (!clone || clone->as.fn.return_type || arity <= 0)
    return clone ? clone->as.fn.return_type : NULL;
  ny_mono_type_kind_t ret = ny_mono_infer_return_kind(clone, types, arity);
  if (ret == NY_MONO_TYPE_NONE)
    return NULL;
  return NULL;
}

static stmt_t *ny_mono_clone_func_stmt(codegen_t *cg, fun_sig *base_sig,
                                       const char *mono_name,
                                       const uint8_t *types, int arity) {
  if (!cg || !base_sig || !base_sig->stmt_t ||
      base_sig->stmt_t->kind != NY_S_FUNC)
    return NULL;
  stmt_t *base = base_sig->stmt_t;
  stmt_t *clone = arena_alloc(cg->arena, sizeof(*clone));
  if (!clone)
    return NULL;
  *clone = *base;
  clone->as.fn = base->as.fn;
  clone->as.fn.name = mono_name ? mono_name : base->as.fn.name;
  clone->as.fn.params = (ny_param_list){0};
  for (size_t i = 0; i < base->as.fn.params.len; i++) {
    param_t p = base->as.fn.params.data[i];
    if (!p.type && i < (size_t)arity) {
      const char *mono_type = ny_mono_type_name(types[i]);
      if (mono_type)
        p.type = mono_type;
    }
    vec_push_arena(cg->arena, &clone->as.fn.params, p);
  }
  if (!clone->as.fn.return_type)
    clone->as.fn.return_type = ny_mono_infer_return_type(clone, types, arity);
  sema_func_t *sema = arena_alloc(cg->arena, sizeof(*sema));
  if (!sema)
    return NULL;
  memset(sema, 0, sizeof(*sema));
  sema->resolved_return_type =
      clone->as.fn.return_type
          ? resolve_abi_type_name(cg, clone->as.fn.return_type, clone->tok)
          : cg->type_i64;
  for (size_t i = 0; i < clone->as.fn.params.len; i++) {
    const char *ptype = clone->as.fn.params.data[i].type;
    LLVMTypeRef pty =
        ptype ? resolve_abi_type_name(cg, ptype, clone->tok) : cg->type_i64;
    vec_push_arena(cg->arena, &sema->resolved_param_types, pty);
    if (i < NY_MONO_MAX_ARITY)
      sema->mono_param_kinds[i] = (i < (size_t)arity) ? types[i] : 0;
  }
  if (base->sema_kind == NY_STMT_SEMA_FUNC && base->sema) {
    sema_func_t *base_sema = (sema_func_t *)base->sema;
    sema->is_pure = base_sema->is_pure;
    sema->purity_known = base_sema->purity_known;
    sema->is_memo_safe = base_sema->is_memo_safe;
    sema->memo_known = base_sema->memo_known;
    sema->effects = base_sema->effects;
    sema->effects_known = base_sema->effects_known;
    sema->args_escape = base_sema->args_escape;
    sema->args_mutated = base_sema->args_mutated;
    sema->returns_alias = base_sema->returns_alias;
    sema->escape_known = base_sema->escape_known;
    sema->is_recursive = base_sema->is_recursive;
  }
  clone->sema = sema;
  clone->sema_kind = NY_STMT_SEMA_FUNC;
  return clone;
}

static expr_t *ny_mono_call_arg_for_param(expr_call_t *c, expr_memcall_t *mc,
                                          bool skip_target,
                                          size_t param_idx) {
  call_arg_t *user_args = c ? c->args.data : (mc ? mc->args.data : NULL);
  size_t user_args_len = c ? c->args.len : (mc ? mc->args.len : 0);
  if (mc && !skip_target && param_idx == 0)
    return mc->target;
  size_t user_idx = (mc && !skip_target) ? param_idx - 1u : param_idx;
  return user_idx < user_args_len ? user_args[user_idx].val : NULL;
}

static bool ny_mono_param_can_carry_list_len(stmt_t *fn, int idx,
                                             uint8_t kind) {
  if (kind == NY_MONO_TYPE_LIST || kind == NY_MONO_TYPE_F64_LIST)
    return true;
  if (!fn || idx < 0 || (size_t)idx >= fn->as.fn.params.len)
    return false;
  const char *ptype = fn->as.fn.params.data[idx].type;
  return ptype && ny_type_is(ptype, "list");
}

static fun_sig *ny_try_monomorphize_call(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *call,
                                         fun_sig *sig, expr_call_t *c,
                                         expr_memcall_t *mc, bool skip_target,
                                         size_t call_argc) {
  if (!cg || !sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
      !sig->stmt_t->as.fn.body)
    return NULL;
  if (cg->mono_emitting || !ny_mono_enabled(cg))
    return NULL;
  if (sig->is_extern || sig->is_variadic || sig->arity <= 0 ||
      sig->arity > NY_MONO_MAX_ARITY)
    return NULL;
  if (call_argc != (size_t)sig->arity)
    return NULL;
  stmt_t *fn = sig->stmt_t;
  if (fn->as.fn.attr_thread || fn->as.fn.attr_naked || fn->as.fn.attr_cache ||
      fn->as.fn.attr_consteval)
    return NULL;
  if (sig->is_recursive)
    return NULL;
  if (ny_is_stdlib_tok(fn->tok) && !ny_env_enabled("NYTRIX_MONO_STDLIB"))
    return NULL;
  if (ny_mono_stmt_has_unsupported(fn->as.fn.body))
    return NULL;
  const char *tail = sig->name ? strrchr(sig->name, '.') : NULL;
  tail = tail ? tail + 1 : sig->name;
  if (ny_mono_stmt_refs_self(fn->as.fn.body, sig->name, tail))
    return NULL;

  size_t body_cost = ny_mono_stmt_cost(fn->as.fn.body);
  int max_cost =
      ny_mono_env_int("NYTRIX_MONO_MAX_COST", fn->as.fn.attr_hot ? 768 : 256);
  if (max_cost > 0 && body_cost > (size_t)max_cost)
    return NULL;

  uint8_t types[NY_MONO_MAX_ARITY] = {0};
  bool arg_list_len_min_known[NY_MONO_MAX_ARITY] = {0};
  int64_t arg_list_len_min_raw[NY_MONO_MAX_ARITY] = {0};
  bool useful = false;
  bool has_keyword = false;
  call_arg_t *user_args = c ? c->args.data : (mc ? mc->args.data : NULL);
  size_t user_args_len = c ? c->args.len : (mc ? mc->args.len : 0);
  for (size_t i = 0; i < user_args_len; i++) {
    if (user_args[i].name) {
      has_keyword = true;
      break;
    }
  }
  if (has_keyword)
    return NULL;
  for (int i = 0; i < sig->arity && i < NY_MONO_MAX_ARITY; i++) {
    expr_t *arg_expr = ny_mono_call_arg_for_param(c, mc, skip_target, (size_t)i);
    if (!fn->as.fn.params.data[i].type) {
      ny_mono_type_kind_t kind =
          ny_mono_expr_kind(cg, scopes, depth, arg_expr);
      if (kind != NY_MONO_TYPE_NONE) {
        types[i] = (uint8_t)kind;
        useful = true;
      }
    }
    int64_t len_min = 0;
    if (ny_mono_param_can_carry_list_len(fn, i, types[i]) &&
        ny_gencall_list_len_min(cg, scopes, depth, arg_expr, &len_min)) {
      arg_list_len_min_known[i] = true;
      arg_list_len_min_raw[i] = len_min;
    }
  }
  if (!useful)
    return NULL;
  bool list_args_only =
      ny_env_enabled("NYTRIX_MONO_LIST_ARGS") &&
      !ny_codegen_speed_profile_enabled(cg) &&
      !ny_env_enabled("NYTRIX_MONO_TYPES") &&
      !ny_env_enabled("NYTRIX_ENABLE_MONOMORPHIZATION");
  if (list_args_only) {
    bool has_list_arg = false;
    for (int i = 0; i < sig->arity && i < NY_MONO_MAX_ARITY; i++) {
      if (types[i] == NY_MONO_TYPE_LIST || types[i] == NY_MONO_TYPE_F64_LIST) {
        has_list_arg = true;
        break;
      }
    }
    if (!has_list_arg)
      return NULL;
  }

  int max_global = ny_mono_env_int("NYTRIX_MONO_MAX_GLOBAL", 128);
  int max_per_fn = ny_mono_env_int("NYTRIX_MONO_MAX_PER_FN", 4);
  uint64_t key_hash = ny_mono_key_hash(sig->name, types, sig->arity);
  key_hash = ny_mono_key_hash_with_list_lens(
      key_hash, arg_list_len_min_known, arg_list_len_min_raw, sig->arity);
  fun_sig *existing =
      ny_mono_lookup_existing(cg, fn, key_hash, types,
                              arg_list_len_min_known, arg_list_len_min_raw,
                              sig->arity);
  if (existing)
    return existing;
  if (max_global > 0 && cg->mono_specs.len >= (size_t)max_global)
    return NULL;
  if (max_per_fn > 0 && ny_mono_count_for_base(cg, fn) >= (size_t)max_per_fn)
    return NULL;

  const char *mono_name =
      ny_mono_make_name(cg, sig->name, types, sig->arity, key_hash);
  if (!mono_name)
    return NULL;
  ny_mono_type_kind_t return_kind =
      ny_mono_infer_return_kind(fn, types, sig->arity);
  stmt_t *clone =
      ny_mono_clone_func_stmt(cg, sig, mono_name, types, sig->arity);
  if (!clone)
    return NULL;
  if (clone->sema_kind == NY_STMT_SEMA_FUNC && clone->sema) {
    sema_func_t *clone_sema = (sema_func_t *)clone->sema;
    for (int i = 0; i < sig->arity && i < NY_MONO_MAX_ARITY; i++) {
      if (!arg_list_len_min_known[i])
        continue;
      clone_sema->mono_param_list_len_min_known[i] = true;
      clone_sema->mono_param_list_len_min_raw[i] = arg_list_len_min_raw[i];
    }
  }
  ny_mono_specialization_t spec = {0};
  spec.base_name = sig->name;
  spec.specialized_name = mono_name;
  spec.base_stmt = fn;
  spec.specialized_stmt = clone;
  spec.key_hash = key_hash;
  spec.body_cost = body_cost;
  spec.arity = sig->arity;
  spec.return_kind = (uint8_t)return_kind;
  spec.raw_return_proven = ny_mono_type_is_raw_scalar(spec.return_kind);
  spec.raw_return_active = false;
  spec.inline_body_eligible =
      spec.raw_return_proven && ny_mono_is_single_return_shape(fn->as.fn.body);
  spec.accept_reason =
      spec.raw_return_proven
          ? "arg-specialized+raw-return-proof"
          : (body_cost <= (size_t)ny_mono_env_int(
                              "NYTRIX_MONO_ALWAYSINLINE_COST", 96)
                 ? "arg-specialized+inline-candidate"
                 : "arg-specialized");
  spec.return_policy =
      spec.raw_return_active
          ? "raw-return-active"
          : (spec.raw_return_proven ? "raw-return-proof-inactive"
                                    : "tagged-return");
  memcpy(spec.types, types, sizeof(spec.types));
  memcpy(spec.arg_list_len_min_known, arg_list_len_min_known,
         sizeof(spec.arg_list_len_min_known));
  memcpy(spec.arg_list_len_min_raw, arg_list_len_min_raw,
         sizeof(spec.arg_list_len_min_raw));
  vec_push(&cg->mono_specs, spec);

  bool old_emitting = cg->mono_emitting;
  cg->mono_emitting = true;
  scope mono_scopes[64] = {0};
  (void)scopes;
  gen_func(cg, clone, mono_name, mono_scopes, 0, NULL);
  cg->mono_emitting = old_emitting;
  fun_sig *mono_sig = lookup_fun_exact(cg, mono_name);
  if (mono_sig) {
    mono_sig->is_pure = sig->is_pure;
    mono_sig->is_memo_safe = sig->is_memo_safe;
    mono_sig->is_stable = sig->is_stable;
    mono_sig->effects = sig->effects;
    mono_sig->args_escape = sig->args_escape;
    mono_sig->args_mutated = sig->args_mutated;
    mono_sig->returns_alias = sig->returns_alias;
    mono_sig->effects_known = sig->effects_known;
    mono_sig->is_recursive = false;
    mono_sig->is_attached_method = sig->is_attached_method;
    if (mono_sig->value) {
      LLVMSetLinkage(mono_sig->value, LLVMInternalLinkage);
      if (!fn->as.fn.attr_noinline) {
        int inline_cost = ny_mono_env_int("NYTRIX_MONO_ALWAYSINLINE_COST", 96);
        if (inline_cost <= 0 || body_cost <= (size_t)inline_cost ||
            ny_mono_is_single_return_shape(fn->as.fn.body)) {
          add_fn_enum_attr(cg, mono_sig->value, "alwaysinline", 0);
          add_fn_enum_attr(cg, mono_sig->value, "hot", 0);
        }
      }
    }
  }
  if (mono_sig && ny_mono_trace_enabled()) {
    fprintf(stderr, "[mono] %s -> %s (", sig->name ? sig->name : "<anon>",
            mono_name);
    for (int i = 0; i < sig->arity; i++) {
      if (i)
        fputc(',', stderr);
      const char *tn = ny_mono_type_name(types[i]);
      fputs(tn ? tn : "_", stderr);
    }
    fprintf(stderr, ") at %s:%d\n",
            call && call->tok.filename ? call->tok.filename : "<unknown>",
            call ? call->tok.line : 0);
  }
  return mono_sig;
}
