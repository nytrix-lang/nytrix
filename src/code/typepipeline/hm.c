static char *hm_arena_strdup(ny_hm_state_t *hm, const char *s) {
  if (!s)
    s = "";
  return arena_strndup(hm ? &hm->arena : NULL, s, strlen(s));
}

static ny_hm_type_t *hm_type_new(ny_hm_state_t *hm, ny_hm_kind_t kind) {
  ny_hm_type_t *t =
      (ny_hm_type_t *)arena_alloc(hm ? &hm->arena : NULL, sizeof(*t));
  if (!t)
    return NULL;
  t->kind = kind;
  vec_init(&t->args);
  if (hm)
    hm->type_nodes++;
  return t;
}

static ny_hm_type_t *hm_any(ny_hm_state_t *hm) {
  return hm_type_new(hm, NY_HM_ANY);
}

static ny_hm_type_t *hm_name(ny_hm_state_t *hm, const char *name) {
  ny_hm_type_t *t = hm_type_new(hm, NY_HM_NAME);
  t->name = hm_arena_strdup(hm, name && *name ? name : "any");
  return t;
}

static ny_hm_type_t *hm_var(ny_hm_state_t *hm) {
  ny_hm_type_t *t = hm_type_new(hm, NY_HM_VAR);
  t->id = hm ? hm->next_var++ : 0;
  return t;
}

static ny_hm_type_t *hm_unary(ny_hm_state_t *hm, ny_hm_kind_t kind,
                              ny_hm_type_t *a) {
  ny_hm_type_t *t = hm_type_new(hm, kind);
  t->a = a;
  return t;
}

static ny_hm_type_t *hm_binary(ny_hm_state_t *hm, ny_hm_kind_t kind,
                               ny_hm_type_t *a, ny_hm_type_t *b) {
  ny_hm_type_t *t = hm_type_new(hm, kind);
  t->a = a;
  t->b = b;
  return t;
}

static ny_hm_type_t *hm_fn_type(ny_hm_state_t *hm) {
  return hm_type_new(hm, NY_HM_FN);
}

static void hm_args_push(ny_hm_state_t *hm, ny_hm_type_t *type,
                         ny_hm_type_t *arg) {
  if (!type)
    return;
  if (hm)
    vec_push_arena(&hm->arena, &type->args, arg);
  else
    vec_push(&type->args, arg);
}

static ny_hm_type_t *hm_prune(ny_hm_type_t *t) {
  if (!t)
    return NULL;
  if (t->kind == NY_HM_VAR && t->instance) {
    t->instance = hm_prune(t->instance);
    return t->instance;
  }
  return t;
}

static bool hm_is_any(ny_hm_type_t *t) {
  t = hm_prune(t);
  return !t || t->kind == NY_HM_ANY ||
         (t->kind == NY_HM_NAME && t->name && strcmp(t->name, "any") == 0);
}

static bool hm_is_name(ny_hm_type_t *t, const char *name) {
  t = hm_prune(t);
  return t && t->kind == NY_HM_NAME && t->name && name &&
         strcmp(t->name, name) == 0;
}

static bool hm_is_callable_name(const char *name) {
  return name && (strcmp(name, "fnptr") == 0 || strcmp(name, "function") == 0 ||
                  strcmp(name, "callable") == 0);
}

static bool hm_strict_types_enabled(ny_hm_state_t *hm, token_t tok) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !hm->ctx->cg->strict_types)
    return false;
  if (!ny_is_stdlib_tok(tok))
    return true;
  return hm->ctx->source_name && tok.filename &&
         strcmp(hm->ctx->source_name, tok.filename) == 0;
}

static const char *hm_strict_code_for_fallback(const char *code) {
  if (!code)
    return NULL;
  if (strcmp(code, "hm-dynamic-member") == 0 ||
      strcmp(code, "hm-unknown-member") == 0)
    return "hm-strict-dynamic-member";
  if (strcmp(code, "hm-index-dynamic-result") == 0)
    return "hm-strict-dynamic-index";
  if (strcmp(code, "hm-unknown-call") == 0 ||
      strcmp(code, "hm-call-missing-callee") == 0)
    return "hm-strict-dynamic-call";
  return NULL;
}

static void hm_strict_diag(ny_hm_state_t *hm, token_t tok, const char *code,
                           const char *context, const char *expected,
                           const char *got, const char *expr_kind,
                           const char *message) {
  if (!hm_strict_types_enabled(hm, tok))
    return;
  tp_add_diag_ex(
      hm->ctx, tok, "hm", code ? code : "hm-strict-type",
      context ? context : "strict type validation",
      expected ? expected : "static type evidence", got ? got : "dynamic any",
      expr_kind ? expr_kind : "expr",
      "strict type checks reject dynamic fallbacks that hide performance or "
      "safety cliffs",
      "add a local annotation/converter, use a layout guard, refine the "
      "Result<T,E> payload, or run with --no-strict-types for intentional "
      "dynamic code",
      "%s", message ? message : "strict type validation failed");
}

static ny_hm_type_t *hm_any_fallback(ny_hm_state_t *hm, expr_t *e,
                                     const char *code, const char *fmt, ...) {
  if (hm && hm->ctx && fmt) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = tp_vformat(fmt, ap);
    va_end(ap);
    tp_add_fallback(hm->ctx, e ? e->tok : (token_t){0}, "hm",
                    code ? code : "hm-any-fallback",
                    e ? tp_expr_kind_name(e->kind) : "expr", "%s",
                    msg ? msg : "any fallback");
    const char *strict_code = hm_strict_code_for_fallback(code);
    if (strict_code && e) {
      hm_strict_diag(hm, e->tok, strict_code, code, "typed expression",
                     "dynamic any", e ? tp_expr_kind_name(e->kind) : "expr",
                     msg ? msg
                         : "dynamic type fallback rejected by strict mode");
    }
    free(msg);
  }
  return hm_any(hm);
}

static bool hm_occurs(ny_hm_type_t *needle, ny_hm_type_t *haystack) {
  needle = hm_prune(needle);
  haystack = hm_prune(haystack);
  if (!needle || !haystack)
    return false;
  if (needle == haystack)
    return true;
  if (haystack->kind == NY_HM_VAR)
    return false;
  if (hm_occurs(needle, haystack->a) || hm_occurs(needle, haystack->b))
    return true;
  for (size_t i = 0; i < haystack->args.len; ++i) {
    if (hm_occurs(needle, haystack->args.data[i]))
      return true;
  }
  return false;
}

static bool hm_numeric_name(const char *name) {
  return tp_is_int_type(name) || tp_is_float_type(name) ||
         (name && (strcmp(name, "number") == 0 ||
                    strcmp(name, "float") == 0 ||
                    strcmp(name, "integer") == 0));
}

static bool hm_integer_name(const char *name) {
  return tp_is_int_type(name) || (name && strcmp(name, "integer") == 0);
}

static bool hm_float_name(const char *name) {
  return tp_is_float_type(name) || (name && strcmp(name, "float") == 0);
}

static bool hm_type_is_unbound_var(ny_hm_type_t *t) {
  t = hm_prune(t);
  return t && t->kind == NY_HM_VAR;
}

static bool hm_type_known_numeric(ny_hm_type_t *t, const char **canonical_out) {
  t = hm_prune(t);
  if (!t || t->kind != NY_HM_NAME || !t->name)
    return false;
  if (tp_is_float_type(t->name) || strcmp(t->name, "float") == 0) {
    if (canonical_out)
      *canonical_out = tp_is_float_type(t->name) ? t->name : "f64";
    return true;
  }
  if (tp_is_int_type(t->name) || strcmp(t->name, "integer") == 0) {
    if (canonical_out)
      *canonical_out = tp_is_int_type(t->name) ? t->name : "int";
    return true;
  }
  if (strcmp(t->name, "number") == 0) {
    if (canonical_out)
      *canonical_out = "number";
    return true;
  }
  return false;
}

static bool hm_type_known_string(ny_hm_type_t *t) {
  t = hm_prune(t);
  return t && t->kind == NY_HM_NAME && t->name && strcmp(t->name, "str") == 0;
}

static bool hm_type_known_collection(ny_hm_type_t *t) {
  t = hm_prune(t);
  return t && (t->kind == NY_HM_LIST || t->kind == NY_HM_SET ||
               t->kind == NY_HM_TUPLE || t->kind == NY_HM_DICT);
}

static bool hm_name_compatible(const char *want, const char *got);

static bool hm_generic_base_same(const char *want, const char *got) {
  return strchr(want ? want : "", '<') && strchr(got ? got : "", '<') &&
         ny_generic_type_base_is(want, got);
}

static bool hm_generic_name_compatible(const char *want, const char *got);
static char *hm_merge_generic_name_owned(const char *want, const char *got);

static bool hm_generic_arg_is_dynamic(const char *name) {
  const char *leaf = ny_generic_type_leaf(name);
  if (!leaf || !*leaf)
    return true;
  return strcmp(leaf, "any") == 0 || strcmp(leaf, "empty") == 0;
}

static char *hm_generic_base_owned(const char *name) {
  const char *leaf = ny_generic_type_leaf(name);
  if (!leaf)
    return NULL;
  const char *lt = strchr(leaf, '<');
  if (!lt || lt == leaf)
    return NULL;
  return ny_strndup(leaf, (size_t)(lt - leaf));
}

static char *hm_merge_generic_arg_owned(const char *want, const char *got) {
  if (hm_generic_arg_is_dynamic(want))
    return ny_strdup(got ? got : "any");
  if (hm_generic_arg_is_dynamic(got))
    return ny_strdup(want ? want : "any");
  if (hm_generic_base_same(want, got)) {
    char *merged = hm_merge_generic_name_owned(want, got);
    if (merged)
      return merged;
  }
  return ny_strdup(want ? want : (got ? got : "any"));
}

static char *hm_merge_generic_name_owned(const char *want, const char *got) {
  if (!hm_generic_base_same(want, got) ||
      !hm_generic_name_compatible(want, got))
    return NULL;
  char *base = hm_generic_base_owned(want);
  if (!base)
    return NULL;
  ny_tp_json_t j = {0};
  tp_append(&j, "%s<", base);
  free(base);
  for (size_t i = 0; i < 32; ++i) {
    char *wa = ny_generic_type_arg_owned(want, i);
    char *ga = ny_generic_type_arg_owned(got, i);
    if (!wa && !ga)
      break;
    if (!wa || !ga) {
      free(wa);
      free(ga);
      free(j.data);
      return NULL;
    }
    char *arg = hm_merge_generic_arg_owned(wa, ga);
    if (i)
      tp_append(&j, ", ");
    tp_append(&j, "%s", arg ? arg : "any");
    free(arg);
    free(wa);
    free(ga);
  }
  tp_append(&j, ">");
  return tp_take(&j, "");
}

static bool hm_generic_name_compatible(const char *want, const char *got) {
  if (!hm_generic_base_same(want, got))
    return false;
  for (size_t i = 0; i < 16; ++i) {
    char *wa = ny_generic_type_arg_owned(want, i);
    char *ga = ny_generic_type_arg_owned(got, i);
    if (!wa && !ga)
      return true;
    if (!wa || !ga) {
      free(wa);
      free(ga);
      return false;
    }
    bool ok = hm_name_compatible(wa, ga);
    free(wa);
    free(ga);
    if (!ok)
      return false;
  }
  return true;
}

static bool hm_name_compatible(const char *want, const char *got) {
  if (!want || !got)
    return true;
  if (strcmp(want, got) == 0 || strcmp(want, "any") == 0 ||
      strcmp(got, "any") == 0)
    return true;
  if (hm_generic_name_compatible(want, got))
    return true;
  if ((hm_is_callable_name(want) && hm_is_callable_name(got)))
    return true;
  if (tp_type_is_group(want) && tp_type_group_accepts(want, got))
    return true;
  if (tp_type_is_group(got) && tp_type_group_accepts(got, want))
    return true;
  if (strcmp(want, "number") == 0 && hm_numeric_name(got))
    return true;
  if (strcmp(got, "number") == 0 && hm_numeric_name(want))
    return true;
  if (strcmp(want, "integer") == 0 && hm_integer_name(got))
    return true;
  if (strcmp(got, "integer") == 0 && hm_integer_name(want))
    return true;
  if (strcmp(want, "float") == 0 && hm_float_name(got))
    return true;
  if (strcmp(got, "float") == 0 && hm_float_name(want))
    return true;
  if (hm_numeric_name(want) && hm_numeric_name(got))
    return true;
  if (tp_is_int_type(want) && tp_is_int_type(got))
    return true;
  if (tp_is_float_type(want) && tp_is_float_type(got))
    return true;
  if ((strcmp(want, "char") == 0 && strcmp(got, "str") == 0) ||
      (strcmp(want, "str") == 0 && strcmp(got, "char") == 0))
    return true;
  if ((strcmp(want, "handle") == 0 && tp_is_int_type(got)) ||
      (strcmp(got, "handle") == 0 && tp_is_int_type(want)))
    return true;
  return false;
}

