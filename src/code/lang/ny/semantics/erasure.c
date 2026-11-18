/*
 * Proof erasure: removes `proof<P>` parameters, and the matching call
 * arguments, from functions before lowering.
 *
 * Runs after the kernel (HM, totality, contract checks) has accepted the
 * program, so every witness has already been checked against its
 * proposition. The pass then removes what is computationally irrelevant:
 * an erased parameter takes no ABI slot and an erased argument is never
 * passed. Representation rules (docs/spec/kernel.md, "Erasure and the ABI
 * model"):
 *
 *  - Only `proof<...>` parameters erase. Erasing never reorders or coalesces
 *    the remaining runtime parameters.
 *  - A function erases only when every use of it is a direct, positional
 *    call: a function used as a value, exported, extern, or carrying a
 *    contract that may name the parameter keeps its full signature.
 *  - Inside the body an erased parameter may only be forwarded to another
 *    erased slot; any other use (equality rules, printing, returning it)
 *    keeps that parameter.
 *  - An erased argument is an identifier, a literal, or a `prove(...)`
 *    witness. A `prove` witness is still checked: it is hoisted to an
 *    expression statement before its statement, where lowering discharges it
 *    exactly as before, then discarded. Positions where hoisting could change
 *    scope (match arms, lambdas, loop headers) keep the parameter instead.
 *
 * Only functions declared at the top level of the main source file erase, so
 * separately compiled and cached modules never see a changed signature.
 */
#include "code/lang/ny/gen/internal.h"
#include "code/lang/ny/parser/ast.h"
#include "code/lang/ny/parser/visitor.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERASE_MAX_FNS 256
#define ERASE_MAX_PARAMS 32

typedef struct {
  stmt_t *fn;
  bool ok;
  bool erase[ERASE_MAX_PARAMS];
} erase_fn_t;

typedef struct {
  erase_fn_t fns[ERASE_MAX_FNS];
  size_t fn_count;
  /*
   * Identifiers in an allowed position: a direct callee, or an argument in
   * an erased slot. Rebuilt on every checking round.
   */
  const expr_t **allowed;
  size_t allowed_len, allowed_cap;
  erase_fn_t *current[64];
  size_t current_depth;
  int unsafe;
  bool rewrite;
  bool changed;
  arena_t *arena;
  /*
   * Other files cannot name main-file functions; their identifiers are
   * unrelated locals.
   */
  const char *main_file;
  /*
   * Witnesses to hoist before the statement being rewritten.
   */
  expr_t **hoist;
  size_t hoist_len, hoist_cap;
} erase_ctx_t;

static bool erase_is_proof_type(const char *type) {
  return type && strncmp(type, "proof<", 6) == 0;
}

static erase_fn_t *erase_find(erase_ctx_t *c, const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < c->fn_count; ++i)
    if (strcmp(c->fns[i].fn->as.fn.name, name) == 0)
      return &c->fns[i];
  return NULL;
}

static bool erase_any(const erase_fn_t *f) {
  for (size_t i = 0; i < f->fn->as.fn.params.len && i < ERASE_MAX_PARAMS; ++i)
    if (f->erase[i])
      return true;
  return false;
}

static void erase_disable(erase_ctx_t *c, erase_fn_t *f, const char *why) {
  if (!f->ok)
    return;
  if (getenv("NYTRIX_TRACE_ERASURE"))
    fprintf(stderr, "[erasure] keep %s: %s\n", f->fn->as.fn.name, why);
  f->ok = false;
  memset(f->erase, 0, sizeof(f->erase));
  c->changed = true;
}

static void erase_allow(erase_ctx_t *c, const expr_t *e) {
  if (c->allowed_len == c->allowed_cap) {
    size_t cap = c->allowed_cap ? c->allowed_cap * 2 : 64;
    const expr_t **grown = realloc(c->allowed, cap * sizeof(*grown));
    if (!grown)
      return;
    c->allowed = grown;
    c->allowed_cap = cap;
  }
  c->allowed[c->allowed_len++] = e;
}

static bool erase_is_allowed(const erase_ctx_t *c, const expr_t *e) {
  for (size_t i = 0; i < c->allowed_len; ++i)
    if (c->allowed[i] == e)
      return true;
  return false;
}

static bool erase_is_prove_call(const expr_t *e) {
  return e && e->kind == NY_E_CALL && e->as.call.callee &&
         e->as.call.callee->kind == NY_E_IDENT &&
         e->as.call.callee->as.ident.name &&
         strcmp(e->as.call.callee->as.ident.name, "prove") == 0;
}

static bool erase_has_call_pre(ny_visitor_t *v, expr_t *e) {
  if (e && (e->kind == NY_E_CALL || e->kind == NY_E_MEMCALL ||
            e->kind == NY_E_LAMBDA || e->kind == NY_E_FN))
    *(bool *)v->ctx = true;
  return true;
}

