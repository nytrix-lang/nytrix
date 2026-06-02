#include "code/typepipeline.h"
#include "base/common.h"
#include "base/util.h"
#include "code/nullnarrow.h"
#include "parse/ast.h"
#include "priv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NYTRIX_HAS_Z3
#include <z3.h>
#endif

typedef struct ny_tp_json_t {
  char *data;
  size_t len;
  size_t cap;
} ny_tp_json_t;

typedef struct ny_tp_env_entry_t {
  const char *name;
  char *type;
  bool is_mut;
} ny_tp_env_entry_t;
typedef VEC(ny_tp_env_entry_t) ny_tp_env_list;

typedef struct ny_tp_env_t {
  ny_tp_env_list vars;
} ny_tp_env_t;

typedef struct ny_tp_param_fact_t {
  char *inferred_type;
  size_t observations;
  bool conflict;
} ny_tp_param_fact_t;
typedef VEC(ny_tp_param_fact_t) ny_tp_param_fact_list;

typedef struct ny_tp_diag_t {
  const char *stage;
  const char *code;
  char *message;
  char *context;
  char *expected;
  char *got;
  char *expr_kind;
  char *hint;
  char *fix;
  token_t tok;
} ny_tp_diag_t;
typedef VEC(ny_tp_diag_t) ny_tp_diag_list;

typedef struct ny_tp_fallback_t {
  const char *stage;
  const char *code;
  const char *expr_kind;
  char *reason;
  token_t tok;
} ny_tp_fallback_t;
typedef VEC(ny_tp_fallback_t) ny_tp_fallback_list;

typedef struct ny_tp_func_fact_t {
  stmt_t *stmt;
  const char *name;
  const char *owner;
  ny_tp_param_fact_list params;
  size_t call_candidates;
} ny_tp_func_fact_t;
typedef VEC(ny_tp_func_fact_t) ny_tp_func_fact_list;

typedef struct ny_tp_solver_info_t {
  const char *requested;
  const char *backend;
  bool z3_available;
  const char *z3_status;
  unsigned z3_checks;
  unsigned z3_obligations;
} ny_tp_solver_info_t;

typedef struct ny_tp_ctx_t {
  program_t *prog;
  codegen_t *cg;
  const char *source_name;
  bool include_std;
  ny_tp_func_fact_list funcs;
  ny_tp_diag_list diagnostics;
  ny_tp_fallback_list fallbacks;
  ny_tp_solver_info_t solver;
  size_t mono_candidates;
} ny_tp_ctx_t;

static void tp_append(ny_tp_json_t *j, const char *fmt, ...) {
  if (!j || !fmt)
    return;
  if (!j->data) {
    j->cap = 1024;
    j->data = malloc(j->cap);
    if (!j->data)
      return;
    j->data[0] = '\0';
    j->len = 0;
  }
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->data + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n < 0)
      return;
    if (j->len + (size_t)n < j->cap) {
      j->len += (size_t)n;
      return;
    }
    size_t new_cap = j->cap * 2 + (size_t)n + 1;
    char *tmp = realloc(j->data, new_cap);
    if (!tmp)
      return;
    j->data = tmp;
    j->cap = new_cap;
  }
}

static void tp_json_str(ny_tp_json_t *j, const char *s) {
  tp_append(j, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        tp_append(j, "\\\"");
        break;
      case '\\':
        tp_append(j, "\\\\");
        break;
      case '\n':
        tp_append(j, "\\n");
        break;
      case '\r':
        tp_append(j, "\\r");
        break;
      case '\t':
        tp_append(j, "\\t");
        break;
      default:
        if (*p < 32)
          tp_append(j, "\\u%04x", (unsigned)*p);
        else
          tp_append(j, "%c", *p);
        break;
      }
    }
  }
  tp_append(j, "\"");
}

static void tp_json_strn(ny_tp_json_t *j, const char *s, size_t n) {
  tp_append(j, "\"");
  if (s) {
    for (size_t i = 0; i < n && s[i]; ++i) {
      unsigned char c = (unsigned char)s[i];
      switch (c) {
      case '"':
        tp_append(j, "\\\"");
        break;
      case '\\':
        tp_append(j, "\\\\");
        break;
      case '\n':
        tp_append(j, "\\n");
        break;
      case '\r':
        tp_append(j, "\\r");
        break;
      case '\t':
        tp_append(j, "\\t");
        break;
      default:
        if (c < 32)
          tp_append(j, "\\u%04x", (unsigned)c);
        else
          tp_append(j, "%c", c);
        break;
      }
    }
  }
  tp_append(j, "\"");
}

static char *tp_take(ny_tp_json_t *j, const char *fallback) {
  if (j && j->data)
    return j->data;
  return ny_strdup(fallback ? fallback : "{}");
}

static char *tp_vformat(const char *fmt, va_list ap) {
  if (!fmt)
    return ny_strdup("");
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n < 0)
    return ny_strdup("diagnostic formatting failed");
  char *buf = malloc((size_t)n + 1);
  if (!buf)
    return ny_strdup("diagnostic allocation failed");
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  return buf;
}

static char *tp_strdup_maybe(const char *s) {
  return (s && *s) ? ny_strdup(s) : NULL;
}

static bool tp_hm_debug_enabled(void) {
  const char *v = getenv("NYTRIX_HM_DEBUG");
  if (!v || !*v)
    v = getenv("NY_HM_DEBUG");
  if (!v || !*v)
    return false;
  return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 &&
         strcmp(v, "off") != 0 && strcmp(v, "no") != 0;
}