static bool hm_name_accepts_kind(const char *name, ny_hm_kind_t kind) {
  if (!name)
    return false;
  const char *kind_name = NULL;
  switch (kind) {
  case NY_HM_LIST:
    kind_name = "list";
    break;
  case NY_HM_TUPLE:
    kind_name = "tuple";
    break;
  case NY_HM_SET:
    kind_name = "set";
    break;
  case NY_HM_DICT:
    kind_name = "dict";
    break;
  case NY_HM_PTR:
    kind_name = "ptr";
    break;
  case NY_HM_FN:
    kind_name = "fnptr";
    break;
  default:
    break;
  }
  if (kind_name && tp_type_is_group(name) &&
      tp_type_group_accepts(name, kind_name))
    return true;
  return (strcmp(name, "list") == 0 && kind == NY_HM_LIST) ||
         (strcmp(name, "tuple") == 0 && kind == NY_HM_TUPLE) ||
         (strcmp(name, "set") == 0 && kind == NY_HM_SET) ||
         (strcmp(name, "dict") == 0 && kind == NY_HM_DICT) ||
         (strcmp(name, "ptr") == 0 && kind == NY_HM_PTR) ||
         (hm_is_callable_name(name) && kind == NY_HM_FN);
}

static char *hm_type_string(ny_hm_type_t *t);

static const char *hm_diag_hint_for(const char *code, const char *context,
                                    const char *expected, const char *got) {
  if (code && strcmp(code, "hm-occurs-check") == 0)
    return "HM would need an infinite recursive type here.";
  if (code && strcmp(code, "hm-arity-mismatch") == 0)
    return "The callable shape inferred by HM does not match the supplied "
           "arguments.";
  if (code && strcmp(code, "hm-call-non-function") == 0)
    return "The callee expression inferred to a non-callable value; check for "
           "a shadowed name or missing function reference.";
  if (code && strcmp(code, "hm-unknown-member") == 0)
    return "HM resolved the receiver type but could not find that member or "
           "zero-argument property.";
  if (context && strstr(context, "implicit return"))
    return "In Nytrix the final expression in a function body is an implicit "
           "return value.";
  if (context && strstr(context, "return"))
    return "The returned expression must satisfy the function return type.";
  if (context && strstr(context, "argument"))
    return "This argument is checked against the parameter type inferred or "
           "declared for the callee.";
  if (context && strstr(context, "index"))
    return "List, tuple, string, bytes, and range indexes must be int; dict "
           "indexes must match the dict key type.";
  if (expected && got && strcmp(expected, "int") == 0 &&
      strcmp(got, "str") == 0)
    return "A string value reached a context that requires an integer.";
  return "HM reports the source expression where the incompatible type was "
         "observed; inspect the nearest declaration, call, or return context.";
}

static const char *hm_diag_fix_for(const char *code, const char *context,
                                   const char *expected, const char *got) {
  if (code && strcmp(code, "hm-occurs-check") == 0)
    return "Add an explicit type boundary or restructure the value so it is "
           "not recursively defined through itself.";
  if (code && strcmp(code, "hm-arity-mismatch") == 0)
    return "Check the function signature, default parameters, and whether a "
           "member call is already passing the receiver.";
  if (code && strcmp(code, "hm-call-non-function") == 0)
    return "Rename the local value or call the actual function symbol; only "
           "fn, fnptr, or callable values can be invoked.";
  if (code && strcmp(code, "hm-unknown-member") == 0)
    return "Check the member spelling, import the module that defines the "
           "property, or add an explicit helper call.";
  if (expected && got && strcmp(expected, "str") == 0 &&
      strcmp(got, "int") == 0)
    return "Use to_str(x), change the declared type to int, or return a "
           "string-producing expression.";
  if (expected && got && strcmp(expected, "int") == 0 &&
      strcmp(got, "str") == 0)
    return "Use int(s) or atoi(s) when parsing text, or change the expected "
           "type to str.";
  if (context && strstr(context, "implicit return"))
    return "Add an explicit return with the right type, change the function "
           "return annotation, or make the final expression produce the "
           "expected type.";
  if (context && strstr(context, "variable declaration"))
    return "Change the annotation, convert the initializer, or remove the "
           "annotation if dynamic typing is intended.";
  if (context && strstr(context, "argument"))
    return "Convert the argument before the call or update the callee "
           "parameter type.";
  return "Make the two sides agree by changing the annotation, converting the "
         "value, or using the intended variable.";
}

static void hm_unify_diag(ny_hm_state_t *hm, token_t tok, const char *code,
                          const char *context, ny_hm_type_t *want,
                          ny_hm_type_t *got) {
  char *ws = hm_type_string(want);
  char *gs = hm_type_string(got);
  const char *hint = hm_diag_hint_for(code, context, ws, gs);
  const char *fix = hm_diag_fix_for(code, context, ws, gs);
  if (ws && gs && strcmp(ws, "int") == 0 && strcmp(gs, "str") == 0) {
    tp_add_diag_ex(hm ? hm->ctx : NULL, tok, "hm",
                   code ? code : "hm-type-mismatch", context, ws, gs, NULL,
                   hint, fix, "cannot assign string literal to int");
  } else {
    tp_add_diag_ex(hm ? hm->ctx : NULL, tok, "hm",
                   code ? code : "hm-type-mismatch", context, ws, gs, NULL,
                   hint, fix, "%s: expected %s, got %s",
                   context ? context : "type mismatch", ws ? ws : "unknown",
                   gs ? gs : "unknown");
  }
  free(ws);
  free(gs);
}

static bool hm_unify(ny_hm_state_t *hm, ny_hm_type_t *want, ny_hm_type_t *got,
                     token_t tok, const char *context);

static bool hm_unify_silent(ny_hm_state_t *hm, ny_hm_type_t *want,
                            ny_hm_type_t *got, token_t tok,
                            const char *context) {
  size_t before = hm && hm->ctx ? hm->ctx->diagnostics.len : 0;
  bool ok = hm_unify(hm, want, got, tok, context);
  if (hm && hm->ctx && hm->ctx->diagnostics.len > before) {
    for (size_t i = before; i < hm->ctx->diagnostics.len; ++i)
      tp_diag_dispose(&hm->ctx->diagnostics.data[i]);
    hm->ctx->diagnostics.len = before;
  }
  return ok;
}

static bool hm_unify_indexable(ny_hm_state_t *hm, ny_hm_type_t *want,
                               ny_hm_type_t *got, token_t tok,
                               const char *context) {
  want = hm_prune(want);
  got = hm_prune(got);
  if (!want || want->kind != NY_HM_INDEXABLE)
    return false;
  if (!got || hm_is_any(got))
    return true;
  if (got->kind == NY_HM_INDEXABLE)
    return hm_unify(hm, want->a, got->a, tok, context);
  if (got->kind == NY_HM_LIST || got->kind == NY_HM_SET ||
      got->kind == NY_HM_TUPLE)
    return hm_unify(hm, want->a, got->a ? got->a : hm_any(hm), tok, context);
  if (got->kind == NY_HM_DICT)
    return hm_unify(hm, want->a, got->b ? got->b : hm_any(hm), tok, context);
  if (hm_is_name(got, "str"))
    return hm_unify(hm, want->a, hm_name(hm, "str"), tok, context);
  if (hm_is_name(got, "bytes"))
    return hm_unify(hm, want->a, hm_name(hm, "int"), tok, context);
  if (hm_is_name(got, "range"))
    return hm_unify(hm, want->a, hm_name(hm, "int"), tok, context);
  if (got->kind == NY_HM_NAME &&
      (strcmp(got->name ? got->name : "", "list") == 0 ||
       strcmp(got->name ? got->name : "", "tuple") == 0 ||
       strcmp(got->name ? got->name : "", "set") == 0 ||
       strcmp(got->name ? got->name : "", "dict") == 0))
    return hm_unify(hm, want->a, hm_any(hm), tok, context);
  hm_unify_diag(hm, tok, "hm-index-target-mismatch",
                context ? context : "index target", want, got);
  return false;
}

static bool hm_unify(ny_hm_state_t *hm, ny_hm_type_t *want, ny_hm_type_t *got,
                     token_t tok, const char *context) {
  want = hm_prune(want);
  got = hm_prune(got);
  if (!want || !got || want == got || hm_is_any(want) || hm_is_any(got))
    return true;
  if (want->kind == NY_HM_VAR) {
    if (hm_occurs(want, got)) {
      hm_unify_diag(hm, tok, "hm-occurs-check", context, want, got);
      return false;
    }
    want->instance = got;
    return true;
  }
  if (got->kind == NY_HM_VAR)
    return hm_unify(hm, got, want, tok, context);
  if (hm_is_name(got, "nil") &&
      (want->kind == NY_HM_NULLABLE || want->kind == NY_HM_PTR))
    return true;
  if (hm_is_name(want, "nil") &&
      (got->kind == NY_HM_NULLABLE || got->kind == NY_HM_PTR))
    return true;
  if (want->kind == NY_HM_NAME && hm_name_accepts_kind(want->name, got->kind))
    return true;
  if (got->kind == NY_HM_NAME && hm_name_accepts_kind(got->name, want->kind))
    return true;
  if (want->kind == NY_HM_NULLABLE && hm_unify(hm, want->a, got, tok, context))
    return true;
  if (got->kind == NY_HM_NULLABLE && hm_unify(hm, want, got->a, tok, context))
    return true;
  if (want->kind == NY_HM_INDEXABLE)
    return hm_unify_indexable(hm, want, got, tok, context);
  if (got->kind == NY_HM_INDEXABLE)
    return hm_unify_indexable(hm, got, want, tok, context);
  if (want->kind == NY_HM_NAME && got->kind == NY_HM_NAME) {
    if (hm_generic_base_same(want->name, got->name)) {
      if (!hm_generic_name_compatible(want->name, got->name)) {
        hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
        return false;
      }
      char *merged = hm_merge_generic_name_owned(want->name, got->name);
      if (merged) {
        want->name = hm_arena_strdup(hm, merged);
        free(merged);
      }
      return true;
    }
    if (hm_name_compatible(want->name, got->name))
      return true;
    hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
    return false;
  }
  if (want->kind != got->kind) {
    hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
    return false;
  }
  switch (want->kind) {
  case NY_HM_LIST:
  case NY_HM_SET:
  case NY_HM_TUPLE:
  case NY_HM_NULLABLE:
  case NY_HM_PTR:
    return hm_unify(hm, want->a, got->a, tok, context);
  case NY_HM_DICT:
    return hm_unify(hm, want->a, got->a, tok, context) &&
           hm_unify(hm, want->b, got->b, tok, context);
  case NY_HM_FN:
    if (want->args.len != got->args.len) {
      hm_unify_diag(hm, tok, "hm-arity-mismatch", context, want, got);
      return false;
    }
    for (size_t i = 0; i < want->args.len; ++i) {
      if (!hm_unify(hm, want->args.data[i], got->args.data[i], tok, context))
        return false;
    }
    return hm_unify(hm, want->a, got->a, tok, context);
  case NY_HM_UNION:
    if (hm_unify(hm, want->a, got, tok, context) ||
        hm_unify(hm, want->b, got, tok, context))
      return true;
    hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
    return false;
  case NY_HM_INDEXABLE:
    return hm_unify_indexable(hm, want, got, tok, context);
  default:
    return true;
  }
}

static bool hm_type_is_dynamic(ny_hm_type_t *t) {
  t = hm_prune(t);
  if (!t)
    return true;
  if (hm_is_any(t) || t->kind == NY_HM_VAR)
    return true;
  if (t->kind == NY_HM_NAME && t->name && strcmp(t->name, "empty") == 0)
    return true;
  if (t->kind == NY_HM_NAME && t->name && strcmp(t->name, "proof") == 0)
    return false;
  return false;
}

static ny_hm_type_t *hm_merge(ny_hm_state_t *hm, ny_hm_type_t *a,
                              ny_hm_type_t *b) {
  a = hm_prune(a);
  b = hm_prune(b);
  if (!a)
    return b ? b : hm_any(hm);
  if (!b)
    return a;
  if (hm_type_is_dynamic(a))
    return b;
  if (hm_type_is_dynamic(b))
    return a;
  if (hm_is_name(a, "nil"))
    return hm_unary(hm, NY_HM_NULLABLE, b);
  if (hm_is_name(b, "nil"))
    return hm_unary(hm, NY_HM_NULLABLE, a);
  if (hm_unify_silent(hm, a, b, (token_t){0}, "branch merge")) {
    return a;
  }
  return hm_binary(hm, NY_HM_UNION, a, b);
}

