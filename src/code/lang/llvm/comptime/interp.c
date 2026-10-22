#include "code/lang/code.h"
#include "code/lang/priv.h"
#include "base/util.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ny_ct_interp_var_t {
  const char *name;
  ny_ct_fast_val_t value;
} ny_ct_interp_var_t;

typedef struct ny_ct_interp_ctx_t {
  codegen_t *cg;
  program_t *prog;
  int64_t *local_fuel;
  uint64_t *rng_state;
  ny_ct_interp_var_t *vars;
  size_t len;
  size_t cap;
  bool did_break;
  bool did_continue;
} ny_ct_interp_ctx_t;

static bool ny_ct_interp_step(ny_ct_interp_ctx_t *ctx) {
  if (!ctx)
    return false;
  if (ctx->cg)
    return ny_ct_fuel_consume(ctx->cg);
  if (!ctx->local_fuel || *ctx->local_fuel <= 0)
    return false;
  --*ctx->local_fuel;
  return true;
}

static void ny_ct_interp_ctx_free(ny_ct_interp_ctx_t *ctx) {
  if (!ctx)
    return;
  for (size_t i = 0; i < ctx->len; i++)
    ny_ct_fast_val_free(&ctx->vars[i].value);
  free(ctx->vars);
  ctx->vars = NULL;
  ctx->len = 0;
  ctx->cap = 0;
}

static bool ny_ct_interp_ctx_clone(const ny_ct_interp_ctx_t *src,
                                   ny_ct_interp_ctx_t *dst) {
  if (!src || !dst)
    return false;
  memset(dst, 0, sizeof(*dst));
  dst->cg = src->cg;
  dst->prog = src->prog;
  dst->local_fuel = src->local_fuel;
  dst->rng_state = src->rng_state;
  if (src->len == 0)
    return true;
  dst->vars = calloc(src->len, sizeof(*dst->vars));
  if (!dst->vars)
    return false;
  dst->cap = src->len;
  for (size_t i = 0; i < src->len; i++) {
    dst->vars[i].name = src->vars[i].name;
    if (!ny_ct_fast_val_clone(&src->vars[i].value, &dst->vars[i].value)) {
      dst->len = i;
      ny_ct_interp_ctx_free(dst);
      return false;
    }
    dst->len++;
  }
  return true;
}

static bool ny_ct_interp_get(ny_ct_interp_ctx_t *ctx, const char *name,
                             ny_ct_fast_val_t *out) {
  if (!ctx || !name || !*name || !out)
    return false;
  for (size_t i = ctx->len; i > 0; --i) {
    ny_ct_interp_var_t *v = &ctx->vars[i - 1];
    if (v->name && strcmp(v->name, name) == 0)
      return ny_ct_fast_val_clone(&v->value, out);
  }
  return false;
}

static bool ny_ct_interp_set(ny_ct_interp_ctx_t *ctx, const char *name,
                             ny_ct_fast_val_t value) {
  if (!ctx || !name || !*name)
    return false;
  ny_ct_fast_val_t copy = ny_ct_fast_none();
  if (!ny_ct_fast_val_clone(&value, &copy))
    return false;
  for (size_t i = ctx->len; i > 0; --i) {
    ny_ct_interp_var_t *v = &ctx->vars[i - 1];
    if (v->name && strcmp(v->name, name) == 0) {
      ny_ct_fast_val_free(&v->value);
      v->value = copy;
      return true;
    }
  }
  if (ctx->len == ctx->cap) {
    size_t next_cap = ctx->cap ? (ctx->cap * 2) : 16;
    ny_ct_interp_var_t *grown =
        realloc(ctx->vars, sizeof(*ctx->vars) * next_cap);
    if (!grown) {
      ny_ct_fast_val_free(&copy);
      return false;
    }
    ctx->vars = grown;
    ctx->cap = next_cap;
  }
  ctx->vars[ctx->len++] = (ny_ct_interp_var_t){.name = name, .value = copy};
  return true;
}

static bool ny_ct_interp_define(ny_ct_interp_ctx_t *ctx, const char *name,
                                ny_ct_fast_val_t value) {
  if (!ctx || !name || !*name)
    return false;
  ny_ct_fast_val_t copy = ny_ct_fast_none();
  if (!ny_ct_fast_val_clone(&value, &copy))
    return false;
  if (ctx->len == ctx->cap) {
    size_t next_cap = ctx->cap ? (ctx->cap * 2) : 16;
    ny_ct_interp_var_t *grown =
        realloc(ctx->vars, sizeof(*ctx->vars) * next_cap);
    if (!grown) {
      ny_ct_fast_val_free(&copy);
      return false;
    }
    ctx->vars = grown;
    ctx->cap = next_cap;
  }
  ctx->vars[ctx->len++] = (ny_ct_interp_var_t){.name = name, .value = copy};
  return true;
}

static void ny_ct_interp_pop_to(ny_ct_interp_ctx_t *ctx, size_t len) {
  if (!ctx || len > ctx->len)
    return;
  while (ctx->len > len)
    ny_ct_fast_val_free(&ctx->vars[--ctx->len].value);
}

static bool ny_ct_interp_eval_expr(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth);
static bool ny_ct_interp_eval_stmt(stmt_t *s, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *ret, bool *did_return,
                                   int depth);

static bool ny_ct_interp_match_arm(const match_arm_t *arm,
                                   const ny_ct_fast_val_t *test,
                                   ny_ct_interp_ctx_t *ctx, bool *matched,
                                   int depth) {
  if (!arm || !test || !ctx || !matched)
    return false;
  *matched = false;
  for (size_t i = 0; i < arm->patterns.len; i++) {
    expr_t *pattern = arm->patterns.data[i];
    if (!pattern)
      return false;
    if (pattern->kind == NY_E_IDENT && pattern->as.ident.name &&
        strcmp(pattern->as.ident.name, "_") == 0) {
      *matched = true;
      break;
    }
    ny_ct_fast_val_t pattern_value = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(pattern, ctx, &pattern_value, depth + 1)) {
      ny_ct_fast_val_free(&pattern_value);
      return false;
    }
    ny_ct_fast_val_t equal = ny_ct_fast_none();
    bool ok = ny_ct_fast_eval_binary("==", test, &pattern_value, &equal);
    ny_ct_fast_val_free(&pattern_value);
    if (!ok) {
      ny_ct_fast_val_free(&equal);
      return false;
    }
    *matched = equal.kind == NY_CT_FAST_BOOL && equal.b;
    ny_ct_fast_val_free(&equal);
    if (*matched)
      break;
  }
  if (!*matched || !arm->guard)
    return true;
  ny_ct_fast_val_t guard = ny_ct_fast_none();
  bool truthy = false;
  bool ok = ny_ct_interp_eval_expr(arm->guard, ctx, &guard, depth + 1) &&
            ny_ct_fast_truthy(&guard, &truthy);
  ny_ct_fast_val_free(&guard);
  if (!ok)
    return false;
  *matched = truthy;
  return true;
}