static void tp_hm_debug_loc(token_t tok, const char *fmt, ...) {
  if (!tp_hm_debug_enabled() || !fmt)
    return;
  fprintf(stderr,
          "[hm debug] %s:%d:%d: ", tok.filename ? tok.filename : "unknown",
          tok.line, tok.col);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static void tp_add_diag_ex(ny_tp_ctx_t *ctx, token_t tok, const char *stage,
                           const char *code, const char *context,
                           const char *expected, const char *got,
                           const char *expr_kind, const char *hint,
                           const char *fix, const char *fmt, ...) {
  if (!ctx || !fmt)
    return;
  va_list ap;
  va_start(ap, fmt);
  char *msg = tp_vformat(fmt, ap);
  va_end(ap);
  ny_tp_diag_t diag = {.stage = stage ? stage : "type",
                       .code = code ? code : "type-error",
                       .message = msg ? msg : ny_strdup("type diagnostic"),
                       .context = tp_strdup_maybe(context),
                       .expected = tp_strdup_maybe(expected),
                       .got = tp_strdup_maybe(got),
                       .expr_kind = tp_strdup_maybe(expr_kind),
                       .hint = tp_strdup_maybe(hint),
                       .fix = tp_strdup_maybe(fix),
                       .tok = tok};
  vec_push(&ctx->diagnostics, diag);
  if (stage && strcmp(stage, "hm") == 0) {
    tp_hm_debug_loc(tok,
                    "diag code=%s context=%s expected=%s got=%s message=%s",
                    code ? code : "hm-type-error", context ? context : "",
                    expected ? expected : "", got ? got : "",
                    diag.message ? diag.message : "");
  }
}

static void tp_add_diag(ny_tp_ctx_t *ctx, token_t tok, const char *stage,
                        const char *code, const char *fmt, ...) {
  if (!ctx || !fmt)
    return;
  va_list ap;
  va_start(ap, fmt);
  char *msg = tp_vformat(fmt, ap);
  va_end(ap);
  tp_add_diag_ex(ctx, tok, stage, code, NULL, NULL, NULL, NULL, NULL, NULL,
                 "%s", msg ? msg : "type diagnostic");
  free(msg);
}

static void tp_diag_dispose(ny_tp_diag_t *diag) {
  if (!diag)
    return;
  free(diag->message);
  free(diag->context);
  free(diag->expected);
  free(diag->got);
  free(diag->expr_kind);
  free(diag->hint);
  free(diag->fix);
  memset(diag, 0, sizeof(*diag));
}

static const char *tp_expr_kind_name(expr_kind_t kind) {
  switch (kind) {
  case NY_E_LITERAL:
    return "literal";
  case NY_E_IDENT:
    return "ident";
  case NY_E_UNARY:
    return "unary";
  case NY_E_BINARY:
    return "binary";
  case NY_E_LOGICAL:
    return "logical";
  case NY_E_TERNARY:
    return "ternary";
  case NY_E_CALL:
    return "call";
  case NY_E_MEMCALL:
    return "memcall";
  case NY_E_INDEX:
    return "index";
  case NY_E_MEMBER:
    return "member";
  case NY_E_PTR_TYPE:
    return "ptr_type";
  case NY_E_DEREF:
    return "deref";
  case NY_E_SIZEOF:
    return "sizeof";
  case NY_E_TRY:
    return "try_expr";
  case NY_E_LAMBDA:
    return "lambda";
  case NY_E_FN:
    return "fn";
  case NY_E_LIST:
    return "list";
  case NY_E_TUPLE:
    return "tuple";
  case NY_E_DICT:
    return "dict";
  case NY_E_SET:
    return "set";
  case NY_E_FSTRING:
    return "fstring";
  case NY_E_MATCH:
    return "match";
  case NY_E_ASM:
    return "asm";
  case NY_E_COMPTIME:
    return "comptime";
  case NY_E_INFERRED_MEMBER:
    return "inferred_member";
  case NY_E_EMBED:
    return "embed";
  default:
    return "expr";
  }
}

static void tp_add_fallback(ny_tp_ctx_t *ctx, token_t tok, const char *stage,
                            const char *code, const char *expr_kind,
                            const char *fmt, ...) {
  if (!ctx || !fmt)
    return;
  va_list ap;
  va_start(ap, fmt);
  char *msg = tp_vformat(fmt, ap);
  va_end(ap);
  ny_tp_fallback_t fb = {.stage = stage ? stage : "type",
                         .code = code ? code : "type-fallback",
                         .expr_kind = expr_kind ? expr_kind : "expr",
                         .reason = msg ? msg : ny_strdup("type fallback"),
                         .tok = tok};
  vec_push(&ctx->fallbacks, fb);
  if (stage && strcmp(stage, "hm") == 0) {
    tp_hm_debug_loc(tok, "fallback code=%s expr=%s reason=%s",
                    code ? code : "hm-any-fallback",
                    expr_kind ? expr_kind : "expr", fb.reason ? fb.reason : "");
  }
}

static void tp_emit_diagnostics_json(ny_tp_json_t *j, const ny_tp_ctx_t *ctx) {
  tp_append(j, "[");
  for (size_t i = 0; ctx && i < ctx->diagnostics.len; ++i) {
    const ny_tp_diag_t *d = &ctx->diagnostics.data[i];
    if (i)
      tp_append(j, ",");
    tp_append(j, "{\"stage\":");
    tp_json_str(j, d->stage ? d->stage : "type");
    tp_append(j, ",\"code\":");
    tp_json_str(j, d->code ? d->code : "type-error");
    tp_append(j, ",\"message\":");
    tp_json_str(j, d->message ? d->message : "");
    tp_append(j, ",\"context\":");
    tp_json_str(j, d->context ? d->context : "");
    tp_append(j, ",\"expected\":");
    tp_json_str(j, d->expected ? d->expected : "");
    tp_append(j, ",\"got\":");
    tp_json_str(j, d->got ? d->got : "");
    tp_append(j, ",\"expr\":");
    tp_json_str(j, d->expr_kind ? d->expr_kind : "");
    tp_append(j, ",\"hint\":");
    tp_json_str(j, d->hint ? d->hint : "");
    tp_append(j, ",\"fix\":");
    tp_json_str(j, d->fix ? d->fix : "");
    tp_append(j, ",\"file\":");
    tp_json_str(j, d->tok.filename ? d->tok.filename : "");
    tp_append(j, ",\"line\":%d,\"col\":%d,\"lexeme\":", d->tok.line,
              d->tok.col);
    tp_json_strn(j, d->tok.lexeme, d->tok.len);
    tp_append(j, "}");
  }
  tp_append(j, "]");
}

static void tp_emit_fallbacks_json(ny_tp_json_t *j, const ny_tp_ctx_t *ctx) {
  tp_append(j, "[");
  for (size_t i = 0; ctx && i < ctx->fallbacks.len; ++i) {
    const ny_tp_fallback_t *fb = &ctx->fallbacks.data[i];
    if (i)
      tp_append(j, ",");
    tp_append(j, "{\"stage\":");
    tp_json_str(j, fb->stage ? fb->stage : "type");
    tp_append(j, ",\"code\":");
    tp_json_str(j, fb->code ? fb->code : "type-fallback");
    tp_append(j, ",\"expr\":");
    tp_json_str(j, fb->expr_kind ? fb->expr_kind : "expr");
    tp_append(j, ",\"reason\":");
    tp_json_str(j, fb->reason ? fb->reason : "");
    tp_append(j, ",\"file\":");
    tp_json_str(j, fb->tok.filename ? fb->tok.filename : "");
    tp_append(j, ",\"line\":%d,\"col\":%d}", fb->tok.line, fb->tok.col);
  }
  tp_append(j, "]");
}

static int tp_diag_error_code(const ny_tp_diag_t *d) {
  const char *code = d ? d->code : NULL;
  if (!code)
    return 1003;
  if (strstr(code, "arity"))
    return 1004;
  if (strstr(code, "non-function") || strstr(code, "non-callable"))
    return 1010;
  if (strstr(code, "unknown") || strstr(code, "unresolved"))
    return 1002;
  if (strstr(code, "return"))
    return 1008;
  return 1003;
}

static void tp_emit_user_diag(const ny_tp_diag_t *d,
                              const char *fallback_message) {
  if (!d)
    return;
  ny_diag_error_code(
      d->tok, tp_diag_error_code(d), "%s",
      d->message
          ? d->message
          : (fallback_message ? fallback_message : "type validation failed"));
  if (d->context && *d->context)
    ny_diag_hint("context: %s", d->context);
  if ((d->expected && *d->expected) || (d->got && *d->got))
    ny_diag_hint("HM types: expected %s, got %s",
                 d->expected ? d->expected : "unknown",
                 d->got ? d->got : "unknown");
  if (d->expr_kind && *d->expr_kind)
    ny_diag_hint("expression kind: %s", d->expr_kind);
  if (d->hint && *d->hint)
    ny_diag_hint("%s", d->hint);
  if (d->fix && *d->fix)
    ny_diag_fix("%s", d->fix);
  if (d->code && strncmp(d->code, "hm-strict-", 10) == 0)
    ny_diag_note_tok(d->tok, "diagnostic code: %s", d->code);
  else if (tp_hm_debug_enabled() && d->code)
    ny_diag_note_tok(d->tok, "HM diagnostic code: %s", d->code);
}

static bool tp_solver_mode_valid(const char *mode) {
  return mode && (strcmp(mode, "auto") == 0 || strcmp(mode, "hm") == 0 ||
                  strcmp(mode, "global") == 0 || strcmp(mode, "z3") == 0);
}

static void tp_solver_probe_z3(ny_tp_solver_info_t *info) {
  if (!info)
    return;
#ifdef NYTRIX_HAS_Z3
  Z3_config cfg = Z3_mk_config();
  Z3_context z3 = Z3_mk_context(cfg);
  Z3_solver solver = Z3_mk_solver(z3);
  Z3_solver_inc_ref(z3, solver);
  Z3_solver_assert(z3, solver, Z3_mk_true(z3));
  Z3_lbool status = Z3_solver_check(z3, solver);
  info->z3_checks++;
  info->z3_obligations++;
  info->z3_status = status == Z3_L_TRUE
                        ? "sat"
                        : (status == Z3_L_FALSE ? "unsat" : "unknown");
  Z3_solver_dec_ref(z3, solver);
  Z3_del_context(z3);
  Z3_del_config(cfg);
#else
  info->z3_status = "unavailable";
#endif
}

static void tp_solver_init(ny_tp_ctx_t *ctx) {
  if (!ctx)
    return;
  const char *requested = getenv("NYTRIX_TYPE_SOLVER");
  if (!requested || !*requested)
    requested = ctx->cg && ctx->cg->type_solver ? ctx->cg->type_solver : "auto";
  ctx->solver.requested = requested && *requested ? requested : "auto";
  ctx->solver.backend = "hm";
#ifdef NYTRIX_HAS_Z3
  ctx->solver.z3_available = true;
#else
  ctx->solver.z3_available = false;
#endif
  ctx->solver.z3_status = ctx->solver.z3_available ? "not-run" : "unavailable";

  if (!tp_solver_mode_valid(ctx->solver.requested)) {
    tp_add_diag(ctx, (token_t){0}, "hm", "solver-mode-unsupported",
                "unsupported type solver mode '%s' (expected auto, hm, or z3)",
                ctx->solver.requested);
    ctx->solver.requested = "auto";
  }

  bool wants_z3 = strcmp(ctx->solver.requested, "z3") == 0 ||
                  strcmp(ctx->solver.requested, "auto") == 0;
  bool forced_hm = strcmp(ctx->solver.requested, "hm") == 0 ||
                   strcmp(ctx->solver.requested, "global") == 0;
  if (wants_z3 && ctx->solver.z3_available) {
    ctx->solver.backend = "z3";
    tp_solver_probe_z3(&ctx->solver);
  } else if (strcmp(ctx->solver.requested, "z3") == 0 &&
             !ctx->solver.z3_available) {
    tp_add_diag(
        ctx, (token_t){0}, "hm", "solver-z3-unavailable",
        "--type-solver=z3 requested but this Nytrix build was compiled without "
        "Z3; "
        "reconfigure with -DNYTRIX_ENABLE_Z3=on or use --type-solver=hm");
  } else if (forced_hm) {
    ctx->solver.backend = "hm";
  }
}

static void tp_emit_solver_json(ny_tp_json_t *j, const ny_tp_ctx_t *ctx) {
  tp_append(j, "{\"requested\":");
  tp_json_str(j, ctx && ctx->solver.requested ? ctx->solver.requested : "auto");
  tp_append(j, ",\"backend\":");
  tp_json_str(j, ctx && ctx->solver.backend ? ctx->solver.backend : "hm");
  tp_append(j,
            ",\"hm_authority\":true,\"hm_arena_backed\":true,\"z3_available\":%"
            "s,\"z3_status\":",
            (ctx && ctx->solver.z3_available) ? "true" : "false");
  tp_json_str(j, ctx && ctx->solver.z3_status ? ctx->solver.z3_status
                                              : "unavailable");
  tp_append(j, ",\"z3_checks\":%u,\"z3_obligations\":%u}",
            ctx ? ctx->solver.z3_checks : 0,
            ctx ? ctx->solver.z3_obligations : 0);
}

static char *tp_errors_v1_json(const ny_tp_ctx_t *ctx, const char *stage,
                               const char *source_name, const char *message) {
  ny_tp_json_t j = {0};
  tp_append(&j, "{\"schema\":\"errors.v1\",\"stage\":");
  tp_json_str(&j, stage ? stage : "type");
  tp_append(&j, ",\"solver\":");
  tp_emit_solver_json(&j, ctx);
  tp_append(&j, ",\"strict_types\":%s",
            (ctx && ctx->cg && ctx->cg->strict_types) ? "true" : "false");
  tp_append(&j, ",\"source\":");
  tp_json_str(&j, source_name ? source_name : "");
  tp_append(
      &j, ",\"error_count\":%zu,\"message\":", ctx ? ctx->diagnostics.len : 0);
  tp_json_str(&j, message ? message : "type validation failed");
  tp_append(&j, ",\"errors\":");
  tp_emit_diagnostics_json(&j, ctx);
  tp_append(&j, ",\"fallback_count\":%zu,\"fallbacks\":",
            ctx ? ctx->fallbacks.len : 0);
  tp_emit_fallbacks_json(&j, ctx);
  tp_append(&j, ",\"dynamic_fallbacks\":");
  tp_emit_fallbacks_json(&j, ctx);
  tp_append(&j, ",\"dynamic_fallback_count\":%zu",
            ctx ? ctx->fallbacks.len : 0);
  tp_append(&j, "}");
  return tp_take(&j, "{\"schema\":\"errors.v1\",\"errors\":[]}");
}

static bool tp_stmt_in_scope(const ny_tp_ctx_t *ctx, const stmt_t *s) {
  if (!s)
    return true;
  if (ctx && ctx->include_std)
    return true;
  if (ny_is_stdlib_tok(s->tok))
    return false;
  if (!ctx || !ctx->source_name || !*ctx->source_name || !s->tok.filename ||
      s->tok.filename[0] == '<')
    return true;
  return strcmp(ctx->source_name, s->tok.filename) == 0;
}

static const char *tp_skip_nullable(const char *t) {
  return (t && t[0] == '?') ? t + 1 : t;
}

static bool tp_type_eq(const char *a, const char *b) {
  if (!a || !b)
    return false;
  return strcmp(a, b) == 0;
}

static bool tp_is_int_type(const char *t) {
  t = tp_skip_nullable(t);
  return t && (strcmp(t, "int") == 0 || strcmp(t, "i8") == 0 ||
               strcmp(t, "i16") == 0 || strcmp(t, "i32") == 0 ||
               strcmp(t, "i64") == 0 || strcmp(t, "i128") == 0 ||
               strcmp(t, "u8") == 0 || strcmp(t, "u16") == 0 ||
               strcmp(t, "u32") == 0 || strcmp(t, "u64") == 0 ||
               strcmp(t, "u128") == 0);
}

static bool tp_is_float_type(const char *t) {
  t = tp_skip_nullable(t);
  return t && (strcmp(t, "f32") == 0 || strcmp(t, "f64") == 0 ||
               strcmp(t, "f128") == 0);
}

static bool tp_is_complex_type(const char *t) {
  t = tp_skip_nullable(t);
  return t && (strcmp(t, "complex") == 0 || strcmp(t, "c64") == 0 ||
               strcmp(t, "c128") == 0);
}

static bool tp_is_core_scalar(const char *t) {
  t = tp_skip_nullable(t);
  return !t || !*t || strcmp(t, "any") == 0 || strcmp(t, "nil") == 0 ||
         strcmp(t, "bool") == 0 || strcmp(t, "str") == 0 ||
         strcmp(t, "char") == 0 || strcmp(t, "ptr") == 0 ||
         strcmp(t, "handle") == 0 || tp_is_int_type(t) || tp_is_float_type(t) ||
         tp_is_complex_type(t);
}

static bool tp_type_base_name(const char *type, char *out, size_t cap) {
  if (!out || cap == 0)
    return false;
  out[0] = '\0';
  const char *t = tp_skip_nullable(type);
  if (!t || !*t)
    return false;
  size_t n = 0;
  while (t[n] && t[n] != '<' && t[n] != '|' && t[n] != ' ' && n + 1 < cap)
    n++;
  if (n == 0)
    return false;
  memcpy(out, t, n);
  out[n] = '\0';
  return true;
}

static bool tp_is_builtin_value_type(const char *type) {
  char base[128];
  if (!tp_type_base_name(type, base, sizeof(base)))
    return true;
  return tp_is_core_scalar(base) || strcmp(base, "list") == 0 ||
         strcmp(base, "tuple") == 0 || strcmp(base, "dict") == 0 ||
         strcmp(base, "set") == 0 || strcmp(base, "bytes") == 0 ||
         strcmp(base, "range") == 0 || strcmp(base, "fnptr") == 0 ||
         strcmp(base, "number") == 0 || strcmp(base, "numeric") == 0 ||
         strcmp(base, "integer") == 0 || strcmp(base, "float") == 0 ||
         strcmp(base, "scalar") == 0 || strcmp(base, "seq") == 0 ||
         strcmp(base, "sequence") == 0 || strcmp(base, "collection") == 0 ||
         strcmp(base, "container") == 0 || strcmp(base, "iterable") == 0 ||
         strcmp(base, "indexable") == 0 || strcmp(base, "allocator") == 0 ||
         strcmp(base, "bigint") == 0;
}

static const char *tp_group_canon(const char *name) {
  if (!name)
    return NULL;
  const char *n = tp_skip_nullable(name);
  if (strcmp(n, "num") == 0)
    return "number";
  if (strcmp(n, "numeric") == 0)
    return "number";
  if (strcmp(n, "intlike") == 0)
    return "integer";
  if (strcmp(n, "floatlike") == 0)
    return "float";
  if (strcmp(n, "sequence") == 0)
    return "seq";
  return n;
}

static bool tp_type_group_accepts(const char *group, const char *type);

static bool tp_type_is_group(const char *name) {
  const char *n = tp_group_canon(name);
  return n && (strcmp(n, "number") == 0 || strcmp(n, "integer") == 0 ||
               strcmp(n, "float") == 0 || strcmp(n, "scalar") == 0 ||
               strcmp(n, "seq") == 0 || strcmp(n, "collection") == 0 ||
               strcmp(n, "container") == 0 || strcmp(n, "iterable") == 0 ||
               strcmp(n, "indexable") == 0 || strcmp(n, "allocator") == 0);
}

static bool tp_type_is_collection_base(const char *type, const char *base) {
  char b[128];
  return tp_type_base_name(type, b, sizeof(b)) && strcmp(b, base) == 0;
}

static bool tp_type_group_accepts(const char *group, const char *type) {
  const char *g = tp_group_canon(group);
  const char *t = tp_group_canon(type);
  if (!g || !t || !*g || !*t)
    return true;
  if (strcmp(g, t) == 0 || strcmp(g, "any") == 0 || strcmp(t, "any") == 0)
    return true;
  if (strcmp(g, "number") == 0)
    return tp_is_int_type(t) || tp_is_float_type(t) || strcmp(t, "bigint") == 0;
  if (strcmp(g, "integer") == 0)
    return tp_is_int_type(t) || strcmp(t, "bigint") == 0;
  if (strcmp(g, "float") == 0)
    return tp_is_float_type(t);
  if (strcmp(g, "scalar") == 0)
    return tp_type_group_accepts("number", t) || strcmp(t, "bool") == 0 ||
           strcmp(t, "str") == 0 || strcmp(t, "char") == 0;
  if (strcmp(g, "seq") == 0)
    return strcmp(t, "str") == 0 || strcmp(t, "bytes") == 0 ||
           strcmp(t, "range") == 0 || tp_type_is_collection_base(t, "list") ||
           tp_type_is_collection_base(t, "tuple");
  if (strcmp(g, "collection") == 0)
    return tp_type_is_collection_base(t, "list") ||
           tp_type_is_collection_base(t, "tuple") ||
           tp_type_is_collection_base(t, "dict") ||
           tp_type_is_collection_base(t, "set");
  if (strcmp(g, "container") == 0)
    return tp_type_group_accepts("collection", t) || strcmp(t, "bytes") == 0 ||
           strcmp(t, "range") == 0;
  if (strcmp(g, "iterable") == 0)
    return tp_type_group_accepts("seq", t) ||
           tp_type_is_collection_base(t, "dict") ||
           tp_type_is_collection_base(t, "set") || strcmp(t, "bytes") == 0;
  if (strcmp(g, "indexable") == 0)
    return tp_type_group_accepts("seq", t) ||
           tp_type_is_collection_base(t, "dict") || strcmp(t, "bytes") == 0;
  if (strcmp(g, "allocator") == 0)
    return strcmp(t, "ptr") == 0 || strcmp(t, "handle") == 0;
  return false;
}

static const char *tp_group_capability(const char *group) {
  const char *g = tp_group_canon(group);
  if (!g)
    return "";
  if (strcmp(g, "number") == 0 || strcmp(g, "integer") == 0 ||
      strcmp(g, "float") == 0)
    return "numeric";
  if (strcmp(g, "seq") == 0)
    return "sequence";
  return g;
}

static void tp_emit_type_groups_json(ny_tp_json_t *j) {
  static const struct {
    const char *name;
    const char *members;
  } groups[] = {
      {"number",
       "\"int\",\"i8\",\"i16\",\"i32\",\"i64\",\"i128\",\"u8\",\"u16\",\"u32\","
       "\"u64\",\"u128\",\"f32\",\"f64\",\"f128\",\"bigint\""},
      {"numeric",
       "\"int\",\"i8\",\"i16\",\"i32\",\"i64\",\"i128\",\"u8\",\"u16\",\"u32\","
       "\"u64\",\"u128\",\"f32\",\"f64\",\"f128\",\"bigint\""},
      {"integer",
       "\"int\",\"i8\",\"i16\",\"i32\",\"i64\",\"i128\",\"u8\",\"u16\",\"u32\","
       "\"u64\",\"u128\",\"bigint\""},
      {"float", "\"f32\",\"f64\",\"f128\""},
      {"scalar",
       "\"number\",\"bool\",\"str\",\"char\",\"complex\",\"c64\",\"c128\""},
      {"seq", "\"list\",\"tuple\",\"str\",\"bytes\",\"range\""},
      {"collection", "\"list\",\"dict\",\"set\",\"tuple\""},
      {"container", "\"collection\",\"bytes\",\"range\""},
      {"iterable", "\"seq\",\"set\",\"dict\",\"bytes\""},
      {"indexable", "\"seq\",\"dict\",\"bytes\""},
      {"allocator", "\"ptr\",\"handle\""},
  };
  tp_append(j, "[");
  for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
    if (i)
      tp_append(j, ",");
    tp_append(j, "{\"name\":");
    tp_json_str(j, groups[i].name);
    tp_append(j, ",\"members\":[%s]}", groups[i].members);
  }
  tp_append(j, "]");
}

static bool tp_is_nominal_custom_type(ny_tp_ctx_t *ctx, const char *type) {
  if (!ctx || !ctx->cg || tp_is_builtin_value_type(type))
    return false;
  char base[128];
  if (!tp_type_base_name(type, base, sizeof(base)))
    return false;
  for (size_t i = 0; i < ctx->cg->tagged_types.len; ++i) {
    const char *tag = ctx->cg->tagged_types.data[i];
    if (tag && strcmp(tag, base) == 0)
      return true;
  }
  return false;
}

static bool tp_is_cmp_op(const char *op) {
  return op && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0);
}

static bool tp_is_arith_op(const char *op) {
  return op && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                strcmp(op, "%") == 0 || strcmp(op, "^") == 0 ||
                strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^^") == 0 ||
                strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0);
}

static char *tp_wrap_nullable(const char *t) {
  if (!t || !*t || strcmp(t, "nil") == 0)
    return ny_strdup("?any");
  if (t[0] == '?')
    return ny_strdup(t);
  size_t n = strlen(t);
  char *out = malloc(n + 2);
  if (!out)
    return NULL;
  out[0] = '?';
  memcpy(out + 1, t, n + 1);
  return out;
}

static char *tp_merge_types_take(char *a, char *b) {
  if (!a)
    return b ? b : ny_strdup("any");
  if (!b)
    return a;
  if (strcmp(a, b) == 0) {
    free(b);
    return a;
  }
  if (strcmp(a, "any") == 0 || strcmp(b, "any") == 0) {
    free(a);
    free(b);
    return ny_strdup("any");
  }
  if (strcmp(a, "nil") == 0) {
    char *out = tp_wrap_nullable(b);
    free(a);
    free(b);
    return out;
  }
  if (strcmp(b, "nil") == 0) {
    char *out = tp_wrap_nullable(a);
    free(a);
    free(b);
    return out;
  }
  if (a[0] == '?' && strcmp(a + 1, b) == 0) {
    free(b);
    return a;
  }
  if (b[0] == '?' && strcmp(b + 1, a) == 0) {
    free(a);
    return b;
  }
  size_t an = strlen(a), bn = strlen(b);
  char *out = malloc(an + bn + 2);
  if (!out) {
    free(a);
    free(b);
    return ny_strdup("any");
  }
  memcpy(out, a, an);
  out[an] = '|';
  memcpy(out + an + 1, b, bn + 1);
  free(a);
  free(b);
  return out;
}

static void tp_env_dispose(ny_tp_env_t *env) {
  if (!env)
    return;
  for (size_t i = 0; i < env->vars.len; ++i)
    free(env->vars.data[i].type);
  vec_free(&env->vars);
}

static const char *tp_env_get(ny_tp_env_t *env, const char *name) {
  if (!env || !name)
    return NULL;
  for (size_t i = env->vars.len; i > 0; --i) {
    ny_tp_env_entry_t *e = &env->vars.data[i - 1];
    if (e->name && strcmp(e->name, name) == 0)
      return e->type;
  }
  return NULL;
}