/*
 * A `prove(...)` witness whose condition calls nothing: hoisting it cannot
 * move a call to an erased function out of the rewrite.
 */
static bool erase_prove_is_simple(const expr_t *e) {
  if (!erase_is_prove_call(e))
    return false;
  bool has_call = false;
  ny_visitor_t v = {.ctx = &has_call, .visit_expr_pre = erase_has_call_pre};
  for (size_t i = 0; i < e->as.call.args.len; ++i)
    ny_visit_expr(&v, e->as.call.args.data[i].val);
  return !has_call;
}

static void erase_push_hoist(erase_ctx_t *c, expr_t *e) {
  if (c->hoist_len == c->hoist_cap) {
    size_t cap = c->hoist_cap ? c->hoist_cap * 2 : 8;
    expr_t **grown = realloc(c->hoist, cap * sizeof(*grown));
    if (!grown)
      return;
    c->hoist = grown;
    c->hoist_cap = cap;
  }
  c->hoist[c->hoist_len++] = e;
}

static void erase_call(erase_ctx_t *c, expr_t *e) {
  expr_t *callee = e->as.call.callee;
  if (!callee || callee->kind != NY_E_IDENT)
    return;
  erase_fn_t *f = erase_find(c, callee->as.ident.name);
  if (!f)
    return;
  erase_allow(c, callee);
  if (!f->ok)
    return;
  const ny_param_list *params = &f->fn->as.fn.params;
  bool positional = e->as.call.args.len == params->len;
  for (size_t i = 0; positional && i < e->as.call.args.len; ++i)
    positional = e->as.call.args.data[i].name == NULL;
  if (!positional) {
    erase_disable(c, f, "a call is not direct and positional");
    return;
  }
  for (size_t i = 0; i < params->len; ++i) {
    if (!f->erase[i])
      continue;
    expr_t *arg = e->as.call.args.data[i].val;
    if (arg && (arg->kind == NY_E_IDENT || arg->kind == NY_E_LITERAL)) {
      erase_allow(c, arg);
      continue;
    }
    if (erase_prove_is_simple(arg) && c->unsafe == 0)
      continue;
    /*
     * Anything else might compute: keep this slot.
     */
    f->erase[i] = false;
    c->changed = true;
  }
  if (!c->rewrite)
    return;
  size_t kept = 0;
  for (size_t i = 0; i < params->len; ++i) {
    if (f->erase[i]) {
      expr_t *arg = e->as.call.args.data[i].val;
      if (erase_is_prove_call(arg))
        erase_push_hoist(c, arg);
      continue;
    }
    e->as.call.args.data[kept++] = e->as.call.args.data[i];
  }
  e->as.call.args.len = kept;
}

static bool erase_expr_pre(ny_visitor_t *v, expr_t *e) {
  erase_ctx_t *c = v->ctx;
  if (!e)
    return false;
  if (c->main_file && e->tok.filename &&
      strcmp(e->tok.filename, c->main_file) != 0)
    return false;
  switch (e->kind) {
  case NY_E_CALL:
    erase_call(c, e);
    break;
  case NY_E_MATCH:
  case NY_E_LAMBDA:
  case NY_E_FN:
  case NY_E_COMPTIME:
  case NY_E_QUOTE:
    c->unsafe++;
    break;
  case NY_E_IDENT: {
    if (c->rewrite || erase_is_allowed(c, e) || !e->as.ident.name)
      break;
    erase_fn_t *f = erase_find(c, e->as.ident.name);
    if (f) { /* used as a value */
      if (getenv("NYTRIX_TRACE_ERASURE"))
        fprintf(stderr, "[erasure] value use at %s:%d:%d\n",
                e->tok.filename ? e->tok.filename : "?", e->tok.line,
                e->tok.col);
      erase_disable(c, f, "used as a value");
    }
    for (size_t d = 0; d < c->current_depth; ++d) {
      erase_fn_t *cur = c->current[d];
      if (!cur || !cur->ok)
        continue;
      const ny_param_list *ps = &cur->fn->as.fn.params;
      for (size_t i = 0; i < ps->len && i < ERASE_MAX_PARAMS; ++i)
        if (cur->erase[i] && ps->data[i].name &&
            strcmp(ps->data[i].name, e->as.ident.name) == 0) {
          cur->erase[i] = false;
          c->changed = true;
        }
    }
    break;
  }
  default:
    break;
  }
  return true;
}

static void erase_expr_post(ny_visitor_t *v, expr_t *e) {
  erase_ctx_t *c = v->ctx;
  if (!e)
    return;
  if (c->main_file && e->tok.filename &&
      strcmp(e->tok.filename, c->main_file) != 0)
    return; /* the pre hook skipped it too */
  switch (e->kind) {
  case NY_E_MATCH:
  case NY_E_LAMBDA:
  case NY_E_FN:
  case NY_E_COMPTIME:
  case NY_E_QUOTE:
    c->unsafe--;
    break;
  default:
    break;
  }
}