static stmt_t *ny_ct_interp_find_function_stmt(stmt_t *s, const char *name,
                                               int depth) {
  if (!s || !name || depth > 64)
    return NULL;
  switch (s->kind) {
  case NY_S_FUNC: {
    const char *fn_name = s->as.fn.name;
    if (fn_name && (strcmp(fn_name, name) == 0 ||
                    strcmp(ny_name_leaf(fn_name), name) == 0))
      return s;
    return NULL;
  }
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      stmt_t *found = ny_ct_interp_find_function_stmt(s->as.block.body.data[i],
                                                      name, depth + 1);
      if (found)
        return found;
    }
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      stmt_t *found = ny_ct_interp_find_function_stmt(s->as.module.body.data[i],
                                                      name, depth + 1);
      if (found)
        return found;
    }
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++) {
      stmt_t *found = ny_ct_interp_find_function_stmt(
          s->as.impl.methods.data[i], name, depth + 1);
      if (found)
        return found;
    }
    break;
  default:
    break;
  }
  return NULL;
}

static stmt_t *ny_ct_interp_find_function(program_t *prog, const char *name) {
  if (!prog || !name)
    return NULL;
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *found =
        ny_ct_interp_find_function_stmt(prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

static stmt_t *ny_ct_interp_find_user_function_stmt(stmt_t *s, const char *name,
                                                    int depth) {
  if (!s || !name || depth > 64 || ny_is_stdlib_tok(s->tok))
    return NULL;
  switch (s->kind) {
  case NY_S_FUNC: {
    const char *fn_name = s->as.fn.name;
    return fn_name && (strcmp(fn_name, name) == 0 ||
                       strcmp(ny_name_leaf(fn_name), name) == 0)
               ? s
               : NULL;
  }
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      stmt_t *found = ny_ct_interp_find_user_function_stmt(
          s->as.block.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++) {
      stmt_t *found = ny_ct_interp_find_user_function_stmt(
          s->as.module.body.data[i], name, depth + 1);
      if (found)
        return found;
    }
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; i++) {
      stmt_t *found = ny_ct_interp_find_user_function_stmt(
          s->as.impl.methods.data[i], name, depth + 1);
      if (found)
        return found;
    }
    break;
  default:
    break;
  }
  return NULL;
}

static stmt_t *ny_ct_interp_find_user_function(program_t *prog,
                                               const char *name) {
  if (!prog || !name)
    return NULL;
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *found =
        ny_ct_interp_find_user_function_stmt(prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

static uint64_t ny_ct_interp_random_key(const stmt_t *body) {
  uint64_t hash = UINT64_C(14695981039346656037);
  const char *file = body && body->tok.filename ? body->tok.filename : "";
  for (const unsigned char *p = (const unsigned char *)file; *p; ++p) {
    hash ^= *p;
    hash *= UINT64_C(1099511628211);
  }
  const uint64_t site[] = {
      body ? (uint64_t)(uint32_t)body->tok.line : 0,
      body ? (uint64_t)(uint32_t)body->tok.col : 0,
      body ? (uint64_t)body->kind : 0,
  };
  for (size_t i = 0; i < sizeof(site) / sizeof(site[0]); ++i) {
    uint64_t value = site[i];
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= (unsigned char)(value >> shift);
      hash *= UINT64_C(1099511628211);
    }
  }
  return hash ? hash : UINT64_C(0x9e3779b97f4a7c15);
}

static int64_t ny_ct_interp_random_next(ny_ct_interp_ctx_t *ctx) {
  if (!ctx || !ctx->rng_state)
    return 0;
  *ctx->rng_state += UINT64_C(0x9e3779b97f4a7c15);
  uint64_t z = *ctx->rng_state;
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  uint64_t value = z ^ (z >> 31);
  return (int64_t)(((value & UINT64_C(0x3fffffffffffffff)) << 1) | 1);
}

static bool ny_ct_interp_is_intrinsic_call(const char *name) {
  if (!name)
    return false;
  return strcmp(name, "len") == 0 || strcmp(name, "range") == 0 ||
         strcmp(name, "range2") == 0 || strcmp(name, "Z") == 0 ||
         strcmp(name, "__main") == 0 || strcmp(name, "os") == 0 ||
         strcmp(name, "__os_name") == 0 || strcmp(name, "arch") == 0 ||
         strcmp(name, "__arch_name") == 0;
}

static expr_t *ny_ct_interp_find_global_init_stmt(stmt_t *s, const char *name,
                                                  int depth) {
  if (!s || !name || depth > 64)
    return NULL;
  if (s->kind == NY_S_VAR && !s->as.var.is_mut && !s->as.var.is_del) {
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *var_name = s->as.var.names.data[i];
      if (var_name && strcmp(var_name, name) == 0 && s->as.var.exprs.len > 0)
        return s->as.var.exprs
            .data[s->as.var.exprs.len == s->as.var.names.len ? i : 0];
    }
  }
  const ny_stmt_list *body = NULL;
  if (s->kind == NY_S_BLOCK)
    body = &s->as.block.body;
  else if (s->kind == NY_S_MODULE)
    body = &s->as.module.body;
  if (body) {
    for (size_t i = 0; i < body->len; i++) {
      expr_t *found =
          ny_ct_interp_find_global_init_stmt(body->data[i], name, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

static expr_t *ny_ct_interp_find_global_init(program_t *prog,
                                             const char *name) {
  if (!prog || !name)
    return NULL;
  for (size_t i = 0; i < prog->body.len; i++) {
    expr_t *found =
        ny_ct_interp_find_global_init_stmt(prog->body.data[i], name, 0);
    if (found)
      return found;
  }
  return NULL;
}

static bool ny_ct_interp_call_user(expr_t *call, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth) {
  if (!call || !ctx || !call->as.call.callee || depth > 256)
    return false;
  const char *name = ny_ct_fast_callee_leaf_name(call->as.call.callee);
  if (!name || !*name)
    return false;
  binding *b = ctx->cg ? lookup_global(ctx->cg, name) : NULL;
  stmt_t *fn = b ? b->stmt_t : NULL;
  bool intrinsic = ny_ct_interp_is_intrinsic_call(name);
  if (!fn || (intrinsic && ny_is_stdlib_tok(fn->tok))) {
    stmt_t *user_fn = ny_ct_interp_find_user_function(ctx->prog, name);
    if (user_fn)
      fn = user_fn;
    else if (intrinsic)
      return false;
    else if (!fn)
      fn = ny_ct_interp_find_function(ctx->prog, name);
  }
  if (!fn || fn->kind != NY_S_FUNC || !fn->as.fn.body ||
      fn->as.fn.is_variadic || call->as.call.args.len > fn->as.fn.params.len)
    return false;

  ny_ct_interp_ctx_t nested = {0};
  /*
   * Top-level functions do not inherit caller locals.
   */
  nested.cg = ctx->cg;
  nested.prog = ctx->prog;
  nested.local_fuel = ctx->local_fuel;
  nested.rng_state = ctx->rng_state;
  size_t param_count = fn->as.fn.params.len;
  bool *assigned = param_count ? calloc(param_count, sizeof(*assigned)) : NULL;
  if (param_count && !assigned)
    return false;
  bool ok = true;
  size_t next_positional = 0;
  for (size_t i = 0; ok && i < call->as.call.args.len; i++) {
    call_arg_t *call_arg = &call->as.call.args.data[i];
    size_t param_index = SIZE_MAX;
    if (call_arg->name) {
      for (size_t p = 0; p < param_count; p++) {
        const char *param_name = fn->as.fn.params.data[p].name;
        if (param_name && strcmp(param_name, call_arg->name) == 0) {
          param_index = p;
          break;
        }
      }
    } else {
      while (next_positional < param_count && assigned[next_positional])
        next_positional++;
      if (next_positional < param_count)
        param_index = next_positional++;
    }
    if (param_index == SIZE_MAX || assigned[param_index]) {
      ok = false;
      break;
    }
    ny_ct_fast_val_t arg = ny_ct_fast_none();
    if (!call_arg->val ||
        !ny_ct_interp_eval_expr(call_arg->val, ctx, &arg, depth + 1))
      ok = false;
    param_t *param = &fn->as.fn.params.data[param_index];
    if (ok && (!param->name || !*param->name ||
               !ny_ct_interp_set(&nested, param->name, arg)))
      ok = false;
    if (ok)
      assigned[param_index] = true;
    ny_ct_fast_val_free(&arg);
  }
  for (size_t i = 0; ok && i < param_count; i++) {
    if (assigned[i])
      continue;
    param_t *param = &fn->as.fn.params.data[i];
    ny_ct_fast_val_t arg = ny_ct_fast_none();
    if (!param->def || !param->name || !*param->name ||
        !ny_ct_interp_eval_expr(param->def, &nested, &arg, depth + 1) ||
        !ny_ct_interp_set(&nested, param->name, arg))
      ok = false;
    ny_ct_fast_val_free(&arg);
  }
  free(assigned);
  ny_ct_fast_val_t result = ny_ct_fast_none();
  bool did_return = false;
  if (ok)
    ok = ny_ct_interp_eval_stmt(fn->as.fn.body, &nested, &result, &did_return,
                                depth + 1);
  ny_ct_interp_ctx_free(&nested);
  if (!ok) {
    ny_ct_fast_val_free(&result);
    return false;
  }
  const stmt_t *tail = fn->as.fn.body;
  while (tail && tail->kind == NY_S_BLOCK && tail->as.block.body.len)
    tail = tail->as.block.body.data[tail->as.block.body.len - 1];
  if (!did_return && (!tail || tail->kind != NY_S_EXPR)) {
    ny_ct_fast_val_free(&result);
    result = ny_ct_fast_none();
  }
  ny_ct_fast_val_move(out, &result);
  return true;
}

static bool ny_ct_interp_eval_long_property(expr_t *target,
                                            ny_ct_interp_ctx_t *ctx,
                                            ny_ct_fast_val_t *out, int depth) {
  if (!target || !ctx || !out || depth > 256)
    return false;
  if (ny_ct_fast_eval_long_property(ctx->cg, target, out, 0))
    return true;
  if (target->kind == NY_E_MEMBER && target->as.member.name) {
    if (strcmp(target->as.member.name, "unhex") == 0) {
      ny_ct_fast_val_t s = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(target->as.member.target, ctx, &s,
                                  depth + 1) ||
          s.kind != NY_CT_FAST_STR) {
        ny_ct_fast_val_free(&s);
        return false;
      }
      bool ok = ny_ct_fast_unhex_long(s.s, s.s ? strlen(s.s) : 0, out);
      ny_ct_fast_val_free(&s);
      return ok;
    }
    if (strcmp(target->as.member.name, "to_bytes") == 0)
      return ny_ct_interp_eval_long_property(target->as.member.target, ctx, out,
                                             depth + 1);
  }
  ny_ct_fast_val_t v = ny_ct_fast_none();
  if (!ny_ct_interp_eval_expr(target, ctx, &v, depth + 1))
    return false;
  if (v.kind == NY_CT_FAST_INT || v.kind == NY_CT_FAST_BIGINT) {
    ny_ct_fast_val_move(out, &v);
    return true;
  }
  if (v.kind == NY_CT_FAST_STR) {
    bool ok = ny_ct_fast_bytes_long(v.s, v.s ? strlen(v.s) : 0, out);
    ny_ct_fast_val_free(&v);
    return ok;
  }
  ny_ct_fast_val_free(&v);
  return false;
}

static bool ny_ct_interp_eval_lambda1(expr_t *fn, ny_ct_interp_ctx_t *ctx,
                                      ny_ct_fast_val_t arg,
                                      ny_ct_fast_val_t *out, int depth) {
  if (!fn || !ctx || !out || (fn->kind != NY_E_LAMBDA && fn->kind != NY_E_FN) ||
      fn->as.lambda.params.len != 1 || !fn->as.lambda.body)
    return false;
  const char *param = fn->as.lambda.params.data[0].name;
  if (!param || !*param)
    return false;
  ny_ct_interp_ctx_t nested = {0};
  if (!ny_ct_interp_ctx_clone(ctx, &nested))
    return false;
  bool ok = ny_ct_interp_set(&nested, param, arg);
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  bool did_return = false;
  if (ok)
    ok = ny_ct_interp_eval_stmt(fn->as.lambda.body, &nested, &ret, &did_return,
                                depth + 1);
  ny_ct_interp_ctx_free(&nested);
  if (!ok) {
    ny_ct_fast_val_free(&ret);
    return false;
  }
  ny_ct_fast_val_move(out, &ret);
  return true;
}

static bool ny_ct_interp_eval_map(expr_t *target, expr_t *fn,
                                  ny_ct_interp_ctx_t *ctx,
                                  ny_ct_fast_val_t *out, int depth) {
  if (!target || !fn || !ctx || !out)
    return false;
  ny_ct_fast_val_t seq = ny_ct_fast_none();
  int64_t len = 0;
  if (!ny_ct_interp_eval_expr(target, ctx, &seq, depth + 1) ||
      !ny_ct_seq_len(&seq, &len) || len < 0 || len > 65536) {
    ny_ct_fast_val_free(&seq);
    return false;
  }
  ny_ct_fast_kind_t out_kind =
      seq.kind == NY_CT_FAST_TUPLE ? NY_CT_FAST_TUPLE : NY_CT_FAST_LIST;
  if (!ny_ct_make_seq(out_kind, (size_t)len, out)) {
    ny_ct_fast_val_free(&seq);
    return false;
  }
  for (int64_t i = 0; i < len; i++) {
    ny_ct_fast_val_t item = ny_ct_fast_none();
    ny_ct_fast_val_t mapped = ny_ct_fast_none();
    if (!ny_ct_seq_item(&seq, i, &item) ||
        !ny_ct_interp_eval_lambda1(fn, ctx, item, &mapped, depth + 1)) {
      ny_ct_fast_val_free(&item);
      ny_ct_fast_val_free(&mapped);
      ny_ct_fast_val_free(out);
      ny_ct_fast_val_free(&seq);
      return false;
    }
    ny_ct_fast_val_free(&item);
    ny_ct_fast_val_move(&out->items[i], &mapped);
  }
  ny_ct_fast_val_free(&seq);
  return true;
}

typedef struct ny_ct_interp_string_builder_t {
  char *data;
  size_t len;
  size_t cap;
} ny_ct_interp_string_builder_t;

static bool ny_ct_interp_string_append(ny_ct_interp_string_builder_t *builder,
                                       const char *data, size_t len) {
  if (!builder || (!data && len) || len > 1024 * 1024 - builder->len)
    return false;
  size_t need = builder->len + len + 1;
  if (need > builder->cap) {
    size_t cap = builder->cap ? builder->cap : 64;
    while (cap < need) {
      if (cap > SIZE_MAX / 2)
        return false;
      cap *= 2;
    }
    char *grown = realloc(builder->data, cap);
    if (!grown)
      return false;
    builder->data = grown;
    builder->cap = cap;
  }
  if (len)
    memcpy(builder->data + builder->len, data, len);
  builder->len += len;
  builder->data[builder->len] = '\0';
  return true;
}

static bool ny_ct_interp_format_string_value(const ny_ct_fast_val_t *value,
                                             const char **data, size_t *len,
                                             char buffer[128]) {
  if (!value || !data || !len || !buffer)
    return false;
  switch (value->kind) {
  case NY_CT_FAST_NONE:
    /*
     * Dynamic nil is represented by tagged zero in runtime stringification.
     */
    *data = "0";
    *len = 1;
    return true;
  case NY_CT_FAST_BOOL:
    *data = value->b ? "true" : "false";
    *len = value->b ? 4 : 5;
    return true;
  case NY_CT_FAST_INT: {
    int n = snprintf(buffer, 128, "%" PRId64, value->i);
    if (n < 0 || n >= 128)
      return false;
    *data = buffer;
    *len = (size_t)n;
    return true;
  }
  case NY_CT_FAST_FLOAT: {
    int n = snprintf(buffer, 128, "%g", value->f64);
    if (n < 0 || n >= 128)
      return false;
    *data = buffer;
    *len = (size_t)n;
    return true;
  }
  case NY_CT_FAST_STR:
    *data = value->s ? value->s : "";
    *len = value->len;
    return true;
  case NY_CT_FAST_BIGINT:
    *data = value->s ? value->s : "0";
    *len = strlen(*data);
    return true;
  default:
    /*
     * Runtime list/dict/pointer formatting is deliberately not guessed.
     */
    return false;
  }
}

static bool ny_ct_interp_eval_fstring(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                      ny_ct_fast_val_t *out, int depth) {
  if (!e || !ctx || !out)
    return false;
  ny_ct_interp_string_builder_t builder = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < e->as.fstring.parts.len; ++i) {
    const fstring_part_t *part = &e->as.fstring.parts.data[i];
    if (part->kind == NY_FSP_STR) {
      ok =
          ny_ct_interp_string_append(&builder, part->as.s.data, part->as.s.len);
      continue;
    }
    ny_ct_fast_val_t value = ny_ct_fast_none();
    char buffer[128];
    const char *text = NULL;
    size_t text_len = 0;
    ok = part->as.e &&
         ny_ct_interp_eval_expr(part->as.e, ctx, &value, depth + 1) &&
         ny_ct_interp_format_string_value(&value, &text, &text_len, buffer) &&
         ny_ct_interp_string_append(&builder, text, text_len);
    ny_ct_fast_val_free(&value);
  }
  if (!ok) {
    free(builder.data);
    return false;
  }
  if (!builder.data) {
    builder.data = calloc(1, 1);
    if (!builder.data)
      return false;
  }
  out->kind = NY_CT_FAST_STR;
  out->s = builder.data;
  out->len = builder.len;
  out->owns_s = true;
  return true;
}

static bool ny_ct_interp_eval_stmt(stmt_t *s, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *ret, bool *did_return,
                                   int depth) {
  if (!s || !ctx || !ret || !did_return)
    return false;
  if (depth > 256 || !ny_ct_interp_step(ctx))
    return false;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t scope_base = ctx->len;
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (!ny_ct_interp_eval_stmt(s->as.block.body.data[i], ctx, ret,
                                  did_return, depth + 1)) {
        if (!s->as.block.transparent)
          ny_ct_interp_pop_to(ctx, scope_base);
        return false;
      }
      if (*did_return || ctx->did_break || ctx->did_continue)
        break;
    }
    if (!s->as.block.transparent)
      ny_ct_interp_pop_to(ctx, scope_base);
    return true;
  }
  case NY_S_RETURN:
    if (!s->as.ret.value) {
      ny_ct_fast_val_free(ret);
      *ret = ny_ct_fast_none();
    } else {
      ny_ct_fast_val_t tmp = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(s->as.ret.value, ctx, &tmp, depth + 1)) {
        return false;
      }
      ny_ct_fast_val_move(ret, &tmp);
    }
    *did_return = true;
    return true;
  case NY_S_BREAK:
    ctx->did_break = true;
    return true;
  case NY_S_CONTINUE:
    ctx->did_continue = true;
    return true;
  case NY_S_MATCH: {
    if (!s->as.match.test)
      return false;
    ny_ct_fast_val_t test = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(s->as.match.test, ctx, &test, depth + 1)) {
      ny_ct_fast_val_free(&test);
      return false;
    }
    stmt_t *conseq = NULL;
    bool arm_selected = false;
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      bool matched = false;
      if (!ny_ct_interp_match_arm(&s->as.match.arms.data[i], &test, ctx,
                                  &matched, depth + 1)) {
        ny_ct_fast_val_free(&test);
        return false;
      }
      if (matched) {
        conseq = s->as.match.arms.data[i].conseq;
        arm_selected = true;
        break;
      }
    }
    ny_ct_fast_val_free(&test);
    if (!arm_selected)
      conseq = s->as.match.default_conseq;
    return !conseq ||
           ny_ct_interp_eval_stmt(conseq, ctx, ret, did_return, depth + 1);
  }
  case NY_S_EXPR: {
    ny_ct_fast_val_t tmp = ny_ct_fast_none();
    if (!s->as.expr.expr ||
        !ny_ct_interp_eval_expr(s->as.expr.expr, ctx, &tmp, depth + 1))
      return false;
    ny_ct_fast_val_move(ret, &tmp);
    return true;
  }
  case NY_S_VAR:
    if (s->as.var.is_destructure) {
      if (s->as.var.names.len == 0 || s->as.var.exprs.len != 1 ||
          !s->as.var.exprs.data[0])
        return false;
      ny_ct_fast_val_t source = ny_ct_fast_none();
      int64_t source_len = 0;
      if (!ny_ct_interp_eval_expr(s->as.var.exprs.data[0], ctx, &source,
                                  depth + 1) ||
          !ny_ct_seq_len(&source, &source_len) ||
          source_len < (int64_t)s->as.var.names.len) {
        ny_ct_fast_val_free(&source);
        return false;
      }
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        const char *name = s->as.var.names.data[i];
        ny_ct_fast_val_t item = ny_ct_fast_none();
        if (!name || !*name || !ny_ct_seq_item(&source, (int64_t)i, &item) ||
            !ny_ct_interp_define(ctx, name, item)) {
          ny_ct_fast_val_free(&item);
          ny_ct_fast_val_free(&source);
          return false;
        }
        ny_ct_fast_val_free(&item);
      }
      ny_ct_fast_val_free(&source);
      return true;
    }
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name)
        return false;
      ny_ct_fast_val_t v = ny_ct_fast_none();
      if (!s->as.var.is_del) {
        expr_t *rhs = NULL;
        if (s->as.var.exprs.len == s->as.var.names.len &&
            i < s->as.var.exprs.len)
          rhs = s->as.var.exprs.data[i];
        else if (s->as.var.exprs.len > 0)
          rhs = s->as.var.exprs.data[0];
        if (rhs && !ny_ct_interp_eval_expr(rhs, ctx, &v, depth + 1)) {
          ny_ct_fast_val_free(&v);
          return false;
        }
      }
      bool stored = s->as.var.is_decl ? ny_ct_interp_define(ctx, name, v)
                                      : ny_ct_interp_set(ctx, name, v);
      if (!stored) {
        ny_ct_fast_val_free(&v);
        return false;
      }
      ny_ct_fast_val_free(&v);
    }
    return true;
  case NY_S_IF: {
    if (!s->as.iff.test)
      return false;
    ny_ct_fast_val_t cond = ny_ct_fast_none();
    bool truthy = false;
    if (!ny_ct_interp_eval_expr(s->as.iff.test, ctx, &cond, depth + 1) ||
        !ny_ct_fast_truthy(&cond, &truthy)) {
      ny_ct_fast_val_free(&cond);
      return false;
    }
    ny_ct_fast_val_free(&cond);
    if (truthy) {
      if (s->as.iff.conseq)
        return ny_ct_interp_eval_stmt(s->as.iff.conseq, ctx, ret, did_return,
                                      depth + 1);
      return true;
    }
    return s->as.iff.alt ? ny_ct_interp_eval_stmt(s->as.iff.alt, ctx, ret,
                                                  did_return, depth + 1)
                         : true;
  }
  case NY_S_WHILE: {
    if (!s->as.whl.test || !s->as.whl.body)
      return false;
    if (s->as.whl.init && !ny_ct_interp_eval_stmt(s->as.whl.init, ctx, ret,
                                                  did_return, depth + 1))
      return false;
    if (*did_return)
      return true;
    while (1) {
      ny_ct_fast_val_t cond = ny_ct_fast_none();
      bool truthy = false;
      if (!ny_ct_interp_eval_expr(s->as.whl.test, ctx, &cond, depth + 1) ||
          !ny_ct_fast_truthy(&cond, &truthy)) {
        ny_ct_fast_val_free(&cond);
        return false;
      }
      ny_ct_fast_val_free(&cond);
      if (!truthy)
        break;
      if (!ny_ct_interp_eval_stmt(s->as.whl.body, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
      if (ctx->did_break) {
        ctx->did_break = false;
        break;
      }
      ctx->did_continue = false;
      if (s->as.whl.update &&
          !ny_ct_interp_eval_stmt(s->as.whl.update, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
    }
    return true;
  }
  case NY_S_FOR: {
    if (!s->as.fr.body)
      return false;
    if (s->as.fr.iterable) {
      if (!s->as.fr.iter_var)
        return false;
      ny_ct_fast_val_t seq = ny_ct_fast_none();
      int64_t len = 0;
      if (!ny_ct_interp_eval_expr(s->as.fr.iterable, ctx, &seq, depth + 1) ||
          !ny_ct_seq_len(&seq, &len) || len < 0 || len > 65536) {
        ny_ct_fast_val_free(&seq);
        return false;
      }
      for (int64_t i = 0; i < len; i++) {
        ny_ct_fast_val_t item = ny_ct_fast_none();
        if (s->as.fr.iter_by_index) {
          item.kind = NY_CT_FAST_INT;
          item.i = i;
        } else if (!ny_ct_seq_item(&seq, i, &item)) {
          ny_ct_fast_val_free(&seq);
          return false;
        }
        bool ok = ny_ct_interp_set(ctx, s->as.fr.iter_var, item);
        ny_ct_fast_val_free(&item);
        if (ok && s->as.fr.iter_index_var) {
          ny_ct_fast_val_t index = ny_ct_fast_none();
          index.kind = NY_CT_FAST_INT;
          index.i = i;
          ok = ny_ct_interp_set(ctx, s->as.fr.iter_index_var, index);
        }
        if (!ok || !ny_ct_interp_eval_stmt(s->as.fr.body, ctx, ret, did_return,
                                           depth + 1)) {
          ny_ct_fast_val_free(&seq);
          return false;
        }
        if (*did_return) {
          ny_ct_fast_val_free(&seq);
          return true;
        }
        if (ctx->did_break) {
          ctx->did_break = false;
          break;
        }
        ctx->did_continue = false;
      }
      ny_ct_fast_val_free(&seq);
      return true;
    }
    if (!s->as.fr.cond || !s->as.fr.update)
      return false;
    if (s->as.fr.init &&
        !ny_ct_interp_eval_stmt(s->as.fr.init, ctx, ret, did_return, depth + 1))
      return false;
    if (*did_return)
      return true;
    while (1) {
      ny_ct_fast_val_t cond = ny_ct_fast_none();
      bool truthy = false;
      if (!ny_ct_interp_eval_expr(s->as.fr.cond, ctx, &cond, depth + 1) ||
          !ny_ct_fast_truthy(&cond, &truthy)) {
        ny_ct_fast_val_free(&cond);
        return false;
      }
      ny_ct_fast_val_free(&cond);
      if (!truthy)
        break;
      if (!ny_ct_interp_eval_stmt(s->as.fr.body, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
      if (ctx->did_break) {
        ctx->did_break = false;
        break;
      }
      ctx->did_continue = false;
      if (!ny_ct_interp_eval_stmt(s->as.fr.update, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
    }
    return true;
  }
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(s->as.macro.args.data[i], ctx, &arg,
                                  depth + 1)) {
        ny_ct_fast_val_free(&arg);
        return false;
      }
      ny_ct_fast_val_free(&arg);
    }
    return !s->as.macro.body ||
           ny_ct_interp_eval_stmt(s->as.macro.body, ctx, ret, did_return,
                                  depth + 1);
  case NY_S_OPERATOR:
    return true;
  default:
    return false;
  }
}

static bool ny_ct_interp_eval_expr(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth) {
  if (!e || !ctx || !out)
    return false;
  if (depth > 256 || !ny_ct_interp_step(ctx))
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->tok.kind == NY_T_NIL) {
      out->kind = NY_CT_FAST_NONE;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      if (e->as.literal.integer_outside_i64)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      out->kind = NY_CT_FAST_FLOAT;
      out->f64 = e->as.literal.as.f;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_STR) {
      out->kind = NY_CT_FAST_STR;
      out->s = e->as.literal.as.s.data;
      out->len = e->as.literal.as.s.len;
      return true;
    }
    return false;
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    if (!name || !*name)
      return false;
    if (strcmp(name, "none") == 0) {
      *out = ny_ct_fast_none();
      return true;
    }
    if (ny_ct_platform_ident(name, out) || ny_ct_interp_get(ctx, name, out))
      return true;
    if (ctx->cg) {
      binding *b = lookup_global(ctx->cg, name);
      expr_t *init = b && !b->is_mut ? ny_binding_var_init_expr(b, name) : NULL;
      if (init && init != e)
        return ny_ct_interp_eval_expr(init, ctx, out, depth + 1);
      if (ny_try_eval_binding_comptime_const(ctx->cg, b, name, out, depth + 1))
        return true;
    } else {
      expr_t *init = ny_ct_interp_find_global_init(ctx->prog, name);
      if (init && init != e)
        return ny_ct_interp_eval_expr(init, ctx, out, depth + 1);
    }
    return false;
  }
  case NY_E_UNARY: {
    ny_ct_fast_val_t r = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.unary.right, ctx, &r, depth + 1))
      return false;
    bool ok = ny_ct_fast_eval_unary(e->as.unary.op, &r, out);
    ny_ct_fast_val_free(&r);
    return ok;
  }
  case NY_E_LOGICAL: {
    ny_ct_fast_val_t l = ny_ct_fast_none();
    bool lt = false;
    if (!ny_ct_interp_eval_expr(e->as.logical.left, ctx, &l, depth + 1) ||
        !ny_ct_fast_truthy(&l, &lt)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    bool is_and = strcmp(e->as.logical.op, "&&") == 0;
    bool is_or = strcmp(e->as.logical.op, "||") == 0;
    ny_ct_fast_val_free(&l);
    if (!is_and && !is_or)
      return false;
    if ((is_and && !lt) || (is_or && lt)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = is_or;
      return true;
    }
    ny_ct_fast_val_t r = ny_ct_fast_none();
    bool rt = false;
    if (!ny_ct_interp_eval_expr(e->as.logical.right, ctx, &r, depth + 1) ||
        !ny_ct_fast_truthy(&r, &rt)) {
      ny_ct_fast_val_free(&r);
      return false;
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = rt;
    ny_ct_fast_val_free(&r);
    return true;
  }
  case NY_E_BINARY: {
    ny_ct_fast_val_t l = ny_ct_fast_none(), r = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.binary.left, ctx, &l, depth + 1))
      return false;
    if (!ny_ct_interp_eval_expr(e->as.binary.right, ctx, &r, depth + 1)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    bool ok = ny_ct_fast_eval_binary(e->as.binary.op, &l, &r, out);
    ny_ct_fast_val_free(&l);
    ny_ct_fast_val_free(&r);
    return ok;
  }
  case NY_E_TERNARY: {
    ny_ct_fast_val_t c = ny_ct_fast_none();
    bool ct = false;
    if (!ny_ct_interp_eval_expr(e->as.ternary.cond, ctx, &c, depth + 1) ||
        !ny_ct_fast_truthy(&c, &ct)) {
      ny_ct_fast_val_free(&c);
      return false;
    }
    ny_ct_fast_val_free(&c);
    return ny_ct_interp_eval_expr(ct ? e->as.ternary.true_expr
                                     : e->as.ternary.false_expr,
                                  ctx, out, depth + 1);
  }
  case NY_E_LIST:
  case NY_E_TUPLE: {
    ny_ct_fast_kind_t kind =
        e->kind == NY_E_TUPLE ? NY_CT_FAST_TUPLE : NY_CT_FAST_LIST;
    if (!ny_ct_make_seq(kind, e->as.list_like.len, out))
      return false;
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (!ny_ct_interp_eval_expr(e->as.list_like.data[i], ctx, &out->items[i],
                                  depth + 1)) {
        ny_ct_fast_val_free(out);
        return false;
      }
    }
    return true;
  }
  case NY_E_DICT: {
    size_t count = e->as.dict.pairs.len;
    if (count > SIZE_MAX / 2 || count * 2 > SIZE_MAX / sizeof(*out->items))
      return false;
    out->kind = NY_CT_FAST_DICT;
    out->len = 0;
    out->items = count ? calloc(count * 2, sizeof(*out->items)) : NULL;
    if (count && !out->items)
      return false;
    for (size_t i = 0; i < count; ++i) {
      ny_ct_fast_val_t key = ny_ct_fast_none();
      ny_ct_fast_val_t value = ny_ct_fast_none();
      if (!e->as.dict.pairs.data[i].key || !e->as.dict.pairs.data[i].value ||
          !ny_ct_interp_eval_expr(e->as.dict.pairs.data[i].key, ctx, &key,
                                  depth + 1) ||
          !ny_ct_interp_eval_expr(e->as.dict.pairs.data[i].value, ctx, &value,
                                  depth + 1)) {
        ny_ct_fast_val_free(&key);
        ny_ct_fast_val_free(&value);
        ny_ct_fast_val_free(out);
        return false;
      }
      bool replaced = false;
      for (size_t j = 0; j < out->len; ++j) {
        ny_ct_fast_val_t equal = ny_ct_fast_none();
        bool ok =
            ny_ct_fast_eval_binary("==", &out->items[j * 2], &key, &equal);
        bool matches = ok && equal.kind == NY_CT_FAST_BOOL && equal.b;
        ny_ct_fast_val_free(&equal);
        if (!ok) {
          ny_ct_fast_val_free(&key);
          ny_ct_fast_val_free(&value);
          ny_ct_fast_val_free(out);
          return false;
        }
        if (matches) {
          ny_ct_fast_val_free(&out->items[j * 2 + 1]);
          ny_ct_fast_val_move(&out->items[j * 2 + 1], &value);
          ny_ct_fast_val_free(&key);
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        ny_ct_fast_val_move(&out->items[out->len * 2], &key);
        ny_ct_fast_val_move(&out->items[out->len * 2 + 1], &value);
        out->len++;
      }
      ny_ct_fast_val_free(&key);
      ny_ct_fast_val_free(&value);
    }
    return true;
  }
  case NY_E_COMPTIME: {
    ny_ct_interp_ctx_t nested = {0};
    ny_ct_fast_val_t nested_ret = ny_ct_fast_none();
    bool did_return = false;
    if (!ny_ct_interp_ctx_clone(ctx, &nested))
      return false;
    /*
     * A nested compile-time site owns its random stream. Sharing the parent
     * stream made its output depend on whether the parent had already drawn a
     * value, which breaks source-keyed determinism and parallel evaluation.
     */
    uint64_t nested_rng_state = 0;
    if (e->as.comptime_expr.origin == NY_COMPTIME_ORIGIN_HASH_RUN ||
        e->as.comptime_expr.origin == NY_COMPTIME_ORIGIN_HASH_COMPTIME) {
      nested_rng_state =
          ny_ct_interp_random_key(e->as.comptime_expr.body);
      nested.rng_state = &nested_rng_state;
    }
    bool ok = ny_ct_interp_eval_stmt(e->as.comptime_expr.body, &nested,
                                     &nested_ret, &did_return, depth + 1);
    ny_ct_interp_ctx_free(&nested);
    if (!ok) {
      ny_ct_fast_val_free(&nested_ret);
      return false;
    }
    ny_ct_fast_val_move(out, &nested_ret);
    return true;
  }
  case NY_E_MATCH: {
    stmt_t match_stmt = {
        .kind = NY_S_MATCH, .tok = e->tok, .as.match = e->as.match};
    bool did_return = false;
    return ny_ct_interp_eval_stmt(&match_stmt, ctx, out, &did_return,
                                  depth + 1) &&
           !did_return;
  }
  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return ny_ct_interp_eval_long_property(e->as.member.target, ctx, out,
                                             depth + 1);
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0) {
      ny_ct_fast_val_t target = ny_ct_fast_none();
      int64_t len = 0;
      bool ok =
          ny_ct_interp_eval_expr(e->as.member.target, ctx, &target, depth + 1);
      if (ok && target.kind == NY_CT_FAST_STR) {
        if (target.len > (size_t)INT64_MAX)
          ok = false;
        else
          len = (int64_t)target.len;
      } else if (ok) {
        ok = ny_ct_seq_len(&target, &len);
      }
      if (!ok) {
        ny_ct_fast_val_free(&target);
        return false;
      }
      out->kind = NY_CT_FAST_INT;
      out->i = len;
      ny_ct_fast_val_free(&target);
      return true;
    }
    return false;
  case NY_E_INDEX: {
    if (!e->as.index.target ||
        (!e->as.index.start && !e->as.index.stop && !e->as.index.step))
      return false;
    ny_ct_fast_val_t target = ny_ct_fast_none();
    ny_ct_fast_val_t start = ny_ct_fast_none();
    ny_ct_fast_val_t stop = ny_ct_fast_none();
    ny_ct_fast_val_t step = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.index.target, ctx, &target, depth + 1))
      return false;
    bool slicing = !e->as.index.start || e->as.index.stop || e->as.index.step;
    if (slicing) {
      const ny_ct_fast_val_t *start_arg = NULL;
      const ny_ct_fast_val_t *stop_arg = NULL;
      const ny_ct_fast_val_t *step_arg = NULL;
      bool stop_is_omitted = false;
      bool ok = true;
      if (e->as.index.start) {
        ok = ny_ct_interp_eval_expr(e->as.index.start, ctx, &start,
                                    depth + 1);
        start_arg = &start;
      }
      if (ok && e->as.index.stop) {
        stop_is_omitted = e->as.index.stop->kind == NY_E_LITERAL &&
                          e->as.index.stop->as.literal.kind == NY_LIT_INT &&
                          e->as.index.stop->as.literal.as.i == 0x3fffffff &&
                          e->as.index.stop->tok.kind != NY_T_NUMBER;
        if (!stop_is_omitted) {
          ok = ny_ct_interp_eval_expr(e->as.index.stop, ctx, &stop,
                                      depth + 1);
          stop_arg = &stop;
        }
      }
      if (ok && e->as.index.step) {
        ok = ny_ct_interp_eval_expr(e->as.index.step, ctx, &step,
                                    depth + 1);
        step_arg = &step;
      }
      if (ok)
        ok = ny_ct_fast_slice_sequence(&target, start_arg, stop_arg, step_arg,
                                       stop_is_omitted, out);
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&start);
      ny_ct_fast_val_free(&stop);
      ny_ct_fast_val_free(&step);
      return ok;
    }
    ny_ct_fast_val_t idx = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.index.start, ctx, &idx, depth + 1) ||
        idx.kind != NY_CT_FAST_INT) {
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&idx);
      return false;
    }
    int64_t len = 0;
    if (!ny_ct_seq_len(&target, &len)) {
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&idx);
      return false;
    }
    int64_t resolved = idx.i;
    if (resolved < 0)
      resolved += len;
    bool ok = ny_ct_seq_item(&target, resolved, out);
    ny_ct_fast_val_free(&target);
    ny_ct_fast_val_free(&idx);
    return ok;
  }
  case NY_E_CALL: {
    if (!e->as.call.callee)
      return false;
    const char *name = ny_ct_fast_callee_leaf_name(e->as.call.callee);
    if (!name)
      return false;
    if (strcmp(name, "__rand64") == 0 && e->as.call.args.len == 0 &&
        ctx->rng_state) {
      out->kind = NY_CT_FAST_INT;
      out->i = ny_ct_interp_random_next(ctx);
      return true;
    }
    if (strcmp(name, "__srand") == 0 && e->as.call.args.len == 1 &&
        !e->as.call.args.data[0].name && ctx->rng_state) {
      ny_ct_fast_val_t seed = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(e->as.call.args.data[0].val, ctx, &seed,
                                  depth + 1) ||
          seed.kind != NY_CT_FAST_INT) {
        ny_ct_fast_val_free(&seed);
        return false;
      }
      *ctx->rng_state = (uint64_t)(seed.i >> 1);
      out->kind = NY_CT_FAST_INT;
      out->i = seed.i;
      ny_ct_fast_val_free(&seed);
      return true;
    }
    /*
     * `core.from_int` preserves an integer value. Keep this tiny scalar
     * conversion available to compile-time callers such as core.math.random,
     * without opening arbitrary runtime conversion intrinsics to the
     * interpreter.
     */
    if (strcmp(name, "from_int") == 0 && e->as.call.args.len == 1 &&
        !e->as.call.args.data[0].name) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      bool ok = ny_ct_interp_eval_expr(e->as.call.args.data[0].val, ctx,
                                       &arg, depth + 1) &&
                arg.kind == NY_CT_FAST_INT;
      if (ok)
        ny_ct_fast_val_move(out, &arg);
      ny_ct_fast_val_free(&arg);
      return ok;
    }
    if (ny_ct_interp_call_user(e, ctx, out, depth + 1))
      return true;
    bool zero_arg = e->as.call.args.len == 0;
    bool one_member_arg =
        e->as.call.args.len == 1 && e->as.call.callee->kind == NY_E_MEMBER;
    if (strcmp(name, "len") == 0 && e->as.call.args.len == 1 &&
        !e->as.call.args.data[0].name) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      int64_t len = 0;
      bool ok = ny_ct_interp_eval_expr(e->as.call.args.data[0].val, ctx, &arg,
                                       depth + 1);
      if (ok && arg.kind == NY_CT_FAST_STR) {
        if (arg.len > (size_t)INT64_MAX)
          ok = false;
        else
          len = (int64_t)arg.len;
      } else if (ok) {
        ok = ny_ct_seq_len(&arg, &len);
      }
      ny_ct_fast_val_free(&arg);
      if (!ok)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = len;
      return true;
    }
    if (strcmp(name, "range") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.len <= 3) {
      ny_ct_fast_val_t vals[3] = {ny_ct_fast_none(), ny_ct_fast_none(),
                                  ny_ct_fast_none()};
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        if (!ny_ct_interp_eval_expr(e->as.call.args.data[i].val, ctx, &vals[i],
                                    depth + 1) ||
            vals[i].kind != NY_CT_FAST_INT) {
          for (size_t j = 0; j <= i && j < 3; j++)
            ny_ct_fast_val_free(&vals[j]);
          return false;
        }
      }
      bool ok = e->as.call.args.len == 1
                    ? ny_ct_make_range(0, vals[0].i, 1, out)
                : e->as.call.args.len == 2
                    ? ny_ct_make_range(vals[0].i, vals[1].i, 1, out)
                    : ny_ct_make_range(vals[0].i, vals[1].i, vals[2].i, out);
      for (size_t j = 0; j < 3; j++)
        ny_ct_fast_val_free(&vals[j]);
      return ok;
    }
    if (strcmp(name, "range2") == 0 && e->as.call.args.len >= 2 &&
        e->as.call.args.len <= 3) {
      ny_ct_fast_val_t vals[3] = {ny_ct_fast_none(), ny_ct_fast_none(),
                                  ny_ct_fast_none()};
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        if (!ny_ct_interp_eval_expr(e->as.call.args.data[i].val, ctx, &vals[i],
                                    depth + 1) ||
            vals[i].kind != NY_CT_FAST_INT) {
          for (size_t j = 0; j <= i && j < 3; j++)
            ny_ct_fast_val_free(&vals[j]);
          return false;
        }
      }
      bool ok = ny_ct_make_range(vals[0].i, vals[1].i,
                                 e->as.call.args.len == 3 ? vals[2].i : 1, out);
      for (size_t j = 0; j < 3; j++)
        ny_ct_fast_val_free(&vals[j]);
      return ok;
    }
    if (strcmp(name, "Z") == 0 && e->as.call.args.len == 1) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(e->as.call.args.data[0].val, ctx, &arg,
                                  depth + 1))
        return false;
      bool ok = ny_ct_fast_eval_bigint_constructor_arg(&arg, out);
      ny_ct_fast_val_free(&arg);
      return ok;
    }
    if (strcmp(name, "__main") == 0 && (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = ny_comptime_main_enabled(ctx->cg, e->tok);
      return true;
    }
    if ((strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_os_name();
      out->len = strlen(out->s ? out->s : "");
      return true;
    }
    if ((strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_arch_name();
      out->len = strlen(out->s ? out->s : "");
      return true;
    }
    refine_def_t *ref = ny_lookup_refine(ctx->cg, name);
    if (ref && e->as.call.args.len == 1) {
      int res = 0;
      if (ny_refine_eval_predicate(ctx->cg, NULL, 0, ref,
                                   e->as.call.args.data[0].val, &res)) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = res == 1;
        return true;
      }
    }
    return false;
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "map") == 0 &&
        e->as.memcall.args.len == 1)
      return ny_ct_interp_eval_map(e->as.memcall.target,
                                   e->as.memcall.args.data[0].val, ctx, out,
                                   depth + 1);
    if (e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0 &&
        (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
      ny_ct_fast_val_t target = ny_ct_fast_none();
      ny_ct_fast_val_t index = ny_ct_fast_none();
      ny_ct_fast_val_t fallback = {.kind = NY_CT_FAST_INT, .i = 0};
      int64_t len = 0;
      bool have_index = false, have_default = false;
      bool ok = ny_ct_interp_eval_expr(e->as.memcall.target, ctx, &target,
                                       depth + 1);
      if (ok && target.kind == NY_CT_FAST_STR)
        ok = ny_ct_fast_string_scalar_len(&target, &len);
      else if (ok && target.kind != NY_CT_FAST_DICT)
        ok = ny_ct_seq_len(&target, &len);
      for (size_t i = 0; ok && i < e->as.memcall.args.len; ++i) {
        const call_arg_t *arg = &e->as.memcall.args.data[i];
        ny_ct_fast_val_t *slot = NULL;
        if (!arg->name) {
          if (!have_index) {
            slot = &index;
            have_index = true;
          } else if (!have_default) {
            slot = &fallback;
            have_default = true;
          }
        } else if (strcmp(arg->name, "key") == 0 && !have_index) {
          slot = &index;
          have_index = true;
        } else if (strcmp(arg->name, "default") == 0 && !have_default) {
          slot = &fallback;
          have_default = true;
        }
        ok = slot && arg->val &&
             ny_ct_interp_eval_expr(arg->val, ctx, slot, depth + 1);
      }
      ok = ok && have_index &&
           (target.kind == NY_CT_FAST_DICT || index.kind == NY_CT_FAST_INT);
      if (!ok) {
        ny_ct_fast_val_free(&target);
        ny_ct_fast_val_free(&index);
        ny_ct_fast_val_free(&fallback);
        return false;
      }
      if (target.kind == NY_CT_FAST_DICT) {
        bool found = false;
        for (size_t i = 0; i < target.len && ok; ++i) {
          ny_ct_fast_val_t equal = ny_ct_fast_none();
          ok = ny_ct_interp_step(ctx) &&
               ny_ct_fast_eval_binary("==", &target.items[i * 2], &index,
                                      &equal);
          bool matches = ok && equal.kind == NY_CT_FAST_BOOL && equal.b;
          ny_ct_fast_val_free(&equal);
          if (matches) {
            found = true;
            ok = ny_ct_fast_val_clone(&target.items[i * 2 + 1], out);
            break;
          }
        }
        if (ok && !found)
          ok = ny_ct_fast_val_clone(&fallback, out);
      } else {
        int64_t resolved = index.i;
        if (resolved < 0 && resolved < -len)
          resolved = len;
        else if (resolved < 0)
          resolved += len;
        if (resolved < 0 || resolved >= len)
          ok = ny_ct_fast_val_clone(&fallback, out);
        else if (target.kind == NY_CT_FAST_STR)
          ok = ny_ct_fast_string_index(&target, resolved, out);
        else
          ok = ny_ct_seq_item(&target, resolved, out);
      }
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&index);
      ny_ct_fast_val_free(&fallback);
      return ok;
    }
    return false;
  case NY_E_FSTRING:
    return ny_ct_interp_eval_fstring(e, ctx, out, depth + 1);
  case NY_E_EXISTS: {
    const char *k_name = e->as.exists.var;
    expr_t *body = e->as.exists.body;
    if (!k_name || !body)
      return false;
    bool found_witness = false;
    for (int64_t cand = -500; cand <= 500; cand++) {
      expr_t lit_k;
      memset(&lit_k, 0, sizeof(lit_k));
      lit_k.kind = NY_E_LITERAL;
      lit_k.tok = e->tok;
      lit_k.as.literal.kind = NY_LIT_INT;
      lit_k.as.literal.as.i = cand;
      expr_t *sub =
          ny_expr_subst(ctx->cg ? ctx->cg->arena : NULL, body, k_name, &lit_k);
      ny_ct_fast_val_t res = ny_ct_fast_none();
      if (ny_ct_interp_eval_expr(sub, ctx, &res, depth + 1) &&
          res.kind == NY_CT_FAST_BOOL && res.b) {
        found_witness = true;
        break;
      }
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = found_witness;
    return true;
  }
  default:
    return false;
  }
}