static void tp_env_set(ny_tp_env_t *env, const char *name, const char *type,
                       bool is_mut) {
  if (!env || !name || !*name)
    return;
  for (size_t i = env->vars.len; i > 0; --i) {
    ny_tp_env_entry_t *e = &env->vars.data[i - 1];
    if (e->name && strcmp(e->name, name) == 0) {
      free(e->type);
      e->type = ny_strdup(type && *type ? type : "any");
      e->is_mut = is_mut;
      return;
    }
  }
  ny_tp_env_entry_t entry = {.name = name,
                             .type = ny_strdup(type && *type ? type : "any"),
                             .is_mut = is_mut};
  vec_push(&env->vars, entry);
}

static ny_tp_func_fact_t *tp_find_func(ny_tp_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return NULL;
  for (size_t i = 0; i < ctx->funcs.len; ++i) {
    ny_tp_func_fact_t *f = &ctx->funcs.data[i];
    if (f->name && strcmp(f->name, name) == 0)
      return f;
    const char *tail = f->name ? strrchr(f->name, '.') : NULL;
    if (tail && strcmp(tail + 1, name) == 0)
      return f;
  }
  return NULL;
}

static fun_sig *tp_lookup_sig(codegen_t *cg, const char *name) {
  if (!cg || !name)
    return NULL;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (sig->name && strcmp(sig->name, name) == 0)
      return sig;
    const char *tail = sig->name ? strrchr(sig->name, '.') : NULL;
    if (tail && strcmp(tail + 1, name) == 0)
      return sig;
  }
  return NULL;
}

static const char *tp_lookup_global_type(codegen_t *cg, const char *name) {
  if (!cg || !name)
    return NULL;
  binding *b = lookup_global(cg, name);
  if (!b)
    return NULL;
  if (b->type_name && !tp_is_int_type(b->type_name) &&
      !tp_is_float_type(b->type_name))
    return b->type_name;
  if (b->is_int_slot || b->is_int_direct)
    return "int";
  if (b->is_f64_slot || b->is_f64_direct)
    return "f64";
  if (b->is_f32_slot || b->is_f32_direct)
    return "f32";
  return b->type_name;
}

static const char *tp_lookup_module_alias_member_type(codegen_t *cg,
                                                      const char *target_name,
                                                      const char *member_name) {
  if (!cg || !target_name || !*target_name || !member_name || !*member_name)
    return NULL;
  const char *module_name =
      ny_lookup_module_alias(cg, NULL, 0, target_name, strlen(target_name), 0);
  if (!module_name || !*module_name)
    return NULL;
  char dotted[512];
  int nw = snprintf(dotted, sizeof(dotted), "%s.%s", module_name, member_name);
  if (nw <= 0 || (size_t)nw >= sizeof(dotted))
    return NULL;
  const char *type_name = tp_lookup_global_type(cg, dotted);
  if (type_name)
    return type_name;
  const char *resolved = resolve_import_alias(cg, dotted);
  return (resolved && *resolved && strcmp(resolved, dotted) != 0)
             ? tp_lookup_global_type(cg, resolved)
             : NULL;
}

static const char *tp_lookup_use_alias_stmt(stmt_t *s, const char *alias) {
  if (!s || !alias || !*alias)
    return NULL;
  switch (s->kind) {
  case NY_S_USE:
    if (!s->as.use.import_all && s->as.use.imports.len == 0 &&
        s->as.use.alias && strcmp(s->as.use.alias, alias) == 0)
      return s->as.use.module;
    return NULL;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const char *found =
          tp_lookup_use_alias_stmt(s->as.block.body.data[i], alias);
      if (found)
        return found;
    }
    return NULL;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      const char *found =
          tp_lookup_use_alias_stmt(s->as.module.body.data[i], alias);
      if (found)
        return found;
    }
    return NULL;
  default:
    return NULL;
  }
}

static const char *tp_lookup_program_use_alias(codegen_t *cg,
                                               const char *alias) {
  if (!cg || !alias || !*alias)
    return NULL;
  if (cg->prog) {
    for (size_t i = 0; i < cg->prog->body.len; ++i) {
      const char *found =
          tp_lookup_use_alias_stmt(cg->prog->body.data[i], alias);
      if (found && *found)
        return found;
    }
  }
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; ++i) {
      const char *found = tp_lookup_use_alias_stmt(prog->body.data[i], alias);
      if (found && *found)
        return found;
    }
  }
  return NULL;
}

static bool tp_resolve_module_expr_path(codegen_t *cg, expr_t *e, char *out,
                                        size_t out_cap) {
  if (!cg || !e || !out || out_cap == 0)
    return false;
  if (ny_resolve_module_expr_path(cg, NULL, 0, e, out, out_cap))
    return true;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    const char *use_alias = tp_lookup_program_use_alias(cg, e->as.ident.name);
    if (use_alias && *use_alias) {
      int n = snprintf(out, out_cap, "%s", use_alias);
      return n > 0 && (size_t)n < out_cap;
    }
    const char *resolved = resolve_import_alias(cg, e->as.ident.name);
    if (resolved && *resolved && strcmp(resolved, e->as.ident.name) != 0) {
      int n = snprintf(out, out_cap, "%s", resolved);
      return n > 0 && (size_t)n < out_cap;
    }
  }
  return false;
}

static const char *tp_func_param_inferred(ny_tp_func_fact_t *f, size_t i) {
  if (!f || i >= f->params.len)
    return NULL;
  ny_tp_param_fact_t *p = &f->params.data[i];
  if (p->conflict)
    return "any";
  return p->inferred_type;
}

static char *tp_collection_type(const char *base, char *elem) {
  if (!elem)
    elem = ny_strdup("empty");
  size_t bn = strlen(base), en = strlen(elem);
  char *out = malloc(bn + en + 3);
  if (!out) {
    free(elem);
    return ny_strdup(base);
  }
  memcpy(out, base, bn);
  out[bn] = '<';
  memcpy(out + bn + 1, elem, en);
  out[bn + 1 + en] = '>';
  out[bn + 2 + en] = '\0';
  free(elem);
  return out;
}

static char *tp_pair_collection_type(const char *base, char *k, char *v) {
  if (!k)
    k = ny_strdup("empty");
  if (!v)
    v = ny_strdup("empty");
  size_t bn = strlen(base), kn = strlen(k), vn = strlen(v);
  char *out = malloc(bn + kn + vn + 5);
  if (!out) {
    free(k);
    free(v);
    return ny_strdup(base);
  }
  memcpy(out, base, bn);
  out[bn] = '<';
  memcpy(out + bn + 1, k, kn);
  out[bn + 1 + kn] = ',';
  out[bn + 2 + kn] = ' ';
  memcpy(out + bn + 3 + kn, v, vn);
  out[bn + 3 + kn + vn] = '>';
  out[bn + 4 + kn + vn] = '\0';
  free(k);
  free(v);
  return out;
}

static char *tp_expr_type(ny_tp_ctx_t *ctx, ny_tp_env_t *env, expr_t *e,
                          int depth);

static char *tp_call_return_type(ny_tp_ctx_t *ctx, ny_tp_env_t *env,
                                 const char *name, ny_call_arg_list *args) {
  (void)env;
  if (!name)
    return ny_strdup("any");
  if (strcmp(name, "list") == 0 || strcmp(name, "std.core.list") == 0)
    return ny_strdup("list");
  if (strcmp(name, "tuple") == 0)
    return ny_strdup("tuple");
  if (strcmp(name, "dict") == 0 || strcmp(name, "std.core.dict") == 0)
    return ny_strdup("dict");
  if (strcmp(name, "set") == 0 || strcmp(name, "std.core.set") == 0)
    return ny_strdup("set");
  if (strcmp(name, "bytes") == 0)
    return ny_strdup("bytes");
  if (strcmp(name, "range") == 0)
    return ny_strdup("range");
  if (strcmp(name, "int") == 0 || strcmp(name, "to_int") == 0)
    return ny_strdup("int");
  if (strcmp(name, "float") == 0 || strcmp(name, "to_float") == 0)
    return ny_strdup("f64");
  if (strcmp(name, "str") == 0 || strcmp(name, "to_str") == 0 ||
      strcmp(name, "type") == 0 || strcmp(name, "type_name") == 0 ||
      strcmp(name, "type_shape") == 0)
    return ny_strdup("str");
  if (strcmp(name, "len") == 0 || strstr(name, ".len"))
    return ny_strdup("int");
  fun_sig *sig = tp_lookup_sig(ctx ? ctx->cg : NULL, name);
  if (sig) {
    if (sig->return_type && *sig->return_type)
      return ny_strdup(sig->return_type);
    if (sig->inferred_return_type && *sig->inferred_return_type)
      return ny_strdup(sig->inferred_return_type);
  }
  ny_tp_func_fact_t *f = tp_find_func(ctx, name);
  if (f && f->stmt && f->stmt->kind == NY_S_FUNC && f->stmt->as.fn.return_type)
    return ny_strdup(f->stmt->as.fn.return_type);
  if (ctx && ctx->cg) {
    for (size_t i = 0; i < ctx->cg->tagged_types.len; ++i) {
      const char *tag = ctx->cg->tagged_types.data[i];
      if (tag && strcmp(tag, name) == 0)
        return ny_strdup(name);
    }
  }
  if (args && args->len <= 1 &&
      (strcmp(name, "__tagof") == 0 || strcmp(name, "__runtime_tag") == 0 ||
       strcmp(name, "__tag") == 0))
    return ny_strdup("int");
  return ny_strdup("any");
}

static const char *tp_literal_type(literal_t *lit, token_t tok) {
  if (!lit)
    return "any";
  if (lit->kind == NY_LIT_BOOL)
    return "bool";
  if (lit->kind == NY_LIT_STR)
    return "str";
  if (lit->kind == NY_LIT_FLOAT) {
    if (lit->hint == NY_LIT_HINT_F32)
      return "f32";
    if (lit->hint == NY_LIT_HINT_F128)
      return "f128";
    return "f64";
  }
  if (lit->kind == NY_LIT_INT) {
    if (tok.kind == NY_T_NIL)
      return "nil";
    switch (lit->hint) {
    case NY_LIT_HINT_I8:
      return "i8";
    case NY_LIT_HINT_I16:
      return "i16";
    case NY_LIT_HINT_I32:
      return "i32";
    case NY_LIT_HINT_I64:
      return "i64";
    case NY_LIT_HINT_I128:
      return "i128";
    case NY_LIT_HINT_U8:
      return "u8";
    case NY_LIT_HINT_U16:
      return "u16";
    case NY_LIT_HINT_U32:
      return "u32";
    case NY_LIT_HINT_U64:
      return "u64";
    case NY_LIT_HINT_U128:
      return "u128";
    default:
      return "int";
    }
  }
  return "any";
}

static const char *tp_enum_member_owner_name(ny_tp_ctx_t *ctx, expr_t *e) {
  codegen_t *cg = ctx ? ctx->cg : NULL;
  if (!cg || !e)
    return NULL;
  enum_def_t *owner = NULL;
  enum_member_def_t *member = NULL;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    member = lookup_enum_member_owner(cg, e->as.ident.name, &owner);
  } else if (e->kind == NY_E_MEMBER) {
    char *full_name = codegen_full_name(cg, e, cg->arena);
    if (full_name)
      member = lookup_enum_member_owner(cg, full_name, &owner);
  }
  return (member && owner && owner->name) ? owner->name : NULL;
}

static char *tp_expr_type(ny_tp_ctx_t *ctx, ny_tp_env_t *env, expr_t *e,
                          int depth) {
  if (!e || depth > 8)
    return ny_strdup("any");
  switch (e->kind) {
  case NY_E_LITERAL:
    return ny_strdup(tp_literal_type(&e->as.literal, e->tok));
  case NY_E_IDENT: {
    const char *t = tp_env_get(env, e->as.ident.name);
    if (t)
      return ny_strdup(t);
    t = tp_lookup_global_type(ctx ? ctx->cg : NULL, e->as.ident.name);
    if (t)
      return ny_strdup(t);
    t = tp_enum_member_owner_name(ctx, e);
    if (t)
      return ny_strdup(t);
    fun_sig *sig = tp_lookup_sig(ctx ? ctx->cg : NULL, e->as.ident.name);
    return ny_strdup(sig ? "fnptr" : "any");
  }
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    char *elem = NULL;
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      elem = tp_merge_types_take(
          elem, tp_expr_type(ctx, env, e->as.list_like.data[i], depth + 1));
    return tp_collection_type(e->kind == NY_E_SET
                                  ? "set"
                                  : (e->kind == NY_E_TUPLE ? "tuple" : "list"),
                              elem);
  }
  case NY_E_DICT: {
    char *key = NULL, *val = NULL;
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      dict_pair_t *p = &e->as.dict.pairs.data[i];
      key = tp_merge_types_take(key, tp_expr_type(ctx, env, p->key, depth + 1));
      val =
          tp_merge_types_take(val, tp_expr_type(ctx, env, p->value, depth + 1));
    }
    return tp_pair_collection_type("dict", key, val);
  }
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT)
      return tp_call_return_type(ctx, env, e->as.call.callee->as.ident.name,
                                 &e->as.call.args);
    return ny_strdup("any");
  case NY_E_MEMCALL: {
    if (e->as.memcall.target && e->as.memcall.name) {
      char module_path[1024];
      if (tp_resolve_module_expr_path(ctx ? ctx->cg : NULL,
                                      e->as.memcall.target, module_path,
                                      sizeof(module_path))) {
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(ctx->cg, module_path,
                                            e->as.memcall.name, resolved_fun,
                                            sizeof(resolved_fun)))
          return tp_call_return_type(ctx, env, resolved_fun,
                                     &e->as.memcall.args);
        return ny_strdup("any");
      }
    }
    char *target_t = tp_expr_type(ctx, env, e->as.memcall.target, depth + 1);
    char full[512];
    snprintf(full, sizeof(full), "%s.%s",
             tp_skip_nullable(target_t ? target_t : "any"),
             e->as.memcall.name ? e->as.memcall.name : "");
    char *ret = tp_call_return_type(ctx, env, full, &e->as.memcall.args);
    free(target_t);
    return ret;
  }
  case NY_E_MEMBER: {
    const char *owner = tp_enum_member_owner_name(ctx, e);
    if (owner)
      return ny_strdup(owner);
  }
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return ny_strdup("integer");
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0)
      return ny_strdup("int");
    if (e->as.member.target && e->as.member.target->kind == NY_E_IDENT &&
        e->as.member.target->as.ident.name && e->as.member.name &&
        !tp_env_get(env, e->as.member.target->as.ident.name)) {
      const char *module_member = tp_lookup_module_alias_member_type(
          ctx ? ctx->cg : NULL, e->as.member.target->as.ident.name,
          e->as.member.name);
      if (module_member)
        return ny_strdup(module_member);
    }
    return ny_strdup("any");
  case NY_E_INDEX: {
    char *target = tp_expr_type(ctx, env, e->as.index.target, depth + 1);
    char *lt = strchr(target, '<');
    if (lt) {
      char *gt = strrchr(lt + 1, '>');
      if (gt && gt > lt + 1) {
        size_t n = (size_t)(gt - lt - 1);
        char *inside = ny_strndup(lt + 1, n);
        char *comma = strchr(inside, ',');
        if (comma && strncmp(target, "dict<", 5) == 0) {
          char *v = comma + 1;
          while (*v == ' ')
            ++v;
          char *out = ny_strdup(v);
          free(inside);
          free(target);
          return out;
        }
        free(target);
        return inside;
      }
    }
    free(target);
    return ny_strdup("any");
  }
  case NY_E_BINARY: {
    char *lt = tp_expr_type(ctx, env, e->as.binary.left, depth + 1);
    char *rt = tp_expr_type(ctx, env, e->as.binary.right, depth + 1);
    const char *op = e->as.binary.op;
    char *out = NULL;
    if (tp_is_cmp_op(op)) {
      out = ny_strdup("bool");
    } else if ((tp_is_float_type(lt) || tp_is_float_type(rt)) &&
               tp_is_arith_op(op)) {
      out = ny_strdup("f64");
    } else if (tp_is_int_type(lt) && tp_is_int_type(rt) && tp_is_arith_op(op)) {
      out = ny_strdup("int");
    } else if (ctx && ctx->cg && op) {
      for (size_t i = ctx->cg->operators.len; i > 0; --i) {
        ny_operator_def_t *def = &ctx->cg->operators.data[i - 1];
        if (def->op && strcmp(def->op, op) == 0 &&
            tp_type_eq(def->left_type, lt) && tp_type_eq(def->right_type, rt)) {
          out = ny_strdup(def->return_type ? def->return_type : "any");
          break;
        }
      }
    }
    free(lt);
    free(rt);
    return out ? out : ny_strdup("any");
  }
  case NY_E_LOGICAL:
    return ny_strdup("bool");
  case NY_E_UNARY:
    if (e->as.unary.op && strcmp(e->as.unary.op, "async") == 0)
      return ny_strdup("handle");
    if (e->as.unary.op && strcmp(e->as.unary.op, "await") == 0)
      return ny_strdup("any");
    if (e->as.unary.op && strcmp(e->as.unary.op, "!") == 0)
      return ny_strdup("bool");
    return tp_expr_type(ctx, env, e->as.unary.right, depth + 1);
  case NY_E_TERNARY: {
    char *a = tp_expr_type(ctx, env, e->as.ternary.true_expr, depth + 1);
    char *b = tp_expr_type(ctx, env, e->as.ternary.false_expr, depth + 1);
    return tp_merge_types_take(a, b);
  }
  case NY_E_LAMBDA:
  case NY_E_FN:
    return ny_strdup("fnptr");
  case NY_E_PTR_TYPE:
  case NY_E_DEREF:
    return ny_strdup("ptr");
  case NY_E_SIZEOF:
    return ny_strdup("int");
  case NY_E_COMPTIME:
    if (e->as.comptime_expr.body) {
      stmt_t *body = e->as.comptime_expr.body;
      if (body->kind == NY_S_BLOCK && body->as.block.body.len > 0)
        body = body->as.block.body.data[body->as.block.body.len - 1];
      if (body && body->kind == NY_S_RETURN && body->as.ret.value)
        return tp_expr_type(ctx, env, body->as.ret.value, depth + 1);
      if (body && body->kind == NY_S_EXPR && body->as.expr.expr)
        return tp_expr_type(ctx, env, body->as.expr.expr, depth + 1);
    }
    return ny_strdup("any");
  default:
    return ny_strdup("any");
  }
}

