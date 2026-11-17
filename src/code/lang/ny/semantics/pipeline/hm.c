/*
 * HM type checker: constraint-based Hindley-Milner inference engine
 * with arena allocation, unification, and type-variable management.
 */
#include "code/lang/ny/parser/index_expr.h"
#include "base/debug.h"
#include "base/loader.h"
#include "code/lang/ny/semantics/ownership.h"

#include "base/strbuf.h"
static char *hm_proof_compact(const char *text);
static char *hm_proof_subst_args(ny_hm_state_t *hm, stmt_t *fn,
                                 const char *type_text, ny_call_arg_list *args,
                                 size_t arg_offset, size_t limit);

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
  if (getenv("NY_DEBUG_STRICT"))
    fprintf(stderr, "[strict] stdlib tok: source=%s file=%s\n",
            hm->ctx->source_name ? hm->ctx->source_name : "(null)",
            tok.filename ? tok.filename : "(null)");
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
  if (strcmp(code, "hm-unsupported-expr") == 0)
    return "hm-strict-unsupported-expression";
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
         (name && (strcmp(name, "number") == 0 || strcmp(name, "float") == 0 ||
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

static const char *hm_lookup_refine_base_in_stmts(ny_stmt_list *stmts,
                                                  const char *name) {
  if (!stmts || !name)
    return NULL;
  for (size_t i = 0; i < stmts->len; ++i) {
    stmt_t *s = stmts->data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_REFINE && s->as.refine.name &&
        strcmp(s->as.refine.name, name) == 0)
      return s->as.refine.base_type;
    if (s->kind == NY_S_MODULE) {
      const char *b = hm_lookup_refine_base_in_stmts(&s->as.module.body, name);
      if (b)
        return b;
    }
  }
  return NULL;
}

static const char *hm_lookup_refine_base(ny_hm_state_t *hm, const char *name) {
  if (!hm || !name)
    return NULL;
  if (hm->ctx && hm->ctx->cg) {
    const char *b = ny_refine_base_type(hm->ctx->cg, name);
    if (b && strcmp(b, name) != 0)
      return b;
    /*
     * ny_refine_base_type already walked cg->prog plus every extra program.
     * When the HM context carries that same program tree, the separate walk
     * below repeats identical full-program scans for every resolved name, so
     * a cg miss settles this lookup directly.
     */
    if (!hm->ctx->prog || hm->ctx->prog == hm->ctx->cg->prog)
      return NULL;
  }
  if (hm->ctx && hm->ctx->prog) {
    const char *b = hm_lookup_refine_base_in_stmts(&hm->ctx->prog->body, name);
    if (b)
      return b;
  }
  return NULL;
}

static const char *hm_resolve_refine_root_base(ny_hm_state_t *hm,
                                               const char *name) {
  if (!name)
    return NULL;
  const char *cur = name;
  for (int depth = 0; depth < 16 && cur; depth++) {
    const char *next = hm_lookup_refine_base(hm, cur);
    if (!next || strcmp(next, cur) == 0)
      break;
    cur = next;
  }
  return (cur != name) ? cur : NULL;
}

static bool hm_type_known_numeric(ny_hm_state_t *hm, ny_hm_type_t *t,
                                  const char **canonical_out) {
  t = hm_prune(t);
  if (!t || t->kind != NY_HM_NAME || !t->name)
    return false;
  const char *name = t->name;
  const char *base = hm_resolve_refine_root_base(hm, name);
  if (base)
    name = base;
  if (tp_is_float_type(name) || strcmp(name, "float") == 0) {
    if (canonical_out)
      *canonical_out = tp_is_float_type(name) ? name : "f64";
    return true;
  }
  if (tp_is_int_type(name) || strcmp(name, "integer") == 0) {
    if (canonical_out)
      *canonical_out = tp_is_int_type(name) ? name : "int";
    return true;
  }
  if (strcmp(name, "number") == 0) {
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

static bool hm_name_compatible(ny_hm_state_t *hm, const char *want,
                               const char *got);

static bool hm_generic_base_same(const char *want, const char *got) {
  return strchr(want ? want : "", '<') && strchr(got ? got : "", '<') &&
         ny_generic_type_base_is(want, got);
}

static bool hm_generic_name_compatible(ny_hm_state_t *hm, const char *want,
                                       const char *got);
static char *hm_merge_generic_name_owned(ny_hm_state_t *hm, const char *want,
                                         const char *got);

static bool hm_generic_arg_is_dynamic(const char *name) {
  const char *leaf = ny_generic_type_leaf(name);
  if (!leaf || !*leaf)
    return true;
  return strcmp(leaf, "any") == 0 || strcmp(leaf, "empty") == 0;
}

static bool hm_generic_arg_is_explicit_any(const char *name) {
  const char *leaf = ny_generic_type_leaf(name);
  return leaf && strcmp(leaf, "any") == 0;
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

static char *hm_merge_generic_arg_owned(ny_hm_state_t *hm, const char *want,
                                        const char *got) {
  /*
   * An explicit `any` in the expected generic contract is a dynamic slot,
   * not an inference variable.  Preserve it while checking a concrete
   * payload so one return branch cannot narrow (for example)
   * Result<int, any> to Result<int, str> and make a later integer error fail.
   * `empty`, by contrast, remains an inference placeholder and may specialize.
   */
  if (hm_generic_arg_is_explicit_any(want))
    return ny_strdup(want ? want : "any");
  if (hm_generic_arg_is_dynamic(want))
    return ny_strdup(got ? got : "any");
  if (hm_generic_arg_is_dynamic(got))
    return ny_strdup(want ? want : "any");
  if (hm_generic_base_same(want, got)) {
    char *merged = hm_merge_generic_name_owned(hm, want, got);
    if (merged)
      return merged;
  }
  return ny_strdup(want ? want : (got ? got : "any"));
}

static char *hm_merge_generic_name_owned(ny_hm_state_t *hm, const char *want,
                                         const char *got) {
  if (!hm_generic_base_same(want, got) ||
      !hm_generic_name_compatible(hm, want, got))
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
    char *arg = hm_merge_generic_arg_owned(hm, wa, ga);
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

static bool hm_generic_name_compatible(ny_hm_state_t *hm, const char *want,
                                       const char *got) {
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
    bool ok = hm_name_compatible(hm, wa, ga);
    free(wa);
    free(ga);
    if (!ok)
      return false;
  }
  return true;
}

static bool hm_name_compatible(ny_hm_state_t *hm, const char *want,
                               const char *got) {
  if (!want || !got)
    return true;
  if (strcmp(want, got) == 0 || strcmp(want, "any") == 0 ||
      strcmp(got, "any") == 0)
    return true;
  const char *r_want = hm_resolve_refine_root_base(hm, want);
  const char *r_got = hm_resolve_refine_root_base(hm, got);
  if (r_want || r_got) {
    if (hm_name_compatible(hm, r_want ? r_want : want, r_got ? r_got : got))
      return true;
  }
  /*
   * A declared user type indexed by a compile-time value keeps its own bound
   * identity, but shares the declared base ABI. Permit the base value type at
   * a call boundary; the legacy value-index checker separately rejects
   * out-of-range integer literals. Two differently indexed names remain
   * distinct and do not take this erasure path.
   */
  char indexed_base[512];
  if (hm && hm->ctx && hm->ctx->cg &&
      (!strchr(got, '<') || strncmp(got, "Fin<", 4) == 0) &&
      ny_indexed_declared_type_base(hm->ctx->cg, want, indexed_base,
                                    sizeof(indexed_base))) {
    const char *root = hm_resolve_refine_root_base(hm, indexed_base);
    if (hm_name_compatible(hm, root ? root : indexed_base, got))
      return true;
  }
  if (hm && hm->ctx && hm->ctx->cg && !strchr(want, '<') &&
      ny_indexed_declared_type_base(hm->ctx->cg, got, indexed_base,
                                    sizeof(indexed_base))) {
    const char *root = hm_resolve_refine_root_base(hm, indexed_base);
    if (hm_name_compatible(hm, want, root ? root : indexed_base))
      return true;
  }
  /*
   * Fin<N> has the integer ABI.  Keep the named bound in the environment
   * for declaration checking, but allow it wherever an ordinary integer is
   * required.
   */
  if ((strncmp(want, "Fin<", 4) == 0 && tp_is_int_type(got)) ||
      (strncmp(got, "Fin<", 4) == 0 && tp_is_int_type(want)))
    return true;
  /*
   * A bare generic name ("Result") is the fully dynamic parameterization of
   * the same generic ("Result<int, int>"); it accepts every instance, the
   * same way an any type argument does.
   */
  if (strchr(want, '<') && !strchr(got, '<')) {
    char *wb = hm_generic_base_owned(want);
    bool ok = wb && strcmp(wb, got) == 0;
    free(wb);
    if (ok)
      return true;
  }
  if (!strchr(want, '<') && strchr(got, '<')) {
    char *gb = hm_generic_base_owned(got);
    bool ok = gb && strcmp(gb, want) == 0;
    free(gb);
    if (ok)
      return true;
  }
  if (hm_generic_name_compatible(hm, want, got))
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
  if ((strcmp(want, "ptr") == 0 && tp_is_int_type(got)) ||
      (strcmp(got, "ptr") == 0 && tp_is_int_type(want)))
    return true;
  return false;
}

static bool hm_fin_literal_bound(const char *type, int64_t *out) {
  if (!type || !out || strncmp(type, "Fin<", 4) != 0)
    return false;
  const char *end = strchr(type + 4, '>');
  if (!end || end == type + 4 || end[1] != '\0')
    return false;
  char *parsed = NULL;
  long long bound = strtoll(type + 4, &parsed, 10);
  if (parsed != end || bound <= 0)
    return false;
  *out = (int64_t)bound;
  return true;
}

static void hm_check_fin_initializer(ny_hm_state_t *hm, const char *decl,
                                     const expr_t *init, token_t tok) {
  int64_t bound = 0;
  if (!hm_fin_literal_bound(decl, &bound) || !init ||
      init->kind != NY_E_LITERAL || init->as.literal.kind != NY_LIT_INT)
    return;
  int64_t value = init->as.literal.as.i;
  if (value >= 0 && value < bound)
    return;
  tp_add_diag_ex(hm->ctx, tok, "hm", "hm-fin-literal-range",
                 "variable declaration", decl, "int", "integer literal",
                 "Fin<N> values must be in the half-open interval [0, N)",
                 "use a literal in range or a checked Fin constructor",
                 "Fin<%" PRId64 "> requires 0 <= value < %" PRId64
                 ", got %" PRId64,
                 bound, bound, value);
}

static stmt_t *hm_lookup_refine_stmt_in_stmts(ny_stmt_list *stmts,
                                              const char *name) {
  if (!stmts || !name)
    return NULL;
  for (size_t i = 0; i < stmts->len; ++i) {
    stmt_t *s = stmts->data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_REFINE && s->as.refine.name &&
        strcmp(s->as.refine.name, name) == 0)
      return s;
    if (s->kind == NY_S_MODULE) {
      stmt_t *res = hm_lookup_refine_stmt_in_stmts(&s->as.module.body, name);
      if (res)
        return res;
    }
  }
  return NULL;
}

static stmt_t *hm_lookup_refine_stmt(ny_hm_state_t *hm, const char *name) {
  if (!hm || !name)
    return NULL;
  if (hm->ctx && hm->ctx->cg) {
    refine_def_t *r = ny_lookup_refine(hm->ctx->cg, name);
    if (r && r->stmt)
      return r->stmt;
    /*
     * Same coverage rule as hm_lookup_refine_base: ny_lookup_refine scans
     * the cg program set, so repeating the walk per HM program duplicates
     * it unless the contexts hold different trees.
     */
    if (!hm->ctx->prog || hm->ctx->prog == hm->ctx->cg->prog)
      return NULL;
  }
  if (hm->ctx && hm->ctx->prog) {
    stmt_t *s = hm_lookup_refine_stmt_in_stmts(&hm->ctx->prog->body, name);
    if (s)
      return s;
  }
  return NULL;
}

static void hm_check_refinement_initializer(ny_hm_state_t *hm, const char *decl,
                                            const expr_t *init, token_t tok) {
  if (!hm || !decl || !*decl || !init)
    return;
  codegen_t *cg = hm->ctx ? hm->ctx->cg : NULL;
  refine_def_t *ref = NULL;
  if (cg)
    ref = ny_lookup_refine(cg, decl);
  if (ref) {
    int res = 0;
    if (ny_refine_eval_predicate(cg, NULL, 0, ref, (expr_t *)init, &res)) {
      if (res == 0) {
        tp_add_diag_ex(hm->ctx, tok, "hm", "hm-refinement-obligation-failed",
                       "variable declaration", decl, "refinement satisfaction",
                       "value",
                       "refinement predicates must hold at compile time for "
                       "constant initializers",
                       "provide a value satisfying the refinement predicate",
                       "refinement obligation failed: value does not satisfy "
                       "'%s' refinement predicate",
                       decl);
      }
    }
    return;
  }
  stmt_t *r_stmt = hm_lookup_refine_stmt(hm, decl);
  if (!r_stmt)
    return;
  if (r_stmt->as.refine.base_type)
    hm_check_refinement_initializer(hm, r_stmt->as.refine.base_type, init, tok);
  if (!r_stmt->as.refine.predicate || !r_stmt->as.refine.param)
    return;
  arena_t inst_arena = {0};
  expr_t *inst = ny_expr_subst(&inst_arena, r_stmt->as.refine.predicate,
                               r_stmt->as.refine.param, (expr_t *)init);
  if (!inst) {
    arena_free(&inst_arena);
    return;
  }
  ny_ct_fast_val_t fval = ny_ct_fast_none();
  if (ny_try_eval_comptime_expr_fast(cg, inst, &fval, 0)) {
    if (fval.kind == NY_CT_FAST_BOOL && !fval.b) {
      tp_add_diag_ex(hm->ctx, tok, "hm", "hm-refinement-obligation-failed",
                     "variable declaration", decl, "refinement satisfaction",
                     "value",
                     "refinement predicates must hold at compile time for "
                     "constant initializers",
                     "provide a value satisfying the refinement predicate",
                     "refinement obligation failed: value does not satisfy "
                     "'%s' refinement predicate",
                     decl);
    }
  }
  ny_ct_fast_val_free(&fval);
  arena_free(&inst_arena);
}

static char *hm_type_string(ny_hm_type_t *t);

static void hm_check_refinement_return(ny_hm_state_t *hm,
                                       const ny_hm_type_t *expected,
                                       ny_hm_type_t *actual,
                                       const expr_t *value, token_t tok) {
  if (!hm || !expected || !value)
    return;
  char *decl = hm_type_string((ny_hm_type_t *)expected);
  if (!decl || !*decl || !hm_lookup_refine_stmt(hm, decl)) {
    free(decl);
    return;
  }
  char *got = hm_type_string(actual);
  if (got && strcmp(got, decl) == 0) {
    free(got);
    free(decl);
    return;
  }
  free(got);
  int result = 0;
  refine_def_t *ref =
      hm->ctx && hm->ctx->cg ? ny_lookup_refine(hm->ctx->cg, decl) : NULL;
  bool proved = ref &&
                ny_refine_eval_predicate(hm->ctx->cg, NULL, 0, ref,
                                         (expr_t *)value, &result) &&
                result != 0;
  if (!proved) {
    tp_add_diag_ex(
        hm->ctx, tok, "hm", "hm-refinement-unproved", "refinement value", decl,
        "refinement predicate", "value",
        "a value must be proved to satisfy its declared refinement",
        "use a value carrying the same refinement type or establish its "
        "predicate at compile time",
        "refinement obligation is not proved for type '%s'", decl);
  }
  free(decl);
}

static void hm_check_indexed_initializer(ny_hm_state_t *hm, const char *decl,
                                         const expr_t *init, token_t tok) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !decl || !*decl || !init)
    return;
  codegen_t *cg = hm->ctx->cg;
  char base[512];
  if (!ny_indexed_declared_type_base(cg, decl, base, sizeof(base)))
    return;
  hm_check_refinement_initializer(hm, base, init, tok);
  if (init->kind != NY_E_LITERAL || init->as.literal.kind != NY_LIT_INT)
    return;
  char ctor[64] = {0};
  int64_t bound = 0;
  if (!ny_indexed_resolve_bound(cg, NULL, 0, decl, ctor, sizeof(ctor),
                                &bound) ||
      strcmp(ctor, "Fin") == 0)
    return;
  int64_t value = init->as.literal.as.i;
  if (value >= 0 && value < bound)
    return;
  tp_add_diag_ex(hm->ctx, tok, "hm", "hm-indexed-literal-range",
                 "value-indexed argument", decl, "int", "integer literal",
                 "user-indexed integer literals must satisfy 0 <= value < N",
                 "use a literal within the declared index bound",
                 "value-indexed bound requires 0 <= value < %" PRId64
                 ", got %" PRId64,
                 bound, value);
}

/*
 * Builtin kind/ primitive names keep their exact-kind unification; any
 * other declared name is a nominal user/impl type.
 */
static bool hm_name_is_builtin_kind(const char *name) {
  if (!name)
    return false;
  static const char *builtins[] = {"list", "tuple", "set",   "dict",
                                   "str",  "ptr",   "fnptr", "any",
                                   "nil",  "proof", "empty"};
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
    if (strcmp(name, builtins[i]) == 0)
      return true;
  return tp_is_int_type(name) || hm_numeric_name(name) ||
         strcmp(name, "bool") == 0 || strcmp(name, "range") == 0 ||
         strcmp(name, "bytes") == 0;
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
         (strcmp(name, "str") == 0 && kind == NY_HM_PTR) ||
         (hm_is_callable_name(name) && kind == NY_HM_FN);
}

static char *hm_type_string(ny_hm_type_t *t);

static void hm_strict_condition_check(ny_hm_state_t *hm, expr_t *expr,
                                      ny_hm_type_t *type) {
  if (!hm || !expr || !hm_strict_types_enabled(hm, expr->tok))
    return;
  char *got = hm_type_string(type);
  if (!got || strcmp(got, "bool") == 0 || strcmp(got, "any") == 0) {
    free(got);
    return;
  }
  tp_add_diag_ex(hm->ctx, expr->tok, "hm", "hm-strict-condition",
                 "strict condition", "bool", got, "condition",
                 "conditions are not implicitly truthy in strict mode",
                 "compare the value explicitly or convert it to bool",
                 "strict condition must be bool, got %s", got);
  free(got);
}

static void hm_module_unknown_member_diag(ny_hm_state_t *hm, token_t tok,
                                          const char *module_path,
                                          const char *member) {
  if (!hm || !module_path || !*module_path || !member || !*member)
    return;
  tp_add_diag_ex(
      hm->ctx, tok, "hm", "hm-module-unknown-member", "module member", member,
      module_path, "member",
      "the module alias resolved, but the requested name is not exported",
      "export the member from the module or use the module that defines it",
      "module '%s' has no exported member '%s'", module_path, member);
}

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
  if (ws && gs && strncmp(ws, "proof<", 6) == 0 &&
      strncmp(gs, "proof<", 6) == 0) {
    tp_add_diag_ex(hm ? hm->ctx : NULL, tok, "hm", "proof-proposition-mismatch",
                   context, ws, gs, NULL,
                   "proof propositions are structural; the received witness "
                   "establishes a different fact",
                   "provide evidence for the required proposition",
                   "proof proposition does not match: expected %s, got %s", ws,
                   gs);
    free(ws);
    free(gs);
    return;
  }
  const char *hint = hm_diag_hint_for(code, context, ws, gs);
  const char *fix = hm_diag_fix_for(code, context, ws, gs);
  if (ws && gs && hm_generic_base_same(ws, gs) && strcmp(ws, gs) != 0) {
    tp_add_diag_ex(hm ? hm->ctx : NULL, tok, "hm", "hm-generic-type-mismatch",
                   "generic type mismatch", ws, gs, "generic", hint, fix,
                   "type mismatch: expected '%s', got '%s'", ws, gs);
  } else if (ws && gs && strcmp(ws, "int") == 0 && strcmp(gs, "str") == 0) {
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
static ny_hm_type_t *hm_parse_type_name(ny_hm_state_t *hm, const char *raw,
                                        const char *self_name);

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
  if (hm_is_name(got, "seq") || hm_is_name(got, "sequence"))
    return hm_unify(hm, want->a, hm_any(hm), tok, context);
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
  /*
   * At the highest verbosity, stdlib default-argument constraints are
   * repetitive setup noise. Keep user constraints fully visible and report
   * the number summarized once the HM pass completes. --unify-trace remains
   * available at lower verbosity when the raw stdlib detail is desired.
   * Trivial same-scalar constraints (`int ~ int`) carry no diagnostic
   * value at trace tiers: count them and keep the trace for constraints
   * that actually say something (different shapes, any ~ T).
   */
  bool trace_type_detail = hm->trace_type_detail;
  bool routine_std_default =
      trace_type_detail && hm->trace_deep &&
      (tok.module_features & NY_MODULE_FEATURE_STDLIB) != 0 && context &&
      strcmp(context, "default parameter") == 0;
  if (routine_std_default)
    hm->debug_std_default_constraints_suppressed++;
  char *lhs_s = trace_type_detail ? hm_type_string(want) : NULL;
  char *rhs_s = trace_type_detail ? hm_type_string(got) : NULL;
  bool trivial = trace_type_detail && lhs_s && rhs_s &&
                 strcmp(lhs_s, rhs_s) == 0 &&
                 strcmp(context ? context : "", "default parameter") == 0;
  if (trivial)
    hm->debug_std_default_constraints_suppressed++;
  if (trace_type_detail && !routine_std_default && !trivial) {
    char detail[768];
    snprintf(detail, sizeof(detail), "%s~%s@%u:%u %s", lhs_s ? lhs_s : "?",
             rhs_s ? rhs_s : "?", tok.line, tok.col,
             context ? context : "unify");
    ny_dbg_type_detail("HM_CONSTRAINT", detail);
  }
  free(lhs_s);
  free(rhs_s);
  want = hm_prune(want);
  got = hm_prune(got);
  if (!want || !got || want == got || hm_is_any(want) || hm_is_any(got))
    return true;
  if (want->kind == NY_HM_VAR) {
    if (hm_occurs(want, got)) {
      hm_unify_diag(hm, tok, "hm-occurs-check", context, want, got);
      return false;
    }
    if (trace_type_detail && !routine_std_default) {
      char *rhs = hm_type_string(got);
      char detail[512];
      snprintf(detail, sizeof(detail), "bind 'a%d := %s (%s)", want->id,
               rhs ? rhs : "?", context ? context : "unification");
      ny_dbg_type_detail("HM_SUBST", detail);
      free(rhs);
    }
    want->instance = got;
    return true;
  }
  if (got->kind == NY_HM_VAR)
    return hm_unify(hm, got, want, tok, context);
  /*
   * A bare `proof` annotation preserves the proposition inferred from
   * prove(...); indexed proof requirements remain exact below.
   */
  if (want->kind == NY_HM_NAME && got->kind == NY_HM_NAME &&
      strcmp(want->name ? want->name : "", "proof") == 0 &&
      strncmp(got->name ? got->name : "", "proof<", 6) == 0)
    return true;
  /*
   * Name-aware proof propositions: two indexed proofs unify when their
   * canonical propositions match up to value-name renaming. Literal parts
   * must be identical; `name:X` vs `name:Y` is accepted here because the
   * range satisfaction is enforced at codegen (ny_proof_call_params_ok),
   * where the caller-side value ranges are available.
   */
  if (want->kind == NY_HM_NAME && got->kind == NY_HM_NAME &&
      strncmp(want->name ? want->name : "", "proof<", 6) == 0 &&
      strncmp(got->name ? got->name : "", "proof<", 6) == 0) {
    /*
     * Proof propositions match exactly (up to printer spacing).  Call sites
     * instantiate the callee's binders with the actual argument terms first
     * (hm_proof_subst_args), so renaming tolerance is no longer needed; it
     * let `proof<a < 10>` stand for `proof<c < 10>`.
     */
    char *w = hm_proof_compact(want->name), *g = hm_proof_compact(got->name);
    bool same = w && g && strcmp(w, g) == 0;
    free(w);
    free(g);
    if (same)
      return true;
    hm_unify_diag(hm, tok, "proof-proposition-mismatch", context, want, got);
    return false;
  }
  if (hm_is_name(got, "nil")) {
    if (want->kind == NY_HM_NAME && strcmp(want->name, "nil") == 0)
      return true;
    if (want->kind != NY_HM_NULLABLE && want->kind != NY_HM_PTR) {
      ny_hm_type_t *inner = hm_type_new(hm, want->kind);
      inner->id = want->id;
      inner->name = want->name;
      inner->a = want->a;
      inner->b = want->b;
      inner->args = want->args;
      want->kind = NY_HM_NULLABLE;
      want->a = inner;
      want->name = NULL;
    }
    return true;
  }
  if (hm_is_name(want, "nil")) {
    if (got->kind != NY_HM_NULLABLE && got->kind != NY_HM_PTR) {
      want->kind = NY_HM_NULLABLE;
      want->a = got;
      want->name = NULL;
    } else {
      want->kind = got->kind;
      want->a = got->a;
      want->b = got->b;
      want->args = got->args;
      want->name = got->name;
    }
    return true;
  }
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
  /*
   * A union on the received side unifies when either member fits: a
   * heterogeneous `[-1, buf]` element merged to int|str must satisfy the
   * int element slot recorded by an earlier return of the same function
   * (remote.ny expect: "expected int, got int|str").
   */
  if (got->kind == NY_HM_UNION && want->kind != NY_HM_UNION) {
    if (hm_unify(hm, want, got->a, tok, context) ||
        hm_unify(hm, want, got->b, tok, context))
      return true;
    hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
    return false;
  }
  /*
   * Mirror case: the recorded type is the union and the received value is
   * one member (return res after return [-1, ""] with a collapsed any
   * element: "expected int|str, got int").  The in-switch union case only
   * runs when both kinds match, so handle the mixed-kind pair here.
   */
  if (want->kind == NY_HM_UNION && got->kind != NY_HM_UNION) {
    if (hm_unify(hm, want->a, got, tok, context) ||
        hm_unify(hm, want->b, got, tok, context))
      return true;
    hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
    return false;
  }
  if (want->kind == NY_HM_NAME && got->kind == NY_HM_NAME) {
    if (hm_generic_base_same(want->name, got->name)) {
      if (!hm_generic_name_compatible(hm, want->name, got->name)) {
        hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
        return false;
      }
      char *merged = hm_merge_generic_name_owned(hm, want->name, got->name);
      if (merged) {
        want->name = hm_arena_strdup(hm, merged);
        free(merged);
      }
      return true;
    }
    if (hm_name_compatible(hm, want->name, got->name))
      return true;
    hm_unify_diag(hm, tok, "hm-type-mismatch", context, want, got);
    return false;
  }
  /*
   * A refinement over a structural type keeps a named HM representation,
   * while its base annotation (for example `list`) is structural. Accept the
   * refinement as a value of that base shape at input boundaries; the
   * reverse direction still requires a refinement proof.
   */
  if (got->kind == NY_HM_NAME && got->name) {
    const char *base = hm_resolve_refine_root_base(hm, got->name);
    if (base && strcmp(base, got->name) != 0) {
      ny_hm_type_t *base_type = hm_parse_type_name(hm, base, NULL);
      if (base_type && base_type->kind != NY_HM_NAME)
        return hm_unify(hm, want, base_type, tok, context);
    }
  }
  /*
   * Nominal impl types are runtime views over tagged dict literals
   * ({"__type": "name", ...}); unify a declared impl name with a dict
   * literal's dict<str, any> unless the name is a builtin kind with its own
   * exact rule (gf.ny GF2BVLinearSystem returns gf2bv_linear).
   */
  if (want->kind == NY_HM_NAME && got->kind == NY_HM_DICT &&
      !hm_name_is_builtin_kind(want->name))
    return true;
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
  out->parameters = t->parameters;
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

static ny_hm_env_entry_t *
hm_env_entry_ctx(ny_hm_env_list *env, const char *name, uint32_t syntax_ctx);

static void hm_env_init(ny_hm_state_t *hm, ny_hm_env_list *env) {
  *env = (ny_hm_env_list){0};
  env->generation = ++hm->next_env_generation;
  if (!env->generation)
    env->generation = ++hm->next_env_generation;
  env->lookup_cache = hm->env_lookup_cache;
}

static void hm_env_set_ctx(ny_hm_env_list *env, const char *name,
                           uint32_t syntax_ctx, ny_hm_type_t *type) {
  if (!env || !name || !*name)
    return;
  ny_hm_env_entry_t *e = hm_env_entry_ctx(env, name, syntax_ctx);
  if (e) {
    e->type = type;
    /*
     * A rebinding replaces the dependent-pair annotation as well.
     */
    e->sigma_decl = NULL;
    e->sigma_index_literal = false;
    e->sigma_index_value = 0;
    return;
  }
  ny_hm_env_entry_t new_entry = {
      .name = name, .syntax_ctx = syntax_ctx, .type = type};
  vec_push(env, new_entry);
}

static void hm_env_push_ctx(ny_hm_env_list *env, const char *name,
                            uint32_t syntax_ctx, ny_hm_type_t *type) {
  if (!env || !name || !*name)
    return;
  ny_hm_env_entry_t e = {.name = name, .syntax_ctx = syntax_ctx, .type = type};
  vec_push(env, e);
}

/*
 * Environments hold thousands of bindings and are searched for every
 * identifier, so compare a cached hash before the name. Zero means unhashed.
 */
static inline uint32_t hm_name_hash(const char *name) {
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    h = (h ^ *p) * 16777619u;
  return h | 1u;
}

static inline bool hm_env_entry_matches(ny_hm_env_entry_t *e, const char *name,
                                        uint32_t hash, uint32_t syntax_ctx) {
  if (e->syntax_ctx != syntax_ctx || !e->name)
    return false;
  if (!e->name_hash)
    e->name_hash = hm_name_hash(e->name);
  return e->name_hash == hash && strcmp(e->name, name) == 0;
}

static ny_hm_env_entry_t *
hm_env_entry_ctx(ny_hm_env_list *env, const char *name, uint32_t syntax_ctx) {
  if (!env || !name)
    return NULL;
  uint32_t name_hash = hm_name_hash(name);
  uint64_t cache_key = ((uintptr_t)env >> 4) ^ env->generation ^
                       ((uint64_t)name_hash << 32) ^ syntax_ctx;
  cache_key ^= cache_key >> 30;
  cache_key *= 0xbf58476d1ce4e5b9ull;
  cache_key ^= cache_key >> 27;
  size_t cache_slot = (cache_key ^ (cache_key >> 31)) & 127u;
  ny_hm_env_lookup_cache_entry_t *cached =
      env->lookup_cache ? &env->lookup_cache[cache_slot] : NULL;
  if (cached && cached->env == env && cached->generation == env->generation &&
      cached->name_hash == name_hash && cached->syntax_ctx == syntax_ctx &&
      cached->len <= env->len && cached->index <= cached->len) {
    if (cached->index == cached->len) {
      for (size_t i = env->len; i > cached->len; --i) {
        ny_hm_env_entry_t *appended = &env->data[i - 1u];
        if (hm_env_entry_matches(appended, name, cached->name_hash,
                                 syntax_ctx)) {
          cached->len = env->len;
          cached->index = i - 1u;
          return appended;
        }
      }
      cached->len = env->len;
      cached->index = env->len;
      return NULL;
    }
    ny_hm_env_entry_t *entry = &env->data[cached->index];
    if (hm_env_entry_matches(entry, name, name_hash, syntax_ctx)) {
      for (size_t i = env->len; i > cached->len; --i) {
        ny_hm_env_entry_t *appended = &env->data[i - 1u];
        if (hm_env_entry_matches(appended, name, name_hash, syntax_ctx)) {
          cached->len = env->len;
          cached->index = i - 1u;
          return appended;
        }
      }
      cached->len = env->len;
      return entry;
    }
  }
  for (size_t i = env->len; i > 0; --i) {
    ny_hm_env_entry_t *e = &env->data[i - 1u];
    if (hm_env_entry_matches(e, name, name_hash, syntax_ctx)) {
      if (cached) {
        *cached = (ny_hm_env_lookup_cache_entry_t){
            .env = env,
            .generation = env->generation,
            .name_hash = name_hash,
            .syntax_ctx = syntax_ctx,
            .len = env->len,
            .index = i - 1u,
        };
        return e;
      }
    }
  }
  if (cached) {
    *cached = (ny_hm_env_lookup_cache_entry_t){
        .env = env,
        .generation = env->generation,
        .name_hash = name_hash,
        .syntax_ctx = syntax_ctx,
        .len = env->len,
        .index = env->len,
    };
  }
  return NULL;
}

static ssize_t hm_env_entry_index_ctx(ny_hm_env_list *env, const char *name,
                                      uint32_t syntax_ctx) {
  if (!env || !name)
    return -1;
  ny_hm_env_entry_t *entry = hm_env_entry_ctx(env, name, syntax_ctx);
  return entry ? (ssize_t)(entry - env->data) : -1;
}

static bool hm_is_mutable_binding(ny_hm_state_t *hm, ny_hm_env_list *env,
                                  const char *name, uint32_t syntax_ctx,
                                  size_t max_env_idx) {
  if (!name)
    return false;
  ssize_t idx = hm_env_entry_index_ctx(env, name, syntax_ctx);
  if (idx >= 0 && (size_t)idx < max_env_idx) {
    return env->data[idx].mutable_value;
  }
  if (hm && hm->ctx && hm->ctx->cg) {
    binding *b = lookup_global(hm->ctx->cg, name);
    if (b && b->is_mut)
      return true;
  }
  if (hm && hm->ctx && hm->ctx->prog) {
    for (size_t i = 0; i < hm->ctx->prog->body.len; ++i) {
      stmt_t *s = hm->ctx->prog->body.data[i];
      if (s && s->kind == NY_S_VAR && s->as.var.is_mut) {
        for (size_t j = 0; j < s->as.var.names.len; ++j) {
          if (s->as.var.names.data[j] &&
              strcmp(s->as.var.names.data[j], name) == 0)
            return true;
        }
      }
    }
  }
  return false;
}

static void hm_env_set_value_facts(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   const char *name, uint32_t syntax_ctx,
                                   bool mutable_value,
                                   ny_sem_ownership_t ownership,
                                   bool preserve_alias) {
  ny_hm_env_entry_t *entry = hm_env_entry_ctx(env, name, syntax_ctx);
  if (!entry)
    return;
  if (!preserve_alias || entry->alias_class == 0)
    entry->alias_class = ++hm->next_alias_class;
  entry->mutable_value = mutable_value;
  entry->ownership = ownership;
}

static ny_hm_type_t *hm_parse_type_name(ny_hm_state_t *hm, const char *raw,
                                        const char *self_name);
static char *hm_type_string(ny_hm_type_t *t);

static ny_hm_type_t *hm_env_get_ctx(ny_hm_env_list *env, const char *name,
                                    uint32_t syntax_ctx) {
  ny_hm_env_entry_t *entry = hm_env_entry_ctx(env, name, syntax_ctx);
  return entry ? entry->type : NULL;
}

/*
 * True when a checked expression already has the parameter's refined or
 * value-indexed type (for example a dependent-return call result).  Such a
 * value carries its proof; re-evaluating it as a fresh constant would fail
 * closed even though the declaration proved it.
 */
static bool hm_type_is_refined_instance(ny_hm_state_t *hm, ny_hm_type_t *arg_t,
                                        const char *param) {
  if (!hm || !arg_t || !param)
    return false;
  char *actual = hm_type_string(arg_t);
  if (!actual)
    return false;
  bool matches = hm_lookup_refine_stmt(hm, actual) != NULL;
  if (!matches) {
    char *actual_base = hm_generic_base_owned(actual);
    char *param_base = hm_generic_base_owned(param);
    /*
     * Only a value-indexed refinement instance (`Vec<2>` against `Vec<2>`)
     * carries a proof.  Ordinary generics such as `dict<str, int>` must keep
     * their normal argument checks.
     */
    matches = actual_base && param_base &&
              strcmp(actual_base, param_base) == 0 &&
              hm_lookup_refine_stmt(hm, actual_base) != NULL;
    free(actual_base);
    free(param_base);
  }
  free(actual);
  return matches;
}

static bool hm_expr_has_refinement_type(ny_hm_state_t *hm, ny_hm_env_list *env,
                                        const char *expected, expr_t *expr) {
  if (!hm || !env || !expected || !expr || expr->kind != NY_E_IDENT ||
      !hm_lookup_refine_stmt(hm, expected))
    return false;
  ny_hm_type_t *actual =
      hm_env_get_ctx(env, expr->as.ident.name, expr->as.ident.syntax_ctx);
  char *actual_name = hm_type_string(actual);
  bool matches = actual_name && strcmp(actual_name, expected) == 0;
  free(actual_name);
  return matches;
}

typedef struct {
  const char *name;
  uint32_t syntax_ctx;
  bool found;
  bool consistent;
} hm_refinement_formal_context_t;

static bool hm_refinement_formal_context_visit(ny_visitor_t *visitor,
                                               expr_t *expr) {
  hm_refinement_formal_context_t *context =
      visitor ? (hm_refinement_formal_context_t *)visitor->ctx : NULL;
  if (!context || !expr || expr->kind != NY_E_IDENT || !expr->as.ident.name ||
      strcmp(expr->as.ident.name, context->name) != 0)
    return true;
  if (!context->found) {
    context->syntax_ctx = expr->as.ident.syntax_ctx;
    context->found = true;
  } else if (context->syntax_ctx != expr->as.ident.syntax_ctx) {
    context->consistent = false;
  }
  return false;
}

static bool hm_refinement_formal_context(const expr_t *predicate,
                                         const char *formal,
                                         uint32_t *syntax_ctx) {
  if (!predicate || !formal || !syntax_ctx)
    return false;
  hm_refinement_formal_context_t context = {
      .name = formal,
      .consistent = true,
  };
  ny_visitor_t visitor = {
      .ctx = &context,
      .visit_expr_pre = hm_refinement_formal_context_visit,
  };
  ny_visit_expr(&visitor, (expr_t *)predicate);
  if (!context.found || !context.consistent)
    return false;
  *syntax_ctx = context.syntax_ctx;
  return true;
}

/*
 * Match the small, side-effect-free expression fragment used to transport a
 * relational guard into a declared refinement.  Only the refinement formal
 * and literals are admitted: calls and unrelated identifiers could observe
 * mutable state, so they are deliberately not proof evidence here.
 */
static const char *hm_refinement_complement_comparison(const char *op);
static ny_sem_rep_t hm_semantic_rep(const char *type_name);

static bool
hm_refinement_expr_matches_guard(const expr_t *predicate, const char *formal,
                                 uint32_t formal_ctx, const expr_t *guard,
                                 const char *subject, uint32_t subject_ctx,
                                 bool negate) {
  if (!predicate || !guard || predicate->kind != guard->kind)
    return false;
  switch (predicate->kind) {
  case NY_E_IDENT:
    if (predicate->as.ident.name &&
        strcmp(predicate->as.ident.name, formal) == 0 &&
        predicate->as.ident.syntax_ctx == formal_ctx)
      return guard->as.ident.name &&
             strcmp(guard->as.ident.name, subject) == 0 &&
             guard->as.ident.syntax_ctx == subject_ctx;
    /*
     * Non-formal names are not stable proof inputs.
     */
    return false;
  case NY_E_LITERAL: {
    const literal_t *a = &predicate->as.literal;
    const literal_t *b = &guard->as.literal;
    if (a->kind != b->kind || a->hint != b->hint)
      return false;
    switch (a->kind) {
    case NY_LIT_INT:
      return a->integer_outside_i64 == b->integer_outside_i64 &&
             (a->integer_outside_i64
                  ? a->numeric_len == b->numeric_len && a->numeric_text &&
                        b->numeric_text &&
                        memcmp(a->numeric_text, b->numeric_text,
                               a->numeric_len) == 0
                  : a->as.i == b->as.i);
    case NY_LIT_FLOAT:
      return a->as.f == b->as.f;
    case NY_LIT_BOOL:
      return a->as.b == b->as.b;
    case NY_LIT_STR:
      return a->as.s.len == b->as.s.len &&
             (a->as.s.len == 0 ||
              memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0);
    }
    return false;
  }
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    if (!predicate->as.binary.op || !guard->as.binary.op)
      return false;
    const char *expected_op = predicate->as.binary.op;
    if (negate) {
      if (predicate->kind != NY_E_BINARY)
        return false;
      expected_op = hm_refinement_complement_comparison(expected_op);
      if (!expected_op)
        return false;
    }
    return strcmp(expected_op, guard->as.binary.op) == 0 &&
           hm_refinement_expr_matches_guard(predicate->as.binary.left, formal,
                                            formal_ctx, guard->as.binary.left,
                                            subject, subject_ctx, false) &&
           hm_refinement_expr_matches_guard(predicate->as.binary.right, formal,
                                            formal_ctx, guard->as.binary.right,
                                            subject, subject_ctx, false);
  case NY_E_UNARY:
    if (negate)
      return false;
    return predicate->as.unary.op && guard->as.unary.op &&
           strcmp(predicate->as.unary.op, guard->as.unary.op) == 0 &&
           hm_refinement_expr_matches_guard(predicate->as.unary.right, formal,
                                            formal_ctx, guard->as.unary.right,
                                            subject, subject_ctx, false);
  case NY_E_MEMBER:
    return predicate->as.member.name && guard->as.member.name &&
           strcmp(predicate->as.member.name, guard->as.member.name) == 0 &&
           hm_refinement_expr_matches_guard(predicate->as.member.target, formal,
                                            formal_ctx, guard->as.member.target,
                                            subject, subject_ctx, false);
  case NY_E_INDEX: {
    const expr_t *pa[] = {predicate->as.index.target, predicate->as.index.start,
                          predicate->as.index.stop, predicate->as.index.step};
    const expr_t *ga[] = {guard->as.index.target, guard->as.index.start,
                          guard->as.index.stop, guard->as.index.step};
    for (size_t i = 0; i < sizeof(pa) / sizeof(pa[0]); ++i) {
      if (!pa[i] || !ga[i]) {
        if (pa[i] != ga[i])
          return false;
      } else if (!hm_refinement_expr_matches_guard(pa[i], formal, formal_ctx,
                                                   ga[i], subject, subject_ctx,
                                                   false)) {
        return false;
      }
    }
    return true;
  }
  default:
    return false;
  }
}

static const char *hm_refinement_swapped_comparison(const char *op) {
  if (!op)
    return NULL;
  if (strcmp(op, "<") == 0)
    return ">";
  if (strcmp(op, "<=") == 0)
    return ">=";
  if (strcmp(op, ">") == 0)
    return "<";
  if (strcmp(op, ">=") == 0)
    return "<=";
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)
    return op;
  return NULL;
}

static const char *hm_refinement_complement_comparison(const char *op) {
  if (!op)
    return NULL;
  if (strcmp(op, ">") == 0)
    return "<=";
  if (strcmp(op, ">=") == 0)
    return "<";
  if (strcmp(op, "<") == 0)
    return ">=";
  if (strcmp(op, "<=") == 0)
    return ">";
  if (strcmp(op, "==") == 0)
    return "!=";
  if (strcmp(op, "!=") == 0)
    return "==";
  return NULL;
}

/*
 * A true conjunction implies each operand; a false disjunction implies each
 * operand is false. Keep these implications one-way and exact.
 */
static bool
hm_refinement_guard_implies_predicate(const expr_t *predicate,
                                      const char *formal, uint32_t formal_ctx,
                                      const expr_t *guard, const char *subject,
                                      uint32_t subject_ctx, bool negate) {
  if (guard && guard->kind == NY_E_UNARY && guard->as.unary.op &&
      strcmp(guard->as.unary.op, "!") == 0)
    return hm_refinement_guard_implies_predicate(predicate, formal, formal_ctx,
                                                 guard->as.unary.right, subject,
                                                 subject_ctx, !negate);
  if (hm_refinement_expr_matches_guard(predicate, formal, formal_ctx, guard,
                                       subject, subject_ctx, negate))
    return true;
  if (predicate && guard && predicate->kind == NY_E_BINARY &&
      guard->kind == NY_E_BINARY) {
    const char *expected_op = predicate->as.binary.op;
    if (negate)
      expected_op = hm_refinement_complement_comparison(expected_op);
    const char *swapped = hm_refinement_swapped_comparison(expected_op);
    if (swapped && guard->as.binary.op &&
        strcmp(swapped, guard->as.binary.op) == 0 &&
        hm_refinement_expr_matches_guard(predicate->as.binary.left, formal,
                                         formal_ctx, guard->as.binary.right,
                                         subject, subject_ctx, false) &&
        hm_refinement_expr_matches_guard(predicate->as.binary.right, formal,
                                         formal_ctx, guard->as.binary.left,
                                         subject, subject_ctx, false))
      return true;
  }
  if (!guard || guard->kind != NY_E_LOGICAL || !guard->as.logical.op)
    return false;
  const char *op = guard->as.logical.op;
  if ((!negate && strcmp(op, "&&") != 0) || (negate && strcmp(op, "||") != 0))
    return false;
  return hm_refinement_guard_implies_predicate(predicate, formal, formal_ctx,
                                               guard->as.logical.left, subject,
                                               subject_ctx, negate) ||
         hm_refinement_guard_implies_predicate(predicate, formal, formal_ctx,
                                               guard->as.logical.right, subject,
                                               subject_ctx, negate);
}

/*
 * A mutable local can retain a guard fact only when its region and enclosing
 * function are statically read-only. Unknown syntax fails closed. Indirect
 * calls, pointer operations, closures, assignments, and deferred bodies are
 * excluded because they can hide a write through a captured or aliased
 * binding.
 */
static ny_hm_type_t *hm_infer_expr(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   expr_t *e, const char *self_name);
static bool hm_refinement_region_readonly_expr(ny_hm_state_t *hm,
                                               ny_hm_env_list *env,
                                               const expr_t *expr);

/*
 * Effect summaries currently expose argument escape as one function-wide bit.
 * Recover a trivially safe case from source: a single-argument helper whose
 * entire body returns that argument's scalar `.len` field. This proves the
 * aggregate argument is observed without being retained or returned.
 */
static bool hm_refinement_call_is_len_reader(const fun_sig *sig,
                                             const expr_t *call) {
  if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC || !call ||
      call->kind != NY_E_CALL || call->as.call.args.len != 1 ||
      sig->stmt_t->as.fn.params.len != 1 || !sig->stmt_t->as.fn.body ||
      sig->stmt_t->as.fn.body->kind != NY_S_BLOCK ||
      sig->stmt_t->as.fn.body->as.block.body.len != 1)
    return false;
  const call_arg_t *arg = &call->as.call.args.data[0];
  const param_t *param = &sig->stmt_t->as.fn.params.data[0];
  if ((arg->name && (!param->name || strcmp(arg->name, param->name) != 0)) ||
      !arg->val || arg->val->kind != NY_E_IDENT || !arg->val->as.ident.name)
    return false;
  const stmt_t *body = sig->stmt_t->as.fn.body->as.block.body.data[0];
  const expr_t *value = NULL;
  if (body && body->kind == NY_S_RETURN)
    value = body->as.ret.value;
  else if (body && body->kind == NY_S_EXPR)
    value = body->as.expr.expr;
  return value && value->kind == NY_E_MEMBER && value->as.member.name &&
         strcmp(value->as.member.name, "len") == 0 && value->as.member.target &&
         value->as.member.target->kind == NY_E_IDENT &&
         value->as.member.target->as.ident.name && param->name &&
         strcmp(value->as.member.target->as.ident.name, param->name) == 0 &&
         value->as.member.target->as.ident.syntax_ctx == param->syntax_ctx;
}

/*
 * A direct source function call can safely cross a retained refinement only
 * when the completed function summary proves its only effect is reading
 * memory and it is non-mutating, non-escaping, and non-aliasing. Externs and
 * unknown/incomplete summaries are deliberately rejected.
 */
static bool hm_refinement_region_readonly_call(ny_hm_state_t *hm,
                                               ny_hm_env_list *env,
                                               const expr_t *expr) {
  const expr_t *callee =
      expr && expr->kind == NY_E_CALL ? expr->as.call.callee : NULL;
  if (!hm || !hm->ctx || !hm->ctx->cg || !callee ||
      callee->kind != NY_E_IDENT || !callee->as.ident.name)
    return false;
  fun_sig *sig = lookup_fun(hm->ctx->cg, callee->as.ident.name, 0);
  bool len_reader = sig && hm_refinement_call_is_len_reader(sig, expr);
  if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
      sig->is_extern || !sig->effects_known ||
      (sig->effects & ~NY_FX_READ_MEMORY) != NY_FX_NONE ||
      (sig->args_escape && !len_reader) || sig->args_mutated ||
      (sig->returns_alias && !len_reader))
    return false;
  for (size_t i = 0; i < expr->as.call.args.len; ++i)
    if (!hm_refinement_region_readonly_expr(hm, env,
                                            expr->as.call.args.data[i].val))
      return false;
  return true;
}

/*
 * A source operator spelling is not necessarily a primitive operation: an
 * active `impl` may route it through an arbitrary function.  Use inferred
 * operand types to detect the selected overload; unknown operand types fail
 * because the primitive implementation cannot be established.
 */
static bool hm_refinement_operator_may_be_overloaded(ny_hm_state_t *hm,
                                                     ny_hm_env_list *env,
                                                     const expr_t *expr) {
  const char *op =
      expr && expr->kind == NY_E_BINARY ? expr->as.binary.op : NULL;
  if (!hm || !expr || !op || strcmp(op, "=") == 0)
    return false;
  codegen_t *cg = hm->ctx ? hm->ctx->cg : NULL;
  if (!env) {
    for (size_t i = 0; cg && i < cg->operators.len; ++i)
      if (cg->operators.data[i].op && strcmp(cg->operators.data[i].op, op) == 0)
        return true;
    return false;
  }
  ny_hm_type_t *left_type = hm_infer_expr(hm, env, expr->as.binary.left, NULL);
  ny_hm_type_t *right_type =
      hm_infer_expr(hm, env, expr->as.binary.right, NULL);
  char *left_name = hm_type_string(hm_prune(left_type));
  char *right_name = hm_type_string(hm_prune(right_type));
  if (!left_name || !right_name || strcmp(left_name, "any") == 0 ||
      strcmp(right_name, "any") == 0) {
    free(left_name);
    free(right_name);
    return true;
  }
  for (size_t i = 0; cg && i < cg->operators.len; ++i) {
    const ny_operator_def_t *definition = &cg->operators.data[i];
    if (definition->op && definition->left_type && definition->right_type &&
        strcmp(definition->op, op) == 0 &&
        strcmp(definition->left_type, left_name) == 0 &&
        strcmp(definition->right_type, right_name) == 0) {
      free(left_name);
      free(right_name);
      return true;
    }
  }
  free(left_name);
  free(right_name);
  return false;
}

static bool hm_refinement_region_readonly_expr(ny_hm_state_t *hm,
                                               ny_hm_env_list *env,
                                               const expr_t *expr) {
  if (!expr)
    return true;
  switch (expr->kind) {
  case NY_E_IDENT:
  case NY_E_LITERAL:
    return true;
  case NY_E_BINARY:
  case NY_E_LOGICAL: {
    const char *op =
        expr->kind == NY_E_BINARY ? expr->as.binary.op : expr->as.logical.op;
    static const char *const writes[] = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    if (!op)
      return false;
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i)
      if (strcmp(op, writes[i]) == 0)
        return false;
    if (expr->kind == NY_E_BINARY &&
        hm_refinement_operator_may_be_overloaded(hm, env, expr))
      return false;
    const expr_t *left = expr->kind == NY_E_BINARY ? expr->as.binary.left
                                                   : expr->as.logical.left;
    const expr_t *right = expr->kind == NY_E_BINARY ? expr->as.binary.right
                                                    : expr->as.logical.right;
    return hm_refinement_region_readonly_expr(hm, env, left) &&
           hm_refinement_region_readonly_expr(hm, env, right);
  }
  case NY_E_UNARY:
    return expr->as.unary.op &&
           (strcmp(expr->as.unary.op, "!") == 0 ||
            strcmp(expr->as.unary.op, "+") == 0 ||
            strcmp(expr->as.unary.op, "-") == 0 ||
            strcmp(expr->as.unary.op, "~") == 0) &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.unary.right);
  case NY_E_TERNARY:
    return hm_refinement_region_readonly_expr(hm, env, expr->as.ternary.cond) &&
           hm_refinement_region_readonly_expr(hm, env,
                                              expr->as.ternary.true_expr) &&
           hm_refinement_region_readonly_expr(hm, env,
                                              expr->as.ternary.false_expr);
  case NY_E_MEMBER:
    return expr->as.member.name &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.member.target);
  case NY_E_INDEX:
    return hm_refinement_region_readonly_expr(hm, env, expr->as.index.target) &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.index.start) &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.index.stop) &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.index.step);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < expr->as.list_like.len; ++i)
      if (!hm_refinement_region_readonly_expr(hm, env,
                                              expr->as.list_like.data[i]))
        return false;
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < expr->as.dict.pairs.len; ++i)
      if (!hm_refinement_region_readonly_expr(
              hm, env, expr->as.dict.pairs.data[i].key) ||
          !hm_refinement_region_readonly_expr(
              hm, env, expr->as.dict.pairs.data[i].value))
        return false;
    return true;
  case NY_E_CALL: {
    /*
     * Recognized refinement predicates and proven read-only source calls can
     * be evaluated without invalidating a retained fact.
     */
    const expr_t *callee = expr->as.call.callee;
    if (hm && callee && callee->kind == NY_E_IDENT && callee->as.ident.name &&
        expr->as.call.args.len == 1 &&
        hm_lookup_refine_stmt(hm, callee->as.ident.name) &&
        hm_refinement_region_readonly_expr(hm, env,
                                           expr->as.call.args.data[0].val))
      return true;
    return hm_refinement_region_readonly_call(hm, env, expr);
  }
  default:
    return false;
  }
}

static bool hm_refinement_region_readonly_stmt(ny_hm_state_t *hm,
                                               ny_hm_env_list *env,
                                               const stmt_t *stmt) {
  if (!stmt)
    return true;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!hm_refinement_region_readonly_stmt(hm, env,
                                              stmt->as.block.body.data[i]))
        return false;
    return true;
  case NY_S_VAR:
    if (stmt->as.var.is_del)
      return false;
    for (size_t i = 0; i < stmt->as.var.exprs.len; ++i)
      if (!hm_refinement_region_readonly_expr(hm, env,
                                              stmt->as.var.exprs.data[i]))
        return false;
    return true;
  case NY_S_EXPR:
    return hm_refinement_region_readonly_expr(hm, env, stmt->as.expr.expr);
  case NY_S_RETURN:
    return hm_refinement_region_readonly_expr(hm, env, stmt->as.ret.value);
  case NY_S_IF:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.iff.init) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.iff.test) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.iff.conseq) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.iff.alt);
  case NY_S_WHILE:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.whl.init) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.whl.test) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.whl.body) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.whl.update);
  case NY_S_FOR:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.fr.init) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.fr.iterable) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.fr.cond) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.fr.body) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.fr.update);
  case NY_S_MATCH:
    if (!hm_refinement_region_readonly_expr(hm, env, stmt->as.match.test))
      return false;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      if (!arm || !hm_refinement_region_readonly_expr(hm, env, arm->guard))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!hm_refinement_region_readonly_expr(hm, env, arm->patterns.data[j]))
          return false;
      if (!hm_refinement_region_readonly_stmt(hm, env, arm->conseq))
        return false;
    }
    return hm_refinement_region_readonly_stmt(hm, env,
                                              stmt->as.match.default_conseq);
  case NY_S_TRY:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.tr.body) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.tr.handler);
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return true;
  default:
    return false;
  }
}

static bool hm_refinement_const_i64_expr(const expr_t *expr,
                                         int64_t *value_out);

/*
 * A direct `.len` lower bound survives element stores and append.
 */
static bool hm_refinement_length_lower_bound_atom(const stmt_t *definition,
                                                  const expr_t *predicate) {
  if (!definition || definition->kind != NY_S_REFINE ||
      !definition->as.refine.param || !definition->as.refine.base_type ||
      !ny_generic_type_base_is(definition->as.refine.base_type, "list") ||
      !predicate)
    return false;
  if (predicate->kind != NY_E_BINARY || !predicate->as.binary.op ||
      !predicate->as.binary.left || !predicate->as.binary.right)
    return false;
  const expr_t *length = NULL;
  const expr_t *bound_expr = NULL;
  const char *op = predicate->as.binary.op;
  if ((strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) &&
      predicate->as.binary.left->kind == NY_E_MEMBER) {
    length = predicate->as.binary.left;
    bound_expr = predicate->as.binary.right;
  } else if ((strcmp(op, "<") == 0 || strcmp(op, "<=") == 0) &&
             predicate->as.binary.right->kind == NY_E_MEMBER) {
    length = predicate->as.binary.right;
    bound_expr = predicate->as.binary.left;
  } else {
    return false;
  }
  if (!length->as.member.name || strcmp(length->as.member.name, "len") != 0 ||
      !length->as.member.target ||
      length->as.member.target->kind != NY_E_IDENT ||
      !length->as.member.target->as.ident.name ||
      strcmp(length->as.member.target->as.ident.name,
             definition->as.refine.param) != 0)
    return false;
  uint32_t formal_ctx = 0;
  if (!hm_refinement_formal_context(predicate, definition->as.refine.param,
                                    &formal_ctx) ||
      length->as.member.target->as.ident.syntax_ctx != formal_ctx)
    return false;
  int64_t bound = 0;
  return hm_refinement_const_i64_expr(bound_expr, &bound) && bound >= 0;
}

/*
 * List length lower bounds survive element stores and append. Keep the
 * accepted syntax deliberately small: a direct `.len` comparison against a
 * folded, nonnegative integer constant on the refinement formal, in either
 * operand order.
 */
static bool
hm_refinement_length_lower_bound_predicate(const stmt_t *definition) {
  return definition && definition->kind == NY_S_REFINE &&
         hm_refinement_length_lower_bound_atom(definition,
                                               definition->as.refine.predicate);
}

static bool hm_refinement_const_i64(const expr_t *expr, int64_t *value_out,
                                    unsigned depth) {
  if (!expr || !value_out || depth > 32)
    return false;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_INT &&
      !expr->as.literal.integer_outside_i64) {
    *value_out = expr->as.literal.as.i;
    return true;
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      strcmp(expr->as.unary.op, "-") == 0) {
    int64_t operand = 0;
    if (!hm_refinement_const_i64(expr->as.unary.right, &operand, depth + 1) ||
        operand == INT64_MIN)
      return false;
    *value_out = -operand;
    return true;
  }
  if (expr->kind == NY_E_BINARY && expr->as.binary.op && expr->as.binary.left &&
      expr->as.binary.right) {
    int64_t left = 0, right = 0, result = 0;
    if (!hm_refinement_const_i64(expr->as.binary.left, &left, depth + 1) ||
        !hm_refinement_const_i64(expr->as.binary.right, &right, depth + 1))
      return false;
    if (strcmp(expr->as.binary.op, "+") == 0) {
      if (__builtin_add_overflow(left, right, &result))
        return false;
    } else if (strcmp(expr->as.binary.op, "-") == 0) {
      if (__builtin_sub_overflow(left, right, &result))
        return false;
    } else if (strcmp(expr->as.binary.op, "*") == 0) {
      if (__builtin_mul_overflow(left, right, &result))
        return false;
    } else if (strcmp(expr->as.binary.op, "/") == 0) {
      if (!right || (left == INT64_MIN && right == -1))
        return false;
      result = left / right;
    } else if (strcmp(expr->as.binary.op, "%") == 0) {
      if (!right || (left == INT64_MIN && right == -1))
        return false;
      result = left % right;
    } else {
      return false;
    }
    *value_out = result;
    return true;
  }
  return false;
}

static bool hm_refinement_const_i64_expr(const expr_t *expr,
                                         int64_t *value_out) {
  return hm_refinement_const_i64(expr, value_out, 0);
}

static const expr_t *hm_refinement_indexed_atom_bound(const stmt_t *definition,
                                                      const expr_t *predicate,
                                                      int64_t *index_out,
                                                      const char **op_out) {
  if (!definition || definition->kind != NY_S_REFINE ||
      !definition->as.refine.param || !definition->as.refine.base_type ||
      !ny_generic_type_base_is(definition->as.refine.base_type, "list"))
    return NULL;
  if (!predicate || predicate->kind != NY_E_BINARY ||
      !predicate->as.binary.op || !predicate->as.binary.left ||
      !predicate->as.binary.right ||
      (strcmp(predicate->as.binary.op, ">") != 0 &&
       strcmp(predicate->as.binary.op, ">=") != 0 &&
       strcmp(predicate->as.binary.op, "<") != 0 &&
       strcmp(predicate->as.binary.op, "<=") != 0 &&
       strcmp(predicate->as.binary.op, "==") != 0 &&
       strcmp(predicate->as.binary.op, "!=") != 0) ||
      predicate->as.binary.left->kind != NY_E_INDEX)
    return NULL;
  const expr_t *indexed = predicate->as.binary.left;
  const expr_t *target = indexed->as.index.target;
  const expr_t *index = indexed->as.index.start;
  const expr_t *bound = predicate->as.binary.right;
  int64_t index_value = 0;
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name ||
      strcmp(target->as.ident.name, definition->as.refine.param) != 0 ||
      !hm_refinement_const_i64_expr(index, &index_value))
    return NULL;
  uint32_t formal_ctx = 0;
  if (!hm_refinement_formal_context(predicate, definition->as.refine.param,
                                    &formal_ctx) ||
      target->as.ident.syntax_ctx != formal_ctx)
    return NULL;
  if (index_out)
    *index_out = index_value;
  if (op_out)
    *op_out = predicate->as.binary.op;
  return bound;
}

static bool hm_refinement_indexed_int_atom(const stmt_t *definition,
                                           const expr_t *predicate,
                                           int64_t *index_out,
                                           const char **op_out,
                                           int64_t *bound_out) {
  int64_t index_value = 0, bound_value = 0;
  const char *op = NULL;
  const expr_t *bound = hm_refinement_indexed_atom_bound(definition, predicate,
                                                         &index_value, &op);
  if (!bound || !hm_refinement_const_i64_expr(bound, &bound_value))
    return false;
  if (index_out)
    *index_out = index_value;
  if (op_out)
    *op_out = op;
  if (bound_out)
    *bound_out = bound_value;
  return true;
}

static bool hm_refinement_indexed_predicate_all(const stmt_t *definition,
                                                const expr_t *predicate,
                                                unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_predicate_all(
               definition, predicate->as.logical.left, depth + 1) &&
           hm_refinement_indexed_predicate_all(
               definition, predicate->as.logical.right, depth + 1);
  return hm_refinement_indexed_atom_bound(definition, predicate, NULL, NULL) !=
         NULL;
}

static bool hm_refinement_indexed_int_predicate_all(const stmt_t *definition,
                                                    const expr_t *predicate,
                                                    unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_int_predicate_all(
               definition, predicate->as.logical.left, depth + 1) &&
           hm_refinement_indexed_int_predicate_all(
               definition, predicate->as.logical.right, depth + 1);
  return hm_refinement_indexed_int_atom(definition, predicate, NULL, NULL,
                                        NULL);
}

static bool hm_refinement_indexed_int_predicate(const stmt_t *definition,
                                                int64_t *index_out,
                                                const char **op_out,
                                                int64_t *bound_out) {
  if (!definition || definition->kind != NY_S_REFINE ||
      !hm_refinement_indexed_int_predicate_all(
          definition, definition->as.refine.predicate, 0))
    return false;
  const expr_t *predicate = definition->as.refine.predicate;
  if (!predicate || predicate->kind == NY_E_LOGICAL)
    return index_out == NULL && op_out == NULL && bound_out == NULL;
  return hm_refinement_indexed_int_atom(definition, predicate, index_out,
                                        op_out, bound_out);
}

static bool hm_refinement_indexed_indices_nonnegative(const stmt_t *definition,
                                                      const expr_t *predicate,
                                                      unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_indices_nonnegative(
               definition, predicate->as.logical.left, depth + 1) &&
           hm_refinement_indexed_indices_nonnegative(
               definition, predicate->as.logical.right, depth + 1);
  int64_t index = 0;
  return hm_refinement_indexed_atom_bound(definition, predicate, &index,
                                          NULL) != NULL &&
         index >= 0;
}

static bool
hm_refinement_indexed_predicate_indices_nonnegative(const stmt_t *definition) {
  return definition && definition->kind == NY_S_REFINE &&
         hm_refinement_indexed_int_predicate_all(
             definition, definition->as.refine.predicate, 0) &&
         hm_refinement_indexed_indices_nonnegative(
             definition, definition->as.refine.predicate, 0);
}

static bool hm_refinement_content_contains_len(const expr_t *expr,
                                               unsigned depth) {
  if (depth > 64)
    return true;
  if (!expr)
    return false;
  switch (expr->kind) {
  case NY_E_MEMBER:
    return (expr->as.member.name && strcmp(expr->as.member.name, "len") == 0) ||
           hm_refinement_content_contains_len(expr->as.member.target,
                                              depth + 1);
  case NY_E_BINARY:
    return hm_refinement_content_contains_len(expr->as.binary.left,
                                              depth + 1) ||
           hm_refinement_content_contains_len(expr->as.binary.right, depth + 1);
  case NY_E_LOGICAL:
    return hm_refinement_content_contains_len(expr->as.logical.left,
                                              depth + 1) ||
           hm_refinement_content_contains_len(expr->as.logical.right,
                                              depth + 1);
  case NY_E_UNARY:
    return hm_refinement_content_contains_len(expr->as.unary.right, depth + 1);
  case NY_E_TERNARY:
    return hm_refinement_content_contains_len(expr->as.ternary.cond,
                                              depth + 1) ||
           hm_refinement_content_contains_len(expr->as.ternary.true_expr,
                                              depth + 1) ||
           hm_refinement_content_contains_len(expr->as.ternary.false_expr,
                                              depth + 1);
  case NY_E_INDEX:
    return hm_refinement_content_contains_len(expr->as.index.target,
                                              depth + 1) ||
           hm_refinement_content_contains_len(expr->as.index.start,
                                              depth + 1) ||
           hm_refinement_content_contains_len(expr->as.index.stop, depth + 1) ||
           hm_refinement_content_contains_len(expr->as.index.step, depth + 1);
  case NY_E_LITERAL:
  case NY_E_IDENT:
    return false;
  default:
    return true;
  }
}

static bool hm_refinement_append_length_atoms_preserved(
    const stmt_t *definition, const expr_t *expr, unsigned depth) {
  if (!definition || definition->kind != NY_S_REFINE ||
      !definition->as.refine.param || !expr || depth > 64)
    return false;
  if (expr->kind == NY_E_LOGICAL && expr->as.logical.op &&
      (strcmp(expr->as.logical.op, "&&") == 0 ||
       strcmp(expr->as.logical.op, "||") == 0))
    return hm_refinement_append_length_atoms_preserved(
               definition, expr->as.logical.left, depth + 1) &&
           hm_refinement_append_length_atoms_preserved(
               definition, expr->as.logical.right, depth + 1);
  if (expr->kind == NY_E_BINARY && expr->as.binary.left &&
      expr->as.binary.left->kind == NY_E_MEMBER &&
      expr->as.binary.left->as.member.name &&
      strcmp(expr->as.binary.left->as.member.name, "len") == 0) {
    if (hm_refinement_length_lower_bound_atom(definition, expr))
      return true;
    const expr_t *length = expr->as.binary.left;
    const expr_t *bound = expr->as.binary.right;
    const char *op = expr->as.binary.op;
    if (!length->as.member.target ||
        length->as.member.target->kind != NY_E_IDENT ||
        !length->as.member.target->as.ident.name ||
        strcmp(length->as.member.target->as.ident.name,
               definition->as.refine.param) != 0 ||
        !op || (strcmp(op, ">") != 0 && strcmp(op, ">=") != 0) ||
        hm_refinement_content_contains_len(bound, 0))
      return false;
    uint32_t formal_ctx = 0;
    return hm_refinement_formal_context(definition->as.refine.predicate,
                                        definition->as.refine.param,
                                        &formal_ctx) &&
           length->as.member.target->as.ident.syntax_ctx == formal_ctx;
  }
  if (expr->kind == NY_E_BINARY && expr->as.binary.right &&
      expr->as.binary.right->kind == NY_E_MEMBER &&
      expr->as.binary.right->as.member.name &&
      strcmp(expr->as.binary.right->as.member.name, "len") == 0) {
    const expr_t *length = expr->as.binary.right;
    const expr_t *bound = expr->as.binary.left;
    const char *op = expr->as.binary.op;
    if (!length->as.member.target ||
        length->as.member.target->kind != NY_E_IDENT ||
        !length->as.member.target->as.ident.name ||
        strcmp(length->as.member.target->as.ident.name,
               definition->as.refine.param) != 0 ||
        !op || (strcmp(op, "<") != 0 && strcmp(op, "<=") != 0) ||
        hm_refinement_content_contains_len(bound, 0))
      return false;
    uint32_t formal_ctx = 0;
    return hm_refinement_formal_context(definition->as.refine.predicate,
                                        definition->as.refine.param,
                                        &formal_ctx) &&
           length->as.member.target->as.ident.syntax_ctx == formal_ctx;
  }
  return !hm_refinement_content_contains_len(expr, depth);
}

typedef struct {
  uint32_t formal_ctx;
  int64_t store_index;
  bool append;
  bool append_copies_last;
  bool aliases_store;
} hm_refinement_content_flow_t;

static bool hm_refinement_content_predicate_walk(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *expr, hm_refinement_content_flow_t *flow, unsigned depth) {
  if (!definition || !expr || !flow || depth > 64)
    return false;
  switch (expr->kind) {
  case NY_E_LITERAL:
    return true;
  case NY_E_IDENT:
    return expr->as.ident.name &&
           !hm_is_mutable_binding(hm, env, expr->as.ident.name,
                                  expr->as.ident.syntax_ctx, SIZE_MAX);
  case NY_E_INDEX: {
    int64_t index = 0;
    const expr_t *target = expr->as.index.target;
    if (expr->as.index.stop || expr->as.index.step || !target ||
        target->kind != NY_E_IDENT || !target->as.ident.name ||
        !definition->as.refine.param ||
        strcmp(target->as.ident.name, definition->as.refine.param) != 0 ||
        target->as.ident.syntax_ctx != flow->formal_ctx ||
        !hm_refinement_const_i64_expr(expr->as.index.start, &index) ||
        (flow->append && index < 0 &&
         !(index == -1 && flow->append_copies_last)))
      return false;
    if (!flow->append &&
        (index == flow->store_index || (index < 0) != (flow->store_index < 0)))
      flow->aliases_store = true;
    return true;
  }
  case NY_E_BINARY:
  case NY_E_LOGICAL: {
    const char *op =
        expr->kind == NY_E_BINARY ? expr->as.binary.op : expr->as.logical.op;
    if (!op)
      return false;
    if (expr->kind == NY_E_BINARY) {
      static const char *const pure_ops[] = {
          "+",  "-",  "*",  "/",  "%", "&",  "|", "^",
          "<<", ">>", "==", "!=", "<", "<=", ">", ">="};
      bool known = false;
      for (size_t i = 0; i < sizeof(pure_ops) / sizeof(pure_ops[0]); ++i)
        if (strcmp(op, pure_ops[i]) == 0) {
          known = true;
          break;
        }
      if (!known)
        return false;
      if (hm_refinement_operator_may_be_overloaded(hm, env, expr))
        return false;
    } else if (strcmp(op, "&&") != 0 && strcmp(op, "||") != 0) {
      return false;
    }
    const expr_t *left = expr->kind == NY_E_BINARY ? expr->as.binary.left
                                                   : expr->as.logical.left;
    const expr_t *right = expr->kind == NY_E_BINARY ? expr->as.binary.right
                                                    : expr->as.logical.right;
    return hm_refinement_content_predicate_walk(hm, env, definition, left, flow,
                                                depth + 1) &&
           hm_refinement_content_predicate_walk(hm, env, definition, right,
                                                flow, depth + 1);
  }
  case NY_E_UNARY:
    return expr->as.unary.op &&
           (strcmp(expr->as.unary.op, "!") == 0 ||
            strcmp(expr->as.unary.op, "+") == 0 ||
            strcmp(expr->as.unary.op, "-") == 0 ||
            strcmp(expr->as.unary.op, "~") == 0) &&
           hm_refinement_content_predicate_walk(
               hm, env, definition, expr->as.unary.right, flow, depth + 1);
  case NY_E_TERNARY:
    return hm_refinement_content_predicate_walk(
               hm, env, definition, expr->as.ternary.cond, flow, depth + 1) &&
           hm_refinement_content_predicate_walk(hm, env, definition,
                                                expr->as.ternary.true_expr,
                                                flow, depth + 1) &&
           hm_refinement_content_predicate_walk(hm, env, definition,
                                                expr->as.ternary.false_expr,
                                                flow, depth + 1);
  case NY_E_MEMBER:
    return expr->as.member.name && strcmp(expr->as.member.name, "len") == 0 &&
           expr->as.member.target &&
           expr->as.member.target->kind == NY_E_IDENT &&
           expr->as.member.target->as.ident.name &&
           strcmp(expr->as.member.target->as.ident.name,
                  definition->as.refine.param) == 0 &&
           expr->as.member.target->as.ident.syntax_ctx == flow->formal_ctx;
  default:
    return false;
  }
}

static bool hm_refinement_append_copies_old_last(const stmt_t *definition,
                                                 const expr_t *appended,
                                                 uint32_t formal_ctx) {
  int64_t index = 0;
  return definition && definition->kind == NY_S_REFINE &&
         definition->as.refine.param && appended &&
         appended->kind == NY_E_INDEX && !appended->as.index.stop &&
         !appended->as.index.step && appended->as.index.target &&
         appended->as.index.target->kind == NY_E_IDENT &&
         appended->as.index.target->as.ident.name &&
         strcmp(appended->as.index.target->as.ident.name,
                definition->as.refine.param) == 0 &&
         appended->as.index.target->as.ident.syntax_ctx == formal_ctx &&
         hm_refinement_const_i64_expr(appended->as.index.start, &index) &&
         index == -1;
}

static bool hm_refinement_content_predicate_survives(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    int64_t store_index, bool append, const expr_t *appended) {
  if (!definition || definition->kind != NY_S_REFINE ||
      !ny_generic_type_base_is(definition->as.refine.base_type, "list") ||
      !definition->as.refine.param || !definition->as.refine.predicate)
    return false;
  hm_refinement_content_flow_t flow = {.store_index = store_index,
                                       .append = append};
  if (!hm_refinement_formal_context(definition->as.refine.predicate,
                                    definition->as.refine.param,
                                    &flow.formal_ctx) ||
      (append && !hm_refinement_append_length_atoms_preserved(
                     definition, definition->as.refine.predicate, 0)))
    return false;
  if (append)
    flow.append_copies_last = hm_refinement_append_copies_old_last(
        definition, appended, flow.formal_ctx);
  if (!hm_refinement_content_predicate_walk(
          hm, env, definition, definition->as.refine.predicate, &flow, 0))
    return false;
  return append || !flow.aliases_store;
}

static bool hm_refinement_indexed_store_preserves_atom(const stmt_t *definition,
                                                       const expr_t *predicate,
                                                       int64_t store_index,
                                                       int64_t stored) {
  int64_t predicate_index = 0, bound = 0;
  const char *op = NULL;
  if (!hm_refinement_indexed_int_atom(definition, predicate, &predicate_index,
                                      &op, &bound))
    return false;
  if (store_index != predicate_index) {
    /*
     * Fixed-length indexed stores keep the relative positions stable.
     */
    return (store_index < 0) == (predicate_index < 0);
  }
  if (strcmp(op, ">") == 0)
    return stored > bound;
  if (strcmp(op, ">=") == 0)
    return stored >= bound;
  if (strcmp(op, "<") == 0)
    return stored < bound;
  if (strcmp(op, "<=") == 0)
    return stored <= bound;
  if (strcmp(op, "==") == 0)
    return stored == bound;
  return stored != bound;
}

static bool hm_refinement_indexed_store_preserves_predicate(
    const stmt_t *definition, const expr_t *predicate, int64_t store_index,
    int64_t stored, unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_store_preserves_predicate(
               definition, predicate->as.logical.left, store_index, stored,
               depth + 1) &&
           hm_refinement_indexed_store_preserves_predicate(
               definition, predicate->as.logical.right, store_index, stored,
               depth + 1);
  return hm_refinement_indexed_store_preserves_atom(definition, predicate,
                                                    store_index, stored);
}

static bool hm_refinement_literal_expr_equal(const expr_t *left,
                                             const expr_t *right) {
  if (!left || !right || left->kind != NY_E_LITERAL ||
      right->kind != NY_E_LITERAL ||
      left->as.literal.kind != right->as.literal.kind)
    return false;
  const literal_t *a = &left->as.literal;
  const literal_t *b = &right->as.literal;
  switch (a->kind) {
  case NY_LIT_INT:
    return !a->integer_outside_i64 && !b->integer_outside_i64 &&
           a->as.i == b->as.i;
  case NY_LIT_BOOL:
    return a->as.b == b->as.b;
  case NY_LIT_STR:
    return a->as.s.len == b->as.s.len &&
           (a->as.s.len == 0 ||
            (a->as.s.data && b->as.s.data &&
             memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0));
  default:
    return false;
  }
}

static bool hm_refinement_local_binding_named(const stmt_t *stmt,
                                              const char *name,
                                              uint32_t syntax_ctx,
                                              unsigned depth) {
  if (!stmt || !name)
    return false;
  if (depth > 128)
    return true;
/*
 * A refinement proof may use a global literal only when the identifier is
 * unshadowed in the function.  Local declarations carry their own syntax
 * context, while several parser-produced statement nodes retain the enclosing
 * context; requiring the statement context to match can therefore miss an
 * ordinary lexical shadow.  Treat a same-spelled local as a shadow here.  This
 * is deliberately conservative: it can forgo a proof, but cannot make one
 * unsound.
 */
#define HM_REFINEMENT_BINDING_MATCH(binding)                                   \
  ((binding) && strcmp((binding), name) == 0)
  switch (stmt->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < stmt->as.var.names.len; ++i)
      if (HM_REFINEMENT_BINDING_MATCH(stmt->as.var.names.data[i]))
        return true;
    return false;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (hm_refinement_local_binding_named(stmt->as.block.body.data[i], name,
                                            syntax_ctx, depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return hm_refinement_local_binding_named(stmt->as.iff.init, name,
                                             syntax_ctx, depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.iff.conseq, name,
                                             syntax_ctx, depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.iff.alt, name, syntax_ctx,
                                             depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        const expr_t *pattern = arm->patterns.data[j];
        if (pattern && pattern->kind == NY_E_IDENT && pattern->as.ident.name &&
            strcmp(pattern->as.ident.name, name) == 0 &&
            (pattern->as.ident.syntax_ctx == syntax_ctx || syntax_ctx == 0))
          return true;
      }
      if (hm_refinement_local_binding_named(arm->conseq, name, syntax_ctx,
                                            depth + 1))
        return true;
    }
    return hm_refinement_local_binding_named(stmt->as.match.default_conseq,
                                             name, syntax_ctx, depth + 1);
  case NY_S_WHILE:
    return hm_refinement_local_binding_named(stmt->as.whl.init, name,
                                             syntax_ctx, depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.whl.body, name,
                                             syntax_ctx, depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.whl.update, name,
                                             syntax_ctx, depth + 1);
  case NY_S_FOR:
    return (HM_REFINEMENT_BINDING_MATCH(stmt->as.fr.iter_var) ||
            HM_REFINEMENT_BINDING_MATCH(stmt->as.fr.iter_index_var)) ||
           hm_refinement_local_binding_named(stmt->as.fr.init, name, syntax_ctx,
                                             depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.fr.body, name, syntax_ctx,
                                             depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.fr.update, name,
                                             syntax_ctx, depth + 1);
  case NY_S_TRY:
    return HM_REFINEMENT_BINDING_MATCH(stmt->as.tr.err) ||
           hm_refinement_local_binding_named(stmt->as.tr.body, name, syntax_ctx,
                                             depth + 1) ||
           hm_refinement_local_binding_named(stmt->as.tr.handler, name,
                                             syntax_ctx, depth + 1);
  case NY_S_GUARD:
    return HM_REFINEMENT_BINDING_MATCH(stmt->as.guard.name) ||
           hm_refinement_local_binding_named(stmt->as.guard.fallback, name,
                                             syntax_ctx, depth + 1);
  case NY_S_DEFER:
    return hm_refinement_local_binding_named(stmt->as.de.body, name, syntax_ctx,
                                             depth + 1);
  case NY_S_FUNC:
    if (HM_REFINEMENT_BINDING_MATCH(stmt->as.fn.name))
      return true;
    for (size_t i = 0; i < stmt->as.fn.params.len; ++i)
      if (stmt->as.fn.params.data[i].name &&
          strcmp(stmt->as.fn.params.data[i].name, name) == 0 &&
          stmt->syntax_ctx == syntax_ctx)
        return true;
    return hm_refinement_local_binding_named(stmt->as.fn.body, name, syntax_ctx,
                                             depth + 1);
  default:
    return false;
  }
#undef HM_REFINEMENT_BINDING_MATCH
}

static const expr_t *hm_refinement_global_value(const program_t *program,
                                                const char *name,
                                                uint32_t syntax_ctx) {
  if (!program || !name || syntax_ctx != 0)
    return NULL;
  const expr_t *found = NULL;
  const ny_stmt_list *stmts = &program->body;
  for (size_t i = 0; i < stmts->len; ++i) {
    const stmt_t *stmt = stmts->data[i];
    if (!stmt || stmt->kind != NY_S_VAR || !stmt->as.var.is_decl ||
        stmt->as.var.is_mut || stmt->syntax_ctx != syntax_ctx)
      continue;
    size_t count = stmt->as.var.names.len < stmt->as.var.exprs.len
                       ? stmt->as.var.names.len
                       : stmt->as.var.exprs.len;
    for (size_t j = 0; j < count; ++j) {
      const char *binding = stmt->as.var.names.data[j];
      const expr_t *value = stmt->as.var.exprs.data[j];
      if (!binding || strcmp(binding, name) != 0 || !value)
        continue;
      if (found)
        return NULL;
      found = value;
    }
  }
  return found;
}

static const expr_t *hm_refinement_static_literal(ny_hm_state_t *hm,
                                                  const expr_t *expr) {
  if (expr && expr->kind == NY_E_LITERAL)
    return expr;
  if (!hm || !hm->ctx || !hm->ctx->prog || !expr || expr->kind != NY_E_IDENT ||
      !expr->as.ident.name)
    return NULL;
  if (hm->active_fn) {
    for (size_t i = 0; i < hm->active_fn->as.fn.params.len; ++i) {
      const param_t *param = &hm->active_fn->as.fn.params.data[i];
      if (param->name && strcmp(param->name, expr->as.ident.name) == 0 &&
          hm->active_fn->syntax_ctx == expr->as.ident.syntax_ctx)
        return NULL;
    }
    if (hm_refinement_local_binding_named(hm->active_fn->as.fn.body,
                                          expr->as.ident.name,
                                          expr->as.ident.syntax_ctx, 0))
      return NULL;
  }
  const expr_t *value = hm_refinement_global_value(
      hm->ctx->prog, expr->as.ident.name, expr->as.ident.syntax_ctx);
  return value && value->kind == NY_E_LITERAL ? value : NULL;
}

static bool hm_refinement_static_i64(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     const expr_t *expr, int64_t *value_out,
                                     unsigned depth) {
  if (!hm || !expr || !value_out || depth > 32)
    return false;
  if (expr->kind == NY_E_LITERAL) {
    if (expr->as.literal.kind != NY_LIT_INT ||
        expr->as.literal.integer_outside_i64)
      return false;
    *value_out = expr->as.literal.as.i;
    return true;
  }
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    if (hm->active_fn) {
      for (size_t i = 0; i < hm->active_fn->as.fn.params.len; ++i) {
        const param_t *param = &hm->active_fn->as.fn.params.data[i];
        if (param->name && strcmp(param->name, expr->as.ident.name) == 0 &&
            hm->active_fn->syntax_ctx == expr->as.ident.syntax_ctx)
          return false;
      }
      if (hm_refinement_local_binding_named(hm->active_fn->as.fn.body,
                                            expr->as.ident.name,
                                            expr->as.ident.syntax_ctx, 0))
        return false;
    }
    const expr_t *global = hm_refinement_global_value(
        hm->ctx->prog, expr->as.ident.name, expr->as.ident.syntax_ctx);
    return global && global != expr &&
           hm_refinement_static_i64(hm, env, global, value_out, depth + 1);
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      (strcmp(expr->as.unary.op, "+") == 0 ||
       strcmp(expr->as.unary.op, "-") == 0 ||
       strcmp(expr->as.unary.op, "~") == 0)) {
    for (size_t i = 0; hm->ctx && hm->ctx->cg && i < hm->ctx->cg->operators.len;
         ++i)
      if (hm->ctx->cg->operators.data[i].op &&
          strcmp(hm->ctx->cg->operators.data[i].op, expr->as.unary.op) == 0)
        return false;
    int64_t operand = 0;
    if (!hm_refinement_static_i64(hm, env, expr->as.unary.right, &operand,
                                  depth + 1))
      return false;
    if (strcmp(expr->as.unary.op, "+") == 0) {
      *value_out = operand;
      return true;
    }
    if (strcmp(expr->as.unary.op, "-") == 0) {
      if (operand == INT64_MIN)
        return false;
      *value_out = -operand;
      return true;
    }
    *value_out = ~operand;
    return true;
  }
  if (expr->kind != NY_E_BINARY || !expr->as.binary.op)
    return false;
  if (hm_refinement_operator_may_be_overloaded(hm, env, expr))
    return false;
  int64_t left = 0, right = 0, result = 0;
  if (!hm_refinement_static_i64(hm, env, expr->as.binary.left, &left,
                                depth + 1) ||
      !hm_refinement_static_i64(hm, env, expr->as.binary.right, &right,
                                depth + 1))
    return false;
  const char *op = expr->as.binary.op;
  if (strcmp(op, "+") == 0) {
    if (__builtin_add_overflow(left, right, &result))
      return false;
  } else if (strcmp(op, "-") == 0) {
    if (__builtin_sub_overflow(left, right, &result))
      return false;
  } else if (strcmp(op, "*") == 0) {
    if (__builtin_mul_overflow(left, right, &result))
      return false;
  } else if (strcmp(op, "/") == 0) {
    if (right == 0 || (left == INT64_MIN && right == -1))
      return false;
    result = left / right;
  } else if (strcmp(op, "%") == 0) {
    if (right == 0 || (left == INT64_MIN && right == -1))
      return false;
    result = left % right;
  } else if (strcmp(op, "&") == 0) {
    result = left & right;
  } else if (strcmp(op, "|") == 0) {
    result = left | right;
  } else if (strcmp(op, "^") == 0) {
    result = left ^ right;
  } else {
    return false;
  }
  *value_out = result;
  return true;
}

static bool hm_refinement_indexed_literal_atom_preserved(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, int64_t store_index, const expr_t *stored) {
  int64_t predicate_index = 0;
  const char *op = NULL;
  const expr_t *bound = hm_refinement_indexed_atom_bound(definition, predicate,
                                                         &predicate_index, &op);
  if (!bound || !op || !stored)
    return false;
  bool equality_op = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
  bool relational_op = strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                       strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;
  if (!equality_op && !relational_op)
    return false;
  if (hm_refinement_operator_may_be_overloaded(hm, env, predicate))
    return false;
  if (store_index != predicate_index) {
    if ((store_index < 0) != (predicate_index < 0))
      return false;
    if (bound->kind == NY_E_IDENT && bound->as.ident.name &&
        !hm_is_mutable_binding(hm, env, bound->as.ident.name,
                               bound->as.ident.syntax_ctx, SIZE_MAX))
      return true;
    hm_refinement_content_flow_t flow = {.store_index = store_index};
    if (!hm_refinement_formal_context(definition->as.refine.predicate,
                                      definition->as.refine.param,
                                      &flow.formal_ctx) ||
        !hm_refinement_content_predicate_walk(hm, env, definition, bound, &flow,
                                              0))
      return false;
    return !flow.aliases_store;
  }
  int64_t stored_integer = 0, bound_integer = 0;
  if (hm_refinement_static_i64(hm, env, stored, &stored_integer, 0) &&
      hm_refinement_static_i64(hm, env, bound, &bound_integer, 0)) {
    if (strcmp(op, "==") == 0)
      return stored_integer == bound_integer;
    if (strcmp(op, "!=") == 0)
      return stored_integer != bound_integer;
    if (strcmp(op, "<") == 0)
      return stored_integer < bound_integer;
    if (strcmp(op, "<=") == 0)
      return stored_integer <= bound_integer;
    if (strcmp(op, ">") == 0)
      return stored_integer > bound_integer;
    if (strcmp(op, ">=") == 0)
      return stored_integer >= bound_integer;
  }
  const expr_t *stored_literal = hm_refinement_static_literal(hm, stored);
  const expr_t *bound_literal = hm_refinement_static_literal(hm, bound);
  if (!stored_literal || !bound_literal ||
      stored_literal->as.literal.kind != bound_literal->as.literal.kind ||
      (stored_literal->as.literal.kind != NY_LIT_BOOL &&
       stored_literal->as.literal.kind != NY_LIT_STR))
    return false;
  if (!equality_op)
    return false;
  bool equal = hm_refinement_literal_expr_equal(stored_literal, bound_literal);
  return strcmp(op, "==") == 0 ? equal : !equal;
}

static bool hm_refinement_indexed_literal_predicate_preserved(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, int64_t store_index, const expr_t *stored,
    unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_literal_predicate_preserved(
               hm, env, definition, predicate->as.logical.left, store_index,
               stored, depth + 1) &&
           hm_refinement_indexed_literal_predicate_preserved(
               hm, env, definition, predicate->as.logical.right, store_index,
               stored, depth + 1);
  return hm_refinement_indexed_literal_atom_preserved(
      hm, env, definition, predicate, store_index, stored);
}

static bool hm_refinement_indexed_literal_append_atom_preserved(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, const expr_t *appended) {
  int64_t index = 0;
  const expr_t *bound =
      hm_refinement_indexed_atom_bound(definition, predicate, &index, NULL);
  if (!bound)
    return false;
  if (index >= 0) {
    if (bound->kind == NY_E_LITERAL)
      return true;
    if (bound->kind == NY_E_IDENT && bound->as.ident.name &&
        strcmp(bound->as.ident.name, definition->as.refine.param) != 0 &&
        !hm_is_mutable_binding(hm, env, bound->as.ident.name,
                               bound->as.ident.syntax_ctx, SIZE_MAX))
      return true;
    hm_refinement_content_flow_t flow = {.append = true};
    if (!hm_refinement_formal_context(definition->as.refine.predicate,
                                      definition->as.refine.param,
                                      &flow.formal_ctx) ||
        !hm_refinement_append_length_atoms_preserved(definition, predicate, 0))
      return false;
    flow.append_copies_last = hm_refinement_append_copies_old_last(
        definition, appended, flow.formal_ctx);
    return hm_refinement_content_predicate_walk(hm, env, definition, bound,
                                                &flow, 0);
  }
  return index == -1 && hm_refinement_indexed_literal_atom_preserved(
                            hm, env, definition, predicate, -1, appended);
}

static bool hm_refinement_indexed_literal_append_predicate_preserved(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, const expr_t *appended, unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_literal_append_predicate_preserved(
               hm, env, definition, predicate->as.logical.left, appended,
               depth + 1) &&
           hm_refinement_indexed_literal_append_predicate_preserved(
               hm, env, definition, predicate->as.logical.right, appended,
               depth + 1);
  return hm_refinement_indexed_literal_append_atom_preserved(
      hm, env, definition, predicate, appended);
}

static bool hm_refinement_same_ident_expr(const expr_t *left,
                                          const expr_t *right) {
  return left && right && left->kind == NY_E_IDENT &&
         right->kind == NY_E_IDENT && left->as.ident.name &&
         right->as.ident.name &&
         strcmp(left->as.ident.name, right->as.ident.name) == 0 &&
         left->as.ident.syntax_ctx == right->as.ident.syntax_ctx;
}

static bool hm_refinement_stable_scalar_expr(ny_hm_state_t *hm,
                                             ny_hm_env_list *env,
                                             const expr_t *expr,
                                             unsigned depth) {
  if (!expr || depth > 64)
    return false;
  switch (expr->kind) {
  case NY_E_LITERAL:
    return true;
  case NY_E_IDENT:
    return expr->as.ident.name &&
           !hm_is_mutable_binding(hm, env, expr->as.ident.name,
                                  expr->as.ident.syntax_ctx, SIZE_MAX);
  case NY_E_UNARY:
    return expr->as.unary.op && expr->as.unary.right &&
           (strcmp(expr->as.unary.op, "!") == 0 ||
            strcmp(expr->as.unary.op, "+") == 0 ||
            strcmp(expr->as.unary.op, "-") == 0 ||
            strcmp(expr->as.unary.op, "~") == 0) &&
           hm_refinement_stable_scalar_expr(hm, env, expr->as.unary.right,
                                            depth + 1);
  case NY_E_BINARY:
    return expr->as.binary.op && expr->as.binary.left &&
           expr->as.binary.right &&
           !hm_refinement_operator_may_be_overloaded(hm, env, expr) &&
           hm_refinement_stable_scalar_expr(hm, env, expr->as.binary.left,
                                            depth + 1) &&
           hm_refinement_stable_scalar_expr(hm, env, expr->as.binary.right,
                                            depth + 1);
  case NY_E_LOGICAL:
    return expr->as.logical.op && expr->as.logical.left &&
           expr->as.logical.right &&
           (strcmp(expr->as.logical.op, "&&") == 0 ||
            strcmp(expr->as.logical.op, "||") == 0) &&
           hm_refinement_stable_scalar_expr(hm, env, expr->as.logical.left,
                                            depth + 1) &&
           hm_refinement_stable_scalar_expr(hm, env, expr->as.logical.right,
                                            depth + 1);
  case NY_E_TERNARY:
    return hm_refinement_stable_scalar_expr(hm, env, expr->as.ternary.cond,
                                            depth + 1) &&
           hm_refinement_stable_scalar_expr(hm, env, expr->as.ternary.true_expr,
                                            depth + 1) &&
           hm_refinement_stable_scalar_expr(
               hm, env, expr->as.ternary.false_expr, depth + 1);
  default:
    return false;
  }
}

static bool hm_refinement_indexed_symbolic_store_preserves_predicate(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, int64_t store_index, const expr_t *stored,
    unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_symbolic_store_preserves_predicate(
               hm, env, definition, predicate->as.logical.left, store_index,
               stored, depth + 1) &&
           hm_refinement_indexed_symbolic_store_preserves_predicate(
               hm, env, definition, predicate->as.logical.right, store_index,
               stored, depth + 1);
  int64_t predicate_index = 0;
  const char *op = NULL;
  const expr_t *bound = hm_refinement_indexed_atom_bound(definition, predicate,
                                                         &predicate_index, &op);
  if (!bound || bound->kind != NY_E_IDENT || !bound->as.ident.name ||
      hm_is_mutable_binding(hm, env, bound->as.ident.name,
                            bound->as.ident.syntax_ctx, SIZE_MAX) ||
      hm_refinement_operator_may_be_overloaded(hm, env, predicate))
    return false;
  if (store_index != predicate_index)
    return (store_index < 0) == (predicate_index < 0);
  return hm_refinement_same_ident_expr(stored, bound) &&
         (strcmp(op, "==") == 0 || strcmp(op, ">=") == 0 ||
          strcmp(op, "<=") == 0);
}

static bool hm_refinement_stable_expr_matches(
    ny_hm_state_t *hm, ny_hm_env_list *env, const expr_t *predicate,
    const char *formal, uint32_t formal_ctx, const expr_t *guard,
    const char *subject, uint32_t subject_ctx, unsigned depth) {
  if (!predicate || !guard || depth > 64 || predicate->kind != guard->kind)
    return false;
  switch (predicate->kind) {
  case NY_E_IDENT:
    if (!predicate->as.ident.name || !guard->as.ident.name)
      return false;
    if (strcmp(predicate->as.ident.name, formal) == 0 &&
        predicate->as.ident.syntax_ctx == formal_ctx)
      return strcmp(guard->as.ident.name, subject) == 0 &&
             guard->as.ident.syntax_ctx == subject_ctx;
    return strcmp(predicate->as.ident.name, guard->as.ident.name) == 0 &&
           predicate->as.ident.syntax_ctx == guard->as.ident.syntax_ctx &&
           !hm_is_mutable_binding(hm, env, predicate->as.ident.name,
                                  predicate->as.ident.syntax_ctx, SIZE_MAX) &&
           !hm_is_mutable_binding(hm, env, guard->as.ident.name,
                                  guard->as.ident.syntax_ctx, SIZE_MAX);
  case NY_E_LITERAL:
    return hm_refinement_literal_expr_equal(predicate, guard);
  case NY_E_BINARY:
    return predicate->as.binary.op && guard->as.binary.op &&
           strcmp(predicate->as.binary.op, guard->as.binary.op) == 0 &&
           hm_refinement_stable_expr_matches(
               hm, env, predicate->as.binary.left, formal, formal_ctx,
               guard->as.binary.left, subject, subject_ctx, depth + 1) &&
           hm_refinement_stable_expr_matches(
               hm, env, predicate->as.binary.right, formal, formal_ctx,
               guard->as.binary.right, subject, subject_ctx, depth + 1);
  case NY_E_LOGICAL:
    return predicate->as.logical.op && guard->as.logical.op &&
           strcmp(predicate->as.logical.op, guard->as.logical.op) == 0 &&
           hm_refinement_stable_expr_matches(
               hm, env, predicate->as.logical.left, formal, formal_ctx,
               guard->as.logical.left, subject, subject_ctx, depth + 1) &&
           hm_refinement_stable_expr_matches(
               hm, env, predicate->as.logical.right, formal, formal_ctx,
               guard->as.logical.right, subject, subject_ctx, depth + 1);
  case NY_E_UNARY:
    return predicate->as.unary.op && guard->as.unary.op &&
           strcmp(predicate->as.unary.op, guard->as.unary.op) == 0 &&
           hm_refinement_stable_expr_matches(
               hm, env, predicate->as.unary.right, formal, formal_ctx,
               guard->as.unary.right, subject, subject_ctx, depth + 1);
  case NY_E_MEMBER:
    return predicate->as.member.name && guard->as.member.name &&
           strcmp(predicate->as.member.name, guard->as.member.name) == 0 &&
           hm_refinement_stable_expr_matches(
               hm, env, predicate->as.member.target, formal, formal_ctx,
               guard->as.member.target, subject, subject_ctx, depth + 1);
  case NY_E_INDEX: {
    const expr_t *p[] = {predicate->as.index.target, predicate->as.index.start,
                         predicate->as.index.stop, predicate->as.index.step};
    const expr_t *g[] = {guard->as.index.target, guard->as.index.start,
                         guard->as.index.stop, guard->as.index.step};
    if (predicate->as.index.stop || predicate->as.index.step ||
        guard->as.index.stop || guard->as.index.step)
      return false;
    for (size_t i = 0; i < sizeof(p) / sizeof(p[0]); ++i) {
      if (!p[i] || !g[i]) {
        if (p[i] != g[i])
          return false;
      } else if (!hm_refinement_stable_expr_matches(hm, env, p[i], formal,
                                                    formal_ctx, g[i], subject,
                                                    subject_ctx, depth + 1)) {
        return false;
      }
    }
    return true;
  }
  default:
    return false;
  }
}

static bool hm_refinement_indexed_content_store_preserves_atom(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, int64_t store_index, const expr_t *stored) {
  if (!definition || definition->kind != NY_S_REFINE ||
      !definition->as.refine.param || !predicate)
    return false;

  uint32_t formal_ctx = 0;
  if (!hm_refinement_formal_context(definition->as.refine.predicate,
                                    definition->as.refine.param, &formal_ctx))
    return false;

  if (predicate->kind == NY_E_BINARY && predicate->as.binary.op &&
      (strcmp(predicate->as.binary.op, "==") == 0 ||
       strcmp(predicate->as.binary.op, "<=") == 0 ||
       strcmp(predicate->as.binary.op, ">=") == 0) &&
      !hm_refinement_operator_may_be_overloaded(hm, env, predicate)) {
    const expr_t *indexed = predicate->as.binary.left;
    const expr_t *other = predicate->as.binary.right;
    if (!indexed || indexed->kind != NY_E_INDEX) {
      indexed = predicate->as.binary.right;
      other = predicate->as.binary.left;
    }
    int64_t predicate_index = 0;
    if (indexed && indexed->kind == NY_E_INDEX && other &&
        !indexed->as.index.stop && !indexed->as.index.step &&
        indexed->as.index.target &&
        indexed->as.index.target->kind == NY_E_IDENT &&
        indexed->as.index.target->as.ident.name &&
        strcmp(indexed->as.index.target->as.ident.name,
               definition->as.refine.param) == 0 &&
        indexed->as.index.target->as.ident.syntax_ctx == formal_ctx &&
        hm_refinement_const_i64_expr(indexed->as.index.start,
                                     &predicate_index) &&
        predicate_index == store_index &&
        hm_refinement_stable_expr_matches(
            hm, env, other, definition->as.refine.param, formal_ctx, stored,
            definition->as.refine.param, formal_ctx, 0)) {
      hm_refinement_content_flow_t equality_flow = {.store_index = store_index};
      if (hm_refinement_content_predicate_walk(hm, env, definition, other,
                                               &equality_flow, 0) &&
          !equality_flow.aliases_store)
        return true;
    }
  }

  hm_refinement_content_flow_t flow = {.store_index = store_index};
  return hm_refinement_content_predicate_walk(hm, env, definition, predicate,
                                              &flow, 0) &&
         !flow.aliases_store;
}

static bool hm_refinement_indexed_content_store_preserves_predicate(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, int64_t store_index, const expr_t *stored,
    unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_content_store_preserves_predicate(
               hm, env, definition, predicate->as.logical.left, store_index,
               stored, depth + 1) &&
           hm_refinement_indexed_content_store_preserves_predicate(
               hm, env, definition, predicate->as.logical.right, store_index,
               stored, depth + 1);
  return hm_refinement_indexed_content_store_preserves_atom(
      hm, env, definition, predicate, store_index, stored);
}

static bool hm_refinement_indexed_append_preserves_predicate(
    const stmt_t *definition, const expr_t *predicate, int64_t appended,
    unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_append_preserves_predicate(
               definition, predicate->as.logical.left, appended, depth + 1) &&
           hm_refinement_indexed_append_preserves_predicate(
               definition, predicate->as.logical.right, appended, depth + 1);
  int64_t index = 0;
  if (!hm_refinement_indexed_int_atom(definition, predicate, &index, NULL,
                                      NULL))
    return false;
  if (index >= 0)
    return true;
  return index == -1 && hm_refinement_indexed_store_preserves_atom(
                            definition, predicate, -1, appended);
}

static bool hm_refinement_indexed_equal_append_preserves_predicate(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, const expr_t *appended, unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_equal_append_preserves_predicate(
               hm, env, definition, predicate->as.logical.left, appended,
               depth + 1) &&
           hm_refinement_indexed_equal_append_preserves_predicate(
               hm, env, definition, predicate->as.logical.right, appended,
               depth + 1);
  if (!definition || definition->kind != NY_S_REFINE ||
      !ny_generic_type_base_is(definition->as.refine.base_type, "list") ||
      !definition->as.refine.param || !predicate)
    return false;
  if (predicate->kind != NY_E_BINARY || !predicate->as.binary.op ||
      hm_refinement_operator_may_be_overloaded(hm, env, predicate))
    return false;
  bool equality = strcmp(predicate->as.binary.op, "==") == 0 ||
                  strcmp(predicate->as.binary.op, "!=") == 0;
  if (!equality || !predicate->as.binary.left || !predicate->as.binary.right ||
      predicate->as.binary.left->kind != NY_E_INDEX ||
      predicate->as.binary.right->kind != NY_E_INDEX) {
    int64_t index = 0;
    const expr_t *bound =
        hm_refinement_indexed_atom_bound(definition, predicate, &index, NULL);
    if (!bound)
      return false;
    if (index < 0)
      return index == -1 && hm_refinement_indexed_literal_append_atom_preserved(
                                hm, env, definition, predicate, appended);
    if (bound->kind == NY_E_LITERAL)
      return true;
    if (bound->kind == NY_E_IDENT && bound->as.ident.name &&
        strcmp(bound->as.ident.name, definition->as.refine.param) != 0 &&
        !hm_is_mutable_binding(hm, env, bound->as.ident.name,
                               bound->as.ident.syntax_ctx, SIZE_MAX))
      return true;
    hm_refinement_content_flow_t flow = {0};
    if (!hm_refinement_formal_context(definition->as.refine.predicate,
                                      definition->as.refine.param,
                                      &flow.formal_ctx) ||
        !hm_refinement_append_length_atoms_preserved(definition, predicate, 0))
      return false;
    flow.append = true;
    flow.append_copies_last = hm_refinement_append_copies_old_last(
        definition, appended, flow.formal_ctx);
    return hm_refinement_content_predicate_walk(hm, env, definition, predicate,
                                                &flow, 0);
  }
  const expr_t *left = predicate->as.binary.left;
  const expr_t *right = predicate->as.binary.right;
  uint32_t formal_ctx = 0;
  if (!hm_refinement_formal_context(definition->as.refine.predicate,
                                    definition->as.refine.param, &formal_ctx))
    return false;
  const expr_t *indices[] = {left, right};
  int64_t values[2] = {0, 0};
  for (size_t i = 0; i < 2; ++i) {
    const expr_t *index = indices[i];
    if (index->as.index.stop || index->as.index.step ||
        !index->as.index.target || index->as.index.target->kind != NY_E_IDENT ||
        !index->as.index.target->as.ident.name ||
        strcmp(index->as.index.target->as.ident.name,
               definition->as.refine.param) != 0 ||
        index->as.index.target->as.ident.syntax_ctx != formal_ctx ||
        !hm_refinement_const_i64_expr(index->as.index.start, &values[i]))
      return false;
  }
  const expr_t *last = NULL;
  if (values[0] == -1 && values[1] == -2)
    last = right;
  else if (values[0] == -2 && values[1] == -1)
    last = left;
  else
    return false;
  int64_t appended_index = 0;
  if (!appended || appended->kind != NY_E_INDEX || appended->as.index.stop ||
      appended->as.index.step || !appended->as.index.target ||
      appended->as.index.target->kind != NY_E_IDENT ||
      !appended->as.index.target->as.ident.name ||
      strcmp(appended->as.index.target->as.ident.name,
             definition->as.refine.param) != 0 ||
      appended->as.index.target->as.ident.syntax_ctx != formal_ctx ||
      !hm_refinement_const_i64_expr(appended->as.index.start, &appended_index))
    return false;
  if (appended_index == -2)
    return last->as.index.target->as.ident.syntax_ctx == formal_ctx;
  return appended_index == -1 && strcmp(predicate->as.binary.op, "==") == 0 &&
         last->as.index.target->as.ident.syntax_ctx == formal_ctx;
}

static bool hm_refinement_indexed_symbolic_append_preserves_predicate(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *predicate, const expr_t *appended, unsigned depth) {
  if (!predicate || depth > 64)
    return false;
  if (predicate->kind == NY_E_LOGICAL && predicate->as.logical.op &&
      (strcmp(predicate->as.logical.op, "&&") == 0 ||
       strcmp(predicate->as.logical.op, "||") == 0))
    return hm_refinement_indexed_symbolic_append_preserves_predicate(
               hm, env, definition, predicate->as.logical.left, appended,
               depth + 1) &&
           hm_refinement_indexed_symbolic_append_preserves_predicate(
               hm, env, definition, predicate->as.logical.right, appended,
               depth + 1);
  int64_t index = 0;
  const expr_t *bound =
      hm_refinement_indexed_atom_bound(definition, predicate, &index, NULL);
  if (!bound || hm_refinement_operator_may_be_overloaded(hm, env, predicate))
    return false;
  if (index >= 0)
    return hm_refinement_stable_scalar_expr(hm, env, bound, 0);
  uint32_t formal_ctx = 0;
  if (index != -1 || predicate->kind != NY_E_BINARY ||
      !predicate->as.binary.op ||
      !hm_refinement_formal_context(definition->as.refine.predicate,
                                    definition->as.refine.param, &formal_ctx) ||
      !hm_refinement_stable_scalar_expr(hm, env, bound, 0) ||
      !hm_refinement_stable_scalar_expr(hm, env, appended, 0) ||
      !hm_refinement_stable_expr_matches(
          hm, env, bound, definition->as.refine.param, formal_ctx, appended,
          definition->as.refine.param, formal_ctx, 0))
    return false;
  const char *op = predicate->as.binary.op;
  return strcmp(op, "==") == 0 || strcmp(op, ">=") == 0 ||
         strcmp(op, "<=") == 0;
}

static bool hm_refinement_indexed_append_value_preserves(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *value) {
  if (hm_refinement_indexed_equal_append_preserves_predicate(
          hm, env, definition, definition->as.refine.predicate, value, 0))
    return true;
  int64_t appended = 0;
  if (hm_refinement_indexed_int_predicate(definition, NULL, NULL, NULL) &&
      hm_refinement_const_i64_expr(value, &appended) &&
      hm_refinement_indexed_append_preserves_predicate(
          definition, definition->as.refine.predicate, appended, 0))
    return true;
  if (hm_refinement_indexed_predicate_all(definition,
                                          definition->as.refine.predicate, 0) &&
      hm_refinement_indexed_literal_append_predicate_preserved(
          hm, env, definition, definition->as.refine.predicate, value, 0))
    return true;
  return hm_refinement_indexed_predicate_all(
             definition, definition->as.refine.predicate, 0) &&
         hm_refinement_indexed_symbolic_append_preserves_predicate(
             hm, env, definition, definition->as.refine.predicate, value, 0);
}

static bool hm_refinement_indexed_store_value_preserves(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const expr_t *index, const expr_t *value) {
  int64_t store_index = 0, stored = 0;
  if (!hm_refinement_const_i64_expr(index, &store_index))
    return false;
  if (hm_refinement_indexed_int_predicate(definition, NULL, NULL, NULL) &&
      hm_refinement_const_i64_expr(value, &stored) &&
      hm_refinement_indexed_store_preserves_predicate(
          definition, definition->as.refine.predicate, store_index, stored, 0))
    return true;
  if (hm_refinement_indexed_predicate_all(definition,
                                          definition->as.refine.predicate, 0) &&
      hm_refinement_indexed_symbolic_store_preserves_predicate(
          hm, env, definition, definition->as.refine.predicate, store_index,
          value, 0))
    return true;
  if (hm_refinement_indexed_content_store_preserves_predicate(
          hm, env, definition, definition->as.refine.predicate, store_index,
          value, 0))
    return true;
  if (hm_refinement_indexed_predicate_all(definition,
                                          definition->as.refine.predicate, 0) &&
      hm_refinement_indexed_literal_predicate_preserved(
          hm, env, definition, definition->as.refine.predicate, store_index,
          value, 0))
    return true;
  return hm_refinement_content_predicate_survives(hm, env, definition,
                                                  store_index, false, NULL);
}

static bool hm_refinement_same_index_expr(const expr_t *left,
                                          const expr_t *right) {
  int64_t left_value = 0, right_value = 0;
  if (hm_refinement_const_i64_expr(left, &left_value) &&
      hm_refinement_const_i64_expr(right, &right_value))
    return left_value == right_value;
  return left && right && left->kind == NY_E_IDENT &&
         right->kind == NY_E_IDENT && left->as.ident.name &&
         right->as.ident.name &&
         strcmp(left->as.ident.name, right->as.ident.name) == 0 &&
         left->as.ident.syntax_ctx == right->as.ident.syntax_ctx;
}

static bool hm_refinement_indexed_self_store(const stmt_t *definition,
                                             const expr_t *index,
                                             const expr_t *value,
                                             const char *subject,
                                             uint32_t subject_ctx) {
  return definition && definition->kind == NY_S_REFINE &&
         ny_generic_type_base_is(definition->as.refine.base_type, "list") &&
         index && value && value->kind == NY_E_INDEX && !value->as.index.stop &&
         !value->as.index.step && value->as.index.target &&
         value->as.index.target->kind == NY_E_IDENT &&
         value->as.index.target->as.ident.name &&
         strcmp(value->as.index.target->as.ident.name, subject) == 0 &&
         value->as.index.target->as.ident.syntax_ctx == subject_ctx &&
         hm_refinement_same_index_expr(index, value->as.index.start);
}

/*
 * Refinement transport for append is valid only for the standard list
 * operation.  A source function can shadow the same leaf name and return an
 * arbitrary list, so syntactic spelling alone is not enough evidence.
 */
static bool hm_refinement_is_builtin_append(ny_hm_state_t *hm,
                                            const expr_t *call) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !call || call->kind != NY_E_CALL ||
      !call->as.call.callee || call->as.call.callee->kind != NY_E_IDENT ||
      !call->as.call.callee->as.ident.name ||
      strcmp(call->as.call.callee->as.ident.name, "append") != 0)
    return false;
  fun_sig *sig = lookup_fun(hm->ctx->cg, "append", 0);
  return !sig || !sig->stmt_t || ny_is_stdlib_tok(sig->stmt_t->tok);
}

static bool hm_refinement_list_concat_assignment(ny_hm_state_t *hm,
                                                 ny_hm_env_list *env,
                                                 const stmt_t *stmt,
                                                 const char *subject,
                                                 uint32_t subject_ctx) {
  if (!stmt || stmt->kind != NY_S_VAR || stmt->as.var.is_decl ||
      stmt->as.var.is_del || stmt->as.var.names.len != 1 ||
      stmt->as.var.exprs.len != 1 || !stmt->as.var.names.data[0] ||
      strcmp(stmt->as.var.names.data[0], subject) != 0 ||
      stmt->syntax_ctx != subject_ctx)
    return false;
  const expr_t *concat = stmt->as.var.exprs.data[0];
  if (!concat || concat->kind != NY_E_BINARY || !concat->as.binary.op ||
      strcmp(concat->as.binary.op, "+") != 0 ||
      hm_refinement_operator_may_be_overloaded(hm, env, concat))
    return false;
  const expr_t *target = concat->as.binary.left;
  const expr_t *suffix = concat->as.binary.right;
  bool suffix_is_list = suffix && suffix->kind == NY_E_LIST;
  if (suffix && suffix->kind == NY_E_IDENT && suffix->as.ident.name) {
    ny_hm_type_t *suffix_type =
        hm_env_get_ctx(env, suffix->as.ident.name, suffix->as.ident.syntax_ctx);
    suffix_type = hm_prune(suffix_type);
    suffix_is_list = suffix_type && suffix_type->kind == NY_HM_LIST;
  }
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name ||
      strcmp(target->as.ident.name, subject) != 0 ||
      target->as.ident.syntax_ctx != subject_ctx || !suffix_is_list ||
      !hm_refinement_region_readonly_expr(hm, env, suffix))
    return false;
  return true;
}

static bool hm_refinement_list_prefix_assignment(ny_hm_state_t *hm,
                                                 ny_hm_env_list *env,
                                                 const stmt_t *stmt,
                                                 const char *subject,
                                                 uint32_t subject_ctx) {
  if (!stmt || stmt->kind != NY_S_VAR || stmt->as.var.is_decl ||
      stmt->as.var.is_del || stmt->as.var.names.len != 1 ||
      stmt->as.var.exprs.len != 1 || !stmt->as.var.names.data[0] ||
      strcmp(stmt->as.var.names.data[0], subject) != 0 ||
      stmt->syntax_ctx != subject_ctx)
    return false;
  const expr_t *concat = stmt->as.var.exprs.data[0];
  if (!concat || concat->kind != NY_E_BINARY || !concat->as.binary.op ||
      strcmp(concat->as.binary.op, "+") != 0 ||
      hm_refinement_operator_may_be_overloaded(hm, env, concat))
    return false;
  const expr_t *prefix = concat->as.binary.left;
  const expr_t *target = concat->as.binary.right;
  return prefix && prefix->kind == NY_E_LIST &&
         hm_refinement_region_readonly_expr(hm, env, prefix) && target &&
         target->kind == NY_E_IDENT && target->as.ident.name &&
         strcmp(target->as.ident.name, subject) == 0 &&
         target->as.ident.syntax_ctx == subject_ctx;
}

static bool hm_refinement_nonempty_extension_assignment(ny_hm_state_t *hm,
                                                        ny_hm_env_list *env,
                                                        const stmt_t *stmt,
                                                        const char *subject,
                                                        uint32_t subject_ctx) {
  if (hm_refinement_list_concat_assignment(hm, env, stmt, subject, subject_ctx))
    return true;
  if (hm_refinement_list_prefix_assignment(hm, env, stmt, subject, subject_ctx))
    return true;
  if (!stmt || stmt->kind != NY_S_VAR || stmt->as.var.is_decl ||
      stmt->as.var.is_del || stmt->as.var.names.len != 1 ||
      stmt->as.var.exprs.len != 1 || !stmt->as.var.names.data[0] ||
      strcmp(stmt->as.var.names.data[0], subject) != 0 ||
      stmt->syntax_ctx != subject_ctx)
    return false;
  const expr_t *append = stmt->as.var.exprs.data[0];
  if (!hm_refinement_is_builtin_append(hm, append) ||
      append->as.call.args.len != 2)
    return false;
  const expr_t *target = append->as.call.args.data[0].val;
  const expr_t *item = append->as.call.args.data[1].val;
  return target && target->kind == NY_E_IDENT && target->as.ident.name &&
         strcmp(target->as.ident.name, subject) == 0 &&
         target->as.ident.syntax_ctx == subject_ctx &&
         hm_refinement_region_readonly_expr(hm, env, item);
}

static bool hm_refinement_extension_assignment_preserves(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *stmt,
    const char *subject, uint32_t subject_ctx, const stmt_t *definition) {
  if (hm_refinement_list_concat_assignment(hm, env, stmt, subject, subject_ctx))
    return hm_refinement_length_lower_bound_predicate(definition) ||
           hm_refinement_indexed_predicate_indices_nonnegative(definition) ||
           hm_refinement_content_predicate_survives(hm, env, definition, 0,
                                                    true, NULL);
  if (hm_refinement_list_prefix_assignment(hm, env, stmt, subject, subject_ctx))
    return hm_refinement_length_lower_bound_predicate(definition);
  if (!stmt || stmt->kind != NY_S_VAR || stmt->as.var.exprs.len != 1)
    return false;
  const expr_t *append = stmt->as.var.exprs.data[0];
  if (!hm_refinement_is_builtin_append(hm, append) ||
      append->as.call.args.len != 2)
    return false;
  const expr_t *item = append->as.call.args.data[1].val;
  return hm_refinement_length_lower_bound_predicate(definition) ||
         hm_refinement_indexed_predicate_indices_nonnegative(definition) ||
         hm_refinement_indexed_append_value_preserves(hm, env, definition,
                                                      item) ||
         hm_refinement_content_predicate_survives(hm, env, definition, 0, true,
                                                  item);
}

/*
 * A successful `len > 0` fact survives stores to existing list elements:
 * index assignment changes contents but cannot change the list length;
 * appending to the same local cannot make a nonempty list empty. Keep this
 * transport deliberately narrow; deleting, arbitrary calls, and writes
 * through aliases still invalidate the fact or fail closed.
 */
static bool hm_refinement_preserving_element_store(
    ny_hm_state_t *hm, ny_hm_env_list *env, const expr_t *expr,
    const char *subject, uint32_t subject_ctx, const stmt_t *definition,
    unsigned depth) {
  if (!expr || depth > 64)
    return false;
  if (expr->kind == NY_E_BINARY && expr->as.binary.op &&
      strcmp(expr->as.binary.op, "=") == 0 && expr->as.binary.left &&
      expr->as.binary.left->kind == NY_E_INDEX) {
    const expr_t *lhs = expr->as.binary.left;
    const expr_t *target = lhs->as.index.target;
    return target && target->kind == NY_E_IDENT && target->as.ident.name &&
           strcmp(target->as.ident.name, subject) == 0 &&
           target->as.ident.syntax_ctx == subject_ctx && lhs->as.index.start &&
           hm_refinement_region_readonly_expr(hm, env, lhs->as.index.start) &&
           hm_refinement_region_readonly_expr(hm, env, lhs->as.index.stop) &&
           hm_refinement_region_readonly_expr(hm, env, lhs->as.index.step) &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.binary.right) &&
           (hm_refinement_length_lower_bound_predicate(definition) ||
            hm_refinement_indexed_self_store(definition, lhs->as.index.start,
                                             expr->as.binary.right, subject,
                                             subject_ctx) ||
            hm_refinement_indexed_store_value_preserves(hm, env, definition,
                                                        lhs->as.index.start,
                                                        expr->as.binary.right));
  }
  if (expr->kind == NY_E_CALL && expr->as.call.synthetic_index_assignment &&
      expr->as.call.callee && expr->as.call.callee->kind == NY_E_IDENT &&
      expr->as.call.callee->as.ident.name &&
      strcmp(expr->as.call.callee->as.ident.name, "set_idx") == 0 &&
      expr->as.call.args.len == 3) {
    const expr_t *target = expr->as.call.args.data[0].val;
    const expr_t *index = expr->as.call.args.data[1].val;
    const expr_t *stored = expr->as.call.args.data[2].val;
    return target && target->kind == NY_E_IDENT && target->as.ident.name &&
           strcmp(target->as.ident.name, subject) == 0 &&
           target->as.ident.syntax_ctx == subject_ctx &&
           hm_refinement_region_readonly_expr(hm, env, index) &&
           hm_refinement_region_readonly_expr(hm, env, stored) &&
           (hm_refinement_length_lower_bound_predicate(definition) ||
            hm_refinement_indexed_self_store(definition, index, stored, subject,
                                             subject_ctx) ||
            hm_refinement_indexed_store_value_preserves(hm, env, definition,
                                                        index, stored));
  }
  if (hm_refinement_is_builtin_append(hm, expr) &&
      expr->as.call.args.len == 2) {
    const expr_t *target = expr->as.call.args.data[0].val;
    const expr_t *item = expr->as.call.args.data[1].val;
    if (!hm_refinement_length_lower_bound_predicate(definition) &&
        !hm_refinement_indexed_predicate_indices_nonnegative(definition) &&
        !hm_refinement_indexed_append_value_preserves(hm, env, definition,
                                                      item) &&
        !hm_refinement_content_predicate_survives(hm, env, definition, 0, true,
                                                  item))
      return false;
    return target && target->kind == NY_E_IDENT && target->as.ident.name &&
           strcmp(target->as.ident.name, subject) == 0 &&
           target->as.ident.syntax_ctx == subject_ctx &&
           hm_refinement_region_readonly_expr(hm, env,
                                              expr->as.call.args.data[1].val);
  }
  if (expr->kind == NY_E_BINARY && expr->as.binary.op &&
      strcmp(expr->as.binary.op, "+") == 0 &&
      !hm_refinement_operator_may_be_overloaded(hm, env, expr) &&
      expr->as.binary.left && expr->as.binary.left->kind == NY_E_IDENT &&
      expr->as.binary.left->as.ident.name &&
      strcmp(expr->as.binary.left->as.ident.name, subject) == 0 &&
      expr->as.binary.left->as.ident.syntax_ctx == subject_ctx &&
      expr->as.binary.right && expr->as.binary.right->kind == NY_E_LIST) {
    if (!hm_refinement_length_lower_bound_predicate(definition) &&
        !hm_refinement_indexed_predicate_indices_nonnegative(definition) &&
        !hm_refinement_content_predicate_survives(hm, env, definition, 0, true,
                                                  NULL))
      return false;
    return hm_refinement_region_readonly_expr(hm, env, expr->as.binary.right);
  }
  if (expr->kind == NY_E_BINARY && expr->as.binary.op &&
      strcmp(expr->as.binary.op, "+") == 0 &&
      !hm_refinement_operator_may_be_overloaded(hm, env, expr) &&
      expr->as.binary.left && expr->as.binary.left->kind == NY_E_LIST &&
      expr->as.binary.right && expr->as.binary.right->kind == NY_E_IDENT &&
      expr->as.binary.right->as.ident.name &&
      strcmp(expr->as.binary.right->as.ident.name, subject) == 0 &&
      expr->as.binary.right->as.ident.syntax_ctx == subject_ctx) {
    return hm_refinement_length_lower_bound_predicate(definition) &&
           hm_refinement_region_readonly_expr(hm, env, expr->as.binary.left);
  }
  return hm_refinement_region_readonly_expr(hm, env, expr);
}

static bool hm_refinement_preserving_element_store_stmt(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *stmt,
    const char *subject, uint32_t subject_ctx, const stmt_t *definition) {
  if (!stmt)
    return true;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!hm_refinement_preserving_element_store_stmt(
              hm, env, stmt->as.block.body.data[i], subject, subject_ctx,
              definition))
        return false;
    return true;
  case NY_S_EXPR:
    return hm_refinement_preserving_element_store(
        hm, env, stmt->as.expr.expr, subject, subject_ctx, definition, 0);
  case NY_S_RETURN:
    return hm_refinement_region_readonly_expr(hm, env, stmt->as.ret.value);
  case NY_S_VAR:
    if (!stmt->as.var.is_decl || stmt->as.var.is_del)
      return hm_refinement_nonempty_extension_assignment(hm, env, stmt, subject,
                                                         subject_ctx) &&
             hm_refinement_extension_assignment_preserves(
                 hm, env, stmt, subject, subject_ctx, definition);
    for (size_t i = 0; i < stmt->as.var.exprs.len; ++i)
      if (!hm_refinement_region_readonly_expr(hm, env,
                                              stmt->as.var.exprs.data[i]))
        return false;
    return true;
  case NY_S_IF:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.iff.init) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.iff.test) &&
           hm_refinement_preserving_element_store_stmt(
               hm, env, stmt->as.iff.conseq, subject, subject_ctx,
               definition) &&
           hm_refinement_preserving_element_store_stmt(
               hm, env, stmt->as.iff.alt, subject, subject_ctx, definition);
  case NY_S_WHILE:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.whl.init) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.whl.test) &&
           hm_refinement_preserving_element_store_stmt(
               hm, env, stmt->as.whl.body, subject, subject_ctx, definition) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.whl.update);
  case NY_S_FOR:
    return hm_refinement_region_readonly_stmt(hm, env, stmt->as.fr.init) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.fr.iterable) &&
           hm_refinement_region_readonly_expr(hm, env, stmt->as.fr.cond) &&
           hm_refinement_preserving_element_store_stmt(
               hm, env, stmt->as.fr.body, subject, subject_ctx, definition) &&
           hm_refinement_region_readonly_stmt(hm, env, stmt->as.fr.update);
  case NY_S_MATCH:
    if (!hm_refinement_region_readonly_expr(hm, env, stmt->as.match.test))
      return false;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      if (!arm || !hm_refinement_region_readonly_expr(hm, env, arm->guard))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!hm_refinement_region_readonly_expr(hm, env, arm->patterns.data[j]))
          return false;
      if (!hm_refinement_preserving_element_store_stmt(
              hm, env, arm->conseq, subject, subject_ctx, definition))
        return false;
    }
    return hm_refinement_preserving_element_store_stmt(
        hm, env, stmt->as.match.default_conseq, subject, subject_ctx,
        definition);
  default:
    return hm_refinement_region_readonly_stmt(hm, env, stmt);
  }
}

static bool hm_refinement_region_preserves_list_fact(
    ny_hm_state_t *hm, ny_hm_env_list *env, const stmt_t *definition,
    const ny_hm_env_entry_t *entry, const stmt_t *region) {
  if (!entry || !entry->name || !definition ||
      definition->kind != NY_S_REFINE ||
      !ny_generic_type_base_is(definition->as.refine.base_type, "list"))
    return false;
  return hm_refinement_preserving_element_store_stmt(
      hm, env, region, entry->name, entry->syntax_ctx, definition);
}

/*
 * A mutable aggregate may be changed before its refinement predicate is
 * checked. For that to be sound, no earlier operation may publish an alias
 * that can outlive the local mutation. This deliberately permits only local
 * expressions and recognized refinement predicates; arbitrary calls,
 * closures, and address-taking remain fail-closed. The guarded arm is checked
 * separately with the preserving-mutation walker, which admits only writes
 * whose effect on the particular list fact is proved.
 */
static bool hm_refinement_noescape_expr(ny_hm_state_t *hm, ny_hm_env_list *env,
                                        const expr_t *expr, unsigned depth) {
  if (!expr || depth > 64)
    return false;
  switch (expr->kind) {
  case NY_E_LITERAL:
  case NY_E_IDENT:
    return true;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    if (expr->kind == NY_E_LOGICAL)
      return expr->as.logical.op &&
             (strcmp(expr->as.logical.op, "&&") == 0 ||
              strcmp(expr->as.logical.op, "||") == 0) &&
             hm_refinement_noescape_expr(hm, env, expr->as.logical.left,
                                         depth + 1) &&
             hm_refinement_noescape_expr(hm, env, expr->as.logical.right,
                                         depth + 1);
    if (!expr->as.binary.op)
      return false;
    if (hm_refinement_operator_may_be_overloaded(hm, env, expr))
      return false;
    if (strcmp(expr->as.binary.op, "=") != 0 &&
        strcmp(expr->as.binary.op, "+") != 0 &&
        strcmp(expr->as.binary.op, "-") != 0 &&
        strcmp(expr->as.binary.op, "*") != 0 &&
        strcmp(expr->as.binary.op, "/") != 0 &&
        strcmp(expr->as.binary.op, "%") != 0 &&
        strcmp(expr->as.binary.op, "<") != 0 &&
        strcmp(expr->as.binary.op, "<=") != 0 &&
        strcmp(expr->as.binary.op, ">") != 0 &&
        strcmp(expr->as.binary.op, ">=") != 0 &&
        strcmp(expr->as.binary.op, "==") != 0 &&
        strcmp(expr->as.binary.op, "!=") != 0)
      return false;
    return hm_refinement_noescape_expr(hm, env, expr->as.binary.left,
                                       depth + 1) &&
           hm_refinement_noescape_expr(hm, env, expr->as.binary.right,
                                       depth + 1);
  case NY_E_UNARY:
    return expr->as.unary.op && strcmp(expr->as.unary.op, "!") == 0 &&
           hm_refinement_noescape_expr(hm, env, expr->as.unary.right,
                                       depth + 1);
  case NY_E_TERNARY:
    return hm_refinement_noescape_expr(hm, env, expr->as.ternary.cond,
                                       depth + 1) &&
           hm_refinement_noescape_expr(hm, env, expr->as.ternary.true_expr,
                                       depth + 1) &&
           hm_refinement_noescape_expr(hm, env, expr->as.ternary.false_expr,
                                       depth + 1);
  case NY_E_MEMBER:
    return hm_refinement_noescape_expr(hm, env, expr->as.member.target,
                                       depth + 1);
  case NY_E_INDEX:
    return hm_refinement_noescape_expr(hm, env, expr->as.index.target,
                                       depth + 1) &&
           hm_refinement_noescape_expr(hm, env, expr->as.index.start,
                                       depth + 1) &&
           hm_refinement_noescape_expr(hm, env, expr->as.index.stop,
                                       depth + 1) &&
           hm_refinement_noescape_expr(hm, env, expr->as.index.step, depth + 1);
  case NY_E_CALL: {
    const expr_t *callee = expr->as.call.callee;
    if (callee && callee->kind == NY_E_IDENT && callee->as.ident.name &&
        ((expr->as.call.synthetic_index_assignment &&
          strcmp(callee->as.ident.name, "set_idx") == 0 &&
          expr->as.call.args.len == 3) ||
         (strcmp(callee->as.ident.name, "append") == 0 &&
          expr->as.call.args.len == 2))) {
      for (size_t i = 0; i < expr->as.call.args.len; ++i) {
        if (!hm_refinement_noescape_expr(
                hm, env, expr->as.call.args.data[i].val, depth + 1))
          return false;
      }
      return true;
    }
    if (hm && callee && callee->kind == NY_E_IDENT && callee->as.ident.name &&
        expr->as.call.args.len == 1 &&
        hm_lookup_refine_stmt(hm, callee->as.ident.name) &&
        hm_refinement_noescape_expr(hm, env, expr->as.call.args.data[0].val,
                                    depth + 1))
      return true;
    if (!hm_refinement_region_readonly_call(hm, env, expr))
      return false;
    for (size_t i = 0; i < expr->as.call.args.len; ++i)
      if (!hm_refinement_noescape_expr(hm, env, expr->as.call.args.data[i].val,
                                       depth + 1))
        return false;
    return true;
  }
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < expr->as.list_like.len; ++i)
      if (!hm_refinement_noescape_expr(hm, env, expr->as.list_like.data[i],
                                       depth + 1))
        return false;
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < expr->as.dict.pairs.len; ++i)
      if (!hm_refinement_noescape_expr(hm, env, expr->as.dict.pairs.data[i].key,
                                       depth + 1) ||
          !hm_refinement_noescape_expr(
              hm, env, expr->as.dict.pairs.data[i].value, depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

static bool hm_refinement_noescape_stmt(ny_hm_state_t *hm, ny_hm_env_list *env,
                                        const stmt_t *stmt) {
  if (!stmt)
    return true;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!hm_refinement_noescape_stmt(hm, env, stmt->as.block.body.data[i]))
        return false;
    return true;
  case NY_S_VAR:
    if (stmt->as.var.is_del)
      return false;
    for (size_t i = 0; i < stmt->as.var.exprs.len; ++i)
      if (!hm_refinement_noescape_expr(hm, env, stmt->as.var.exprs.data[i], 0))
        return false;
    return true;
  case NY_S_EXPR:
    return hm_refinement_noescape_expr(hm, env, stmt->as.expr.expr, 0);
  case NY_S_RETURN:
    return hm_refinement_noescape_expr(hm, env, stmt->as.ret.value, 0);
  case NY_S_IF:
    return hm_refinement_noescape_stmt(hm, env, stmt->as.iff.init) &&
           hm_refinement_noescape_expr(hm, env, stmt->as.iff.test, 0) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.iff.conseq) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.iff.alt);
  case NY_S_WHILE:
    return hm_refinement_noescape_stmt(hm, env, stmt->as.whl.init) &&
           hm_refinement_noescape_expr(hm, env, stmt->as.whl.test, 0) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.whl.body) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.whl.update);
  case NY_S_FOR:
    return hm_refinement_noescape_stmt(hm, env, stmt->as.fr.init) &&
           hm_refinement_noescape_expr(hm, env, stmt->as.fr.iterable, 0) &&
           hm_refinement_noescape_expr(hm, env, stmt->as.fr.cond, 0) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.fr.body) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.fr.update);
  case NY_S_TRY:
    return hm_refinement_noescape_stmt(hm, env, stmt->as.tr.body) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.tr.handler);
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return true;
  default:
    return false;
  }
}

/*
 * A mutable local is eligible for a refinement guard only if the statements
 * on the path to that guard have not published an alias. Do not inspect code
 * after the guard here: the selected arm is checked independently, and a
 * later read-only consumer must not erase the fact established by the guard.
 */
static bool hm_refinement_noescape_to_guard(ny_hm_state_t *hm,
                                            ny_hm_env_list *env,
                                            const stmt_t *stmt,
                                            const expr_t *guard, bool *found) {
  if (!stmt || !guard || !found || *found)
    return true;
  if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len && !*found; ++i)
      if (!hm_refinement_noescape_to_guard(hm, env, stmt->as.block.body.data[i],
                                           guard, found))
        return false;
    return true;
  }
  if (stmt->kind == NY_S_IF) {
    if (!hm_refinement_noescape_stmt(hm, env, stmt->as.iff.init) ||
        !hm_refinement_noescape_expr(hm, env, stmt->as.iff.test, 0))
      return false;
    if (stmt->as.iff.test == guard) {
      *found = true;
      return true;
    }
    bool found_in_conseq = false;
    bool found_in_alt = false;
    if (!hm_refinement_noescape_to_guard(hm, env, stmt->as.iff.conseq, guard,
                                         &found_in_conseq) ||
        !hm_refinement_noescape_to_guard(hm, env, stmt->as.iff.alt, guard,
                                         &found_in_alt))
      return false;
    if (found_in_conseq || found_in_alt) {
      *found = true;
      return true;
    }
    return hm_refinement_noescape_stmt(hm, env, stmt->as.iff.conseq) &&
           hm_refinement_noescape_stmt(hm, env, stmt->as.iff.alt);
  }
  if (stmt->kind == NY_S_WHILE && stmt->as.whl.test == guard) {
    if (!hm_refinement_noescape_stmt(hm, env, stmt->as.whl.init) ||
        !hm_refinement_noescape_expr(hm, env, stmt->as.whl.test, 0))
      return false;
    *found = true;
    return true;
  }
  if (stmt->kind == NY_S_FOR && stmt->as.fr.cond == guard) {
    if (!hm_refinement_noescape_stmt(hm, env, stmt->as.fr.init) ||
        !hm_refinement_noescape_expr(hm, env, stmt->as.fr.iterable, 0) ||
        !hm_refinement_noescape_expr(hm, env, stmt->as.fr.cond, 0))
      return false;
    *found = true;
    return true;
  }
  return hm_refinement_noescape_stmt(hm, env, stmt);
}

static bool hm_refinement_noescape_before_guard(ny_hm_state_t *hm,
                                                ny_hm_env_list *env,
                                                const stmt_t *body,
                                                const expr_t *guard) {
  bool found = false;
  bool safe = hm_refinement_noescape_to_guard(hm, env, body, guard, &found);
  return safe && found;
}

typedef struct {
  const char *name;
  uint32_t syntax_ctx;
} hm_refinement_solver_ident_t;

static bool hm_refinement_solver_expr_safe(const expr_t *expr,
                                           hm_refinement_solver_ident_t *idents,
                                           size_t *ident_count, unsigned depth,
                                           const char *formal,
                                           uint32_t formal_ctx) {
  if (!expr || depth > 64)
    return false;
  switch (expr->kind) {
  case NY_E_IDENT:
    if (!expr->as.ident.name || !*expr->as.ident.name || !idents ||
        !ident_count || *ident_count >= 16 ||
        (formal && strcmp(expr->as.ident.name, formal) == 0 &&
         expr->as.ident.syntax_ctx != formal_ctx))
      return false;
    for (size_t i = 0; i < *ident_count; ++i)
      if (strcmp(idents[i].name, expr->as.ident.name) == 0)
        return idents[i].syntax_ctx == expr->as.ident.syntax_ctx;
    idents[*ident_count] = (hm_refinement_solver_ident_t){
        expr->as.ident.name, expr->as.ident.syntax_ctx};
    (*ident_count)++;
    return true;
  case NY_E_LITERAL:
    return expr->as.literal.kind == NY_LIT_BOOL ||
           (expr->as.literal.kind == NY_LIT_INT &&
            !expr->as.literal.integer_outside_i64);
  case NY_E_UNARY:
    return expr->as.unary.op &&
           (strcmp(expr->as.unary.op, "!") == 0 ||
            strcmp(expr->as.unary.op, "-") == 0) &&
           hm_refinement_solver_expr_safe(expr->as.unary.right, idents,
                                          ident_count, depth + 1, formal,
                                          formal_ctx);
  case NY_E_LOGICAL:
    return expr->as.logical.op &&
           (strcmp(expr->as.logical.op, "&&") == 0 ||
            strcmp(expr->as.logical.op, "||") == 0) &&
           hm_refinement_solver_expr_safe(expr->as.logical.left, idents,
                                          ident_count, depth + 1, formal,
                                          formal_ctx) &&
           hm_refinement_solver_expr_safe(expr->as.logical.right, idents,
                                          ident_count, depth + 1, formal,
                                          formal_ctx);
  case NY_E_BINARY: {
    static const char *const operators[] = {"+",  "-", "*",  "/",  "%", "<",
                                            "<=", ">", ">=", "==", "!="};
    bool supported = false;
    for (size_t i = 0;
         expr->as.binary.op && i < sizeof(operators) / sizeof(operators[0]);
         ++i)
      if (strcmp(expr->as.binary.op, operators[i]) == 0) {
        supported = true;
        break;
      }
    return supported &&
           hm_refinement_solver_expr_safe(expr->as.binary.left, idents,
                                          ident_count, depth + 1, formal,
                                          formal_ctx) &&
           hm_refinement_solver_expr_safe(expr->as.binary.right, idents,
                                          ident_count, depth + 1, formal,
                                          formal_ctx);
  }
  default:
    return false;
  }
}

/*
 * Ask the bounded, overflow-aware contract solver whether a pure branch
 * condition implies the refinement predicate. Unsupported syntax, side
 * effects, and hygienically ambiguous identifiers stay fail-closed.
 */
static bool hm_refinement_guard_proves_predicate(
    ny_hm_state_t *hm, ny_hm_env_list *env, const expr_t *predicate,
    const char *formal, uint32_t formal_ctx, const expr_t *guard,
    const char *subject, uint32_t subject_ctx, bool negate) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !predicate || !formal || !guard ||
      !subject || !hm_refinement_region_readonly_expr(hm, env, guard))
    return false;
  hm_refinement_solver_ident_t idents[16];
  size_t ident_count = 0;
  if (!hm_refinement_solver_expr_safe(predicate, idents, &ident_count, 0,
                                      formal, formal_ctx))
    return false;
  expr_t subject_expr = {.kind = NY_E_IDENT, .tok = guard->tok};
  subject_expr.as.ident.name = (char *)subject;
  subject_expr.as.ident.syntax_ctx = subject_ctx;
  expr_t *inst =
      ny_expr_subst(&hm->arena, (expr_t *)predicate, formal, &subject_expr);
  if (!inst)
    return false;
  ident_count = 0;
  if (!hm_refinement_solver_expr_safe(inst, idents, &ident_count, 0, NULL, 0) ||
      !hm_refinement_solver_expr_safe(guard, idents, &ident_count, 0, NULL, 0))
    return false;

  expr_t not_guard = {.kind = NY_E_UNARY, .tok = guard->tok};
  not_guard.as.unary.op = "!";
  not_guard.as.unary.right = (expr_t *)guard;
  expr_t implication = {.kind = NY_E_LOGICAL, .tok = guard->tok};
  implication.as.logical.op = "||";
  implication.as.logical.left = negate ? (expr_t *)guard : &not_guard;
  implication.as.logical.right = inst;
  char counterexample[128] = {0};
  return ny_proof_try_contract_solver(hm->ctx->cg, NULL, 0, &implication,
                                      counterexample, sizeof(counterexample));
}

static expr_t *hm_refinement_full_predicate(ny_hm_state_t *hm,
                                            stmt_t *definition) {
  if (!hm || !definition || !definition->as.refine.param ||
      !definition->as.refine.predicate)
    return NULL;
  expr_t *predicate = definition->as.refine.predicate;
  uint32_t formal_ctx = 0;
  if (!hm_refinement_formal_context(predicate, definition->as.refine.param,
                                    &formal_ctx))
    return NULL;
  const char *base_name = definition->as.refine.base_type;
  unsigned depth = 0;
  while (base_name && *base_name && depth++ < 32) {
    stmt_t *base = hm_lookup_refine_stmt(hm, base_name);
    if (!base)
      break;
    if (!base->as.refine.param || !base->as.refine.predicate)
      return NULL;
    expr_t formal = {.kind = NY_E_IDENT, .tok = definition->tok};
    formal.as.ident.name = definition->as.refine.param;
    formal.as.ident.syntax_ctx = formal_ctx;
    expr_t *base_predicate = ny_expr_subst(
        &hm->arena, base->as.refine.predicate, base->as.refine.param, &formal);
    expr_t *conjunction =
        (expr_t *)arena_alloc(&hm->arena, sizeof(*conjunction));
    if (!base_predicate || !conjunction)
      return NULL;
    memset(conjunction, 0, sizeof(*conjunction));
    conjunction->kind = NY_E_LOGICAL;
    conjunction->tok = definition->tok;
    conjunction->as.logical.op = "&&";
    conjunction->as.logical.left = predicate;
    conjunction->as.logical.right = base_predicate;
    predicate = conjunction;
    base_name = base->as.refine.base_type;
  }
  return predicate;
}

static bool
hm_refinement_candidate_matches_guard(ny_hm_state_t *hm, ny_hm_env_list *env,
                                      stmt_t *candidate, const expr_t *guard,
                                      const char *subject, uint32_t subject_ctx,
                                      bool negate) {
  if (!candidate || !candidate->as.refine.name || !candidate->as.refine.param)
    return false;
  uint32_t formal_ctx = 0;
  if (!hm_refinement_formal_context(candidate->as.refine.predicate,
                                    candidate->as.refine.param, &formal_ctx))
    return false;
  expr_t *predicate = hm_refinement_full_predicate(hm, candidate);
  return predicate && (hm_refinement_guard_implies_predicate(
                           predicate, candidate->as.refine.param, formal_ctx,
                           guard, subject, subject_ctx, negate) ||
                       hm_refinement_guard_proves_predicate(
                           hm, env, predicate, candidate->as.refine.param,
                           formal_ctx, guard, subject, subject_ctx, negate));
}

/*
 * Prefer the strongest matching nominal refinement only when its predicate
 * implies every weaker match. Incomparable or equivalent declarations remain
 * ambiguous and fail closed.
 */
static int hm_refinement_compare_candidates(ny_hm_state_t *hm,
                                            ny_hm_env_list *env,
                                            stmt_t *candidate, stmt_t *current,
                                            const char *subject,
                                            uint32_t subject_ctx) {
  if (!hm || !candidate || !current || !subject)
    return 0;
  expr_t *candidate_predicate = hm_refinement_full_predicate(hm, candidate);
  expr_t *current_predicate = hm_refinement_full_predicate(hm, current);
  uint32_t candidate_ctx = 0;
  uint32_t current_ctx = 0;
  if (!candidate_predicate || !current_predicate ||
      !hm_refinement_formal_context(candidate->as.refine.predicate,
                                    candidate->as.refine.param,
                                    &candidate_ctx) ||
      !hm_refinement_formal_context(current->as.refine.predicate,
                                    current->as.refine.param, &current_ctx))
    return 0;
  expr_t subject_ident = {.kind = NY_E_IDENT, .tok = candidate->tok};
  subject_ident.as.ident.name = (char *)subject;
  subject_ident.as.ident.syntax_ctx = subject_ctx;
  expr_t *candidate_guard =
      ny_expr_subst(&hm->arena, candidate_predicate, candidate->as.refine.param,
                    &subject_ident);
  expr_t *current_guard = ny_expr_subst(
      &hm->arena, current_predicate, current->as.refine.param, &subject_ident);
  if (!candidate_guard || !current_guard)
    return 0;
  bool candidate_implies_current = hm_refinement_guard_proves_predicate(
      hm, env, current_predicate, current->as.refine.param, current_ctx,
      candidate_guard, subject, subject_ctx, false);
  bool current_implies_candidate = hm_refinement_guard_proves_predicate(
      hm, env, candidate_predicate, candidate->as.refine.param, candidate_ctx,
      current_guard, subject, subject_ctx, false);
  if (candidate_implies_current && !current_implies_candidate)
    return 1;
  if (current_implies_candidate && !candidate_implies_current)
    return -1;
  return 0;
}

static bool hm_refinement_literal_aggregate(const expr_t *expr,
                                            unsigned depth) {
  if (!expr || depth > 64)
    return false;
  switch (expr->kind) {
  case NY_E_LITERAL:
    return true;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < expr->as.list_like.len; ++i)
      if (!hm_refinement_literal_aggregate(expr->as.list_like.data[i],
                                           depth + 1))
        return false;
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < expr->as.dict.pairs.len; ++i)
      if (!hm_refinement_literal_aggregate(expr->as.dict.pairs.data[i].key,
                                           depth + 1) ||
          !hm_refinement_literal_aggregate(expr->as.dict.pairs.data[i].value,
                                           depth + 1))
        return false;
    return true;
  default:
    return false;
  }
}

/*
 * A mutable aggregate can carry a branch refinement only when it was freshly
 * built from literals in this function. The whole function must separately
 * pass the read-only AST walk, which excludes aliases escaping through calls,
 * closures, pointer operations, or writes. Restricting this to a unique
 * top-level declaration avoids confusing shadowed lexical bindings.
 */
static bool
hm_refinement_fresh_mutable_aggregate(const ny_hm_state_t *hm,
                                      const ny_hm_env_entry_t *entry) {
  const stmt_t *body = hm ? hm->active_fn_body : NULL;
  if (!body || body->kind != NY_S_BLOCK || !entry || !entry->name)
    return false;
  const expr_t *initializer = NULL;
  size_t matches = 0;
  for (size_t i = 0; i < body->as.block.body.len; ++i) {
    const stmt_t *stmt = body->as.block.body.data[i];
    if (!stmt || stmt->kind != NY_S_VAR || !stmt->as.var.is_decl ||
        !stmt->as.var.is_mut || stmt->syntax_ctx != entry->syntax_ctx)
      continue;
    for (size_t j = 0; j < stmt->as.var.names.len; ++j) {
      if (!stmt->as.var.names.data[j] ||
          strcmp(stmt->as.var.names.data[j], entry->name) != 0)
        continue;
      matches++;
      if (j < stmt->as.var.exprs.len)
        initializer = stmt->as.var.exprs.data[j];
    }
  }
  return matches == 1 && hm_refinement_literal_aggregate(initializer, 0) &&
         initializer->kind != NY_E_LITERAL;
}

/*
 * Apply a refinement predicate as a lexical fact for the selected arm.
 * Direct predicate calls remain supported for true arms. Relational
 * transport admits exact literal/formal matches and equivalent operand-order
 * swaps for comparisons, optionally as a positive conjunction operand or a
 * complemented disjunction operand. Explicit `!` polarity is propagated.
 * Mutable lists are admitted when the selected region is read-only or every
 * mutation in it is proved to preserve the predicate (including exact
 * same-place self-stores). The enclosing function must still pass the
 * no-escape walk.
 */
static ny_hm_env_entry_t *
hm_refinement_guard_entry(ny_hm_state_t *hm, ny_hm_env_list *env, expr_t *test,
                          ny_hm_type_t **refined_type, bool negate,
                          const stmt_t *region) {
  if (refined_type)
    *refined_type = NULL;
  if (!hm || !env || !test)
    return NULL;
  bool region_readonly = hm_refinement_region_readonly_stmt(hm, env, region);
  bool function_noescape =
      hm_refinement_noescape_before_guard(hm, env, hm->active_fn_body, test);
  stmt_t *definition = NULL;
  expr_t *arg = NULL;
  ny_hm_env_entry_t *entry = NULL;
  const char *refine_name = NULL;
  bool ambiguous_definition = false;
  if (!negate && test->kind == NY_E_CALL && test->as.call.callee &&
      test->as.call.callee->kind == NY_E_IDENT && test->as.call.args.len == 1) {
    expr_t *callee = test->as.call.callee;
    arg = test->as.call.args.data[0].val;
    refine_name = callee->as.ident.name;
    definition = refine_name ? hm_lookup_refine_stmt(hm, refine_name) : NULL;
    if (!arg || arg->kind != NY_E_IDENT)
      return NULL;
  } else if (test->kind == NY_E_BINARY || test->kind == NY_E_LOGICAL ||
             (test->kind == NY_E_UNARY && test->as.unary.op &&
              strcmp(test->as.unary.op, "!") == 0)) {
    /*
     * Find a unique refinement whose declared predicate is exactly this
     * guard after replacing its formal with one immutable local.
     */
    if (!hm->ctx)
      return NULL;
    codegen_t *cg = hm->ctx->cg;
    size_t search_count = cg ? 1 + cg->extra_progs.len : 1;
    for (size_t pi = 0; pi < search_count; ++pi) {
      program_t *search_prog =
          cg ? (pi == 0 ? cg->prog : cg->extra_progs.data[pi - 1])
             : hm->ctx->prog;
      if (!search_prog || (pi == 0 && cg && !cg->prog))
        continue;
      bool already_searched = false;
      for (size_t previous = 0; previous < pi; ++previous) {
        program_t *previous_prog =
            cg ? (previous == 0 ? cg->prog : cg->extra_progs.data[previous - 1])
               : hm->ctx->prog;
        if (previous_prog == search_prog) {
          already_searched = true;
          break;
        }
      }
      if (already_searched)
        continue;
      for (size_t i = 0; i < search_prog->body.len; ++i) {
        stmt_t *candidate = search_prog->body.data[i];
        if (!candidate || candidate->kind != NY_S_REFINE ||
            !candidate->as.refine.name || !candidate->as.refine.param ||
            !candidate->as.refine.predicate)
          continue;
        /*
         * The subject is derived from the predicate's sole formal occurrence;
         * try visible immutable locals and require a unique structural match
         * across the current and imported programs.
         */
        for (size_t j = 0; j < env->len; ++j) {
          ny_hm_env_entry_t *local = &env->data[j];
          bool mutable_region_ok = !local->mutable_value || region_readonly ||
                                   hm_refinement_region_preserves_list_fact(
                                       hm, env, candidate, local, region);
          if (!local->name || !local->type ||
              (local->mutable_value &&
               (!mutable_region_ok || !function_noescape)))
            continue;
          if (!hm_refinement_candidate_matches_guard(hm, env, candidate, test,
                                                     local->name,
                                                     local->syntax_ctx, negate))
            continue;
          if (!definition) {
            definition = candidate;
            entry = local;
          } else if (definition != candidate) {
            if (entry != local) {
              ambiguous_definition = true;
              break;
            }
            int comparison = hm_refinement_compare_candidates(
                hm, env, candidate, definition, local->name, local->syntax_ctx);
            if (comparison == 0) {
              ambiguous_definition = true;
              break;
            }
            if (comparison > 0)
              definition = candidate;
          }
          refine_name = definition->as.refine.name;
        }
        if (ambiguous_definition)
          break;
      }
      if (ambiguous_definition)
        break;
    }
    /*
     * Imported refinements can be registered from module interfaces without
     * retaining their source program in cg->extra_progs. Include the canonical
     * type registry, deduplicating by declaration pointer so the ambiguity
     * check still rejects genuinely distinct matching refinements.
     */
    if (cg) {
      size_t registered_count = cg->refines.len + cg->registry.refines.len;
      for (size_t ri = 0; ri < registered_count; ++ri) {
        refine_def_t *ref =
            ri < cg->refines.len
                ? cg->refines.data[ri]
                : cg->registry.refines.data[ri - cg->refines.len];
        stmt_t *candidate = ref ? ref->stmt : NULL;
        if (!candidate || !candidate->as.refine.name ||
            !candidate->as.refine.param || !candidate->as.refine.predicate)
          continue;
        for (size_t j = 0; j < env->len; ++j) {
          ny_hm_env_entry_t *local = &env->data[j];
          if (!local->name || !local->type ||
              (local->mutable_value &&
               ((!region_readonly && !hm_refinement_region_preserves_list_fact(
                                         hm, env, candidate, local, region)) ||
                !function_noescape)))
            continue;
          if (!hm_refinement_candidate_matches_guard(hm, env, candidate, test,
                                                     local->name,
                                                     local->syntax_ctx, negate))
            continue;
          if (!definition) {
            definition = candidate;
            entry = local;
          } else if (definition != candidate) {
            if (entry != local) {
              ambiguous_definition = true;
              break;
            }
            int comparison = hm_refinement_compare_candidates(
                hm, env, candidate, definition, local->name, local->syntax_ctx);
            if (comparison == 0) {
              ambiguous_definition = true;
              break;
            }
            if (comparison > 0)
              definition = candidate;
          }
          refine_name = definition->as.refine.name;
        }
        if (ambiguous_definition)
          break;
      }
    }
    if (ambiguous_definition)
      return NULL;
    if (!definition)
      return NULL;
  } else {
    return NULL;
  }
  if (!definition)
    return NULL;
  if (arg) {
    entry = hm_env_entry_ctx(env, arg->as.ident.name, arg->as.ident.syntax_ctx);
  }
  if (!entry ||
      (entry->mutable_value &&
       ((!region_readonly && !hm_refinement_region_preserves_list_fact(
                                 hm, env, definition, entry, region)) ||
        !function_noescape)) ||
      !entry->type)
    return NULL;
  ny_hm_type_t *current = hm_prune(entry->type);
  if (!current)
    return NULL;
  char *current_spelling = NULL;
  const char *current_name = current->kind == NY_HM_NAME ? current->name : NULL;
  if (!current_name &&
      (current->kind == NY_HM_LIST || current->kind == NY_HM_SET ||
       current->kind == NY_HM_TUPLE || current->kind == NY_HM_DICT)) {
    current_spelling = hm_type_string(current);
    current_name = current_spelling;
  }
  if (!current_name)
    return NULL;
  const char *current_base = hm_resolve_refine_root_base(hm, current_name);
  if (!current_base)
    current_base = current_name;
  const char *required_base = hm_resolve_refine_root_base(hm, refine_name);
  if (!current_base || !required_base ||
      (strcmp(current_base, required_base) != 0 &&
       !ny_generic_type_base_is(current_base, required_base))) {
    free(current_spelling);
    return NULL;
  }
  /*
   * Scalar bindings have no object alias. For aggregates require a fresh
   * literal initializer plus a whole-function no-escape proof.
   */
  if (entry->mutable_value) {
    ny_sem_rep_t rep = hm_semantic_rep(current_base);
    if (rep != NY_SEM_REP_RAW_INT && rep != NY_SEM_REP_F32 &&
        rep != NY_SEM_REP_F64 &&
        !hm_refinement_fresh_mutable_aggregate(hm, entry)) {
      free(current_spelling);
      return NULL;
    }
  }
  free(current_spelling);
  if (refined_type)
    *refined_type = hm_parse_type_name(hm, refine_name, NULL);
  return refined_type && *refined_type ? entry : NULL;
}

static expr_t *hm_module_expr_root_ident(expr_t *e) {
  if (!e)
    return NULL;
  while (e && e->kind == NY_E_MEMBER)
    e = e->as.member.target;
  return (e && e->kind == NY_E_IDENT) ? e : NULL;
}

static bool hm_declares_inline_module(ny_hm_state_t *hm, const char *name) {
  if (!hm || !hm->ctx || !hm->ctx->prog || !name || !*name)
    return false;
  for (size_t i = 0; i < hm->ctx->prog->body.len; ++i) {
    stmt_t *s = hm->ctx->prog->body.data[i];
    if (s && s->kind == NY_S_MODULE && s->as.module.name &&
        strcmp(s->as.module.name, name) == 0)
      return true;
  }
  return false;
}

static bool hm_resolve_module_expr_path(ny_hm_state_t *hm, ny_hm_env_list *env,
                                        expr_t *e, char *out, size_t out_cap) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !e || !out || out_cap == 0)
    return false;
  expr_t *root_ident = hm_module_expr_root_ident(e);
  if (!root_ident)
    return false;
  const char *root = root_ident->as.ident.name;
  /*
   * Lexical values shadow imported aliases, including aliases collected
   * from unrelated modules in the flattened program.
   */
  ny_hm_type_t *root_type =
      hm_env_get_ctx(env, root, root_ident->as.ident.syntax_ctx);
  if (root_type &&
      (!hm_is_any(root_type) || !hm_declares_inline_module(hm, root))) {
    /*
     * A lexical binding shadows aliases, but an inline module may itself
     * have a dotted name below that binding's spelling (for example
     * `module core.math.float` while `core` is also the stdlib root). Permit
     * only a path that resolves to a module declared in this source program.
     */
    if (!e || e->kind != NY_E_MEMBER)
      return false;
    char *root_type_name = hm_type_string(root_type);
    const char *module_alias = ny_lookup_module_alias(
        hm->ctx->cg, NULL, 0, root, strlen(root), root_ident->as.ident.hash);
    if (!module_alias || !*module_alias)
      module_alias = resolve_import_alias(hm->ctx->cg, root);
    bool is_module_alias = root_type_name && module_alias && *module_alias &&
                           strcmp(root_type_name, module_alias) == 0;
    free(root_type_name);
    if (!is_module_alias)
      return false;
    char inline_module[1024];
    if (!tp_resolve_module_expr_path(hm->ctx->cg, e, inline_module,
                                     sizeof(inline_module)) ||
        (!ny_module_path_exists(hm->ctx->cg, inline_module) &&
         !ny_module_path_has_descendant(hm->ctx->cg, inline_module)))
      return false;
    size_t n = strlen(inline_module);
    if (n >= out_cap)
      return false;
    memcpy(out, inline_module, n + 1);
    return true;
  }
  return tp_resolve_module_expr_path(hm->ctx->cg, e, out, out_cap);
}

static bool hm_member_root_is_unresolved_ident(ny_hm_state_t *hm,
                                               ny_hm_env_list *env,
                                               expr_t *target) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !target)
    return false;
  expr_t *root_ident = hm_module_expr_root_ident(target);
  if (!root_ident)
    return false;
  const char *root = root_ident->as.ident.name;
  ny_hm_type_t *root_type =
      hm_env_get_ctx(env, root, root_ident->as.ident.syntax_ctx);
  /*
   * A lexical `any` binding is still a value.  Treating it as an unresolved
   * module name skips dynamic-member diagnostics in strict mode.
   */
  if (root_type)
    return false;
  if (ny_module_path_exists(hm->ctx->cg, root))
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

static uint64_t hm_scheme_hash(const char *name) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (const unsigned char *p = (const unsigned char *)name; p && *p; ++p) {
    h ^= *p;
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static void hm_scheme_index_put(ny_hm_state_t *hm, const char *name,
                                size_t scheme_index) {
  if (!hm || !name || !*name || !hm->scheme_index_cap)
    return;
  size_t mask = hm->scheme_index_cap - 1;
  size_t slot = (size_t)hm_scheme_hash(name) & mask;
  while (hm->scheme_index[slot].name &&
         strcmp(hm->scheme_index[slot].name, name) != 0)
    slot = (slot + 1) & mask;
  hm->scheme_index[slot].name = name;
  hm->scheme_index[slot].scheme_plus_one = scheme_index + 1;
}

static ny_hm_scheme_t *hm_find_scheme(ny_hm_state_t *hm, const char *name) {
  if (!hm || !name)
    return NULL;
  if (hm->scheme_index_cap) {
    size_t mask = hm->scheme_index_cap - 1;
    size_t slot = (size_t)hm_scheme_hash(name) & mask;
    while (hm->scheme_index[slot].name) {
      if (strcmp(hm->scheme_index[slot].name, name) == 0) {
        size_t index = hm->scheme_index[slot].scheme_plus_one - 1;
        return index < hm->schemes.len ? &hm->schemes.data[index] : NULL;
      }
      slot = (slot + 1) & mask;
    }
    return NULL;
  }
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

typedef struct hm_sigma_parts_t {
  char *index_type;
  char *binder;
  char *value_type;
} hm_sigma_parts_t;

/*
 * Split `exists(IndexType binder) ValueType` into its three authored pieces.
 * Generic argument brackets are tracked so an index type such as `Fin<8>`
 * cannot be mistaken for the closing binder parenthesis.
 */
static bool hm_sigma_parts_parse(const char *decl, hm_sigma_parts_t *out) {
  if (!decl || !out)
    return false;
  while (*decl == ' ' || *decl == '\t')
    decl++;
  if (strncmp(decl, "exists(", 7) != 0)
    return false;
  const char *body = decl + 7;
  const char *close = NULL;
  int depth = 0;
  for (const char *p = body; *p; ++p) {
    if (*p == '<') {
      depth++;
    } else if (*p == '>') {
      if (depth > 0)
        depth--;
    } else if (*p == ')' && depth == 0) {
      close = p;
      break;
    }
  }
  if (!close)
    return false;
  const char *binder_end = close;
  while (binder_end > body && (binder_end[-1] == ' ' || binder_end[-1] == '\t'))
    binder_end--;
  const char *binder_start = binder_end;
  while (binder_start > body &&
         (isalnum((unsigned char)binder_start[-1]) || binder_start[-1] == '_'))
    binder_start--;
  if (binder_start == binder_end ||
      !(isalpha((unsigned char)*binder_start) || *binder_start == '_'))
    return false;
  const char *value_start = close + 1;
  while (*value_start == ' ' || *value_start == '\t')
    value_start++;
  if (!*value_start)
    return false;
  char *index = hm_trim_owned(body, (size_t)(binder_start - body));
  char *binder = ny_strndup(binder_start, (size_t)(binder_end - binder_start));
  char *value = ny_strdup(value_start);
  if (!index || !*index || !binder || !value) {
    free(index);
    free(binder);
    free(value);
    return false;
  }
  out->index_type = index;
  out->binder = binder;
  out->value_type = value;
  return true;
}

static void hm_sigma_parts_free(hm_sigma_parts_t *parts) {
  if (!parts)
    return;
  free(parts->index_type);
  free(parts->binder);
  free(parts->value_type);
  parts->index_type = NULL;
  parts->binder = NULL;
  parts->value_type = NULL;
}

/*
 * Resolve a short enum name in its declaring module. ADT constructors carry
 * the canonical module-qualified identity, so function annotations must use
 * the same identity when their generic payload contains a local enum.
 */
static char *hm_owner_enum_name(ny_hm_state_t *hm, const char *owner,
                                const char *short_name) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !owner || !*owner || !short_name ||
      !*short_name || strchr(short_name, '.'))
    return NULL;
  size_t owner_len = strlen(owner);
  size_t type_len = strlen(short_name);
  if (owner_len > SIZE_MAX - type_len - 2)
    return NULL;
  char *qualified = malloc(owner_len + type_len + 2);
  if (!qualified)
    return NULL;
  memcpy(qualified, owner, owner_len);
  qualified[owner_len] = '.';
  memcpy(qualified + owner_len + 1, short_name, type_len + 1);
  for (size_t i = 0; i < hm->ctx->cg->enums.len; ++i) {
    enum_def_t *def = hm->ctx->cg->enums.data[i];
    if (def && def->name && strcmp(def->name, qualified) == 0)
      return qualified;
  }
  free(qualified);
  return NULL;
}

static char *hm_qualify_nested_type_name(ny_hm_state_t *hm, const char *owner,
                                         const char *raw) {
  if (!raw || !*raw)
    return NULL;
  const char *lt = strchr(raw, '<');
  if (!lt) {
    char *enum_name = hm_owner_enum_name(hm, owner, raw);
    return enum_name ? enum_name : ny_strdup(raw);
  }
  const char *gt = strrchr(raw, '>');
  if (!gt || gt < lt)
    return ny_strdup(raw);
  char *base = hm_trim_owned(raw, (size_t)(lt - raw));
  char *enum_base = hm_owner_enum_name(hm, owner, base);
  if (enum_base) {
    free(base);
    base = enum_base;
  }
  ny_tp_json_t out = {0};
  tp_append(&out, "%s<", base ? base : "");
  free(base);
  const char *part = lt + 1;
  while (part <= gt) {
    const char *comma = hm_find_top_comma(part);
    if (!comma || comma > gt)
      comma = gt;
    char *piece = hm_trim_owned(part, (size_t)(comma - part));
    char *qualified = hm_qualify_nested_type_name(hm, owner, piece);
    tp_append(&out, "%s%s", part == lt + 1 ? "" : ", ",
              qualified ? qualified : (piece ? piece : ""));
    free(qualified);
    free(piece);
    if (comma == gt)
      break;
    part = comma + 1;
  }
  tp_append(&out, ">");
  return tp_take(&out, "");
}

static ny_hm_type_t *hm_parse_type_name(ny_hm_state_t *hm, const char *raw,
                                        const char *self_name) {
  if (!raw || !*raw)
    return hm_var(hm);
  while (*raw == ' ' || *raw == '\t')
    raw++;
  for (size_t i = 0; hm && i < hm->tvar_count; ++i) {
    size_t n = strlen(hm->tvar_names[i]);
    if (strncmp(raw, hm->tvar_names[i], n) == 0) {
      const char *rest = raw + n;
      while (*rest == ' ' || *rest == '\t')
        rest++;
      if (*rest == '\0')
        return hm->tvar_types[i];
    }
  }
  /*
   * An indexed proof type is one atomic name: its canonical proposition may
   * contain `<`/`>` operators (`proof<binary:<(name:a,int:10)>`), which the
   * generic-argument parser below would split, collapsing every such
   * proposition to the same mangled `proof<binary:<>>`.
   */
  if (strncmp(raw, "proof<", 6) == 0) {
    size_t rl = strlen(raw);
    while (rl > 0 && (raw[rl - 1] == ' ' || raw[rl - 1] == '\t'))
      --rl;
    if (rl > 7 && raw[rl - 1] == '>') {
      char *exact = ny_strndup(raw, rl);
      char *compact = hm_proof_compact(exact);
      ny_hm_type_t *t = hm_name(hm, compact ? compact : exact);
      free(compact);
      free(exact);
      return t;
    }
  }
  /*
   * A dependent pair is represented by its erased two-element tuple shape.
   * The `exists` annotation itself is retained on the binding, where the
   * construction check and projections consult it for the index and payload
   * types.
   */
  {
    hm_sigma_parts_t sigma;
    if (hm_sigma_parts_parse(raw, &sigma)) {
      hm_sigma_parts_free(&sigma);
      return hm_unary(hm, NY_HM_TUPLE, hm_any(hm));
    }
  }
  if (strcmp(raw, "self") == 0 && self_name && *self_name)
    return hm_name(hm, self_name);
  /*
   * Type values erase to their canonical type-name string, so the `Type`
   * universe has the same runtime representation as `str`.
   */
  {
    const char *leaf = ny_name_leaf(raw);
    /*
     * `Type` and its leveled spellings (`Type 3`) are the same erased value;
     * the level is a static index, not runtime data.
     */
    bool universe = leaf && strncmp(leaf, "Type", 4) == 0;
    if (universe && leaf[4] == ' ') {
      /* `Type 3`, a level variable `Type u`, or `Type max(u,3)`. */
      const char *d = leaf + 5;
      universe = *d != '\0';
      for (; universe && *d; ++d)
        if (!(isalnum((unsigned char)*d) || *d == '_' || *d == '(' ||
              *d == ')' || *d == ','))
          universe = false;
    } else if (universe) {
      universe = leaf[4] == '\0';
    }
    if (universe)
      return hm_name(hm, "str");
  }
  if (raw[0] == '?')
    return hm_unary(hm, NY_HM_NULLABLE,
                    hm_parse_type_name(hm, raw + 1, self_name));
  if (raw[0] == '*')
    return hm_unary(hm, NY_HM_PTR, hm_parse_type_name(hm, raw + 1, self_name));
  /*
   * Imported nominal type spellings are aliases for their declaration's
   * canonical identity. Resolve them here as well as at value-call sites, so
   * a re-exported constructor result unifies with methods declared on the
   * original module-owned type. Restrict this to known layouts: function and
   * value aliases must not change ordinary type-name interpretation.
   */
  if (!strchr(raw, '<') && !strchr(raw, '.')) {
    const char *resolved = hm && hm->ctx && hm->ctx->cg
                               ? resolve_import_alias(hm->ctx->cg, raw)
                               : NULL;
    if (resolved && *resolved && strcmp(resolved, raw) != 0 &&
        lookup_layout(hm->ctx->cg, resolved))
      return hm_name(hm, resolved);
  }
  const char *lt = strchr(raw, '<');
  if (!lt) {
    char *enum_name = hm_owner_enum_name(hm, self_name, raw);
    if (enum_name) {
      ny_hm_type_t *result = hm_name(hm, enum_name);
      free(enum_name);
      return result;
    }
    /*
     * A bare aggregate annotation carries no element type.  Using a fresh
     * variable here makes every use of the value constrain the same element
     * type, so a tuple-style list (a dict and an int, for example) binds it
     * twice and reports a spurious mismatch.  The dynamic element type keeps
     * indexing and element passing valid without inventing a constraint.
     */
    if (strcmp(raw, "list") == 0)
      return hm_unary(hm, NY_HM_LIST, hm_any(hm));
    if (strcmp(raw, "set") == 0)
      return hm_unary(hm, NY_HM_SET, hm_any(hm));
    if (strcmp(raw, "tuple") == 0)
      return hm_unary(hm, NY_HM_TUPLE, hm_any(hm));
    if (strcmp(raw, "dict") == 0)
      return hm_binary(hm, NY_HM_DICT, hm_any(hm), hm_any(hm));
    return hm_name(hm, raw);
  }
  const char *gt = strrchr(raw, '>');
  if (!gt || gt < lt)
    return hm_name(hm, raw);
  char *base = hm_trim_owned(raw, (size_t)(lt - raw));
  char *inside = hm_trim_owned(lt + 1, (size_t)(gt - lt - 1));
  char *enum_base = hm_owner_enum_name(hm, self_name, base);
  if (enum_base) {
    free(base);
    base = enum_base;
  }
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
    char *qualified = hm_qualify_nested_type_name(hm, self_name, raw);
    out = hm_name(hm, qualified ? qualified : raw);
    free(qualified);
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
  tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm", "hm-result-payload-dynamic",
                  "result", "%s has no statically known Result payload",
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
static void hm_env_clone(ny_hm_state_t *hm, ny_hm_env_list *dst,
                         const ny_hm_env_list *src);

static ny_hm_type_t *hm_infer_comptime_expr(ny_hm_state_t *hm,
                                            ny_hm_env_list *env, expr_t *e,
                                            const char *self_name) {
  if (!hm || !e || e->kind != NY_E_COMPTIME || !e->as.comptime_expr.body)
    return hm_any(hm);
  /*
   * Comptime can materialize an unhinted integer expression as a BigInt
   * (notably `.long` over a long byte sequence). Keep that runtime result type
   * from the closed evaluator instead of unifying it with the property's
   * conservative AST-level `integer` type.
   */
  ny_ct_fast_val_t value = ny_ct_fast_none();
  bool fast_value =
      hm->ctx && hm->ctx->cg &&
      ny_try_eval_comptime_expr_fast(hm->ctx->cg, e, &value, 0);
  ny_ct_fast_kind_t fast_kind = value.kind;
  ny_ct_fast_val_free(&value);
  if (fast_value) {
    if (fast_kind == NY_CT_FAST_BIGINT)
      return hm_parse_type_name(hm, "bigint", self_name);
    if (fast_kind == NY_CT_FAST_INT)
      return hm_parse_type_name(hm, "int", self_name);
  }
  ny_hm_env_list local;
  hm_env_clone(hm, &local, env);
  ny_hm_type_t *ret = hm_var(hm);
  int saved_depth = hm->comptime_depth;
  size_t saved_outer_len = hm->comptime_outer_env_len;
  if (saved_depth == 0)
    hm->comptime_outer_env_len = env ? env->len : 0;
  hm->comptime_depth++;
  hm_infer_stmt(hm, &local, e->as.comptime_expr.body, self_name, ret);
  hm->comptime_depth = saved_depth;
  hm->comptime_outer_env_len = saved_outer_len;
  vec_free(&local);
  ny_comptime_effect_status_t effect_status =
      hm->ctx && hm->ctx->cg ? ny_stmt_comptime_effect_status(
                                   hm->ctx->cg, e->as.comptime_expr.body)
                             : NY_COMPTIME_EFFECTS_SAFE;
  if (effect_status == NY_COMPTIME_EFFECTS_IO)
    tp_add_diag_ex(hm->ctx, e->tok, "hm", "comptime-io", "comptime", NULL, NULL,
                   NULL, NULL, NULL, "comptime expression cannot perform IO");
  else if (effect_status == NY_COMPTIME_EFFECTS_INCOMPLETE)
    tp_add_diag_ex(hm->ctx, e->tok, "hm", "comptime-effects-incomplete",
                   "comptime", "complete effect scan", "bounded scan limit",
                   NULL, NULL, NULL,
                   "comptime effect analysis is incomplete; cannot verify "
                   "that the expression is IO-free");
  ny_hm_type_t *out = hm_prune(ret);
  if (!out || (out->kind == NY_HM_VAR && !out->instance))
    return hm_any(hm);
  return out;
}

static void hm_env_clone(ny_hm_state_t *hm, ny_hm_env_list *dst,
                         const ny_hm_env_list *src) {
  hm_env_init(hm, dst);
  if (src && src->len) {
    vec_reserve(dst, src->len);
    memcpy(dst->data, src->data, src->len * sizeof(*src->data));
    dst->len = src->len;
  }
}

static void hm_env_clone_reuse(ny_hm_state_t *hm, ny_hm_env_list *dst,
                               const ny_hm_env_list *src) {
  if (!hm || !dst)
    return;
  dst->len = 0;
  dst->generation = ++hm->next_env_generation;
  if (!dst->generation)
    dst->generation = ++hm->next_env_generation;
  dst->lookup_cache = hm->env_lookup_cache;
  if (src && src->len) {
    vec_reserve(dst, src->len);
    memcpy(dst->data, src->data, src->len * sizeof(*src->data));
    dst->len = src->len;
  }
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
  /*
   * Runtime float leaves are compiler-recognized calls and do not always
   * have a source declaration in the importing module. Preserve their
   * declared scalar result in HM so inferred locals keep the f64 class
   * through subsequent arithmetic (notably top-level numeric benchmarks).
   */
  if (strncmp(leaf, "__flt_", 6) == 0) {
    if (strcmp(leaf, "__flt_floor") == 0 || strcmp(leaf, "__flt_ceil") == 0 ||
        strcmp(leaf, "__flt_round") == 0 || strcmp(leaf, "__flt_to_int") == 0 ||
        strcmp(leaf, "__flt_trunc") == 0 || strcmp(leaf, "__flt_hash") == 0)
      return hm_name(hm, "int");
    if (strcmp(leaf, "__flt_is_inf") == 0 || strcmp(leaf, "__flt_is_nan") == 0)
      return hm_name(hm, "bool");
    return hm_name(hm, "f64");
  }
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

/*
 * Value indices of inductive families. A constructor's evidence
 * (`Cons(A head, Vec<A, k> tail) where n is k + 1`) relates the family's
 * parameters to names it binds from its fields (`k`, existential). At a
 * construction the field types bind those names from the argument types;
 * in a match arm the scrutinee's indices solve them, so each payload gets the
 * type its index says (`t : Vec<int, 2>` when matching a `Vec<int, 3>`).
 */
typedef struct {
  char names[24][64];
  char values[24][160];
  size_t len;
} hm_index_map_t;

static const char *hm_index_map_get(const hm_index_map_t *m, const char *name,
                                    size_t len) {
  for (size_t i = 0; i < m->len; ++i)
    if (strlen(m->names[i]) == len && strncmp(m->names[i], name, len) == 0)
      return m->values[i];
  return NULL;
}

static bool hm_index_map_put(hm_index_map_t *m, const char *name,
                             const char *value) {
  const char *have = hm_index_map_get(m, name, strlen(name));
  if (have)
    return strcmp(have, value) == 0;
  if (m->len >= 24)
    return false;
  snprintf(m->names[m->len], sizeof(m->names[0]), "%s", name);
  snprintf(m->values[m->len], sizeof(m->values[0]), "%s", value);
  m->len++;
  return true;
}

static bool hm_index_is_bindable(const enum_def_t *owner,
                                 const enum_member_def_t *member,
                                 const char *name, size_t len) {
  for (size_t i = 0; owner && i < owner->type_params.len; ++i)
    if (owner->type_params.data[i] &&
        strlen(owner->type_params.data[i]) == len &&
        strncmp(owner->type_params.data[i], name, len) == 0)
      return true;
  const char *c = member ? member->index_constraint_type : NULL;
  for (const char *q = c; q && *q;) {
    if (!(isalpha((unsigned char)*q) || *q == '_')) {
      q++;
      continue;
    }
    const char *start = q;
    while (isalnum((unsigned char)*q) || *q == '_')
      q++;
    if ((size_t)(q - start) == len && strncmp(start, name, len) == 0)
      return true;
  }
  return false;
}

/* Splits the arguments of `Base<a, b>`; returns the count (0 when `text`
 * is not applied). Pointers are into `text`. */
static size_t hm_index_split_args(const char *text, const char **parts,
                                  size_t *lens, size_t max, size_t *base_len) {
  const char *lt = strchr(text, '<');
  if (!lt)
    return 0;
  *base_len = (size_t)(lt - text);
  size_t n = 0;
  int level = 0;
  const char *start = lt + 1;
  for (const char *q = lt; *q; ++q) {
    if (*q == '<' || *q == '(') {
      level++;
      continue;
    }
    if ((*q == '>' || *q == ')') && --level == 0) {
      if (n < max) {
        while (*start == ' ')
          start++;
        parts[n] = start;
        lens[n++] = (size_t)(q - start);
      }
      break;
    }
    if (*q == ',' && level == 1 && n < max) {
      while (*start == ' ')
        start++;
      parts[n] = start;
      lens[n++] = (size_t)(q - start);
      start = q + 1;
    }
  }
  return n;
}

/* Binds the names of `tmpl` (a field type) against the actual type text. */
static bool hm_index_bind(const enum_def_t *owner,
                          const enum_member_def_t *member, const char *tmpl,
                          size_t tlen, const char *actual, size_t alen,
                          hm_index_map_t *m) {
  while (tlen && tmpl[tlen - 1] == ' ')
    tlen--;
  while (alen && actual[alen - 1] == ' ')
    alen--;
  char t[256], a[256];
  if (tlen >= sizeof(t) || alen >= sizeof(a))
    return true;
  memcpy(t, tmpl, tlen);
  t[tlen] = '\0';
  memcpy(a, actual, alen);
  a[alen] = '\0';
  /* A bare bindable name. */
  size_t k = 0;
  while (isalnum((unsigned char)t[k]) || t[k] == '_')
    k++;
  if (k == tlen && k > 0 && hm_index_is_bindable(owner, member, t, k)) {
    /* An actual that is itself an unsolved parameter (`Vec.Nil : Vec<A, 0>`)
     * says nothing; a conflicting binding is left for unification to
     * report on the field. */
    if (hm_index_is_bindable(owner, NULL, a, strlen(a)))
      return true;
    char canon[160];
    ny_index_canon(a, canon, sizeof(canon));
    (void)hm_index_map_put(m, t, strchr(a, '<') ? a : canon);
    return true;
  }
  /* A linear index over a bindable name: `k+1` against `3`. */
  char var[64], solved[160];
  if ((strchr(t, '+') || strchr(t, '-')) && !strchr(t, '<') &&
      ny_index_solve(t, a, var, sizeof(var), solved, sizeof(solved)) &&
      hm_index_is_bindable(owner, member, var, strlen(var))) {
    (void)hm_index_map_put(m, var, solved);
    return true;
  }
  /* Same constructor applied: bind argument-wise. */
  const char *tp[16], *ap[16];
  size_t tl[16], al[16], tb = 0, ab = 0;
  size_t tn = hm_index_split_args(t, tp, tl, 16, &tb);
  size_t an = hm_index_split_args(a, ap, al, 16, &ab);
  if (tn && tn == an && tb == ab && strncmp(t, a, tb) == 0)
    for (size_t i = 0; i < tn; ++i)
      if (!hm_index_bind(owner, member, tp[i], tl[i], ap[i], al[i], m))
        return false;
  return true;
}

/* Substitutes bound names into `text` and canonicalizes each index. */
static char *hm_index_subst(const char *text, const hm_index_map_t *m) {
  if (!text)
    return NULL;
  char out[1024];
  size_t n = 0;
  for (const char *q = text; *q && n + 1 < sizeof(out);) {
    if ((isalpha((unsigned char)*q) || *q == '_') &&
        (q == text || !(isalnum((unsigned char)q[-1]) || q[-1] == '_' ||
                        q[-1] == '.'))) {
      const char *start = q;
      while (isalnum((unsigned char)*q) || *q == '_' || *q == '.')
        q++;
      const char *v = hm_index_map_get(m, start, (size_t)(q - start));
      const char *src = v ? v : start;
      size_t len = v ? strlen(v) : (size_t)(q - start);
      if (n + len + 1 >= sizeof(out))
        break;
      memcpy(out + n, src, len);
      n += len;
      continue;
    }
    out[n++] = *q++;
  }
  out[n] = '\0';
  /* Canonicalize each top-level generic argument that is an index term. */
  const char *parts[16];
  size_t lens[16], base_len = 0;
  size_t argc = hm_index_split_args(out, parts, lens, 16, &base_len);
  if (!argc) {
    char canon[256];
    if ((strchr(out, '+') || strchr(out, '-')) &&
        ny_index_canon(out, canon, sizeof(canon)))
      return strdup(canon);
    return strdup(out);
  }
  char rebuilt[1024];
  size_t r = (size_t)snprintf(rebuilt, sizeof(rebuilt), "%.*s<",
                              (int)base_len, out);
  for (size_t i = 0; i < argc && r < sizeof(rebuilt); ++i) {
    char arg[256];
    size_t len = lens[i] < sizeof(arg) - 1 ? lens[i] : sizeof(arg) - 1;
    memcpy(arg, parts[i], len);
    arg[len] = '\0';
    char canon[256];
    const char *use = arg;
    if (!strchr(arg, '<') && (strchr(arg, '+') || strchr(arg, '-')) &&
        ny_index_canon(arg, canon, sizeof(canon)))
      use = canon;
    r += (size_t)snprintf(rebuilt + r, sizeof(rebuilt) - r, "%s%s",
                          i ? ", " : "", use);
  }
  if (r + 1 < sizeof(rebuilt))
    snprintf(rebuilt + r, sizeof(rebuilt) - r, ">");
  return strdup(rebuilt);
}

static bool hm_index_member_uses_values(const enum_member_def_t *member) {
  const char *c = member ? member->index_constraint_type : NULL;
  if (!c || !*c)
    return false;
  return isdigit((unsigned char)c[0]) || strchr(c, '+') || strchr(c, '-');
}

/* Does any field type of the member name an existential of its evidence? */
static bool hm_index_member_binds(const enum_def_t *owner,
                                  const enum_member_def_t *member) {
  if (!member)
    return false;
  if (hm_index_member_uses_values(member))
    return true;
  for (size_t i = 0; i < member->fields.len; ++i) {
    const char *t = member->fields.data[i].type_name;
    if (t && owner && ny_generic_type_base_is(t, owner->name))
      return true;
  }
  return false;
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
          hm_env_set_ctx(env, name, args[0].val->as.ident.syntax_ctx,
                         payload_t);
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
  /* Indexed payloads: solve the evidence against the scrutinee's indices. */
  hm_index_map_t index_map = {0};
  bool indexed = owner->type_params.len > 0 && subject_name &&
                 ny_generic_type_base_is(subject_name, owner->name) &&
                 hm_index_member_binds(owner, mem);
  if (indexed) {
    for (size_t p = 0; p < owner->type_params.len; ++p) {
      char *actual = ny_generic_type_arg_owned(subject_name, p);
      if (actual && owner->type_params.data[p])
        hm_index_map_put(&index_map, owner->type_params.data[p], actual);
      if (actual && mem->index_constraint_param &&
          owner->type_params.data[p] &&
          strcmp(mem->index_constraint_param, owner->type_params.data[p]) ==
              0 &&
          mem->index_constraint_type) {
        hm_index_map_t sub = {0};
        (void)hm_index_bind(owner, mem, mem->index_constraint_type,
                            strlen(mem->index_constraint_type), actual,
                            strlen(actual), &sub);
        for (size_t k = 0; k < sub.len; ++k)
          if (strcmp(sub.names[k], owner->type_params.data[p]) != 0)
            hm_index_map_put(&index_map, sub.names[k], sub.values[k]);
      }
      free(actual);
    }
  }
  for (size_t i = 0; i < len; ++i) {
    if (!args[i].val || args[i].val->kind != NY_E_IDENT)
      continue;
    const char *bind = args[i].val->as.ident.name;
    if (!bind || strcmp(bind, "_") == 0)
      continue;
    ssize_t field = args[i].name ? ny_enum_member_field_index(mem, args[i].name)
                                 : (i < mem->fields.len ? (ssize_t)i : -1);
    if (field < 0 || (size_t)field >= mem->fields.len)
      continue;
    if (indexed) {
      char *t = hm_index_subst(mem->fields.data[field].type_name, &index_map);
      hm_env_set_ctx(env, bind, args[i].val->as.ident.syntax_ctx,
                     t ? hm_parse_type_name(hm, t, owner->name) : hm_any(hm));
      free(t);
      continue;
    }
    hm_env_set_ctx(env, bind, args[i].val->as.ident.syntax_ctx,
                   hm_adt_field_match_type(hm, owner, subject_name,
                                           mem->fields.data[field].type_name));
  }
  free(subject_name);
}

/*
 * A GADT constructor can only appear in an arm whose scrutinee index it
 * refines.  Return false when the scrutinee's known index contradicts the
 * constructor's evidence; an unindexed or dynamic scrutinee admits every
 * constructor.
 */
static bool hm_member_index_matches(ny_hm_state_t *hm, enum_def_t *owner,
                                    enum_member_def_t *member,
                                    const char *subject_name) {
  if (!owner || !member || !member->index_constraint_param ||
      !member->index_constraint_type || !subject_name)
    return true;
  if (!ny_generic_type_base_is(subject_name, owner->name))
    return true;
  for (size_t i = 0; i < owner->type_params.len; ++i) {
    if (strcmp(owner->type_params.data[i], member->index_constraint_param) != 0)
      continue;
    char *actual = ny_generic_type_arg_owned(subject_name, i);
    if (!actual)
      return true;
    bool ok;
    if (hm_index_member_uses_values(member)) {
      /* Value indices: a literal against a literal decides; a term with a
       * name could take either value, so the arm stays possible. */
      char an[64], cn[64];
      int64_t ao = 0, co = 0;
      bool a_lin = ny_index_parse(actual, an, sizeof(an), &ao);
      bool c_lin = ny_index_parse(member->index_constraint_type, cn,
                                  sizeof(cn), &co);
      ok = !(a_lin && c_lin && !an[0] && !cn[0] && ao != co);
    } else {
      ok = hm_name_compatible(hm, actual, member->index_constraint_type);
    }
    free(actual);
    return ok;
  }
  return true;
}

static void hm_validate_match_patterns(ny_hm_state_t *hm, stmt_t *s,
                                       ny_hm_type_t *subject_t) {
  if (!hm || !s || s->kind != NY_S_MATCH || !hm->ctx || !hm->ctx->cg)
    return;
  enum_def_t *owner = NULL;
  char *subject_name = subject_t ? hm_type_string(subject_t) : NULL;
  bool has_wild = s->as.match.default_conseq != NULL;
  bool has_other = false;
  VEC(const char *) matched;
  vec_init(&matched);
  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    for (size_t j = 0; j < arm->patterns.len; ++j) {
      expr_t *pat = arm->patterns.data[j];
      if (ny_expr_is_wildcard_ident(pat)) {
        if (!arm->guard)
          has_wild = true;
        continue;
      }
      char *full_name = hm_pattern_call_name(hm, pat);
      enum_def_t *pat_owner = NULL;
      enum_member_def_t *member =
          full_name
              ? lookup_enum_member_owner(hm->ctx->cg, full_name, &pat_owner)
              : NULL;
      if (!member || !pat_owner) {
        free(full_name);
        has_other = true;
        continue;
      }
      if (!owner)
        owner = pat_owner;
      else if (owner != pat_owner)
        has_other = true;
      if (!hm_member_index_matches(hm, pat_owner, member, subject_name)) {
        tp_add_diag_ex(
            hm->ctx, pat->tok, "hm", "hm-gadt-impossible-pattern",
            "GADT match pattern", member->index_constraint_type,
            subject_name ? subject_name : owner->name, "constructor",
            "this constructor refines the enum to a different index than the "
            "scrutinee",
            "match the indexed constructor that produces the scrutinee's "
            "index, or erase the index before matching",
            "constructor '%s.%s' produces index '%s' and cannot match '%s'",
            pat_owner->name, member->name, member->index_constraint_type,
            subject_name ? subject_name : owner->name);
        free(full_name);
        continue;
      }

      if ((pat->kind == NY_E_CALL || pat->kind == NY_E_MEMCALL) &&
          member->has_payload) {
        call_arg_t *args = NULL;
        size_t argc = 0;
        if (ny_expr_call_args(pat, &args, &argc)) {
          for (size_t k = 0; k < argc; ++k) {
            ssize_t field =
                args[k].name ? ny_enum_member_field_index(member, args[k].name)
                             : (k < member->fields.len ? (ssize_t)k : -1);
            const char *field_name =
                args[k].name ? args[k].name
                             : (field >= 0 ? member->fields.data[field].name
                                           : "<field>");
            if (args[k].name && field < 0) {
              tp_add_diag_ex(
                  hm->ctx, pat->tok, "hm", "hm-adt-unknown-field",
                  "ADT match pattern", field_name, member->name, "field",
                  "named ADT pattern fields must exist on the selected variant",
                  "use one of the variant's declared fields",
                  "unknown field '%s' in ADT match pattern '%s.%s'", field_name,
                  pat_owner->name, member->name);
            } else if (field >= 0 && !ny_expr_is_wildcard_ident(args[k].val) &&
                       (!args[k].val || args[k].val->kind != NY_E_IDENT)) {
              tp_add_diag_ex(
                  hm->ctx, pat->tok, "hm", "hm-adt-field-binding",
                  "ADT match pattern", field_name, "identifier or _", "field",
                  "payload fields in an ADT pattern introduce bindings",
                  "bind the field to a name or use '_' to ignore it",
                  "ADT match field '%s' must bind to an identifier or '_'",
                  field_name);
            }
          }
        }
      }
      if (!arm->guard) {
        bool seen = false;
        for (size_t k = 0; k < matched.len; ++k)
          if (strcmp(matched.data[k], member->name) == 0)
            seen = true;
        if (!seen)
          vec_push(&matched, member->name);
      }
      free(full_name);
    }
  }
  if (owner && !has_wild && !has_other) {
    char missing[256] = {0};
    size_t used = 0;
    for (size_t i = 0; i < owner->members.len; ++i) {
      if (!hm_member_index_matches(hm, owner, &owner->members.data[i],
                                   subject_name))
        continue;
      bool found = false;
      for (size_t j = 0; j < matched.len; ++j)
        if (strcmp(matched.data[j], owner->members.data[i].name) == 0)
          found = true;
      if (!found) {
        const char *name = owner->members.data[i].name;
        size_t n = strlen(name);
        if (used && used + 2 < sizeof(missing)) {
          missing[used++] = ',';
          missing[used++] = ' ';
        }
        if (used + n < sizeof(missing)) {
          memcpy(missing + used, name, n);
          used += n;
          missing[used] = '\0';
        }
      }
    }
    if (used > 0)
      tp_add_diag_ex(
          hm->ctx, s->tok, "hm", "hm-adt-non-exhaustive", "ADT match",
          owner->name, missing, "match",
          "all constructors of an enum must be covered or a default arm used",
          "add the missing constructor arm or a wildcard/default arm",
          "non-exhaustive match for enum '%s' (missing: %s)", owner->name,
          missing);
  }
  free(subject_name);
  vec_free(&matched);
}

typedef struct hm_owner_fact_t {
  const char *name;
  const char *borrow_source;
  /*
   * Shared identity for raw allocations and their copied/interior aliases.
   */
  size_t raw_origin;
  bool owned;
  bool moved;
  bool released;
} hm_owner_fact_t;
typedef struct hm_owner_fact_list {
  hm_owner_fact_t *data;
  size_t len;
  size_t cap;
  size_t next_origin;
} hm_owner_fact_list;

static void hm_owner_clone_facts(hm_owner_fact_list *dst,
                                 const hm_owner_fact_list *src) {
  vec_init(dst);
  dst->next_origin = src ? src->next_origin : 0;
  for (size_t i = 0; src && i < src->len; ++i)
    vec_push(dst, src->data[i]);
}

static void hm_owner_merge_branch_facts(hm_owner_fact_list *dst,
                                        size_t base_len,
                                        const hm_owner_fact_list *left,
                                        const hm_owner_fact_list *right) {
  if (!dst || !left || !right)
    return;
  dst->next_origin = left->next_origin > right->next_origin
                         ? left->next_origin
                         : right->next_origin;
  size_t len = base_len < dst->len ? base_len : dst->len;
  if (len > left->len)
    len = left->len;
  if (len > right->len)
    len = right->len;
  for (size_t i = 0; i < len; ++i) {
    dst->data[i].moved = left->data[i].moved || right->data[i].moved;
    dst->data[i].released = left->data[i].released || right->data[i].released;
  }
}

static hm_owner_fact_t *hm_owner_fact(hm_owner_fact_list *facts,
                                      const char *name) {
  if (!facts || !name)
    return NULL;
  for (size_t i = facts->len; i > 0; --i)
    if (facts->data[i - 1].name && strcmp(facts->data[i - 1].name, name) == 0)
      return &facts->data[i - 1];
  return NULL;
}

static const char *hm_owner_borrower(hm_owner_fact_list *facts,
                                     const char *source) {
  if (!facts || !source)
    return NULL;
  for (size_t i = facts->len; i > 0; --i) {
    hm_owner_fact_t *fact = &facts->data[i - 1];
    if (fact->borrow_source && strcmp(fact->borrow_source, source) == 0)
      return fact->name;
  }
  return NULL;
}

static void hm_owner_diag(ny_hm_state_t *hm, token_t tok, const char *fmt,
                          ...) {
  if (!hm || !hm->ctx || !fmt)
    return;
  char message[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);
  tp_add_diag_ex(
      hm->ctx, tok, "ownership", "hm-ownership", "ownership",
      "valid ownership flow", "invalid ownership flow", "ownership",
      "strict ownership prevents dangling borrows and double cleanup",
      "change the borrow/consume/release order or add the matching contract",
      "%s", message);
}

static const char *hm_owner_call_name(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee)
    return NULL;
  if (e->semantic.canonical_callee)
    return e->semantic.canonical_callee;
  expr_t *callee = e->as.call.callee;
  if (callee->kind == NY_E_IDENT)
    return callee->as.ident.name;
  if (callee->kind == NY_E_MEMBER)
    return callee->as.member.name;
  return NULL;
}

static bool hm_owner_contract_has(ny_str_list *list, const char *name) {
  if (!list || !name)
    return false;
  for (size_t i = 0; i < list->len; ++i)
    if (list->data[i] && strcmp(list->data[i], name) == 0)
      return true;
  return false;
}

static expr_t *hm_owner_return_borrow_arg(codegen_t *cg, expr_t *call) {
  if (!cg || !call || call->kind != NY_E_CALL)
    return NULL;
  const char *name = hm_owner_call_name(call);
  fun_sig *sig = name ? lookup_fun(cg, name, 0) : NULL;
  if (!sig || !sig->returns_borrow || !sig->stmt_t ||
      sig->stmt_t->kind != NY_S_FUNC)
    return NULL;
  for (size_t i = 0; i < call->as.call.args.len; ++i) {
    const char *param = call->as.call.args.data[i].name;
    if (!param && i < sig->stmt_t->as.fn.params.len)
      param = sig->stmt_t->as.fn.params.data[i].name;
    if (param && strcmp(param, sig->returns_borrow) == 0)
      return call->as.call.args.data[i].val;
  }
  return NULL;
}

static const char *hm_owner_ident(expr_t *e) {
  return e && e->kind == NY_E_IDENT ? e->as.ident.name : NULL;
}

static void hm_owner_check_use(ny_hm_state_t *hm, hm_owner_fact_list *facts,
                               expr_t *e) {
  const char *name = hm_owner_ident(e);
  hm_owner_fact_t *fact = hm_owner_fact(facts, name);
  if (fact && fact->raw_origin && fact->released)
    hm_owner_diag(hm, e->tok, "use after free of raw pointer '%s'", name);
  else if (fact && fact->moved)
    hm_owner_diag(hm, e->tok, "use after move of owned slot '%s'", name);
}

static size_t hm_owner_raw_origin(ny_hm_state_t *hm, hm_owner_fact_list *facts,
                                  expr_t *e) {
  if (!e)
    return 0;
  hm_owner_fact_t *fact = hm_owner_fact(facts, hm_owner_ident(e));
  if (fact)
    return fact->raw_origin;
  if (e->kind == NY_E_UNARY)
    return hm_owner_raw_origin(hm, facts, e->as.unary.right);
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0))
    return hm_owner_raw_origin(hm, facts, e->as.binary.left);
  if (e->kind == NY_E_CALL && e->as.call.args.len == 1) {
    const char *callee = hm_owner_call_name(e);
    const char *name = callee ? ny_name_leaf(callee) : NULL;
    if (name &&
        (strcmp(name, "borrow") == 0 || strcmp(name, "borrow_mut") == 0))
      return hm_owner_raw_origin(hm, facts, e->as.call.args.data[0].val);
  }
  if (e->kind == NY_E_CALL && hm && hm->ctx && hm->ctx->cg) {
    expr_t *borrowed = hm_owner_return_borrow_arg(hm->ctx->cg, e);
    if (borrowed)
      return hm_owner_raw_origin(hm, facts, borrowed);
  }
  return 0;
}

static void hm_owner_release_raw(ny_hm_state_t *hm, hm_owner_fact_list *facts,
                                 expr_t *arg) {
  size_t origin = hm_owner_raw_origin(hm, facts, arg);
  if (!origin)
    return;
  for (size_t i = 0; i < facts->len; ++i) {
    hm_owner_fact_t *fact = &facts->data[i];
    if (fact->raw_origin != origin)
      continue;
    if (fact->released) {
      hm_owner_diag(hm, arg->tok, "double free of raw pointer");
      return;
    }
  }
  for (size_t i = 0; i < facts->len; ++i)
    if (facts->data[i].raw_origin == origin)
      facts->data[i].released = true;
}

static void hm_owner_scan_expr(ny_hm_state_t *hm, hm_owner_fact_list *facts,
                               expr_t *e) {
  if (!e)
    return;
  if (e->kind == NY_E_CALL) {
    const char *call_name = hm_owner_call_name(e);
    const char *leaf = call_name ? ny_name_leaf(call_name) : NULL;
    bool raw_free =
        leaf &&
        (strcmp(leaf, "free") == 0 || strcmp(leaf, "__free") == 0 ||
         strcmp(leaf, "free_raw") == 0 || strcmp(leaf, "__free_raw") == 0);
    if (raw_free && e->as.call.args.len == 1) {
      expr_t *arg = e->as.call.args.data[0].val;
      if (hm_owner_raw_origin(hm, facts, arg))
        hm_owner_release_raw(hm, facts, arg);
      else
        hm_owner_scan_expr(hm, facts, arg);
      return;
    }
    if (e->as.call.args.len == 1 &&
        (call_name && (strcmp(ny_name_leaf(call_name), "release") == 0 ||
                       strcmp(ny_name_leaf(call_name), "forget") == 0))) {
      const char *name = hm_owner_ident(e->as.call.args.data[0].val);
      hm_owner_fact_t *fact = hm_owner_fact(facts, name);
      if (fact) {
        const char *borrower = hm_owner_borrower(facts, name);
        if (borrower)
          hm_owner_diag(hm, e->tok,
                        "cannot %s owned slot '%s' while borrow '%s' is live",
                        ny_name_leaf(call_name), name, borrower);
        else if (fact->released &&
                 strcmp(ny_name_leaf(call_name), "release") == 0)
          hm_owner_diag(hm, e->tok, "double release of owned slot '%s'", name);
        else {
          fact->moved = true;
          fact->released = strcmp(ny_name_leaf(call_name), "release") == 0;
        }
      }
    } else {
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        hm_owner_scan_expr(hm, facts, e->as.call.args.data[i].val);
      if (hm && hm->ctx && hm->ctx->cg && call_name) {
        fun_sig *sig = lookup_fun(hm->ctx->cg, call_name, 0);
        if (sig) {
          for (size_t i = 0; i < e->as.call.args.len; ++i) {
            call_arg_t *actual = &e->as.call.args.data[i];
            const char *param = actual->name;
            if (!param && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
                i < sig->stmt_t->as.fn.params.len)
              param = sig->stmt_t->as.fn.params.data[i].name;
            bool releasing = hm_owner_contract_has(&sig->releases, param);
            bool consuming = hm_owner_contract_has(&sig->consumes, param);
            bool mutating = hm_owner_contract_has(&sig->mutates, param);
            if (releasing)
              hm_owner_release_raw(hm, facts, actual->val);
            const char *name = hm_owner_ident(actual->val);
            hm_owner_fact_t *fact = hm_owner_fact(facts, name);
            if (!fact || (!consuming && !mutating))
              continue;
            const char *borrower = hm_owner_borrower(facts, name);
            if (borrower)
              hm_owner_diag(
                  hm, e->tok,
                  "cannot %s owned slot '%s' while borrow '%s' is live",
                  consuming ? "move" : "mutate", name, borrower);
            else if (consuming) {
              /*
               * Each parameter consumes independently. Aliases of a raw
               * owner cannot remain usable after the ownership transfer.
               */
              if (!releasing)
                hm_owner_check_use(hm, facts, actual->val);
              size_t origin = fact->raw_origin;
              if (origin) {
                for (size_t j = 0; j < facts->len; ++j)
                  if (facts->data[j].raw_origin == origin)
                    facts->data[j].moved = true;
              } else {
                fact->moved = true;
              }
            }
          }
        }
      }
    }
    return;
  }
  if (e->kind == NY_E_IDENT) {
    hm_owner_check_use(hm, facts, e);
    return;
  }
  if (e->kind == NY_E_BINARY) {
    hm_owner_scan_expr(hm, facts, e->as.binary.left);
    hm_owner_scan_expr(hm, facts, e->as.binary.right);
  } else if (e->kind == NY_E_UNARY) {
    hm_owner_scan_expr(hm, facts, e->as.unary.right);
  } else if (e->kind == NY_E_MEMBER) {
    hm_owner_scan_expr(hm, facts, e->as.member.target);
  } else if (e->kind == NY_E_INDEX) {
    hm_owner_scan_expr(hm, facts, e->as.index.target);
    hm_owner_scan_expr(hm, facts, e->as.index.start);
    hm_owner_scan_expr(hm, facts, e->as.index.stop);
    hm_owner_scan_expr(hm, facts, e->as.index.step);
  } else if (e->kind == NY_E_MEMCALL) {
    hm_owner_scan_expr(hm, facts, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      hm_owner_scan_expr(hm, facts, e->as.memcall.args.data[i].val);
  }
}

static void hm_owner_scan_stmt(ny_hm_state_t *hm, stmt_t *s,
                               hm_owner_fact_list *facts, stmt_t *fn) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t base_len = facts->len;
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      hm_owner_scan_stmt(hm, s->as.block.body.data[i], facts, fn);
    facts->len = base_len;
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      hm_owner_scan_expr(hm, facts, init);
      size_t origin = hm_owner_raw_origin(hm, facts, init);
      const char *callee = hm_owner_call_name(init);
      const char *leaf = callee ? ny_name_leaf(callee) : NULL;
      fun_sig *sig = callee && hm && hm->ctx && hm->ctx->cg
                         ? lookup_fun(hm->ctx->cg, callee, 0)
                         : NULL;
      bool returned_owned = sig && sig->returns_owned;
      bool fresh_raw =
          returned_owned ||
          (leaf &&
           (strcmp(leaf, "malloc") == 0 || strcmp(leaf, "__malloc") == 0 ||
            strcmp(leaf, "zalloc") == 0 || strcmp(leaf, "malloc_raw") == 0 ||
            strcmp(leaf, "calloc_raw") == 0));
      if (!s->as.var.is_decl) {
        hm_owner_fact_t *old = hm_owner_fact(facts, name);
        /*
         * Last-use and overlap checking is handled by the function-level B3
         * pass.  This sequential owner scan has no lexical-use cursor and
         * otherwise rejects a borrow even after its last use.
         */
        if (old) {
          old->moved = false;
          old->released = false;
          /*
           * An old alias may still refer to the old allocation. Do not reuse
           * its identity for a fresh allocation on assignment.
           */
          if (fresh_raw) {
            hm_owner_fact_t replacement = *old;
            replacement.raw_origin = ++facts->next_origin;
            vec_push(facts, replacement);
          } else {
            old->raw_origin = origin;
          }
        }
      } else {
        hm_owner_fact_t fact = {
            .name = name,
            .raw_origin = fresh_raw ? ++facts->next_origin : origin,
            .owned =
                returned_owned ||
                (init && (init->kind == NY_E_LIST || init->kind == NY_E_DICT ||
                          init->kind == NY_E_SET || init->kind == NY_E_TUPLE))};
        if (init && init->kind == NY_E_CALL && init->as.call.args.len == 1 &&
            (hm_owner_call_name(init) &&
             strcmp(ny_name_leaf(hm_owner_call_name(init)), "borrow") == 0))
          fact.borrow_source = hm_owner_ident(init->as.call.args.data[0].val);
        if (init && init->kind == NY_E_UNARY && init->as.unary.op &&
            (strcmp(init->as.unary.op, "&") == 0 ||
             strcmp(init->as.unary.op, "borrow") == 0))
          fact.borrow_source = hm_owner_ident(init->as.unary.right);
        vec_push(facts, fact);
      }
    }
    break;
  case NY_S_EXPR:
    hm_owner_scan_expr(hm, facts, s->as.expr.expr);
    break;
  case NY_S_RETURN: {
    expr_t *value = s->as.ret.value;
    if (value && value->kind == NY_E_CALL) {
      expr_t *borrowed = hm_owner_return_borrow_arg(hm->ctx->cg, value);
      const char *owner = hm_owner_ident(borrowed);
      hm_owner_fact_t *owner_fact = hm_owner_fact(facts, owner);
      if (owner_fact && owner_fact->owned)
        hm_owner_diag(
            hm, s->tok,
            "returning borrow of local owner '%s' would outlive its slot",
            owner);
    }
    const char *name = hm_owner_ident(value);
    hm_owner_fact_t *fact = hm_owner_fact(facts, name);
    const char *borrower = hm_owner_borrower(facts, name);
    if (fact && borrower)
      hm_owner_diag(hm, s->tok,
                    "cannot return owned slot '%s' while borrow '%s' is live",
                    name, borrower);
    else if (fact && fact->owned && !fn->as.fn.attr_returns_owned)
      hm_owner_diag(hm, s->tok,
                    "returning owned slot '%s' requires @returns_owned", name);
    hm_owner_scan_expr(hm, facts, value);
    break;
  }
  case NY_S_IF: {
    hm_owner_scan_expr(hm, facts, s->as.iff.test);
    size_t base_len = facts->len;
    hm_owner_fact_list yes, no;
    hm_owner_clone_facts(&yes, facts);
    hm_owner_clone_facts(&no, facts);
    hm_owner_scan_stmt(hm, s->as.iff.conseq, &yes, fn);
    hm_owner_scan_stmt(hm, s->as.iff.alt, &no, fn);
    hm_owner_merge_branch_facts(facts, base_len, &yes, &no);
    vec_free(&yes);
    vec_free(&no);
    break;
  }
  case NY_S_WHILE:
    hm_owner_scan_stmt(hm, s->as.whl.init, facts, fn);
    hm_owner_scan_expr(hm, facts, s->as.whl.test);
    hm_owner_scan_stmt(hm, s->as.whl.body, facts, fn);
    hm_owner_scan_stmt(hm, s->as.whl.update, facts, fn);
    /*
     * A release in one iteration must invalidate a later iteration's use.
     */
    hm_owner_scan_expr(hm, facts, s->as.whl.test);
    hm_owner_scan_stmt(hm, s->as.whl.body, facts, fn);
    hm_owner_scan_stmt(hm, s->as.whl.update, facts, fn);
    break;
  case NY_S_FOR:
    hm_owner_scan_stmt(hm, s->as.fr.init, facts, fn);
    hm_owner_scan_expr(hm, facts, s->as.fr.iterable);
    hm_owner_scan_expr(hm, facts, s->as.fr.cond);
    hm_owner_scan_stmt(hm, s->as.fr.body, facts, fn);
    hm_owner_scan_stmt(hm, s->as.fr.update, facts, fn);
    hm_owner_scan_expr(hm, facts, s->as.fr.cond);
    hm_owner_scan_stmt(hm, s->as.fr.body, facts, fn);
    hm_owner_scan_stmt(hm, s->as.fr.update, facts, fn);
    break;
  default:
    break;
  }
}

static void hm_validate_ownership(ny_hm_state_t *hm, stmt_t *fn) {
  if (!hm || !fn || fn->kind != NY_S_FUNC || !hm->ctx || !hm->ctx->cg ||
      (!hm->ctx->cg->ownership_strict && fn->as.fn.attr_allowances.len == 0) ||
      ny_is_stdlib_tok(fn->tok))
    return;
  if (hm->ctx->cg->ownership_strict) {
    hm_owner_fact_list facts;
    vec_init(&facts);
    facts.next_origin = 0;
    hm_owner_scan_stmt(hm, fn->as.fn.body, &facts, fn);
    vec_free(&facts);
  }
  int had_error = hm->ctx->cg->had_error;
  stmt_ownership_check_function(hm->ctx->cg, fn);
  if (!had_error && hm->ctx->cg->had_error)
    hm_owner_diag(hm, fn->tok, "ownership rule validation failed in '%s'",
                  fn->as.fn.name ? fn->as.fn.name : "<function>");
}

static char *hm_attached_owner_name(const char *type_name) {
  if (!type_name)
    return ny_strdup("any");
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  const char *end = type_name;
  while (*end && *end != '<' && *end != '[' && *end != '|' && *end != ' ')
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
  return ny_gencall_lookup_attached_method(hm->ctx->cg, owner, member);
}

static const char *hm_sig_scheme_name(ny_hm_state_t *hm, const fun_sig *sig) {
  if (!sig || !sig->stmt_t)
    return sig ? sig->name : NULL;
  for (size_t i = 0; hm && i < hm->schemes.len; ++i) {
    ny_hm_scheme_t *scheme = &hm->schemes.data[i];
    if (scheme->stmt == sig->stmt_t)
      return scheme->full_name ? scheme->full_name : scheme->name;
  }
  return sig->name;
}

static char *hm_qualify_signature_type(ny_hm_state_t *hm, const fun_sig *sig,
                                       const char *type_name);
static char *hm_qualify_owner_type(ny_hm_state_t *hm, const char *owner,
                                   const char *type_name);

static ny_hm_type_t *hm_sig_return_type(ny_hm_state_t *hm, fun_sig *sig,
                                        const char *self_name) {
  if (!sig)
    return hm_any(hm);
  const char *type_name = sig->return_type ? sig->return_type
                          : sig->inferred_return_type
                              ? sig->inferred_return_type
                              : NULL;
  if (type_name) {
    char *qualified = hm_qualify_signature_type(hm, sig, type_name);
    ny_hm_type_t *result =
        hm_parse_type_name(hm, qualified ? qualified : type_name, self_name);
    free(qualified);
    return result;
  }
  return hm_any(hm);
}

/*
 * Replace whole identifier occurrences of `name` with `value`.
 */
static char *hm_replace_type_index_owned(const char *src, const char *name,
                                         int64_t value) {
  if (!src || !name || !*name)
    return src ? ny_strdup(src) : NULL;
  size_t name_len = strlen(name);
  ny_tp_json_t out = {0};
  const char *p = src;
  while (*p) {
    if ((*p == '_' || isalpha((unsigned char)*p)) &&
        (p == src ||
         !(p[-1] == '_' || isalnum((unsigned char)p[-1]) || p[-1] == '.')) &&
        strncmp(p, name, name_len) == 0 &&
        !(p[name_len] == '_' || isalnum((unsigned char)p[name_len]))) {
      tp_append(&out, "%" PRId64, value);
      p += name_len;
      continue;
    }
    tp_append(&out, "%c", *p++);
  }
  return tp_take(&out, src);
}

/*
 * A dependent return type mentions parameter names as value indices.  At a
 * call with a literal argument, substitute the literal so `Vec<n>` becomes
 * `Vec<2>`; non-literal arguments keep the symbolic type and fail closed when
 * a caller requires a different index.
 */
static ny_hm_type_t *hm_subst_return_indices(ny_hm_state_t *hm, stmt_t *fn,
                                             ny_hm_type_t *base,
                                             ny_call_arg_list *args,
                                             size_t arg_offset,
                                             const char *self_name) {
  if (!fn || fn->kind != NY_S_FUNC || !args || !base)
    return base;
  if (fn->as.fn.return_type &&
      strncmp(fn->as.fn.return_type, "proof<", 6) == 0) {
    char *inst = hm_proof_subst_args(hm, fn, fn->as.fn.return_type, args,
                                     arg_offset, fn->as.fn.params.len);
    if (inst) {
      ny_hm_type_t *t = hm_name(hm, inst);
      free(inst);
      return t;
    }
  }
  char *name = hm_type_string(base);
  if (!name || !strchr(name, '<')) {
    free(name);
    return base;
  }
  char *text = name;
  for (size_t i = 0; i < args->len && i + arg_offset < fn->as.fn.params.len;
       ++i) {
    param_t *param = &fn->as.fn.params.data[i + arg_offset];
    expr_t *arg = args->data[i].val;
    if (!param->name || !param->type || !arg || arg->kind != NY_E_LITERAL ||
        arg->as.literal.kind != NY_LIT_INT)
      continue;
    /*
     * Only value-indexed parameters (`Fin<N>`, `Vec<N>`) bind a value into a
     * type.  Ordinary parameter names must not textually rewrite return
     * types that happen to use a same-named type variable.
     */
    char indexed_base[512];
    bool value_indexed =
        strncmp(param->type, "Fin<", 4) == 0 ||
        (hm->ctx && hm->ctx->cg &&
         ny_indexed_declared_type_base(hm->ctx->cg, param->type, indexed_base,
                                       sizeof(indexed_base)));
    if (!value_indexed)
      continue;
    char *next =
        hm_replace_type_index_owned(text, param->name, arg->as.literal.as.i);
    if (!next)
      break;
    if (text != name)
      free(text);
    text = next;
  }
  ny_hm_type_t *result =
      text == name ? base : hm_parse_type_name(hm, text, self_name);
  if (text != name)
    free(text);
  free(name);
  return result ? result : base;
}

/*
 * A constructed pair literal's index component, when it is a literal.
 */
static bool hm_sigma_literal_index(const expr_t *init, int64_t *out) {
  if (!init || !out || init->kind != NY_E_TUPLE || init->as.list_like.len != 2)
    return false;
  const expr_t *index = init->as.list_like.data[0];
  if (!index || index->kind != NY_E_LITERAL ||
      index->as.literal.kind != NY_LIT_INT ||
      index->as.literal.integer_outside_i64)
    return false;
  *out = index->as.literal.as.i;
  return true;
}

/*
 * Dependent pair construction.  The first component is checked against the
 * index type; when it is a literal, that literal is substituted for the
 * binder in the payload type before the second component is checked.  The
 * substituted payload annotation also drives the existing Fin<N> and
 * refinement initializer rules.
 */
static void hm_check_sigma_initializer(ny_hm_state_t *hm, ny_hm_env_list *env,
                                       const char *decl, const expr_t *init,
                                       const char *self_name, token_t tok) {
  hm_sigma_parts_t parts;
  if (!hm || !decl || !hm_sigma_parts_parse(decl, &parts))
    return;
  if (!init || init->kind != NY_E_TUPLE || init->as.list_like.len != 2) {
    /*
     * A value already carrying the pair type (a parameter or an earlier
     * binding) satisfies the shape; only a fresh non-pair value is an error.
     */
    ny_hm_type_t *actual =
        init ? hm_prune(hm_infer_expr(hm, env, (expr_t *)init, self_name))
             : NULL;
    if (actual && (actual->kind == NY_HM_TUPLE || hm_is_any(actual))) {
      hm_sigma_parts_free(&parts);
      return;
    }
    tp_add_diag_ex(hm->ctx, tok, "hm", "hm-sigma-shape",
                   "dependent pair initialization", "pair(index, value)",
                   "non-pair initializer", "dependent pair",
                   "a dependent pair value must package exactly two components",
                   "construct the value with pair(index, value)",
                   "dependent pair initializer must be pair(index, value)");
    hm_sigma_parts_free(&parts);
    return;
  }
  expr_t *index_e = init->as.list_like.data[0];
  expr_t *value_e = init->as.list_like.data[1];
  ny_hm_type_t *want_index =
      hm_parse_type_name(hm, parts.index_type, self_name);
  ny_hm_type_t *got_index = hm_infer_expr(hm, env, index_e, self_name);
  hm_check_fin_initializer(hm, parts.index_type, index_e, tok);
  hm_check_refinement_initializer(hm, parts.index_type, index_e, tok);
  hm_unify(hm, want_index, got_index, tok, "sigma index component");

  int64_t literal = 0;
  char *value_type_raw = NULL;
  if (hm_sigma_literal_index(init, &literal))
    value_type_raw =
        hm_replace_type_index_owned(parts.value_type, parts.binder, literal);
  if (!value_type_raw)
    value_type_raw = ny_strdup(parts.value_type);
  if (value_type_raw) {
    ny_hm_type_t *want_value =
        hm_parse_type_name(hm, value_type_raw, self_name);
    ny_hm_type_t *got_value = hm_infer_expr(hm, env, value_e, self_name);
    hm_check_fin_initializer(hm, value_type_raw, value_e, tok);
    hm_check_indexed_initializer(hm, value_type_raw, value_e, tok);
    hm_check_refinement_initializer(hm, value_type_raw, value_e, tok);
    hm_unify(hm, want_value, got_value, tok, "sigma value component");
    free(value_type_raw);
  }
  hm_sigma_parts_free(&parts);
}

static ny_hm_type_t *hm_sig_return_type_for_call(ny_hm_state_t *hm,
                                                 fun_sig *sig, stmt_t *fn,
                                                 ny_call_arg_list *args,
                                                 size_t arg_offset,
                                                 const char *self_name) {
  return hm_subst_return_indices(hm, fn, hm_sig_return_type(hm, sig, self_name),
                                 args, arg_offset, self_name);
}

static ny_hm_type_t *hm_infer_lambda(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     expr_t *e, const char *self_name) {
  ny_hm_type_t *ft = hm_fn_type(hm);
  if (!e)
    return ft;
  ft->parameters = &e->as.lambda.params;
  ny_hm_env_list local;
  hm_env_clone(hm, &local, env);
  bool explicit_type = e->as.lambda.return_type != NULL;
  for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
    param_t *p = &e->as.lambda.params.data[i];
    if (p->type)
      explicit_type = true;
    ny_hm_type_t *pt =
        p->type ? hm_parse_type_name(hm, p->type, self_name) : hm_var(hm);
    hm_args_push(hm, ft, pt);
    hm_env_set_ctx(&local, p->name,
                   e->as.lambda.body ? e->as.lambda.body->syntax_ctx : 0, pt);
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

/*
 * Minimum positional arguments needed for a *complete* call.  A default may
 * only be omitted when every later parameter is also defaulted; otherwise a
 * positional call would have to skip over the default and land on a later
 * required parameter, which is not how source arguments bind.  Thus
 * `fn f(int a, int b = a, int c)` needs all three arguments, while
 * `fn g(int a, int b = 1)` needs only `a`.
 */
static size_t hm_param_min_args(const ny_param_list *params, size_t offset) {
  if (!params || offset >= params->len)
    return 0;
  size_t first_default = params->len;
  for (size_t i = offset; i < params->len; ++i) {
    if (params->data[i].def) {
      first_default = i;
      break;
    }
  }
  if (first_default == params->len)
    return params->len - offset;
  for (size_t i = first_default; i < params->len; ++i)
    if (!params->data[i].def)
      return params->len - offset;
  return first_default - offset;
}

/*
 * One past the last parameter without a default at or after `offset`.
 */
static size_t hm_param_required_end(const ny_param_list *params,
                                    size_t offset) {
  if (!params || offset >= params->len)
    return offset;
  size_t end = offset;
  for (size_t i = offset; i < params->len; ++i)
    if (!params->data[i].def)
      end = i + 1;
  return end;
}

static bool hm_param_range_required(const ny_param_list *params, size_t begin,
                                    size_t end) {
  if (!params)
    return false;
  if (end > params->len)
    end = params->len;
  for (size_t i = begin; i < end; ++i)
    if (params->data[i].def)
      return false;
  return true;
}

static bool hm_param_is_consumed(const stmt_t *fn, const char *name) {
  if (!fn || fn->kind != NY_S_FUNC || !name)
    return false;
  for (size_t i = 0; i < fn->as.fn.attr_consumes.len; ++i) {
    const char *consumed = fn->as.fn.attr_consumes.data[i];
    if (consumed && strcmp(consumed, name) == 0)
      return true;
  }
  return false;
}

static size_t hm_scheme_min_args(const ny_hm_scheme_t *scheme, size_t offset) {
  if (!scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC)
    return 0;
  return hm_param_min_args(&scheme->stmt->as.fn.params, offset);
}

/*
 * Variadic calls still require every non-rest parameter up to the first
 * defaulted one; the rest parameter itself is never required.
 */
static size_t hm_scheme_variadic_min_args(const ny_hm_scheme_t *scheme,
                                          size_t offset) {
  if (!scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC)
    return 0;
  const ny_param_list *params = &scheme->stmt->as.fn.params;
  size_t end = params->len > 0 ? params->len - 1 : 0;
  while (end > offset && params->data[end - 1].def)
    end--;
  return end > offset ? end - offset : 0;
}

static bool hm_scheme_is_variadic(const ny_hm_scheme_t *scheme) {
  return scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
         scheme->stmt->as.fn.is_variadic;
}

/*
 * The slot of an explicit type binder (`fn id(Type A, A x)`) is `Type<A>`.
 * A written type as the argument instantiates `A` and is passed as the
 * erased type value, its name; any other expression is a runtime type value.
 */
static bool hm_bind_type_argument(ny_hm_state_t *hm, ny_hm_env_list *env,
                                  ny_hm_type_t *slot_type, expr_t *arg,
                                  const char *self_name) {
  ny_hm_type_t *slot = hm_prune(slot_type);
  if (!slot || slot->kind != NY_HM_NAME || !slot->name ||
      strcmp(slot->name, "Type") != 0 ||
      (slot->args.len != 1 && slot->args.len != 2))
    return false;
  /*
   * Universe levels: a written type is level 0; a universe value `Type n`
   * passed as the type is level n + 1. A level variable takes the argument's
   * level; a literal level must be at least it (cumulativity).
   */
  if (slot->args.len == 2 && arg) {
    long arg_level = -1;
    if (arg->kind == NY_E_IDENT && arg->as.ident.name &&
        strcmp(arg->as.ident.name, "_") != 0 &&
        !hm_env_get_ctx(env, arg->as.ident.name, arg->as.ident.syntax_ctx))
      arg_level = 0;
    else if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_STR &&
             arg->as.literal.as.s.data &&
             strncmp(arg->as.literal.as.s.data, "Type", 4) == 0)
      arg_level = arg->as.literal.as.s.data[4] == ' '
                      ? strtol(arg->as.literal.as.s.data + 5, NULL, 10) + 1
                      : 1;
    ny_hm_type_t *level = hm_prune(slot->args.data[1]);
    if (arg_level >= 0 && level) {
      char text[32];
      snprintf(text, sizeof(text), "%ld", arg_level);
      if (level->kind == NY_HM_VAR) {
        hm_unify(hm, level, hm_name(hm, text), arg->tok, "universe level");
      } else if (level->kind == NY_HM_NAME && level->name &&
                 isdigit((unsigned char)level->name[0]) &&
                 strtol(level->name, NULL, 10) < arg_level) {
        tp_add_diag_ex(
            hm->ctx, arg->tok, "hm", "hm-universe-level", "type argument",
            level->name, text, "call",
            "a type argument must live in the binder's universe",
            "raise the binder's level or pass a lower-level type",
            "universe level too low: the binder is 'Type %s', the argument "
            "lives in 'Type %s'",
            level->name, text);
      }
    }
  }
  if (slot->args.len == 2 && arg && arg->kind == NY_E_LITERAL &&
      arg->as.literal.kind == NY_LIT_STR && arg->as.literal.as.s.data &&
      strncmp(arg->as.literal.as.s.data, "Type", 4) == 0) {
    hm_unify(hm, slot->args.data[0],
             hm_parse_type_name(hm, arg->as.literal.as.s.data, self_name),
             arg->tok, "type argument");
    (void)hm_infer_expr(hm, env, arg, self_name);
    return true;
  }
  /* `id(_, 3)`: a hole for the type argument, filled once the other
   * arguments have fixed the binder (hm_fill_type_holes). */
  if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name &&
      strcmp(arg->as.ident.name, "_") == 0) {
    if (hm->type_hole_count < 16) {
      hm->type_holes[hm->type_hole_count] = arg;
      hm->type_hole_vars[hm->type_hole_count] = slot->args.data[0];
      hm->type_hole_count++;
    }
    return true;
  }
  if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name &&
      !hm_env_get_ctx(env, arg->as.ident.name, arg->as.ident.syntax_ctx)) {
    const char *type_name = arg->as.ident.name;
    hm_unify(hm, slot->args.data[0],
             hm_parse_type_name(hm, type_name, self_name), arg->tok,
             "type argument");
    arg->kind = NY_E_LITERAL;
    memset(&arg->as, 0, sizeof(arg->as));
    arg->as.literal.kind = NY_LIT_STR;
    arg->as.literal.as.s.data = type_name;
    arg->as.literal.as.s.len = strlen(type_name);
  }
  (void)hm_infer_expr(hm, env, arg, self_name);
  return true;
}

/* Fills type-argument holes recorded during a call, after its arguments
 * have been unified. An unsolved hole is an error. */
static void hm_fill_type_holes(ny_hm_state_t *hm, size_t from) {
  for (size_t i = from; i < hm->type_hole_count; ++i) {
    expr_t *arg = hm->type_holes[i];
    ny_hm_type_t *t = hm_prune(hm->type_hole_vars[i]);
    char *name = t && t->kind != NY_HM_VAR ? hm_type_string(t) : NULL;
    if (!name || hm_type_is_dynamic(t)) {
      tp_add_diag_ex(hm->ctx, arg->tok, "hm", "hm-unsolved-hole", "hole", "a type",
                     "unknown", "call",
                     "the other arguments do not fix the type this hole stands for",
                     "write the type argument, or pass arguments whose types fix it",
                     "unsolved hole '_': the type argument is not determined by the "
                     "other arguments");
      free(name);
      continue;
    }
    const char *interned = ny_intern_get(ny_intern_cstr(name));
    free(name);
    arg->kind = NY_E_LITERAL;
    memset(&arg->as, 0, sizeof(arg->as));
    arg->as.literal.kind = NY_LIT_STR;
    arg->as.literal.as.s.data = interned;
    arg->as.literal.as.s.len = strlen(interned);
  }
  hm->type_hole_count = from;
}

static ny_hm_type_t *
hm_call_function_type(ny_hm_state_t *hm, ny_hm_env_list *env,
                      ny_hm_type_t *callee_t, ny_call_arg_list *args,
                      token_t tok, const char *self_name, const char *context) {
  callee_t = hm_prune(callee_t);
  if (!callee_t || hm_is_any(callee_t)) {
    tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm", "hm-dynamic-call", "call",
                    "%s uses a callee inferred as any",
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
    tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm", "hm-callable-return-any",
                    "call", "%s calls %s without a known return payload",
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
  size_t minimum = callee_t->args.len;
  if (callee_t->parameters) {
    minimum = 0;
    for (size_t i = 0; i < callee_t->parameters->len; ++i)
      if (!callee_t->parameters->data[i].def)
        minimum = i + 1;
  }
  if (given > callee_t->args.len) {
    tp_add_diag_ex(
        hm ? hm->ctx : NULL, tok, "hm", "hm-too-many-arguments",
        context ? context : "function call",
        "at most the declared parameter count", "too many arguments", "call",
        "the call supplies more positional arguments than the callable accepts",
        "remove the extra arguments or pass them as a collection",
        "too many arguments: expected at most %zu, got %zu", callee_t->args.len,
        given);
  } else if (given < minimum) {
    hm_unify_diag(hm, tok, "hm-arity-mismatch",
                  context ? context : "function call", callee_t, hm_any(hm));
  }
  size_t n = given < callee_t->args.len ? given : callee_t->args.len;
  size_t holes_before = hm->type_hole_count;
  for (size_t i = 0; i < n; ++i) {
    if (hm_bind_type_argument(hm, env, callee_t->args.data[i],
                              args->data[i].val, self_name))
      continue;
    ny_hm_type_t *arg_t = hm_infer_expr(hm, env, args->data[i].val, self_name);
    char *parameter_type = hm_type_string(callee_t->args.data[i]);
    if (parameter_type &&
        !hm_type_is_refined_instance(hm, arg_t, parameter_type) &&
        !hm_expr_has_refinement_type(hm, env, parameter_type,
                                     args->data[i].val)) {
      token_t argument_tok = args->data[i].val ? args->data[i].val->tok : tok;
      hm_check_fin_initializer(hm, parameter_type, args->data[i].val,
                               argument_tok);
      hm_check_indexed_initializer(hm, parameter_type, args->data[i].val,
                                   argument_tok);
      hm_check_refinement_initializer(hm, parameter_type, args->data[i].val,
                                      argument_tok);
    }
    free(parameter_type);
    hm_unify(hm, callee_t->args.data[i], arg_t,
             args->data[i].val ? args->data[i].val->tok : tok,
             "function argument");
  }
  for (size_t i = n; args && i < args->len; ++i)
    (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
  hm_fill_type_holes(hm, holes_before);
  return callee_t->a ? callee_t->a : hm_any(hm);
}

static ny_hm_type_t *hm_builtin_call_type(ny_hm_state_t *hm, const char *name,
                                          ny_call_arg_list *args) {
  if (!name)
    return NULL;
  const char *leaf = ny_name_leaf(name);
  if (!leaf)
    leaf = name;
  if (strcmp(name, "list") == 0 || strcmp(name, "core.list") == 0)
    return hm_unary(hm, NY_HM_LIST, hm_any(hm));
  if (strcmp(name, "tuple") == 0)
    return hm_unary(hm, NY_HM_TUPLE, hm_any(hm));
  if (strcmp(name, "dict") == 0 || strcmp(name, "core.dict") == 0)
    return hm_binary(hm, NY_HM_DICT, hm_any(hm), hm_any(hm));
  if (strcmp(name, "set") == 0 || strcmp(name, "core.set") == 0) {
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
  /*
   * The layout pipeline resolves this intrinsic's result from the literal
   * layout and field names. HM sees an opaque value in generic template bodies
   * before that metadata is available, but the call itself is still known.
   */
  if (strcmp(name, "load_layout") == 0)
    return hm_any(hm);
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
      strcmp(name, "store64_i") == 0 || strcmp(name, "store_layout") == 0 ||
      strcmp(name, "store_layout_field") == 0)
    return hm_name(hm, "nil");
  if (strcmp(leaf, "static_assert") == 0 ||
      strcmp(leaf, "assert_compile") == 0 ||
      strcmp(leaf, "assert_compile_range") == 0 ||
      strcmp(leaf, "assert_compile_index") == 0 ||
      strcmp(leaf, "proof_matches") == 0)
    return hm_name(hm, "bool");
  if (strcmp(leaf, "prove") == 0) {
    if (!args || args->len == 0)
      return hm_name(hm, "proof");
    /*
     * A lemma instance is evidence for the lemma's conclusion at those
     * arguments: `prove(add_comm_bounded(3, 4))` is `proof<3 + 4 == 4 + 3>`,
     * so it composes with the equality rules. prove() itself checks the
     * instance (premise included) before the program runs.
     */
    expr_t *claim = args->data[0].val;
    if (claim && claim->kind == NY_E_CALL && claim->as.call.callee &&
        hm->ctx && hm->ctx->cg) {
      const char *callee =
          claim->as.call.callee->kind == NY_E_IDENT
              ? ny_name_leaf(claim->as.call.callee->as.ident.name)
          : claim->as.call.callee->kind == NY_E_MEMBER
              ? claim->as.call.callee->as.member.name
              : NULL;
      stmt_t *lemma = NULL;
      for (size_t i = 0; callee && i < hm->ctx->cg->registry.lemmas.len; ++i) {
        lemma_def_t *def = hm->ctx->cg->registry.lemmas.data[i];
        if (def && def->stmt && def->name &&
            strcmp(ny_name_leaf(def->name), callee) == 0) {
          lemma = def->stmt;
          break;
        }
      }
      if (lemma && lemma->kind == NY_S_LEMMA && lemma->as.lemma.proposition &&
          claim->as.call.args.len == lemma->as.lemma.params.len) {
        expr_t *conclusion = lemma->as.lemma.proposition;
        /* `premise -> conclusion` is kept as `!premise || conclusion`. */
        if (conclusion->kind == NY_E_BINARY && conclusion->as.binary.op &&
            strcmp(conclusion->as.binary.op, "->") == 0)
          conclusion = conclusion->as.binary.right;
        else if (conclusion->kind == NY_E_LOGICAL &&
                 conclusion->as.logical.op &&
                 strcmp(conclusion->as.logical.op, "||") == 0 &&
                 conclusion->as.logical.left &&
                 conclusion->as.logical.left->kind == NY_E_UNARY &&
                 conclusion->as.logical.left->as.unary.op &&
                 strcmp(conclusion->as.logical.left->as.unary.op, "!") == 0)
          conclusion = conclusion->as.logical.right;
        char *general = ny_proof_type_from_expr(conclusion);
        stmt_t fake = {.kind = NY_S_FUNC};
        fake.as.fn.params = lemma->as.lemma.params;
        char *instance =
            general ? hm_proof_subst_args(hm, &fake, general,
                                          &claim->as.call.args, 0,
                                          claim->as.call.args.len)
                    : NULL;
        free(general);
        if (instance) {
          ny_hm_type_t *out = hm_name(hm, instance);
          free(instance);
          return out;
        }
      }
    }
    char *proof_type = ny_proof_type_from_expr(args->data[0].val);
    ny_hm_type_t *out = hm_name(hm, proof_type ? proof_type : "proof");
    free(proof_type);
    return out;
  }
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
    const char *tag = hm->ctx->cg->tagged_types.data[i].name;
    if (tag && strcmp(tag, name) == 0)
      return true;
  }
  return false;
}


static ny_hm_type_t *
hm_adt_constructor_type(ny_hm_state_t *hm, ny_hm_env_list *env,
                        enum_def_t *owner, enum_member_def_t *member,
                        ny_call_arg_list *args, const char *self_name) {
  if (!hm || !owner || !member)
    return NULL;
  size_t n = args ? args->len : 0;
  ny_hm_type_t **arg_types = n ? calloc(n, sizeof(*arg_types)) : NULL;
  if (n && !arg_types)
    return hm_name(hm, owner->name);
  for (size_t i = 0; i < n; ++i)
    arg_types[i] = hm_infer_expr(hm, env, args->data[i].val, self_name);

  if (owner->type_params.len > 0 && hm_index_member_binds(owner, member)) {
    hm_index_map_t map = {0};
    for (size_t i = 0; i < member->fields.len && i < n; ++i) {
      const char *tmpl = member->fields.data[i].type_name;
      char *got = arg_types[i] ? hm_type_string(hm_prune(arg_types[i])) : NULL;
      if (tmpl && got)
        (void)hm_index_bind(owner, member, tmpl, strlen(tmpl), got,
                            strlen(got), &map);
      free(got);
    }
    /* The evidence fixes its parameter from the bound names. */
    if (member->index_constraint_param && member->index_constraint_type) {
      char *value = hm_index_subst(member->index_constraint_type, &map);
      if (value) {
        hm_index_map_t fixed = map;
        hm_index_map_put(&fixed, member->index_constraint_param, value);
        map = fixed;
        free(value);
      }
    }
    char type_name[1024];
    size_t used = (size_t)snprintf(type_name, sizeof(type_name), "%s<",
                                   owner->name);
    for (size_t p = 0; p < owner->type_params.len && used < sizeof(type_name);
         ++p) {
      const char *param = owner->type_params.data[p];
      const char *v = param ? hm_index_map_get(&map, param, strlen(param))
                            : NULL;
      /* An unsolved type parameter is left open (`Vec.Nil` fits any element
       * type); an unsolved value index keeps its name, so it can only agree
       * with itself. */
      bool value_index = false;
      for (size_t k = 0; param && k < owner->members.len; ++k)
        if (owner->members.data[k].index_constraint_param &&
            strcmp(owner->members.data[k].index_constraint_param, param) ==
                0 &&
            hm_index_member_uses_values(&owner->members.data[k]))
          value_index = true;
      used += (size_t)snprintf(type_name + used, sizeof(type_name) - used,
                               "%s%s", p ? ", " : "",
                               v ? v
                                 : (value_index && param ? param : "any"));
    }
    if (used + 1 < sizeof(type_name))
      snprintf(type_name + used, sizeof(type_name) - used, ">");
    for (size_t i = 0; i < member->fields.len && i < n; ++i) {
      char *want_text = hm_index_subst(member->fields.data[i].type_name, &map);
      ny_hm_type_t *want =
          want_text ? hm_parse_type_name(hm, want_text, owner->name)
                    : hm_any(hm);
      free(want_text);
      hm_unify(hm, want, arg_types[i],
               args->data[i].val ? args->data[i].val->tok : (token_t){0},
               "ADT constructor field");
    }
    free(arg_types);
    return hm_parse_type_name(hm, type_name, self_name);
  }

  char type_name[1024];
  int used = snprintf(type_name, sizeof(type_name), "%s", owner->name);
  if (used > 0 && owner->type_params.len > 0 &&
      (size_t)used < sizeof(type_name)) {
    type_name[used++] = '<';
    for (size_t p = 0; p < owner->type_params.len; ++p) {
      if (p)
        type_name[used++] = ',';
      const char *actual = NULL;
      /*
       * GADT constructor evidence fixes an index directly:
       * `IntLit(int) where t is int` constructs `Expr<int>` regardless of
       * its payload types.
       */
      if (member->index_constraint_param && member->index_constraint_type &&
          owner->type_params.data[p] &&
          strcmp(member->index_constraint_param, owner->type_params.data[p]) ==
              0) {
        size_t left = sizeof(type_name) - (size_t)used;
        int nw = snprintf(type_name + used, left, "%s",
                          member->index_constraint_type);
        if (nw > 0 && (size_t)nw < left)
          used += nw;
        continue;
      }
      for (size_t i = 0; i < member->fields.len && i < n; ++i) {
        const char *field_type = member->fields.data[i].type_name;
        if (field_type && strcmp(field_type, owner->type_params.data[p]) == 0) {
          char *s = hm_type_string(arg_types[i]);
          if (s) {
            size_t left = sizeof(type_name) - (size_t)used;
            int nw = snprintf(type_name + used, left, "%s", s);
            free(s);
            if (nw > 0 && (size_t)nw < left) {
              used += nw;
              actual = type_name + used - nw;
            }
          }
          break;
        }
      }
      if (!actual) {
        const char *param = owner->type_params.data[p];
        size_t left = sizeof(type_name) - (size_t)used;
        int nw = snprintf(type_name + used, left, "%s", param ? param : "any");
        if (nw > 0 && (size_t)nw < left)
          used += nw;
      }
    }
    if ((size_t)used + 1 < sizeof(type_name))
      type_name[used++] = '>';
    type_name[used < (int)sizeof(type_name) ? used : sizeof(type_name) - 1] =
        '\0';
  }

  for (size_t i = 0; i < member->fields.len && i < n; ++i) {
    ny_hm_type_t *want = hm_adt_field_match_type(
        hm, owner, type_name, member->fields.data[i].type_name);
    hm_unify(hm, want, arg_types[i],
             args->data[i].val ? args->data[i].val->tok : (token_t){0},
             "ADT constructor field");
  }
  free(arg_types);
  return hm_parse_type_name(hm, type_name, self_name);
}

/*
 * Substitute literal earlier arguments into a dependent parameter type:
 * `fn pick(int n, Fin<n> i, ...)` called as `pick(3, i, ...)` checks `i`
 * against `Fin<3>`.  Only value-indexed parameters (`Fin<N>` and declared
 * indexed types) contribute bindings, matching the return-type rule.
 * Does `name` occur as a type index argument (after '<' or ',')?
 */
static bool hm_type_text_mentions_index(const char *text, const char *name) {
  if (!text || !name || !*name)
    return false;
  size_t n = strlen(name);
  for (const char *s = text; (s = strstr(s, name)) != NULL; s += n) {
    const char *before = s;
    while (before > text && (before[-1] == ' ' || before[-1] == '\t'))
      before--;
    if (before > text && (before[-1] == '<' || before[-1] == ','))
      return true;
  }
  return false;
}

/* Drop the ", " spacing some printers emit so canonical texts compare. */
static char *hm_proof_compact(const char *text) {
  if (!text)
    return NULL;
  size_t len = strlen(text);
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  size_t o = 0;
  bool in_str = false;
  for (size_t i = 0; i < len; ++i) {
    if (text[i] == '"')
      in_str = !in_str;
    if (!in_str && text[i] == ' ' && i > 0 && text[i - 1] == ',')
      continue;
    out[o++] = text[i];
  }
  out[o] = '\0';
  return out;
}

/*
 * Pi instantiation for proof types: replace every `name:<param>` of the
 * callee's parameters [0, limit) by the canonical term of the actual
 * argument, simultaneously, then re-normalize.  Arguments whose term is
 * opaque (`ast:`) are not substituted, so the proposition keeps the
 * parameter name and fails the exact match below instead of passing.
 */
static char *hm_proof_subst_args(ny_hm_state_t *hm, stmt_t *fn,
                                 const char *type_text, ny_call_arg_list *args,
                                 size_t arg_offset, size_t limit) {
  if (!fn || fn->kind != NY_S_FUNC || !type_text || !args ||
      strncmp(type_text, "proof<", 6) != 0)
    return NULL;
  ny_param_list *params = &fn->as.fn.params;
  if (limit > params->len)
    limit = params->len;
  char *terms[64] = {0};
  bool any = false;
  for (size_t j = arg_offset; j < limit && j < 64; ++j) {
    if (j - arg_offset >= args->len || !params->data[j].name)
      continue;
    expr_t *arg = args->data[j - arg_offset].val;
    char *full = arg ? ny_proof_type_from_expr(arg) : NULL;
    size_t fl = full ? strlen(full) : 0;
    if (full && fl > 7 && strncmp(full, "proof<", 6) == 0 &&
        full[fl - 1] == '>' && !strstr(full, "ast:")) {
      terms[j] = ny_strndup(full + 6, fl - 7);
      any = terms[j] != NULL;
    }
    free(full);
  }
  char *text = any ? hm_proof_compact(type_text) : NULL;
  char *out = NULL;
  if (text) {
    ny_strbuf_t b;
    ny_strbuf_init(&b);
    bool changed = false;
    for (size_t i = 0; text[i];) {
      bool start_ok = i == 0 || text[i - 1] == '(' || text[i - 1] == ',' ||
                      text[i - 1] == '<';
      if (start_ok && strncmp(text + i, "name:", 5) == 0) {
        size_t n = 5;
        while (text[i + n] && text[i + n] != ',' && text[i + n] != ')' &&
               text[i + n] != '>')
          ++n;
        const char *term = NULL;
        for (size_t j = arg_offset; j < limit && j < 64 && !term; ++j)
          if (terms[j] && strlen(params->data[j].name) == n - 5 &&
              strncmp(text + i + 5, params->data[j].name, n - 5) == 0)
            term = terms[j];
        if (term) {
          ny_strbuf_append(&b, term);
          changed = true;
        } else {
          ny_strbuf_append_n(&b, text + i, n);
        }
        i += n;
        continue;
      }
      ny_strbuf_append_n(&b, text + i, 1);
      ++i;
    }
    char *subst = ny_strbuf_take(&b);
    if (changed && subst) {
      size_t sl = strlen(subst);
      char *inner = sl > 7 ? ny_strndup(subst + 6, sl - 7) : NULL;
      expr_t *prop =
          inner ? ny_proof_expr_from_canonical(&hm->arena, inner) : NULL;
      out = prop ? ny_proof_type_from_expr(prop) : NULL;
      if (!out)
        out = ny_strdup(subst);
      free(inner);
    }
    free(subst);
  }
  free(text);
  for (size_t j = 0; j < 64; ++j)
    free(terms[j]);
  return out;
}

static char *hm_subst_param_type_text(ny_hm_state_t *hm, stmt_t *fn,
                                      size_t param_index,
                                      ny_call_arg_list *args,
                                      size_t arg_offset) {
  if (!hm || !fn || fn->kind != NY_S_FUNC || !args)
    return NULL;
  ny_param_list *params = &fn->as.fn.params;
  if (param_index >= params->len)
    return NULL;
  param_t *target = &params->data[param_index];
  if (!target->type)
    return NULL;
  if (strncmp(target->type, "proof<", 6) == 0)
    return hm_proof_subst_args(hm, fn, target->type, args, arg_offset,
                               param_index);
  char *text = ny_strdup(target->type);
  if (!text)
    return NULL;
  bool changed = false;
  for (size_t j = 0; j < param_index && j < params->len; ++j) {
    param_t *prev = &params->data[j];
    if (!prev->name || !prev->type)
      continue;
    if (j < arg_offset || j - arg_offset >= args->len)
      continue;
    expr_t *arg = args->data[j - arg_offset].val;
    if (!arg || arg->kind != NY_E_LITERAL ||
        arg->as.literal.kind != NY_LIT_INT ||
        arg->as.literal.integer_outside_i64)
      continue;
    /*
     * Only a value used inside type-index position is substituted, so an
     * ordinary parameter name is not rewritten where it appears as a type
     * constructor or an unrelated identifier.
     */
    if (!hm_type_text_mentions_index(text, prev->name))
      continue;
    char *next =
        hm_replace_type_index_owned(text, prev->name, arg->as.literal.as.i);
    if (!next)
      break;
    free(text);
    text = next;
    changed = true;
  }
  if (!changed) {
    free(text);
    return NULL;
  }
  return text;
}


/* Binds `name` where it occurs in the type text `tmpl` against `actual`:
 * `Fin<n>` against `Fin<5>` gives 5; `Vec<A, n+1>` against `Vec<int, 3>`
 * gives 2. */
static bool hm_bind_value_binder(const char *tmpl, size_t tlen,
                                 const char *actual, size_t alen,
                                 const char *name, char *out, size_t cap) {
  while (tlen && tmpl[tlen - 1] == ' ')
    tlen--;
  while (alen && actual[alen - 1] == ' ')
    alen--;
  while (tlen && *tmpl == ' ') {
    tmpl++;
    tlen--;
  }
  while (alen && *actual == ' ') {
    actual++;
    alen--;
  }
  char t[256], a[256];
  if (tlen >= sizeof(t) || alen >= sizeof(a))
    return false;
  memcpy(t, tmpl, tlen);
  t[tlen] = '\0';
  memcpy(a, actual, alen);
  a[alen] = '\0';
  if (strcmp(t, name) == 0) {
    char an[64];
    int64_t off = 0;
    if (!ny_index_parse(a, an, sizeof(an), &off) || an[0])
      return false; /* only a known literal fills a runtime binder */
    snprintf(out, cap, "%lld", (long long)off);
    return true;
  }
  char var[64], solved[160];
  if ((strchr(t, '+') || strchr(t, '-')) && !strchr(t, '<') &&
      ny_index_solve(t, a, var, sizeof(var), solved, sizeof(solved)) &&
      strcmp(var, name) == 0) {
    char sn[64];
    int64_t off = 0;
    if (!ny_index_parse(solved, sn, sizeof(sn), &off) || sn[0])
      return false;
    snprintf(out, cap, "%lld", (long long)off);
    return true;
  }
  const char *tp[16], *ap[16];
  size_t tl[16], al[16], tb = 0, ab = 0;
  size_t tn = hm_index_split_args(t, tp, tl, 16, &tb);
  size_t an2 = hm_index_split_args(a, ap, al, 16, &ab);
  if (!tn || tn != an2 || tb != ab || strncmp(t, a, tb) != 0)
    return false;
  for (size_t i = 0; i < tn; ++i)
    if (hm_bind_value_binder(tp[i], tl[i], ap[i], al[i], name, out, cap))
      return true;
  return false;
}

/*
 * A call that omits exactly its implicit value binders gets them inserted,
 * solved from the other arguments' types. The call node is rewritten once,
 * so later passes and lowering see an ordinary full call.
 */
static void hm_fill_implicit_binders(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     stmt_t *fn, ny_call_arg_list *args,
                                     token_t tok, const char *self_name) {
  if (!hm || !fn || fn->kind != NY_S_FUNC || !args || !hm->ctx ||
      !hm->ctx->cg)
    return;
  size_t implicits = 0;
  while (implicits < fn->as.fn.params.len &&
         fn->as.fn.params.data[implicits].implicit)
    implicits++;
  if (implicits == 0)
    return;
  /* `bound(_, i)`: holes written for the implicit binders are filled the
   * same way as omitted ones. */
  if (args->len == fn->as.fn.params.len) {
    for (size_t k = 0; k < implicits; ++k) {
      const expr_t *a = args->data[k].val;
      if (!a || a->kind != NY_E_IDENT || !a->as.ident.name ||
          strcmp(a->as.ident.name, "_") != 0)
        return;
    }
    memmove(args->data, args->data + implicits,
            (args->len - implicits) * sizeof(*args->data));
    args->len -= implicits;
  }
  if (args->len + implicits != fn->as.fn.params.len)
    return;
  for (size_t i = 0; i < args->len; ++i)
    if (args->data[i].name)
      return;
  char values[8][64];
  if (implicits > 8)
    return;
  for (size_t k = 0; k < implicits; ++k) {
    const char *name = fn->as.fn.params.data[k].name;
    bool solved = false;
    for (size_t j = 0; j < args->len && !solved; ++j) {
      const param_t *formal = &fn->as.fn.params.data[implicits + j];
      if (!formal->type || !strstr(formal->type, name))
        continue;
      ny_hm_type_t *got =
          hm_prune(hm_infer_expr(hm, env, args->data[j].val, self_name));
      char *got_s = got ? hm_type_string(got) : NULL;
      if (got_s)
        solved = hm_bind_value_binder(formal->type, strlen(formal->type),
                                      got_s, strlen(got_s), name, values[k],
                                      sizeof(values[k]));
      free(got_s);
    }
    if (!solved) {
      tp_add_diag_ex(hm->ctx, tok, "hm", "hm-implicit-binder-unsolved",
                     "implicit binder", name, "unknown", "call",
                     "an implicit value binder is read from the argument "
                     "types; none of them fixes it to a literal",
                     "pass an argument whose type gives the binder a value, "
                     "or call with every argument written out",
                     "cannot infer implicit binder '%s' from the arguments",
                     name);
      return;
    }
  }
  arena_t *arena = hm->ctx->cg->arena;
  size_t len = args->len + implicits;
  call_arg_t *data = arena_alloc(arena, len * sizeof(*data));
  if (!data)
    return;
  memset(data, 0, len * sizeof(*data));
  for (size_t k = 0; k < implicits; ++k) {
    expr_t *lit = arena_alloc(arena, sizeof(*lit));
    if (!lit)
      return;
    memset(lit, 0, sizeof(*lit));
    lit->kind = NY_E_LITERAL;
    lit->tok = tok;
    lit->as.literal.kind = NY_LIT_INT;
    lit->as.literal.as.i = strtoll(values[k], NULL, 10);
    data[k].val = lit;
  }
  memcpy(data + implicits, args->data, args->len * sizeof(*data));
  args->data = data;
  args->len = len;
  args->cap = len;
}

static ny_hm_type_t *hm_call_named(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   const char *name, expr_t *target,
                                   ny_call_arg_list *args, token_t tok,
                                   const char *self_name) {
  const char *leaf = ny_name_leaf(name);
  /*
   * Compiler-provided numeric leaves have no ordinary source declaration.
   * Resolve their intrinsic result before consulting imported signatures,
   * which may be the permissive runtime placeholder and otherwise erase the
   * f64 fact from inferred locals.
   */
  if (leaf && strncmp(leaf, "__flt_", 6) == 0) {
    ny_hm_type_t *intrinsic = hm_builtin_call_type(hm, name, args);
    if (intrinsic) {
      for (size_t i = 0; args && i < args->len; ++i)
        (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
      return intrinsic;
    }
  }
  bool allow_dynamic_literal_args = hm_call_is_layout_from(hm, name);
  if (!target && hm && hm->ctx && hm->ctx->cg && name) {
    enum_def_t *owner = NULL;
    enum_member_def_t *member =
        lookup_enum_member_owner(hm->ctx->cg, name, &owner);
    if (member && owner)
      return hm_adt_constructor_type(hm, env, owner, member, args, self_name);
  }
  if (!target && name) {
    const char *refine_base = hm_lookup_refine_base(hm, name);
    if (refine_base) {
      for (size_t i = 0; args && i < args->len; ++i)
        (void)hm_infer_expr(hm, env, args->data[i].val, self_name);
      return hm_name(hm, "bool");
    }
  }
  if (!target && leaf && args) {
    if ((strcmp(leaf, "require_shape") == 0 ||
         strcmp(leaf, "assert_shape") == 0) &&
        args->len >= 2) {
      if (args->data[1].val && args->data[1].val->kind == NY_E_LITERAL &&
          args->data[1].val->as.literal.kind == NY_LIT_STR &&
          args->data[1].val->as.literal.as.s.data) {
        ny_hm_type_t *refined = hm_parse_type_name(
            hm, args->data[1].val->as.literal.as.s.data, self_name);
        if (refined)
          return refined;
      }
    }
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
  if (!target && leaf && ny_proof_rule_name(leaf) && args) {
    /*
     * Equality kernel rules: the result proposition is computed from the
     * argument propositions, so these are typed here rather than by a
     * fixed signature.  eq_cong's last argument names the function.
     */
    const char *props[2] = {NULL, NULL};
    char *owned[2] = {NULL, NULL};
    size_t nproofs = 0;
    const char *fn_name = NULL;
    bool cong = strcmp(leaf, "eq_cong") == 0;
    for (size_t i = 0; i < args->len; ++i) {
      expr_t *arg = args->data[i].val;
      if (cong && i == args->len - 1 && arg && arg->kind == NY_E_IDENT) {
        fn_name = arg->as.ident.name;
        continue;
      }
      ny_hm_type_t *at = hm_prune(hm_infer_expr(hm, env, arg, self_name));
      if (nproofs < 2) {
        owned[nproofs] = hm_type_string(at);
        props[nproofs] = owned[nproofs];
      }
      ++nproofs;
    }
    const char *reason = NULL;
    /*
     * Indexed transport: `transport(p, v)` with `p : proof<n == m>` and
     * `v : Vec<int, n>` is `v` at `Vec<int, m>`. The equation's sides are
     * index terms here, rewritten wherever they occur as a type argument.
     */
    if (strcmp(leaf, "transport") == 0 && nproofs == 2 && props[0] &&
        props[1] && strncmp(props[1], "proof<", 6) != 0) {
      char *data = ny_proof_index_transport(props[0], props[1], &reason);
      free(owned[0]);
      free(owned[1]);
      if (!data) {
        tp_add_diag_ex(hm->ctx, tok, "hm", "proof-rule-misuse", leaf, "proof",
                       NULL, NULL,
                       "transport moves a value along an equation between "
                       "the indices of its type",
                       "pass proof<a == b> where a or b is an index of the "
                       "value's type",
                       "%s: %s", leaf, reason ? reason : "invalid arguments");
        return hm_any(hm);
      }
      ny_hm_type_t *moved = hm_parse_type_name(hm, data, self_name);
      free(data);
      return moved;
    }
    char *result = nproofs <= 2 ? ny_proof_rule_result(leaf, props, nproofs,
                                                      fn_name, &reason)
                                : NULL;
    free(owned[0]);
    free(owned[1]);
    if (!result) {
      tp_add_diag_ex(hm->ctx, tok, "hm", "proof-rule-misuse", leaf, "proof",
                     NULL, NULL,
                     "equality rules take proofs of equations and compute "
                     "the proposition they establish",
                     "pass proof<a == b> evidence built by prove/refl or "
                     "another rule",
                     "%s: %s", leaf, reason ? reason : "invalid arguments");
      return hm_name(hm, "proof");
    }
    ny_hm_type_t *out = hm_name(hm, result);
    free(result);
    return out;
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
  /*
   * A bare-name call made from inside module M must resolve to M's own
   * declaration before any other module's same-leaf function.  The plain
   * name lookup returns the most recently registered scheme, so
   * `shell(...)` inside the os facade type-checked against
   * os.interact.shell (int timeout) instead of os.shell (bool, bool) and
   * every call failed with "expected int, got bool".
   */
  ny_hm_scheme_t *scheme = NULL;
  if (self_name && *self_name && leaf && name && !strchr(name, '.') &&
      strcmp(leaf, name) == 0) {
    char owned[512];
    int qn = snprintf(owned, sizeof(owned), "%s.%s", self_name, leaf);
    if (qn > 0 && (size_t)qn < sizeof(owned))
      scheme = hm_find_scheme(hm, owned);
  }
  if (!scheme)
    scheme = hm_find_scheme(hm, name);

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
                ny_hm_type_t *param_t =
                    hm_parse_type_name(hm, params->data[param].type, self_name);
                if (!hm_type_is_refined_instance(hm, arg_t,
                                                 params->data[param].type))
                  hm_check_indexed_initializer(
                      hm, params->data[param].type, args->data[i].val,
                      args->data[i].val ? args->data[i].val->tok : tok);
                if (!hm_expr_has_refinement_type(
                        hm, env, params->data[param].type, args->data[i].val) &&
                    !hm_type_is_refined_instance(hm, arg_t,
                                                 params->data[param].type))
                  hm_check_refinement_initializer(
                      hm, params->data[param].type, args->data[i].val,
                      args->data[i].val ? args->data[i].val->tok : tok);
                hm_check_refinement_return(
                    hm, param_t, arg_t, args->data[i].val,
                    args->data[i].val ? args->data[i].val->tok : tok);
                hm_unify(hm, param_t, arg_t,
                         args->data[i].val ? args->data[i].val->tok : tok,
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
          ny_hm_type_t *out = hm_sig_return_type_for_call(
              hm, attached, stmt, args, 1u,
              attached_self ? attached_self : self_name);
          free(owner);
          return out;
        }
        free(owner);
      }
    }
    /*
     * A member call that has a receiver cannot fall through to a free
     * function with the same leaf name.  Only the attached-method lookup
     * above may satisfy that call; otherwise `poly_elem.get(...)` can bind to
     * an unrelated root `get(Fin<N>)` while HM checks another module.
     */
    fun_sig *sig =
        hm && hm->ctx && hm->ctx->cg && !(target && name && strchr(name, '.'))
            ? lookup_fun(hm->ctx->cg, name, 0)
            : NULL;
    if (sig && self_name && *self_name &&
        (!sig->module_name || !*sig->module_name))
      sig = NULL;
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
            ny_hm_type_t *param_t =
                hm_parse_type_name(hm, params->data[param].type, self_name);
            if (!hm_type_is_refined_instance(hm, arg_t,
                                             params->data[param].type))
              hm_check_indexed_initializer(
                  hm, params->data[param].type, args->data[i].val,
                  args->data[i].val ? args->data[i].val->tok : tok);
            if (!hm_expr_has_refinement_type(hm, env, params->data[param].type,
                                             args->data[i].val) &&
                !hm_type_is_refined_instance(hm, arg_t,
                                             params->data[param].type))
              hm_check_refinement_initializer(
                  hm, params->data[param].type, args->data[i].val,
                  args->data[i].val ? args->data[i].val->tok : tok);
            hm_check_refinement_return(
                hm, param_t, arg_t, args->data[i].val,
                args->data[i].val ? args->data[i].val->tok : tok);
            hm_unify(hm, param_t, arg_t,
                     args->data[i].val ? args->data[i].val->tok : tok,
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
      return hm_sig_return_type_for_call(hm, sig, sig->stmt_t, args,
                                         sig->stmt_t ? (target ? 1u : 0u) : 0u,
                                         self_name);
    }
    if (target || (name && strchr(name, '.'))) {
      if (target)
        (void)hm_infer_expr(hm, env, target, self_name);
      for (size_t i = 0; args && i < args->len; ++i)
        (void)hm_infer_expr_dynamic_literal_ok(
            hm, env, args->data[i].val, self_name, allow_dynamic_literal_args);
      tp_add_fallback(hm ? hm->ctx : NULL, tok, "hm", "hm-unknown-call", "call",
                      "unresolved call '%s' returns any",
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
  if (!target && scheme->stmt)
    hm_fill_implicit_binders(hm, env, scheme->stmt, args, tok, self_name);
  ny_hm_type_t *fn = hm_instantiate(hm, scheme->type);
  fn = hm_prune(fn);
  if (!fn || fn->kind != NY_HM_FN) {
    if (target)
      (void)hm_infer_expr(hm, env, target, self_name);
    for (size_t i = 0; args && i < args->len; ++i)
      (void)hm_infer_expr_dynamic_literal_ok(
          hm, env, args->data[i].val, self_name, allow_dynamic_literal_args);
    return hm_any(hm);
  }
  size_t offset = target ? 1u : 0u;
  size_t given = args ? args->len : 0;
  size_t available = fn->args.len > offset ? fn->args.len - offset : 0;
  if (!hm_scheme_is_variadic(scheme) && given > available) {
    tp_add_diag_ex(
        hm ? hm->ctx : NULL, tok, "hm", "hm-too-many-arguments",
        "function call", "at most the declared parameter count",
        "too many arguments", "call",
        "the call supplies more positional arguments than the callee accepts",
        "remove the extra arguments or pass them as a collection",
        "too many arguments: expected at most %zu, got %zu", available, given);
  } else if (!hm_scheme_is_variadic(scheme) &&
             given < hm_scheme_min_args(scheme, offset)) {
    hm_unify_diag(hm, tok, "hm-arity-mismatch", "function call", fn,
                  hm_any(hm));
  } else if (hm_scheme_is_variadic(scheme) &&
             given < hm_scheme_variadic_min_args(scheme, offset)) {
    tp_add_diag_ex(
        hm ? hm->ctx : NULL, tok, "hm", "hm-missing-arguments",
        "variadic function call", "all required fixed parameters",
        "too few arguments", "call",
        "a variadic function still requires its fixed parameters; automatic "
        "partial application does not apply to variadic calls",
        "supply the missing fixed arguments; the variadic tail may stay empty",
        "variadic function call: expected at least %zu arguments, got %zu",
        hm_scheme_variadic_min_args(scheme, offset), given);
  }
  if (target && fn->args.len > 0)
    hm_unify(hm, fn->args.data[0], hm_infer_expr(hm, env, target, self_name),
             tok, "member call target");
  size_t named_holes_before = hm->type_hole_count;
  for (size_t i = 0; args && i < args->len; ++i) {
    if (i + offset < fn->args.len &&
        hm_bind_type_argument(hm, env, fn->args.data[i + offset],
                              args->data[i].val, self_name))
      continue;
    ny_hm_type_t *arg_t = hm_infer_expr_dynamic_literal_ok(
        hm, env, args->data[i].val, self_name, allow_dynamic_literal_args);
    size_t param = i + offset;
    /* A type parameter already fixed by an earlier argument of this call. */
    if (param < fn->args.len && scheme && scheme->stmt &&
        scheme->stmt->kind == NY_S_FUNC &&
        param < scheme->stmt->as.fn.erased_param_types.len &&
        scheme->stmt->as.fn.type_params.len > 0) {
      const char *annotated = scheme->stmt->as.fn.erased_param_types.data[param];
      const char *type_param = NULL;
      for (size_t k = 0; annotated && k < scheme->stmt->as.fn.type_params.len;
           ++k)
        if (strcmp(annotated, scheme->stmt->as.fn.type_params.data[k]) == 0)
          type_param = annotated;
      ny_hm_type_t *slot = hm_prune(fn->args.data[param]);
      ny_hm_type_t *got = hm_prune(arg_t);
      if (type_param && slot && got && slot->kind != NY_HM_VAR &&
          got->kind != NY_HM_VAR && !hm_type_is_dynamic(got) &&
          !hm_type_is_dynamic(slot)) {
        char *want_s = hm_type_string(slot), *got_s = hm_type_string(got);
        bool differ = want_s && got_s && strcmp(want_s, got_s) != 0;
        if (differ)
          tp_add_diag_ex(
              hm->ctx, args->data[i].val ? args->data[i].val->tok : tok, "hm",
              "hm-type-param-inconsistent", "function argument", want_s,
              got_s, "call",
              "every argument annotated with a type parameter must agree",
              "pass values of one type, or give the parameters distinct "
              "type parameters",
              "type parameter '%s' instantiated inconsistently: '%s' here, "
              "'%s' earlier in this call",
              type_param, got_s, want_s);
        free(want_s);
        free(got_s);
        if (differ)
          continue;
      }
    }
    if (param < fn->args.len) {
      bool param_substituted = false;
      if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC) {
        ny_param_list *sparams = &scheme->stmt->as.fn.params;
        if (param < sparams->len && sparams->data[param].type) {
          char *subst_param =
              hm_subst_param_type_text(hm, scheme->stmt, param, args, offset);
          const char *param_type_text =
              subst_param ? subst_param : sparams->data[param].type;
          ny_hm_type_t *param_t =
              hm_parse_type_name(hm, param_type_text, self_name);
          hm_check_sigma_initializer(
              hm, env, param_type_text, args->data[i].val, self_name,
              args->data[i].val ? args->data[i].val->tok : tok);
          if (!hm_type_is_refined_instance(hm, arg_t, param_type_text))
            hm_check_indexed_initializer(
                hm, param_type_text, args->data[i].val,
                args->data[i].val ? args->data[i].val->tok : tok);
          if (!hm_expr_has_refinement_type(hm, env, param_type_text,
                                           args->data[i].val) &&
              !hm_type_is_refined_instance(hm, arg_t, param_type_text))
            hm_check_refinement_initializer(
                hm, param_type_text, args->data[i].val,
                args->data[i].val ? args->data[i].val->tok : tok);
          hm_check_refinement_return(hm, param_t, arg_t, args->data[i].val,
                                     args->data[i].val ? args->data[i].val->tok
                                                       : tok);
          if (subst_param) {
            hm_unify(hm, param_t, arg_t,
                     args->data[i].val ? args->data[i].val->tok : tok,
                     "function argument");
            param_substituted = true;
          }
          free(subst_param);
        }
      }
      if (!param_substituted)
        hm_unify(hm, fn->args.data[param], arg_t,
                 args->data[i].val ? args->data[i].val->tok : tok,
                 "function argument");
    }
  }
  hm_fill_type_holes(hm, named_holes_before);
  if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
      scheme->stmt->as.fn.attr_async_effects)
    return hm_name(hm, "handle");
  ny_hm_type_t *ret = fn->a ? fn->a : hm_any(hm);
  if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC)
    ret =
        hm_subst_return_indices(hm, scheme->stmt, ret, args, offset, self_name);
  return ret;
}

/*
 * A bare call inside a type's method names the module function: resolving
 * `divs(v, s)` to the enclosing `vec3.divs` recursed forever.
 */
static bool hm_scheme_is_method(const ny_hm_scheme_t *scheme) {
  if (!scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC ||
      scheme->stmt->as.fn.params.len == 0)
    return false;
  /*
   * The parser rewrites `self` to the owner type, so a method is a function
   * whose first parameter has the type its qualified name is nested under.
   */
  const char *first_type = scheme->stmt->as.fn.params.data[0].type;
  const char *full =
      scheme->full_name ? scheme->full_name : scheme->stmt->as.fn.name;
  const char *leaf_dot = full ? strrchr(full, '.') : NULL;
  if (!first_type || !leaf_dot || leaf_dot == full)
    return false;
  const char *owner_end = leaf_dot;
  const char *owner = owner_end;
  while (owner > full && owner[-1] != '.')
    --owner;
  size_t owner_len = (size_t)(owner_end - owner);
  return owner_len > 0 && strncmp(first_type, owner, owner_len) == 0 &&
         (first_type[owner_len] == '\0' || first_type[owner_len] == '<');
}

static void hm_persist_named_call_target(ny_hm_state_t *hm, expr_t *call,
                                         const char *name,
                                         const char *self_name) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !call || !name)
    return;
  ny_hm_scheme_t *declared = hm_find_scheme(hm, name);
  if (declared && declared->stmt && declared->stmt->kind == NY_S_FUNC &&
      !ny_is_stdlib_tok(declared->stmt->tok) && call->tok.filename &&
      declared->stmt->tok.filename &&
      strcmp(call->tok.filename, declared->stmt->tok.filename) == 0) {
    const char *canonical =
        declared->full_name ? declared->full_name : declared->stmt->as.fn.name;
    call->semantic.canonical_callee = ny_intern_get(ny_intern_cstr(canonical));
    call->semantic.canonical_callee_stmt = declared->stmt;
    return;
  }
  const char *leaf = ny_name_leaf(name);
  if (self_name && *self_name && leaf && !strchr(name, '.')) {
    char owned_name[512];
    int n = snprintf(owned_name, sizeof(owned_name), "%s.%s", self_name, leaf);
    if (n > 0 && (size_t)n < sizeof(owned_name)) {
      /*
       * The module's own function outranks the global leaf lookup below,
       * for stdlib modules too: the table can answer with any module that
       * registered the same leaf (`save` in tga bound to render.dump.save).
       */
      ny_hm_scheme_t *owned = hm_find_scheme(hm, owned_name);
      if (owned && owned->stmt && owned->stmt->kind == NY_S_FUNC &&
          !hm_scheme_is_method(owned)) {
        const char *canonical =
            owned->full_name ? owned->full_name : owned->stmt->as.fn.name;
        call->semantic.canonical_callee =
            ny_intern_get(ny_intern_cstr(canonical));
        call->semantic.canonical_callee_stmt = owned->stmt;
        return;
      }
    }
  }
  if (call->semantic.canonical_callee_stmt)
    return;
  fun_sig *sig = lookup_fun(hm->ctx->cg, name, 0);
  if (sig && self_name && *self_name &&
      (!sig->module_name || !*sig->module_name))
    sig = NULL;
  if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return;
  call->semantic.canonical_callee = ny_intern_get(ny_intern_cstr(sig->name));
  call->semantic.canonical_callee_stmt = sig->stmt_t;
}

static ny_hm_type_t *hm_infer_binary(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     expr_t *e, const char *self_name) {
  ny_hm_type_t *lt = hm_infer_expr(hm, env, e->as.binary.left, self_name);
  ny_hm_type_t *rt = hm_infer_expr(hm, env, e->as.binary.right, self_name);
  const char *op = e->as.binary.op;
  if (op && strcmp(op, "..") == 0) {
    ny_hm_type_t *int_t = hm_name(hm, "int");
    hm_unify(hm, lt, int_t, e->tok, "range bound");
    hm_unify(hm, rt, int_t, e->tok, "range bound");
    return hm_name(hm, "range");
  }
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
      bool lnum = hm_type_known_numeric(hm, lt, &ln);
      bool rnum = hm_type_known_numeric(hm, rt, &rn);
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
  if (op && strcmp(op, "..") == 0) {
    ny_hm_type_t *int_t = hm_name(hm, "int");
    hm_unify(hm, lt, int_t, e->tok, "range bound");
    hm_unify(hm, rt, int_t, e->tok, "range bound");
    return hm_name(hm, "range");
  }
  if (hm_is_any(lt) || hm_is_any(rt)) {
    char *ls = hm_type_string(lt);
    char *rs = hm_type_string(rt);
    char got[512];
    snprintf(got, sizeof(got), "%s %s %s", ls ? ls : "any", op ? op : "op",
             rs ? rs : "any");
    tp_add_fallback(hm ? hm->ctx : NULL, e->tok, "hm", "hm-dynamic-arithmetic",
                    "binary", "arithmetic operands inferred as %s", got);
    hm_strict_diag(hm, e->tok, "hm-strict-dynamic-arithmetic",
                   "arithmetic expression",
                   "numeric/string/collection operands", got, "binary",
                   "strict type checks reject dynamic arithmetic operands");
    free(ls);
    free(rs);
    return hm_any(hm);
  }
  /*
   * BigInt operators keep the opaque handle representation on every route.
   * The primitive numeric predicate intentionally recognizes only machine
   * integers and floats; allowing these known BigInt operators to fall
   * through that predicate erased the result type to `any`, so a chained
   * property such as `(Z(2) ^ Z(8)).str` reached the NYIR VM as a raw pointer.
   */
  if (hm_is_name(lt, "bigint") || hm_is_name(rt, "bigint"))
    return hm_name(hm, "bigint");
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
  bool lnum = hm_type_known_numeric(hm, lt, &ln);
  bool rnum = hm_type_known_numeric(hm, rt, &rn);
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
  const char *ls_base = hm_resolve_refine_root_base(hm, ls);
  const char *rs_base = hm_resolve_refine_root_base(hm, rs);
  const char *ls_c = ls_base ? ls_base : ls;
  const char *rs_c = rs_base ? rs_base : rs;
  if ((tp_is_int_type(ls_c) || tp_is_float_type(ls_c)) &&
      (tp_is_int_type(rs_c) || tp_is_float_type(rs_c))) {
    bool f = tp_is_float_type(ls_c) || tp_is_float_type(rs_c);
    /*
     * f32 arithmetic stays in the f32 domain.  Joining f32+f32 to f64 here
     * stamped the result and every downstream use with the f64 rep while
     * the native ABI still computes f32, so a later f32 slot was reloaded
     * as f64 (`movsd`) and produced garbage (e.g. regalloc oracle tests).
     */
    const char *ls_f = tp_skip_nullable(ls_c);
    const char *rs_f = tp_skip_nullable(rs_c);
    bool both_f32 = f && ls_f && rs_f && strcmp(ls_f, "f32") == 0 &&
                    strcmp(rs_f, "f32") == 0;
    bool big =
        !f && (strcmp(ls_c, "bigint") == 0 || strcmp(rs_c, "bigint") == 0 ||
               ny_tp_const_expr_promotes_bigint(e));
    free(ls);
    free(rs);
    return hm_name(hm,
                   f ? (both_f32 ? "f32" : "f64") : (big ? "bigint" : "int"));
  }
  free(ls);
  free(rs);
  return hm_any(hm);
}

/*
 * Runtime-index constructor.  `checked_fin(value, bound)` is an ordinary
 * library function that performs the dynamic bounds test; the checker also
 * records the bound in the result type so a `Fin<n>` parameter can be
 * satisfied by a value whose bound is only known while the program runs.
 * Literal bounds are range-checked here, and the library call still emits
 * the runtime test, so a symbolic bound never pretends the fact was proved.
 */
static ny_hm_type_t *hm_try_checked_fin_call(ny_hm_state_t *hm,
                                             ny_hm_env_list *env, expr_t *e,
                                             const char *self_name) {
  if (!hm || !e || e->kind != NY_E_CALL || e->as.call.args.len != 2)
    return NULL;
  expr_t *callee = e->as.call.callee;
  while (callee && callee->kind == NY_E_SPLICE)
    callee = callee->as.splice.expr;
  if (!callee || callee->kind != NY_E_IDENT || !callee->as.ident.name)
    return NULL;
  const char *leaf = ny_name_leaf(callee->as.ident.name);
  if (!leaf || strcmp(leaf, "checked_fin") != 0)
    return NULL;
  if (hm->ctx && hm->ctx->cg) {
    fun_sig *sig = lookup_use_module_fun(hm->ctx->cg, leaf, 2);
    ny_hm_scheme_t *scheme =
        sig && sig->name ? hm_find_scheme(hm, sig->name) : NULL;
    if (!sig || !scheme || !scheme->stmt ||
        !ny_is_stdlib_tok(scheme->stmt->tok))
      return NULL;
  } else {
    return NULL;
  }
  expr_t *value_e = e->as.call.args.data[0].val;
  expr_t *bound_e = e->as.call.args.data[1].val;
  if (!value_e || !bound_e)
    return NULL;
  ny_hm_type_t *want_int = hm_parse_type_name(hm, "int", self_name);
  ny_hm_type_t *value_t = hm_infer_expr(hm, env, value_e, self_name);
  hm_unify(hm, want_int, value_t, value_e->tok, "checked Fin value");
  ny_hm_type_t *bound_t = hm_infer_expr(hm, env, bound_e, self_name);
  hm_unify(hm, want_int, bound_t, bound_e->tok, "checked Fin bound");

  if (bound_e->kind == NY_E_LITERAL && bound_e->as.literal.kind == NY_LIT_INT &&
      !bound_e->as.literal.integer_outside_i64) {
    int64_t bound = bound_e->as.literal.as.i;
    if (bound <= 0) {
      tp_add_diag_ex(hm->ctx, bound_e->tok, "hm", "hm-checked-fin-bound",
                     "runtime Fin construction", "positive Fin bound",
                     "non-positive bound", "checked_fin",
                     "a runtime Fin bound must be positive",
                     "use a bound greater than zero",
                     "checked Fin bound must be positive, got %" PRId64, bound);
      return hm_name(hm, "int");
    }
    if (value_e->kind == NY_E_LITERAL &&
        value_e->as.literal.kind == NY_LIT_INT &&
        !value_e->as.literal.integer_outside_i64) {
      int64_t value = value_e->as.literal.as.i;
      if (value < 0 || value >= bound) {
        tp_add_diag_ex(hm->ctx, e->tok, "hm", "hm-checked-fin-range",
                       "runtime Fin construction",
                       "value in the half-open interval [0, N)",
                       "out-of-range literal", "checked_fin",
                       "the runtime bound check rejects out-of-range values",
                       "pass a value in range or adjust the bound",
                       "checked Fin value %" PRId64 " is outside [0, %" PRId64
                       ")",
                       value, bound);
        return hm_name(hm, "int");
      }
    }
    char fin[64];
    snprintf(fin, sizeof(fin), "Fin<%" PRId64 ">", bound);
    return hm_name(hm, fin);
  }
  if (bound_e->kind == NY_E_IDENT && bound_e->as.ident.name &&
      *bound_e->as.ident.name) {
    char *fin = hm_type_format("Fin<%s>", bound_e->as.ident.name);
    if (fin) {
      ny_hm_type_t *out = hm_name(hm, fin);
      free(fin);
      return out;
    }
  }
  /*
   * Other bound expressions are checked at runtime only; no type-level index
   * is available, so the result degrades to a plain integer.
   */
  return hm_name(hm, "int");
}

static ny_hm_type_t *hm_infer_expr_impl(ny_hm_state_t *hm, ny_hm_env_list *env,
                                        expr_t *e, const char *self_name) {
  if (!e)
    return hm_any(hm);
  switch (e->kind) {
  case NY_E_LITERAL:
    return hm_name(hm, tp_literal_type(&e->as.literal, e->tok));
  case NY_E_IDENT: {
    if (hm && hm->comptime_depth > 0 && e->as.ident.name &&
        hm_is_mutable_binding(hm, env, e->as.ident.name, e->as.ident.syntax_ctx,
                              hm->comptime_outer_env_len)) {
      tp_add_diag_ex(hm->ctx, e->tok, "hm", "hm-comptime-mut-capture",
                     "comptime", "immutable compile-time value",
                     "runtime mutable binding", "comptime",
                     "comptime{} is closed code and cannot capture runtime "
                     "mutable variables",
                     "pass the value as an explicit compile-time parameter or "
                     "make the binding immutable",
                     "cannot capture runtime mutable variable '%s' in "
                     "compile-time evaluation",
                     e->as.ident.name);
    }
    if (e->as.ident.name && e->as.ident.name[0] == '?') {
      for (size_t h = 0; hm && h < hm->hole_count; ++h)
        if (hm->holes[h].expr == e)
          return hm->holes[h].type;
      ny_hm_type_t *hole = hm_var(hm);
      if (hm && hm->hole_count < sizeof(hm->holes) / sizeof(hm->holes[0])) {
        hm->holes[hm->hole_count].expr = e;
        hm->holes[hm->hole_count].type = hole;
        ++hm->hole_count;
      }
      return hole;
    }
    ny_hm_type_t *t =
        hm_env_get_ctx(env, e->as.ident.name, e->as.ident.syntax_ctx);
    if (t)
      return t;
    const char *global_type = tp_lookup_global_type(
        hm && hm->ctx ? hm->ctx->cg : NULL, e->as.ident.name);
    if (!global_type && hm && hm->ctx && hm->ctx->cg && self_name &&
        *self_name && ny_module_path_exists(hm->ctx->cg, self_name)) {
      char *qualified = hm_full_name(self_name, e->as.ident.name);
      if (qualified) {
        global_type = tp_lookup_global_type(hm->ctx->cg, qualified);
        free(qualified);
      }
    }
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
    char *elem_repr = NULL;
    bool saw = false;
    bool heterogeneous = false;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      ny_hm_type_t *it =
          hm_infer_expr(hm, env, e->as.list_like.data[i], self_name);
      if (!saw) {
        elem = it;
        saw = true;
        elem_repr = hm_type_string(it);
      } else if (elem_repr) {
        char *it_repr = hm_type_string(it);
        if (!it_repr || strcmp(elem_repr, it_repr) != 0)
          heterogeneous = true;
        free(it_repr);
      }
    }
    free(elem_repr);
    /*
     * A literal such as `[["ch", c], pos]` is a tuple in practice: its
     * elements do not share one type.  Merging them would pick or union a
     * single element type, so `a[0]` could no longer be indexed or passed to
     * a scalar parameter.  Heterogeneous literals have no precise element
     * type here, so use the dynamic element type.
     */
    if (!saw || heterogeneous)
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
  case NY_E_CALL: {
    ny_hm_type_t *checked_fin = hm_try_checked_fin_call(hm, env, e, self_name);
    if (checked_fin)
      return checked_fin;
  }
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_MEMBER &&
        e->as.call.callee->as.member.target &&
        e->as.call.callee->as.member.name) {
      expr_t *callee = e->as.call.callee;
      char module_path[1024];
      if (hm_resolve_module_expr_path(hm, env, callee->as.member.target,
                                      module_path, sizeof(module_path))) {
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(
                hm->ctx->cg, module_path, callee->as.member.name, resolved_fun,
                sizeof(resolved_fun))) {
          ny_hm_type_t *result = hm_call_named(
              hm, env, resolved_fun, NULL, &e->as.call.args, e->tok, self_name);
          hm_persist_named_call_target(hm, e, resolved_fun, self_name);
          return result;
        }
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
    expr_t *named_callee = e->as.call.callee;
    while (named_callee && named_callee->kind == NY_E_SPLICE)
      named_callee = named_callee->as.splice.expr;
    if (named_callee && named_callee->kind == NY_E_IDENT) {
      const char *name = named_callee->as.ident.name;
      const char *leaf = ny_name_leaf(name);
      if (leaf && strncmp(leaf, "__flt_", 6) == 0) {
        ny_hm_type_t *intrinsic =
            hm_builtin_call_type(hm, name, &e->as.call.args);
        if (intrinsic) {
          for (size_t i = 0; i < e->as.call.args.len; ++i)
            (void)hm_infer_expr(hm, env, e->as.call.args.data[i].val,
                                self_name);
          return intrinsic;
        }
      }
      /*
       * Module bodies share the root HM scheme index. If its bare-name entry
       * is a user declaration, resolve the stdlib module's own declaration
       * or imported overload instead of applying that unrelated signature.
       * Keep the common path unchanged: import resolution here is needed only
       * for an actual cross-module name collision.
       */
      if (ny_is_stdlib_tok(e->tok) && self_name && *self_name && name &&
          !strchr(name, '.')) {
        ny_hm_scheme_t *shared = hm_find_scheme(hm, name);
        if (shared && shared->stmt && shared->stmt->kind == NY_S_FUNC &&
            !ny_is_stdlib_tok(shared->stmt->tok)) {
          char *qualified = hm_full_name(self_name, name);
          ny_hm_scheme_t *owned =
              qualified ? hm_find_scheme(hm, qualified) : NULL;
          if (owned) {
            ny_hm_type_t *result = hm_call_named(
                hm, env, qualified, NULL, &e->as.call.args, e->tok, self_name);
            hm_persist_named_call_target(hm, e, qualified, self_name);
            free(qualified);
            return result;
          }
          free(qualified);
          fun_sig *imported = hm && hm->ctx && hm->ctx->cg
                                  ? lookup_use_module_fun(hm->ctx->cg, name,
                                                          e->as.call.args.len)
                                  : NULL;
          if (imported && imported->name &&
              hm_find_scheme(hm, imported->name)) {
            ny_hm_type_t *result =
                hm_call_named(hm, env, imported->name, NULL, &e->as.call.args,
                              e->tok, self_name);
            hm_persist_named_call_target(hm, e, imported->name, self_name);
            return result;
          }
        }
      }
      ny_hm_type_t *local =
          hm_env_get_ctx(env, name, named_callee->as.ident.syntax_ctx);
      /*
       * Module-level VALUES from every compiled module share this
       * environment, so a bare call whose name collides with a module-level
       * dict/list (core.math.vector's sample vectors x/y/z) must not take
       * the value path when a function of that name exists.  Only
       * actually-callable locals dispatch as "local call" directly; for
       * non-callable locals a same-leaf function scheme wins first.
       */
      ny_hm_type_t *pruned = local ? hm_prune(local) : NULL;
      bool callable_local =
          pruned && (pruned->kind == NY_HM_FN ||
                     (pruned->kind == NY_HM_NAME && pruned->name &&
                      hm_is_callable_name(pruned->name)));
      /*
       * An `any` local is still the lexically selected callee.  Do not let an
       * unrelated module's same-leaf function scheme replace it; bundled
       * stdlib modules and the user entry share the scheme index.
       */
      if (local && (callable_local || hm_is_any(pruned)))
        return hm_call_function_type(hm, env, local, &e->as.call.args, e->tok,
                                     self_name, "local call");
      /*
       * A module's own declaration shadows every imported function with
       * the same leaf name.  The compiler's function table is global, so
       * asking it first let `save` inside core...tga bind to the unrelated
       * `save(str, ...)` of whichever module registered that leaf last.
       */
      if (self_name && *self_name && name && !strchr(name, '.')) {
        char *own = hm_full_name(self_name, name);
        ny_hm_scheme_t *own_scheme = own ? hm_find_scheme(hm, own) : NULL;
        if (own_scheme && !hm_scheme_is_method(own_scheme)) {
          const char *own_key = ny_intern_get(ny_intern_cstr(own));
          free(own);
          ny_hm_type_t *result = hm_call_named(
              hm, env, own_key, NULL, &e->as.call.args, e->tok, self_name);
          hm_persist_named_call_target(hm, e, own_key, self_name);
          return result;
        }
        free(own);
      }
      if (local) {
        /*
         * Bare names imported into a module are resolved by the compiler's
         * function table to their qualified declaration (for example,
         * `all` -> `core.iter.all`).  HM's hash index intentionally
         * retains only the qualified key when imported schemes collide, so
         * consult that authoritative resolution before letting an unrelated
         * module-global value of the same leaf name become the callee.
         */
        fun_sig *resolved_sig = hm && hm->ctx && hm->ctx->cg
                                    ? lookup_fun(hm->ctx->cg, name, 0)
                                    : NULL;
        if (resolved_sig && resolved_sig->name &&
            hm_find_scheme(hm, resolved_sig->name)) {
          ny_hm_type_t *result =
              hm_call_named(hm, env, resolved_sig->name, NULL, &e->as.call.args,
                            e->tok, self_name);
          hm_persist_named_call_target(hm, e, resolved_sig->name, self_name);
          return result;
        }
        const char *local_leaf = ny_name_leaf(name);
        bool named_fn = hm_find_scheme(hm, name) != NULL ||
                        (local_leaf && self_name && *self_name &&
                         strcmp(local_leaf, name) == 0);
        if (!named_fn && local_leaf && self_name && *self_name &&
            strcmp(local_leaf, name) == 0) {
          char owned[512];
          int qn =
              snprintf(owned, sizeof(owned), "%s.%s", self_name, local_leaf);
          if (qn > 0 && (size_t)qn < sizeof(owned))
            named_fn = hm_find_scheme(hm, owned) != NULL;
        }
        if (!named_fn)
          return hm_call_function_type(hm, env, local, &e->as.call.args, e->tok,
                                       self_name, "local call");
      }
      ny_hm_type_t *result = hm_call_named(hm, env, name, NULL,
                                           &e->as.call.args, e->tok, self_name);
      hm_persist_named_call_target(hm, e, name, self_name);
      return result;
    } else if (e->as.call.callee) {
      ny_hm_type_t *callee_t =
          hm_infer_expr(hm, env, e->as.call.callee, self_name);
      return hm_call_function_type(hm, env, callee_t, &e->as.call.args, e->tok,
                                   self_name, "call expression");
    }
    return hm_any_fallback(hm, e, "hm-call-missing-callee",
                           "call has no callee");
  case NY_E_MEMCALL: {
    if (hm && hm->ctx && hm->ctx->cg && e->as.memcall.name) {
      char *constructor = hm_pattern_call_name(hm, e);
      enum_def_t *owner = NULL;
      enum_member_def_t *member =
          constructor
              ? lookup_enum_member_owner(hm->ctx->cg, constructor, &owner)
              : NULL;
      if (constructor && self_name && *self_name) {
        char *qualified = hm_full_name(self_name, constructor);
        enum_def_t *local_owner = NULL;
        enum_member_def_t *local =
            qualified
                ? lookup_enum_member_owner(hm->ctx->cg, qualified, &local_owner)
                : NULL;
        free(qualified);
        if (local) {
          member = local;
          owner = local_owner;
        }
      }
      if (member && owner) {
        ny_hm_type_t *out = hm_adt_constructor_type(
            hm, env, owner, member, &e->as.memcall.args, self_name);
        free(constructor);
        return out;
      }
      free(constructor);
    }
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
        hm_module_unknown_member_diag(hm, e->tok, module_path,
                                      e->as.memcall.name);
        return hm_any(hm);
      }
    }
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name && e->as.memcall.name) {
      const char *target_name = e->as.memcall.target->as.ident.name;
      ny_hm_type_t *root_type = hm_env_get_ctx(
          env, target_name, e->as.memcall.target->as.ident.syntax_ctx);
      if (!root_type && hm && hm->ctx && hm->ctx->cg) {
        const char *global_type =
            tp_lookup_global_type(hm->ctx->cg, target_name);
        if (global_type)
          root_type = hm_parse_type_name(hm, global_type, self_name);
      }
      if (!root_type || (hm_is_any(root_type) &&
                         hm_declares_inline_module(hm, target_name))) {
        const char *module_name =
            hm && hm->ctx && hm->ctx->cg
                ? ny_lookup_module_alias(hm->ctx->cg, NULL, 0, target_name,
                                         strlen(target_name), 0)
                : NULL;
        if ((!module_name || !*module_name) && hm && hm->ctx && hm->ctx->cg)
          module_name = resolve_import_alias(hm->ctx->cg, target_name);
        if (!module_name || !*module_name)
          module_name = target_name;
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(hm->ctx->cg, module_name,
                                            e->as.memcall.name, resolved_fun,
                                            sizeof(resolved_fun))) {
          return hm_call_named(hm, env, resolved_fun, NULL, &e->as.memcall.args,
                               e->tok, self_name);
        }
        if (ny_module_path_exists(hm->ctx->cg, module_name)) {
          hm_module_unknown_member_diag(hm, e->tok, module_name,
                                        e->as.memcall.name);
          return hm_any(hm);
        }
      }
      /*
       * A known local identifier is a receiver, not a module name.  The
       * module-resolution probes above are opportunistic; only unresolved or
       * dynamically typed roots should fall back to `any`.  Returning here
       * for a concrete root erased `SelfBox.add -> SelfBox` and let the broad
       * `any.add` contract win for immediate chains such as `a.add(b).value`.
       */
      if (!root_type || hm_is_any(root_type)) {
        for (size_t i = 0; i < e->as.memcall.args.len; ++i)
          (void)hm_infer_expr_dynamic_literal_ok(
              hm, env, e->as.memcall.args.data[i].val, self_name, true);
        return hm_any(hm);
      }
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
    const char *method = e->as.memcall.name ? e->as.memcall.name : "";
    const char *lookup_owner = owner ? owner : "any";
    fun_sig *sig = hm->ctx && hm->ctx->cg
                       ? ny_gencall_lookup_attached_method(hm->ctx->cg,
                                                           lookup_owner, method)
                       : NULL;
    if (!sig && strcmp(lookup_owner, "any") != 0 && hm->ctx && hm->ctx->cg)
      sig = ny_gencall_lookup_attached_method(hm->ctx->cg, "any", method);
    char full[512];
    const char *callee = sig ? hm_sig_scheme_name(hm, sig) : NULL;
    if (!callee) {
      snprintf(full, sizeof(full), "%s.%s", lookup_owner, method);
      callee = full;
    }
    ny_hm_type_t *ret = hm_call_named(hm, env, callee, e->as.memcall.target,
                                      &e->as.memcall.args, e->tok, self_name);
    /*
     * The public core.list.append wrapper is intentionally dynamically typed
     * at its API boundary, but a known list receiver still carries its
     * element type through this functional update. Keeping the wrapper's
     * `any` result here turns `list<fn(...)>.append(fn(...))` into
     * `list<any>`, which loses the callable signature before indexing. If
     * either side is dynamic, retain that uncertainty instead of claiming
     * that all elements have the receiver's former type.
     */
    bool builtin_list_append =
        target_t && target_t->kind == NY_HM_LIST &&
        strcmp(method, "append") == 0 &&
        (!sig || !sig->stmt_t || ny_is_stdlib_tok(sig->stmt_t->tok));
    if (builtin_list_append && e->as.memcall.args.len == 1) {
      ny_hm_type_t *item = hm_prune(hm_infer_expr(
          hm, env, e->as.memcall.args.data[0].val, self_name));
      ny_hm_type_t *elem = hm_prune(target_t->a);
      if (!elem || hm_is_any(elem) || hm_type_is_dynamic(item)) {
        elem = hm_any(hm);
      } else if (elem->kind == NY_HM_VAR) {
        elem = item ? item : hm_any(hm);
      } else if (item) {
        elem = hm_merge(hm, elem, item);
      }
      ret = hm_unary(hm, NY_HM_LIST, elem ? elem : hm_any(hm));
    }
    free(owner);
    free(target_s);
    return ret;
  }
  case NY_E_MEMBER: {
    const char *enum_owner = tp_enum_member_owner_name(hm ? hm->ctx : NULL, e);
    if (enum_owner && hm && hm->ctx && hm->ctx->cg && e->as.member.target &&
        e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name && e->as.member.name) {
      /* A nullary constructor with value evidence (`Vec.Nil`) carries its
       * index: it is a `Vec<A, 0>`, not a bare `Vec`. */
      char qualified[512];
      snprintf(qualified, sizeof(qualified), "%s.%s",
               e->as.member.target->as.ident.name, e->as.member.name);
      enum_def_t *owner = NULL;
      enum_member_def_t *member =
          lookup_enum_member_owner(hm->ctx->cg, qualified, &owner);
      if (member && owner && !member->has_payload &&
          owner->type_params.len > 0 && hm_index_member_uses_values(member))
        return hm_adt_constructor_type(hm, env, owner, member, NULL,
                                       self_name);
    }
    if (enum_owner)
      return hm_parse_type_name(hm, enum_owner, self_name);
  }
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0)
      return hm_name(hm, "int");
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return hm_name(hm, "bigint");
    if (e->as.member.target && e->as.member.name) {
      if (e->as.member.target->kind == NY_E_IDENT &&
          e->as.member.target->as.ident.name &&
          !hm_env_get_ctx(env, e->as.member.target->as.ident.name,
                          e->as.member.target->as.ident.syntax_ctx)) {
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
      if (hm_resolve_module_expr_path(hm, env, e->as.member.target, module_path,
                                      sizeof(module_path))) {
        char sub_module[1024];
        if (hm_resolve_module_expr_path(hm, env, e, sub_module,
                                        sizeof(sub_module)))
          return hm_any(hm);
        if (ny_module_exported_module_path(hm && hm->ctx ? hm->ctx->cg : NULL,
                                           module_path, e->as.member.name,
                                           sub_module, sizeof(sub_module)))
          return hm_any(hm);
        /*
         * Constants and exported data members do not have function paths, but
         * they are still valid module members.  Keep their dynamic value
         * type instead of reporting them as missing exports.
         */
        if (e->as.member.target->kind == NY_E_IDENT &&
            e->as.member.target->as.ident.name &&
            (tp_module_alias_member_exists(hm && hm->ctx ? hm->ctx->cg : NULL,
                                           e->as.member.target->as.ident.name,
                                           e->as.member.name) ||
             ny_module_declares_exported_member(
                 hm && hm->ctx ? hm->ctx->cg : NULL, module_path,
                 e->as.member.name)))
          return hm_any(hm);
        hm_module_unknown_member_diag(hm, e->tok, module_path,
                                      e->as.member.name);
        return hm_any(hm);
      }
      if (hm_member_root_is_unresolved_ident(hm, env, e->as.member.target))
        return hm_any(hm);
      if (e->as.member.target->kind == NY_E_IDENT &&
          e->as.member.target->as.ident.name &&
          !hm_env_get_ctx(env, e->as.member.target->as.ident.name,
                          e->as.member.target->as.ident.syntax_ctx) &&
          (tp_module_alias_member_exists(hm && hm->ctx ? hm->ctx->cg : NULL,
                                         e->as.member.target->as.ident.name,
                                         e->as.member.name) ||
           (hm_resolve_module_expr_path(hm, env, e->as.member.target,
                                        module_path, sizeof(module_path)) &&
            ny_module_declares_exported_member(
                hm && hm->ctx ? hm->ctx->cg : NULL, module_path,
                e->as.member.name))))
        return hm_any(hm);
      ny_hm_type_t *target_t =
          hm_infer_expr(hm, env, e->as.member.target, self_name);
      if (hm_type_is_unbound_var(target_t)) {
        tp_add_fallback(hm ? hm->ctx : NULL, e->tok, "hm", "hm-dynamic-member",
                        "member", "member '%s' read from unconstrained value",
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
        if (getenv("NY_TRACE_MEMBER_GET") &&
            strcmp(e->as.member.name, "get") == 0) {
          fprintf(stderr, "[hm-member] receiver=%s owner=%s scheme=%s sig=%s\n",
                  target_s ? target_s : "<null>", owner,
                  scheme && scheme->full_name ? scheme->full_name : "<none>",
                  sig && sig->name ? sig->name : "<none>");
        }
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
          return hm_name(hm, "bigint");
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
      return hm_name(hm, "bigint");
    return hm_any_fallback(hm, e, "hm-dynamic-member",
                           "member target is dynamic");
  case NY_E_INDEX: {
    /*
     * Projection from a dependent pair.  Component 0 has the index type;
     * component 1 has the payload type with the binder replaced by the
     * pair's literal index when one is known.
     */
    if (e->as.index.target && e->as.index.target->kind == NY_E_IDENT &&
        !e->as.index.stop && !e->as.index.step && e->as.index.start &&
        e->as.index.start->kind == NY_E_LITERAL &&
        e->as.index.start->as.literal.kind == NY_LIT_INT &&
        !e->as.index.start->as.literal.integer_outside_i64) {
      const expr_t *pair_ref = e->as.index.target;
      ny_hm_env_entry_t *entry = hm_env_entry_ctx(
          env, pair_ref->as.ident.name, pair_ref->as.ident.syntax_ctx);
      if (entry && entry->sigma_decl) {
        hm_sigma_parts_t parts;
        if (hm_sigma_parts_parse(entry->sigma_decl, &parts)) {
          int64_t at = e->as.index.start->as.literal.as.i;
          if (at == 0) {
            ny_hm_type_t *t =
                hm_parse_type_name(hm, parts.index_type, self_name);
            hm_sigma_parts_free(&parts);
            return t;
          }
          if (at == 1) {
            char *raw = NULL;
            if (entry->sigma_index_literal)
              raw = hm_replace_type_index_owned(parts.value_type, parts.binder,
                                                entry->sigma_index_value);
            if (!raw)
              raw = ny_strdup(parts.value_type);
            ny_hm_type_t *t = hm_parse_type_name(hm, raw, self_name);
            free(raw);
            hm_sigma_parts_free(&parts);
            return t;
          }
          hm_sigma_parts_free(&parts);
        }
      }
    }
    ny_hm_type_t *target =
        hm_prune(hm_infer_expr(hm, env, e->as.index.target, self_name));
    /*
     * Refinement names inherit their base aggregate's indexing interface.
     */
    if (target && target->kind == NY_HM_NAME && target->name) {
      const char *base = hm_resolve_refine_root_base(hm, target->name);
      if (base && ny_generic_type_base_is(base, "list")) {
        target = hm_prune(hm_parse_type_name(hm, base, self_name));
        /*
         * A bare `list` refinement leaves its element type implicit. Give
         * each indexed expression an inference variable so its surrounding
         * operators can establish the scalar type. Keeping `any` here makes
         * even `items[1] + 5` fail strict typing inside otherwise typed
         * callers and refinement predicates.
         */
        if (target && target->kind == NY_HM_LIST && target->a &&
            target->a->kind == NY_HM_ANY)
          target->a = hm_var(hm);
      }
    }
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
      index_t = hm_prune(index_t);
      if (index_t &&
          (hm_is_name(index_t, "int") || hm_type_is_unbound_var(index_t))) {
        hm_unify(hm, target, hm_unary(hm, NY_HM_INDEXABLE, elem), e->tok,
                 "index target");
      } else {
        hm_unify(
            hm, target,
            hm_binary(hm, NY_HM_DICT, index_t ? index_t : hm_any(hm), elem),
            e->tok, "index target");
      }
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
        hm_is_name(target, "range") || hm_is_name(target, "seq") ||
        hm_is_name(target, "sequence");
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
    if (!e->as.szof.is_type && e->as.szof.target) {
      ny_hm_type_t *target_type =
          hm_infer_expr(hm, env, e->as.szof.target, self_name);
      char *target_name = hm_type_string(target_type);
      e->as.szof.resolved_type_name =
          target_name ? ny_intern_get(ny_intern_cstr(target_name)) : NULL;
      free(target_name);
    }
    return hm_name(hm, "int");
  case NY_E_LAMBDA:
  case NY_E_FN:
    return hm_infer_lambda(hm, env, e, self_name);
  case NY_E_COMPTIME:
    return hm_infer_comptime_expr(hm, env, e, self_name);
  case NY_E_QUOTE:
    return hm_name(hm, "ast");
  case NY_E_SPLICE:
    return e->as.splice.expr
               ? hm_infer_expr(hm, env, e->as.splice.expr, self_name)
               : hm_name(hm, "any");
  case NY_E_EXISTS: {
    ny_hm_env_list local;
    hm_env_clone(hm, &local, env);
    if (e->as.exists.var)
      hm_env_set_ctx(&local, e->as.exists.var, e->as.exists.syntax_ctx,
                     hm_name(hm, "int"));
    (void)hm_infer_expr(hm, &local, e->as.exists.body, self_name);
    vec_free(&local);
    return hm_name(hm, "bool");
  }
  case NY_E_MATCH: {
    /*
     * Match expressions share the statement representation. Infer each arm
     * as an implicit return and merge it into one result variable instead of
     * degrading the whole expression to `any`.
     */
    stmt_t match_stmt = {.kind = NY_S_MATCH, .tok = e->tok};
    match_stmt.as.match = e->as.match;
    ny_hm_type_t *result = hm_var(hm);
    if (e->as.match.motive_type) {
      /*
       * Explicit motive: every arm must produce the motive type.  A binder
       * names the scrutinee; inside a proof motive it is instantiated with
       * the scrutinee's canonical term.
       */
      const char *motive = e->as.match.motive_type;
      char *inst = NULL;
      if (e->as.match.motive_binder && strncmp(motive, "proof<", 6) == 0 &&
          e->as.match.test) {
        param_t binder = {.name = e->as.match.motive_binder};
        stmt_t fake = {.kind = NY_S_FUNC};
        fake.as.fn.params.data = &binder;
        fake.as.fn.params.len = 1;
        call_arg_t arg = {.val = e->as.match.test};
        ny_call_arg_list args = {.data = &arg, .len = 1, .cap = 1};
        inst = hm_proof_subst_args(hm, &fake, motive, &args, 0, 1);
      }
      result = hm_parse_type_name(hm, inst ? inst : motive, self_name);
      free(inst);
      /*
       * Dependent elimination: an arm whose pattern is a literal or a
       * nullary constructor is checked against the motive at that pattern
       * (`0 -> prove(0 == 0)` under `as (v) -> proof<v == v>`), since the
       * scrutinee is that value on the arm. Every other arm, and the
       * match as a whole, has the motive at the scrutinee.
       */
      if (e->as.match.motive_binder && strncmp(motive, "proof<", 6) == 0) {
        ny_hm_type_t *subject_t =
            hm_infer_expr(hm, env, e->as.match.test, self_name);
        hm_validate_match_patterns(hm, &match_stmt, subject_t);
        param_t binder = {.name = e->as.match.motive_binder};
        stmt_t fake = {.kind = NY_S_FUNC};
        fake.as.fn.params.data = &binder;
        fake.as.fn.params.len = 1;
        for (size_t i = 0; i < e->as.match.arms.len; ++i) {
          match_arm_t *arm = &e->as.match.arms.data[i];
          const expr_t *pattern =
              arm->patterns.len == 1 ? arm->patterns.data[0] : NULL;
          bool literal = pattern && !arm->guard &&
                         ((pattern->kind == NY_E_LITERAL &&
                           (pattern->as.literal.kind == NY_LIT_INT ||
                            pattern->as.literal.kind == NY_LIT_BOOL) &&
                           pattern->tok.kind != NY_T_NIL) ||
                          (pattern->kind == NY_E_MEMBER &&
                           pattern->as.member.target &&
                           pattern->as.member.target->kind == NY_E_IDENT));
          ny_hm_type_t *arm_ret = result;
          if (literal) {
            call_arg_t arg = {.val = (expr_t *)pattern};
            ny_call_arg_list args = {.data = &arg, .len = 1, .cap = 1};
            char *arm_inst =
                hm_proof_subst_args(hm, &fake, motive, &args, 0, 1);
            if (arm_inst) {
              arm_ret = hm_parse_type_name(hm, arm_inst, self_name);
              free(arm_inst);
            }
          }
          ny_hm_env_list arm_env;
          hm_env_clone(hm, &arm_env, env);
          if (arm->patterns.len == 1)
            hm_refine_match_pattern(hm, &arm_env, subject_t,
                                    arm->patterns.data[0], self_name);
          (void)hm_infer_expr(hm, &arm_env, arm->guard, self_name);
          if (arm_ret == result) {
            hm_infer_stmt_mode(hm, &arm_env, arm->conseq, self_name, arm_ret,
                               true);
          } else {
            /* On this arm the scrutinee is the pattern, so evidence for the
             * motive at the scrutinee is evidence at the pattern too. */
            ny_hm_type_t *got = hm_var(hm);
            hm_infer_stmt_mode(hm, &arm_env, arm->conseq, self_name, got,
                               true);
            got = hm_prune(got);
            char *got_s = got ? hm_type_string(got) : NULL;
            char *at_scrutinee = hm_type_string(result);
            bool same = got_s && at_scrutinee &&
                        strcmp(got_s, at_scrutinee) == 0;
            free(got_s);
            free(at_scrutinee);
            if (!same && got &&
                !(got->kind == NY_HM_VAR && !got->instance))
              (void)hm_unify(hm, arm_ret, got,
                             arm->conseq ? arm->conseq->tok : e->tok,
                             "implicit return value");
          }
          vec_free(&arm_env);
        }
        hm_infer_stmt_mode(hm, env, e->as.match.default_conseq, self_name,
                           result, true);
        return hm_prune(result);
      }
    }
    hm_infer_stmt(hm, env, &match_stmt, self_name, result);
    result = hm_prune(result);
    if (!result || (result->kind == NY_HM_VAR && !result->instance))
      return hm_any(hm);
    return result;
  }
  case NY_E_TRY: {
    ny_hm_type_t *result_t =
        hm_infer_expr(hm, env, e->as.try_expr.target, self_name);
    ny_hm_type_t *payload_t = hm_result_payload_type(hm, result_t, true);
    hm_strict_result_payload_check(hm, e->tok, result_t, payload_t,
                                   "try propagation");
    return payload_t;
  }
  case NY_E_ASM:
    /*
     * Inline assembly is a typed expression, not an opaque `any` value.
     * Structured and compact spellings are normalized to the same node, so
     * infer the result from its output constraint.  Integer/register outputs
     * use the machine-word `int` ABI; floating-register outputs use f64.
     */
    if (e->kind == NY_E_ASM && e->as.as_asm.constraints) {
      const char *cons = e->as.as_asm.constraints;
      const char *end = strchr(cons, ',');
      if (!end)
        end = cons + strlen(cons);
      for (const char *p = cons; p < end; ++p)
        if (*p == 'f')
          return hm_name(hm, "f64");
    }
    if (e->kind == NY_E_ASM)
      return hm_name(hm, "int");
  case NY_E_EMBED:
    return hm_name(hm, "str");
  case NY_E_INFERRED_MEMBER:
  default:
    return hm_any_fallback(hm, e, "hm-unsupported-expr",
                           "unsupported expression form");
  }
}

static ny_sem_rep_t hm_semantic_rep(const char *type_name) {
  return ny_semantic_rep_for_type(type_name);
}

static bool hm_stmt_declares_nominal_type(const stmt_t *stmt,
                                          const char *type_name,
                                          const char *module_name) {
  if (!stmt || !type_name)
    return false;
  if (stmt->kind == NY_S_MODULE) {
    const char *child_module =
        stmt->as.module.name ? hm_full_name(module_name, stmt->as.module.name)
                             : module_name;
    for (size_t i = 0; i < stmt->as.module.body.len; ++i)
      if (hm_stmt_declares_nominal_type(stmt->as.module.body.data[i], type_name,
                                        child_module))
        return true;
    return false;
  }
  const char *decl_name = NULL;
  switch (stmt->kind) {
  case NY_S_STRUCT:
    decl_name = stmt->as.struc.name;
    break;
  case NY_S_LAYOUT:
    decl_name = stmt->as.layout.name;
    break;
  case NY_S_ENUM:
    decl_name = stmt->as.enu.name;
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (hm_stmt_declares_nominal_type(stmt->as.block.body.data[i], type_name,
                                        module_name))
        return true;
    return false;
  default:
    return false;
  }
  if (!decl_name)
    return false;
  char *qualified = hm_full_name(module_name, decl_name);
  bool found = qualified && strcmp(qualified, type_name) == 0;
  free(qualified);
  return found;
}

/*
 * `impl Name` can introduce a nominal method receiver without declaring a
 * struct/layout. Those values use the runtime object handle ABI. Preserve
 * aggregate representation for explicit aggregate declarations and scalar
 * impls; use the compiler's function-owner records as the evidence that this
 * name is an impl receiver rather than guessing from its spelling.
 */
static ny_sem_rep_t hm_semantic_rep_in_context(ny_hm_state_t *hm,
                                               const char *type_name) {
  ny_sem_rep_t rep = hm_semantic_rep(type_name);
  if (rep != NY_SEM_REP_AGGREGATE || !hm || !hm->ctx || !hm->ctx->prog ||
      !type_name)
    return rep;
  const char *owner = type_name;
  while (*owner == '?')
    ++owner;
  bool has_impl_owner = false;
  for (size_t i = 0; i < hm->ctx->funcs.len; ++i) {
    const ny_tp_func_fact_t *fact = &hm->ctx->funcs.data[i];
    if (fact->owner && strcmp(fact->owner, owner) == 0) {
      has_impl_owner = true;
      break;
    }
  }
  if (!has_impl_owner)
    return rep;
  for (size_t i = 0; i < hm->ctx->prog->body.len; ++i)
    if (hm_stmt_declares_nominal_type(hm->ctx->prog->body.data[i], owner, NULL))
      return rep;
  return NY_SEM_REP_OBJECT;
}

/*
 * Preserve module identity on locally declared nominal return types. Without
 * this, `make() *Token` imported from two modules collapses to the same leaf
 * and attached-method resolution can bind both values to the first module.
 */
static char *hm_qualify_signature_type(ny_hm_state_t *hm, const fun_sig *sig,
                                       const char *type_name) {
  return hm_qualify_owner_type(hm, sig ? sig->module_name : NULL, type_name);
}

static char *hm_qualify_owner_type(ny_hm_state_t *hm, const char *owner,
                                   const char *type_name) {
  if (!hm || !hm->ctx || !hm->ctx->cg || !owner || !*owner || !type_name ||
      !*type_name)
    return NULL;
  const char *base = type_name;
  while (*base == '?' || *base == '*')
    base++;
  const char *end = base;
  while (*end && *end != '<' && *end != '[' && *end != '|' && *end != ' ')
    end++;
  if (end == base || memchr(base, '.', (size_t)(end - base)))
    return NULL;
  char qualified[1024];
  int n = snprintf(qualified, sizeof(qualified), "%s.%.*s", owner,
                   (int)(end - base), base);
  if (n <= 0 || (size_t)n >= sizeof(qualified) ||
      !lookup_layout(hm->ctx->cg, qualified))
    return NULL;
  /*
   * Keep the short spelling when it is unambiguous. Qualifying every nominal
   * type changes unrelated module inference; module identity is required when
   * equal leaf names actually collide.
   */
  size_t same_leaf = 0;
  for (size_t i = 0; i < hm->ctx->cg->layouts.len; ++i) {
    layout_def_t *layout = hm->ctx->cg->layouts.data[i];
    if (!layout || !layout->name)
      continue;
    const char *candidate_leaf = strrchr(layout->name, '.');
    candidate_leaf = candidate_leaf ? candidate_leaf + 1 : layout->name;
    const char *candidate_end = strchr(candidate_leaf, '<');
    size_t candidate_len = candidate_end
                               ? (size_t)(candidate_end - candidate_leaf)
                               : strlen(candidate_leaf);
    if (candidate_len == (size_t)(end - base) &&
        strncmp(candidate_leaf, base, candidate_len) == 0)
      same_leaf++;
  }
  if (same_leaf < 2)
    return NULL;
  size_t prefix_len = (size_t)(base - type_name);
  size_t qualified_len = (size_t)n;
  size_t suffix_len = strlen(end);
  if (prefix_len > SIZE_MAX - qualified_len - suffix_len - 1)
    return NULL;
  char *result = malloc(prefix_len + qualified_len + suffix_len + 1);
  if (!result)
    return NULL;
  memcpy(result, type_name, prefix_len);
  memcpy(result + prefix_len, qualified, qualified_len);
  memcpy(result + prefix_len + qualified_len, end, suffix_len + 1);
  return result;
}

/*
 * Owned schemes chained by method leaf.  Member resolution scanned every
 * scheme for every call; the chain keeps the same ascending visit order.
 */
static _Thread_local struct {
  const ny_hm_state_t *hm;
  size_t indexed_len;
  int32_t *heads;
  int32_t *next;
  size_t mask;
} hm_method_index;

static size_t hm_method_hash(const char *s) {
  size_t h = 2166136261u;
  for (; *s; ++s)
    h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}

static int32_t hm_method_first(ny_hm_state_t *hm, const char *method) {
  if (hm_method_index.hm != hm ||
      hm_method_index.indexed_len != hm->schemes.len) {
    size_t buckets = 64;
    while (buckets < hm->schemes.len * 2)
      buckets <<= 1;
    free(hm_method_index.heads);
    free(hm_method_index.next);
    hm_method_index.heads = malloc(buckets * sizeof(int32_t));
    hm_method_index.next = malloc((hm->schemes.len + 1) * sizeof(int32_t));
    hm_method_index.hm = NULL;
    if (!hm_method_index.heads || !hm_method_index.next)
      return -2;
    memset(hm_method_index.heads, 0xff, buckets * sizeof(int32_t));
    hm_method_index.mask = buckets - 1;
    for (size_t i = hm->schemes.len; i > 0; --i) {
      ny_hm_scheme_t *candidate = &hm->schemes.data[i - 1];
      hm_method_index.next[i - 1] = -1;
      if (!candidate->name || !candidate->owner)
        continue;
      size_t bucket =
          hm_method_hash(ny_name_leaf(candidate->name)) & hm_method_index.mask;
      hm_method_index.next[i - 1] = hm_method_index.heads[bucket];
      hm_method_index.heads[bucket] = (int32_t)(i - 1);
    }
    hm_method_index.hm = hm;
    hm_method_index.indexed_len = hm->schemes.len;
  }
  return hm_method_index.heads[hm_method_hash(method) & hm_method_index.mask];
}

static void hm_persist_member_target(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     expr_t *e, const char *self_name) {
  expr_t *target = NULL;
  const char *method = NULL;
  if (e->kind == NY_E_MEMCALL) {
    target = e->as.memcall.target;
    method = e->as.memcall.name;
  } else if (e->kind == NY_E_MEMBER) {
    target = e->as.member.target;
    method = e->as.member.name;
  }
  if (!target || !method || !hm || !hm->ctx)
    return;
  /*
   * Later module-global completion visits can widen a locally inferred
   * receiver to any. Preserve its already resolved attached method.
   */
  if (e->semantic.resolved &&
      e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED &&
      e->semantic.canonical_callee && e->semantic.canonical_callee_stmt)
    return;
  ny_hm_type_t *target_t = hm_infer_expr(hm, env, target, self_name);
  char *target_name = hm_type_string(target_t);
  char *owner_name = hm_attached_owner_name(target_name);
  const char *lookup_type = owner_name && *owner_name ? owner_name : "any";
  fun_sig *sig =
      hm->ctx->cg
          ? ny_gencall_lookup_attached_method(hm->ctx->cg, lookup_type, method)
          : NULL;
  ny_hm_scheme_t *scheme = NULL;
  bool scheme_ambiguous = false;
  if (!sig) {
    int32_t first = hm_method_first(hm, method);
    for (size_t i = first >= 0 ? (size_t)first : 0;
         first == -2 ? i < hm->schemes.len : first >= 0;
         first == -2 ? (void)++i
                     : (void)((first = hm_method_index.next[i]) >= 0 &&
                              (i = (size_t)first))) {
      ny_hm_scheme_t *candidate = &hm->schemes.data[i];
      if (candidate->name && candidate->owner &&
          strcmp(ny_name_leaf(candidate->name), method) == 0 &&
          strcmp(ny_name_leaf(candidate->owner), ny_name_leaf(lookup_type)) ==
              0) {
        if (scheme && scheme->stmt != candidate->stmt) {
          scheme = NULL;
          scheme_ambiguous = true;
          break;
        }
        scheme = candidate;
      }
    }
  }
  bool any_contract = false;
  if (!sig && !scheme && !scheme_ambiguous && strcmp(lookup_type, "any") != 0 &&
      hm->ctx->cg) {
    sig = ny_gencall_lookup_attached_method(hm->ctx->cg, "any", method);
    any_contract = sig != NULL;
  }
  if (!sig && !scheme && !scheme_ambiguous && strcmp(lookup_type, "any") != 0) {
    bool any_scheme_ambiguous = false;
    int32_t first = hm_method_first(hm, method);
    for (size_t i = first >= 0 ? (size_t)first : 0;
         first == -2 ? i < hm->schemes.len : first >= 0;
         first == -2 ? (void)++i
                     : (void)((first = hm_method_index.next[i]) >= 0 &&
                              (i = (size_t)first))) {
      ny_hm_scheme_t *candidate = &hm->schemes.data[i];
      if (candidate->name && candidate->owner &&
          strcmp(ny_name_leaf(candidate->name), method) == 0 &&
          strcmp(ny_name_leaf(candidate->owner), "any") == 0) {
        if (scheme && scheme->stmt != candidate->stmt) {
          scheme = NULL;
          any_scheme_ambiguous = true;
          break;
        }
        scheme = candidate;
      }
    }
    any_contract = scheme != NULL && !any_scheme_ambiguous;
    if (any_scheme_ambiguous)
      scheme = NULL;
  } else if ((sig || scheme) && strcmp(lookup_type, "any") == 0) {
    any_contract = true;
  }
  if (sig || scheme) {
    if (ny_dbg_is_enabled(NY_DBG_TRAIT)) {
      char detail[768];
      const char *selected = sig ? sig->name : scheme->stmt->as.fn.name;
      snprintf(detail, sizeof(detail),
               "receiver=%s method=%s selected=%s via=%s", lookup_type, method,
               selected ? selected : "<unnamed>",
               any_contract ? "dynamic any contract"
               : scheme     ? "HM impl scheme"
                            : "attached method");
      ny_dbg_trait_detail(detail);
    }
    e->semantic.member_call_kind = any_contract ? NY_SEM_CALL_DYNAMIC_CONTRACT
                                                : NY_SEM_CALL_DIRECT_ATTACHED;
    e->semantic.canonical_callee = ny_intern_get(
        ny_intern_cstr(sig ? sig->name : scheme->stmt->as.fn.name));
    e->semantic.canonical_callee_stmt = sig ? sig->stmt_t : scheme->stmt;
    e->semantic.ownership =
        sig && sig->returns_owned
            ? NY_SEM_OWN_OWNED
            : (sig && sig->returns_borrow ? NY_SEM_OWN_BORROWED
                                          : NY_SEM_OWN_UNKNOWN);
  } else {
    e->semantic.member_call_kind = NY_SEM_CALL_ERROR;
    if (ny_dbg_is_enabled(NY_DBG_TRAIT)) {
      char detail[768];
      snprintf(detail, sizeof(detail), "receiver=%s method=%s unresolved%s",
               lookup_type, method,
               scheme_ambiguous ? " (ambiguous HM impl candidates)" : "");
      ny_dbg_trait_detail(detail);
    }
  }
  free(target_name);
  free(owner_name);
}

typedef struct hm_call_arg_fact_ctx_t {
  ny_hm_state_t *hm;
  ny_hm_env_list *env;
  const char *owner;
  /*
   * Module-global completion pass scope tracking.  Names bound by enclosing
   * params/locals are pushed here so a shadowed identifier is never resolved
   * against the global environment.  Unshadowed lowercase identifiers and
   * stdlib globals now resolve like uppercase constants: the completion
   * facts carry their representation instead of leaving later passes to
   * re-derive them from syntax.
   */
  const char **shadow;
  size_t shadow_len, shadow_cap;
  size_t *scope_marks;
  size_t scope_marks_len, scope_marks_cap;
} hm_call_arg_fact_ctx_t;

static void hm_completion_scope_push_name(hm_call_arg_fact_ctx_t *ctx,
                                          const char *name) {
  if (!ctx || !name || !*name)
    return;
  if (ctx->shadow_len == ctx->shadow_cap) {
    size_t nc = ctx->shadow_cap ? ctx->shadow_cap * 2 : 16;
    const char **grown = realloc(ctx->shadow, nc * sizeof(*grown));
    if (!grown)
      return;
    ctx->shadow = grown;
    ctx->shadow_cap = nc;
  }
  ctx->shadow[ctx->shadow_len++] = name;
}

static void hm_completion_scope_mark(hm_call_arg_fact_ctx_t *ctx) {
  if (!ctx)
    return;
  if (ctx->scope_marks_len == ctx->scope_marks_cap) {
    size_t nc = ctx->scope_marks_cap ? ctx->scope_marks_cap * 2 : 16;
    size_t *grown = realloc(ctx->scope_marks, nc * sizeof(*grown));
    if (!grown)
      return;
    ctx->scope_marks = grown;
    ctx->scope_marks_cap = nc;
  }
  ctx->scope_marks[ctx->scope_marks_len++] = ctx->shadow_len;
}

static void hm_completion_scope_pop(hm_call_arg_fact_ctx_t *ctx) {
  if (!ctx || ctx->scope_marks_len == 0)
    return;
  ctx->shadow_len = ctx->scope_marks[--ctx->scope_marks_len];
}

static bool hm_completion_shadowed(hm_call_arg_fact_ctx_t *ctx,
                                   const char *name) {
  if (!ctx || !name || !*name)
    return false;
  /*
   * A local shadows the first component of a qualified name.  A local
   * named `value` does not shadow the imported global `module.value`.
   */
  size_t name_len = strcspn(name, ".");
  for (size_t i = 0; i < ctx->shadow_len; ++i) {
    const char *bound = ctx->shadow[i];
    if (!bound)
      continue;
    const char *bleaf = strrchr(bound, '.');
    bleaf = bleaf ? bleaf + 1 : bound;
    if (strlen(bleaf) == name_len && strncmp(bleaf, name, name_len) == 0)
      return true;
  }
  return false;
}

/*
 * Scope tracking for the module-global completion pass.  Function params
 * are pushed on entry; `def`/`mut` names are pushed after their initializer.
 * The enclosing block (or the per-top-level-statement boundary) pops them,
 * so later sibling statements observe the binding without leaking it into
 * outer scopes.
 */
static void hm_completion_scope_stmt_pre(hm_call_arg_fact_ctx_t *ctx,
                                         stmt_t *s) {
  if (!ctx || !s)
    return;
  if (s->kind == NY_S_FUNC) {
    hm_completion_scope_mark(ctx);
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      hm_completion_scope_push_name(ctx, s->as.fn.params.data[i].name);
  } else if (s->kind == NY_S_BLOCK) {
    hm_completion_scope_mark(ctx);
  } else if (s->kind == NY_S_FOR) {
    hm_completion_scope_mark(ctx);
    hm_completion_scope_push_name(ctx, s->as.fr.iter_var);
    hm_completion_scope_push_name(ctx, s->as.fr.iter_index_var);
  } else if (s->kind == NY_S_GUARD) {
    hm_completion_scope_mark(ctx);
    hm_completion_scope_push_name(ctx, s->as.guard.name);
  }
}

static void hm_completion_scope_stmt_post(hm_call_arg_fact_ctx_t *ctx,
                                          stmt_t *s) {
  if (!ctx || !s)
    return;
  /*
   * Bind a declaration only after its initializer has been visited.  Its
   * name then remains visible to later statements until the enclosing
   * block/function mark is popped.
   */
  if (s->kind == NY_S_VAR && s->as.var.is_decl) {
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      hm_completion_scope_push_name(ctx, s->as.var.names.data[i]);
  } else if (s->kind == NY_S_FUNC || s->kind == NY_S_BLOCK ||
             s->kind == NY_S_FOR || s->kind == NY_S_GUARD) {
    hm_completion_scope_pop(ctx);
  }
}

static void hm_complete_leaf_fact(ny_visitor_t *visitor, expr_t *e);

static bool hm_completion_visitor_expr_pre(ny_visitor_t *visitor, expr_t *e) {
  hm_call_arg_fact_ctx_t *ctx = visitor ? visitor->ctx : NULL;
  if (ctx && e && e->kind == NY_E_EXISTS) {
    hm_completion_scope_mark(ctx);
    hm_completion_scope_push_name(ctx, e->as.exists.var);
  }
  return true;
}

static void hm_completion_visitor_expr_post(ny_visitor_t *visitor, expr_t *e) {
  hm_call_arg_fact_ctx_t *ctx = visitor ? visitor->ctx : NULL;
  if (ctx && e && e->kind == NY_E_EXISTS)
    hm_completion_scope_pop(ctx);
  hm_complete_leaf_fact(visitor, e);
}

static bool hm_completion_visitor_stmt_pre(ny_visitor_t *visitor, stmt_t *s) {
  hm_completion_scope_stmt_pre(visitor ? visitor->ctx : NULL, s);
  return true;
}

static void hm_completion_visitor_stmt_post(ny_visitor_t *visitor, stmt_t *s) {
  hm_completion_scope_stmt_post(visitor ? visitor->ctx : NULL, s);
}

static void hm_completion_scope_reset(hm_call_arg_fact_ctx_t *ctx) {
  if (!ctx)
    return;
  free(ctx->shadow);
  free(ctx->scope_marks);
  ctx->shadow = NULL;
  ctx->shadow_len = ctx->shadow_cap = 0;
  ctx->scope_marks = NULL;
  ctx->scope_marks_len = ctx->scope_marks_cap = 0;
}

static void hm_complete_leaf_fact(ny_visitor_t *visitor, expr_t *e) {
  hm_call_arg_fact_ctx_t *ctx = visitor ? visitor->ctx : NULL;
  if (!ctx || !e || e->semantic.resolved)
    return;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    if (!ctx->owner) {
      /*
       * In module-global completion pass, bare lowercase identifiers now
       * resolve against the global environment when a global of that name
       * is actually visible here.  Identifiers shadowed by an enclosing
       * parameter/local are skipped, so a local never inherits a global's
       * facts, and genuinely unknown locals stay unresolved exactly as
       * before.
       */
      if (hm_completion_shadowed(ctx, e->as.ident.name))
        return;
    }
    ny_hm_env_entry_t *entry =
        hm_env_entry_ctx(ctx->env, e->as.ident.name, e->as.ident.syntax_ctx);
    if (!entry)
      return;
    char *name = hm_type_string(entry->type);
    e->semantic.rep = hm_semantic_rep_in_context(ctx->hm, name);
    e->semantic.resolved_type_name =
        name ? ny_intern_get(ny_intern_cstr(name)) : NULL;
    e->semantic.ownership = entry->ownership;
    e->semantic.alias_class = entry->alias_class;
    e->semantic.mutable_value = entry->mutable_value;
    e->semantic.resolved = true;
    free(name);
    return;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    /*
     * `nil` is the canonical zero-valued language immediate. Its source
     * context may be a nullable scalar, pointer, or dynamic value, but the
     * producer is never an aggregate; typed boundaries decide whether that
     * immediate is consumed as a raw null or preserved as tagged `any`.
     */
    e->semantic.rep =
        e->tok.kind == NY_T_NIL
            ? NY_SEM_REP_TAGGED_DYNAMIC
            : hm_semantic_rep(tp_literal_type(&e->as.literal, e->tok));
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    e->semantic.rep = NY_SEM_REP_TYPED_BUFFER;
    break;
  case NY_E_DICT:
    e->semantic.rep = NY_SEM_REP_OBJECT;
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    e->semantic.rep = NY_SEM_REP_CLOSURE;
    break;
  default:
    return;
  }
  e->semantic.resolved = true;
}

static void hm_visit_semantic_completion(ny_visitor_t *visitor, stmt_t *s) {
  if (!s)
    return;
  if (s->kind == NY_S_MODULE) {
    hm_call_arg_fact_ctx_t *ctx = visitor ? visitor->ctx : NULL;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      /*
       * Module declarations are global environment entries, not lexical
       * locals that shadow later declarations in this module.
       */
      hm_completion_scope_mark(ctx);
      hm_visit_semantic_completion(visitor, s->as.module.body.data[i]);
      hm_completion_scope_pop(ctx);
    }
    return;
  }
  if (s->kind == NY_S_FUNC) {
    hm_call_arg_fact_ctx_t *ctx = visitor ? visitor->ctx : NULL;
    if (ctx && ctx->hm && ctx->hm->ctx && !ctx->hm->ctx->include_std &&
        s->as.fn.return_semantic.resolved)
      return;
  }
  ny_visit_stmt(visitor, s);
}

static void hm_desugar_partial_call(ny_hm_state_t *hm, ny_hm_env_list *env,
                                    expr_t *e, const char *self_name) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee || !hm->ctx ||
      !hm->ctx->cg)
    return;
  expr_t *callee = e->as.call.callee;
  /* Implicit value binders are filled before the call can look partial. */
  if (callee->kind == NY_E_IDENT && callee->as.ident.name &&
      !hm_env_get_ctx(env, callee->as.ident.name,
                      callee->as.ident.syntax_ctx)) {
    ny_hm_scheme_t *direct = hm_find_scheme(
        hm, e->semantic.canonical_callee ? e->semantic.canonical_callee
                                         : callee->as.ident.name);
    if (direct && direct->stmt)
      hm_fill_implicit_binders(hm, env, direct->stmt, &e->as.call.args, e->tok,
                               self_name);
  }
  if (callee->kind == NY_E_CALL && callee->as.call.callee &&
      callee->as.call.callee->kind == NY_E_IDENT) {
    const char *name = callee->as.call.callee->as.ident.name;
    ny_hm_type_t *shadow =
        hm_env_get_ctx(env, name, callee->as.call.callee->as.ident.syntax_ctx);
    if (!shadow)
      hm_persist_named_call_target(hm, callee, name, self_name);
    ny_hm_scheme_t *direct =
        shadow ? NULL
               : hm_find_scheme(hm, callee->semantic.canonical_callee
                                        ? callee->semantic.canonical_callee
                                        : name);
    size_t prefix = callee->as.call.args.len;
    size_t combined = prefix + e->as.call.args.len;
    if (direct && direct->stmt && direct->stmt->kind == NY_S_FUNC &&
        !hm_scheme_is_variadic(direct) && prefix > 0 &&
        prefix < hm_scheme_min_args(direct, 0) &&
        combined <= direct->stmt->as.fn.params.len &&
        hm_param_range_required(&direct->stmt->as.fn.params, prefix,
                                combined)) {
      bool positional = true;
      for (size_t i = 0; i < prefix; ++i)
        positional &= callee->as.call.args.data[i].name == NULL;
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        positional &= e->as.call.args.data[i].name == NULL;
      if (positional) {
        ny_call_arg_list args = {0};
        arena_t *arena = hm->ctx->cg->arena;
        for (size_t i = 0; i < prefix; ++i)
          vec_push_arena(arena, &args, callee->as.call.args.data[i]);
        for (size_t i = 0; i < e->as.call.args.len; ++i)
          vec_push_arena(arena, &args, e->as.call.args.data[i]);
        e->as.call.callee = callee->as.call.callee;
        e->as.call.args = args;
        callee = e->as.call.callee;
      }
    }
  }
  ny_hm_type_t *type = NULL;
  ny_hm_scheme_t *scheme = NULL;
  if (callee->kind == NY_E_IDENT) {
    type =
        hm_env_get_ctx(env, callee->as.ident.name, callee->as.ident.syntax_ctx);
    if (!type) {
      hm_persist_named_call_target(hm, e, callee->as.ident.name, self_name);
      scheme = hm_find_scheme(hm, e->semantic.canonical_callee
                                      ? e->semantic.canonical_callee
                                      : callee->as.ident.name);
      if (scheme)
        type = hm_instantiate(hm, scheme->type);
    }
  } else {
    type = hm_infer_expr(hm, env, callee, self_name);
  }
  type = hm_prune(type);
  size_t given = e->as.call.args.len;
  if (type && type->parameters) {
    size_t minimum = 0;
    for (size_t i = 0; i < type->parameters->len; ++i)
      if (!type->parameters->data[i].def)
        minimum = i + 1;
    if (!scheme && given >= minimum && given < type->parameters->len) {
      arena_t *arena = hm->ctx->cg->arena;
      for (size_t i = given; i < type->parameters->len; ++i) {
        expr_t *value = type->parameters->data[i].def;
        if (!value)
          return;
        for (size_t j = 0; j < i; ++j)
          value = ny_expr_subst(arena, value, type->parameters->data[j].name,
                                e->as.call.args.data[j].val);
        vec_push_arena(arena, &e->as.call.args, ((call_arg_t){.val = value}));
      }
      return;
    }
    if (given >= minimum)
      return;
  }
  if (!type || type->kind != NY_HM_FN || given == 0 ||
      given >= type->args.len ||
      (scheme && (hm_scheme_is_variadic(scheme) ||
                  given >= hm_scheme_min_args(scheme, 0) ||
                  scheme->stmt->as.fn.is_extern)))
    return;
  for (size_t i = 0; i < given; ++i)
    if (e->as.call.args.data[i].name)
      return;

  arena_t *arena = hm->ctx->cg->arena;
  const ny_param_list *params = NULL;
  if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC)
    params = &scheme->stmt->as.fn.params;
  else if (type->parameters)
    params = type->parameters;
  size_t param_count = params ? params->len : type->args.len;
  /*
   * Defaults between the supplied prefix and the last required parameter
   * cannot become residual parameters: a positional residual argument would
   * bind the default slot instead of the required one.  Evaluate those
   * defaults in the residual body and expose only the parameters that keep
   * positional binding intact (required parameters plus trailing optionals).
   */
  size_t required_end =
      params ? hm_param_required_end(params, given) : type->args.len;
  if (required_end <= given)
    return;
  /*
   * A residual closure is reusable, so it cannot safely capture a parameter
   * the callee consumes.  Reject the partial instead of letting a second
   * residual call consume the same linear resource again.
   */
  if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC && params) {
    for (size_t p = 0; p < given && p < params->len; ++p) {
      if (!hm_param_is_consumed(scheme->stmt, params->data[p].name))
        continue;
      tp_add_diag_ex(
          hm->ctx,
          e->as.call.args.data[p].val ? e->as.call.args.data[p].val->tok
                                      : e->tok,
          "hm", "hm-partial-consuming-capture", "partial application",
          "non-consuming capture", "consuming parameter", "partial application",
          "a residual closure can run more than once, so a consumed resource "
          "cannot be captured by value",
          "pass the resource to the residual call instead of binding it",
          "cannot partially bind consuming parameter `%s`: the residual "
          "callable may consume it more than once",
          params->data[p].name ? params->data[p].name : "<argument>");
      return;
    }
  }
  expr_t **actuals =
      arena_alloc(arena, sizeof(*actuals) * (param_count ? param_count : 1));
  if (!actuals)
    return;
  /*
   * A direct partial application with only stable supplied arguments
   * (literals/identifiers) can be expressed as one residual closure over
   * those arguments. This avoids the factory call and, for constant or
   * non-captured arguments, the per-application closure allocation that the
   * general factory form requires. Defaults between the supplied prefix and
   * the last required parameter are evaluated in the residual body so
   * positional binding remains intact.
   */
  bool stable_direct = callee && callee->kind == NY_E_IDENT;
  for (size_t p = 0; stable_direct && p < given; ++p) {
    expr_t *arg = e->as.call.args.data[p].val;
    if (!arg || (arg->kind != NY_E_LITERAL && arg->kind != NY_E_IDENT))
      stable_direct = false;
  }
  if (stable_direct) {
    expr_t *partial = expr_new(arena, NY_E_FN, e->tok);
    if (!scheme && type && type->a) {
      char *ret = hm_type_string(type->a);
      if (ret)
        partial->as.lambda.return_type =
            arena_strndup(arena, ret, strlen(ret));
      free(ret);
    }
    if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
        scheme->stmt->as.fn.return_type) {
      ny_hm_type_t *result = hm_subst_return_indices(
          hm, scheme->stmt, type->a, &e->as.call.args, 0, self_name);
      char *spelling = hm_type_string(result);
      if (spelling)
        partial->as.lambda.return_type =
            arena_strndup(arena, spelling, strlen(spelling));
      free(spelling);
    }
    expr_t *invoke = expr_new(arena, NY_E_CALL, e->tok);
    expr_t *direct = expr_new(arena, NY_E_IDENT, callee->tok);
    if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC) {
      direct->as.ident.name = ny_intern_get(ny_intern_cstr(
          scheme->full_name ? scheme->full_name : scheme->name));
      direct->as.ident.syntax_ctx = callee->as.ident.syntax_ctx;
      invoke->as.call.callee = direct;
      invoke->semantic.canonical_callee = direct->as.ident.name;
      invoke->semantic.canonical_callee_stmt = scheme->stmt;
    } else {
      const char *direct_name = callee->as.ident.name;
      stmt_t *direct_stmt = NULL;
      ny_hm_env_entry_t *alias_entry = hm_env_entry_ctx(
          env, callee->as.ident.name, callee->as.ident.syntax_ctx);
      if (alias_entry && alias_entry->alias_target) {
        direct_name = alias_entry->alias_target;
        direct_stmt = alias_entry->alias_target_stmt;
      }
      direct->as.ident.name = direct_name;
      direct->as.ident.syntax_ctx = callee->as.ident.syntax_ctx;
      invoke->as.call.callee = direct;
      invoke->semantic.canonical_callee = direct_name;
      invoke->semantic.canonical_callee_stmt = direct_stmt;
    }
    for (size_t p = 0; p < given; ++p) {
      expr_t *argument = e->as.call.args.data[p].val;
      actuals[p] = argument;
      vec_push_arena(arena, &invoke->as.call.args,
                     ((call_arg_t){.val = argument}));
    }
    for (size_t p = given; p < param_count; ++p) {
      const char *formal =
          params && p < params->len ? params->data[p].name : NULL;
      expr_t *param_def =
          params && p < params->len ? params->data[p].def : NULL;
      if (p < required_end && param_def) {
        expr_t *value = param_def;
        for (size_t j = 0; j < p; ++j) {
          const char *prev =
              params && j < params->len ? params->data[j].name : NULL;
          if (prev && actuals[j])
            value = ny_expr_subst(arena, value, prev, actuals[j]);
        }
        actuals[p] = value;
        vec_push_arena(arena, &invoke->as.call.args,
                       ((call_arg_t){.val = value}));
        continue;
      }
      char name[96];
      snprintf(name, sizeof(name), "$partial_%d_%zu", hm->next_var++, p);
      const char *owned_name = arena_strndup(arena, name, strlen(name));
      if (formal && *formal)
        owned_name = formal;
      expr_t *reference = expr_new(arena, NY_E_IDENT, e->tok);
      reference->as.ident.name = owned_name;
      param_t parameter = {.name = owned_name, .tok = e->tok};
      if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
          p < scheme->stmt->as.fn.params.len) {
        char *substituted =
            hm_subst_param_type_text(hm, scheme->stmt, p, &e->as.call.args, 0);
        parameter.type =
            substituted ? arena_strndup(arena, substituted, strlen(substituted))
                        : scheme->stmt->as.fn.params.data[p].type;
        free(substituted);
      } else if (p < type->args.len) {
        char *spelling = hm_type_string(type->args.data[p]);
        if (spelling)
          parameter.type = arena_strndup(arena, spelling, strlen(spelling));
        free(spelling);
      }
      if (param_def) {
        expr_t *value = param_def;
        for (size_t j = 0; j < p; ++j) {
          const char *prev =
              params && j < params->len ? params->data[j].name : NULL;
          if (prev && actuals[j])
            value = ny_expr_subst(arena, value, prev, actuals[j]);
        }
        parameter.def = value;
      }
      actuals[p] = reference;
      vec_push_arena(arena, &partial->as.lambda.params, parameter);
      vec_push_arena(arena, &invoke->as.call.args,
                     ((call_arg_t){.val = reference}));
    }
    stmt_t *inner = stmt_new(arena, NY_S_BLOCK, e->tok);
    stmt_t *inner_return = stmt_new(arena, NY_S_RETURN, e->tok);
    inner_return->as.ret.value = invoke;
    vec_push_arena(arena, &inner->as.block.body, inner_return);
    partial->as.lambda.body = inner;
    *e = *partial;
    return;
  }
  expr_t *factory = expr_new(arena, NY_E_FN, e->tok);
  expr_t *partial = expr_new(arena, NY_E_FN, e->tok);
  if (!scheme && type && type->a) {
    char *ret = hm_type_string(type->a);
    if (ret)
      partial->as.lambda.return_type = arena_strndup(arena, ret, strlen(ret));
    free(ret);
  }
  if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
      scheme->stmt->as.fn.return_type) {
    ny_hm_type_t *result = hm_subst_return_indices(
        hm, scheme->stmt, type->a, &e->as.call.args, 0, self_name);
    char *spelling = hm_type_string(result);
    partial->as.lambda.return_type =
        arena_strndup(arena, spelling, strlen(spelling));
    free(spelling);
  }
  expr_t *invoke = expr_new(arena, NY_E_CALL, e->tok);
  expr_t *replacement = expr_new(arena, NY_E_CALL, e->tok);
  replacement->as.call.callee = factory;
  /*
   * A factory binds the callable and supplied arguments exactly once,
   * before the residual closure escapes. Full applications stay untouched.
   */
  int partial_id = hm->next_var++;
  for (size_t i = 0; i <= param_count; ++i) {
    char name[96];
    snprintf(name, sizeof(name), "$partial_%d_%zu", partial_id, i);
    const char *owned_name = arena_strndup(arena, name, strlen(name));
    size_t p = i - 1;
    const char *param_name =
        params && p < params->len ? params->data[p].name : NULL;
    expr_t *param_def = params && p < params->len ? params->data[p].def : NULL;
    if (i > 0 && p >= given && param_name && *param_name && scheme &&
        scheme->stmt && scheme->stmt->kind == NY_S_FUNC)
      owned_name = param_name;
    expr_t *reference = expr_new(arena, NY_E_IDENT, e->tok);
    reference->as.ident.name = owned_name;
    param_t parameter = {.name = owned_name, .tok = e->tok};
    if (i == 0) {
      if (scheme) {
        expr_t *direct = expr_new(arena, NY_E_IDENT, callee->tok);
        direct->as.ident.name = ny_intern_get(ny_intern_cstr(
            scheme->full_name ? scheme->full_name : scheme->name));
        invoke->as.call.callee = direct;
        invoke->semantic.canonical_callee = direct->as.ident.name;
        invoke->semantic.canonical_callee_stmt = scheme->stmt;
        continue;
      }
      /*
       * A callable bound to a name needs no factory slot.  The residual
       * closure can capture that immutable binding directly; passing it as an
       * untyped factory parameter erased its call signature and made the
       * deferred call dynamic.
       */
      if (callee && callee->kind == NY_E_IDENT) {
        expr_t *direct = expr_new(arena, NY_E_IDENT, callee->tok);
        direct->as.ident.name = callee->as.ident.name;
        direct->as.ident.syntax_ctx = callee->as.ident.syntax_ctx;
        invoke->as.call.callee = direct;
        continue;
      }
      vec_push_arena(arena, &factory->as.lambda.params, parameter);
      vec_push_arena(arena, &replacement->as.call.args,
                     ((call_arg_t){.val = callee}));
      invoke->as.call.callee = reference;
    } else {
      if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC) {
        char *substituted =
            hm_subst_param_type_text(hm, scheme->stmt, p, &e->as.call.args, 0);
        parameter.type =
            substituted ? arena_strndup(arena, substituted, strlen(substituted))
                        : scheme->stmt->as.fn.params.data[p].type;
        free(substituted);
      } else if (!scheme && type && p < type->args.len) {
        char *spelling = hm_type_string(type->args.data[p]);
        if (spelling)
          parameter.type = arena_strndup(arena, spelling, strlen(spelling));
        free(spelling);
      }
      /*
       * Inferred lambdas keep residual parameter types inferred.  Stamping the
       * source spelling here changes the generated closure's representation
       * and ABI even though the lambda had no explicit parameter types.
       */
      bool captured = p < given;
      bool inject = !captured && param_def && p < required_end;
      if (captured) {
        expr_t *argument = reference;
        if (scheme && e->as.call.args.data[p].val->kind == NY_E_LITERAL &&
            e->as.call.args.data[p].val->as.literal.kind == NY_LIT_INT)
          argument = e->as.call.args.data[p].val;
        actuals[p] = reference;
        vec_push_arena(arena, &invoke->as.call.args,
                       ((call_arg_t){.val = argument}));
        vec_push_arena(arena, &factory->as.lambda.params, parameter);
        vec_push_arena(arena, &replacement->as.call.args,
                       e->as.call.args.data[p]);
      } else if (inject) {
        expr_t *value = param_def;
        for (size_t j = 0; j < p; ++j) {
          const char *formal =
              params && j < params->len ? params->data[j].name : NULL;
          if (formal && actuals[j])
            value = ny_expr_subst(arena, value, formal, actuals[j]);
        }
        actuals[p] = value;
        vec_push_arena(arena, &invoke->as.call.args,
                       ((call_arg_t){.val = value}));
      } else {
        if (param_def) {
          expr_t *value = param_def;
          for (size_t j = 0; j < p; ++j) {
            const char *formal =
                params && j < params->len ? params->data[j].name : NULL;
            if (formal && actuals[j])
              value = ny_expr_subst(arena, value, formal, actuals[j]);
          }
          parameter.def = value;
        }
        actuals[p] = reference;
        vec_push_arena(arena, &partial->as.lambda.params, parameter);
        vec_push_arena(arena, &invoke->as.call.args,
                       ((call_arg_t){.val = reference}));
      }
    }
  }
  stmt_t *inner = stmt_new(arena, NY_S_BLOCK, e->tok);
  stmt_t *inner_return = stmt_new(arena, NY_S_RETURN, e->tok);
  inner_return->as.ret.value = invoke;
  vec_push_arena(arena, &inner->as.block.body, inner_return);
  partial->as.lambda.body = inner;
  stmt_t *outer = stmt_new(arena, NY_S_BLOCK, e->tok);
  stmt_t *outer_return = stmt_new(arena, NY_S_RETURN, e->tok);
  outer_return->as.ret.value = partial;
  vec_push_arena(arena, &outer->as.block.body, outer_return);
  factory->as.lambda.body = outer;
  /*
   * Statement-position bookkeeping: when this exact call is the enclosing
   * expression statement, its residual closure is created and immediately
   * discarded, which almost always means the user under-supplied arguments.
   * Record the required-but-missing parameter names so the statement frame
   * can report them after inference completes.
   */
  if (e == hm->stmt_partial_call && !hm->stmt_partial_missing && hm->ctx) {
    char names[1024];
    size_t used = 0;
    size_t missing_count = 0;
    names[0] = '\0';
    for (size_t k = given; k < type->args.len; ++k) {
      const char *name = NULL;
      const expr_t *def = NULL;
      if (scheme && scheme->stmt && scheme->stmt->kind == NY_S_FUNC &&
          k < scheme->stmt->as.fn.params.len) {
        name = scheme->stmt->as.fn.params.data[k].name;
        def = scheme->stmt->as.fn.params.data[k].def;
      } else if (type->parameters && k < type->parameters->len) {
        name = type->parameters->data[k].name;
        def = type->parameters->data[k].def;
      }
      /*
       * Options before the last required parameter are evaluated, not
       * exposed, so they are not part of the missing residual signature.
       */
      if (k < required_end && def)
        continue;
      if (def)
        continue; /* defaulted residual parameters need no further argument */
      missing_count++;
      int nw;
      if (name && *name)
        nw = snprintf(names + used, sizeof(names) - used, "%s`%s`",
                      used ? ", " : "", name);
      else
        nw = snprintf(names + used, sizeof(names) - used, "%sargument %zu",
                      used ? ", " : "", k + 1);
      if (nw < 0 || (size_t)nw >= sizeof(names) - used) {
        used = sizeof(names) - 1;
        names[used] = '\0';
        break;
      }
      used += (size_t)nw;
    }
    if (used && used < sizeof(names) && missing_count) {
      hm->stmt_partial_missing = ny_intern_get(ny_intern_cstr(names));
      hm->stmt_partial_missing_count = missing_count;
    }
  }
  *e = *replacement;
}

static ny_hm_type_t *hm_infer_expr(ny_hm_state_t *hm, ny_hm_env_list *env,
                                   expr_t *e, const char *self_name) {
  hm_desugar_partial_call(hm, env, e, self_name);
  ny_hm_type_t *result = hm_infer_expr_impl(hm, env, e, self_name);
  if (!e)
    return result;
  char *type_name = hm_type_string(result);
  bool collect_semantic_facts =
      hm->ctx && hm->ctx->collect_semantic_artifact_facts;
  const char *hm_type_name = collect_semantic_facts && result && type_name
                                 ? ny_intern_get(ny_intern_cstr(type_name))
                                 : NULL;
  if (collect_semantic_facts && result) {
    ny_hm_expr_type_fact_t fact = {.expr = e, .type = result};
    vec_push(&hm->expr_type_facts, fact);
  }
  e->semantic.rep = hm_semantic_rep_in_context(hm, type_name);
  e->semantic.resolved = result != NULL;
  e->semantic.hm_type_name = hm_type_name;
  /*
   * Indexed/member resource values need their actual type during ownership
   * validation even when semantic-artifact collection is off. Nominal
   * projections also need it for layout-aware native field lowering.
   */
  bool typed_projection =
      (e->kind == NY_E_INDEX || e->kind == NY_E_MEMBER) &&
      ((hm->ctx && hm->ctx->cg && hm->ctx->cg->ownership_strict) ||
       e->semantic.rep == NY_SEM_REP_AGGREGATE);
  if (e->kind == NY_E_IDENT || typed_projection)
    e->semantic.resolved_type_name =
        result && type_name ? ny_intern_get(ny_intern_cstr(type_name)) : NULL;
  free(type_name);
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    ny_hm_env_entry_t *entry =
        hm_env_entry_ctx(env, e->as.ident.name, e->as.ident.syntax_ctx);
    if (entry) {
      e->semantic.ownership = entry->ownership;
      e->semantic.alias_class = entry->alias_class;
      e->semantic.mutable_value = entry->mutable_value;
    }
  }
  if (e->kind == NY_E_MEMCALL || e->kind == NY_E_MEMBER)
    hm_persist_member_target(hm, env, e, self_name);
  else if (e->kind == NY_E_CALL && e->as.call.callee &&
           e->as.call.callee->kind == NY_E_MEMBER) {
    expr_t member = {.kind = NY_E_MEMCALL, .tok = e->tok};
    member.as.memcall.target = e->as.call.callee->as.member.target;
    member.as.memcall.name = e->as.call.callee->as.member.name;
    hm_persist_member_target(hm, env, &member, self_name);
    if (member.semantic.member_call_kind != NY_SEM_CALL_ERROR) {
      e->semantic = member.semantic;
      e->semantic.hm_type_name = hm_type_name;
    }
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name) {
    const char *full = e->as.call.callee->as.ident.name;
    const char *dot = strrchr(full, '.');
    if (dot && dot > full && dot[1]) {
      size_t base_len = (size_t)(dot - full);
      char *base = hm_arena_strdup(hm, full);
      if (base) {
        base[base_len] = '\0';
        /*
         * Module globals are not installed in a function's local HM
         * environment.  Private dotted values are nevertheless value member
         * calls (for example `_history.append(...)`), not qualified module
         * calls, and must receive the normal `any` attached-method contract.
         */
        if (base[0] != '_' &&
            !hm_env_get_ctx(env, base, e->as.call.callee->as.ident.syntax_ctx))
          return result;
        expr_t target = {.kind = NY_E_IDENT, .tok = e->tok};
        target.as.ident.name = base;
        target.as.ident.syntax_ctx = e->as.call.callee->as.ident.syntax_ctx;
        expr_t member = {.kind = NY_E_MEMCALL, .tok = e->tok};
        member.as.memcall.target = &target;
        member.as.memcall.name = dot + 1;
        hm_persist_member_target(hm, env, &member, self_name);
        if (member.semantic.member_call_kind != NY_SEM_CALL_ERROR) {
          e->semantic = member.semantic;
          e->semantic.hm_type_name = hm_type_name;
        }
      }
    }
  }
  return result;
}

static void hm_finalize_expr_type_facts(ny_hm_state_t *hm) {
  for (size_t i = 0; hm && i < hm->expr_type_facts.len; ++i) {
    ny_hm_expr_type_fact_t *fact = &hm->expr_type_facts.data[i];
    if (!fact->expr || !fact->type)
      continue;
    char *name = hm_type_string(fact->type);
    fact->expr->semantic.hm_type_name =
        name ? ny_intern_get(ny_intern_cstr(name)) : NULL;
    free(name);
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

static ny_hm_env_entry_t *
hm_refinement_element_store_fact(ny_hm_state_t *hm, ny_hm_env_list *env,
                                 const expr_t *value) {
  if (!value || value->kind != NY_E_CALL ||
      !value->as.call.synthetic_index_assignment || !value->as.call.callee ||
      value->as.call.callee->kind != NY_E_IDENT ||
      !value->as.call.callee->as.ident.name ||
      strcmp(value->as.call.callee->as.ident.name, "set_idx") != 0 ||
      value->as.call.args.len != 3)
    return NULL;
  const expr_t *target = value->as.call.args.data[0].val;
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return NULL;
  ny_hm_env_entry_t *entry =
      hm_env_entry_ctx(env, target->as.ident.name, target->as.ident.syntax_ctx);
  if (!entry || !entry->mutable_value || !entry->type)
    return NULL;
  char *type_name = hm_type_string(entry->type);
  stmt_t *definition = type_name ? hm_lookup_refine_stmt(hm, type_name) : NULL;
  int64_t store_index = 0;
  bool fixed_index = hm_refinement_const_i64_expr(
      value->as.call.args.data[1].val, &store_index);
  bool preserves =
      definition &&
      (hm_refinement_length_lower_bound_predicate(definition) ||
       hm_refinement_indexed_store_value_preserves(
           hm, env, definition, value->as.call.args.data[1].val,
           value->as.call.args.data[2].val) ||
       (fixed_index && hm_refinement_content_predicate_survives(
                           hm, env, definition, store_index, false, NULL)));
  free(type_name);
  return preserves ? entry : NULL;
}

static ny_hm_env_entry_t *
hm_refinement_append_assignment_fact(ny_hm_state_t *hm, ny_hm_env_list *env,
                                     const stmt_t *stmt) {
  if (!hm || !env || !stmt || stmt->kind != NY_S_VAR || stmt->as.var.is_decl ||
      stmt->as.var.is_del || stmt->as.var.names.len != 1 ||
      stmt->as.var.exprs.len != 1 || !stmt->as.var.names.data[0])
    return NULL;
  ny_hm_env_entry_t *entry =
      hm_env_entry_ctx(env, stmt->as.var.names.data[0], stmt->syntax_ctx);
  if (!entry || !entry->mutable_value || !entry->type ||
      !hm_refinement_nonempty_extension_assignment(hm, env, stmt, entry->name,
                                                   entry->syntax_ctx))
    return NULL;
  char *type_name = hm_type_string(entry->type);
  stmt_t *definition = type_name ? hm_lookup_refine_stmt(hm, type_name) : NULL;
  bool preserves = definition && hm_refinement_extension_assignment_preserves(
                                     hm, env, stmt, entry->name,
                                     entry->syntax_ctx, definition);
  free(type_name);
  return preserves ? entry : NULL;
}

static void hm_infer_stmt_mode(ny_hm_state_t *hm, ny_hm_env_list *env,
                               stmt_t *s, const char *self_name,
                               ny_hm_type_t *ret, bool allow_tail_expr) {
  if (!hm || !s || !tp_stmt_in_scope(hm->ctx, s))
    return;
  switch (s->kind) {
  case NY_S_VAR: {
    ny_hm_env_entry_t *preserved_append_fact =
        hm_refinement_append_assignment_fact(hm, env, s);
    ny_hm_type_t *preserved_append_type =
        preserved_append_fact ? preserved_append_fact->type : NULL;
    ny_hm_type_t *destructure_item_type = NULL;
    if (s->as.var.is_destructure && s->as.var.exprs.len == 1 &&
        s->as.var.exprs.data[0]) {
      ny_hm_type_t *source_type = hm_prune(hm_infer_expr_dynamic_literal_ok(
          hm, env, s->as.var.exprs.data[0], self_name, false));
      if (source_type &&
          (source_type->kind == NY_HM_LIST ||
           source_type->kind == NY_HM_TUPLE || source_type->kind == NY_HM_SET))
        destructure_item_type = source_type->a;
      else if (hm_is_name(source_type, "str"))
        destructure_item_type = hm_name(hm, "str");
      else
        destructure_item_type = hm_any(hm);
    }
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      if (!s->as.var.is_decl && hm && hm->comptime_depth > 0 && name &&
          hm_is_mutable_binding(hm, env, name, s->syntax_ctx,
                                hm->comptime_outer_env_len)) {
        tp_add_diag_ex(hm->ctx, s->tok, "hm", "hm-comptime-mut-capture",
                       "comptime", "local compile-time mutation",
                       "outer variable mutation", "comptime",
                       "comptime{} is closed code and cannot mutate variables "
                       "from outer runtime scope",
                       "declare the mutable variable inside the comptime block",
                       "cannot mutate outer runtime variable '%s' in "
                       "compile-time evaluation",
                       name);
      }
      if (s->as.var.is_del) {
        ny_hm_env_entry_t *entry = hm_env_entry_ctx(env, name, s->syntax_ctx);
        if (entry && !entry->mutable_value) {
          tp_add_diag_ex(
              hm->ctx, s->tok, "hm", "hm-immutable-delete", "delete",
              "mutable binding", "immutable binding", "delete",
              "deleting a binding changes the surrounding environment",
              "declare the binding with `mut` before deleting it",
              "cannot reassign immutable variable '%s'", name ? name : "");
        }
        continue;
      }
      const char *decl =
          i < s->as.var.types.len ? s->as.var.types.data[i] : NULL;
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      bool explicit_dynamic = hm_type_name_allows_dynamic_literal(decl);
      ny_hm_type_t *it =
          s->as.var.is_destructure
              ? (destructure_item_type ? destructure_item_type : hm_any(hm))
              : (init ? hm_infer_expr_dynamic_literal_ok(
                            hm, env, init, self_name, explicit_dynamic)
                      : hm_var(hm));
      ny_hm_type_t *final_t =
          decl && *decl ? hm_parse_type_name(hm, decl, self_name) : it;
      ny_hm_type_t *preserved_refined_type = NULL;
      if (!s->as.var.is_decl && init && !preserved_append_fact) {
        ny_hm_env_entry_t *entry = hm_env_entry_ctx(env, name, s->syntax_ctx);
        ny_hm_type_t *previous = entry && entry->guard_base_type
                                     ? entry->guard_base_type
                                     : hm_env_get_ctx(env, name, s->syntax_ctx);
        char *previous_name = previous ? hm_type_string(previous) : NULL;
        bool refined =
            previous_name && hm_lookup_refine_stmt(hm, previous_name);
        free(previous_name);
        size_t before = hm->ctx ? hm->ctx->diagnostics.len : 0;
        hm_check_refinement_return(hm, previous, it, init, init->tok);
        /*
         * Keep the declaration's refinement after a valid reassignment.
         * `a = 6` proves `Positive` for a `mut Positive a`; dropping the fact
         * here would reject every later call that requires `Positive` even
         * though the value was checked.  A failed proof leaves the binding
         * unrefined and already reports the violation.
         */
        if (refined && hm->ctx && hm->ctx->diagnostics.len == before)
          preserved_refined_type = previous;
        if (entry && entry->guard_base_type)
          entry->guard_base_type = NULL;
      }
      if (init && decl && strcmp(decl, "i64") == 0 &&
          hm_is_name(it, "bigint")) {
        __int128 value = 0;
        if (tp_eval_const_int_wide(init, &value, 0) && value >= INT64_MIN &&
            value <= INT64_MAX) {
          it = final_t;
        }
      }
      if (decl && *decl) {
        hm_check_sigma_initializer(hm, env, decl, init, self_name,
                                   init ? init->tok : s->tok);
        hm_check_fin_initializer(hm, decl, init, init ? init->tok : s->tok);
        hm_check_indexed_initializer(hm, decl, init, init ? init->tok : s->tok);
        hm_check_refinement_initializer(hm, decl, init,
                                        init ? init->tok : s->tok);
        hm_check_refinement_return(hm, final_t, it, init,
                                   init ? init->tok : s->tok);
        hm_unify(hm, final_t, it, init ? init->tok : s->tok,
                 "variable declaration");
      } else if (!s->as.var.is_decl && preserved_refined_type) {
        final_t = preserved_refined_type;
      } else if (!s->as.var.is_decl && hm_is_any(final_t)) {
        /*
         * Reassignment without an annotation where the new value degrades to
         * `any`: keep the binding's existing concrete collection type.
         * Without this, a value flowing through a generic helper (e.g.
         * append(any, any) -> any) would downgrade an explicitly typed list
         * binding to `any`, and later uses (index reads, member access) would
         * fall back to dynamic dispatch.  Only collections are preserved;
         * scalar/nil bindings keep normal inference.
         */
        ny_hm_type_t *prev = hm_env_get_ctx(env, name, s->syntax_ctx);
        if (prev && hm_type_known_collection(prev) && prev != final_t)
          final_t = prev;
      }
      if (decl && strcmp(decl, "proof") == 0 && it && it->kind == NY_HM_NAME &&
          it->name && strncmp(it->name, "proof<", 6) == 0)
        final_t = it;
      if (s->as.var.is_decl && hm && hm->comptime_depth > 0)
        hm_env_push_ctx(env, name, s->syntax_ctx, final_t);
      else
        hm_env_set_ctx(env, name, s->syntax_ctx, final_t);
      /*
       * Preserve callable provenance through simple aliases.  This lets a
       * deferred partial call resolve the underlying function's effect row
       * instead of degrading to an indirect call whose effects are unknown.
       */
      if (!s->as.var.is_destructure && name && init &&
          init->kind == NY_E_IDENT && init->as.ident.name) {
        const char *target = init->as.ident.name;
        stmt_t *target_stmt = NULL;
        ny_hm_env_entry_t *source =
            hm_env_entry_ctx(env, target, init->as.ident.syntax_ctx);
        if (source && source->alias_target) {
          target = source->alias_target;
          target_stmt = source->alias_target_stmt;
        } else {
          ny_hm_scheme_t *target_scheme = hm_find_scheme(hm, target);
          if (target_scheme) {
            target = target_scheme->full_name ? target_scheme->full_name
                                              : target_scheme->name;
            target_stmt = target_scheme->stmt;
          }
        }
        ny_hm_env_entry_t *alias_entry =
            hm_env_entry_ctx(env, name, s->syntax_ctx);
        if (alias_entry && target) {
          /*
           * Scheme names are not guaranteed to outlive this inference step.
           * Keep a stable arena copy so deferred desugaring and the effect
           * pass see the real callee spelling.
           */
          alias_entry->alias_target = ny_intern_get(ny_intern_cstr(target));
          alias_entry->alias_target_stmt = target_stmt;
        }
      }
      if (s->as.var.is_decl && decl) {
        hm_sigma_parts_t sigma_probe;
        if (hm_sigma_parts_parse(decl, &sigma_probe)) {
          hm_sigma_parts_free(&sigma_probe);
          ny_hm_env_entry_t *sigma_entry =
              hm_env_entry_ctx(env, name, s->syntax_ctx);
          if (sigma_entry) {
            sigma_entry->sigma_decl = decl;
            sigma_entry->sigma_index_literal =
                hm_sigma_literal_index(init, &sigma_entry->sigma_index_value);
          }
        }
      }
      ny_sem_ownership_t ownership =
          !s->as.var.is_destructure && init &&
                  (init->kind == NY_E_LIST || init->kind == NY_E_DICT ||
                   init->kind == NY_E_SET || init->kind == NY_E_TUPLE)
              ? NY_SEM_OWN_OWNED
              : NY_SEM_OWN_UNKNOWN;
      hm_env_set_value_facts(hm, env, name, s->syntax_ctx, s->as.var.is_mut,
                             ownership, !s->as.var.is_decl);
      if (init) {
        ny_hm_env_entry_t *entry = hm_env_entry_ctx(env, name, s->syntax_ctx);
        if (entry) {
          init->semantic.ownership = entry->ownership;
          init->semantic.alias_class = entry->alias_class;
          init->semantic.mutable_value = entry->mutable_value;
        }
      }
    }
    if (preserved_append_fact) {
      ny_hm_env_entry_t *entry = hm_env_entry_ctx(
          env, preserved_append_fact->name, preserved_append_fact->syntax_ctx);
      if (entry)
        entry->type = preserved_append_type;
    }
    break;
  }
  case NY_S_EXPR: {
    ny_hm_env_entry_t *preserved_element_fact =
        hm_refinement_element_store_fact(hm, env, s->as.expr.expr);
    ny_hm_type_t *preserved_type =
        preserved_element_fact ? preserved_element_fact->type : NULL;
    if (ret && allow_tail_expr) {
      ny_hm_type_t *value_t =
          hm_infer_expr(hm, env, s->as.expr.expr, self_name);
      if (s->as.expr.expr)
        hm_check_refinement_return(hm, ret, value_t, s->as.expr.expr,
                                   s->as.expr.expr->tok);
      hm_unify(hm, ret, value_t,
               s->as.expr.expr ? s->as.expr.expr->tok : s->tok,
               "implicit return value");
    } else {
      /*
       * Statement-position partial applications never run the callee; the
       * desugared residual is created and immediately discarded. Stamp the
       * exact call being inferred so hm_desugar_partial_call can record the
       * required-but-missing parameter names, and report them here. Save and
       * restore the frame around the inference: argument lambdas infer their
       * own statement frames beneath this one.
       */
      expr_t *stmt_call = s->as.expr.expr && s->as.expr.expr->kind == NY_E_CALL
                              ? s->as.expr.expr
                              : NULL;
      expr_t *saved_partial_call = hm->stmt_partial_call;
      const char *saved_partial_missing = hm->stmt_partial_missing;
      size_t saved_partial_count = hm->stmt_partial_missing_count;
      hm->stmt_partial_call = stmt_call;
      hm->stmt_partial_missing = NULL;
      hm->stmt_partial_missing_count = 0;
      (void)hm_infer_expr(hm, env, s->as.expr.expr, self_name);
      if (stmt_call && hm->stmt_partial_call == stmt_call &&
          hm->stmt_partial_missing)
        tp_add_diag_ex(hm->ctx, s->as.expr.expr->tok, "hm", "hm-partial-arity",
                       "partial application", "saturated call",
                       "residual function value discarded as a statement",
                       "function call",
                       "a partial application produces a residual function "
                       "value and never runs the callee",
                       "supply the remaining required arguments, or bind the "
                       "partial with `def` or `mut`",
                       "missing %s", hm->stmt_partial_missing);
      hm->stmt_partial_call = saved_partial_call;
      hm->stmt_partial_missing = saved_partial_missing;
      hm->stmt_partial_missing_count = saved_partial_count;
    }
    if (preserved_element_fact)
      preserved_element_fact->type = preserved_type;
  } break;
  case NY_S_RETURN:
    if (ret) {
      ny_hm_type_t *vt =
          s->as.ret.value ? hm_infer_expr(hm, env, s->as.ret.value, self_name)
                          : hm_name(hm, "nil");
      if (s->as.ret.value)
        hm_check_refinement_return(hm, ret, vt, s->as.ret.value,
                                   s->as.ret.value->tok);
      hm_unify(hm, ret, vt, s->as.ret.value ? s->as.ret.value->tok : s->tok,
               "return value");
    }
    break;
  case NY_S_IF:
    hm_infer_stmt_mode(hm, env, s->as.iff.init, self_name, ret, false);
    hm_strict_condition_check(
        hm, s->as.iff.test, hm_infer_expr(hm, env, s->as.iff.test, self_name));
    ny_hm_type_t *saved_guard_type = NULL;
    ny_hm_type_t *saved_guard_base_type = NULL;
    ny_hm_type_t *guard_type = NULL;
    ny_hm_env_entry_t *guard_entry = hm_refinement_guard_entry(
        hm, env, s->as.iff.test, &guard_type, false, s->as.iff.conseq);
    ssize_t guard_entry_idx = -1;
    if (guard_entry) {
      guard_entry_idx = guard_entry - env->data;
      saved_guard_type = guard_entry->type;
      saved_guard_base_type = guard_entry->guard_base_type;
      guard_entry->guard_base_type = saved_guard_type;
      guard_entry->type = guard_type;
    }
    hm_infer_stmt_mode(hm, env, s->as.iff.conseq, self_name, ret,
                       allow_tail_expr);
    if (guard_entry_idx >= 0 && (size_t)guard_entry_idx < env->len) {
      ny_hm_env_entry_t *restored = &env->data[guard_entry_idx];
      bool invalidated_outer_guard =
          saved_guard_base_type && !restored->guard_base_type;
      restored->type =
          invalidated_outer_guard ? saved_guard_base_type : saved_guard_type;
      restored->guard_base_type =
          invalidated_outer_guard ? NULL : saved_guard_base_type;
    }
    ny_hm_type_t *else_guard_type = NULL;
    ny_hm_env_entry_t *else_guard_entry = hm_refinement_guard_entry(
        hm, env, s->as.iff.test, &else_guard_type, true, s->as.iff.alt);
    ssize_t else_guard_entry_idx = -1;
    ny_hm_type_t *else_saved_guard_type = NULL;
    ny_hm_type_t *else_saved_guard_base_type = NULL;
    if (else_guard_entry) {
      else_guard_entry_idx = else_guard_entry - env->data;
      else_saved_guard_type = else_guard_entry->type;
      else_saved_guard_base_type = else_guard_entry->guard_base_type;
      else_guard_entry->guard_base_type = else_saved_guard_type;
      else_guard_entry->type = else_guard_type;
    }
    hm_infer_stmt_mode(hm, env, s->as.iff.alt, self_name, ret, allow_tail_expr);
    if (else_guard_entry_idx >= 0 && (size_t)else_guard_entry_idx < env->len) {
      ny_hm_env_entry_t *restored = &env->data[else_guard_entry_idx];
      bool invalidated_outer_guard =
          else_saved_guard_base_type && !restored->guard_base_type;
      restored->type = invalidated_outer_guard ? else_saved_guard_base_type
                                               : else_saved_guard_type;
      restored->guard_base_type =
          invalidated_outer_guard ? NULL : else_saved_guard_base_type;
    }
    break;
  case NY_S_BLOCK:
    hm_infer_stmt_list_mode(hm, env, &s->as.block.body, self_name, ret,
                            allow_tail_expr);
    break;
  case NY_S_WHILE:
    hm_infer_stmt_mode(hm, env, s->as.whl.init, self_name, ret, false);
    hm_strict_condition_check(
        hm, s->as.whl.test, hm_infer_expr(hm, env, s->as.whl.test, self_name));
    {
      ny_hm_type_t *while_guard_type = NULL;
      ny_hm_env_entry_t *while_guard_entry = hm_refinement_guard_entry(
          hm, env, s->as.whl.test, &while_guard_type, false, s->as.whl.body);
      ssize_t while_guard_idx = -1;
      ny_hm_type_t *while_saved_type = NULL;
      ny_hm_type_t *while_saved_guard_base_type = NULL;
      if (while_guard_entry) {
        while_guard_idx = while_guard_entry - env->data;
        while_saved_type = while_guard_entry->type;
        while_saved_guard_base_type = while_guard_entry->guard_base_type;
        while_guard_entry->guard_base_type = while_saved_type;
        while_guard_entry->type = while_guard_type;
      }
      hm_infer_stmt_mode(hm, env, s->as.whl.body, self_name, ret, false);
      if (while_guard_idx >= 0 && (size_t)while_guard_idx < env->len) {
        ny_hm_env_entry_t *restored = &env->data[while_guard_idx];
        bool invalidated_outer_guard =
            while_saved_guard_base_type && !restored->guard_base_type;
        restored->type = invalidated_outer_guard ? while_saved_guard_base_type
                                                 : while_saved_type;
        restored->guard_base_type =
            invalidated_outer_guard ? NULL : while_saved_guard_base_type;
      }
    }
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
      else if (iter_t && hm_is_name(iter_t, "str"))
        item = hm_name(hm, "str");
      else if (iter_t &&
               (hm_is_name(iter_t, "bytes") || hm_is_name(iter_t, "range")))
        item = hm_name(hm, "int");
      hm_env_set_ctx(env, s->as.fr.iter_var, s->syntax_ctx, item);
    }
    if (s->as.fr.iter_index_var)
      hm_env_set_ctx(env, s->as.fr.iter_index_var, s->syntax_ctx,
                     hm_name(hm, "int"));
    hm_strict_condition_check(hm, s->as.fr.cond,
                              hm_infer_expr(hm, env, s->as.fr.cond, self_name));
    ny_hm_type_t *for_guard_type = NULL;
    ny_hm_env_entry_t *for_guard_entry = hm_refinement_guard_entry(
        hm, env, s->as.fr.cond, &for_guard_type, false, s->as.fr.body);
    ssize_t for_guard_idx = -1;
    ny_hm_type_t *for_saved_type = NULL;
    ny_hm_type_t *for_saved_guard_base_type = NULL;
    if (for_guard_entry) {
      for_guard_idx = for_guard_entry - env->data;
      for_saved_type = for_guard_entry->type;
      for_saved_guard_base_type = for_guard_entry->guard_base_type;
      for_guard_entry->guard_base_type = for_saved_type;
      for_guard_entry->type = for_guard_type;
    }
    hm_infer_stmt_mode(hm, env, s->as.fr.body, self_name, ret, false);
    if (for_guard_idx >= 0 && (size_t)for_guard_idx < env->len) {
      ny_hm_env_entry_t *restored = &env->data[for_guard_idx];
      bool invalidated_outer_guard =
          for_saved_guard_base_type && !restored->guard_base_type;
      restored->type =
          invalidated_outer_guard ? for_saved_guard_base_type : for_saved_type;
      restored->guard_base_type =
          invalidated_outer_guard ? NULL : for_saved_guard_base_type;
    }
    hm_infer_stmt_mode(hm, env, s->as.fr.update, self_name, ret, false);
    break;
  }
  case NY_S_TRY:
    /*
     * A try/catch in tail position contributes the value of either arm just
     * like `if` and `match`.  Dropping `ret` here forced untyped functions
     * whose entire body was a try/catch to `any`, which in turn made raw
     * list results cross the dynamic ABI and appear as zero/scalars.
     */
    hm_infer_stmt_mode(hm, env, s->as.tr.body, self_name, ret, allow_tail_expr);
    if (s->as.tr.err) {
      /*
       * A caught panic payload is a tagged runtime value. Give the binding
       * that dynamic type in a handler-local environment so an assignment
       * such as `caught = e` cannot infer a scalar predicate from the try
       * branch's control-flow result.
       */
      ny_hm_env_list handler_env;
      hm_env_clone(hm, &handler_env, env);
      hm_env_set_ctx(&handler_env, s->as.tr.err, s->syntax_ctx, hm_any(hm));
      hm_infer_stmt_mode(hm, &handler_env, s->as.tr.handler, self_name, ret,
                         allow_tail_expr);
      vec_free(&handler_env);
    } else {
      hm_infer_stmt_mode(hm, env, s->as.tr.handler, self_name, ret,
                         allow_tail_expr);
    }
    break;
  case NY_S_DEFER:
    hm_infer_stmt_mode(hm, env, s->as.de.body, self_name, ret, false);
    break;
  case NY_S_GUARD:
    hm_env_set_ctx(
        env, s->as.guard.name, s->syntax_ctx,
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
    hm_validate_match_patterns(hm, s, subject_t);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      ny_hm_env_list arm_env;
      hm_env_clone(hm, &arm_env, env);
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
  ft->parameters = &fn->as.fn.params;
  /*
   * Function type parameters (`fn first<A>(list<A> xs) A`) are erased for
   * the runtime but not for checking: each names one variable of the
   * signature, so a call instantiates it from its arguments and the result
   * has the instance's type (`first([1, 2])` is an `int`).
   */
  bool generic = fn->as.fn.type_params.len > 0 &&
                 fn->as.fn.erased_param_types.len == fn->as.fn.params.len &&
                 hm->tvar_count == 0;
  if (generic) {
    for (size_t i = 0; i < fn->as.fn.type_params.len && hm->tvar_count < 16;
         ++i) {
      hm->tvar_names[hm->tvar_count] = fn->as.fn.type_params.data[i];
      hm->tvar_types[hm->tvar_count] = hm_var(hm);
      hm->tvar_count++;
    }
    /* Level variables are instantiated per call like type parameters. */
    for (size_t i = 0; i < fn->as.fn.level_params.len && hm->tvar_count < 16;
         ++i) {
      hm->tvar_names[hm->tvar_count] = fn->as.fn.level_params.data[i];
      hm->tvar_types[hm->tvar_count] = hm_var(hm);
      hm->tvar_count++;
    }
  }
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    param_t *p = &fn->as.fn.params.data[i];
    const char *type = generic ? fn->as.fn.erased_param_types.data[i]
                               : p->type;
    /* The explicit binder of `fn id(Type A, A x)`: its argument is a type,
     * which instantiates `A` at the call. */
    if (generic && type && strncmp(type, "Type", 4) == 0 && p->name) {
      ny_hm_type_t *bound = NULL;
      for (size_t k = 0; k < hm->tvar_count; ++k)
        if (strcmp(hm->tvar_names[k], p->name) == 0)
          bound = hm->tvar_types[k];
      if (bound) {
        ny_hm_type_t *binder = hm_name(hm, "Type");
        hm_args_push(hm, binder, bound);
        /* Second argument: the binder's universe level, a literal or a
         * level variable (`Type u A`). */
        const char *level = type[4] == ' ' ? type + 5 : "0";
        ny_hm_type_t *level_t = NULL;
        for (size_t k = 0; k < hm->tvar_count; ++k)
          if (strcmp(hm->tvar_names[k], level) == 0)
            level_t = hm->tvar_types[k];
        hm_args_push(hm, binder, level_t ? level_t : hm_name(hm, level));
        hm_args_push(hm, ft, binder);
        continue;
      }
    }
    char *qualified = type ? hm_qualify_owner_type(hm, owner, type) : NULL;
    hm_args_push(hm, ft,
                 type ? hm_parse_type_name(hm, qualified ? qualified : type,
                                           owner)
                      : hm_var(hm));
    free(qualified);
  }
  const char *return_type =
      generic ? fn->as.fn.erased_return_type : fn->as.fn.return_type;
  char *qualified_return =
      return_type ? hm_qualify_owner_type(hm, owner, return_type) : NULL;
  ft->a = return_type
              ? hm_parse_type_name(hm,
                                   qualified_return ? qualified_return
                                                    : return_type,
                                   owner)
              : hm_var(hm);
  free(qualified_return);
  if (generic)
    hm->tvar_count = 0;
  return ft;
}

static void hm_collect_program_schemes(ny_hm_state_t *hm, stmt_t *s,
                                       const char *owner) {
  if (!hm || !s)
    return;
  if (s->kind == NY_S_MODULE) {
    const char *module_owner = s->as.module.name ? s->as.module.name : owner;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      hm_collect_program_schemes(hm, s->as.module.body.data[i], module_owner);
    return;
  }
  if (s->kind != NY_S_FUNC)
    return;
  for (size_t i = 0; i < hm->schemes.len; ++i) {
    if (hm->schemes.data[i].stmt == s)
      return;
  }
  ny_hm_scheme_t scheme = {
      .name = s->as.fn.name,
      .owner = owner,
      .stmt = s,
      .type = hm_make_func_type(hm, s, owner),
  };
  scheme.full_name = hm_full_name(owner, scheme.name);
  vec_push(&hm->schemes, scheme);
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
  if (!hm || !hm->ctx || hm->ctx->funcs.len == 0) {
    for (size_t i = 0;
         hm && hm->ctx && hm->ctx->prog && i < hm->ctx->prog->body.len; ++i)
      hm_collect_program_schemes(hm, hm->ctx->prog->body.data[i], NULL);
  }
  size_t cap = 16;
  while (cap < hm->schemes.len * 4)
    cap *= 2;
  hm->scheme_index = calloc(cap, sizeof(*hm->scheme_index));
  if (hm->scheme_index) {
    hm->scheme_index_cap = cap;
    for (size_t i = 0; i < hm->schemes.len; ++i) {
      hm_scheme_index_put(hm, hm->schemes.data[i].name, i);
      hm_scheme_index_put(hm, hm->schemes.data[i].full_name, i);
    }
  }
}

typedef struct {
  unsigned carriers;
} hm_return_carriers_t;

static void hm_record_return_carrier(hm_return_carriers_t *scan,
                                     const expr_t *value) {
  if (value && !value->semantic.resolved)
    return;
  ny_sem_rep_t rep = value ? value->semantic.rep : NY_SEM_REP_TAGGED_DYNAMIC;
  scan->carriers |= rep == NY_SEM_REP_F32   ? 2u
                    : rep == NY_SEM_REP_F64 ? 4u
                                            : 1u;
}

static bool hm_return_carrier_stmt(ny_visitor_t *v, stmt_t *s) {
  if (s->kind == NY_S_FUNC)
    return false;
  if (s->kind == NY_S_RETURN)
    hm_record_return_carrier(v->ctx, s->as.ret.value);
  return true;
}

static bool hm_return_carrier_expr(ny_visitor_t *v, expr_t *e) {
  (void)v;
  return e->kind != NY_E_FN && e->kind != NY_E_LAMBDA &&
         e->kind != NY_E_COMPTIME;
}

static bool hm_function_has_mixed_return_carriers(stmt_t *fn) {
  hm_return_carriers_t scan = {0};
  ny_visitor_t visitor = {.ctx = &scan,
                          .visit_stmt_pre = hm_return_carrier_stmt,
                          .visit_expr_pre = hm_return_carrier_expr};
  stmt_t *body = fn->as.fn.body;
  ny_visit_stmt(&visitor, body);
  if (body && body->kind == NY_S_BLOCK && body->as.block.body.len)
    body = body->as.block.body.data[body->as.block.body.len - 1];
  if (body && body->kind == NY_S_EXPR)
    hm_record_return_carrier(&scan, body->as.expr.expr);
  return scan.carriers && (scan.carriers & (scan.carriers - 1u));
}

static void hm_infer_function_scheme(ny_hm_state_t *hm, ny_hm_scheme_t *scheme,
                                     ny_hm_env_list *globals,
                                     ny_hm_env_list *env_scratch) {
  if (!hm || !scheme || !scheme->stmt || scheme->stmt->kind != NY_S_FUNC)
    return;
  stmt_t *fn = scheme->stmt;
  if (hm->ctx && !hm->ctx->include_std && fn->as.fn.return_semantic.resolved &&
      (!hm->ctx->collect_semantic_artifact_facts ||
       fn->as.fn.return_semantic.hm_type_name))
    return;
  ny_hm_type_t *ft = hm_prune(scheme->type);
  /*
   * Reachable native compilation already has the checked AST and only needs
   * semantic carrier facts here. Re-running HM over every expression in a
   * fully annotated stdlib function is redundant; derive its ABI facts from
   * the resolved signature and leave body facts to the native expression
   * resolver.
   */
  if (hm->ctx && !hm->ctx->include_std &&
      !hm->ctx->collect_semantic_artifact_facts && ny_is_stdlib_tok(fn->tok) &&
      fn->as.fn.return_type) {
    bool complete_signature = true;
    for (size_t i = 0; i < fn->as.fn.params.len; ++i)
      if (!fn->as.fn.params.data[i].type) {
        complete_signature = false;
        break;
      }
    ny_sem_rep_t *param_reps =
        complete_signature && fn->as.fn.params.len
            ? calloc(fn->as.fn.params.len, sizeof(*param_reps))
            : NULL;
    if (complete_signature && (!fn->as.fn.params.len || param_reps)) {
      for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
        char *name = i < ft->args.len ? hm_type_string(ft->args.data[i]) : NULL;
        param_reps[i] = hm_semantic_rep_in_context(hm, name);
        free(name);
        if (!ny_sem_rep_is_valid(param_reps[i])) {
          complete_signature = false;
          break;
        }
      }
      char *return_name = hm_type_string(ft->a);
      ny_sem_rep_t return_rep = hm_semantic_rep_in_context(hm, return_name);
      free(return_name);
      if (!ny_sem_rep_is_valid(return_rep))
        complete_signature = false;
      if (complete_signature) {
        for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
          fn->as.fn.params.data[i].semantic.rep = param_reps[i];
          fn->as.fn.params.data[i].semantic.resolved = true;
        }
        fn->as.fn.return_semantic.rep = return_rep;
        fn->as.fn.return_semantic.resolved = true;
        fn->as.fn.return_semantic.ownership =
            fn->as.fn.attr_returns_owned
                ? NY_SEM_OWN_OWNED
                : (fn->as.fn.attr_returns_borrow ? NY_SEM_OWN_BORROWED
                                                 : NY_SEM_OWN_UNKNOWN);
        free(param_reps);
        return;
      }
    }
    free(param_reps);
  }
  ny_hm_env_list env = env_scratch ? *env_scratch : (ny_hm_env_list){0};
  if (env_scratch)
    hm_env_clone_reuse(hm, &env, globals);
  else
    hm_env_clone(hm, &env, globals);
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    param_t *p = &fn->as.fn.params.data[i];
    if (i < ft->args.len) {
      /* Inside the body an explicit binder is its erased value, a `str`. */
      ny_hm_type_t *slot = hm_prune(ft->args.data[i]);
      bool binder = slot && slot->kind == NY_HM_NAME && slot->name &&
                    strcmp(slot->name, "Type") == 0 && slot->args.len >= 1;
      hm_env_set_ctx(&env, p->name, fn->syntax_ctx,
                     binder ? hm_name(hm, "str") : ft->args.data[i]);
    }
    if (p->name && p->type) {
      hm_sigma_parts_t sigma_probe;
      if (hm_sigma_parts_parse(p->type, &sigma_probe)) {
        hm_sigma_parts_free(&sigma_probe);
        ny_hm_env_entry_t *sigma_entry =
            hm_env_entry_ctx(&env, p->name, fn->syntax_ctx);
        if (sigma_entry)
          sigma_entry->sigma_decl = p->type;
      }
    }
    hm_env_set_value_facts(hm, &env, p->name, fn->syntax_ctx, false,
                           NY_SEM_OWN_BORROWED, false);
    if (p->def && i < ft->args.len)
      hm_unify(hm, ft->args.data[i],
               hm_infer_expr(hm, &env, p->def, scheme->owner), p->def->tok,
               "default parameter");
  }
  stmt_t *saved_active_fn = hm->active_fn;
  stmt_t *saved_active_fn_body = hm->active_fn_body;
  hm->active_fn = fn;
  hm->active_fn_body = fn->as.fn.body;
  if (fn->as.fn.body && fn->as.fn.body->kind == NY_S_EXPR) {
    expr_t *value = fn->as.fn.body->as.expr.expr;
    ny_hm_type_t *value_t = hm_infer_expr(hm, &env, value, scheme->owner);
    if (value)
      hm_check_refinement_return(hm, ft->a, value_t, value, value->tok);
    hm_unify(hm, ft->a, value_t, value ? value->tok : fn->as.fn.body->tok,
             "implicit return value");
  } else {
    hm_infer_stmt(hm, &env, fn->as.fn.body, scheme->owner, ft->a);
  }
  hm->active_fn_body = saved_active_fn_body;
  hm->active_fn = saved_active_fn;
  hm_validate_ownership(hm, fn);
  for (size_t i = 0; i < fn->as.fn.params.len && i < ft->args.len; ++i) {
    ny_hm_type_t *slot_t = hm_prune(ft->args.data[i]);
    char *name = slot_t && slot_t->kind == NY_HM_NAME && slot_t->name &&
                         strcmp(slot_t->name, "Type") == 0 &&
                         slot_t->args.len >= 1
                     ? ny_strdup("str")
                     : hm_type_string(ft->args.data[i]);
    ny_sem_rep_t r = hm_semantic_rep_in_context(hm, name);
    NY_INVARIANT_ASSERT(
        ny_sem_rep_is_valid(r), "valid semantic rep", fn->tok.filename,
        fn->tok.line, fn->tok.col, fn->as.fn.name ? fn->as.fn.name : "<anon>",
        name, "invalid semantic representation computed for parameter %zu", i);
    fn->as.fn.params.data[i].semantic.rep = r;
    fn->as.fn.params.data[i].semantic.resolved = true;
    if (hm->ctx && hm->ctx->collect_semantic_artifact_facts)
      fn->as.fn.params.data[i].semantic.hm_type_name =
          name ? ny_intern_get(ny_intern_cstr(name)) : NULL;
    if (fn->as.fn.params.data[i].type) {
      NY_INVARIANT_ASSERT(
          ny_sem_rep_coherent_with_type(fn->as.fn.params.data[i].type, r),
          "HM representation coherence", fn->tok.filename, fn->tok.line,
          fn->tok.col, fn->as.fn.name ? fn->as.fn.name : "<anon>",
          ny_sem_rep_name(r),
          "parameter '%s' declared as '%s' but inferred impossible "
          "representation '%s'",
          fn->as.fn.params.data[i].name ? fn->as.fn.params.data[i].name
                                        : "<param>",
          fn->as.fn.params.data[i].type, ny_sem_rep_name(r));
    }
    free(name);
  }
  /*
   * Numeric compatibility does not mean equal calling conventions. Preserve
   * all inferred return values through one tagged ABI when source exits use
   * different carriers, instead of retaining whichever return unified first.
   */
  if (!fn->as.fn.return_type && hm_function_has_mixed_return_carriers(fn))
    ft->a = hm_any(hm);
  char *return_name = hm_type_string(ft->a);
  ny_sem_rep_t ret_r = hm_semantic_rep_in_context(hm, return_name);
  NY_INVARIANT_ASSERT(ny_sem_rep_is_valid(ret_r), "valid semantic rep",
                      fn->tok.filename, fn->tok.line, fn->tok.col,
                      fn->as.fn.name ? fn->as.fn.name : "<anon>", return_name,
                      "invalid semantic representation computed for return");
  fn->as.fn.return_semantic.rep = ret_r;
  fn->as.fn.return_semantic.resolved = true;
  if (hm->ctx && hm->ctx->collect_semantic_artifact_facts)
    fn->as.fn.return_semantic.hm_type_name =
        return_name ? ny_intern_get(ny_intern_cstr(return_name)) : NULL;
  if (fn->as.fn.return_type) {
    NY_INVARIANT_ASSERT(
        ny_sem_rep_coherent_with_type(fn->as.fn.return_type, ret_r),
        "HM representation coherence", fn->tok.filename, fn->tok.line,
        fn->tok.col, fn->as.fn.name ? fn->as.fn.name : "<anon>",
        ny_sem_rep_name(ret_r),
        "function '%s' return declared as '%s' but inferred impossible "
        "representation '%s'",
        fn->as.fn.name ? fn->as.fn.name : "<anon>", fn->as.fn.return_type,
        ny_sem_rep_name(ret_r));
  }
  fn->as.fn.return_semantic.ownership =
      fn->as.fn.attr_returns_owned
          ? NY_SEM_OWN_OWNED
          : (fn->as.fn.attr_returns_borrow ? NY_SEM_OWN_BORROWED
                                           : NY_SEM_OWN_UNKNOWN);
  free(return_name);
  hm_call_arg_fact_ctx_t completion = {
      .hm = hm, .env = &env, .owner = scheme->owner};
  ny_visitor_t completion_visitor = {.ctx = &completion,
                                     .visit_expr_post = hm_complete_leaf_fact};
  ny_visit_stmt(&completion_visitor, fn->as.fn.body);
  if (env_scratch)
    *env_scratch = env;
  else
    vec_free(&env);
}

/*
 * Report every named hole with the type inference settled on.  A hole is a
 * goal display, never a value: reporting it as an error keeps programs with
 * holes from running.
 */
static void hm_report_holes(ny_hm_state_t *hm) {
  for (size_t h = 0; hm && h < hm->hole_count; ++h) {
    const expr_t *e = hm->holes[h].expr;
    char *ty = hm_type_string(hm_prune(hm->holes[h].type));
    /* An unsolved type variable prints as 'mN: nothing constrained it. */
    const char *shown = ty && ty[0] && ty[0] != '\'' ? ty : NULL;
    tp_add_diag_ex(hm->ctx, e->tok, "hm", "hm-unsolved-hole", "hole",
                   shown ? shown : "unconstrained", NULL, NULL,
                   "a named hole is a placeholder whose type inference "
                   "reports; it cannot be compiled or run",
                   "replace the hole with an expression of the reported type",
                   "unsolved hole '%s' : %s", e->as.ident.name,
                   shown ? shown : "unconstrained");
    free(ty);
  }
  if (hm)
    hm->hole_count = 0;
}

static void hm_run(ny_hm_state_t *hm, ny_tp_ctx_t *ctx) {
  bool trace_timing = ny_env_enabled("NYTRIX_TRACE_TYPE_PIPELINE_TIMING");
  ny_tick_t stage_start = trace_timing ? ny_ticks_now() : 0;
#define NY_HM_STAGE_TIMED(label)                                               \
  do {                                                                         \
    if (trace_timing) {                                                        \
      ny_tick_t stage_end = ny_ticks_now();                                    \
      fprintf(stderr, "[type-hm-timing] stage=%s elapsed=%.3f s\n", label,     \
              ny_ticks_elapsed_sec(stage_start));                              \
      stage_start = stage_end;                                                 \
    }                                                                          \
  } while (0)
  memset(hm, 0, sizeof(*hm));
  hm->ctx = ctx;
  hm->trace_type_detail = ny_dbg_is_enabled(NY_DBG_TYPE_DETAIL);
  hm->trace_deep = ny_dbg_is_enabled(NY_DBG_DEEP);
  vec_init(&hm->schemes);
  hm_collect_function_schemes(hm);
  NY_HM_STAGE_TIMED("scheme-collection");
  ny_hm_env_list env;
  hm_env_init(hm, &env);
  ny_hm_env_list function_env_scratch = {0};
  size_t first_pass_function_inferences = 0;
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i) {
    stmt_t *s = ctx->prog->body.data[i];
    if (s && s->kind == NY_S_FUNC) {
      ny_hm_scheme_t *scheme = hm_find_scheme(hm, s->as.fn.name);
      if (scheme) {
        hm_infer_function_scheme(hm, scheme, &env, &function_env_scratch);
        first_pass_function_inferences++;
      }
    } else if (s && s->kind != NY_S_IMPL && s->kind != NY_S_STRUCT &&
               s->kind != NY_S_LAYOUT) {
      hm_infer_stmt(hm, &env, s, NULL, NULL);
    }
  }
  NY_HM_STAGE_TIMED("top-level-sweep");
  for (size_t i = 0; i < hm->schemes.len; ++i)
    hm_infer_function_scheme(hm, &hm->schemes.data[i], &env,
                             &function_env_scratch);
  NY_HM_STAGE_TIMED("all-scheme-inference");

  /*
   * The native reachability pass may later select a function that was not in
   * the initial function-fact set.  Complete structurally decidable leaves
   * across the program while the module-global environment is still alive.
   * Lowercase identifiers now resolve when a global of that name is visible;
   * identifiers shadowed by an enclosing parameter/local are excluded via
   * the scope stack, and genuinely unknown locals remain unresolved errors
   * instead of being guessed.  Imported constants — including lowercase and
   * stdlib globals — receive the representation established above.
   */
  hm_call_arg_fact_ctx_t completion = {.hm = hm, .env = &env, .owner = NULL};
  ny_visitor_t completion_visitor = {
      .ctx = &completion,
      .visit_expr_pre = hm_completion_visitor_expr_pre,
      .visit_expr_post = hm_completion_visitor_expr_post,
      .visit_stmt_pre = hm_completion_visitor_stmt_pre,
      .visit_stmt_post = hm_completion_visitor_stmt_post};
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i) {
    hm_completion_scope_mark(&completion);
    hm_visit_semantic_completion(&completion_visitor, ctx->prog->body.data[i]);
    hm_completion_scope_pop(&completion);
  }
  NY_HM_STAGE_TIMED("semantic-completion");
  hm_completion_scope_reset(&completion);
  if (ctx && ctx->collect_semantic_artifact_facts)
    hm_finalize_expr_type_facts(hm);
  NY_HM_STAGE_TIMED("expression-fact-finalization");
  if (trace_timing)
    fprintf(stderr,
            "[type-hm-timing] functions=%zu first-pass-inferences=%zu\n",
            hm->schemes.len, first_pass_function_inferences);
  vec_free(&function_env_scratch);
  vec_free(&env);
  hm_report_holes(hm);
  if (hm->debug_std_default_constraints_suppressed > 0) {
    char summary[192];
    snprintf(summary, sizeof(summary),
             "stdlib default-parameter constraints summarized=%zu",
             hm->debug_std_default_constraints_suppressed);
    ny_dbg_type_detail("HM_SUMMARY", summary);
  }
#undef NY_HM_STAGE_TIMED
}

static void hm_run_reachable(ny_hm_state_t *hm, ny_tp_ctx_t *ctx,
                             stmt_t *const *targets, size_t target_count) {
  memset(hm, 0, sizeof(*hm));
  hm->ctx = ctx;
  hm->trace_type_detail = ny_dbg_is_enabled(NY_DBG_TYPE_DETAIL);
  hm->trace_deep = ny_dbg_is_enabled(NY_DBG_DEEP);
  if (ny_env_enabled("NY_TRACE_NATIVE_REACHABLE")) {
    size_t resolved = 0, typed_signatures = 0;
    for (size_t i = 0; i < target_count; ++i)
      if (targets && targets[i] && targets[i]->kind == NY_S_FUNC) {
        const stmt_func_t *fn = &targets[i]->as.fn;
        if (fn->return_semantic.resolved)
          ++resolved;
        bool complete = fn->return_type != NULL;
        for (size_t p = 0; complete && p < fn->params.len; ++p)
          complete = fn->params.data[p].type != NULL;
        if (complete)
          ++typed_signatures;
      }
    fprintf(stderr,
            "native reachable HM input: %zu functions (%zu resolved, "
            "%zu fully typed signatures)\n",
            target_count, resolved, typed_signatures);
  }
  vec_init(&hm->schemes);
  size_t *target_scheme_indices =
      target_count ? malloc(target_count * sizeof(*target_scheme_indices))
                   : NULL;
  if (target_scheme_indices)
    for (size_t i = 0; i < target_count; ++i)
      target_scheme_indices[i] = SIZE_MAX;
  size_t cap = 16;
  while (cap < target_count * 4)
    cap *= 2;
  hm->scheme_index = calloc(cap, sizeof(*hm->scheme_index));
  if (hm->scheme_index)
    hm->scheme_index_cap = cap;

  for (size_t i = 0; i < target_count; ++i) {
    stmt_t *fn = targets ? targets[i] : NULL;
    if (!fn || fn->kind != NY_S_FUNC)
      continue;
    const char *owner = NULL;
    const char *name = fn->as.fn.name;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot && dot != name) {
      char *qualified_owner = hm_arena_strdup(hm, name);
      if (qualified_owner) {
        qualified_owner[dot - name] = '\0';
        owner = qualified_owner;
        name = dot + 1;
      }
    }
    ny_hm_scheme_t added = {
        .name = name,
        .owner = owner,
        .stmt = fn,
        .type = hm_make_func_type(hm, fn, owner),
    };
    added.full_name = hm_full_name(owner, name);
    vec_push(&hm->schemes, added);
    size_t idx = hm->schemes.len - 1;
    if (target_scheme_indices)
      target_scheme_indices[i] = idx;
    hm_scheme_index_put(hm, added.name, idx);
    hm_scheme_index_put(hm, added.full_name, idx);
  }
  if (ny_env_enabled("NY_TRACE_NATIVE_REACHABLE")) {
    fprintf(stderr, "native reachable HM schemes built: %zu\n",
            hm->schemes.len);
    fflush(stderr);
  }

  ny_hm_env_list env;
  hm_env_init(hm, &env);
  ny_hm_env_list function_env_scratch = {0};
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i) {
    stmt_t *s = ctx->prog->body.data[i];
    if (s && s->kind == NY_S_VAR && !ny_is_stdlib_tok(s->tok))
      hm_infer_stmt(hm, &env, s, NULL, NULL);
  }

  for (size_t i = 0; i < target_count; ++i) {
    stmt_t *fn = targets ? targets[i] : NULL;
    if (!fn || fn->kind != NY_S_FUNC)
      continue;
    ny_hm_scheme_t *scheme =
        target_scheme_indices && target_scheme_indices[i] != SIZE_MAX
            ? &hm->schemes.data[target_scheme_indices[i]]
            : NULL;
    if (!scheme || scheme->stmt != fn) {
      scheme = hm_find_scheme(hm, fn->as.fn.name);
    }
    if (!scheme || scheme->stmt != fn) {
      for (size_t j = hm->schemes.len; j > 0; --j) {
        if (hm->schemes.data[j - 1].stmt == fn) {
          scheme = &hm->schemes.data[j - 1];
          break;
        }
      }
    }
    if (!scheme) {
      const char *owner = NULL;
      const char *name = fn->as.fn.name;
      const char *dot = name ? strrchr(name, '.') : NULL;
      if (dot && dot != name) {
        char *qualified_owner = hm_arena_strdup(hm, name);
        if (qualified_owner) {
          qualified_owner[dot - name] = '\0';
          owner = qualified_owner;
          name = dot + 1;
        }
      }
      ny_hm_scheme_t added = {
          .name = name,
          .owner = owner,
          .stmt = fn,
          .type = hm_make_func_type(hm, fn, owner),
      };
      added.full_name = hm_full_name(owner, name);
      vec_push(&hm->schemes, added);
      scheme = &hm->schemes.data[hm->schemes.len - 1];
      hm_scheme_index_put(hm, scheme->name, hm->schemes.len - 1);
      hm_scheme_index_put(hm, scheme->full_name, hm->schemes.len - 1);
    }
    hm_infer_function_scheme(hm, scheme, &env, &function_env_scratch);
  }
  if (ny_env_enabled("NY_TRACE_NATIVE_REACHABLE")) {
    fprintf(stderr, "native reachable HM inference complete\n");
    fflush(stderr);
  }
  free(target_scheme_indices);
  vec_free(&function_env_scratch);
  if (ctx && ctx->collect_semantic_artifact_facts)
    hm_finalize_expr_type_facts(hm);
  vec_free(&env);
  hm_report_holes(hm);
}

static void hm_dispose(ny_hm_state_t *hm) {
  if (!hm)
    return;
  for (size_t i = 0; i < hm->schemes.len; ++i)
    free(hm->schemes.data[i].full_name);
  vec_free(&hm->schemes);
  vec_free(&hm->expr_type_facts);
  free(hm->scheme_index);
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