static bool ny_ct_interp_eval_value(codegen_t *cg, program_t *prog,
                                    stmt_t *body, ny_ct_fast_val_t *out) {
  if (!body || !out || (cg && !ny_ct_fuel_consume(cg)))
    return false;
  int64_t local_fuel = INT64_C(1000000);
  uint64_t rng_state = ny_ct_interp_random_key(body);
  ny_ct_interp_ctx_t ctx = {0};
  ctx.cg = cg;
  ctx.prog = prog;
  ctx.local_fuel = cg ? NULL : &local_fuel;
  ctx.rng_state = &rng_state;
  if (!cg && !ny_ct_interp_step(&ctx))
    return false;
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  bool did_return = false;
  bool ok = ny_ct_interp_eval_stmt(body, &ctx, &ret, &did_return, 0);
  ny_ct_interp_ctx_free(&ctx);
  if (!ok) {
    ny_ct_fast_val_free(&ret);
    return false;
  }
  ny_ct_fast_val_move(out, &ret);
  return true;
}

bool ny_try_eval_comptime_interp_value(codegen_t *cg, stmt_t *body,
                                       ny_ct_fast_val_t *out) {
  return ny_ct_interp_eval_value(cg, cg ? cg->prog : NULL, body, out);
}

bool ny_try_eval_comptime_interp_program(program_t *prog, stmt_t *body,
                                         ny_ct_fast_val_t *out) {
  return ny_ct_interp_eval_value(NULL, prog, body, out);
}

bool ny_try_eval_comptime_interp(codegen_t *cg, stmt_t *body,
                                 int64_t *out_tagged) {
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  if (!ny_try_eval_comptime_interp_value(cg, body, &ret))
    return false;
  bool ok = ny_ct_fast_to_tagged(&ret, out_tagged);
  ny_ct_fast_val_free(&ret);
  return ok;
}

bool ny_ct_interp_eval_pure_call(codegen_t *cg, expr_t *call,
                                 ny_ct_fast_val_t *out) {
  if (!call || !out)
    return false;
  ny_ct_interp_ctx_t ctx = {0};
  ctx.cg = cg;
  ctx.prog = cg ? cg->prog : NULL;
  bool ok = ny_ct_interp_eval_expr(call, &ctx, out, 0);
  ny_ct_interp_ctx_free(&ctx);
  return ok;
}