static void tp_observe_param(ny_tp_func_fact_t *f, size_t idx, char *type) {
  if (!f || !type || idx >= f->params.len) {
    free(type);
    return;
  }
  if (strcmp(type, "any") == 0) {
    free(type);
    return;
  }
  ny_tp_param_fact_t *p = &f->params.data[idx];
  p->observations++;
  if (!p->inferred_type) {
    p->inferred_type = type;
    return;
  }
  if (strcmp(p->inferred_type, type) != 0)
    p->conflict = true;
  free(type);
}

static void tp_collect_func_defs_stmt(ny_tp_ctx_t *ctx, stmt_t *s,
                                      const char *owner) {
  if (!ctx || !s)
    return;
  switch (s->kind) {
  case NY_S_FUNC:
    if (tp_stmt_in_scope(ctx, s)) {
      ny_tp_func_fact_t f = {.stmt = s, .name = s->as.fn.name, .owner = owner};
      vec_init(&f.params);
      for (size_t i = 0; i < s->as.fn.params.len; ++i) {
        ny_tp_param_fact_t p = {0};
        vec_push(&f.params, p);
      }
      vec_push(&ctx->funcs, f);
    }
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_collect_func_defs_stmt(ctx, s->as.module.body.data[i],
                                s->as.module.name);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      tp_collect_func_defs_stmt(ctx, s->as.impl.methods.data[i],
                                s->as.impl.type_name);
    break;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      tp_collect_func_defs_stmt(ctx, s->as.layout.methods.data[i],
                                s->as.layout.name);
    break;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      tp_collect_func_defs_stmt(ctx, s->as.struc.methods.data[i],
                                s->as.struc.name);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_collect_func_defs_stmt(ctx, s->as.block.body.data[i], owner);
    break;
  default:
    break;
  }
}

static void tp_collect_constraints_expr(ny_tp_ctx_t *ctx, ny_tp_env_t *env,
                                        expr_t *e);

static void tp_collect_call_observation(ny_tp_ctx_t *ctx, ny_tp_env_t *env,
                                        const char *name, expr_t *target,
                                        ny_call_arg_list *args) {
  ny_tp_func_fact_t *f = tp_find_func(ctx, name);
  if (!f)
    return;
  bool useful = false;
  size_t arg_offset = target ? 1u : 0u;
  if (target && f->params.len > 0) {
    char *tt = tp_expr_type(ctx, env, target, 0);
    if (tt && strcmp(tt, "any") != 0)
      useful = true;
    tp_observe_param(f, 0, tt);
  }
  for (size_t i = 0; args && i < args->len; ++i) {
    size_t param_idx = i + arg_offset;
    char *at = tp_expr_type(ctx, env, args->data[i].val, 0);
    if (at && strcmp(at, "any") != 0)
      useful = true;
    tp_observe_param(f, param_idx, at);
    tp_collect_constraints_expr(ctx, env, args->data[i].val);
  }
  if (useful) {
    f->call_candidates++;
    ctx->mono_candidates++;
  }
}

static void tp_collect_constraints_expr(ny_tp_ctx_t *ctx, ny_tp_env_t *env,
                                        expr_t *e) {
  if (!ctx || !e)
    return;
  switch (e->kind) {
  case NY_E_CALL:
    if (e->as.call.callee) {
      tp_collect_constraints_expr(ctx, env, e->as.call.callee);
      if (e->as.call.callee->kind == NY_E_IDENT)
        tp_collect_call_observation(ctx, env, e->as.call.callee->as.ident.name,
                                    NULL, &e->as.call.args);
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_collect_constraints_expr(ctx, env, e->as.call.args.data[i].val);
    break;
  case NY_E_MEMCALL: {
    if (e->as.memcall.target && e->as.memcall.name) {
      char module_path[1024];
      if (tp_resolve_module_expr_path(ctx ? ctx->cg : NULL,
                                      e->as.memcall.target, module_path,
                                      sizeof(module_path))) {
        char resolved_fun[1280];
        if (ny_resolve_module_function_path(ctx->cg, module_path,
                                            e->as.memcall.name, resolved_fun,
                                            sizeof(resolved_fun)))
          tp_collect_call_observation(ctx, env, resolved_fun, NULL,
                                      &e->as.memcall.args);
        tp_collect_constraints_expr(ctx, env, e->as.memcall.target);
        for (size_t i = 0; i < e->as.memcall.args.len; ++i)
          tp_collect_constraints_expr(ctx, env, e->as.memcall.args.data[i].val);
        break;
      }
    }
    char *target_t = tp_expr_type(ctx, env, e->as.memcall.target, 0);
    char full[512];
    snprintf(full, sizeof(full), "%s.%s",
             tp_skip_nullable(target_t ? target_t : "any"),
             e->as.memcall.name ? e->as.memcall.name : "");
    tp_collect_call_observation(ctx, env, full, e->as.memcall.target,
                                &e->as.memcall.args);
    free(target_t);
    tp_collect_constraints_expr(ctx, env, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_collect_constraints_expr(ctx, env, e->as.memcall.args.data[i].val);
    break;
  }
  case NY_E_BINARY:
    tp_collect_constraints_expr(ctx, env, e->as.binary.left);
    tp_collect_constraints_expr(ctx, env, e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    tp_collect_constraints_expr(ctx, env, e->as.logical.left);
    tp_collect_constraints_expr(ctx, env, e->as.logical.right);
    break;
  case NY_E_UNARY:
    tp_collect_constraints_expr(ctx, env, e->as.unary.right);
    break;
  case NY_E_TERNARY:
    tp_collect_constraints_expr(ctx, env, e->as.ternary.cond);
    tp_collect_constraints_expr(ctx, env, e->as.ternary.true_expr);
    tp_collect_constraints_expr(ctx, env, e->as.ternary.false_expr);
    break;
  case NY_E_INDEX:
    tp_collect_constraints_expr(ctx, env, e->as.index.target);
    tp_collect_constraints_expr(ctx, env, e->as.index.start);
    tp_collect_constraints_expr(ctx, env, e->as.index.stop);
    tp_collect_constraints_expr(ctx, env, e->as.index.step);
    break;
  case NY_E_MEMBER:
    tp_collect_constraints_expr(ctx, env, e->as.member.target);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      tp_collect_constraints_expr(ctx, env, e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      tp_collect_constraints_expr(ctx, env, e->as.dict.pairs.data[i].key);
      tp_collect_constraints_expr(ctx, env, e->as.dict.pairs.data[i].value);
    }
    break;
  default:
    break;
  }
}

static void tp_collect_constraints_stmt(ny_tp_ctx_t *ctx, ny_tp_env_t *env,
                                        stmt_t *s) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_FUNC: {
    ny_tp_env_t fn_env = {0};
    ny_tp_func_fact_t *fact = tp_find_func(ctx, s->as.fn.name);
    for (size_t i = 0; i < s->as.fn.params.len; ++i) {
      param_t *p = &s->as.fn.params.data[i];
      const char *pt = p->type ? p->type : tp_func_param_inferred(fact, i);
      tp_env_set(&fn_env, p->name, pt ? pt : "any", false);
    }
    tp_collect_constraints_stmt(ctx, &fn_env, s->as.fn.body);
    tp_env_dispose(&fn_env);
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const char *decl =
          i < s->as.var.types.len ? s->as.var.types.data[i] : NULL;
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      const char *old_t = tp_env_get(env, name);
      tp_collect_constraints_expr(ctx, env, init);
      char *it = init ? tp_expr_type(ctx, env, init, 0) : ny_strdup("any");
      const char *final_t = decl ? decl : it;
      char *old_copy = NULL;
      if (!s->as.var.is_decl && old_t && (!it || strcmp(it, "any") == 0)) {
        old_copy = ny_strdup(old_t);
        final_t = old_copy ? old_copy : old_t;
      }
      tp_env_set(env, name, final_t, s->as.var.is_mut);
      free(old_copy);
      free(it);
    }
    break;
  case NY_S_EXPR:
    tp_collect_constraints_expr(ctx, env, s->as.expr.expr);
    break;
  case NY_S_RETURN:
    tp_collect_constraints_expr(ctx, env, s->as.ret.value);
    break;
  case NY_S_IF:
    tp_collect_constraints_stmt(ctx, env, s->as.iff.init);
    tp_collect_constraints_expr(ctx, env, s->as.iff.test);
    tp_collect_constraints_stmt(ctx, env, s->as.iff.conseq);
    tp_collect_constraints_stmt(ctx, env, s->as.iff.alt);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_collect_constraints_stmt(ctx, env, s->as.block.body.data[i]);
    break;
  case NY_S_WHILE:
    tp_collect_constraints_stmt(ctx, env, s->as.whl.init);
    tp_collect_constraints_expr(ctx, env, s->as.whl.test);
    tp_collect_constraints_stmt(ctx, env, s->as.whl.body);
    tp_collect_constraints_stmt(ctx, env, s->as.whl.update);
    break;
  case NY_S_FOR:
    tp_collect_constraints_stmt(ctx, env, s->as.fr.init);
    tp_collect_constraints_expr(ctx, env, s->as.fr.cond);
    tp_collect_constraints_expr(ctx, env, s->as.fr.iterable);
    if (s->as.fr.iter_var)
      tp_env_set(env, s->as.fr.iter_var, "any", true);
    if (s->as.fr.iter_index_var)
      tp_env_set(env, s->as.fr.iter_index_var, "int", true);
    tp_collect_constraints_stmt(ctx, env, s->as.fr.body);
    tp_collect_constraints_stmt(ctx, env, s->as.fr.update);
    break;
  case NY_S_TRY:
    tp_collect_constraints_stmt(ctx, env, s->as.tr.body);
    tp_collect_constraints_stmt(ctx, env, s->as.tr.handler);
    break;
  case NY_S_MATCH:
    tp_collect_constraints_expr(ctx, env, s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      tp_collect_constraints_stmt(ctx, env, s->as.match.arms.data[i].conseq);
    tp_collect_constraints_stmt(ctx, env, s->as.match.default_conseq);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_collect_constraints_stmt(ctx, env, s->as.module.body.data[i]);
    break;
  default:
    break;
  }
}

static void tp_ctx_build(ny_tp_ctx_t *ctx, program_t *prog, codegen_t *cg,
                         const char *source_name, bool include_std) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->prog = prog;
  ctx->cg = cg;
  ctx->source_name = source_name;
  ctx->include_std = include_std;
  vec_init(&ctx->funcs);
  vec_init(&ctx->diagnostics);
  vec_init(&ctx->fallbacks);
  tp_solver_init(ctx);
  if (prog) {
    for (size_t i = 0; i < prog->body.len; ++i)
      tp_collect_func_defs_stmt(ctx, prog->body.data[i], NULL);
    ny_tp_env_t env = {0};
    for (size_t i = 0; i < prog->body.len; ++i)
      tp_collect_constraints_stmt(ctx, &env, prog->body.data[i]);
    tp_env_dispose(&env);
  }
}

static void tp_ctx_dispose(ny_tp_ctx_t *ctx) {
  if (!ctx)
    return;
  for (size_t i = 0; i < ctx->funcs.len; ++i) {
    ny_tp_func_fact_t *f = &ctx->funcs.data[i];
    for (size_t j = 0; j < f->params.len; ++j)
      free(f->params.data[j].inferred_type);
    vec_free(&f->params);
  }
  vec_free(&ctx->funcs);
  for (size_t i = 0; i < ctx->diagnostics.len; ++i)
    tp_diag_dispose(&ctx->diagnostics.data[i]);
  vec_free(&ctx->diagnostics);
  for (size_t i = 0; i < ctx->fallbacks.len; ++i)
    free(ctx->fallbacks.data[i].reason);
  vec_free(&ctx->fallbacks);
}

typedef struct ny_tp_scan_fn_t {
  ny_tp_ctx_t *ctx;
  ny_tp_env_t env;
  ny_tp_json_t locals;
  bool first_local;
  char *return_type;
  size_t return_count;
  size_t nullable_guards;
} ny_tp_scan_fn_t;

static void tp_scan_function_stmt(ny_tp_scan_fn_t *scan, stmt_t *s);

static void tp_scan_return(ny_tp_scan_fn_t *scan, expr_t *value) {
  char *rt =
      value ? tp_expr_type(scan->ctx, &scan->env, value, 0) : ny_strdup("nil");
  scan->return_type = tp_merge_types_take(scan->return_type, rt);
  scan->return_count++;
}

static const char *tp_nil_cmp_var(expr_t *e, const char **op_out) {
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op)
    return NULL;
  bool nil_left = ny_expr_is_nil_literal(e->as.binary.left);
  bool nil_right = ny_expr_is_nil_literal(e->as.binary.right);
  if (!nil_left && !nil_right)
    return NULL;
  expr_t *other = nil_left ? e->as.binary.right : e->as.binary.left;
  if (!other || other->kind != NY_E_IDENT)
    return NULL;
  if (strcmp(e->as.binary.op, "==") != 0 && strcmp(e->as.binary.op, "!=") != 0)
    return NULL;
  if (op_out)
    *op_out = e->as.binary.op;
  return other->as.ident.name;
}

static void tp_count_nullable_guards(ny_tp_scan_fn_t *scan, expr_t *e) {
  if (!scan || !e)
    return;
  const char *op = NULL;
  if (tp_nil_cmp_var(e, &op)) {
    (void)op;
    scan->nullable_guards++;
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    tp_count_nullable_guards(scan, e->as.logical.left);
    tp_count_nullable_guards(scan, e->as.logical.right);
  }
}

static void tp_emit_local_fact(ny_tp_scan_fn_t *scan, const char *name,
                               const char *decl, const char *inferred,
                               bool is_mut) {
  if (!scan || !name)
    return;
  if (!scan->first_local)
    tp_append(&scan->locals, ",");
  scan->first_local = false;
  tp_append(&scan->locals, "{\"name\":");
  tp_json_str(&scan->locals, name);
  tp_append(&scan->locals, ",\"declared\":");
  tp_json_str(&scan->locals, decl ? decl : "");
  tp_append(&scan->locals, ",\"inferred\":");
  tp_json_str(&scan->locals, inferred ? inferred : "any");
  tp_append(&scan->locals, ",\"nullable\":%s,\"mutable\":%s}",
            (decl && decl[0] == '?') || (inferred && inferred[0] == '?')
                ? "true"
                : "false",
            is_mut ? "true" : "false");
}