static ny_hm_type_t *hm_type_clone_inst(ny_hm_state_t *hm, ny_hm_type_t *t,
                                        int *ids, ny_hm_type_t **fresh,
                                        size_t *len, size_t cap) {
  t = hm_prune(t);
  if (!t)
    return hm_any(hm);
  if (t->kind == NY_HM_VAR) {
    for (size_t i = 0; i < *len; ++i) {
      if (ids[i] == t->id)
        return fresh[i];
    }
    ny_hm_type_t *v = hm_var(hm);
    if (*len < cap) {
      ids[*len] = t->id;
      fresh[*len] = v;
      (*len)++;
    }
    return v;
  }
  ny_hm_type_t *out = hm_type_new(hm, t->kind);
  out->id = t->id;
  out->name = t->name ? hm_arena_strdup(hm, t->name) : NULL;
  out->a = hm_type_clone_inst(hm, t->a, ids, fresh, len, cap);
  out->b = hm_type_clone_inst(hm, t->b, ids, fresh, len, cap);
  for (size_t i = 0; i < t->args.len; ++i)
    hm_args_push(hm, out,
                 hm_type_clone_inst(hm, t->args.data[i], ids, fresh, len, cap));
  return out;
}

static ny_hm_type_t *hm_instantiate(ny_hm_state_t *hm, ny_hm_type_t *scheme) {
  int ids[128];
  ny_hm_type_t *fresh[128];
  size_t len = 0;
  return hm_type_clone_inst(hm, scheme, ids, fresh, &len, 128);
}

static void hm_env_set(ny_hm_env_list *env, const char *name,
                       ny_hm_type_t *type) {
  if (!env || !name || !*name)
    return;
  for (size_t i = env->len; i > 0; --i) {
    ny_hm_env_entry_t *e = &env->data[i - 1];
    if (e->name && strcmp(e->name, name) == 0) {
      e->type = type;
      return;
    }
  }
  ny_hm_env_entry_t e = {.name = name, .type = type};
  vec_push(env, e);
}

static ny_hm_type_t *hm_env_get(ny_hm_state_t *hm, ny_hm_env_list *env,
                                const char *name) {
  (void)hm;
  if (!name)
    return NULL;
  for (size_t i = env ? env->len : 0; i > 0; --i) {
    ny_hm_env_entry_t *e = &env->data[i - 1];
    if (e->name && strcmp(e->name, name) == 0)
      return e->type;
  }
  return NULL;
}

static const char *hm_module_expr_root_name(expr_t *e) {
  if (!e)
    return NULL;
  while (e && e->kind == NY_E_MEMBER)
    e = e->as.member.target;
  return (e && e->kind == NY_E_IDENT) ? e->as.ident.name : NULL;
}

static bool hm_resolve_module_expr_path(ny_hm_state_t *hm, ny_hm_env_list *env,
                                        expr_t *e, char *out, size_t out_cap) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !e || !out || out_cap == 0)
    return false;
  const char *root = hm_module_expr_root_name(e);
  if (!root)
    return false;
  if (hm_env_get(hm, env, root) &&
      !tp_lookup_program_use_alias(hm->ctx->cg, root))
    return false;
  return tp_resolve_module_expr_path(hm->ctx->cg, e, out, out_cap);
}

static bool hm_member_root_is_unresolved_ident(ny_hm_state_t *hm,
                                               ny_hm_env_list *env,
                                               expr_t *target) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !target)
    return false;
  const char *root = hm_module_expr_root_name(target);
  if (!root)
    return false;
  if (hm_env_get(hm, env, root) &&
      !tp_lookup_program_use_alias(hm->ctx->cg, root))
    return false;
  if (tp_lookup_global_type(hm->ctx->cg, root))
    return false;
  const char *module_name =
      ny_lookup_module_alias(hm->ctx->cg, NULL, 0, root, strlen(root), 0);
  if (module_name && *module_name)
    return false;
  const char *resolved = resolve_import_alias(hm->ctx->cg, root);
  if (resolved && *resolved && strcmp(resolved, root) != 0)
    return false;
  return true;
}

static ny_hm_scheme_t *hm_find_scheme(ny_hm_state_t *hm, const char *name) {
  if (!hm || !name)
    return NULL;
  for (size_t i = hm->schemes.len; i > 0; --i) {
    ny_hm_scheme_t *s = &hm->schemes.data[i - 1];
    if ((s->full_name && strcmp(s->full_name, name) == 0) ||
        (s->name && strcmp(s->name, name) == 0))
      return s;
  }
  return NULL;
}

static char *hm_full_name(const char *owner, const char *name) {
  if (!owner || !*owner || !name || strchr(name, '.'))
    return ny_strdup(name ? name : "");
  size_t on = strlen(owner), nn = strlen(name);
  char *out = malloc(on + nn + 2);
  if (!out)
    return ny_strdup(name);
  memcpy(out, owner, on);
  out[on] = '.';
  memcpy(out + on + 1, name, nn + 1);
  return out;
}

static char *hm_trim_owned(const char *s, size_t n) {
  while (n > 0 && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
    n--;
  return ny_strndup(s, n);
}

static const char *hm_find_top_comma(const char *s) {
  int depth = 0;
  for (const char *p = s; p && *p; ++p) {
    if (*p == '<')
      depth++;
    else if (*p == '>')
      depth--;
    else if (*p == ',' && depth == 0)
      return p;
  }
  return NULL;
}

static ny_hm_type_t *hm_parse_type_name(ny_hm_state_t *hm, const char *raw,
                                        const char *self_name) {
  if (!raw || !*raw)
    return hm_var(hm);
  while (*raw == ' ' || *raw == '\t')
    raw++;
  if (strcmp(raw, "self") == 0 && self_name && *self_name)
    return hm_name(hm, self_name);
  if (raw[0] == '?')
    return hm_unary(hm, NY_HM_NULLABLE,
                    hm_parse_type_name(hm, raw + 1, self_name));
  if (raw[0] == '*')
    return hm_unary(hm, NY_HM_PTR, hm_parse_type_name(hm, raw + 1, self_name));
  const char *lt = strchr(raw, '<');
  if (!lt) {
    if (strcmp(raw, "list") == 0)
      return hm_unary(hm, NY_HM_LIST, hm_var(hm));
    if (strcmp(raw, "set") == 0)
      return hm_unary(hm, NY_HM_SET, hm_var(hm));
    if (strcmp(raw, "tuple") == 0)
      return hm_unary(hm, NY_HM_TUPLE, hm_var(hm));
    if (strcmp(raw, "dict") == 0)
      return hm_binary(hm, NY_HM_DICT, hm_var(hm), hm_var(hm));
    return hm_name(hm, raw);
  }
  const char *gt = strrchr(raw, '>');
  if (!gt || gt < lt)
    return hm_name(hm, raw);
  char *base = hm_trim_owned(raw, (size_t)(lt - raw));
  char *inside = hm_trim_owned(lt + 1, (size_t)(gt - lt - 1));
  ny_hm_type_t *out = NULL;
  if (strcmp(base, "list") == 0)
    out = hm_unary(hm, NY_HM_LIST, hm_parse_type_name(hm, inside, self_name));
  else if (strcmp(base, "set") == 0)
    out = hm_unary(hm, NY_HM_SET, hm_parse_type_name(hm, inside, self_name));
  else if (strcmp(base, "tuple") == 0)
    out = hm_unary(hm, NY_HM_TUPLE, hm_parse_type_name(hm, inside, self_name));
  else if (strcmp(base, "dict") == 0) {
    const char *comma = hm_find_top_comma(inside);
    if (comma) {
      char *k = hm_trim_owned(inside, (size_t)(comma - inside));
      char *v = hm_trim_owned(comma + 1, strlen(comma + 1));
      out = hm_binary(hm, NY_HM_DICT, hm_parse_type_name(hm, k, self_name),
                      hm_parse_type_name(hm, v, self_name));
      free(k);
      free(v);
    } else {
      out = hm_binary(hm, NY_HM_DICT, hm_any(hm), hm_any(hm));
    }
  } else {
    out = hm_name(hm, raw);
  }
  free(base);
  free(inside);
  return out ? out : hm_name(hm, raw);
}

static const char *hm_type_part(const char *s) { return s ? s : "any"; }

static char *hm_type_format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n < 0) {
    va_end(ap);
    return NULL;
  }
  char *out = malloc((size_t)n + 1);
  if (!out) {
    va_end(ap);
    return NULL;
  }
  vsnprintf(out, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return out;
}

static char *hm_type_string(ny_hm_type_t *t) {
  t = hm_prune(t);
  if (!t)
    return ny_strdup("any");
  if (t->kind == NY_HM_ANY)
    return ny_strdup("any");
  if (t->kind == NY_HM_VAR) {
    char buf[32];
    snprintf(buf, sizeof(buf), "'%c%d", 'a' + (t->id % 26), t->id / 26);
    return ny_strdup(buf);
  }
  if (t->kind == NY_HM_NAME)
    return ny_strdup(t->name ? t->name : "any");
  char *a = hm_type_string(t->a);
  char *b = hm_type_string(t->b);
  const char *name = NULL;
  char *out = NULL;
  switch (t->kind) {
  case NY_HM_LIST:
    name = "list";
    break;
  case NY_HM_SET:
    name = "set";
    break;
  case NY_HM_TUPLE:
    name = "tuple";
    break;
  case NY_HM_NULLABLE:
    out = hm_type_format("?%s", hm_type_part(a));
    break;
  case NY_HM_PTR:
    out = hm_type_format("*%s", hm_type_part(a));
    break;
  case NY_HM_DICT:
    out = hm_type_format("dict<%s, %s>", hm_type_part(a), hm_type_part(b));
    break;
  case NY_HM_FN: {
    ny_tp_json_t j = {0};
    tp_append(&j, "fn(");
    for (size_t i = 0; i < t->args.len; ++i) {
      char *arg = hm_type_string(t->args.data[i]);
      if (i)
        tp_append(&j, ", ");
      tp_append(&j, "%s", arg ? arg : "any");
      free(arg);
    }
    tp_append(&j, ")->%s", a ? a : "any");
    out = tp_take(&j, "fn");
    break;
  }
  case NY_HM_UNION:
    out = hm_type_format("%s|%s", hm_type_part(a), hm_type_part(b));
    break;
  case NY_HM_INDEXABLE:
    name = "indexable";
    break;
  default:
    break;
  }
  if (!out && name) {
    out = hm_type_format("%s<%s>", name, hm_type_part(a));
  }
  free(a);
  free(b);
  return out ? out : ny_strdup("any");
}

static ny_hm_type_t *hm_make_result_type(ny_hm_state_t *hm, ny_hm_type_t *ok_t,
                                         ny_hm_type_t *err_t) {
  char *ok = hm_type_string(ok_t ? ok_t : hm_any(hm));
  char *err = hm_type_string(err_t ? err_t : hm_any(hm));
  size_t ok_len = strlen(ok ? ok : "any");
  size_t err_len = strlen(err ? err : "any");
  char *name = malloc(ok_len + err_len + 11);
  if (!name) {
    free(ok);
    free(err);
    return hm_name(hm, "Result");
  }
  snprintf(name, ok_len + err_len + 11, "Result<%s, %s>", ok ? ok : "any",
           err ? err : "any");
  ny_hm_type_t *out = hm_parse_type_name(hm, name, NULL);
  free(name);
  free(ok);
  free(err);
  return out;
}

static ny_hm_type_t *hm_result_payload_type(ny_hm_state_t *hm,
                                            ny_hm_type_t *result_t,
                                            bool want_ok) {
  char *name = hm_type_string(result_t);
  if (!name || !hm_generic_base_same(name, "Result<any, any>")) {
    free(name);
    return hm_any(hm);
  }
  char *payload = ny_generic_type_arg_owned(name, want_ok ? 0u : 1u);
  ny_hm_type_t *out =
      payload ? hm_parse_type_name(hm, payload, NULL) : hm_any(hm);
  free(payload);
  free(name);
  return out;
}

static bool hm_type_is_result_type(ny_hm_type_t *t) {
  char *name = hm_type_string(t);
  bool ok = name && ny_generic_type_base_is(name, "Result");
  free(name);
  return ok;
}

static void hm_strict_result_payload_check(ny_hm_state_t *hm, token_t tok,
                                           ny_hm_type_t *result_t,
                                           ny_hm_type_t *payload_t,
                                           const char *context) {
  if (hm_type_is_result_type(result_t) && !hm_type_is_dynamic(payload_t))
    return;
  char *got = hm_type_string(result_t);
  tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm",
                  "hm-result-payload-dynamic", "result",
                  "%s has no statically known Result payload",
                  context ? context : "result payload");
  if (!hm_strict_types_enabled(hm, tok)) {
    free(got);
    return;
  }
  hm_strict_diag(hm, tok, "hm-strict-result-payload",
                 context ? context : "result payload refinement",
                 "Result<T, E> with statically known payload",
                 got ? got : "any", "result",
                 "strict type checks require Result<T, E> payload evidence "
                 "before unwrap or result-pattern binding");
  free(got);
}

static ny_hm_type_t *hm_infer_expr(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   expr_t *e, const char *self_name);
static void hm_infer_stmt(ny_hm_state_t *hm, ny_hm_env_list *env, stmt_t *s,
                          const char *self_name, ny_hm_type_t *ret);