static void erase_list(erase_ctx_t *c, ny_stmt_list *list);

static bool erase_stmt_pre(ny_visitor_t *v, stmt_t *s) {
  erase_ctx_t *c = v->ctx;
  if (!s)
    return false;
  if (s->kind == NY_S_BLOCK) {
    int saved = c->unsafe;
    size_t saved_hoist = c->hoist_len;
    c->unsafe = 0;
    erase_list(c, &s->as.block.body);
    c->unsafe = saved;
    c->hoist_len = saved_hoist;
    return false;
  }
  if (s->kind == NY_S_MODULE) {
    erase_list(c, &s->as.module.body);
    return false;
  }
  switch (s->kind) {
  case NY_S_FUNC:
    if (c->current_depth < 64)
      c->current[c->current_depth] = erase_find(c, s->as.fn.name);
    if (c->current[c->current_depth] &&
        c->current[c->current_depth]->fn != s)
      c->current[c->current_depth] = NULL;
    c->current_depth++;
    /*
     * Signature contracts are not rewritten; their expressions are
     * ordinary uses.
     */
    c->unsafe++;
    break;
  case NY_S_MATCH:
  case NY_S_FOR:
  case NY_S_TRY:
  case NY_S_MACRO:
  case NY_S_DEFER:
    c->unsafe++;
    break;
  default:
    break;
  }
  return true;
}

static void erase_stmt_post(ny_visitor_t *v, stmt_t *s) {
  erase_ctx_t *c = v->ctx;
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_FUNC:
    c->current_depth--;
    c->unsafe--;
    break;
  case NY_S_MATCH:
  case NY_S_FOR:
  case NY_S_TRY:
  case NY_S_MACRO:
  case NY_S_DEFER:
    c->unsafe--;
    break;
  default:
    break;
  }
}

static void erase_list(erase_ctx_t *c, ny_stmt_list *list) {
  ny_visitor_t v = {.ctx = c,
                    .visit_expr_pre = erase_expr_pre,
                    .visit_expr_post = erase_expr_post,
                    .visit_stmt_pre = erase_stmt_pre,
                    .visit_stmt_post = erase_stmt_post};
  ny_stmt_list out = {0};
  for (size_t i = 0; i < list->len; ++i) {
    stmt_t *s = list->data[i];
    size_t mark = c->hoist_len;
    if (s && s->kind == NY_S_FUNC) {
      /*
       * A function body opens a new hoisting scope at its block.
       */
      int saved = c->unsafe;
      c->unsafe = 0;
      ny_visit_stmt(&v, s);
      c->unsafe = saved;
    } else {
      ny_visit_stmt(&v, s);
    }
    if (!c->rewrite)
      continue;
    for (size_t h = mark; h < c->hoist_len; ++h) {
      stmt_t *w = arena_alloc(c->arena, sizeof(*w));
      if (!w)
        continue;
      memset(w, 0, sizeof(*w));
      w->kind = NY_S_EXPR;
      w->tok = c->hoist[h]->tok;
      w->syntax_ctx = s ? s->syntax_ctx : 0;
      w->as.expr.expr = c->hoist[h];
      vec_push_arena(c->arena, &out, w);
    }
    c->hoist_len = mark;
    vec_push_arena(c->arena, &out, s);
  }
  if (c->rewrite && out.len != list->len)
    *list = out;
}

static bool erase_fn_eligible(const stmt_t *s, const char *main_file) {
  const stmt_func_t *fn = &s->as.fn;
  if (!fn->name || strchr(fn->name, '.') || !fn->body || fn->is_extern ||
      fn->link_name || fn->is_variadic || fn->requires_cond ||
      fn->attr_proves || fn->dependent_return_param || fn->attr_naked ||
      fn->attr_jit || fn->attr_sys || fn->attr_accel || fn->attr_consteval ||
      fn->attr_cache || fn->attr_async_effects || fn->decreases_expr ||
      fn->params.len > ERASE_MAX_PARAMS)
    return false;
  if (main_file && s->tok.filename && strcmp(s->tok.filename, main_file) != 0)
    return false;
  for (size_t i = 0; i < fn->params.len; ++i)
    if (erase_is_proof_type(fn->params.data[i].type))
      return true;
  return false;
}