static void tp_scan_function_stmt(ny_tp_scan_fn_t *scan, stmt_t *s) {
  if (!scan || !s)
    return;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const char *decl =
          i < s->as.var.types.len ? s->as.var.types.data[i] : NULL;
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      char *it = init ? tp_expr_type(scan->ctx, &scan->env, init, 0)
                      : ny_strdup("any");
      const char *final_t = decl && *decl ? decl : it;
      tp_emit_local_fact(scan, name, decl, final_t, s->as.var.is_mut);
      tp_env_set(&scan->env, name, final_t, s->as.var.is_mut);
      free(it);
    }
    break;
  case NY_S_RETURN:
    tp_scan_return(scan, s->as.ret.value);
    break;
  case NY_S_EXPR:
    break;
  case NY_S_IF:
    tp_scan_function_stmt(scan, s->as.iff.init);
    tp_count_nullable_guards(scan, s->as.iff.test);
    tp_scan_function_stmt(scan, s->as.iff.conseq);
    tp_scan_function_stmt(scan, s->as.iff.alt);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_scan_function_stmt(scan, s->as.block.body.data[i]);
    break;
  case NY_S_WHILE:
    tp_scan_function_stmt(scan, s->as.whl.init);
    tp_count_nullable_guards(scan, s->as.whl.test);
    tp_scan_function_stmt(scan, s->as.whl.body);
    tp_scan_function_stmt(scan, s->as.whl.update);
    break;
  case NY_S_FOR:
    tp_scan_function_stmt(scan, s->as.fr.init);
    if (s->as.fr.iter_var) {
      tp_emit_local_fact(scan, s->as.fr.iter_var, "", "any", true);
      tp_env_set(&scan->env, s->as.fr.iter_var, "any", true);
    }
    if (s->as.fr.iter_index_var) {
      tp_emit_local_fact(scan, s->as.fr.iter_index_var, "", "int", true);
      tp_env_set(&scan->env, s->as.fr.iter_index_var, "int", true);
    }
    tp_scan_function_stmt(scan, s->as.fr.body);
    tp_scan_function_stmt(scan, s->as.fr.update);
    break;
  case NY_S_TRY:
    tp_scan_function_stmt(scan, s->as.tr.body);
    tp_scan_function_stmt(scan, s->as.tr.handler);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      tp_scan_function_stmt(scan, s->as.match.arms.data[i].conseq);
    tp_scan_function_stmt(scan, s->as.match.default_conseq);
    break;
  default:
    break;
  }
}

static void tp_emit_function_json(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                  ny_tp_func_fact_t *fact, bool *first) {
  stmt_t *fn = fact ? fact->stmt : NULL;
  if (!j || !ctx || !fn || fn->kind != NY_S_FUNC || !tp_stmt_in_scope(ctx, fn))
    return;
  ny_tp_scan_fn_t scan = {.ctx = ctx, .first_local = true};
  vec_init(&scan.env.vars);
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    param_t *p = &fn->as.fn.params.data[i];
    const char *pt = p->type ? p->type : tp_func_param_inferred(fact, i);
    tp_env_set(&scan.env, p->name, pt ? pt : "any", false);
  }
  tp_scan_function_stmt(&scan, fn->as.fn.body);
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"name\":");
  tp_json_str(j, fn->as.fn.name ? fn->as.fn.name : "");
  tp_append(j, ",\"owner\":");
  tp_json_str(j, fact && fact->owner ? fact->owner : "");
  tp_append(j, ",\"return_decl\":");
  tp_json_str(j, fn->as.fn.return_type ? fn->as.fn.return_type : "");
  tp_append(j, ",\"return_inferred\":");
  tp_json_str(j, scan.return_type ? scan.return_type : "");
  tp_append(j,
            ",\"return_count\":%zu,\"nullable_guard_count\":%zu,\"params\":[",
            scan.return_count, scan.nullable_guards);
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (i)
      tp_append(j, ",");
    param_t *p = &fn->as.fn.params.data[i];
    const char *inferred = tp_func_param_inferred(fact, i);
    bool conflict =
        fact && i < fact->params.len && fact->params.data[i].conflict;
    size_t observations =
        fact && i < fact->params.len ? fact->params.data[i].observations : 0;
    tp_append(j, "{\"name\":");
    tp_json_str(j, p->name ? p->name : "");
    tp_append(j, ",\"declared\":");
    tp_json_str(j, p->type ? p->type : "");
    tp_append(j, ",\"inferred\":");
    tp_json_str(j, p->type ? p->type : (inferred ? inferred : "any"));
    tp_append(j, ",\"observations\":%zu,\"conflict\":%s}", observations,
              conflict ? "true" : "false");
  }
  tp_append(j, "],\"locals\":[");
  if (scan.locals.data)
    tp_append(j, "%s", scan.locals.data);
  tp_append(j, "]}");
  free(scan.locals.data);
  free(scan.return_type);
  tp_env_dispose(&scan.env);
}

typedef enum ny_hm_kind_t {
  NY_HM_ANY,
  NY_HM_VAR,
  NY_HM_NAME,
  NY_HM_LIST,
  NY_HM_SET,
  NY_HM_TUPLE,
  NY_HM_DICT,
  NY_HM_NULLABLE,
  NY_HM_PTR,
  NY_HM_FN,
  NY_HM_UNION,
  NY_HM_INDEXABLE,
} ny_hm_kind_t;

typedef struct ny_hm_type_t ny_hm_type_t;
typedef VEC(ny_hm_type_t *) ny_hm_type_list;

struct ny_hm_type_t {
  ny_hm_kind_t kind;
  int id;
  char *name;
  ny_hm_type_t *a;
  ny_hm_type_t *b;
  ny_hm_type_list args;
  ny_hm_type_t *instance;
};

typedef struct ny_hm_env_entry_t {
  const char *name;
  ny_hm_type_t *type;
} ny_hm_env_entry_t;
typedef VEC(ny_hm_env_entry_t) ny_hm_env_list;

typedef struct ny_hm_scheme_t {
  const char *name;
  char *full_name;
  const char *owner;
  stmt_t *stmt;
  ny_hm_type_t *type;
} ny_hm_scheme_t;
typedef VEC(ny_hm_scheme_t) ny_hm_scheme_list;

typedef struct ny_hm_state_t {
  ny_tp_ctx_t *ctx;
  int next_var;
  int dynamic_lambda_depth;
  arena_t arena;
  size_t type_nodes;
  int allow_dynamic_literal_depth;
  ny_hm_scheme_list schemes;
} ny_hm_state_t;

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

static bool hm_type_is_callable(ny_hm_type_t *t) {
  t = hm_prune(t);
  return t && (t->kind == NY_HM_FN ||
               (t->kind == NY_HM_NAME && hm_is_callable_name(t->name)));
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
      "Strict type mode rejects dynamic fallbacks that hide performance or "
      "safety cliffs.",
      "Add an explicit annotation, use a layout guard/converter, return "
      "Result<T,E>, or intentionally annotate the value as any.",
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
         (name && strcmp(name, "number") == 0);
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
    out = malloc(strlen(a ? a : "any") + 2);
    if (out)
      snprintf(out, strlen(a ? a : "any") + 2, "?%s", a ? a : "any");
    break;
  case NY_HM_PTR:
    out = malloc(strlen(a ? a : "any") + 2);
    if (out)
      snprintf(out, strlen(a ? a : "any") + 2, "*%s", a ? a : "any");
    break;
  case NY_HM_DICT:
    out = malloc(strlen(a ? a : "any") + strlen(b ? b : "any") + 9);
    if (out)
      snprintf(out, strlen(a ? a : "any") + strlen(b ? b : "any") + 9,
               "dict<%s, %s>", a ? a : "any", b ? b : "any");
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
    out = malloc(strlen(a ? a : "any") + strlen(b ? b : "any") + 2);
    if (out)
      snprintf(out, strlen(a ? a : "any") + strlen(b ? b : "any") + 2, "%s|%s",
               a ? a : "any", b ? b : "any");
    break;
  case NY_HM_INDEXABLE:
    name = "indexable";
    break;
  default:
    break;
  }
  if (!out && name) {
    out = malloc(strlen(name) + strlen(a ? a : "any") + 3);
    if (out)
      snprintf(out, strlen(name) + strlen(a ? a : "any") + 3, "%s<%s>", name,
               a ? a : "any");
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
  if (!hm_strict_types_enabled(hm, tok))
    return;
  if (hm_type_is_result_type(result_t) && !hm_type_is_dynamic(payload_t))
    return;
  char *got = hm_type_string(result_t);
  hm_strict_diag(hm, tok, "hm-strict-result-payload",
                 context ? context : "result payload refinement",
                 "Result<T, E> with statically known payload",
                 got ? got : "any", "result",
                 "strict type mode requires Result<T, E> payload evidence "
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
    hm_strict_diag(
        hm, tok, "hm-strict-dynamic-call",
        context ? context : "call expression", "known callable signature",
        "dynamic any", "call",
        "strict type mode rejects calls through values inferred as any");
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
    hm_strict_diag(
        hm, tok, "hm-strict-dynamic-call",
        context ? context : "call expression", "fn(...) -> T signature",
        callee_t->name ? callee_t->name : "callable", "call",
        "strict type mode rejects calls whose return payload is unknown");
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
      strcmp(leaf, "assert_compile_range") == 0 ||
      strcmp(leaf, "assert_compile_index") == 0)
    return hm_name(hm, "bool");
  if (strcmp(name, "malloc") == 0)
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
      hm_strict_diag(hm, tok, "hm-strict-dynamic-call", "function call",
                     "known callable", name ? name : "<anonymous>", "call",
                     "strict type mode rejects unresolved member or qualified "
                     "calls that would return any");
      return hm_any(hm);
    }
    hm_strict_diag(
        hm, tok, "hm-strict-dynamic-call", "function call", "known callable",
        name ? name : "<anonymous>", "call",
        "strict type mode rejects unknown calls that would return any");
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
      hm_strict_diag(hm, e->tok, "hm-strict-dynamic-arithmetic",
                     "comparison expression", "statically typed operands", got,
                     "binary",
                     "strict type mode rejects dynamic comparison operands");
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
    hm_strict_diag(hm, e->tok, "hm-strict-dynamic-arithmetic",
                   "arithmetic expression",
                   "numeric/string/collection operands", got, "binary",
                   "strict type mode rejects dynamic arithmetic operands");
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
                       "strict type mode rejects heterogeneous dict literals "
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
      ny_hm_type_t *target_t =
          hm_infer_expr(hm, env, e->as.member.target, self_name);
      if (hm_type_is_unbound_var(target_t))
        return hm_any(hm);
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
  case NY_E_DEREF:
    return hm_name(hm, "ptr");
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

static void hm_infer_stmt_list(ny_hm_state_t *hm, ny_hm_env_list *env,
                               ny_stmt_list *body, const char *self_name,
                               ny_hm_type_t *ret) {
  hm_infer_stmt_list_mode(hm, env, body, self_name, ret, true);
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

char *ny_type_pipeline_typed_json(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  ny_tp_json_t j = {0};
  tp_append(
      &j,
      "{\"engine\":\"type-pipeline-v1\",\"schema\":\"typed_ast.v1\","
      "\"notes\":[\"HM principal type facts for the current Ny surface\"],"
      "\"stats\":{\"functions\":%zu,\"mono_candidates\":%zu,\"hm_errors\":%zu,"
      "\"hm_type_nodes\":%zu,\"dynamic_fallbacks\":%zu,\"strict_types\":%s},"
      "\"solver\":",
      ctx.funcs.len, ctx.mono_candidates, ctx.diagnostics.len, hm.type_nodes,
      ctx.fallbacks.len, (cg && cg->strict_types) ? "true" : "false");
  tp_emit_solver_json(&j, &ctx);
  tp_append(&j, ",\"schemes\":");
  hm_emit_schemes_json(&j, &hm);
  tp_append(&j, ",\"functions\":[");
  bool first = true;
  for (size_t i = 0; i < ctx.funcs.len; ++i)
    tp_emit_function_json(&j, &ctx, &ctx.funcs.data[i], &first);
  tp_append(&j, "],\"diagnostics\":");
  tp_emit_diagnostics_json(&j, &ctx);
  tp_append(&j, ",\"hm_fallbacks\":");
  tp_emit_fallbacks_json(&j, &ctx);
  tp_append(&j, ",\"dynamic_fallbacks\":");
  tp_emit_fallbacks_json(&j, &ctx);
  tp_append(&j, "}");
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"engine\":\"type-pipeline-v1\",\"functions\":[]}");
}

static void tp_validate_operator_decls(ny_tp_ctx_t *ctx);
static void tp_validate_impl_coherence(ny_tp_ctx_t *ctx);
static void tp_emit_operator_obligations_stmt(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                              ny_tp_env_t *env, stmt_t *s,
                                              bool *first);
static void tp_validate_abi_program(ny_tp_ctx_t *ctx);

int ny_type_pipeline_validate_hm(program_t *prog, codegen_t *cg,
                                 const char *source_name, bool include_std,
                                 bool emit_diagnostics) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  int count = (int)ctx.diagnostics.len;
  if (emit_diagnostics) {
    for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
      const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
      tp_emit_user_diag(d, "HM validation failed");
    }
  }
  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return count;
}

int ny_type_pipeline_validate_semantics(
    program_t *prog, codegen_t *cg, const char *source_name, bool include_std,
    ny_type_pipeline_stage_t max_stage, ny_type_pipeline_stage_t *failed_stage,
    bool emit_diagnostics, char **errors_json_out) {
  if (failed_stage)
    *failed_stage = NY_TYPE_PIPELINE_STAGE_OK;
  if (errors_json_out)
    *errors_json_out = NULL;
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);

  ny_hm_state_t hm;
  hm_run(&hm, &ctx);
  if (ctx.diagnostics.len > 0) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_HM;
    if (emit_diagnostics) {
      for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
        const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
        tp_emit_user_diag(d, "HM validation failed");
      }
    }
    int count = (int)ctx.diagnostics.len;
    if (errors_json_out)
      *errors_json_out = tp_errors_v1_json(&ctx, "hm", source_name,
                                           "HM type validation failed");
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return count;
  }
  if (max_stage <= NY_TYPE_PIPELINE_STAGE_HM) {
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return 0;
  }

  tp_validate_operator_decls(&ctx);
  tp_validate_impl_coherence(&ctx);
  bool first = true;
  ny_tp_env_t env = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_operator_obligations_stmt(NULL, &ctx, &env, prog->body.data[i],
                                      &first);
  tp_env_dispose(&env);
  if (ctx.diagnostics.len > 0) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_TRAIT;
    if (emit_diagnostics) {
      for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
        const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
        tp_emit_user_diag(d, "trait validation failed");
      }
    }
    int count = (int)ctx.diagnostics.len;
    if (errors_json_out)
      *errors_json_out = tp_errors_v1_json(&ctx, "trait", source_name,
                                           "trait validation failed");
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return count;
  }
  if (max_stage <= NY_TYPE_PIPELINE_STAGE_TRAIT) {
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return 0;
  }

  tp_validate_abi_program(&ctx);
  if (ctx.diagnostics.len > 0) {
    if (failed_stage)
      *failed_stage = NY_TYPE_PIPELINE_STAGE_ABI;
    if (emit_diagnostics) {
      for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
        const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
        tp_emit_user_diag(d, "ABI validation failed");
      }
    }
    int count = (int)ctx.diagnostics.len;
    if (errors_json_out)
      *errors_json_out =
          tp_errors_v1_json(&ctx, "abi", source_name, "ABI validation failed");
    hm_dispose(&hm);
    tp_ctx_dispose(&ctx);
    return count;
  }

  hm_dispose(&hm);
  tp_ctx_dispose(&ctx);
  return 0;
}

static bool tp_operator_module_active(ny_tp_ctx_t *ctx,
                                      const ny_operator_def_t *def) {
  if (!def || !def->module_name || !*def->module_name)
    return true;
  if (ctx && ctx->cg && ctx->cg->current_module_name &&
      strcmp(ctx->cg->current_module_name, def->module_name) == 0)
    return true;
  return ctx && ny_is_module_active(ctx->cg, def->module_name);
}