static void hm_infer_stmt_mode(ny_hm_state_t *hm, ny_hm_env_list *env,
                               stmt_t *s, const char *self_name,
                               ny_hm_type_t *ret, bool allow_tail_expr);
static void hm_env_clone(ny_hm_env_list *dst, const ny_hm_env_list *src);

static ny_hm_type_t *hm_infer_comptime_expr(ny_hm_state_t *hm,
                                            ny_hm_env_list *env, expr_t *e,
                                            const char *self_name) {
  if (!hm || !e || e->kind != NY_E_COMPTIME || !e->as.comptime_expr.body)
    return hm_any(hm);
  ny_hm_env_list local;
  hm_env_clone(&local, env);
  ny_hm_type_t *ret = hm_var(hm);
  hm_infer_stmt(hm, &local, e->as.comptime_expr.body, self_name, ret);
  vec_free(&local);
  ny_hm_type_t *out = hm_prune(ret);
  if (!out || (out->kind == NY_HM_VAR && !out->instance))
    return hm_any(hm);
  return out;
}

static void hm_env_clone(ny_hm_env_list *dst, const ny_hm_env_list *src) {
  vec_init(dst);
  for (size_t i = 0; src && i < src->len; ++i)
    vec_push(dst, src->data[i]);
}

static bool hm_type_name_allows_dynamic_literal(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  if (strcmp(type_name, "any") == 0)
    return true;
  const char *leaf = ny_name_leaf(type_name);
  if (!leaf)
    leaf = type_name;
  if (strcmp(leaf, "any") == 0)
    return true;
  return strstr(leaf, "<any") || strstr(leaf, ", any") ||
         strstr(leaf, ",any") || strstr(leaf, " any>");
}

static bool hm_call_is_layout_from(ny_hm_state_t *hm, const char *name) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !name)
    return false;
  const char *leaf = ny_name_leaf(name);
  if (!leaf)
    leaf = name;
  size_t len = strlen(leaf);
  const char suffix[] = "_from";
  size_t suffix_len = sizeof(suffix) - 1;
  if (len <= suffix_len || strcmp(leaf + len - suffix_len, suffix) != 0)
    return false;
  char *layout_name = ny_strndup(leaf, len - suffix_len);
  bool ok = lookup_layout(hm->ctx->cg, layout_name) != NULL;
  free(layout_name);
  return ok;
}

static ny_hm_type_t *
hm_infer_expr_dynamic_literal_ok(ny_hm_state_t *hm, ny_hm_env_list *env,
                                 expr_t *e, const char *self_name, bool allow) {
  if (allow)
    hm->allow_dynamic_literal_depth++;
  ny_hm_type_t *out = hm_infer_expr(hm, env, e, self_name);
  if (allow)
    hm->allow_dynamic_literal_depth--;
  return out;
}

static char *hm_pattern_call_name(ny_hm_state_t *hm, expr_t *pat) {
  codegen_t *cg = hm && hm->ctx ? hm->ctx->cg : NULL;
  if (!cg || !pat)
    return NULL;
  if (pat->kind == NY_E_IDENT)
    return ny_strdup(pat->as.ident.name ? pat->as.ident.name : "");
  if (pat->kind == NY_E_MEMBER)
    return codegen_full_name(cg, pat, NULL);
  if (pat->kind == NY_E_CALL && pat->as.call.callee)
    return codegen_full_name(cg, pat->as.call.callee, NULL);
  if (pat->kind == NY_E_MEMCALL && pat->as.memcall.target &&
      pat->as.memcall.name) {
    char *target = codegen_full_name(cg, pat->as.memcall.target, NULL);
    if (!target)
      return NULL;
    size_t a = strlen(target);
    size_t b = strlen(pat->as.memcall.name);
       char *out = malloc(a + b + 2);
       if (!out) {
         free(target);
         return NULL;
       }
       memcpy(out, target, a);
       out[a] = '.';
       memcpy(out + a + 1, pat->as.memcall.name, b + 1);
       free(target);
       return out;
     }
  return NULL;
}

static ny_hm_type_t *hm_adt_field_match_type(ny_hm_state_t *hm,
                                             enum_def_t *owner,
                                             const char *subject_name,
                                             const char *field_type) {
  if (!field_type)
    return hm_any(hm);
  bool is_type_param = false;
  for (size_t i = 0; owner && i < owner->type_params.len; ++i) {
    const char *param = owner->type_params.data[i];
    if (!param || strcmp(field_type, param) != 0)
      continue;
    is_type_param = true;
    if (ny_generic_type_base_is(subject_name, owner->name)) {
      char *actual = ny_generic_type_arg_owned(subject_name, i);
      if (actual) {
        ny_hm_type_t *out = hm_parse_type_name(hm, actual, owner->name);
        free(actual);
        return out;
      }
    }
  }
  return is_type_param
             ? hm_any(hm)
             : hm_parse_type_name(hm, field_type, owner ? owner->name : NULL);
}

static void hm_refine_match_pattern(ny_hm_state_t *hm, ny_hm_env_list *env,
                                    ny_hm_type_t *subject_t, expr_t *pat,
                                    const char *self_name) {
  (void)self_name;
  if (!hm || !env || !pat)
    return;
  if (pat->kind == NY_E_CALL && pat->as.call.callee &&
      pat->as.call.callee->kind == NY_E_IDENT) {
    const char *callee = pat->as.call.callee->as.ident.name;
    bool is_ok = callee && strcmp(callee, "ok") == 0;
    bool is_err = callee && strcmp(callee, "err") == 0;
    if (is_ok || is_err) {
      call_arg_t *args = NULL;
      size_t len = 0;
      ny_expr_call_args(pat, &args, &len);
      if (len > 0 && args && args[0].val && args[0].val->kind == NY_E_IDENT) {
        const char *name = args[0].val->as.ident.name;
        if (name && strcmp(name, "_") != 0) {
          ny_hm_type_t *payload_t =
              hm_result_payload_type(hm, subject_t, is_ok);
          hm_strict_result_payload_check(hm, pat->tok, subject_t, payload_t,
                                         is_ok ? "ok pattern payload"
                                               : "err pattern payload");
          hm_env_set(env, name, payload_t);
        }
      }
      return;
    }
  }

  enum_def_t *owner = NULL;
  char *name = hm_pattern_call_name(hm, pat);
  enum_member_def_t *mem =
      name && hm && hm->ctx && hm->ctx->cg
          ? lookup_enum_member_owner(hm->ctx->cg, name, &owner)
          : NULL;
  free(name);
  if (!mem || !owner || !mem->has_payload)
    return;

  call_arg_t *args = NULL;
  size_t len = 0;
  if (!ny_expr_call_args(pat, &args, &len))
    return;
  char *subject_name = hm_type_string(subject_t);
  for (size_t i = 0; i < len; ++i) {
    if (!args[i].name || !args[i].val || args[i].val->kind != NY_E_IDENT)
      continue;
    const char *bind = args[i].val->as.ident.name;
    if (!bind || strcmp(bind, "_") == 0)
      continue;
    ssize_t field = ny_enum_member_field_index(mem, args[i].name);
    if (field < 0)
      continue;
    hm_env_set(env, bind,
               hm_adt_field_match_type(hm, owner, subject_name,
                                       mem->fields.data[field].type_name));
  }
  free(subject_name);
}

static char *hm_attached_owner_name(const char *type_name) {
  if (!type_name)
    return ny_strdup("any");
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  const char *end = type_name;
  while (*end && *end != '<' && *end != '|' && *end != ' ')
    end++;
  if (end == type_name)
    return ny_strdup("any");
  return ny_strndup(type_name, (size_t)(end - type_name));
}

static bool hm_scheme_allows_property(const ny_hm_scheme_t *scheme) {
  if (!scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC)
    return true;
  ny_param_list *params = &scheme->stmt->as.fn.params;
  if (params->len < 1)
    return false;
  for (size_t i = 1; i < params->len; ++i) {
    if (!params->data[i].def)
      return false;
  }
  return true;
}

static fun_sig *hm_lookup_attached_sig(ny_hm_state_t *hm, const char *owner,
                                       const char *member) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !owner || !*owner || !member ||
      !*member)
    return NULL;
  char direct[512];
  int n = snprintf(direct, sizeof(direct), "%s.%s", owner, member);
  if (n > 0 && (size_t)n < sizeof(direct)) {
    fun_sig *sig = lookup_fun(hm->ctx->cg, direct, 0);
    if (sig)
      return sig;
  }
  if (strcmp(owner, "integer") == 0) {
    fun_sig *sig = hm_lookup_attached_sig(hm, "bigint", member);
    if (sig)
      return sig;
    sig = hm_lookup_attached_sig(hm, "int", member);
    if (sig)
      return sig;
  }
  if (strchr(owner, '.'))
    return NULL;
  for (size_t i = 0; i < hm->ctx->cg->use_modules.len; ++i) {
    const char *mod = hm->ctx->cg->use_modules.data[i];
    if (!mod || !*mod)
      continue;
    char imported[512];
    int in =
        snprintf(imported, sizeof(imported), "%s.%s.%s", mod, owner, member);
    if (in > 0 && (size_t)in < sizeof(imported)) {
      fun_sig *sig = lookup_fun(hm->ctx->cg, imported, 0);
      if (sig)
        return sig;
    }
  }
  for (size_t i = 0; i < hm->ctx->cg->user_use_modules.len; ++i) {
    const char *mod = hm->ctx->cg->user_use_modules.data[i];
    if (!mod || !*mod)
      continue;
    char imported[512];
    int in =
        snprintf(imported, sizeof(imported), "%s.%s.%s", mod, owner, member);
    if (in > 0 && (size_t)in < sizeof(imported)) {
      fun_sig *sig = lookup_fun(hm->ctx->cg, imported, 0);
      if (sig)
        return sig;
    }
  }
  return NULL;
}

static ny_hm_type_t *hm_sig_return_type(ny_hm_state_t *hm, fun_sig *sig,
                                        const char *self_name) {
  if (!sig)
    return hm_any(hm);
  if (sig->return_type)
    return hm_parse_type_name(hm, sig->return_type, self_name);
  if (sig->inferred_return_type)
    return hm_parse_type_name(hm, sig->inferred_return_type, self_name);
  return hm_any(hm);
}

static ny_hm_type_t *hm_infer_lambda(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     expr_t *e, const char *self_name) {
  ny_hm_type_t *ft = hm_fn_type(hm);
  if (!e)
    return ft;
  ny_hm_env_list local;
  hm_env_clone(&local, env);
  bool explicit_type = e->as.lambda.return_type != NULL;
  for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
    param_t *p = &e->as.lambda.params.data[i];
    if (p->type)
      explicit_type = true;
    ny_hm_type_t *pt =
        p->type ? hm_parse_type_name(hm, p->type, self_name) : hm_var(hm);
    hm_args_push(hm, ft, pt);
    hm_env_set(&local, p->name, pt);
    if (p->def)
      hm_unify(hm, pt, hm_infer_expr(hm, &local, p->def, self_name),
               p->def->tok, "lambda default parameter");
  }
  ft->a = e->as.lambda.return_type
              ? hm_parse_type_name(hm, e->as.lambda.return_type, self_name)
              : hm_var(hm);
  if (!explicit_type)
    hm->dynamic_lambda_depth++;
  hm_infer_stmt(hm, &local, e->as.lambda.body, self_name, ft->a);
  if (!explicit_type)
    hm->dynamic_lambda_depth--;
  vec_free(&local);
  return ft;
}

static size_t hm_scheme_min_args(const ny_hm_scheme_t *scheme, size_t offset) {
  if (!scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC)
    return 0;
  ny_param_list *params = &scheme->stmt->as.fn.params;
  if (offset >= params->len)
    return 0;
  size_t min_args = params->len - offset;
  for (size_t i = offset; i < params->len; ++i) {
    if (params->data[i].def) {
      min_args = i - offset;
      break;
    }
  }
  return min_args;
}

static bool hm_scheme_is_variadic(const ny_hm_scheme_t *scheme) {
  return scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
         scheme->stmt->as.fn.is_variadic;
}

