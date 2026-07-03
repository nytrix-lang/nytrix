
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

#define NY_TP_USE_ALIAS_CACHE_SLOTS 4096u
#define NY_TP_USE_ALIAS_KEY_MAX 128u

typedef struct ny_tp_use_alias_cache_entry_t {
  const codegen_t *cg;
  const program_t *prog;
  const void *extra_data;
  size_t extra_len;
  uint64_t hash;
  uint16_t len;
  uint8_t state;
  char key[NY_TP_USE_ALIAS_KEY_MAX];
  const char *value;
} ny_tp_use_alias_cache_entry_t;

typedef struct ny_tp_use_alias_cache_t {
  ny_tp_use_alias_cache_entry_t entries[NY_TP_USE_ALIAS_CACHE_SLOTS];
} ny_tp_use_alias_cache_t;

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

static bool tp_reserve(ny_tp_json_t *j, size_t add) {
  if (!j)
    return false;
  if (!j->data) {
    j->cap = 1024;
    while (j->cap <= add)
      j->cap *= 2;
    j->data = malloc(j->cap);
    if (!j->data) {
      j->cap = 0;
      return false;
    }
    j->data[0] = '\0';
    j->len = 0;
  }
  if (add >= SIZE_MAX - j->len)
    return false;
  size_t need = j->len + add + 1;
  if (need <= j->cap)
    return true;
  size_t new_cap = j->cap;
  while (new_cap < need) {
    if (new_cap > SIZE_MAX / 2) {
      new_cap = need;
      break;
    }
    new_cap *= 2;
  }
  char *tmp = realloc(j->data, new_cap);
  if (!tmp)
    return false;
  j->data = tmp;
  j->cap = new_cap;
  return true;
}

static void tp_append_raw(ny_tp_json_t *j, const char *s, size_t n) {
  if (!s || n == 0 || !tp_reserve(j, n))
    return;
  memcpy(j->data + j->len, s, n);
  j->len += n;
  j->data[j->len] = '\0';
}

static void tp_append_char(ny_tp_json_t *j, char c) {
  if (!tp_reserve(j, 1))
    return;
  j->data[j->len++] = c;
  j->data[j->len] = '\0';
}

static void tp_append(ny_tp_json_t *j, const char *fmt, ...) {
  if (!j || !fmt)
    return;
  if (!tp_reserve(j, 0))
    return;
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
    if (!tp_reserve(j, (size_t)n))
      return;
  }
}

static bool tp_json_needs_escape(unsigned char c) {
  return c < 32 || c == '"' || c == '\\';
}

static void tp_json_escape_char(ny_tp_json_t *j, unsigned char c) {
  switch (c) {
  case '"':
    tp_append_raw(j, "\\\"", 2);
    break;
  case '\\':
    tp_append_raw(j, "\\\\", 2);
    break;
  case '\n':
    tp_append_raw(j, "\\n", 2);
    break;
  case '\r':
    tp_append_raw(j, "\\r", 2);
    break;
  case '\t':
    tp_append_raw(j, "\\t", 2);
    break;
  default:
    tp_append(j, "\\u%04x", (unsigned)c);
    break;
  }
}

static void tp_json_str(ny_tp_json_t *j, const char *s) {
  tp_append_char(j, '"');
  if (s) {
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *run = p;
    for (; *p; ++p) {
      if (tp_json_needs_escape(*p)) {
        tp_append_raw(j, (const char *)run, (size_t)(p - run));
        tp_json_escape_char(j, *p);
        run = p + 1;
      }
    }
    tp_append_raw(j, (const char *)run, (size_t)(p - run));
  }
  tp_append_char(j, '"');
}