static bool tp_type_compatible_for_decl(const char *want, const char *got) {
  if (!want || !*want || !got || !*got)
    return true;
  if (strcmp(want, got) == 0 || strcmp(want, "any") == 0 ||
      strcmp(got, "any") == 0)
    return true;
  const char *w = tp_skip_nullable(want);
  const char *g = tp_skip_nullable(got);
  if (strcmp(w, g) == 0)
    return true;
  if (tp_type_is_group(w) && tp_type_group_accepts(w, g))
    return true;
  if (tp_type_is_group(g) && tp_type_group_accepts(g, w))
    return true;
  if (strcmp(w, "number") == 0 && (tp_is_int_type(g) || tp_is_float_type(g)))
    return true;
  if (strcmp(w, "integer") == 0 && tp_is_int_type(g))
    return true;
  if (strcmp(w, "float") == 0 && tp_is_float_type(g))
    return true;
  if (strcmp(w, "nil") == 0 && (strcmp(g, "nil") == 0 || got[0] == '?'))
    return true;
  return false;
}

static fun_sig *tp_operator_target_sig(ny_tp_ctx_t *ctx,
                                       const ny_operator_def_t *def) {
  return tp_lookup_sig(ctx ? ctx->cg : NULL, def ? def->target_name : NULL);
}

static ny_operator_def_t *tp_find_operator_def(ny_tp_ctx_t *ctx, const char *op,
                                               const char *lt, const char *rt,
                                               size_t *count_out) {
  size_t count = 0;
  ny_operator_def_t *found = NULL;
  if (ctx && ctx->cg && op && lt && rt) {
    for (size_t i = ctx->cg->operators.len; i > 0; --i) {
      ny_operator_def_t *def = &ctx->cg->operators.data[i - 1];
      if (!def->op || strcmp(def->op, op) != 0)
        continue;
      if (!tp_operator_module_active(ctx, def))
        continue;
      if (!tp_type_eq(def->left_type, lt) || !tp_type_eq(def->right_type, rt))
        continue;
      count++;
      if (!found)
        found = def;
    }
  }
  if (count_out)
    *count_out = count;
  return found;
}

static bool tp_operator_has_previous_duplicate(ny_tp_ctx_t *ctx,
                                               const ny_operator_def_t *def) {
  if (!ctx || !ctx->cg || !def)
    return false;
  for (size_t i = 0; i < ctx->cg->operators.len; ++i) {
    ny_operator_def_t *other = &ctx->cg->operators.data[i];
    if (other == def)
      return false;
    if (!tp_stmt_in_scope(ctx, other->stmt))
      continue;
    if (tp_type_eq(other->op, def->op) &&
        tp_type_eq(other->left_type, def->left_type) &&
        tp_type_eq(other->right_type, def->right_type))
      return true;
  }
  return false;
}

static void tp_validate_operator_decl(ny_tp_ctx_t *ctx,
                                      ny_operator_def_t *def) {
  if (!ctx || !def || !tp_stmt_in_scope(ctx, def->stmt))
    return;
  token_t tok = def->stmt ? def->stmt->tok : (token_t){0};
  if (tp_operator_has_previous_duplicate(ctx, def)) {
    tp_add_diag(ctx, tok, "trait", "operator-duplicate",
                "duplicate operator '%s' for %s and %s",
                def->op ? def->op : "?",
                def->left_type ? def->left_type : "unknown",
                def->right_type ? def->right_type : "unknown");
    return;
  }

  fun_sig *target = tp_operator_target_sig(ctx, def);
  if (!target) {
    tp_add_diag(ctx, tok, "trait", "operator-target-missing",
                "operator target '%s' is not defined",
                def->target_name ? def->target_name : "<missing>");
    return;
  }
  if (!target->is_variadic && target->arity != 2) {
    tp_add_diag(ctx, tok, "trait", "operator-target-arity",
                "operator target '%s' must take exactly two arguments",
                def->target_name ? def->target_name : "<missing>");
  }

  const char *left_param = ny_sig_param_type(target, 0);
  const char *right_param = ny_sig_param_type(target, 1);
  if (left_param && !tp_type_compatible_for_decl(left_param, def->left_type)) {
    tp_add_diag(ctx, tok, "trait", "operator-left-mismatch",
                "operator target '%s' expects %s for left operand but operator "
                "declares %s",
                def->target_name ? def->target_name : "<missing>", left_param,
                def->left_type ? def->left_type : "unknown");
  }
  if (right_param &&
      !tp_type_compatible_for_decl(right_param, def->right_type)) {
    tp_add_diag(ctx, tok, "trait", "operator-right-mismatch",
                "operator target '%s' expects %s for right operand but "
                "operator declares %s",
                def->target_name ? def->target_name : "<missing>", right_param,
                def->right_type ? def->right_type : "unknown");
  }

  const char *target_ret = target->return_type && *target->return_type
                               ? target->return_type
                               : target->inferred_return_type;
  if (target_ret && def->return_type && *def->return_type &&
      !tp_type_compatible_for_decl(def->return_type, target_ret)) {
    tp_add_diag(ctx, tok, "trait", "operator-return-mismatch",
                "operator target '%s' returns %s but operator declares %s",
                def->target_name ? def->target_name : "<missing>", target_ret,
                def->return_type);
  }
}

static void tp_validate_operator_decls(ny_tp_ctx_t *ctx) {
  if (!ctx || !ctx->cg)
    return;
  for (size_t i = 0; i < ctx->cg->operators.len; ++i)
    tp_validate_operator_decl(ctx, &ctx->cg->operators.data[i]);
}

typedef struct tp_impl_method_entry_t {
  const char *owner;
  const char *method;
  size_t arity;
  token_t tok;
} tp_impl_method_entry_t;
typedef VEC(tp_impl_method_entry_t) tp_impl_method_entry_list;

static const char *tp_method_tail(const char *name) {
  if (!name)
    return "";
  const char *tail = strrchr(name, '.');
  return tail ? tail + 1 : name;
}

static void tp_impl_coherence_add(ny_tp_ctx_t *ctx,
                                  tp_impl_method_entry_list *entries,
                                  const char *owner, stmt_t *method) {
  if (!ctx || !entries || !owner || !*owner || !method ||
      method->kind != NY_S_FUNC || !tp_stmt_in_scope(ctx, method))
    return;
  const char *name = tp_method_tail(method->as.fn.name);
  size_t arity = method->as.fn.params.len;
  for (size_t i = 0; i < entries->len; ++i) {
    tp_impl_method_entry_t *prev = &entries->data[i];
    if (prev->owner && strcmp(prev->owner, owner) == 0 && prev->method &&
        strcmp(prev->method, name) == 0 && prev->arity == arity) {
      tp_add_diag(
          ctx, method->tok, "trait", "impl-method-duplicate",
          "duplicate impl method '%s.%s/%zu' overlaps a visible impl at %s:%d",
          owner, name, arity,
          prev->tok.filename ? prev->tok.filename : "unknown", prev->tok.line);
      return;
    }
  }
  tp_impl_method_entry_t entry = {
      .owner = owner, .method = name, .arity = arity, .tok = method->tok};
  vec_push(entries, entry);
}

static void tp_validate_impl_coherence_stmt(ny_tp_ctx_t *ctx,
                                            tp_impl_method_entry_list *entries,
                                            stmt_t *s) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      tp_impl_coherence_add(ctx, entries, s->as.impl.type_name,
                            s->as.impl.methods.data[i]);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_validate_impl_coherence_stmt(ctx, entries, s->as.module.body.data[i]);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_validate_impl_coherence_stmt(ctx, entries, s->as.block.body.data[i]);
    break;
  default:
    break;
  }
}

static void tp_validate_impl_coherence(ny_tp_ctx_t *ctx) {
  tp_impl_method_entry_list entries = {0};
  for (size_t i = 0; ctx && ctx->prog && i < ctx->prog->body.len; ++i)
    tp_validate_impl_coherence_stmt(ctx, &entries, ctx->prog->body.data[i]);
  vec_free(&entries);
}

static void tp_emit_group_contract(ny_tp_json_t *j, bool *first,
                                   const char *owner, const char *name,
                                   const char *slot, const char *group,
                                   const char *kind) {
  if (!j || !first || !group || !tp_type_is_group(group))
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"kind\":");
  tp_json_str(j, kind ? kind : "value");
  tp_append(j, ",\"owner\":");
  tp_json_str(j, owner ? owner : "");
  tp_append(j, ",\"name\":");
  tp_json_str(j, name ? name : "");
  tp_append(j, ",\"slot\":");
  tp_json_str(j, slot ? slot : "");
  tp_append(j, ",\"group\":");
  tp_json_str(j, tp_group_canon(group));
  tp_append(j, ",\"capability\":");
  tp_json_str(j, tp_group_capability(group));
  tp_append(j, ",\"zero_cost\":true,\"status\":\"static-conformance\"}");
}

static void tp_emit_group_conformance_json(ny_tp_json_t *j, ny_tp_ctx_t *ctx) {
  bool first = true;
  tp_append(j, "[");
  for (size_t i = 0; ctx && i < ctx->funcs.len; ++i) {
    ny_tp_func_fact_t *fact = &ctx->funcs.data[i];
    stmt_t *fn = fact->stmt;
    if (!fn || fn->kind != NY_S_FUNC || !tp_stmt_in_scope(ctx, fn))
      continue;
    for (size_t k = 0; k < fn->as.fn.params.len; ++k) {
      param_t *p = &fn->as.fn.params.data[k];
      tp_emit_group_contract(j, &first, fact->owner, fn->as.fn.name, p->name,
                             p->type, "param");
    }
    tp_emit_group_contract(j, &first, fact->owner, fn->as.fn.name, "return",
                           fn->as.fn.return_type, "return");
  }
  for (size_t i = 0; ctx && ctx->cg && i < ctx->cg->operators.len; ++i) {
    ny_operator_def_t *op = &ctx->cg->operators.data[i];
    if (!tp_stmt_in_scope(ctx, op->stmt))
      continue;
    tp_emit_group_contract(j, &first, "", op->target_name, "left",
                           op->left_type, "operator");
    tp_emit_group_contract(j, &first, "", op->target_name, "right",
                           op->right_type, "operator");
    tp_emit_group_contract(j, &first, "", op->target_name, "return",
                           op->return_type, "operator");
  }
  tp_append(j, "]");
}

static void tp_emit_operator_obligation_json(ny_tp_json_t *j, bool *first,
                                             const char *op, const char *lt,
                                             const char *rt,
                                             ny_operator_def_t *def,
                                             size_t matches) {
  if (!j || !first)
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"op\":");
  tp_json_str(j, op ? op : "");
  tp_append(j, ",\"left\":");
  tp_json_str(j, lt ? lt : "any");
  tp_append(j, ",\"right\":");
  tp_json_str(j, rt ? rt : "any");
  tp_append(j, ",\"status\":");
  tp_json_str(j, matches > 1 ? "ambiguous" : (def ? "resolved" : "unresolved"));
  tp_append(j, ",\"return\":");
  tp_json_str(j, def && def->return_type ? def->return_type : "");
  tp_append(j, ",\"target\":");
  tp_json_str(j, def && def->target_name ? def->target_name : "");
  tp_append(j, "}");
}

static void tp_emit_operator_obligations_expr(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                              ny_tp_env_t *env, expr_t *e,
                                              bool *first) {
  if (!ctx || !e)
    return;
  switch (e->kind) {
  case NY_E_BINARY: {
    char *lt = tp_expr_type(ctx, env, e->as.binary.left, 0);
    char *rt = tp_expr_type(ctx, env, e->as.binary.right, 0);
    const char *op = e->as.binary.op;
    if (op && (tp_is_arith_op(op) || tp_is_cmp_op(op))) {
      size_t matches = 0;
      ny_operator_def_t *def = tp_find_operator_def(ctx, op, lt, rt, &matches);
      bool equality = strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
      bool nominal = tp_is_nominal_custom_type(ctx, lt) ||
                     tp_is_nominal_custom_type(ctx, rt);
      bool needs_trait = matches > 0 || (!equality && nominal);
      if (needs_trait)
        tp_emit_operator_obligation_json(j, first, op, lt, rt, def, matches);
      if (needs_trait && matches == 0) {
        tp_add_diag(ctx, e->tok, "trait", "operator-unresolved",
                    "unresolved operator '%s' for %s and %s", op,
                    lt ? lt : "unknown", rt ? rt : "unknown");
      } else if (needs_trait && matches > 1) {
        tp_add_diag(ctx, e->tok, "trait", "operator-ambiguous",
                    "ambiguous operator '%s' for %s and %s", op,
                    lt ? lt : "unknown", rt ? rt : "unknown");
      }
    }
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.binary.left, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.binary.right, first);
    free(lt);
    free(rt);
    break;
  }
  case NY_E_CALL:
    if (e->as.call.callee)
      tp_emit_operator_obligations_expr(j, ctx, env, e->as.call.callee, first);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.call.args.data[i].val, first);
    break;
  case NY_E_MEMCALL:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.memcall.target, first);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.memcall.args.data[i].val, first);
    break;
  case NY_E_LOGICAL:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.logical.left, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.logical.right, first);
    break;
  case NY_E_UNARY:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.unary.right, first);
    break;
  case NY_E_TERNARY:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.ternary.cond, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.ternary.true_expr,
                                      first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.ternary.false_expr,
                                      first);
    break;
  case NY_E_INDEX:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.target, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.start, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.stop, first);
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.index.step, first);
    break;
  case NY_E_MEMBER:
    tp_emit_operator_obligations_expr(j, ctx, env, e->as.member.target, first);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      tp_emit_operator_obligations_expr(j, ctx, env, e->as.list_like.data[i],
                                        first);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.dict.pairs.data[i].key, first);
      tp_emit_operator_obligations_expr(j, ctx, env,
                                        e->as.dict.pairs.data[i].value, first);
    }
    break;
  default:
    break;
  }
}

static void tp_emit_operator_obligations_stmt(ny_tp_json_t *j, ny_tp_ctx_t *ctx,
                                              ny_tp_env_t *env, stmt_t *s,
                                              bool *first) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_FUNC: {
    ny_tp_env_t fn_env = {0};
    ny_tp_func_fact_t *fact = tp_find_func(ctx, s->as.fn.name);
    for (size_t i = 0; i < s->as.fn.params.len; ++i) {
      param_t *p = &s->as.fn.params.data[i];
      const char *pt = p->type ? p->type : tp_func_param_inferred(fact, i);
      tp_env_set(&fn_env, p->name, pt ? pt : "any", false);
    }
    tp_emit_operator_obligations_stmt(j, ctx, &fn_env, s->as.fn.body, first);
    tp_env_dispose(&fn_env);
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const char *decl =
          i < s->as.var.types.len ? s->as.var.types.data[i] : NULL;
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      tp_emit_operator_obligations_expr(j, ctx, env, init, first);
      char *it = init ? tp_expr_type(ctx, env, init, 0) : ny_strdup("any");
      tp_env_set(env, name, decl ? decl : it, s->as.var.is_mut);
      free(it);
    }
    break;
  case NY_S_EXPR:
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.expr.expr, first);
    break;
  case NY_S_RETURN:
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.ret.value, first);
    break;
  case NY_S_IF:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.iff.init, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.iff.test, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.iff.conseq, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.iff.alt, first);
    break;
  case NY_S_WHILE:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.whl.init, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.whl.test, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.whl.body, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.whl.update, first);
    break;
  case NY_S_FOR:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.fr.init, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.fr.cond, first);
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.fr.iterable, first);
    if (s->as.fr.iter_var)
      tp_env_set(env, s->as.fr.iter_var, "any", true);
    if (s->as.fr.iter_index_var)
      tp_env_set(env, s->as.fr.iter_index_var, "int", true);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.fr.body, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.fr.update, first);
    break;
  case NY_S_TRY:
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.tr.body, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.tr.handler, first);
    break;
  case NY_S_MATCH:
    tp_emit_operator_obligations_expr(j, ctx, env, s->as.match.test, first);
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      tp_emit_operator_obligations_stmt(j, ctx, env,
                                        s->as.match.arms.data[i].conseq, first);
    tp_emit_operator_obligations_stmt(j, ctx, env, s->as.match.default_conseq,
                                      first);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_emit_operator_obligations_stmt(j, ctx, env, s->as.block.body.data[i],
                                        first);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_emit_operator_obligations_stmt(j, ctx, env, s->as.module.body.data[i],
                                        first);
    break;
  default:
    break;
  }
}