static ny_hm_type_t *
hm_call_function_type(ny_hm_state_t *hm, ny_hm_env_list *env,
                      ny_hm_type_t *callee_t, ny_call_arg_list *args,
                      token_t tok, const char *self_name, const char *context) {
  callee_t = hm_prune(callee_t);
  if (!callee_t || hm_is_any(callee_t)) {
    tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm", "hm-dynamic-call",
                    "call", "%s uses a callee inferred as any",
                    context ? context : "call expression");
    hm_strict_diag(
        hm, tok, "hm-strict-dynamic-call",
        context ? context : "call expression", "known callable signature",
        "dynamic any", "call",
        "strict type checks reject calls through values inferred as any");
    for (size_t i = 0; args && i < args->len; ++i)
      (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
    return hm_any(hm);
  }
  if (hm_type_is_unbound_var(callee_t)) {
    ny_hm_type_t *fn = hm_fn_type(hm);
    for (size_t i = 0; args && i < args->len; ++i)
      hm_args_push(hm, fn, hm_var(hm));
    fn->a = hm_var(hm);
    hm_unify(hm, callee_t, fn, tok, context ? context : "call expression");
    return hm_call_function_type(hm, env, fn, args, tok, self_name, context);
  }
  if (callee_t->kind == NY_HM_NAME && hm_is_callable_name(callee_t->name)) {
    tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm",
                    "hm-callable-return-any", "call",
                    "%s calls %s without a known return payload",
                    context ? context : "call expression",
                    callee_t->name ? callee_t->name : "callable");
    hm_strict_diag(
        hm, tok, "hm-strict-dynamic-call",
        context ? context : "call expression", "fn(...) -> T signature",
        callee_t->name ? callee_t->name : "callable", "call",
        "strict type checks reject calls whose return payload is unknown");
    for (size_t i = 0; args && i < args->len; ++i)
      (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
    return hm_any(hm);
  }
  if (callee_t->kind != NY_HM_FN) {
    char *got = hm_type_string(callee_t);
    tp_add_diag_ex(
        hm ? hm->ctx : NULL, tok, "hm", "hm-call-non-function",
        context ? context : "call expression", "callable",
        got ? got : "unknown", "call",
        hm_diag_hint_for("hm-call-non-function", context, "callable", got),
        hm_diag_fix_for("hm-call-non-function", context, "callable", got),
        "%s: expected callable, got %s", context ? context : "call expression",
        got ? got : "unknown");
    free(got);
    for (size_t i = 0; args && i < args->len; ++i)
      (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
    return hm_any(hm);
  }
  size_t given = args ? args->len : 0;
  if (given != callee_t->args.len) {
    hm_unify_diag(hm, tok, "hm-arity-mismatch",
                  context ? context : "function call", callee_t, hm_any(hm));
  }
  size_t n = given < callee_t->args.len ? given : callee_t->args.len;
  for (size_t i = 0; i < n; ++i) {
    ny_hm_type_t *arg_t = hm_infer_expr(hm, env, args->data[i].val, self_name);
    hm_unify(hm, callee_t->args.data[i], arg_t,
             args->data[i].val ? args->data[i].val->tok : tok,
             "function argument");
  }
  for (size_t i = n; args && i < args->len; ++i)
    (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
  return callee_t->a ? callee_t->a : hm_any(hm);
}

static ny_hm_type_t *hm_builtin_call_type(ny_hm_state_t *hm, const char *name,
                                          ny_call_arg_list *args) {
  if (!name)
    return NULL;
  const char *leaf = ny_name_leaf(name);
  if (!leaf)
    leaf = name;
  if (strcmp(name, "list") == 0 || strcmp(name, "std.core.list") == 0)
    return hm_unary(hm, NY_HM_LIST, hm_any(hm));
  if (strcmp(name, "tuple") == 0)
    return hm_unary(hm, NY_HM_TUPLE, hm_any(hm));
  if (strcmp(name, "dict") == 0 || strcmp(name, "std.core.dict") == 0)
    return hm_binary(hm, NY_HM_DICT, hm_any(hm), hm_any(hm));
  if (strcmp(name, "set") == 0 || strcmp(name, "std.core.set") == 0) {
    if (args && args->len >= 3)
      return hm_binary(hm, NY_HM_DICT, hm_any(hm), hm_any(hm));
    return hm_unary(hm, NY_HM_SET, hm_any(hm));
  }
  if (strcmp(name, "bytes") == 0)
    return hm_name(hm, "bytes");
  if (strcmp(name, "range") == 0)
    return hm_name(hm, "range");
  if (strcmp(name, "int") == 0 || strcmp(name, "to_int") == 0 ||
      strcmp(name, "len") == 0 || strcmp(name, "__layout_size") == 0 ||
      strcmp(name, "__layout_offset") == 0 || strcmp(name, "load64") == 0 ||
      strcmp(name, "load32") == 0 || strcmp(name, "load64_i") == 0)
    return hm_name(hm, "int");
  if (strcmp(name, "load64_h") == 0)
    return hm_name(hm, "handle");
  if (strcmp(name, "float") == 0 || strcmp(name, "to_float") == 0)
    return hm_name(hm, "f64");
  if (strcmp(name, "str") == 0 || strcmp(name, "to_str") == 0 ||
      strcmp(name, "type") == 0 || strcmp(name, "type_name") == 0 ||
      strcmp(name, "type_shape") == 0)
    return hm_name(hm, "str");
  if (strcmp(name, "assert") == 0 || strcmp(name, "print") == 0 ||
      strcmp(name, "eprint") == 0 || strcmp(name, "free") == 0 ||
      strcmp(name, "store64") == 0 || strcmp(name, "store64_h") == 0 ||
      strcmp(name, "store64_i") == 0 || strcmp(name, "store_layout") == 0)
    return hm_name(hm, "nil");
  if (strcmp(leaf, "static_assert") == 0 ||
      strcmp(leaf, "assert_compile") == 0 ||
      strcmp(leaf, "assert_compile_range") == 0 ||
      strcmp(leaf, "assert_compile_index") == 0 ||
      strcmp(leaf, "proof_matches") == 0)
    return hm_name(hm, "bool");
  if (strcmp(leaf, "prove") == 0)
    return hm_name(hm, "proof");
  if (strcmp(name, "malloc") == 0)
    return hm_unary(hm, NY_HM_PTR, hm_any(hm));
  if (strcmp(name, "addr_of") == 0)
    return hm_unary(hm, NY_HM_PTR, hm_any(hm));
  if (args && args->len <= 1 &&
      (strcmp(name, "__tagof") == 0 || strcmp(name, "__runtime_tag") == 0 ||
       strcmp(name, "__tag") == 0))
    return hm_name(hm, "int");
  return NULL;
}

static bool hm_tagged_type_exists(ny_hm_state_t *hm, const char *name) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !name)
    return false;
  for (size_t i = 0; i < hm->ctx->cg->tagged_types.len; ++i) {
    const char *tag = hm->ctx->cg->tagged_types.data[i];
    if (tag && strcmp(tag, name) == 0)
      return true;
  }
  return false;
}

static ny_hm_type_t *hm_call_named(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   const char *name, expr_t *target,
                                   ny_call_arg_list *args, token_t tok,
                                   const char *self_name) {
  const char *leaf = ny_name_leaf(name);
  bool allow_dynamic_literal_args = hm_call_is_layout_from(hm, name);
  if (!target && leaf && args) {
    if ((strcmp(leaf, "ok") == 0 || strcmp(leaf, "__result_ok") == 0) &&
        args->len == 1) {
      ny_hm_type_t *ok_t = hm_infer_expr(hm, env, args->data[0].val, self_name);
      return hm_make_result_type(hm, ok_t, hm_any(hm));
    }
    if ((strcmp(leaf, "err") == 0 || strcmp(leaf, "__result_err") == 0) &&
        args->len == 1) {
      ny_hm_type_t *err_t =
          hm_infer_expr(hm, env, args->data[0].val, self_name);
      return hm_make_result_type(hm, hm_any(hm), err_t);
    }
    if ((strcmp(leaf, "unwrap") == 0 || strcmp(leaf, "__unwrap") == 0) &&
        args->len == 1) {
      ny_hm_type_t *result_t =
          hm_infer_expr(hm, env, args->data[0].val, self_name);
      ny_hm_type_t *payload_t = hm_result_payload_type(hm, result_t, true);
      hm_strict_result_payload_check(hm, tok, result_t, payload_t,
                                     "unwrap payload");
      return payload_t;
    }
    if (strcmp(leaf, "unwrap_or") == 0 && args->len == 2) {
      ny_hm_type_t *result_t =
          hm_infer_expr(hm, env, args->data[0].val, self_name);
      ny_hm_type_t *fallback_t =
          hm_infer_expr(hm, env, args->data[1].val, self_name);
      ny_hm_type_t *ok_t = hm_result_payload_type(hm, result_t, true);
      hm_strict_result_payload_check(hm, tok, result_t, ok_t,
                                     "unwrap_or payload");
      return hm_type_is_dynamic(ok_t) ? fallback_t : ok_t;
    }
  }
  ny_hm_type_t *builtin = hm_builtin_call_type(hm, name, args);
  if (builtin) {
    if (target)
      (void)hm_infer_expr(hm, env, target, self_name);
    for (size_t i = 0; args && i < args->len; ++i)
      (void)hm_infer_expr_dynamic_literal_ok(
          hm, env, args->data[i].val, self_name, allow_dynamic_literal_args);
    return builtin;
  }
  if (hm_tagged_type_exists(hm, name)) {
    for (size_t i = 0; args && i < args->len; ++i)
      (void)hm_infer_expr_dynamic_literal_ok(
          hm, env, args->data[i].val, self_name, allow_dynamic_literal_args);
    return hm_name(hm, name);
  }
  ny_hm_scheme_t *scheme = hm_find_scheme(hm, name);
  if (!scheme) {
    if (target && leaf && name && strchr(name, '.')) {
      if (hm_member_root_is_unresolved_ident(hm, env, target)) {
        (void)hm_infer_expr(hm, env, target, self_name);
        for (size_t i = 0; args && i < args->len; ++i)
          (void)hm_infer_expr_dynamic_literal_ok(hm, env, args->data[i].val,
                                                 self_name,
                                                 allow_dynamic_literal_args);
        return hm_any(hm);
      }
      const char *dot = strrchr(name, '.');
      if (dot && dot > name) {
        char *owner = ny_strndup(name, (size_t)(dot - name));
        fun_sig *attached = hm_lookup_attached_sig(hm, owner, leaf);
        const char *attached_self = owner;
        if (!attached && strcmp(owner, "any") != 0) {
          attached = hm_lookup_attached_sig(hm, "any", leaf);
          attached_self = "any";
        }
        if (attached) {
          stmt_t *stmt = attached->stmt_t;
          if (stmt && stmt->kind == NY_S_FUNC) {
            size_t offset = 1u;
            ny_param_list *params = &stmt->as.fn.params;
            if (params->len > 0) {
              ny_hm_type_t *want =
                  params->data[0].type
                      ? hm_parse_type_name(hm, params->data[0].type, self_name)
                      : hm_any(hm);
              hm_unify(hm, want, hm_infer_expr(hm, env, target, self_name), tok,
                       "member call target");
            } else {
              (void)hm_infer_expr(hm, env, target, self_name);
            }
            for (size_t i = 0; args && i < args->len; ++i) {
              size_t param = i + offset;
              ny_hm_type_t *arg_t = hm_infer_expr_dynamic_literal_ok(
                  hm, env, args->data[i].val, self_name,
                  allow_dynamic_literal_args);
              if (param < params->len && params->data[param].type) {
                hm_unify(
                    hm,
                    hm_parse_type_name(hm, params->data[param].type, self_name),
                    arg_t, args->data[i].val ? args->data[i].val->tok : tok,
                    "function argument");
              }
            }
          } else {
            (void)hm_infer_expr(hm, env, target, self_name);
            for (size_t i = 0; args && i < args->len; ++i)
              (void)hm_infer_expr_dynamic_literal_ok(
                  hm, env, args->data[i].val, self_name,
                  allow_dynamic_literal_args);
          }
          ny_hm_type_t *out = hm_sig_return_type(
              hm, attached, attached_self ? attached_self : self_name);
          free(owner);
          return out;
        }
        free(owner);
      }
    }
    fun_sig *sig =
        hm && hm->ctx && hm->ctx->cg ? lookup_fun(hm->ctx->cg, name, 0) : NULL;
    if (sig) {
      stmt_t *stmt = sig->stmt_t;
      if (stmt && stmt->kind == NY_S_FUNC) {
        size_t offset = target ? 1u : 0u;
        ny_param_list *params = &stmt->as.fn.params;
        if (target && params->len > 0) {
          ny_hm_type_t *want =
              params->data[0].type
                  ? hm_parse_type_name(hm, params->data[0].type, self_name)
                  : hm_any(hm);
          hm_unify(hm, want, hm_infer_expr(hm, env, target, self_name), tok,
                   "member call target");
        }
        for (size_t i = 0; args && i < args->len; ++i) {
          size_t param = i + offset;
          ny_hm_type_t *arg_t = hm_infer_expr_dynamic_literal_ok(
              hm, env, args->data[i].val, self_name,
              allow_dynamic_literal_args);
          if (param < params->len && params->data[param].type) {
            hm_unify(
                hm, hm_parse_type_name(hm, params->data[param].type, self_name),
                arg_t, args->data[i].val ? args->data[i].val->tok : tok,
                "function argument");
          }
        }
      } else {
        if (target)
          (void)hm_infer_expr(hm, env, target, self_name);
        for (size_t i = 0; args && i < args->len; ++i)
          (void)hm_infer_expr_dynamic_literal_ok(hm, env, args->data[i].val,
                                                 self_name,
                                                 allow_dynamic_literal_args);
      }
      return hm_sig_return_type(hm, sig, self_name);
    }
    if (target || (name && strchr(name, '.'))) {
      tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm", "hm-unknown-call",
                      "call", "unresolved call '%s' returns any",
                      name ? name : "<anonymous>");
      hm_strict_diag(hm, tok, "hm-strict-dynamic-call", "function call",
                     "known callable", name ? name : "<anonymous>", "call",
                     "strict type checks reject unresolved member or qualified "
                     "calls that would return any");
      return hm_any(hm);
    }
    hm_strict_diag(
        hm, tok, "hm-strict-dynamic-call", "function call", "known callable",
        name ? name : "<anonymous>", "call",
        "strict type checks reject unknown calls that would return any");
    return hm_any_fallback(hm, NULL, "hm-unknown-call", "unknown callable '%s'",
                           name ? name : "<anonymous>");
  }
  ny_hm_type_t *fn = hm_instantiate(hm, scheme->type);
  fn = hm_prune(fn);
  if (!fn || fn->kind != NY_HM_FN)
    return hm_any(hm);
  size_t offset = target ? 1u : 0u;
  size_t given = args ? args->len : 0;
  size_t available = fn->args.len > offset ? fn->args.len - offset : 0;
  if (!hm_scheme_is_variadic(scheme) &&
      (given < hm_scheme_min_args(scheme, offset) || given > available)) {
    hm_unify_diag(hm, tok, "hm-arity-mismatch", "function call", fn,
                  hm_any(hm));
  }
  if (target && fn->args.len > 0)
    hm_unify(hm, fn->args.data[0], hm_infer_expr(hm, env, target, self_name),
             tok, "member call target");
  for (size_t i = 0; args && i < args->len; ++i) {
    ny_hm_type_t *arg_t = hm_infer_expr_dynamic_literal_ok(
        hm, env, args->data[i].val, self_name, allow_dynamic_literal_args);
    size_t param = i + offset;
    if (param < fn->args.len)
      hm_unify(hm, fn->args.data[param], arg_t,
               args->data[i].val ? args->data[i].val->tok : tok,
               "function argument");
  }
  return fn->a ? fn->a : hm_any(hm);
}