static void tp_json_strn(ny_tp_json_t *j, const char *s, size_t n) {
  tp_append_char(j, '"');
  if (s) {
    size_t run = 0;
    size_t i = 0;
    for (; i < n && s[i]; ++i) {
      unsigned char c = (unsigned char)s[i];
      if (tp_json_needs_escape(c)) {
        tp_append_raw(j, s + run, i - run);
        tp_json_escape_char(j, c);
        run = i + 1;
      }
    }
    tp_append_raw(j, s + run, i - run);
  }
  tp_append_char(j, '"');
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
  static const char *names[] = {
      [NY_E_IDENT] = "ident",
      [NY_E_LITERAL] = "literal",
      [NY_E_UNARY] = "unary",
      [NY_E_BINARY] = "binary",
      [NY_E_LOGICAL] = "logical",
      [NY_E_TERNARY] = "ternary",
      [NY_E_CALL] = "call",
      [NY_E_MEMCALL] = "memcall",
      [NY_E_INDEX] = "index",
      [NY_E_LAMBDA] = "lambda",
      [NY_E_FN] = "fn",
      [NY_E_LIST] = "list",
      [NY_E_TUPLE] = "tuple",
      [NY_E_DICT] = "dict",
      [NY_E_SET] = "set",
      [NY_E_ASM] = "asm",
      [NY_E_COMPTIME] = "comptime",
      [NY_E_FSTRING] = "fstring",
      [NY_E_INFERRED_MEMBER] = "inferred_member",
      [NY_E_EMBED] = "embed",
      [NY_E_MATCH] = "match",
      [NY_E_MEMBER] = "member",
      [NY_E_PTR_TYPE] = "ptr_type",
      [NY_E_DEREF] = "deref",
      [NY_E_SIZEOF] = "sizeof",
      [NY_E_TRY] = "try_expr",
  };
  return kind >= 0 && (size_t)kind < sizeof(names) / sizeof(names[0]) &&
                 names[kind]
             ? names[kind]
             : "expr";
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

static bool tp_fallback_warning_enabled(const ny_tp_ctx_t *ctx) {
  if (!ctx || !ctx->fallbacks.len)
    return false;
  if (ctx->cg && ctx->cg->strict_types)
    return false;
  return ny_env_enabled_default_on("NYTRIX_TYPE_FALLBACK_WARN");
}

static bool tp_fallback_warning_interesting(const ny_tp_fallback_t *fb) {
  if (!fb || !fb->code || fb->tok.line <= 0)
    return false;
  if (strcmp(fb->code, "hm-unknown-member") == 0)
    return true;
  if (strcmp(fb->code, "hm-dynamic-member") == 0)
    return true;
  if (strcmp(fb->code, "hm-dynamic-call") == 0 ||
      strcmp(fb->code, "hm-unknown-call") == 0 ||
      strcmp(fb->code, "hm-call-missing-callee") == 0)
    return true;
  if (strcmp(fb->code, "hm-result-payload-dynamic") == 0)
    return true;
  bool verbose = ny_diag_warn_level() >= 2 ||
                 ny_env_enabled("NYTRIX_TYPE_FALLBACK_WARN_VERBOSE");
  if (!verbose)
    return false;
  if (strcmp(fb->code, "hm-dict-heterogeneous-literal") == 0)
    return true;
  if (strcmp(fb->code, "hm-dynamic-arithmetic") == 0 ||
      strcmp(fb->code, "hm-dynamic-comparison") == 0)
    return true;
  if (strcmp(fb->code, "hm-index-dynamic-result") == 0)
    return true;
  if (strcmp(fb->code, "hm-callable-return-any") == 0)
    return true;
  return false;
}

static const char *tp_fallback_hint(const ny_tp_fallback_t *fb) {
  const char *code = fb ? fb->code : NULL;
  if (!code)
    return "the program still compiles; the checker lost static evidence here";
  if (strcmp(code, "hm-dict-heterogeneous-literal") == 0)
    return "the dict literal mixes key or value shapes and was widened to any";
  if (strcmp(code, "hm-dynamic-arithmetic") == 0 ||
      strcmp(code, "hm-dynamic-comparison") == 0)
    return "an operand is any, so the checker cannot prove the operator result "
           "or select an unboxed fast path";
  if (strcmp(code, "hm-dynamic-member") == 0 ||
      strcmp(code, "hm-unknown-member") == 0)
    return "the receiver type is unknown or the member is not present on the "
           "inferred receiver";
  if (strcmp(code, "hm-index-dynamic-result") == 0)
    return "the indexed value has no proven element type, so reads stay on the "
           "boxed dynamic path";
  if (strcmp(code, "hm-dynamic-call") == 0 ||
      strcmp(code, "hm-unknown-call") == 0 ||
      strcmp(code, "hm-call-missing-callee") == 0 ||
      strcmp(code, "hm-callable-return-any") == 0)
    return "the callable or its return payload is not statically known, so the "
           "call cannot be specialized";
  if (strcmp(code, "hm-result-payload-dynamic") == 0)
    return "the Result payload type is not known before unwrap or pattern bind";
  return "the program still compiles; the checker lost static evidence here";
}

static const char *tp_fallback_fix(const ny_tp_fallback_t *fb) {
  const char *code = fb ? fb->code : NULL;
  if (!code)
    return "add a local annotation/converter or use --strict-types to turn this "
           "class into an error";
  if (strcmp(code, "hm-dict-heterogeneous-literal") == 0)
    return "split the record shape, annotate the dict value type, or make the "
           "dynamic intent explicit near the literal";
  if (strcmp(code, "hm-dynamic-arithmetic") == 0 ||
      strcmp(code, "hm-dynamic-comparison") == 0)
    return "convert or annotate the operand before the operator, for example "
           "int(x), float(x), str(x), or a typed local";
  if (strcmp(code, "hm-dynamic-member") == 0 ||
      strcmp(code, "hm-unknown-member") == 0)
    return "prove the receiver shape with a type annotation, layout guard, or "
           "correct import/member spelling";
  if (strcmp(code, "hm-index-dynamic-result") == 0)
    return "prove the container element type before indexing or guard the "
           "dynamic lookup result";
  if (strcmp(code, "hm-dynamic-call") == 0 ||
      strcmp(code, "hm-unknown-call") == 0 ||
      strcmp(code, "hm-call-missing-callee") == 0 ||
      strcmp(code, "hm-callable-return-any") == 0)
    return "import or annotate the callable, or give the returned fnptr/result a "
           "typed boundary";
  if (strcmp(code, "hm-result-payload-dynamic") == 0)
    return "construct or annotate the value as Result<T,E> before unwrap or "
           "pattern binding";
  return "add a local annotation/converter or use --strict-types to turn this "
         "class into an error";
}

static size_t tp_emit_fallback_warnings(const ny_tp_ctx_t *ctx) {
  if (!tp_fallback_warning_enabled(ctx))
    return 0;
  int limit = ny_env_int_range("NYTRIX_TYPE_FALLBACK_WARN_LIMIT", 5, 0, 100);
  if (limit <= 0)
    return 0;
  size_t emitted = 0;
  size_t skipped = 0;
  token_t skipped_tok = {0};
  for (size_t i = 0; i < ctx->fallbacks.len; ++i) {
    const ny_tp_fallback_t *fb = &ctx->fallbacks.data[i];
    if (!tp_fallback_warning_interesting(fb))
      continue;
    if (emitted >= (size_t)limit) {
      skipped++;
      if (skipped_tok.line <= 0)
        skipped_tok = fb->tok;
      continue;
    }
    ny_diag_warning_code(
        fb->tok, 2101, "dynamic type fallback: %s",
        fb->reason && *fb->reason ? fb->reason
                                  : (fb->code ? fb->code : "inferred any"));
    ny_diag_hint("%s", tp_fallback_hint(fb));
    ny_diag_fix("%s", tp_fallback_fix(fb));
    emitted++;
  }
  if (skipped > 0) {
    ny_diag_warning_code(
        skipped_tok.line > 0 ? skipped_tok : (token_t){0}, 2102,
        "%zu more dynamic type fallback warning(s) suppressed; set "
        "NYTRIX_TYPE_FALLBACK_WARN_LIMIT or use --strict-types for the full "
        "gate",
        skipped);
  }
  return emitted;
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

static bool tp_str_in(const char *s, const char *const *vals, size_t n) {
  if (!s)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (strcmp(s, vals[i]) == 0)
      return true;
  return false;
}

static bool tp_is_int_type(const char *t) {
  t = tp_skip_nullable(t);
  static const char *const ints[] = {"int", "i8",   "i16",  "i32",
                                     "i64", "i128", "u8",   "u16",
                                     "u32", "u64",  "u128"};
  return tp_str_in(t, ints, sizeof(ints) / sizeof(ints[0]));
}

static bool tp_is_float_type(const char *t) {
  t = tp_skip_nullable(t);
  static const char *const floats[] = {"f32", "f64", "f128"};
  return tp_str_in(t, floats, sizeof(floats) / sizeof(floats[0]));
}

static bool tp_is_complex_type(const char *t) {
  t = tp_skip_nullable(t);
  static const char *const complex[] = {"complex", "c64", "c128"};
  return tp_str_in(t, complex, sizeof(complex) / sizeof(complex[0]));
}

static bool tp_is_core_scalar(const char *t) {
  t = tp_skip_nullable(t);
  static const char *const scalars[] = {"any",  "nil", "bool", "str",
                                        "char", "ptr", "handle"};
  return !t || !*t ||
         tp_str_in(t, scalars, sizeof(scalars) / sizeof(scalars[0])) ||
         tp_is_int_type(t) || tp_is_float_type(t) || tp_is_complex_type(t);
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
  static const char *const values[] = {
      "list",       "tuple",    "dict",       "set",       "bytes",
      "range",      "fnptr",    "number",     "numeric",   "integer",
      "float",      "scalar",   "seq",        "sequence",  "collection",
      "container",  "iterable", "indexable",  "allocator", "bigint",
  };
  return tp_is_core_scalar(base) ||
         tp_str_in(base, values, sizeof(values) / sizeof(values[0]));
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

static bool tp_module_alias_member_exists(codegen_t *cg, const char *target_name,
                                          const char *member_name) {
  if (!cg || !target_name || !*target_name || !member_name || !*member_name)
    return false;
  const char *module_name =
      ny_lookup_module_alias(cg, NULL, 0, target_name, strlen(target_name), 0);
  if (!module_name || !*module_name)
    return false;
  char dotted[512];
  int nw = snprintf(dotted, sizeof(dotted), "%s.%s", module_name, member_name);
  if (nw <= 0 || (size_t)nw >= sizeof(dotted))
    return false;
  if (lookup_fun(cg, dotted, 0) || lookup_global(cg, dotted))
    return true;
  const char *resolved = resolve_import_alias(cg, dotted);
  return resolved && *resolved && strcmp(resolved, dotted) != 0 &&
         (lookup_fun(cg, resolved, 0) || lookup_global(cg, resolved));
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

static bool tp_use_alias_cacheable_name(const char *alias, size_t *len_out) {
  if (!alias || !*alias)
    return false;
  size_t len = strlen(alias);
  if (len == 0 || len >= NY_TP_USE_ALIAS_KEY_MAX || len > UINT16_MAX)
    return false;
  if (len_out)
    *len_out = len;
  return true;
}

static ny_tp_use_alias_cache_t *tp_use_alias_cache(codegen_t *cg) {
  if (!cg)
    return NULL;
  if (!cg->use_alias_lookup_cache)
    cg->use_alias_lookup_cache = calloc(1, sizeof(ny_tp_use_alias_cache_t));
  return (ny_tp_use_alias_cache_t *)cg->use_alias_lookup_cache;
}

static int tp_use_alias_cache_get(codegen_t *cg, const char *alias,
                                  size_t alias_len, uint64_t hash,
                                  const char **out) {
  if (!cg || !alias || alias_len == 0 || alias_len >= NY_TP_USE_ALIAS_KEY_MAX)
    return -1;
  ny_tp_use_alias_cache_t *cache = tp_use_alias_cache(cg);
  if (!cache)
    return -1;
  ny_tp_use_alias_cache_entry_t *e =
      &cache->entries[hash & (NY_TP_USE_ALIAS_CACHE_SLOTS - 1u)];
  if (!e->state || e->cg != cg || e->prog != cg->prog ||
      e->extra_data != cg->extra_progs.data ||
      e->extra_len != cg->extra_progs.len || e->hash != hash ||
      e->len != (uint16_t)alias_len ||
      memcmp(e->key, alias, alias_len) != 0 || e->key[alias_len] != '\0')
    return -1;
  if (e->state == 2u) {
    *out = e->value;
    return 1;
  }
  return 0;
}

static void tp_use_alias_cache_put(codegen_t *cg, const char *alias,
                                   size_t alias_len, uint64_t hash,
                                   const char *value) {
  if (!cg || !alias || alias_len == 0 || alias_len >= NY_TP_USE_ALIAS_KEY_MAX)
    return;
  ny_tp_use_alias_cache_t *cache = tp_use_alias_cache(cg);
  if (!cache)
    return;
  ny_tp_use_alias_cache_entry_t *e =
      &cache->entries[hash & (NY_TP_USE_ALIAS_CACHE_SLOTS - 1u)];
  e->cg = cg;
  e->prog = cg->prog;
  e->extra_data = cg->extra_progs.data;
  e->extra_len = cg->extra_progs.len;
  e->hash = hash;
  e->len = (uint16_t)alias_len;
  memcpy(e->key, alias, alias_len);
  e->key[alias_len] = '\0';
  e->value = value;
  e->state = value ? 2u : 1u;
}

static const char *tp_lookup_program_use_alias(codegen_t *cg,
                                               const char *alias) {
  if (!cg || !alias || !*alias)
    return NULL;
  size_t alias_len = 0;
  bool cacheable = tp_use_alias_cacheable_name(alias, &alias_len);
  uint64_t hash = cacheable ? ny_hash_name(alias, alias_len) : 0;
  if (cacheable) {
    const char *cached = NULL;
    int hit = tp_use_alias_cache_get(cg, alias, alias_len, hash, &cached);
    if (hit == 1)
      return cached;
    if (hit == 0)
      return NULL;
  }
  if (cg->prog) {
    for (size_t i = 0; i < cg->prog->body.len; ++i) {
      const char *found =
          tp_lookup_use_alias_stmt(cg->prog->body.data[i], alias);
      if (found && *found) {
        if (cacheable)
          tp_use_alias_cache_put(cg, alias, alias_len, hash, found);
        return found;
      }
    }
  }
  for (size_t p = 0; p < cg->extra_progs.len; ++p) {
    program_t *prog = cg->extra_progs.data[p];
    if (!prog)
      continue;
    for (size_t i = 0; i < prog->body.len; ++i) {
      const char *found = tp_lookup_use_alias_stmt(prog->body.data[i], alias);
      if (found && *found) {
        if (cacheable)
          tp_use_alias_cache_put(cg, alias, alias_len, hash, found);
        return found;
      }
    }
  }
  if (cacheable)
    tp_use_alias_cache_put(cg, alias, alias_len, hash, NULL);
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
  if (strcmp(name, "addr_of") == 0)
    return ny_strdup("ptr");
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
    return ny_strdup("ptr");
  case NY_E_DEREF: {
    if (e->as.deref.target) {
      char *t = tp_expr_type(ctx, env, e->as.deref.target, depth + 1);
      if (t && t[0] == '*')
        return ny_strdup(t + 1);
      free(t);
    }
    return ny_strdup("int");
  }
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