char *ny_type_pipeline_resolved_json(program_t *prog, codegen_t *cg,
                                     const char *source_name,
                                     bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_tp_json_t j = {0};
  tp_append(&j, "{\"schema\":\"resolved.v1\",\"operators\":[");
  size_t emitted = 0;
  for (size_t i = 0; cg && i < cg->operators.len; ++i) {
    ny_operator_def_t *op = &cg->operators.data[i];
    if (!tp_stmt_in_scope(&ctx, op->stmt))
      continue;
    if (emitted++)
      tp_append(&j, ",");
    tp_append(&j, "{\"op\":");
    tp_json_str(&j, op->op ? op->op : "");
    tp_append(&j, ",\"left\":");
    tp_json_str(&j, op->left_type ? op->left_type : "");
    tp_append(&j, ",\"right\":");
    tp_json_str(&j, op->right_type ? op->right_type : "");
    tp_append(&j, ",\"return\":");
    tp_json_str(&j, op->return_type ? op->return_type : "");
    tp_append(&j, ",\"target\":");
    tp_json_str(&j, op->target_name ? op->target_name : "");
    tp_append(&j, "}");
  }
  tp_validate_operator_decls(&ctx);
  tp_validate_impl_coherence(&ctx);
  tp_append(&j, "],\"operator_obligations\":[");
  bool first_ob = true;
  ny_tp_env_t env = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_operator_obligations_stmt(&j, &ctx, &env, prog->body.data[i],
                                      &first_ob);
  tp_env_dispose(&env);
  tp_append(&j, "],\"type_groups\":");
  tp_emit_type_groups_json(&j);
  tp_append(&j, ",\"protocol_conformance\":");
  tp_emit_group_conformance_json(&j, &ctx);
  tp_append(&j, ",\"trait_conformance\":");
  tp_emit_group_conformance_json(&j, &ctx);
  tp_append(&j, ",\"diagnostics\":");
  tp_emit_diagnostics_json(&j, &ctx);
  tp_append(&j, "}");
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"schema\":\"resolved.v1\",\"operators\":[]}");
}

int ny_type_pipeline_validate_trait(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std,
                                    bool emit_diagnostics) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  tp_validate_operator_decls(&ctx);
  tp_validate_impl_coherence(&ctx);
  bool first = true;
  ny_tp_env_t env = {0};
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_operator_obligations_stmt(NULL, &ctx, &env, prog->body.data[i],
                                      &first);
  tp_env_dispose(&env);
  int count = (int)ctx.diagnostics.len;
  if (emit_diagnostics) {
    for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
      const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
      ny_diag_error(d->tok, "%s",
                    d->message ? d->message : "type validation failed");
    }
  }
  tp_ctx_dispose(&ctx);
  return count;
}

typedef struct ny_tp_flow_len_t {
  const char *name;
  int64_t len;
} ny_tp_flow_len_t;
typedef VEC(ny_tp_flow_len_t) ny_tp_flow_len_list;

static bool tp_expr_str_lit(expr_t *e, const char **out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  if (out)
    *out = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
  return true;
}

static const char *tp_expr_ident(expr_t *e) {
  return (e && e->kind == NY_E_IDENT) ? e->as.ident.name : NULL;
}

static int64_t tp_static_list_len(expr_t *e) {
  if (!e)
    return -1;
  if (e->kind == NY_E_LIST || e->kind == NY_E_TUPLE || e->kind == NY_E_SET)
    return (int64_t)e->as.list_like.len;
  return -1;
}

static void tp_flow_len_set(ny_tp_flow_len_list *env, const char *name,
                            int64_t len) {
  if (!env || !name || !*name || len < 0)
    return;
  for (size_t i = env->len; i > 0; --i) {
    if (env->data[i - 1].name && strcmp(env->data[i - 1].name, name) == 0) {
      env->data[i - 1].len = len;
      return;
    }
  }
  ny_tp_flow_len_t item = {.name = name, .len = len};
  vec_push(env, item);
}

static int64_t tp_flow_len_get(ny_tp_flow_len_list *env, const char *name) {
  if (!env || !name)
    return -1;
  for (size_t i = env->len; i > 0; --i)
    if (env->data[i - 1].name && strcmp(env->data[i - 1].name, name) == 0)
      return env->data[i - 1].len;
  return -1;
}

static void tp_emit_range_refinement_json(ny_tp_json_t *j, bool *first,
                                          const char *var, const char *source,
                                          bool has_min, int64_t min_v,
                                          bool has_max, int64_t max_v,
                                          const char *branch) {
  if (!j || !first || !var || !*var || (!has_min && !has_max))
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"var\":");
  tp_json_str(j, var);
  tp_append(j, ",\"source\":");
  tp_json_str(j, source ? source : "condition");
  tp_append(j, ",\"branch\":");
  tp_json_str(j, branch ? branch : "");
  if (has_min)
    tp_append(j, ",\"min\":%lld", (long long)min_v);
  if (has_max)
    tp_append(j, ",\"max\":%lld", (long long)max_v);
  tp_append(j, "}");
}

static const char *tp_reverse_cmp_op(const char *op) {
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
  return op;
}

static void tp_emit_range_condition(ny_tp_json_t *j, bool *first, expr_t *e,
                                    const char *source) {
  if (!j || !first || !e || e->kind != NY_E_BINARY || !e->as.binary.op)
    return;
  const char *var = tp_expr_ident(e->as.binary.left);
  int64_t v = 0;
  const char *op = e->as.binary.op;
  if (!var || !ny_expr_literal_i64(e->as.binary.right, &v)) {
    var = tp_expr_ident(e->as.binary.right);
    if (!var || !ny_expr_literal_i64(e->as.binary.left, &v))
      return;
    op = tp_reverse_cmp_op(e->as.binary.op);
  }
  if (strcmp(op, "<") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v - 1,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, true, v, false, 0,
                                  "else");
  } else if (strcmp(op, "<=") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, true, v + 1, false, 0,
                                  "else");
  } else if (strcmp(op, ">") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, true, v + 1, false, 0,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v,
                                  "else");
  } else if (strcmp(op, ">=") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, true, v, false, 0,
                                  "then");
    tp_emit_range_refinement_json(j, first, var, source, false, 0, true, v - 1,
                                  "else");
  } else if (strcmp(op, "==") == 0) {
    tp_emit_range_refinement_json(j, first, var, source, true, v, true, v,
                                  "then");
  }
}

static void tp_emit_index_proof_json(ny_tp_json_t *j, bool *first,
                                     const char *container, const char *index,
                                     int64_t index_lit, bool has_index_lit,
                                     int64_t len, const char *source) {
  if (!j || !first)
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  bool proven =
      len >= 0 && ((has_index_lit && index_lit >= 0 && index_lit < len) ||
                   (!has_index_lit && index && *index));
  tp_append(j, "{\"container\":");
  tp_json_str(j, container ? container : "");
  tp_append(j, ",\"index\":");
  if (has_index_lit) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)index_lit);
    tp_json_str(j, buf);
  } else {
    tp_json_str(j, index ? index : "");
  }
  tp_append(j, ",\"len\":%lld,\"source\":", (long long)len);
  tp_json_str(j, source ? source : "index");
  tp_append(j, ",\"status\":");
  tp_json_str(j, proven ? "static-len-known" : "observed");
  tp_append(j, "}");
}

static void tp_emit_discriminant_json(ny_tp_json_t *j, bool *first,
                                      const char *subject, const char *variant,
                                      const char *source, const char *binding) {
  if (!j || !first || !variant || !*variant)
    return;
  if (!*first)
    tp_append(j, ",");
  *first = false;
  tp_append(j, "{\"subject\":");
  tp_json_str(j, subject ? subject : "");
  tp_append(j, ",\"variant\":");
  tp_json_str(j, variant);
  tp_append(j, ",\"source\":");
  tp_json_str(j, source ? source : "match");
  tp_append(j, ",\"binding\":");
  tp_json_str(j, binding ? binding : "");
  tp_append(j, "}");
}

static void tp_emit_pattern_discriminant(ny_tp_json_t *j, bool *first,
                                         const char *subject, expr_t *pattern) {
  if (!pattern)
    return;
  if (pattern->kind == NY_E_IDENT) {
    tp_emit_discriminant_json(j, first, subject, pattern->as.ident.name,
                              "match", "");
  } else if (pattern->kind == NY_E_LITERAL) {
    char buf[96];
    if (pattern->as.literal.kind == NY_LIT_STR) {
      snprintf(buf, sizeof(buf), "str:%.*s", (int)pattern->as.literal.as.s.len,
               pattern->as.literal.as.s.data ? pattern->as.literal.as.s.data
                                             : "");
    } else if (pattern->as.literal.kind == NY_LIT_INT) {
      snprintf(buf, sizeof(buf), "int:%lld",
               (long long)pattern->as.literal.as.i);
    } else if (pattern->as.literal.kind == NY_LIT_BOOL) {
      snprintf(buf, sizeof(buf), "bool:%s",
               pattern->as.literal.as.b ? "true" : "false");
    } else {
      snprintf(buf, sizeof(buf), "literal");
    }
    tp_emit_discriminant_json(j, first, subject, buf, "match", "");
  }
}

static void tp_emit_flow_expr(ny_tp_json_t *nullable_j, ny_tp_json_t *range_j,
                              ny_tp_json_t *index_j, expr_t *e,
                              ny_tp_flow_len_list *lens, bool *first_nullable,
                              bool *first_range, bool *first_index) {
  if (!e)
    return;
  ny_null_narrow_list_t list;
  vec_init(&list);
  if (ny_null_narrow_collect(e, &list) && !ny_null_narrow_list_empty(&list)) {
    for (size_t i = 0; i < list.len; ++i) {
      const ny_null_narrow_info_t *info = &list.data[i];
      if (!info->name)
        continue;
      if (!*first_nullable)
        tp_append(nullable_j, ",");
      *first_nullable = false;
      tp_append(nullable_j, "{\"var\":");
      tp_json_str(nullable_j, info->name);
      tp_append(nullable_j, ",\"then\":");
      tp_json_str(nullable_j, info->true_nonnull ? "non-null" : "");
      tp_append(nullable_j, ",\"else\":");
      tp_json_str(nullable_j, info->false_nonnull ? "non-null" : "");
      tp_append(nullable_j, "}");
    }
  }
  vec_free(&list);
  tp_emit_range_condition(range_j, first_range, e, "condition");
  if (e->kind == NY_E_LOGICAL) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.logical.left, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.logical.right, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_UNARY) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.unary.right, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_BINARY) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.binary.left, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.binary.right, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_INDEX) {
    const char *container = tp_expr_ident(e->as.index.target);
    const char *idx_name = tp_expr_ident(e->as.index.start);
    int64_t idx_lit = 0;
    bool has_idx_lit = ny_expr_literal_i64(e->as.index.start, &idx_lit);
    int64_t len = tp_static_list_len(e->as.index.target);
    if (len < 0)
      len = tp_flow_len_get(lens, container);
    tp_emit_index_proof_json(index_j, first_index, container, idx_name, idx_lit,
                             has_idx_lit, len, "index-expression");
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.index.target, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.index.start, lens,
                      first_nullable, first_range, first_index);
  } else if (e->kind == NY_E_CALL) {
    const char *name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    if (name &&
        (strcmp(name, "assert_compile_range") == 0 ||
         strcmp(name, "range_proven") == 0) &&
        e->as.call.args.len >= 3) {
      int64_t min_v = 0, max_v = 0;
      bool has_min = ny_expr_literal_i64(e->as.call.args.data[1].val, &min_v);
      bool has_max = ny_expr_literal_i64(e->as.call.args.data[2].val, &max_v);
      const char *var = tp_expr_ident(e->as.call.args.data[0].val);
      if (!var && ny_expr_literal_i64(e->as.call.args.data[0].val, NULL))
        var = "<literal>";
      tp_emit_range_refinement_json(range_j, first_range, var,
                                    strcmp(name, "range_proven") == 0
                                        ? "range_proven"
                                        : "assert_compile_range",
                                    has_min, min_v, has_max, max_v, "asserted");
    } else if (name &&
               (strcmp(name, "assert_compile_index") == 0 ||
                strcmp(name, "index_proven") == 0) &&
               e->as.call.args.len >= 2) {
      const char *container = tp_expr_ident(e->as.call.args.data[0].val);
      const char *idx_name = tp_expr_ident(e->as.call.args.data[1].val);
      int64_t idx_lit = 0;
      bool has_idx_lit =
          ny_expr_literal_i64(e->as.call.args.data[1].val, &idx_lit);
      int64_t len = tp_static_list_len(e->as.call.args.data[0].val);
      if (len < 0)
        len = tp_flow_len_get(lens, container);
      tp_emit_index_proof_json(
          index_j, first_index, container, idx_name, idx_lit, has_idx_lit, len,
          strcmp(name, "index_proven") == 0 ? "index_proven"
                                            : "assert_compile_index");
    }
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.call.callee, lens,
                      first_nullable, first_range, first_index);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_emit_flow_expr(nullable_j, range_j, index_j,
                        e->as.call.args.data[i].val, lens, first_nullable,
                        first_range, first_index);
  } else if (e->kind == NY_E_MEMCALL) {
    tp_emit_flow_expr(nullable_j, range_j, index_j, e->as.memcall.target, lens,
                      first_nullable, first_range, first_index);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_emit_flow_expr(nullable_j, range_j, index_j,
                        e->as.memcall.args.data[i].val, lens, first_nullable,
                        first_range, first_index);
  }
}

static void tp_emit_flow_stmt(ny_tp_json_t *nullable_j, ny_tp_json_t *range_j,
                              ny_tp_json_t *index_j, ny_tp_json_t *disc_j,
                              ny_tp_ctx_t *ctx, stmt_t *s,
                              ny_tp_flow_len_list *lens, bool *first_nullable,
                              bool *first_range, bool *first_index,
                              bool *first_disc) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_IF:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.iff.test, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.iff.init,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.iff.conseq, lens, first_nullable, first_range,
                      first_index, first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.iff.alt,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    break;
  case NY_S_WHILE:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.whl.test, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.whl.init,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.whl.body,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.whl.update, lens, first_nullable, first_range,
                      first_index, first_disc);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      int64_t len = tp_static_list_len(init);
      if (len >= 0)
        tp_flow_len_set(lens, name, len);
      tp_emit_flow_expr(nullable_j, range_j, index_j, init, lens,
                        first_nullable, first_range, first_index);
    }
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                        s->as.block.body.data[i], lens, first_nullable,
                        first_range, first_index, first_disc);
    break;
  case NY_S_FUNC:
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, s->as.fn.body,
                      lens, first_nullable, first_range, first_index,
                      first_disc);
    break;
  case NY_S_EXPR:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.expr.expr, lens,
                      first_nullable, first_range, first_index);
    break;
  case NY_S_RETURN:
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.ret.value, lens,
                      first_nullable, first_range, first_index);
    break;
  case NY_S_GUARD:
    tp_emit_discriminant_json(
        disc_j, first_disc, tp_expr_ident(s->as.guard.value),
        s->as.guard.type_name, "layout_guard", s->as.guard.name);
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.guard.value, lens,
                      first_nullable, first_range, first_index);
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.guard.fallback, lens, first_nullable, first_range,
                      first_index, first_disc);
    break;
  case NY_S_MATCH: {
    const char *subject = tp_expr_ident(s->as.match.test);
    tp_emit_flow_expr(nullable_j, range_j, index_j, s->as.match.test, lens,
                      first_nullable, first_range, first_index);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t k = 0; k < arm->patterns.len; ++k)
        tp_emit_pattern_discriminant(disc_j, first_disc, subject,
                                     arm->patterns.data[k]);
      tp_emit_flow_expr(nullable_j, range_j, index_j, arm->guard, lens,
                        first_nullable, first_range, first_index);
      tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx, arm->conseq,
                        lens, first_nullable, first_range, first_index,
                        first_disc);
    }
    tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                      s->as.match.default_conseq, lens, first_nullable,
                      first_range, first_index, first_disc);
    break;
  }
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_emit_flow_stmt(nullable_j, range_j, index_j, disc_j, ctx,
                        s->as.module.body.data[i], lens, first_nullable,
                        first_range, first_index, first_disc);
    break;
  default:
    break;
  }
}