static ny_hm_type_t *hm_infer_binary(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     expr_t *e, const char *self_name) {
  ny_hm_type_t *lt = hm_infer_expr(hm, env, e->as.binary.left, self_name);
  ny_hm_type_t *rt = hm_infer_expr(hm, env, e->as.binary.right, self_name);
  const char *op = e->as.binary.op;
  if (tp_is_cmp_op(op)) {
    if (hm_is_any(lt) || hm_is_any(rt)) {
      char *ls = hm_type_string(lt);
      char *rs = hm_type_string(rt);
      char got[512];
      snprintf(got, sizeof(got), "%s %s %s", ls ? ls : "any", op ? op : "cmp",
               rs ? rs : "any");
      tp_add_fallback(hm ? hm->ctx : NULL, e->tok, "hm",
                      "hm-dynamic-comparison", "binary",
                      "comparison operands inferred as %s", got);
      hm_strict_diag(hm, e->tok, "hm-strict-dynamic-arithmetic",
                     "comparison expression", "statically typed operands", got,
                     "binary",
                     "strict type checks reject dynamic comparison operands");
      free(ls);
      free(rs);
    }
    if (op && strcmp(op, "==") != 0 && strcmp(op, "!=") != 0) {
      const char *ln = NULL, *rn = NULL;
      bool lnum = hm_type_known_numeric(lt, &ln);
      bool rnum = hm_type_known_numeric(rt, &rn);
      if (lnum && hm_type_is_unbound_var(rt))
        hm_unify(hm, rt, hm_name(hm, ln ? ln : "number"), e->tok,
                 "ordered comparison");
      else if (rnum && hm_type_is_unbound_var(lt))
        hm_unify(hm, lt, hm_name(hm, rn ? rn : "number"), e->tok,
                 "ordered comparison");
    }
    return hm_name(hm, "bool");
  }
  if (!tp_is_arith_op(op))
    return hm_any(hm);
  if (hm_is_any(lt) || hm_is_any(rt)) {
    char *ls = hm_type_string(lt);
    char *rs = hm_type_string(rt);
    char got[512];
    snprintf(got, sizeof(got), "%s %s %s", ls ? ls : "any", op ? op : "op",
             rs ? rs : "any");
    tp_add_fallback(hm ? hm->ctx : NULL, e->tok, "hm",
                    "hm-dynamic-arithmetic", "binary",
                    "arithmetic operands inferred as %s", got);
    hm_strict_diag(hm, e->tok, "hm-strict-dynamic-arithmetic",
                   "arithmetic expression",
                   "numeric/string/collection operands", got, "binary",
                   "strict type checks reject dynamic arithmetic operands");
    free(ls);
    free(rs);
    return hm_any(hm);
  }
  if (op && strcmp(op, "+") == 0) {
    ny_hm_type_t *lp = hm_prune(lt);
    ny_hm_type_t *rp = hm_prune(rt);
    if (lp && rp && lp->kind == rp->kind &&
        (lp->kind == NY_HM_LIST || lp->kind == NY_HM_SET ||
         lp->kind == NY_HM_TUPLE)) {
      hm_unify(hm, lp->a, rp->a, e->tok, "collection operator");
      return lp;
    }
    if (hm_is_name(lp, "str") && hm_is_name(rp, "str"))
      return hm_name(hm, "str");
    if (hm_type_known_string(lp) && hm_type_is_unbound_var(rp)) {
      hm_unify(hm, rp, hm_name(hm, "str"), e->tok, "string operator");
      return hm_name(hm, "str");
    }
    if (hm_type_known_string(rp) && hm_type_is_unbound_var(lp)) {
      hm_unify(hm, lp, hm_name(hm, "str"), e->tok, "string operator");
      return hm_name(hm, "str");
    }
    if (hm_type_known_collection(lp) && hm_type_is_unbound_var(rp)) {
      hm_unify(hm, rp, lp, e->tok, "collection operator");
      return lp;
    }
    if (hm_type_known_collection(rp) && hm_type_is_unbound_var(lp)) {
      hm_unify(hm, lp, rp, e->tok, "collection operator");
      return rp;
    }
  }
  const char *ln = NULL, *rn = NULL;
  bool lnum = hm_type_known_numeric(lt, &ln);
  bool rnum = hm_type_known_numeric(rt, &rn);
  if (lnum && hm_type_is_unbound_var(rt)) {
    hm_unify(hm, rt, hm_name(hm, ln ? ln : "number"), e->tok,
             "numeric operator");
    return hm_name(hm,
                   (ln && tp_is_float_type(ln)) ? ln : (ln ? ln : "number"));
  }
  if (rnum && hm_type_is_unbound_var(lt)) {
    hm_unify(hm, lt, hm_name(hm, rn ? rn : "number"), e->tok,
             "numeric operator");
    return hm_name(hm,
                   (rn && tp_is_float_type(rn)) ? rn : (rn ? rn : "number"));
  }
  if (hm_type_is_unbound_var(lt) && hm_type_is_unbound_var(rt)) {
    ny_hm_type_t *result = hm_var(hm);
    hm_unify(hm, lt, result, e->tok, "arithmetic operand");
    hm_unify(hm, rt, result, e->tok, "arithmetic operand");
    return result;
  }
  char *ls = hm_type_string(lt);
  char *rs = hm_type_string(rt);
  if ((tp_is_int_type(ls) || tp_is_float_type(ls)) &&
      (tp_is_int_type(rs) || tp_is_float_type(rs))) {
    bool f = tp_is_float_type(ls) || tp_is_float_type(rs);
    free(ls);
    free(rs);
    return hm_name(hm, f ? "f64" : "int");
  }
  free(ls);
  free(rs);
  return hm_any(hm);
}