size_t ny_erase_proof_params(program_t *prog, arena_t *arena,
                             const char *main_file) {
  if (!prog || !arena || getenv("NYTRIX_NO_PROOF_ERASURE"))
    return 0;
  erase_ctx_t *c = calloc(1, sizeof(*c));
  if (!c)
    return 0;
  c->arena = arena;
  c->main_file = main_file;
  for (size_t i = 0; i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC || !erase_fn_eligible(s, main_file))
      continue;
    erase_fn_t *dup = erase_find(c, s->as.fn.name);
    if (dup) {
      dup->ok = false;
      memset(dup->erase, 0, sizeof(dup->erase));
      continue;
    }
    if (c->fn_count == ERASE_MAX_FNS)
      break;
    erase_fn_t *f = &c->fns[c->fn_count++];
    f->fn = s;
    f->ok = true;
    for (size_t k = 0; k < s->as.fn.params.len; ++k)
      f->erase[k] = erase_is_proof_type(s->as.fn.params.data[k].type);
  }
  /*
   * Any function sharing a name with a candidate anywhere else (modules,
   * nested definitions) makes calls ambiguous: keep it whole.
   */
  size_t erased = 0;
  if (c->fn_count) {
    for (int round = 0; round < 64; ++round) {
      c->changed = false;
      c->allowed_len = 0;
      erase_list(c, &prog->body);
      bool any = false;
      for (size_t i = 0; i < c->fn_count; ++i) {
        if (c->fns[i].ok && !erase_any(&c->fns[i])) {
          c->fns[i].ok = false;
          c->changed = true;
        }
        any |= c->fns[i].ok;
      }
      if (!any || !c->changed)
        break;
    }
    c->rewrite = true;
    c->allowed_len = 0;
    erase_list(c, &prog->body);
    for (size_t i = 0; i < c->fn_count; ++i) {
      erase_fn_t *f = &c->fns[i];
      if (!f->ok)
        continue;
      ny_param_list *ps = &f->fn->as.fn.params;
      ny_type_param_list *erased_types = &f->fn->as.fn.erased_param_types;
      bool aligned = erased_types->len == ps->len;
      size_t kept = 0;
      for (size_t k = 0; k < ps->len; ++k) {
        if (f->erase[k]) {
          erased++;
          continue;
        }
        if (aligned)
          erased_types->data[kept] = erased_types->data[k];
        ps->data[kept++] = ps->data[k];
      }
      ps->len = kept;
      if (aligned)
        erased_types->len = kept;
      if (getenv("NYTRIX_TRACE_ERASURE"))
        fprintf(stderr, "[erasure] %s: %zu runtime parameter(s) kept\n",
                f->fn->as.fn.name, kept);
    }
  }
  free(c->allowed);
  free(c->hoist);
  free(c);
  return erased;
}

/*
 * Representation rule 3: an FFI boundary sees only runtime-representable
 * types. A proof, a type value, or an implicit binder at an `extern` has no
 * C counterpart, so the declaration is rejected instead of silently passing
 * a digest or a type name.
 */
static bool erase_type_is_static_only(const char *type) {
  if (!type)
    return false;
  while (*type == '?' || *type == '*')
    type++;
  if (strncmp(type, "proof<", 6) == 0)
    return true;
  return strncmp(type, "Type", 4) == 0 && (type[4] == '\0' || type[4] == ' ');
}

static bool erase_check_extern_params(const ny_param_list *params,
                                      const char *ret, token_t tok,
                                      const char *name) {
  bool ok = true;
  for (size_t i = 0; i < params->len; ++i) {
    const param_t *p = &params->data[i];
    if (!p->implicit && !erase_type_is_static_only(p->type))
      continue;
    ny_diag_error(p->tok.lexeme ? p->tok : tok,
                  "extern '%s' takes an erased parameter '%s' (%s): proofs, "
                  "type values, and implicit binders have no runtime "
                  "representation at an FFI boundary",
                  name ? name : "?", p->name ? p->name : "_",
                  p->type ? p->type : "implicit");
    ok = false;
  }
  if (erase_type_is_static_only(ret)) {
    ny_diag_error(tok,
                  "extern '%s' returns an erased type (%s): proofs and type "
                  "values have no runtime representation at an FFI boundary",
                  name ? name : "?", ret);
    ok = false;
  }
  return ok;
}

static bool erase_check_stmt_list(const ny_stmt_list *list) {
  bool ok = true;
  for (size_t i = 0; i < list->len; ++i) {
    const stmt_t *s = list->data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_EXTERN)
      ok &= erase_check_extern_params(&s->as.ext.params, s->as.ext.return_type,
                                      s->tok, s->as.ext.name);
    else if (s->kind == NY_S_FUNC && s->as.fn.is_extern)
      ok &= erase_check_extern_params(&s->as.fn.params, s->as.fn.return_type,
                                      s->tok, s->as.fn.name);
    else if (s->kind == NY_S_MODULE)
      ok &= erase_check_stmt_list(&s->as.module.body);
  }
  return ok;
}

bool ny_check_erased_representations(const program_t *prog) {
  return !prog || erase_check_stmt_list(&prog->body);
}