char *ny_type_pipeline_refined_json(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  ny_tp_json_t j = {0}, nullable_j = {0}, range_j = {0}, index_j = {0},
               disc_j = {0};
  bool first_nullable = true;
  bool first_range = true;
  bool first_index = true;
  bool first_disc = true;
  ny_tp_flow_len_list lens;
  vec_init(&lens);
  for (size_t i = 0; prog && i < prog->body.len; ++i)
    tp_emit_flow_stmt(&nullable_j, &range_j, &index_j, &disc_j, &ctx,
                      prog->body.data[i], &lens, &first_nullable, &first_range,
                      &first_index, &first_disc);
  tp_append(&j,
            "{\"schema\":\"refined.v1\",\"nullable_refinements\":[%s],"
            "\"range_refinements\":[%s],\"index_proofs\":[%s],"
            "\"discriminant_refinements\":[%s],\"diagnostics\":[]}",
            nullable_j.data ? nullable_j.data : "",
            range_j.data ? range_j.data : "", index_j.data ? index_j.data : "",
            disc_j.data ? disc_j.data : "");
  free(nullable_j.data);
  free(range_j.data);
  free(index_j.data);
  free(disc_j.data);
  vec_free(&lens);
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"schema\":\"refined.v1\",\"nullable_refinements\":[]}");
}

static const char *tp_layout_carrier(size_t size) {
  if (size == 1)
    return "i8";
  if (size == 2)
    return "i16";
  if (size <= 4)
    return "i32";
  if (size <= 8)
    return "i64";
  return "aggregate";
}

static const char *tp_layout_abi_policy(size_t size) {
  if (size == 0)
    return "empty";
  if (size == 1 || size == 2 || size == 4 || size == 8)
    return "direct-scalar";
  return "aggregate-memory";
}

static layout_field_info_t *tp_layout_field(layout_def_t *layout,
                                            const char *field_name) {
  if (!layout || !field_name)
    return NULL;
  for (size_t i = 0; i < layout->fields.len; ++i) {
    layout_field_info_t *f = &layout->fields.data[i];
    if (f->name && strcmp(f->name, field_name) == 0)
      return f;
  }
  return NULL;
}

static void tp_validate_layout_call_expr(ny_tp_ctx_t *ctx, expr_t *e);

static void tp_validate_layout_call_args(ny_tp_ctx_t *ctx, expr_t *e,
                                         const char *name,
                                         ny_call_arg_list *args) {
  if (!ctx || !e || !name || !args)
    return;
  bool is_size = strcmp(name, "__layout_size") == 0;
  bool is_align = strcmp(name, "__layout_align") == 0;
  bool is_offset = strcmp(name, "__layout_offset") == 0;
  bool is_store = strcmp(name, "store_layout") == 0;
  bool is_load = strcmp(name, "load_layout") == 0;
  if (!is_size && !is_align && !is_offset && !is_store && !is_load)
    return;
  size_t layout_arg = is_store || is_load ? 1u : 0u;
  if (args->len <= layout_arg) {
    tp_add_diag(ctx, e->tok, "abi", "layout-arity",
                "%s expects a string literal layout name", name);
    return;
  }
  const char *layout_name = NULL;
  if (!tp_expr_str_lit(args->data[layout_arg].val, &layout_name)) {
    tp_add_diag(ctx, e->tok, "abi", "layout-nonliteral",
                "%s expects a string literal layout name", name);
    return;
  }
  layout_def_t *layout = lookup_layout(ctx->cg, layout_name);
  if (!layout) {
    tp_add_diag(ctx, e->tok, "abi", "layout-unknown", "unknown layout '%s'",
                layout_name);
    return;
  }
  if (is_offset || is_load) {
    size_t field_arg = is_load ? 2u : 1u;
    if (args->len <= field_arg) {
      tp_add_diag(ctx, e->tok, "abi", "layout-field-missing",
                  "%s expects a string literal field name", name);
      return;
    }
    const char *field_name = NULL;
    if (!tp_expr_str_lit(args->data[field_arg].val, &field_name)) {
      tp_add_diag(ctx, e->tok, "abi", "layout-field-nonliteral",
                  "%s expects a string literal field name", name);
      return;
    }
    if (!tp_layout_field(layout, field_name)) {
      tp_add_diag(ctx, e->tok, "abi", "layout-field-unknown",
                  "unknown field '%s' in layout '%s'", field_name, layout_name);
    }
  }
  if (is_store && args->len != layout->fields.len + 2u) {
    tp_add_diag(ctx, e->tok, "abi", "layout-store-arity",
                "store_layout('%s') expects %zu field value(s), got %zu",
                layout_name, layout->fields.len,
                args->len > 2u ? args->len - 2u : 0u);
  }
}

static void tp_validate_layout_call_expr(ny_tp_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return;
  switch (e->kind) {
  case NY_E_CALL: {
    const char *name =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    tp_validate_layout_call_args(ctx, e, name, &e->as.call.args);
    tp_validate_layout_call_expr(ctx, e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      tp_validate_layout_call_expr(ctx, e->as.call.args.data[i].val);
    break;
  }
  case NY_E_MEMCALL:
    tp_validate_layout_call_expr(ctx, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      tp_validate_layout_call_expr(ctx, e->as.memcall.args.data[i].val);
    break;
  case NY_E_BINARY:
    tp_validate_layout_call_expr(ctx, e->as.binary.left);
    tp_validate_layout_call_expr(ctx, e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    tp_validate_layout_call_expr(ctx, e->as.logical.left);
    tp_validate_layout_call_expr(ctx, e->as.logical.right);
    break;
  case NY_E_UNARY:
    tp_validate_layout_call_expr(ctx, e->as.unary.right);
    break;
  case NY_E_TERNARY:
    tp_validate_layout_call_expr(ctx, e->as.ternary.cond);
    tp_validate_layout_call_expr(ctx, e->as.ternary.true_expr);
    tp_validate_layout_call_expr(ctx, e->as.ternary.false_expr);
    break;
  case NY_E_INDEX:
    tp_validate_layout_call_expr(ctx, e->as.index.target);
    tp_validate_layout_call_expr(ctx, e->as.index.start);
    tp_validate_layout_call_expr(ctx, e->as.index.stop);
    tp_validate_layout_call_expr(ctx, e->as.index.step);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      tp_validate_layout_call_expr(ctx, e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      tp_validate_layout_call_expr(ctx, e->as.dict.pairs.data[i].key);
      tp_validate_layout_call_expr(ctx, e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_MEMBER:
    tp_validate_layout_call_expr(ctx, e->as.member.target);
    break;
  default:
    break;
  }
}

static void tp_validate_abi_stmt(ny_tp_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s || !tp_stmt_in_scope(ctx, s))
    return;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      tp_validate_layout_call_expr(ctx, s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    tp_validate_layout_call_expr(ctx, s->as.expr.expr);
    break;
  case NY_S_RETURN:
    tp_validate_layout_call_expr(ctx, s->as.ret.value);
    break;
  case NY_S_IF:
    tp_validate_abi_stmt(ctx, s->as.iff.init);
    tp_validate_layout_call_expr(ctx, s->as.iff.test);
    tp_validate_abi_stmt(ctx, s->as.iff.conseq);
    tp_validate_abi_stmt(ctx, s->as.iff.alt);
    break;
  case NY_S_WHILE:
    tp_validate_abi_stmt(ctx, s->as.whl.init);
    tp_validate_layout_call_expr(ctx, s->as.whl.test);
    tp_validate_abi_stmt(ctx, s->as.whl.body);
    tp_validate_abi_stmt(ctx, s->as.whl.update);
    break;
  case NY_S_FOR:
    tp_validate_abi_stmt(ctx, s->as.fr.init);
    tp_validate_layout_call_expr(ctx, s->as.fr.cond);
    tp_validate_layout_call_expr(ctx, s->as.fr.iterable);
    tp_validate_abi_stmt(ctx, s->as.fr.body);
    tp_validate_abi_stmt(ctx, s->as.fr.update);
    break;
  case NY_S_GUARD:
    tp_validate_layout_call_expr(ctx, s->as.guard.value);
    tp_validate_abi_stmt(ctx, s->as.guard.fallback);
    break;
  case NY_S_MATCH:
    tp_validate_layout_call_expr(ctx, s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t k = 0; k < arm->patterns.len; ++k)
        tp_validate_layout_call_expr(ctx, arm->patterns.data[k]);
      tp_validate_layout_call_expr(ctx, arm->guard);
      tp_validate_abi_stmt(ctx, arm->conseq);
    }
    tp_validate_abi_stmt(ctx, s->as.match.default_conseq);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      tp_validate_abi_stmt(ctx, s->as.block.body.data[i]);
    break;
  case NY_S_FUNC:
    tp_validate_abi_stmt(ctx, s->as.fn.body);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      tp_validate_abi_stmt(ctx, s->as.module.body.data[i]);
    break;
  default:
    break;
  }
}

static void tp_validate_abi_program(ny_tp_ctx_t *ctx) {
  if (!ctx || !ctx->prog)
    return;
  for (size_t i = 0; i < ctx->prog->body.len; ++i)
    tp_validate_abi_stmt(ctx, ctx->prog->body.data[i]);
}

int ny_type_pipeline_validate_abi(program_t *prog, codegen_t *cg,
                                  const char *source_name, bool include_std,
                                  bool emit_diagnostics) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  tp_validate_abi_program(&ctx);
  int count = (int)ctx.diagnostics.len;
  if (emit_diagnostics) {
    for (size_t i = 0; i < ctx.diagnostics.len; ++i) {
      const ny_tp_diag_t *d = &ctx.diagnostics.data[i];
      ny_diag_error(d->tok, "%s",
                    d->message ? d->message : "ABI validation failed");
    }
  }
  tp_ctx_dispose(&ctx);
  return count;
}

char *ny_type_pipeline_lowered_json(program_t *prog, codegen_t *cg,
                                    const char *source_name, bool include_std) {
  ny_tp_ctx_t ctx;
  tp_ctx_build(&ctx, prog, cg, source_name, include_std);
  tp_validate_abi_program(&ctx);
  ny_tp_json_t j = {0};
  tp_append(&j, "{\"schema\":\"lowered.v1\",\"layouts\":[");
  size_t emitted = 0;
  for (size_t i = 0; cg && i < cg->layouts.len; ++i) {
    layout_def_t *layout = cg->layouts.data[i];
    if (!layout || !tp_stmt_in_scope(&ctx, layout->stmt))
      continue;
    if (emitted++)
      tp_append(&j, ",");
    tp_append(&j, "{\"name\":");
    tp_json_str(&j, layout->name ? layout->name : "");
    tp_append(&j, ",\"size\":%zu,\"align\":%zu,\"pack\":%zu,\"carrier\":",
              layout->size, layout->align, layout->pack);
    tp_json_str(&j, tp_layout_carrier(layout->size));
    tp_append(&j, ",\"abi_policy\":");
    tp_json_str(&j, tp_layout_abi_policy(layout->size));
    tp_append(&j, ",\"direct\":%s,\"field_count\":%zu,\"fields\":[",
              strcmp(tp_layout_abi_policy(layout->size), "direct-scalar") == 0
                  ? "true"
                  : "false",
              layout->fields.len);
    for (size_t k = 0; k < layout->fields.len; ++k) {
      layout_field_info_t *f = &layout->fields.data[k];
      if (k)
        tp_append(&j, ",");
      tp_append(&j, "{\"name\":");
      tp_json_str(&j, f->name ? f->name : "");
      tp_append(&j, ",\"type\":");
      tp_json_str(&j, f->type_name ? f->type_name : "");
      tp_append(&j, ",\"offset\":%zu,\"size\":%zu,\"align\":%zu}", f->offset,
                f->size, f->align);
    }
    tp_append(&j, "]}");
  }
  size_t accepted = cg ? cg->mono_specs.len : 0;
  size_t rejected =
      ctx.mono_candidates > accepted ? ctx.mono_candidates - accepted : 0;
  tp_append(&j,
            "],\"monomorphization\":{\"candidates\":%zu,\"accepted\":%zu,"
            "\"rejected\":%zu,\"candidate_decisions\":[",
            ctx.mono_candidates, accepted, rejected);
  bool first_candidate = true;
  for (size_t i = 0; i < ctx.funcs.len; ++i) {
    ny_tp_func_fact_t *fact = &ctx.funcs.data[i];
    if (!fact->call_candidates)
      continue;
    if (!first_candidate)
      tp_append(&j, ",");
    first_candidate = false;
    bool useful_raw = false;
    tp_append(&j, "{\"name\":");
    tp_json_str(&j, fact->name ? fact->name : "");
    tp_append(&j, ",\"call_candidates\":%zu,\"arg_proofs\":[",
              fact->call_candidates);
    for (size_t k = 0; k < fact->params.len; ++k) {
      if (k)
        tp_append(&j, ",");
      const char *t = tp_func_param_inferred(fact, k);
      bool raw = tp_is_int_type(t) || tp_is_float_type(t);
      useful_raw = useful_raw || raw;
      tp_append(&j, "{\"type\":");
      tp_json_str(&j, t ? t : "any");
      tp_append(&j, ",\"raw\":%s}", raw ? "true" : "false");
    }
    tp_append(&j, "],\"decision\":");
    tp_json_str(&j, useful_raw ? "eligible-for-codegen-specialization"
                               : "rejected-no-raw-proof");
    tp_append(&j, "}");
  }
  tp_append(&j, "],\"accepted_specs\":[");
  for (size_t i = 0; cg && i < cg->mono_specs.len; ++i) {
    ny_mono_specialization_t *spec = &cg->mono_specs.data[i];
    if (i)
      tp_append(&j, ",");
    tp_append(&j, "{\"base\":");
    tp_json_str(&j, spec->base_name ? spec->base_name : "");
    tp_append(&j, ",\"specialized\":");
    tp_json_str(&j, spec->specialized_name ? spec->specialized_name : "");
    tp_append(&j, ",\"arity\":%d,\"body_cost\":%zu,\"reason\":", spec->arity,
              spec->body_cost);
    tp_json_str(&j,
                spec->accept_reason ? spec->accept_reason : "arg-specialized");
    tp_append(&j, ",\"return_policy\":");
    tp_json_str(&j,
                spec->return_policy ? spec->return_policy : "tagged-return");
    tp_append(&j, ",\"raw_return_proven\":%s,\"return_kind\":",
              spec->raw_return_proven ? "true" : "false");
    tp_json_str(&j, spec->return_kind == NY_MONO_TYPE_INT
                        ? "int"
                        : (spec->return_kind == NY_MONO_TYPE_F64
                               ? "f64"
                               : (spec->return_kind == NY_MONO_TYPE_LIST
                                      ? "list"
                                      : (spec->return_kind == NY_MONO_TYPE_F64_LIST
                                             ? "list<f64>"
                                             : ""))));
    tp_append(&j, ",\"arg_types\":[");
    for (int k = 0; k < spec->arity && k < NY_MONO_MAX_ARITY; ++k) {
      if (k)
        tp_append(&j, ",");
      tp_json_str(&j, spec->types[k] == NY_MONO_TYPE_INT
                          ? "int"
                          : (spec->types[k] == NY_MONO_TYPE_F64
                                 ? "f64"
                                 : (spec->types[k] == NY_MONO_TYPE_LIST
                                        ? "list"
                                        : (spec->types[k] == NY_MONO_TYPE_F64_LIST
                                               ? "list<f64>"
                                               : "dynamic"))));
    }
    tp_append(&j, "],\"raw_return_active\":%s,\"inline_body_eligible\":%s}",
              spec->raw_return_active ? "true" : "false",
              spec->inline_body_eligible ? "true" : "false");
  }
  tp_append(&j,
            "],\"inline_body_uses\":%zu,\"masked_range_uses\":%zu,"
            "\"rejection_reasons\":{\"not_specialized_or_capped\":%zu}},"
            "\"diagnostics\":",
            cg ? cg->mono_inline_body_uses : 0,
            cg ? cg->mono_masked_range_uses : 0, rejected);
  tp_emit_diagnostics_json(&j, &ctx);
  tp_append(&j, "}");
  tp_ctx_dispose(&ctx);
  return tp_take(&j, "{\"schema\":\"lowered.v1\",\"layouts\":[]}");
}