static ny_hm_type_t *hm_infer_expr(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   expr_t *e, const char *self_name) {
  if (!e)
    return hm_any(hm);
  switch (e->kind) {
  case NY_E_LITERAL:
    return hm_name(hm, tp_literal_type(&e->as.literal, e->tok));
  case NY_E_IDENT: {
    ny_hm_type_t *t = hm_env_get(hm, env, e->as.ident.name);
    if (t)
      return t;
    const char *global_type = tp_lookup_global_type(
        hm && hm->ctx ? hm->ctx->cg : NULL, e->as.ident.name);
    if (global_type)
      return hm_parse_type_name(hm, global_type, self_name);
    const char *enum_owner = tp_enum_member_owner_name(hm ? hm->ctx : NULL, e);
    if (enum_owner)
      return hm_parse_type_name(hm, enum_owner, self_name);
    ny_hm_scheme_t *scheme = hm_find_scheme(hm, e->as.ident.name);
    if (scheme)
      return hm_instantiate(hm, scheme->type);
    return hm_any(hm);
  }
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    ny_hm_type_t *elem = hm_var(hm);
    bool saw = false;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      ny_hm_type_t *it =
          hm_infer_expr(hm, env, e->as.list_like.data[i], self_name);
      if (saw) {
        elem = hm_merge(hm, elem, it);
      } else {
        elem = it;
        saw = true;
      }
    }
    if (!saw)
      elem = hm_any(hm);
    return hm_unary(hm,
                    e->kind == NY_E_SET
                        ? NY_HM_SET
                        : (e->kind == NY_E_TUPLE ? NY_HM_TUPLE : NY_HM_LIST),
                    elem);
  }
  case NY_E_DICT: {
    ny_hm_type_t *key = hm_any(hm);
    ny_hm_type_t *val = hm_any(hm);
    bool saw = false;
    bool dynamic_key = false;
    bool dynamic_val = false;
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      dict_pair_t *p = &e->as.dict.pairs.data[i];
      ny_hm_type_t *kt = hm_infer_expr(hm, env, p->key, self_name);
      ny_hm_type_t *vt = hm_infer_expr(hm, env, p->value, self_name);
      if (!saw) {
        key = kt;
        val = vt;
        saw = true;
      } else {
        if (!dynamic_key &&
            !hm_unify_silent(hm, key, kt, p->key ? p->key->tok : e->tok,
                             "dict key")) {
          key = hm_any(hm);
          dynamic_key = true;
        }
        if (!dynamic_val &&
            !hm_unify_silent(hm, val, vt, p->value ? p->value->tok : e->tok,
                             "dict value")) {
          val = hm_any(hm);
          dynamic_val = true;
        }
      }
    }
    if (dynamic_key || dynamic_val) {
      tp_add_fallback(hm ? hm->ctx : NULL, e->tok, "hm",
                      "hm-dict-heterogeneous-literal", "dict",
                      "heterogeneous dict literal widened %s%s%s to any",
                      dynamic_key ? "keys" : "",
                      (dynamic_key && dynamic_val) ? " and " : "",
                      dynamic_val ? "values" : "");
      if (hm && hm->allow_dynamic_literal_depth <= 0) {
        hm_strict_diag(hm, e->tok, "hm-strict-heterogeneous-dict",
                       "dict literal",
                       "homogeneous dict or explicit dynamic annotation",
                       "mixed dict literal", "dict",
                       "strict type checks reject heterogeneous dict literals "
                       "without explicit dynamic intent");
      }
    }
    return hm_binary(hm, NY_HM_DICT, key, val);
  }
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.name) {
      expr_t *callee = e->as.call.callee;
      char module_path[1024];
      if (hm_resolve_module_expr_path(hm, env, callee->as.member.target,
                                      module_path, sizeof(module_path))) {
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(hm->ctx->cg, module_path,
                                            callee->as.member.name,
                                            resolved_fun, sizeof(resolved_fun)))
          return hm_call_named(hm, env, resolved_fun, NULL, &e->as.call.args,
                               e->tok, self_name);
        for (size_t i = 0; i < e->as.call.args.len; ++i)
          (void)hm_infer_expr_dynamic_literal_ok(
              hm, env, e->as.call.args.data[i].val, self_name, true);
        return hm_any(hm);
      }
      if (hm_member_root_is_unresolved_ident(hm, env,
                                             callee->as.member.target)) {
        for (size_t i = 0; i < e->as.call.args.len; ++i)
          (void)hm_infer_expr_dynamic_literal_ok(
              hm, env, e->as.call.args.data[i].val, self_name, true);
        return hm_any(hm);
      }
    }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const char *name = e->as.call.callee->as.ident.name;
      ny_hm_type_t *local = hm_env_get(hm, env, name);
      if (local)
        return hm_call_function_type(hm, env, local, &e->as.call.args, e->tok,
                                     self_name, "local call");
      return hm_call_named(hm, env, name, NULL, &e->as.call.args, e->tok,
                           self_name);
    } else if (e->as.call.callee) {
      ny_hm_type_t *callee_t =
          hm_infer_expr(hm, env, e->as.call.callee, self_name);
      return hm_call_function_type(hm, env, callee_t, &e->as.call.args, e->tok,
                                   self_name, "call expression");
    }
    return hm_any_fallback(hm, e, "hm-call-missing-callee",
                           "call has no callee");
  case NY_E_MEMCALL: {
    if (e->as.memcall.target && e->as.memcall.name) {
      char module_path[1024];
      if (hm_resolve_module_expr_path(hm, env, e->as.memcall.target,
                                      module_path, sizeof(module_path))) {
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(hm->ctx->cg, module_path,
                                            e->as.memcall.name, resolved_fun,
                                            sizeof(resolved_fun)))
          return hm_call_named(hm, env, resolved_fun, NULL, &e->as.memcall.args,
                               e->tok, self_name);
        return hm_any(hm);
      }
    }
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name && e->as.memcall.name) {
      const char *target_name = e->as.memcall.target->as.ident.name;
      if (!hm_env_get(hm, env, target_name)) {
        const char *module_name =
            hm && hm->ctx && hm->ctx->cg
                ? ny_lookup_module_alias(hm->ctx->cg, NULL, 0, target_name,
                                         strlen(target_name), 0)
                : NULL;
        if ((!module_name || !*module_name) && hm && hm->ctx && hm->ctx->cg)
          module_name = resolve_import_alias(hm->ctx->cg, target_name);
        if (module_name && *module_name) {
          char resolved_fun[1280];
          if (ny_resolve_module_function_path(hm->ctx->cg, module_name,
                                              e->as.memcall.name, resolved_fun,
                                              sizeof(resolved_fun)))
            return hm_call_named(hm, env, resolved_fun, NULL,
                                 &e->as.memcall.args, e->tok, self_name);
          return hm_any(hm);
        }
      }
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        (void)hm_infer_expr_dynamic_literal_ok(
            hm, env, e->as.memcall.args.data[i].val, self_name, true);
      return hm_any(hm);
    }
    if (hm_member_root_is_unresolved_ident(hm, env, e->as.memcall.target)) {
      for (size_t i = 0; i < e->as.memcall.args.len; ++i)
        (void)hm_infer_expr_dynamic_literal_ok(
            hm, env, e->as.memcall.args.data[i].val, self_name, true);
      return hm_any(hm);
    }
    ny_hm_type_t *target_t =
        hm_infer_expr(hm, env, e->as.memcall.target, self_name);
    char *target_s = hm_type_string(target_t);
    char *owner = hm_attached_owner_name(target_s);
    char full[512];
    snprintf(full, sizeof(full), "%s.%s", owner ? owner : "any",
             e->as.memcall.name ? e->as.memcall.name : "");
    ny_hm_type_t *ret = hm_call_named(hm, env, full, e->as.memcall.target,
                                      &e->as.memcall.args, e->tok, self_name);
    free(owner);
    free(target_s);
    return ret;
  }
  case NY_E_MEMBER: {
    const char *enum_owner = tp_enum_member_owner_name(hm ? hm->ctx : NULL, e);
    if (enum_owner)
      return hm_parse_type_name(hm, enum_owner, self_name);
  }
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0)
      return hm_name(hm, "int");
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return hm_name(hm, "integer");
    if (e->as.member.target && e->as.member.name) {
      if (e->as.member.target->kind == NY_E_IDENT &&
          e->as.member.target->as.ident.name &&
          !hm_env_get(hm, env, e->as.member.target->as.ident.name)) {
        const char *module_member = tp_lookup_module_alias_member_type(
            hm && hm->ctx ? hm->ctx->cg : NULL,
            e->as.member.target->as.ident.name, e->as.member.name);
        if (module_member)
          return hm_parse_type_name(hm, module_member, self_name);
      }
      char module_path[1024];
      char resolved_fun[1280];
      if (hm_resolve_module_expr_path(hm, env, e->as.member.target, module_path,
                                      sizeof(module_path)) &&
          ny_resolve_module_function_path(hm->ctx->cg, module_path,
                                          e->as.member.name, resolved_fun,
                                          sizeof(resolved_fun)))
        return hm_name(hm, "fnptr");
      if (hm_member_root_is_unresolved_ident(hm, env, e->as.member.target))
        return hm_any(hm);
      if (e->as.member.target->kind == NY_E_IDENT &&
          e->as.member.target->as.ident.name &&
          !hm_env_get(hm, env, e->as.member.target->as.ident.name) &&
          tp_module_alias_member_exists(hm && hm->ctx ? hm->ctx->cg : NULL,
                                        e->as.member.target->as.ident.name,
                                        e->as.member.name))
        return hm_any(hm);
      ny_hm_type_t *target_t =
          hm_infer_expr(hm, env, e->as.member.target, self_name);
      if (hm_type_is_unbound_var(target_t)) {
        tp_add_fallback(hm ? hm->ctx : NULL, e->tok, "hm",
                        "hm-dynamic-member", "member",
                        "member '%s' read from unconstrained value",
                        e->as.member.name ? e->as.member.name : "<member>");
        return hm_any(hm);
      }
      char *target_s = hm_type_string(target_t);
      char *owner = hm_attached_owner_name(target_s);
      if (target_s && strcmp(target_s, "nil") == 0) {
        free(owner);
        free(target_s);
        return hm_name(hm, "nil");
      }
      if (owner && strcmp(owner, "any") != 0) {
        const char *member = e->as.member.name;
        const char *owner_leaf = ny_name_leaf(owner);
        if (!owner_leaf)
          owner_leaf = owner;
        if (strcmp(owner_leaf, "nil") == 0) {
          free(owner);
          free(target_s);
          return hm_name(hm, "nil");
        }
        int vec_dim = 0;
        if (strcmp(owner_leaf, "vec2") == 0 ||
            strcmp(owner_leaf, "Vector2") == 0)
          vec_dim = 2;
        else if (strcmp(owner_leaf, "vec3") == 0 ||
                 strcmp(owner_leaf, "Vector3") == 0)
          vec_dim = 3;
        else if (strcmp(owner_leaf, "vec4") == 0 ||
                 strcmp(owner_leaf, "Vector4") == 0)
          vec_dim = 4;
        if (vec_dim > 0 &&
            ((strcmp(member, "x") == 0) || (strcmp(member, "y") == 0) ||
             (vec_dim >= 3 && strcmp(member, "z") == 0) ||
             (vec_dim >= 4 && strcmp(member, "w") == 0))) {
          free(owner);
          free(target_s);
          return hm_name(hm, "f64");
        }
        if (strcmp(owner_leaf, "dict") == 0) {
          char *value_type = ny_generic_type_arg_owned(target_s, 1);
          ny_hm_type_t *out =
              value_type && *value_type
                  ? hm_parse_type_name(hm, value_type, self_name)
                  : hm_any(hm);
          free(value_type);
          free(owner);
          free(target_s);
          return out;
        }
        layout_def_t *layout = hm && hm->ctx && hm->ctx->cg
                                   ? lookup_layout(hm->ctx->cg, owner)
                                   : NULL;
        if (layout) {
          for (size_t i = 0; i < layout->fields.len; ++i) {
            layout_field_info_t *field = &layout->fields.data[i];
            if (field->name && strcmp(field->name, e->as.member.name) == 0) {
              ny_hm_type_t *out =
                  hm_parse_type_name(hm, field->type_name, owner);
              free(owner);
              free(target_s);
              return out;
            }
          }
          tp_add_diag_ex(hm ? hm->ctx : NULL, e->tok, "hm", "hm-unknown-member",
                         "member access", e->as.member.name, owner, "member",
                         hm_diag_hint_for("hm-unknown-member", "member access",
                                          e->as.member.name, owner),
                         hm_diag_fix_for("hm-unknown-member", "member access",
                                         e->as.member.name, owner),
                         "unknown member '%s' for layout '%s'",
                         e->as.member.name, owner);
          free(owner);
          free(target_s);
          return hm_any(hm);
        }
        char full[512];
        snprintf(full, sizeof(full), "%s.%s", owner, e->as.member.name);
        ny_hm_scheme_t *scheme = hm_find_scheme(hm, full);
        if (scheme && hm_scheme_allows_property(scheme)) {
          ny_hm_type_t *fn = hm_prune(hm_instantiate(hm, scheme->type));
          if (fn && fn->kind == NY_HM_FN && fn->args.len >= 1) {
            hm_unify(hm, fn->args.data[0], target_t, e->tok,
                     "member property target");
            ny_hm_type_t *out = fn->a ? fn->a : hm_any(hm);
            free(owner);
            free(target_s);
            return out;
          }
        }
        fun_sig *sig = hm_lookup_attached_sig(hm, owner, e->as.member.name);
        if (ny_sig_allows_zero_arg_property(sig)) {
          ny_hm_type_t *out = hm_sig_return_type(hm, sig, owner);
          free(owner);
          free(target_s);
          return out;
        }
        if (strcmp(owner, "any") != 0) {
          sig = hm_lookup_attached_sig(hm, "any", e->as.member.name);
          if (ny_sig_allows_zero_arg_property(sig)) {
            ny_hm_type_t *out = hm_sig_return_type(hm, sig, "any");
            free(owner);
            free(target_s);
            return out;
          }
        }
        if (strcmp(e->as.member.name, "long") == 0) {
          free(owner);
          free(target_s);
          return hm_name(hm, "integer");
        }
        ny_hm_type_t *out =
            hm_any_fallback(hm, e, "hm-unknown-member",
                            "unknown member '%s' for inferred type %s",
                            e->as.member.name, target_s ? target_s : "any");
        free(owner);
        free(target_s);
        return out;
      }
      free(owner);
      free(target_s);
    }
    if (e->as.member.name) {
      char full[512];
      snprintf(full, sizeof(full), "any.%s", e->as.member.name);
      ny_hm_scheme_t *scheme = hm_find_scheme(hm, full);
      if (scheme && hm_scheme_allows_property(scheme)) {
        ny_hm_type_t *fn = hm_prune(hm_instantiate(hm, scheme->type));
        if (fn && fn->kind == NY_HM_FN && fn->args.len >= 1)
          return fn->a ? fn->a : hm_any(hm);
      }
      fun_sig *sig = hm_lookup_attached_sig(hm, "any", e->as.member.name);
      if (ny_sig_allows_zero_arg_property(sig))
        return hm_sig_return_type(hm, sig, self_name);
    }
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return hm_name(hm, "integer");
    return hm_any_fallback(hm, e, "hm-dynamic-member",
                           "member target is dynamic");
  case NY_E_INDEX: {
    ny_hm_type_t *target =
        hm_prune(hm_infer_expr(hm, env, e->as.index.target, self_name));
    ny_hm_type_t *index_t =
        hm_infer_expr(hm, env, e->as.index.start, self_name);
    (void)hm_infer_expr(hm, env, e->as.index.stop, self_name);
    (void)hm_infer_expr(hm, env, e->as.index.step, self_name);
    if (!target)
      return hm_any(hm);
    if (hm_type_is_unbound_var(target)) {
      ny_hm_type_t *elem = hm_var(hm);
      if (hm_type_is_unbound_var(index_t))
        hm_unify(hm, index_t, hm_name(hm, "int"),
                 e->as.index.start ? e->as.index.start->tok : e->tok,
                 "index expression");
      hm_unify(hm, target, hm_unary(hm, NY_HM_INDEXABLE, elem), e->tok,
               "index target");
      return elem;
    }
    if (target->kind == NY_HM_DICT) {
      if (index_t && target->a)
        hm_unify(hm, target->a, index_t,
                 e->as.index.start ? e->as.index.start->tok : e->tok,
                 "dict index");
      return target->b ? target->b : hm_any(hm);
    }
    bool integer_index =
        target->kind == NY_HM_INDEXABLE || target->kind == NY_HM_LIST ||
        target->kind == NY_HM_TUPLE || target->kind == NY_HM_SET ||
        hm_is_name(target, "str") || hm_is_name(target, "bytes") ||
        hm_is_name(target, "range");
    if (integer_index) {
      if (hm_type_is_unbound_var(index_t))
        hm_unify(hm, index_t, hm_name(hm, "int"),
                 e->as.index.start ? e->as.index.start->tok : e->tok,
                 "index expression");
      else if (!hm_is_any(index_t) && (!hm || hm->dynamic_lambda_depth == 0))
        hm_unify(hm, hm_name(hm, "int"), index_t,
                 e->as.index.start ? e->as.index.start->tok : e->tok,
                 "index expression");
    }
    if (target->kind == NY_HM_INDEXABLE)
      return target->a ? target->a : hm_any(hm);
    if (target->kind == NY_HM_LIST || target->kind == NY_HM_TUPLE ||
        target->kind == NY_HM_SET)
      return target->a ? target->a : hm_any(hm);
    if (hm_is_name(target, "str"))
      return hm_name(hm, "str");
    if (hm_is_name(target, "bytes"))
      return hm_name(hm, "int");
    if (!hm_is_any(target))
      hm_unify_indexable(hm, hm_unary(hm, NY_HM_INDEXABLE, hm_any(hm)), target,
                         e->tok, "index target");
    return hm_any_fallback(hm, e, "hm-index-dynamic-result",
                           "index result is dynamic");
  }
  case NY_E_BINARY:
    return hm_infer_binary(hm, env, e, self_name);
  case NY_E_LOGICAL:
    (void)hm_infer_expr(hm, env, e->as.logical.left, self_name);
    (void)hm_infer_expr(hm, env, e->as.logical.right, self_name);
    return hm_name(hm, "bool");
  case NY_E_UNARY:
    if (e->as.unary.op && strcmp(e->as.unary.op, "async") == 0) {
      (void)hm_infer_expr(hm, env, e->as.unary.right, self_name);
      return hm_name(hm, "handle");
    }
    if (e->as.unary.op && strcmp(e->as.unary.op, "await") == 0) {
      (void)hm_infer_expr(hm, env, e->as.unary.right, self_name);
      return hm_any(hm);
    }
    if (e->as.unary.op && strcmp(e->as.unary.op, "!") == 0) {
      (void)hm_infer_expr(hm, env, e->as.unary.right, self_name);
      return hm_name(hm, "bool");
    }
    return hm_infer_expr(hm, env, e->as.unary.right, self_name);
  case NY_E_TERNARY: {
    (void)hm_infer_expr(hm, env, e->as.ternary.cond, self_name);
    ny_hm_type_t *a =
        hm_infer_expr(hm, env, e->as.ternary.true_expr, self_name);
    ny_hm_type_t *b =
        hm_infer_expr(hm, env, e->as.ternary.false_expr, self_name);
    return hm_merge(hm, a, b);
  }
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        (void)hm_infer_expr(hm, env, e->as.fstring.parts.data[i].as.e,
                            self_name);
    }
    return hm_name(hm, "str");
  case NY_E_PTR_TYPE:
    return hm_name(hm, "ptr");
  case NY_E_DEREF: {
    if (e->as.deref.target) {
      ny_hm_type_t *t = hm_infer_expr(hm, env, e->as.deref.target, self_name);
      if (t && t->name && t->name[0] == '*')
        return hm_name(hm, t->name + 1);
    }
    return hm_name(hm, "int");
  }
  case NY_E_SIZEOF:
    return hm_name(hm, "int");
  case NY_E_LAMBDA:
  case NY_E_FN:
    return hm_infer_lambda(hm, env, e, self_name);
  case NY_E_COMPTIME:
    return hm_infer_comptime_expr(hm, env, e, self_name);
  case NY_E_MATCH:
  case NY_E_TRY:
  case NY_E_ASM:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  default:
    return hm_any_fallback(hm, e, "hm-unsupported-expr",
                           "unsupported expression form");
  }
}

static void hm_infer_stmt_list_mode(ny_hm_state_t *hm, ny_hm_env_list *env,
                                    ny_stmt_list *body, const char *self_name,
                                    ny_hm_type_t *ret, bool allow_tail_expr) {
  for (size_t i = 0; body && i < body->len; ++i) {
    bool child_tail = allow_tail_expr && i + 1 == body->len;
    hm_infer_stmt_mode(hm, env, body->data[i], self_name, ret, child_tail);
  }
}

static void hm_infer_stmt_mode(ny_hm_state_t *hm, ny_hm_env_list *env,
                               stmt_t *s, const char *self_name,
                               ny_hm_type_t *ret, bool allow_tail_expr) {
  if (!hm || !s || !tp_stmt_in_scope(hm->ctx, s))
    return;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const char *decl =
          i < s->as.var.types.len ? s->as.var.types.data[i] : NULL;
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      bool explicit_dynamic = hm_type_name_allows_dynamic_literal(decl);
      ny_hm_type_t *it = init ? hm_infer_expr_dynamic_literal_ok(
                                    hm, env, init, self_name, explicit_dynamic)
                              : hm_var(hm);
      ny_hm_type_t *final_t =
          decl && *decl ? hm_parse_type_name(hm, decl, self_name) : it;
      if (decl && *decl)
        hm_unify(hm, final_t, it, init ? init->tok : s->tok,
                 "variable declaration");
      hm_env_set(env, name, final_t);
    }
    break;
  case NY_S_EXPR:
    if (ret && allow_tail_expr)
      hm_unify(hm, ret, hm_infer_expr(hm, env, s->as.expr.expr, self_name),
               s->as.expr.expr ? s->as.expr.expr->tok : s->tok,
               "implicit return value");
    else
      (void)hm_infer_expr(hm, env, s->as.expr.expr, self_name);
    break;
  case NY_S_RETURN:
    if (ret)
      hm_unify(hm, ret,
               s->as.ret.value
                   ? hm_infer_expr(hm, env, s->as.ret.value, self_name)
                   : hm_name(hm, "nil"),
               s->as.ret.value ? s->as.ret.value->tok : s->tok, "return value");
    break;
  case NY_S_IF:
    hm_infer_stmt_mode(hm, env, s->as.iff.init, self_name, ret, false);
    (void)hm_infer_expr(hm, env, s->as.iff.test, self_name);
    hm_infer_stmt_mode(hm, env, s->as.iff.conseq, self_name, ret,
                       allow_tail_expr);
    hm_infer_stmt_mode(hm, env, s->as.iff.alt, self_name, ret, allow_tail_expr);
    break;
  case NY_S_BLOCK:
    hm_infer_stmt_list_mode(hm, env, &s->as.block.body, self_name, ret,
                            allow_tail_expr);
    break;
  case NY_S_WHILE:
    hm_infer_stmt_mode(hm, env, s->as.whl.init, self_name, ret, false);
    (void)hm_infer_expr(hm, env, s->as.whl.test, self_name);
    hm_infer_stmt_mode(hm, env, s->as.whl.body, self_name, ret, false);
    hm_infer_stmt_mode(hm, env, s->as.whl.update, self_name, ret, false);
    break;
  case NY_S_FOR: {
    hm_infer_stmt_mode(hm, env, s->as.fr.init, self_name, ret, false);
    ny_hm_type_t *iter_t = hm_infer_expr(hm, env, s->as.fr.iterable, self_name);
    if (s->as.fr.iter_var) {
      iter_t = hm_prune(iter_t);
      ny_hm_type_t *item = hm_any(hm);
      if (iter_t && (iter_t->kind == NY_HM_LIST ||
                     iter_t->kind == NY_HM_TUPLE || iter_t->kind == NY_HM_SET))
        item = s->as.fr.iter_by_index ? hm_name(hm, "int") : iter_t->a;
      hm_env_set(env, s->as.fr.iter_var, item);
    }
    if (s->as.fr.iter_index_var)
      hm_env_set(env, s->as.fr.iter_index_var, hm_name(hm, "int"));
    (void)hm_infer_expr(hm, env, s->as.fr.cond, self_name);
    hm_infer_stmt_mode(hm, env, s->as.fr.body, self_name, ret, false);
    hm_infer_stmt_mode(hm, env, s->as.fr.update, self_name, ret, false);
    break;
  }
  case NY_S_TRY:
    hm_infer_stmt(hm, env, s->as.tr.body, self_name, NULL);
    hm_infer_stmt(hm, env, s->as.tr.handler, self_name, NULL);
    break;
  case NY_S_GUARD:
    hm_env_set(
        env, s->as.guard.name,
        hm_unary(hm, NY_HM_PTR,
                 hm_parse_type_name(hm, s->as.guard.type_name, self_name)));
    (void)hm_infer_expr_dynamic_literal_ok(hm, env, s->as.guard.value,
                                           self_name, true);
    hm_infer_stmt_mode(hm, env, s->as.guard.fallback, self_name, ret,
                       allow_tail_expr);
    break;
  case NY_S_MATCH: {
    ny_hm_type_t *subject_t =
        hm_infer_expr(hm, env, s->as.match.test, self_name);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      ny_hm_env_list arm_env;
      hm_env_clone(&arm_env, env);
      if (arm->patterns.len == 1)
        hm_refine_match_pattern(hm, &arm_env, subject_t, arm->patterns.data[0],
                                self_name);
      (void)hm_infer_expr(hm, &arm_env, arm->guard, self_name);
      hm_infer_stmt_mode(hm, &arm_env, arm->conseq, self_name, ret,
                         allow_tail_expr);
      vec_free(&arm_env);
    }
    hm_infer_stmt_mode(hm, env, s->as.match.default_conseq, self_name, ret,
                       allow_tail_expr);
  } break;
  case NY_S_MODULE:
    hm_infer_stmt_list_mode(hm, env, &s->as.module.body, s->as.module.name, ret,
                            allow_tail_expr);
    break;
  default:
    break;
  }
}

static void hm_infer_stmt(ny_hm_state_t *hm, ny_hm_env_list *env, stmt_t *s,
                          const char *self_name, ny_hm_type_t *ret) {
  hm_infer_stmt_mode(hm, env, s, self_name, ret, true);
}

static ny_hm_type_t *hm_make_func_type(ny_hm_state_t *hm, stmt_t *fn,
                                       const char *owner) {
  ny_hm_type_t *ft = hm_fn_type(hm);
  if (!fn || fn->kind != NY_S_FUNC)
    return ft;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    param_t *p = &fn->as.fn.params.data[i];
    hm_args_push(hm, ft,
                 p->type ? hm_parse_type_name(hm, p->type, owner) : hm_var(hm));
  }
  ft->a = fn->as.fn.return_type
              ? hm_parse_type_name(hm, fn->as.fn.return_type, owner)
              : hm_var(hm);
  return ft;
}

static void hm_collect_function_schemes(ny_hm_state_t *hm) {
  for (size_t i = 0; hm && hm->ctx && i < hm->ctx->funcs.len; ++i) {
    ny_tp_func_fact_t *fact = &hm->ctx->funcs.data[i];
    if (!fact->stmt || fact->stmt->kind != NY_S_FUNC)
      continue;
    ny_hm_scheme_t s = {.name = fact->name,
                        .owner = fact->owner,
                        .stmt = fact->stmt,
                        .type = hm_make_func_type(hm, fact->stmt, fact->owner)};
    s.full_name = hm_full_name(fact->owner, fact->name);
    vec_push(&hm->schemes, s);
  }
}

static void hm_infer_function_scheme(ny_hm_state_t *hm,
                                     ny_hm_scheme_t *scheme) {
  if (!hm || !scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC)
    return;
  stmt_t *fn = scheme->stmt;
  ny_hm_type_t *ft = hm_prune(scheme->type);
  ny_hm_env_list env;
  vec_init(&env);
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    param_t *p = &fn->as.fn.params.data[i];
    if (i < ft->args.len)
      hm_env_set(&env, p->name, ft->args.data[i]);
    if (p->def && i < ft->args.len)
      hm_unify(hm, ft->args.data[i],
               hm_infer_expr(hm, &env, p->def, scheme->owner), p->def->tok,
               "default parameter");
  }
  if (fn->as.fn.body && fn->as.fn.body->kind == NY_S_EXPR) {
    hm_unify(
        hm, ft->a,
        hm_infer_expr(hm, &env, fn->as.fn.body->as.expr.expr, scheme->owner),
        fn->as.fn.body->as.expr.expr ? fn->as.fn.body->as.expr.expr->tok
                                     : fn->as.fn.body->tok,
        "implicit return value");
  } else {
    hm_infer_stmt(hm, &env, fn->as.fn.body, scheme->owner, ft->a);
  }
  vec_free(&env);
}

static void hm_run(ny_hm_state_t *hm, ny_tp_ctx_t *ctx) {
  memset(hm, 0, sizeof(*hm));
  hm->ctx = ctx;
  vec_init(&hm->schemes);
  hm_collect_function_schemes(hm);
  for (size_t i = 0; i < hm->schemes.len; ++i)
    hm_infer_function_scheme(hm, &hm->schemes.data[i]);
  ny_hm_env_list env;
  vec_init(&env);
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i) {
    stmt_t *s = ctx->prog->body.data[i];
    if (s && s->kind != NY_S_FUNC && s->kind != NY_S_IMPL &&
        s->kind != NY_S_STRUCT && s->kind != NY_S_LAYOUT)
      hm_infer_stmt(hm, &env, s, NULL, NULL);
  }
  vec_free(&env);
}

static void hm_dispose(ny_hm_state_t *hm) {
  if (!hm)
    return;
  for (size_t i = 0; i < hm->schemes.len; ++i)
    free(hm->schemes.data[i].full_name);
  vec_free(&hm->schemes);
  arena_free(&hm->arena);
  memset(hm, 0, sizeof(*hm));
}

static void hm_emit_schemes_json(ny_tp_json_t *j, ny_hm_state_t *hm) {
  tp_append(j, "[");
  for (size_t i = 0; hm && i < hm->schemes.len; ++i) {
    ny_hm_scheme_t *s = &hm->schemes.data[i];
    char *ts = hm_type_string(s->type);
    if (i)
      tp_append(j, ",");
    tp_append(j, "{\"name\":");
    tp_json_str(j, s->name ? s->name : "");
    tp_append(j, ",\"owner\":");
    tp_json_str(j, s->owner ? s->owner : "");
    tp_append(j, ",\"full_name\":");
    tp_json_str(j, s->full_name ? s->full_name : "");
    tp_append(j, ",\"scheme\":");
    tp_json_str(j, ts ? ts : "any");
    tp_append(j, "}");
    free(ts);
  }
  tp_append(j, "]");
}
