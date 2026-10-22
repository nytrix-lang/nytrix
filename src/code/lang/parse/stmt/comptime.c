/*
 * Comptime statement parser: compile-time evaluation blocks, templates,
 * table generators, compile-time reflection, and comptime control flow.
 */
static bool tok_is_ident_text(token_t tok, const char *text) {
  if (tok.kind != NY_T_IDENT || !text)
    return false;
  size_t want = strlen(text);
  return tok.len == want && memcmp(tok.lexeme, text, want) == 0;
}

static bool tok_is_hash_kw(token_t tok, const char *text,
                           token_kind keyword_kind) {
  if (keyword_kind != NY_T_IDENT && tok.kind == keyword_kind)
    return true;
  return tok_is_ident_text(tok, text);
}

typedef struct parser_form_name_t {
  const char *name;
  token_kind kind;
} parser_form_name_t;

static bool tok_matches_form_name(token_t tok, const parser_form_name_t *form) {
  if (!form || !form->name)
    return false;
  if (form->kind != NY_T_IDENT && tok.kind == form->kind)
    return true;
  return tok_is_ident_text(tok, form->name);
}

static const char *tok_form_name(parser_t *p, token_t tok) {
  if (tok.lexeme && tok.len > 0)
    return arena_strndup(p->arena, tok.lexeme, tok.len);
  return parser_token_name(tok.kind);
}

static bool tok_is_unsupported_comptime_stmt_form(token_t tok) {
  static const parser_form_name_t forms[] = {
      {"module", NY_T_MODULE},
      {"use", NY_T_USE},
      {"struct", NY_T_STRUCT},
      {"layout", NY_T_STRUCT},
      {"enum", NY_T_ENUM},
      {"extern", NY_T_EXTERN},
      {"fn", NY_T_FN},
      {"def", NY_T_DEF},
      {"mut", NY_T_MUT},
      {"if", NY_T_IF},
      {"while", NY_T_WHILE},
      {"for", NY_T_FOR},
      {"try", NY_T_TRY},
      {"defer", NY_T_DEFER},
      {"return", NY_T_RETURN},
      {"break", NY_T_BREAK},
      {"continue", NY_T_CONTINUE},
      {"macro", NY_T_IDENT},
      {"operator", NY_T_IDENT},
      {"impl", NY_T_IDENT},
      {"with", NY_T_IDENT},
      {"import", NY_T_IDENT},
      {"func", NY_T_IDENT},
      {"function", NY_T_IDENT},
      {"let", NY_T_IDENT},
      {"var", NY_T_IDENT},
  };
  for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); ++i) {
    if (tok_matches_form_name(tok, &forms[i]))
      return true;
  }
  return false;
}

static bool parse_unsupported_comptime_stmt_form(parser_t *p) {
  token_t form = parser_peek(p);
  if (!tok_is_unsupported_comptime_stmt_form(form))
    return false;
  const char *name = tok_form_name(p, form);
  char msg[160];
  snprintf(msg, sizeof(msg), "unsupported comptime form '%s'",
           name ? name : "unknown");
  parser_error(p, form, msg,
               "supported comptime forms: diagnostic, template, emit, table, "
               "fields, exports, variants; "
               "use comptime { ... } for expressions");
  int brace_depth = 0;
  while (p->cur.kind != NY_T_EOF) {
    if (p->cur.kind == NY_T_LBRACE) {
      brace_depth++;
    } else if (p->cur.kind == NY_T_RBRACE) {
      if (brace_depth == 0)
        break;
      brace_depth--;
      parser_advance(p);
      if (brace_depth == 0)
        break;
      continue;
    } else if (p->cur.kind == NY_T_SEMI && brace_depth == 0) {
      parser_advance(p);
      break;
    }
    parser_advance(p);
  }
  return true;
}

static bool tok_is_platform_guard(token_t tok) {
  if (tok.kind != NY_T_IDENT)
    return false;
  static const char *guards[] = {
      "linux",   "unix",      "posix",      "macos",    "mac",
      "windows", "x86",       "x86_64",     "x64",      "aarch64",
      "arm64",   "arm",       "riscv",      "LINUX",    "UNIX",
      "MACOS",   "IS_LINUX",  "IS_UNIX",    "IS_MACOS", "IS_WINDOWS",
      "IS_X86",  "IS_X86_64", "IS_AARCH64", "IS_ARM",   "IS_RISCV",
  };
  for (size_t i = 0; i < sizeof(guards) / sizeof(guards[0]); i++) {
    size_t len = strlen(guards[i]);
    if (tok.len == len && memcmp(tok.lexeme, guards[i], len) == 0)
      return true;
  }
  return false;
}

static expr_t *ct_make_ident_expr(parser_t *p, token_t tok, const char *name) {
  expr_t *id = expr_new(p->arena, NY_E_IDENT, tok);
  id->as.ident.name = parser_intern(p, name, strlen(name));
  return id;
}

static expr_t *make_comptime_return_expr(parser_t *p, token_t tok,
                                         expr_t *value) {
  if (!value)
    return NULL;
  expr_t *ct = expr_new(p->arena, NY_E_COMPTIME, tok);
  stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, tok);
  ret->as.ret.value = value;
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &blk->as.block.body, ret);
  ct->as.comptime_expr.body = blk;
  ct->as.comptime_expr.origin = NY_COMPTIME_ORIGIN_SYNTHETIC;
  return ct;
}

static expr_t *make_platform_guard_comptime(parser_t *p, token_t tok) {
  expr_t *id = expr_new(p->arena, NY_E_IDENT, tok);
  id->as.ident.name = parser_intern_hash(p, tok.lexeme, tok.len, tok.hash);
  id->as.ident.sym_id = tok.sym_id;
  id->as.ident.hash = tok.hash;
  return make_comptime_return_expr(p, tok, id);
}

static expr_t *make_main_guard_comptime(parser_t *p, token_t tok) {
  return make_comptime_return_expr(p, tok,
                                   make_zero_arg_call_expr(p, tok, "__main"));
}

static expr_t *parse_hash_if_cond_as_comptime(parser_t *p, token_t tok) {
  bool has_paren = parser_match(p, NY_T_LPAREN);
  expr_t *cond = p_parse_expr(p, 0);
  if (has_paren)
    parser_expect(p, NY_T_RPAREN, "')' after #if condition", NULL);
  if (!cond)
    return NULL;
  return make_comptime_return_expr(p, tok, cond);
}

static void parse_optional_hash_endif(parser_t *p) {
  if (p->cur.kind != NY_T_HASH)
    return;
  parser_t saved = *p;
  parser_advance(p);
  if (tok_is_hash_kw(p->cur, "endif", NY_T_IDENT)) {
    parser_advance(p);
    return;
  }
  *p = saved;
}

static stmt_t *parse_hash_if_stmt(parser_t *p, token_t hash_tok) {
  token_t kw_tok = p->cur;
  if (!tok_is_hash_kw(kw_tok, "if", NY_T_IF) &&
      !tok_is_hash_kw(kw_tok, "elif", NY_T_ELIF)) {
    parser_error(p, kw_tok, "expected 'if' or 'elif' after '#'",
                 "write '#if condition' or '#elif condition'");
    return NULL;
  }
  parser_advance(p);

  expr_t *cond = parse_hash_if_cond_as_comptime(p, kw_tok);
  if (!cond)
    return NULL;

  stmt_t *conseq = ny_parse_stmt_or_block(p);
  if (!conseq)
    return NULL;

  stmt_t *alt = NULL;
  if (p->cur.kind == NY_T_ELSE) {
    parser_advance(p);
    alt = ny_parse_stmt_or_block(p);
    parse_optional_hash_endif(p);
  } else if (p->cur.kind == NY_T_ELIF) {
    alt = ny_parse_if_stmt(p);
  } else if (p->cur.kind == NY_T_HASH) {
    parser_t saved = *p;
    parser_advance(p);
    if (tok_is_hash_kw(p->cur, "else", NY_T_ELSE)) {
      parser_advance(p);
      alt = ny_parse_stmt_or_block(p);
      parse_optional_hash_endif(p);
    } else if (tok_is_hash_kw(p->cur, "elif", NY_T_ELIF)) {
      alt = parse_hash_if_stmt(p, hash_tok);
    } else if (tok_is_hash_kw(p->cur, "endif", NY_T_IDENT)) {
      parser_advance(p);
    } else {
      *p = saved;
    }
  }

  stmt_t *s = stmt_new(p->arena, NY_S_IF, hash_tok);
  s->as.iff.test = cond;
  s->as.iff.conseq = conseq;
  s->as.iff.alt = alt;
  s->as.iff.init = NULL;
  return s;
}

static stmt_t *parse_hash_main_guard_stmt(parser_t *p, token_t hash_tok) {
  token_t main_tok = p->cur;
  if (!tok_is_hash_kw(main_tok, "main", NY_T_IDENT)) {
    parser_error(p, main_tok, "expected 'main' after '#'",
                 "write '#main { ... }' for top-level script guard");
    return NULL;
  }
  parser_advance(p);

  expr_t *cond = make_main_guard_comptime(p, main_tok);
  if (!cond)
    return NULL;

  stmt_t *conseq = ny_parse_stmt_or_block(p);
  if (!conseq)
    return NULL;

  stmt_t *s = stmt_new(p->arena, NY_S_IF, hash_tok);
  s->as.iff.test = cond;
  s->as.iff.conseq = conseq;
  s->as.iff.alt = NULL;
  s->as.iff.init = NULL;
  return s;
}

static stmt_t *parse_hash_platform_guard_stmt(parser_t *p, token_t hash_tok) {
  token_t guard_tok = p->cur;
  if (!tok_is_platform_guard(guard_tok)) {
    parser_error(p, guard_tok, "expected platform guard after '#'",
                 "supported guards are #linux, #macos, #windows, #wasm, "
                 "#x86_64, #aarch64");
    return NULL;
  }
  parser_advance(p);

  expr_t *cond = make_platform_guard_comptime(p, guard_tok);
  if (!cond)
    return NULL;

  stmt_t *conseq = ny_parse_stmt_or_block(p);
  if (!conseq)
    return NULL;

  stmt_t *alt = NULL;
  if (p->cur.kind == NY_T_ELSE) {
    parser_advance(p);
    alt = ny_parse_stmt_or_block(p);
    parse_optional_hash_endif(p);
  } else if (p->cur.kind == NY_T_ELIF) {
    alt = ny_parse_if_stmt(p);
  } else if (p->cur.kind == NY_T_HASH) {
    parser_t saved = *p;
    parser_advance(p);
    if (tok_is_hash_kw(p->cur, "else", NY_T_ELSE)) {
      parser_advance(p);
      alt = ny_parse_stmt_or_block(p);
      parse_optional_hash_endif(p);
    } else if (tok_is_hash_kw(p->cur, "elif", NY_T_ELIF)) {
      alt = parse_hash_if_stmt(p, hash_tok);
    } else if (tok_is_hash_kw(p->cur, "endif", NY_T_IDENT)) {
      parser_advance(p);
    } else {
      *p = saved;
    }
  }

  stmt_t *s = stmt_new(p->arena, NY_S_IF, hash_tok);
  s->as.iff.test = cond;
  s->as.iff.conseq = conseq;
  s->as.iff.alt = alt;
  s->as.iff.init = NULL;
  return s;
}

static expr_t *parse_table_ident_expr(parser_t *p, token_t tok,
                                      const char *name) {
  size_t len = strlen(name);
  token_t ident_tok = tok;
  ident_tok.lexeme = name;
  ident_tok.len = len;
  expr_t *e = expr_new(p->arena, NY_E_IDENT, ident_tok);
  e->as.ident.name = parser_intern(p, name, len);
  return e;
}

static stmt_t *parse_table_return_block(parser_t *p, token_t tok,
                                        expr_t *value) {
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  if (value) {
    stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, tok);
    ret->as.ret.value = value;
    vec_push_arena(p->arena, &blk->as.block.body, ret);
  }
  return blk;
}

static const char *parse_table_matcher_name(parser_t *p,
                                            const char *decl_name) {
  static const char prefix[] = "_ct_table_";
  const char *tail = strrchr(decl_name, '.');
  const char *leaf = tail ? tail + 1 : decl_name;
  size_t leaf_len = strlen(leaf);
  size_t prefix_len = sizeof(prefix) - 1;
  size_t owner_len = 0;
  const char *owner = NULL;
  if (tail) {
    owner = decl_name;
    owner_len = (size_t)(tail - decl_name + 1);
  } else if (p->current_module && *p->current_module) {
    owner = p->current_module;
    owner_len = strlen(p->current_module) + 1;
  }
  size_t total = owner_len + prefix_len + leaf_len;
  char *buf = arena_alloc(p->arena, total + 1);
  size_t at = 0;
  if (owner_len) {
    if (tail) {
      memcpy(buf, owner, owner_len);
      at = owner_len;
    } else {
      size_t mod_len = owner_len - 1;
      memcpy(buf, owner, mod_len);
      buf[mod_len] = '.';
      at = owner_len;
    }
  }
  memcpy(buf + at, prefix, prefix_len);
  at += prefix_len;
  memcpy(buf + at, leaf, leaf_len);
  at += leaf_len;
  buf[at] = '\0';
  return parser_intern(p, buf, at);
}

static bool parse_table_is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool parse_table_is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool parse_table_is_digit(char c) { return c >= '0' && c <= '9'; }
static char parse_table_to_lower(char c) {
  return parse_table_is_upper(c) ? (char)(c - 'A' + 'a') : c;
}

static const char *parse_table_legacy_helper_name(parser_t *p,
                                                  const char *decl_name) {
  const char *tail = strrchr(decl_name, '.');
  const char *leaf = tail ? tail + 1 : decl_name;
  size_t leaf_len = strlen(leaf);
  size_t owner_len = 0;
  const char *owner = NULL;
  if (tail) {
    owner = decl_name;
    owner_len = (size_t)(tail - decl_name + 1);
  } else if (p->current_module && *p->current_module) {
    owner = p->current_module;
    owner_len = strlen(p->current_module) + 1;
  }

  size_t snake_cap = leaf_len * 2 + 2;
  char *snake = arena_alloc(p->arena, snake_cap);
  size_t sn = 0;
  snake[sn++] = '_';
  for (size_t i = 0; i < leaf_len; i++) {
    char c = leaf[i];
    bool upper = parse_table_is_upper(c);
    if (upper && i > 0) {
      char prev = leaf[i - 1];
      char next = (i + 1 < leaf_len) ? leaf[i + 1] : '\0';
      bool prev_word = parse_table_is_lower(prev) || parse_table_is_digit(prev);
      bool acronym_end =
          parse_table_is_upper(prev) && parse_table_is_lower(next);
      if (sn > 1 && snake[sn - 1] != '_' && (prev_word || acronym_end)) {
        snake[sn++] = '_';
      }
    }
    snake[sn++] = parse_table_to_lower(c);
  }
  snake[sn] = '\0';

  size_t total = owner_len + sn;
  char *buf = arena_alloc(p->arena, total + 1);
  size_t at = 0;
  if (owner_len) {
    if (tail) {
      memcpy(buf, owner, owner_len);
      at = owner_len;
    } else {
      size_t mod_len = owner_len - 1;
      memcpy(buf, owner, mod_len);
      buf[mod_len] = '.';
      at = owner_len;
    }
  }
  memcpy(buf + at, snake, sn);
  at += sn;
  buf[at] = '\0';
  return parser_intern(p, buf, at);
}

static expr_t *parse_table_call_expr(parser_t *p, token_t tok,
                                     const char *callee_name,
                                     const char *first_arg,
                                     const char *second_arg) {
  expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
  call->as.call.callee = parse_table_ident_expr(p, tok, callee_name);
  vec_push_arena(
      p->arena, &call->as.call.args,
      ((call_arg_t){.name = NULL,
                    .val = parse_table_ident_expr(p, tok, first_arg)}));
  vec_push_arena(
      p->arena, &call->as.call.args,
      ((call_arg_t){.name = NULL,
                    .val = parse_table_ident_expr(p, tok, second_arg)}));
  return call;
}

static expr_t *ct_string_expr(parser_t *p, token_t tok, const char *value);
static expr_t *ct_int_expr(parser_t *p, token_t tok, int64_t value);
static expr_t *ct_float_expr(parser_t *p, token_t tok, double value);
static expr_t *ct_bool_expr(parser_t *p, token_t tok, bool value);

static stmt_t *parse_comptime_table_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_COMPTIME)
    parser_advance(p);
  if (!tok_is_ident_text(p->cur, "table")) {
    parser_error(p, p->cur, "expected 'table' after 'comptime'",
                 "write comptime table Name { pattern -> value }");
    return NULL;
  }
  parser_advance(p);

  char *decl_owned =
      parse_dotted_ident_owned(p, "expected table name after 'comptime table'",
                               "expected identifier after '.' in table name");
  if (!decl_owned)
    return NULL;
  const char *matcher_name = parse_table_matcher_name(p, decl_owned);
  const char *legacy_name = parse_table_legacy_helper_name(p, decl_owned);
  free(decl_owned);

  /*
   * The generated matcher's fallback parameter must match the table's VALUE
   * domain: string tables take a str default, numeric tables an i32 one.
   * Hardcoding i32 type-checked `return default` (i32) against the str
   * entries and rejected the user's str default at the call site.
   *
   * Infer the lookup-key domain from the first literal pattern.  String-key
   * tables otherwise inherit the legacy integer key and reject valid calls
   * at the first use.
   */
  const char *fallback_type = "i32";
  bool fallback_type_locked = false;
  const char *key_type = "i32";
  bool key_type_locked = false;

  parser_expect(p, NY_T_LBRACE, "'{' after comptime table name", NULL);

  stmt_t *match = stmt_new(p->arena, NY_S_MATCH, tok);
  match->as.match.test = parse_table_ident_expr(p, tok, "raw");

  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    match_arm_t arm;
    memset(&arm, 0, sizeof(arm));
    expr_t *first = p_parse_expr(p, 0);
    if (!first)
      break;
    vec_push_arena(p->arena, &arm.patterns, first);
    while (parser_match(p, NY_T_COMMA)) {
      if (p->cur.kind == NY_T_ARROW)
        break;
      expr_t *pat = p_parse_expr(p, 0);
      if (pat)
        vec_push_arena(p->arena, &arm.patterns, pat);
    }
    if (!key_type_locked) {
      for (size_t i = 0; i < arm.patterns.len; ++i) {
        expr_t *pattern = arm.patterns.data[i];
        if (pattern && pattern->kind == NY_E_LITERAL) {
          if (pattern->as.literal.kind == NY_LIT_STR)
            key_type = "str";
          else
            key_type = "i32";
          key_type_locked = true;
          break;
        }
      }
    }
    if (parser_match(p, NY_T_IF)) {
      arm.guard = p_parse_expr(p, 0);
    }
    parser_expect(p, NY_T_ARROW, "'->' in comptime table entry", NULL);
    token_t value_tok = p->cur;
    expr_t *value = p_parse_expr(p, 0);
    if (!fallback_type_locked && value) {
      if (value->kind == NY_E_LITERAL && value->as.literal.kind == NY_LIT_STR) {
        fallback_type = "str";
        fallback_type_locked = true;
      } else if (value->kind == NY_E_LITERAL &&
                 value->as.literal.kind == NY_LIT_INT) {
        fallback_type = "i32";
        fallback_type_locked = true;
      } else if (value->kind == NY_E_LITERAL &&
                 value->as.literal.kind == NY_LIT_FLOAT) {
        fallback_type = "f64";
        fallback_type_locked = true;
      } else if (value->kind == NY_E_LITERAL &&
                 value->as.literal.kind == NY_LIT_BOOL) {
        fallback_type = "bool";
        fallback_type_locked = true;
      }
    }
    stmt_t *ret_blk = parse_table_return_block(p, value_tok, value);
    if (arm.patterns.len == 1 && !arm.guard &&
        ny_expr_is_wildcard_ident(first)) {
      match->as.match.default_conseq = ret_blk;
    } else {
      arm.conseq = ret_blk;
      vec_push_arena(p->arena, &match->as.match.arms, arm);
    }
    parser_match(p, NY_T_SEMI);
    parser_match(p, NY_T_COMMA);
  }
  parser_expect(p, NY_T_RBRACE, "'}' after comptime table", NULL);

  if (!match->as.match.default_conseq) {
    match->as.match.default_conseq = parse_table_return_block(
        p, tok, parse_table_ident_expr(p, tok, "default"));
  }

  stmt_t *body = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &body->as.block.body, match);

  stmt_t *fn = stmt_new(p->arena, NY_S_FUNC, tok);
  fn->as.fn.name = matcher_name;
  /*
   * The generated matcher is a scalar lookup, not an inferred dynamic
   * callback.  Give its result the same domain as its fallback so native
   * calls retain the raw scalar ABI instead of decoding an already-raw table
   * value as a tagged integer.
   */
  fn->as.fn.return_type = fallback_type;
  fn->as.fn.body = body;
  fn->as.fn.src_start = tok.lexeme;
  fn->as.fn.src_end = p->prev.lexeme + p->prev.len;
  /*
   * The generated matcher is a scalar lookup, not an `any` callback. Keeping
   * the key typed prevents ordinary integer match lowering from tagging it a
   * second time before range comparisons.
   */
  param_t raw = {
      .name = parser_intern(p, "raw", 3), .type = key_type, .def = NULL};
  /*
   * The fallback is a scalar in the same domain as `raw`; typing it keeps
   * the native call boundary from tagging it (and decoding `-7` to `-4`
   * across a module boundary).
   */
  param_t fallback = {.name = parser_intern(p, "default", 7),
                      .type = fallback_type,
                      .def = NULL};
  vec_push_arena(p->arena, &fn->as.fn.params, raw);
  vec_push_arena(p->arena, &fn->as.fn.params, fallback);

  stmt_t *wrapper_body = stmt_new(p->arena, NY_S_BLOCK, tok);
  stmt_t *wrapper_ret = stmt_new(p->arena, NY_S_RETURN, tok);
  wrapper_ret->as.ret.value =
      parse_table_call_expr(p, tok, matcher_name, "raw", "default");
  vec_push_arena(p->arena, &wrapper_body->as.block.body, wrapper_ret);

  stmt_t *wrapper = stmt_new(p->arena, NY_S_FUNC, tok);
  wrapper->as.fn.name = legacy_name;
  wrapper->as.fn.return_type = fallback_type;
  wrapper->as.fn.body = wrapper_body;
  wrapper->as.fn.src_start = tok.lexeme;
  wrapper->as.fn.src_end = p->prev.lexeme + p->prev.len;
  param_t wrapper_raw = {
      .name = parser_intern(p, "raw", 3), .type = key_type, .def = NULL};
  param_t wrapper_fallback = {
      .name = parser_intern(p, "default", 7),
      .type = fallback_type,
      .def = strcmp(fallback_type, "str") == 0    ? ct_string_expr(p, tok, "")
             : strcmp(fallback_type, "bool") == 0 ? ct_bool_expr(p, tok, false)
             : strcmp(fallback_type, "f64") == 0  ? ct_float_expr(p, tok, 0.0)
                                                  : ct_int_expr(p, tok, 0),
  };
  vec_push_arena(p->arena, &wrapper->as.fn.params, wrapper_raw);
  vec_push_arena(p->arena, &wrapper->as.fn.params, wrapper_fallback);

  stmt_t *out = stmt_new_transparent_block(p, tok);
  vec_push_arena(p->arena, &out->as.block.body, fn);
  vec_push_arena(p->arena, &out->as.block.body, wrapper);
  return out;
}

static int parse_diag_rule_call_arg_index(parser_t *p) {
  if (!tok_is_ident_text(p->cur, "call")) {
    parser_error(
        p, p->cur, "expected call.arg(n) in diagnostic rule predicate",
        "write 'when call.name == \"...\" && !is_literal(call.arg(0))'");
    return -1;
  }
  parser_advance(p);
  parser_expect(p, NY_T_DOT, "'.' before call predicate member", NULL);
  if (!tok_is_ident_text(p->cur, "arg")) {
    parser_error(p, p->cur, "expected arg in call.arg(n)",
                 "use call.arg(0), call.arg(1), etc.");
    return -1;
  }
  parser_advance(p);
  parser_expect(p, NY_T_LPAREN, "'(' after call.arg", NULL);
  if (p->cur.kind != NY_T_NUMBER) {
    parser_error(p, p->cur, "expected integer argument index in call.arg(n)",
                 "pass a 0-based argument index, e.g. call.arg(0)");
    return -1;
  }
  char buf[32];
  size_t n = p->cur.len < sizeof(buf) - 1 ? p->cur.len : sizeof(buf) - 1;
  memcpy(buf, p->cur.lexeme, n);
  buf[n] = '\0';
  int idx = atoi(buf);
  parser_advance(p);
  parser_expect(p, NY_T_RPAREN, "')' after call.arg index", NULL);
  return idx;
}

static bool parse_diag_rule_when(parser_t *p, ny_diag_rule_t *rule) {
  if (!tok_is_ident_text(p->cur, "when"))
    return true;
  parser_advance(p);
  if (!tok_is_ident_text(p->cur, "call")) {
    parser_error(p, p->cur, "expected call.name in diagnostic rule predicate",
                 "write 'when call.name == \"target_function\"'");
    return false;
  }
  parser_advance(p);
  parser_expect(p, NY_T_DOT, "'.' before call predicate member", NULL);
  if (!tok_is_ident_text(p->cur, "name")) {
    parser_error(p, p->cur, "expected name in call.name predicate",
                 "write 'call.name == \"target_function\"'");
    return false;
  }
  parser_advance(p);
  parser_expect(p, NY_T_EQ, "'==' after call.name", NULL);
  if (p->cur.kind != NY_T_STRING) {
    parser_error(
        p, p->cur, "diagnostic rule call.name expects a string literal",
        "wrap the function name in quotes, e.g. call.name == \"fn_name\"");
    return false;
  }
  size_t call_len = 0;
  const char *call_name = parser_decode_string(p, p->cur, &call_len);
  rule->call_name = parser_intern(p, call_name, call_len);
  parser_advance(p);

  if (!parser_match(p, NY_T_AND))
    return true;
  parser_expect(p, NY_T_NOT,
                "'!' before is_literal in diagnostic rule predicate", NULL);
  if (!tok_is_ident_text(p->cur, "is_literal")) {
    parser_error(
        p, p->cur,
        "expected is_literal(call.arg(n)) in diagnostic rule predicate",
        "check literalness with !is_literal(call.arg(0))");
    return false;
  }
  parser_advance(p);
  parser_expect(p, NY_T_LPAREN, "'(' after is_literal", NULL);
  int idx = parse_diag_rule_call_arg_index(p);
  parser_expect(p, NY_T_RPAREN, "')' after is_literal argument", NULL);
  rule->arg_index = idx;
  rule->reject_non_literal = idx >= 0;
  return true;
}

static stmt_t *parse_comptime_diagnostic_rule_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_COMPTIME)
    parser_advance(p);
  if (tok_is_ident_text(p->cur, "diagnostic")) {
    parser_advance(p);
  } else if (!tok_is_ident_text(p->cur, "rule")) {
    parser_error(
        p, p->cur, "expected diagnostic rule declaration",
        "write comptime diagnostic rule name { when ... error ... fix ... }");
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  if (!tok_is_ident_text(p->cur, "rule")) {
    parser_error(p, p->cur, "expected 'rule' after 'comptime diagnostic'",
                 "write 'comptime diagnostic rule name { ... }'");
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  parser_advance(p);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected diagnostic rule name",
                 "give the diagnostic rule a name identifier");
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  ny_diag_rule_t rule = {
      .name = parser_intern_hash(p, p->cur.lexeme, p->cur.len, p->cur.hash),
      .arg_index = -1};
  parser_advance(p);
  parser_expect(p, NY_T_LBRACE, "'{' after diagnostic rule name", NULL);
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (parser_match(p, NY_T_SEMI) || parser_match(p, NY_T_COMMA))
      continue;
    if (tok_is_ident_text(p->cur, "when")) {
      parse_diag_rule_when(p, &rule);
      parser_match(p, NY_T_SEMI);
      continue;
    }
    if (tok_is_ident_text(p->cur, "error")) {
      parser_advance(p);
      if (p->cur.kind != NY_T_STRING) {
        parser_error(p, p->cur,
                     "diagnostic rule error expects a string literal",
                     "provide an error message string, e.g. error \"message\"");
        parser_sync_stmt_boundary(p);
        continue;
      }
      size_t msg_len = 0;
      const char *msg = parser_decode_string(p, p->cur, &msg_len);
      rule.message = parser_intern(p, msg, msg_len);
      parser_advance(p);
      parser_match(p, NY_T_SEMI);
      continue;
    }
    if (tok_is_ident_text(p->cur, "fix")) {
      parser_advance(p);
      if (p->cur.kind != NY_T_STRING) {
        parser_error(
            p, p->cur, "diagnostic rule fix expects a string literal",
            "provide a fix suggestion string, e.g. fix \"how to resolve\"");
        parser_sync_stmt_boundary(p);
        continue;
      }
      size_t fix_len = 0;
      const char *fix = parser_decode_string(p, p->cur, &fix_len);
      rule.fix = parser_intern(p, fix, fix_len);
      parser_advance(p);
      parser_match(p, NY_T_SEMI);
      continue;
    }
    parser_error(p, p->cur, "unknown diagnostic rule clause",
                 "supported clauses are when, error, and fix");
    parser_sync_stmt_boundary(p);
  }
  parser_expect(p, NY_T_RBRACE, "'}' after diagnostic rule", NULL);
  if (!rule.call_name || rule.arg_index < 0 || !rule.message) {
    parser_error(p, tok, "incomplete diagnostic rule",
                 "rules need when call.name == \"...\" && "
                 "!is_literal(call.arg(n)) plus error text");
  } else {
    vec_push_arena(p->arena, &p->ct_diag_rules, rule);
  }
  return stmt_new(p->arena, NY_S_BLOCK, tok);
}

parser_ct_layout_meta *parser_find_layout_meta(parser_t *p, const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < p->ct_layouts.len; i++) {
    if (strcmp(p->ct_layouts.data[i].name, name) == 0)
      return &p->ct_layouts.data[i];
  }
  if (p->current_module && *p->current_module && !strchr(name, '.')) {
    size_t mlen = strlen(p->current_module);
    size_t nlen = strlen(name);
    char *qualified = arena_alloc(p->arena, mlen + 1 + nlen + 1);
    memcpy(qualified, p->current_module, mlen);
    qualified[mlen] = '.';
    memcpy(qualified + mlen + 1, name, nlen + 1);
    for (size_t i = 0; i < p->ct_layouts.len; i++) {
      if (strcmp(p->ct_layouts.data[i].name, qualified) == 0)
        return &p->ct_layouts.data[i];
    }
  }
  if (!strchr(name, '.')) {
    for (size_t i = 0; i < p->ct_layouts.len; i++) {
      const char *leaf = ny_tail_name(p->ct_layouts.data[i].name);
      if (leaf && strcmp(leaf, name) == 0)
        return &p->ct_layouts.data[i];
    }
  }
  return NULL;
}

static parser_ct_module_meta *parser_find_module_meta(parser_t *p,
                                                      const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < p->ct_modules.len; i++) {
    if (strcmp(p->ct_modules.data[i].name, name) == 0)
      return &p->ct_modules.data[i];
  }
  if (p->current_module && *p->current_module && !strchr(name, '.')) {
    size_t mlen = strlen(p->current_module);
    size_t nlen = strlen(name);
    char *qualified = arena_alloc(p->arena, mlen + 1 + nlen + 1);
    memcpy(qualified, p->current_module, mlen);
    qualified[mlen] = '.';
    memcpy(qualified + mlen + 1, name, nlen + 1);
    for (size_t i = 0; i < p->ct_modules.len; i++) {
      if (strcmp(p->ct_modules.data[i].name, qualified) == 0)
        return &p->ct_modules.data[i];
    }
  }
  if (!strchr(name, '.')) {
    for (size_t i = 0; i < p->ct_modules.len; i++) {
      const char *leaf = ny_tail_name(p->ct_modules.data[i].name);
      if (leaf && strcmp(leaf, name) == 0)
        return &p->ct_modules.data[i];
    }
  }
  return NULL;
}

typedef enum ct_reflect_kind_t {
  CT_REFLECT_FIELDS,
  CT_REFLECT_EXPORTS,
  CT_REFLECT_VARIANTS,
  CT_REFLECT_ATTRIBUTES,
  CT_REFLECT_TEMPLATE,
} ct_reflect_kind_t;

typedef enum ct_value_kind_t {
  CT_VALUE_STRING,
  CT_VALUE_IDENT,
  CT_VALUE_INT,
  CT_VALUE_BOOL,
  CT_VALUE_AST_BLOCK,
  CT_VALUE_AST_EXPR,
  CT_VALUE_AST_LIST,
} ct_value_kind_t;

typedef struct ct_value_t {
  ct_value_kind_t kind;
  const char *s;
  size_t s_len;
  bool s_counted;
  int64_t i;
  bool b;
  stmt_t *block;
  expr_t *expr;
  struct ct_value_t *items;
  size_t items_len;
  uint32_t syntax_ctx;
  token_t source_tok;
  bool has_source_tok;
  /*
   * True only for syntax values produced by a checked Code<T> Result
   * constructor at a compile-time template boundary.
   */
  bool typed_code_result;
} ct_value_t;

typedef struct ct_bind_t {
  const char *name;
  ct_value_t value;
} ct_bind_t;
typedef VEC(ct_bind_t) ct_bind_list;

typedef struct ct_reflect_ctx_t {
  ct_reflect_kind_t kind;
  const char *var;
  const char *layout_name;
  const char *module_name;
  const char *template_module_name;
  const token_t *emission_origin;
  layout_field_t *field;
  int field_index;
  const char *export_name;
  const char *enum_name;
  stmt_enum_item_t *variant;
  int variant_index;
  attribute_t *attribute;
  bool pattern_slot;
  ct_bind_list binds;
  ct_value_t reflected_ident;
} ct_reflect_ctx_t;

static expr_t *ct_mark_expr_emission(expr_t *expr, ct_reflect_ctx_t *ctx);

static const char *ct_stmt_decl_name(const stmt_t *stmt) {
  if (!stmt)
    return NULL;
  switch (stmt->kind) {
  case NY_S_FUNC:
    return stmt->as.fn.name;
  case NY_S_LEMMA:
    return stmt->as.lemma.name;
  case NY_S_EXTERN:
    return stmt->as.ext.name;
  case NY_S_LAYOUT:
    return stmt->as.layout.name;
  case NY_S_MODULE:
    return stmt->as.module.name;
  case NY_S_STRUCT:
    return stmt->as.struc.name;
  case NY_S_ENUM:
    return stmt->as.enu.name;
  case NY_S_DEFINE:
    return stmt->as.def.name;
  case NY_S_REFINE:
    return stmt->as.refine.name;
  case NY_S_TRAIT:
    return stmt->as.trait.name;
  default:
    return NULL;
  }
}

static const char *ct_stmt_kind_name(stmt_kind_t kind) {
  switch (kind) {
  case NY_S_BLOCK:
    return "block";
  case NY_S_USE:
    return "use";
  case NY_S_VAR:
    return "var";
  case NY_S_EXPR:
    return "expr_stmt";
  case NY_S_IF:
    return "if";
  case NY_S_GUARD:
    return "layout_guard";
  case NY_S_WHILE:
    return "while";
  case NY_S_FOR:
    return "for";
  case NY_S_TRY:
    return "try";
  case NY_S_FUNC:
    return "func";
  case NY_S_LEMMA:
    return "lemma";
  case NY_S_EXTERN:
    return "extern";
  case NY_S_LINK:
    return "link";
  case NY_S_RETURN:
    return "return";
  case NY_S_LABEL:
    return "label";
  case NY_S_DEFER:
    return "defer";
  case NY_S_GOTO:
    return "goto";
  case NY_S_BREAK:
    return "break";
  case NY_S_CONTINUE:
    return "continue";
  case NY_S_LAYOUT:
    return "layout";
  case NY_S_MATCH:
    return "match_stmt";
  case NY_S_MODULE:
    return "module";
  case NY_S_EXPORT:
    return "export";
  case NY_S_STRUCT:
    return "struct";
  case NY_S_ENUM:
    return "enum";
  case NY_S_MACRO:
    return "macro";
  case NY_S_INCLUDE:
    return "include";
  case NY_S_DEFINE:
    return "define";
  case NY_S_OPERATOR:
    return "operator";
  case NY_S_IMPL:
    return "impl";
  case NY_S_REFINE:
    return "refine";
  case NY_S_TRAIT:
    return "trait";
  default:
    return "unknown";
  }
}

static const char *ct_stmt_category(stmt_kind_t kind) {
  switch (kind) {
  case NY_S_USE:
  case NY_S_VAR:
  case NY_S_FUNC:
  case NY_S_LEMMA:
  case NY_S_EXTERN:
  case NY_S_LINK:
  case NY_S_LAYOUT:
  case NY_S_MODULE:
  case NY_S_EXPORT:
  case NY_S_STRUCT:
  case NY_S_ENUM:
  case NY_S_MACRO:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_OPERATOR:
  case NY_S_IMPL:
  case NY_S_REFINE:
  case NY_S_TRAIT:
    return "declarations";
  case NY_S_BLOCK:
  case NY_S_EXPR:
  case NY_S_IF:
  case NY_S_GUARD:
  case NY_S_WHILE:
  case NY_S_FOR:
  case NY_S_TRY:
  case NY_S_RETURN:
  case NY_S_LABEL:
  case NY_S_DEFER:
  case NY_S_GOTO:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
  case NY_S_MATCH:
    return "statements";
  default:
    return "unknown";
  }
}

static const char *ct_stmt_actual_category(const stmt_t *stmt) {
  if (stmt && stmt->kind == NY_S_VAR && !stmt->as.var.is_decl)
    return "statements";
  return stmt ? ct_stmt_category(stmt->kind) : "unknown";
}

static const char *ct_expr_kind_name(expr_kind_t kind) {
  switch (kind) {
  case NY_E_IDENT:
    return "ident";
  case NY_E_LITERAL:
    return "literal";
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
  case NY_E_ASM:
    return "asm";
  case NY_E_COMPTIME:
    return "comptime";
  case NY_E_FSTRING:
    return "fstring";
  case NY_E_INFERRED_MEMBER:
    return "inferred_member";
  case NY_E_EMBED:
    return "embed";
  case NY_E_MATCH:
    return "match";
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
  case NY_E_QUOTE:
    return "quote";
  case NY_E_SPLICE:
    return "splice";
  case NY_E_EXISTS:
    return "exists";
  default:
    return "unknown";
  }
}

static bool ct_block_is_expression_code(const stmt_t *block) {
  const stmt_t *stmt = block;
  if (!stmt)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    if (stmt->as.block.body.len != 1)
      return false;
    stmt = stmt->as.block.body.data[0];
  }
  if (!stmt || stmt->kind != NY_S_EXPR || !stmt->as.expr.expr)
    return false;
  expr_t *expr = stmt->as.expr.expr;
  return !(expr->kind == NY_E_BINARY && expr->as.binary.op &&
           strcmp(expr->as.binary.op, "=") == 0);
}

static const char *ct_code_sequence_category(const ct_value_t *items,
                                             size_t len) {
  if (len == 0)
    return "empty";
  const char *category = NULL;
  for (size_t i = 0; i < len; ++i) {
    const ct_value_t *item = &items[i];
    const char *next = "invalid";
    if (item->kind == CT_VALUE_AST_EXPR || item->kind == CT_VALUE_IDENT)
      next = "expressions";
    else if (item->kind == CT_VALUE_AST_BLOCK && item->block) {
      if (ct_block_is_expression_code(item->block))
        next = "expressions";
      else if (item->block->kind == NY_S_BLOCK &&
               item->block->as.block.body.len > 0) {
        next = NULL;
        for (size_t j = 0; j < item->block->as.block.body.len; ++j) {
          stmt_t *child = item->block->as.block.body.data[j];
          const char *child_category = ct_stmt_actual_category(child);
          if (!next)
            next = child_category;
          else if (strcmp(next, child_category) != 0) {
            next = "mixed";
            break;
          }
        }
        if (!next)
          next = "invalid";
      } else {
        next = ct_stmt_actual_category(item->block);
      }
    } else if (item->kind == CT_VALUE_AST_LIST) {
      next = ct_code_sequence_category(item->items, item->items_len);
    }
    if (strcmp(next, "unknown") == 0 || strcmp(next, "invalid") == 0)
      return next;
    if (!category)
      category = next;
    else if (strcmp(category, next) != 0)
      return "mixed";
  }
  return category ? category : "empty";
}

static const char *ct_value_code_category(const ct_value_t *value) {
  if (!value)
    return "invalid";
  if (value->kind == CT_VALUE_IDENT)
    return "ident";
  if (value->kind == CT_VALUE_AST_EXPR && value->expr)
    return "expr";
  if (value->kind == CT_VALUE_AST_BLOCK && value->block) {
    /*
     * `quote{expr}` is represented by the parser as a one-expression block.
     */
    if (ct_block_is_expression_code(value->block))
      return "expr";
    if (value->block->kind == NY_S_BLOCK &&
        value->block->as.block.body.len > 0) {
      const char *category = NULL;
      for (size_t i = 0; i < value->block->as.block.body.len; ++i) {
        stmt_t *child = value->block->as.block.body.data[i];
        const char *child_category = ct_stmt_actual_category(child);
        if (!category)
          category = child_category;
        else if (strcmp(category, child_category) != 0)
          return "mixed";
      }
      if (category && strcmp(category, "declarations") == 0)
        return "decl";
      if (category && strcmp(category, "statements") == 0)
        return "stmt";
    }
    const char *category = ct_stmt_actual_category(value->block);
    if (strcmp(category, "declarations") == 0)
      return "decl";
    if (strcmp(category, "statements") == 0)
      return "stmt";
    return "invalid";
  }
  if (value->kind == CT_VALUE_AST_LIST) {
    const char *category =
        ct_code_sequence_category(value->items, value->items_len);
    if (strcmp(category, "expressions") == 0)
      return "exprs";
    if (strcmp(category, "declarations") == 0)
      return "decls";
    if (strcmp(category, "statements") == 0)
      return "stmts";
    return category;
  }
  return "invalid";
}

static stmt_t *ct_single_code_decl(const ct_value_t *code) {
  if (!code)
    return NULL;
  stmt_t *decl = NULL;
  if (code->kind == CT_VALUE_AST_BLOCK)
    decl = code->block;
  else if (code->kind == CT_VALUE_AST_LIST && code->items_len == 1 &&
           code->items && code->items[0].kind == CT_VALUE_AST_BLOCK)
    decl = code->items[0].block;
  return decl && strcmp(ct_stmt_actual_category(decl), "declarations") == 0
             ? decl
             : NULL;
}

void parser_record_ct_decl(parser_t *p, stmt_t *stmt) {
  if (!p || !stmt)
    return;
  if (stmt->kind == NY_S_BLOCK && stmt->as.block.transparent) {
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      parser_record_ct_decl(p, stmt->as.block.body.data[i]);
    return;
  }
  /*
   * Only file and module declarations are visible to codeof.
   */
  if (p->block_depth != 0)
    return;
  /*
   * Assignments share NY_S_VAR with declarations but are not codeof targets.
   */
  if (stmt->kind == NY_S_VAR && !stmt->as.var.is_decl)
    return;
  const char *name = ct_stmt_decl_name(stmt);
  if (name && *name) {
    parser_ct_decl_meta entry = {.name = name, .decl = stmt};
    vec_push_arena(p->arena, &p->ct_decls, entry);
  }
  if (stmt->kind == NY_S_VAR) {
    for (size_t i = 0; i < stmt->as.var.names.len; ++i) {
      parser_ct_decl_meta entry = {.name = stmt->as.var.names.data[i],
                                   .decl = stmt};
      vec_push_arena(p->arena, &p->ct_decls, entry);
    }
  }
}

static stmt_t *parser_find_ct_decl(parser_t *p, const char *name,
                                   bool *ambiguous) {
  if (ambiguous)
    *ambiguous = false;
  if (!p || !name)
    return NULL;
  stmt_t *found = NULL;
  for (size_t i = 0; i < p->ct_decls.len; ++i) {
    const parser_ct_decl_meta *entry = &p->ct_decls.data[i];
    if (entry->name && strcmp(entry->name, name) == 0) {
      if (found && found != entry->decl) {
        if (ambiguous)
          *ambiguous = true;
        return NULL;
      }
      found = entry->decl;
    }
  }
  if (found || strchr(name, '.'))
    return found;
  if (p->current_module && *p->current_module) {
    size_t module_len = strlen(p->current_module);
    size_t name_len = strlen(name);
    char *qualified = arena_alloc(p->arena, module_len + name_len + 2);
    memcpy(qualified, p->current_module, module_len);
    qualified[module_len] = '.';
    memcpy(qualified + module_len + 1, name, name_len + 1);
    found = parser_find_ct_decl(p, qualified, ambiguous);
    if (found || (ambiguous && *ambiguous))
      return found;
  }
  for (size_t i = 0; i < p->ct_decls.len; ++i) {
    const parser_ct_decl_meta *entry = &p->ct_decls.data[i];
    if (!entry->name || strcmp(ny_tail_name(entry->name), name) != 0)
      continue;
    if (found && found != entry->decl) {
      if (ambiguous)
        *ambiguous = true;
      return NULL;
    }
    found = entry->decl;
  }
  return found;
}

static expr_t *ct_string_expr(parser_t *p, token_t tok, const char *value) {
  if (!value)
    value = "";
  expr_t *e = expr_new(p->arena, NY_E_LITERAL, tok);
  e->as.literal.kind = NY_LIT_STR;
  e->as.literal.hint = NY_LIT_HINT_NONE;
  e->as.literal.hint_explicit = false;
  e->as.literal.as.s.data = parser_intern(p, value, strlen(value));
  e->as.literal.as.s.len = strlen(value);
  return e;
}

static expr_t *ct_int_expr(parser_t *p, token_t tok, int64_t value) {
  expr_t *e = expr_new(p->arena, NY_E_LITERAL, tok);
  e->as.literal.kind = NY_LIT_INT;
  e->as.literal.hint = NY_LIT_HINT_NONE;
  e->as.literal.hint_explicit = false;
  e->as.literal.as.i = value;
  return e;
}

static expr_t *ct_float_expr(parser_t *p, token_t tok, double value) {
  expr_t *e = expr_new(p->arena, NY_E_LITERAL, tok);
  e->as.literal.kind = NY_LIT_FLOAT;
  e->as.literal.hint = NY_LIT_HINT_NONE;
  e->as.literal.hint_explicit = false;
  e->as.literal.as.f = value;
  return e;
}

static expr_t *ct_bool_expr(parser_t *p, token_t tok, bool value) {
  expr_t *e = expr_new(p->arena, NY_E_LITERAL, tok);
  e->as.literal.kind = NY_LIT_BOOL;
  e->as.literal.hint = NY_LIT_HINT_NONE;
  e->as.literal.hint_explicit = false;
  e->as.literal.as.b = value;
  return e;
}

static expr_t *ct_variant_payload_fields_expr(parser_t *p, token_t tok,
                                              const stmt_enum_item_t *variant,
                                              ct_reflect_ctx_t *ctx) {
  expr_t *fields =
      ct_mark_expr_emission(expr_new(p->arena, NY_E_TUPLE, tok), ctx);
  if (!variant)
    return fields;
  for (size_t i = 0; i < variant->fields.len; ++i) {
    const enum_field_t *field = &variant->fields.data[i];
    expr_t *pair =
        ct_mark_expr_emission(expr_new(p->arena, NY_E_TUPLE, tok), ctx);
    vec_push_arena(
        p->arena, &pair->as.list_like,
        ct_mark_expr_emission(ct_string_expr(p, tok, field->name), ctx));
    vec_push_arena(
        p->arena, &pair->as.list_like,
        ct_mark_expr_emission(ct_string_expr(p, tok, field->type_name), ctx));
    vec_push_arena(p->arena, &fields->as.list_like, pair);
  }
  return fields;
}

static expr_t *ct_variant_payload_origins_expr(parser_t *p, token_t tok,
                                               const stmt_enum_item_t *variant,
                                               ct_reflect_ctx_t *ctx) {
  expr_t *fields =
      ct_mark_expr_emission(expr_new(p->arena, NY_E_TUPLE, tok), ctx);
  if (!variant)
    return fields;
  for (size_t i = 0; i < variant->fields.len; ++i) {
    const enum_field_t *field = &variant->fields.data[i];
    expr_t *row =
        ct_mark_expr_emission(expr_new(p->arena, NY_E_TUPLE, tok), ctx);
    vec_push_arena(
        p->arena, &row->as.list_like,
        ct_mark_expr_emission(ct_string_expr(p, tok, field->name), ctx));
    vec_push_arena(
        p->arena, &row->as.list_like,
        ct_mark_expr_emission(ct_string_expr(p, tok, field->type_name), ctx));
    vec_push_arena(
        p->arena, &row->as.list_like,
        ct_mark_expr_emission(
            ct_string_expr(p, tok,
                           field->tok.filename ? field->tok.filename : ""),
            ctx));
    vec_push_arena(
        p->arena, &row->as.list_like,
        ct_mark_expr_emission(ct_int_expr(p, tok, field->tok.line), ctx));
    vec_push_arena(
        p->arena, &row->as.list_like,
        ct_mark_expr_emission(ct_int_expr(p, tok, field->tok.col), ctx));
    vec_push_arena(p->arena, &fields->as.list_like, row);
  }
  return fields;
}

static const ct_value_t *ct_find_bind(ct_reflect_ctx_t *ctx, const char *name) {
  if (!ctx || !name ||
      (ctx->kind != CT_REFLECT_TEMPLATE && ctx->binds.len == 0))
    return NULL;
  for (size_t i = 0; i < ctx->binds.len; i++) {
    if (ctx->binds.data[i].name && strcmp(ctx->binds.data[i].name, name) == 0)
      return &ctx->binds.data[i].value;
  }
  return NULL;
}

static bool ct_current_unit_declares(parser_t *p, const char *name) {
  if (!p || !name)
    return false;
  for (size_t i = 0; i < p->ct_decls.len; ++i) {
    const char *decl_name = p->ct_decls.data[i].name;
    if (decl_name && strcmp(decl_name, name) == 0)
      return true;
    if (decl_name && p->current_module && *p->current_module) {
      size_t module_len = strlen(p->current_module);
      size_t decl_len = strlen(decl_name);
      if (decl_len > module_len &&
          strncmp(decl_name, p->current_module, module_len) == 0 &&
          decl_name[module_len] == '.' &&
          strcmp(decl_name + module_len + 1, name) == 0)
        return true;
    }
  }
  return false;
}

static bool ct_ctx_has_template_bindings(const ct_reflect_ctx_t *ctx) {
  return ctx && (ctx->kind == CT_REFLECT_TEMPLATE || ctx->binds.len > 0);
}

static const char *ct_value_text(parser_t *p, const ct_value_t *v) {
  if (!v)
    return "";
  if (v->kind == CT_VALUE_STRING || v->kind == CT_VALUE_IDENT)
    return v->s ? v->s : "";
  if (v->kind == CT_VALUE_BOOL)
    return v->b ? "true" : "false";
  char *buf = arena_alloc(p->arena, 64);
  snprintf(buf, 64, "%lld", (long long)v->i);
  return buf;
}

static const char *ct_substitute_string(parser_t *p, const char *value,
                                        size_t value_len, size_t *result_len,
                                        ct_reflect_ctx_t *ctx) {
  *result_len = value_len;
  if (!value || !ct_ctx_has_template_bindings(ctx))
    return value;
  size_t cap = value_len + 32;
  char *buf = arena_alloc(p->arena, cap);
  size_t out = 0;
  for (size_t i = 0; i < value_len;) {
    if (value[i] == '$' && i + 1 < value_len && value[i + 1] == '{') {
      size_t j = i + 2;
      while (j < value_len && value[j] != '}')
        ++j;
      if (j == value_len) {
        size_t tail_len = value_len - i;
        if (out + tail_len + 1 > cap) {
          cap = out + tail_len + 1;
          char *next = arena_alloc(p->arena, cap);
          memcpy(next, buf, out);
          buf = next;
        }
        memcpy(buf + out, value + i, tail_len);
        out += tail_len;
        break;
      }
      char key_buf[128];
      size_t key_len = j - (i + 2);
      const char *key = NULL;
      if (key_len < sizeof(key_buf)) {
        memcpy(key_buf, value + i + 2, key_len);
        key_buf[key_len] = '\0';
        key = key_buf;
      } else {
        char *long_key = arena_alloc(p->arena, key_len + 1);
        memcpy(long_key, value + i + 2, key_len);
        long_key[key_len] = '\0';
        key = long_key;
      }
      const ct_value_t *bound = ct_find_bind(ctx, key);
      if (!bound ||
          (bound->kind != CT_VALUE_IDENT && bound->kind != CT_VALUE_STRING &&
           bound->kind != CT_VALUE_INT && bound->kind != CT_VALUE_BOOL)) {
        parser_error(p, p->cur, "invalid comptime value in string splice",
                     "use a string, scalar, or identifier template argument");
      }
      const char *text = bound ? ct_value_text(p, bound) : "";
      size_t text_len =
          bound && bound->kind == CT_VALUE_STRING && bound->s_counted
              ? bound->s_len
              : strlen(text);
      if (out + text_len + 1 > cap) {
        cap = (out + text_len + 1) * 2;
        char *next = arena_alloc(p->arena, cap);
        memcpy(next, buf, out);
        buf = next;
      }
      memcpy(buf + out, text, text_len);
      out += text_len;
      i = j + 1;
      continue;
    }
    if (out + 2 > cap) {
      cap *= 2;
      char *next = arena_alloc(p->arena, cap);
      memcpy(next, buf, out);
      buf = next;
    }
    buf[out++] = value[i++];
  }
  buf[out] = '\0';
  *result_len = out;
  return parser_intern(p, buf, out);
}

static bool ct_valid_ident_fragment(const char *value) {
  if (!value || !*value)
    return false;
  for (const unsigned char *ch = (const unsigned char *)value; *ch; ++ch) {
    if (!((*ch >= 'a' && *ch <= 'z') || (*ch >= 'A' && *ch <= 'Z') ||
          (*ch >= '0' && *ch <= '9') || *ch == '_'))
      return false;
  }
  return true;
}

static bool ct_valid_ident_name(const char *value) {
  if (!value || !*value)
    return false;
  bool segment_start = true;
  for (const unsigned char *ch = (const unsigned char *)value; *ch; ++ch) {
    if (*ch == '.') {
      if (segment_start)
        return false;
      segment_start = true;
      continue;
    }
    if (segment_start && !((*ch >= 'a' && *ch <= 'z') ||
                           (*ch >= 'A' && *ch <= 'Z') || *ch == '_'))
      return false;
    if (!((*ch >= 'a' && *ch <= 'z') || (*ch >= 'A' && *ch <= 'Z') ||
          (*ch >= '0' && *ch <= '9') || *ch == '_'))
      return false;
    segment_start = false;
  }
  return !segment_start;
}

static expr_t *ct_value_expr(parser_t *p, token_t tok, const ct_value_t *v) {
  if (!v)
    return NULL;
  if (v->has_source_tok)
    tok = v->source_tok;
  switch (v->kind) {
  case CT_VALUE_STRING: {
    expr_t *value = expr_new(p->arena, NY_E_LITERAL, tok);
    value->as.literal.kind = NY_LIT_STR;
    value->as.literal.as.s.data = v->s ? v->s : "";
    value->as.literal.as.s.len =
        v->s_counted ? v->s_len : strlen(value->as.literal.as.s.data);
    return value;
  }
  case CT_VALUE_IDENT: {
    expr_t *ident = parse_table_ident_expr(p, tok, v->s ? v->s : "");
    ident->as.ident.syntax_ctx = v->syntax_ctx;
    return ident;
  }
  case CT_VALUE_INT:
    return ct_int_expr(p, tok, v->i);
  case CT_VALUE_BOOL:
    return ct_bool_expr(p, tok, v->b);
  case CT_VALUE_AST_EXPR:
    return v->expr ? v->expr : ct_string_expr(p, tok, "");
  case CT_VALUE_AST_BLOCK: {
    expr_t *q = expr_new(p->arena, NY_E_QUOTE, tok);
    q->as.quote.body = v->block;
    q->as.quote.syntax_ctx = v->syntax_ctx;
    return q;
  }
  case CT_VALUE_AST_LIST:
    return NULL;
  }
  return NULL;
}

static expr_t *ct_ident_splice_expr(parser_t *p, token_t tok,
                                    const ct_value_t *value,
                                    uint32_t splice_ctx,
                                    ct_reflect_ctx_t *ctx) {
  if (!value || value->kind != CT_VALUE_IDENT)
    return ct_value_expr(p, tok, value);
  token_t source_tok = value->has_source_tok ? value->source_tok : tok;
  expr_t *splice =
      ct_mark_expr_emission(expr_new(p->arena, NY_E_SPLICE, tok), ctx);
  splice->as.splice.expr =
      ct_mark_expr_emission(ct_value_expr(p, source_tok, value), ctx);
  splice->as.splice.syntax_ctx = splice_ctx;
  return splice;
}

static const char *ct_substitute_name(parser_t *p, const char *name,
                                      ct_reflect_ctx_t *ctx) {
  if (!name || !ct_ctx_has_template_bindings(ctx) || !strstr(name, "${"))
    return name;
  size_t cap = strlen(name) + 32;
  char *buf = arena_alloc(p->arena, cap);
  size_t out = 0;
  for (size_t i = 0; name[i];) {
    if (name[i] == '$' && name[i + 1] == '{') {
      size_t j = i + 2;
      while (name[j] && name[j] != '}')
        j++;
      if (name[j] == '}') {
        char key_buf[128];
        size_t key_len = j - (i + 2);
        const char *key = NULL;
        if (key_len < sizeof(key_buf)) {
          memcpy(key_buf, name + i + 2, key_len);
          key_buf[key_len] = '\0';
          key = key_buf;
        } else {
          char *long_key = arena_alloc(p->arena, key_len + 1);
          memcpy(long_key, name + i + 2, key_len);
          long_key[key_len] = '\0';
          key = long_key;
        }
        const ct_value_t *bound = ct_find_bind(ctx, key);
        const char *val = "";
        if (!bound) {
          /*
           * Reflection facts that are identifiers may be used directly in
           * declaration-name splices. Other reflected values remain
           * expression-only, so they cannot capture syntax via string
           * coercion.
           */
          if (ctx && ctx->kind == CT_REFLECT_FIELDS && ctx->field &&
              strcmp(key, "field.name") == 0) {
            val = ctx->field->name ? ctx->field->name : "";
          } else if (ctx && ctx->kind == CT_REFLECT_FIELDS && ctx->field &&
                     (strcmp(key, "field.type") == 0 ||
                      strcmp(key, "field.type_name") == 0)) {
            val = ctx->field->type_name ? ctx->field->type_name : "";
          } else {
            parser_error(p, p->cur, "unknown comptime identifier splice",
                         "use a declared template parameter inside `${...}`");
          }
        } else if (bound->kind == CT_VALUE_IDENT) {
          val = bound->s ? bound->s : "";
        } else {
          parser_error(p, p->cur,
                       "string or scalar interpolation into an identifier "
                       "requires ident(...)",
                       "wrap the value as `ident(name)` at the template call");
        }
        size_t val_len = strlen(val);
        if (out + val_len + 1 > cap) {
          cap = (out + val_len + 1) * 2;
          char *nb = arena_alloc(p->arena, cap);
          memcpy(nb, buf, out);
          buf = nb;
        }
        memcpy(buf + out, val, val_len);
        out += val_len;
        i = j + 1;
        continue;
      }
    }
    if (out + 2 > cap) {
      cap *= 2;
      char *nb = arena_alloc(p->arena, cap);
      memcpy(nb, buf, out);
      buf = nb;
    }
    buf[out++] = name[i++];
  }
  buf[out] = '\0';
  if (!ct_valid_ident_name(buf)) {
    parser_error(p, p->cur,
                 "identifier splice does not form a valid identifier",
                 "start with a letter or underscore; use digits only after it");
    return name;
  }
  return parser_intern(p, buf, out);
}

static const char *ct_substitute_symbol_name(parser_t *p, const char *name,
                                             ct_reflect_ctx_t *ctx) {
  if (!name || !ct_ctx_has_template_bindings(ctx))
    return name;
  size_t prefix_len = 0;
  while (name[prefix_len] == '*' || name[prefix_len] == '?')
    prefix_len++;
  if (prefix_len > 0) {
    const char *core = ct_substitute_symbol_name(p, name + prefix_len, ctx);
    if (!core || strcmp(core, name + prefix_len) == 0)
      return name;
    size_t core_len = strlen(core);
    char *substituted = arena_alloc(p->arena, prefix_len + core_len + 1);
    if (!substituted)
      return name;
    memcpy(substituted, name, prefix_len);
    memcpy(substituted + prefix_len, core, core_len + 1);
    return parser_intern(p, substituted, prefix_len + core_len);
  }
  const char *generic_open = strchr(name, '<');
  if (generic_open) {
    size_t name_len = strlen(name);
    if (name_len < 3 || name[name_len - 1] != '>')
      return name;
    char *base_name =
        arena_strndup(p->arena, name, (size_t)(generic_open - name));
    const char *base = ct_substitute_symbol_name(p, base_name, ctx);
    size_t cap = name_len + 32;
    if (!base)
      return name;
    size_t base_len = strlen(base);
    if (base_len + 2 > cap)
      cap = (base_len + 2) * 2;
    char *buf = arena_alloc(p->arena, cap);
    if (!buf)
      return name;
    size_t out = 0;
    memcpy(buf, base, base_len);
    out = base_len;
    buf[out++] = '<';
    size_t arg_start = (size_t)(generic_open - name) + 1;
    size_t nesting = 0;
    bool closed = false;
    for (size_t i = arg_start; i < name_len; ++i) {
      bool at_end = false;
      if (name[i] == '<')
        nesting++;
      else if (name[i] == '>') {
        if (nesting == 0) {
          at_end = true;
          closed = true;
        } else {
          nesting--;
        }
      } else if (name[i] == ',' && nesting == 0) {
        at_end = true;
      }
      if (!at_end)
        continue;
      size_t arg_len = i - arg_start;
      char *arg = arena_strndup(p->arena, name + arg_start, arg_len);
      const ct_value_t *arg_binding = ct_find_bind(ctx, arg);
      const char *substituted_arg =
          arg_binding && arg_binding->kind == CT_VALUE_INT
              ? ct_value_text(p, arg_binding)
              : ct_substitute_symbol_name(p, arg, ctx);
      if (!substituted_arg)
        return name;
      size_t substituted_len = strlen(substituted_arg);
      if (out + substituted_len + 3 > cap) {
        cap = (out + substituted_len + 3) * 2;
        char *grown = arena_alloc(p->arena, cap);
        if (!grown)
          return name;
        memcpy(grown, buf, out);
        buf = grown;
      }
      memcpy(buf + out, substituted_arg, substituted_len);
      out += substituted_len;
      if (i < name_len - 1) {
        buf[out++] = ',';
        arg_start = i + 1;
      }
      if (closed)
        break;
    }
    if (!closed)
      return name;
    buf[out++] = '>';
    buf[out] = '\0';
    return parser_intern(p, buf, out);
  }
  const ct_value_t *v = ct_find_bind(ctx, name);
  if (v && v->kind == CT_VALUE_IDENT)
    return parser_intern(p, v->s ? v->s : "", strlen(v->s ? v->s : ""));
  if (v) {
    parser_error(
        p, p->cur,
        "non-identifier template values cannot be used as type or symbol names",
        "pass a bare symbol or use an explicit `ident(...)` splice");
    return name;
  }
  const char *dot = strrchr(name, '.');
  if (dot && dot[1]) {
    v = ct_find_bind(ctx, dot + 1);
    if (v && v->kind == CT_VALUE_IDENT) {
      size_t qualified_prefix_len = (size_t)(dot - name) + 1;
      const char *leaf = v->s ? v->s : "";
      size_t leaf_len = strlen(leaf);
      char *buf = arena_alloc(p->arena, qualified_prefix_len + leaf_len + 1);
      memcpy(buf, name, qualified_prefix_len);
      memcpy(buf + qualified_prefix_len, leaf, leaf_len + 1);
      return parser_intern(p, buf, qualified_prefix_len + leaf_len);
    }
    if (v) {
      parser_error(p, p->cur,
                   "non-identifier template values cannot be used as type or "
                   "symbol names",
                   "pass a bare symbol or use an explicit `ident(...)` splice");
      return name;
    }
  }
  return ct_substitute_name(p, name, ctx);
}

static const char *ct_qualify_module_decl_name(parser_t *p, const char *name,
                                               const char *module_name) {
  if (!name || !*name || !module_name || !*module_name || strchr(name, '.'))
    return name;
  size_t mlen = strlen(module_name);
  size_t nlen = strlen(name);
  char *buf = arena_alloc(p->arena, mlen + 1 + nlen + 1);
  memcpy(buf, module_name, mlen);
  buf[mlen] = '.';
  memcpy(buf + mlen + 1, name, nlen + 1);
  return parser_intern(p, buf, mlen + 1 + nlen);
}

static const char *ct_substitute_decl_name(parser_t *p, const char *name,
                                           ct_reflect_ctx_t *ctx) {
  if (name && ctx && ctx->template_module_name) {
    size_t prefix_len = strlen(ctx->template_module_name);
    if (strncmp(name, ctx->template_module_name, prefix_len) == 0 &&
        name[prefix_len] == '.')
      name += prefix_len + 1;
  }
  name = ct_substitute_symbol_name(p, name, ctx);
  return ct_qualify_module_decl_name(p, name, ctx ? ctx->module_name : NULL);
}

static expr_t *ct_layout_offset_expr(parser_t *p, token_t tok,
                                     const char *layout_name,
                                     const char *field_name,
                                     ct_reflect_ctx_t *ctx) {
  expr_t *callee = ct_mark_expr_emission(
      parse_table_ident_expr(p, tok, "__layout_offset"), ctx);
  expr_t *call = ct_mark_expr_emission(expr_new(p->arena, NY_E_CALL, tok), ctx);
  call->as.call.callee = callee;
  call_arg_t layout_arg = {
      .name = NULL,
      .val = ct_mark_expr_emission(ct_string_expr(p, tok, layout_name), ctx)};
  call_arg_t field_arg = {
      .name = NULL,
      .val = ct_mark_expr_emission(ct_string_expr(p, tok, field_name), ctx)};
  vec_push_arena(p->arena, &call->as.call.args, layout_arg);
  vec_push_arena(p->arena, &call->as.call.args, field_arg);
  return call;
}

static expr_t *ct_clone_expr(parser_t *p, expr_t *e, ct_reflect_ctx_t *ctx);
static expr_t *ct_clone_expr_impl(parser_t *p, expr_t *e,
                                  ct_reflect_ctx_t *ctx);
static stmt_t *ct_clone_stmt(parser_t *p, stmt_t *s, ct_reflect_ctx_t *ctx);

static token_t ct_emitted_token(token_t tok, ct_reflect_ctx_t *ctx) {
  if (ctx && ctx->emission_origin) {
    tok.module_id = ctx->emission_origin->module_id;
    tok.module_features = ctx->emission_origin->module_features;
  }
  return tok;
}

static expr_t *ct_mark_expr_emission(expr_t *expr, ct_reflect_ctx_t *ctx) {
  if (expr) {
    expr->tok = ct_emitted_token(expr->tok, ctx);
    if (ctx && ctx->emission_origin)
      expr->expansion_origin = ctx->emission_origin;
  }
  return expr;
}

static expr_t *ct_clone_expr(parser_t *p, expr_t *e, ct_reflect_ctx_t *ctx) {
  expr_t *out = ct_clone_expr_impl(p, e, ctx);
  return ct_mark_expr_emission(out, ctx);
}

static expr_t *ct_clone_pattern(parser_t *p, expr_t *pattern,
                                ct_reflect_ctx_t *ctx) {
  if (!ctx)
    return ct_clone_expr(p, pattern, NULL);
  ct_reflect_ctx_t pattern_ctx = *ctx;
  pattern_ctx.pattern_slot = true;
  return ct_clone_expr(p, pattern, &pattern_ctx);
}

/*
 * `${name}` uses the same syntax in scalar and sequence positions. Its AST
 * category is selected by the destination slot: an expression slot accepts
 * one expression, while a list/call/block slot can expand a list of code.
 */
static const ct_value_t *ct_placeholder_value(parser_t *p, expr_t *expr,
                                              ct_reflect_ctx_t *ctx) {
  if (!expr || !ctx)
    return NULL;
  expr_t *source = expr;
  const char *name = NULL;
  if (expr->kind == NY_E_SPLICE) {
    source = expr->as.splice.expr;
    if (source && source->kind == NY_E_IDENT)
      name = source->as.ident.name;
    else if (source && source->kind == NY_E_MEMBER &&
             source->as.member.target &&
             source->as.member.target->kind == NY_E_IDENT &&
             source->as.member.target->as.ident.name &&
             source->as.member.name) {
      const char *base = source->as.member.target->as.ident.name;
      const char *member = source->as.member.name;
      size_t base_len = strlen(base), member_len = strlen(member);
      char *qualified = arena_alloc(p->arena, base_len + member_len + 2);
      if (!qualified)
        return NULL;
      memcpy(qualified, base, base_len);
      qualified[base_len] = '.';
      memcpy(qualified + base_len + 1, member, member_len + 1);
      name = qualified;
    }
  } else if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    const char *raw = expr->as.ident.name;
    size_t len = strlen(raw);
    if (len > 3 && raw[0] == '$' && raw[1] == '{' && raw[len - 1] == '}') {
      char *key = arena_alloc(p->arena, len - 2);
      if (!key)
        return NULL;
      memcpy(key, raw + 2, len - 3);
      key[len - 3] = '\0';
      name = key;
    }
  }
  if (!name)
    return NULL;
  if (ct_ctx_has_template_bindings(ctx)) {
    const ct_value_t *bound = ct_find_bind(ctx, name);
    if (bound)
      return bound;
  }
  if (ctx->kind == CT_REFLECT_FIELDS && ctx->field &&
      strcmp(name, "field.name") == 0) {
    ctx->reflected_ident = (ct_value_t){
        .kind = CT_VALUE_IDENT,
        .s = ctx->field->name ? ctx->field->name : "",
        .source_tok = ctx->field->tok,
        .has_source_tok = true,
        .syntax_ctx = 0,
    };
    return &ctx->reflected_ident;
  }
  return NULL;
}

static expr_t *ct_single_expr_block(const ct_value_t *value) {
  if (!value || value->kind != CT_VALUE_AST_BLOCK || !value->block ||
      value->block->kind != NY_S_BLOCK || value->block->as.block.body.len != 1)
    return NULL;
  stmt_t *only = value->block->as.block.body.data[0];
  return only && only->kind == NY_S_EXPR ? only->as.expr.expr : NULL;
}

static void ct_append_expr_value(parser_t *p, ny_expr_list *dst,
                                 const ct_value_t *value, token_t tok,
                                 ct_reflect_ctx_t *ctx) {
  if (value->kind == CT_VALUE_AST_LIST) {
    for (size_t i = 0; i < value->items_len; i++) {
      const ct_value_t *item = &value->items[i];
      if (item->kind == CT_VALUE_IDENT) {
        expr_t *clone = ct_mark_expr_emission(
            ct_ident_splice_expr(p, tok, item, item->syntax_ctx, ctx), ctx);
        if (clone)
          vec_push_arena(p->arena, dst, clone);
        continue;
      }
      expr_t *source = item->kind == CT_VALUE_AST_EXPR
                           ? item->expr
                           : ct_single_expr_block(item);
      if (!source) {
        parser_error(p, tok,
                     "expression sequence splice requires expression code",
                     "use a quote containing one expression for every item");
        return;
      }
      expr_t *clone = ct_clone_expr(p, source, ctx);
      if (clone)
        vec_push_arena(p->arena, dst, clone);
    }
    return;
  }
  if (value->kind == CT_VALUE_AST_BLOCK) {
    expr_t *source = ct_single_expr_block(value);
    if (!source) {
      parser_error(p, tok,
                   "block code cannot be spliced into an expression list",
                   "use a quote containing one expression in this position");
      return;
    }
    expr_t *clone = ct_clone_expr(p, source, ctx);
    if (clone)
      vec_push_arena(p->arena, dst, clone);
    return;
  }
  expr_t *clone =
      value->kind == CT_VALUE_AST_EXPR && value->expr
          ? ct_clone_expr(p, value->expr, ctx)
      : value->kind == CT_VALUE_IDENT
          ? ct_ident_splice_expr(p, tok, value, value->syntax_ctx, ctx)
          : ct_mark_expr_emission(ct_value_expr(p, tok, value), ctx);
  if (clone)
    vec_push_arena(p->arena, dst, clone);
}

static bool ct_expand_expr_sequence_splice(parser_t *p, ny_expr_list *dst,
                                           expr_t *item,
                                           ct_reflect_ctx_t *ctx) {
  const ct_value_t *value = ct_placeholder_value(p, item, ctx);
  if (!value)
    return false;
  ct_append_expr_value(p, dst, value, item->tok, ctx);
  return true;
}

static void ct_append_stmt_value(parser_t *p, ny_stmt_list *dst,
                                 const ct_value_t *value, token_t tok,
                                 ct_reflect_ctx_t *ctx) {
  ct_reflect_ctx_t splice_ctx;
  if (ctx) {
    splice_ctx = *ctx;
    splice_ctx.template_module_name = NULL;
    ctx = &splice_ctx;
  }
  if (value->kind == CT_VALUE_AST_BLOCK && value->block) {
    stmt_t *cloned = ct_clone_stmt(p, value->block, ctx);
    if (cloned && cloned->kind == NY_S_BLOCK) {
      for (size_t i = 0; i < cloned->as.block.body.len; i++)
        vec_push_arena(p->arena, dst, cloned->as.block.body.data[i]);
    } else if (cloned) {
      vec_push_arena(p->arena, dst, cloned);
    }
    return;
  }
  stmt_t *expanded = stmt_new(p->arena, NY_S_EXPR, tok);
  expanded->tok = ct_emitted_token(expanded->tok, ctx);
  if (ctx && ctx->emission_origin)
    expanded->expansion_origin = ctx->emission_origin;
  if (value->kind == CT_VALUE_AST_EXPR && value->expr)
    expanded->as.expr.expr = ct_clone_expr(p, value->expr, ctx);
  else if (value->kind == CT_VALUE_IDENT)
    expanded->as.expr.expr = ct_mark_expr_emission(
        ct_ident_splice_expr(p, tok, value, value->syntax_ctx, ctx), ctx);
  else if (value->kind != CT_VALUE_AST_LIST)
    expanded->as.expr.expr =
        ct_mark_expr_emission(ct_value_expr(p, tok, value), ctx);
  else
    parser_error(p, tok, "nested code lists cannot occupy a block slot",
                 "pass a flat list of quoted statements");
  if (expanded->as.expr.expr)
    vec_push_arena(p->arena, dst, expanded);
}

static bool ct_expand_deferred_reflection(parser_t *p, ny_stmt_list *dst,
                                          stmt_t *stmt,
                                          ct_reflect_ctx_t *template_ctx) {
  if (!stmt || stmt->kind != NY_S_EXPR || !stmt->as.expr.expr ||
      stmt->as.expr.expr->kind != NY_E_CALL || !template_ctx)
    return false;
  expr_t *call = stmt->as.expr.expr;
  if (!call->as.call.deferred_comptime_reflection)
    return false;
  expr_t *callee = call->as.call.callee;
  if (!callee || callee->kind != NY_E_IDENT || !callee->as.ident.name)
    return false;
  bool is_fields =
      strcmp(callee->as.ident.name, "__ny_deferred_comptime_fields") == 0;
  bool is_exports =
      strcmp(callee->as.ident.name, "__ny_deferred_comptime_exports") == 0;
  bool is_variants =
      strcmp(callee->as.ident.name, "__ny_deferred_comptime_variants") == 0;
  bool is_attributes =
      strcmp(callee->as.ident.name, "__ny_deferred_comptime_attributes") == 0;
  if (!is_fields && !is_exports && !is_variants && !is_attributes)
    return false;
  if (call->as.call.args.len < 2 || !call->as.call.args.data[0].val ||
      call->as.call.args.data[0].val->kind != NY_E_IDENT ||
      !call->as.call.args.data[0].val->as.ident.name ||
      !call->as.call.args.data[1].val ||
      call->as.call.args.data[1].val->kind != NY_E_IDENT ||
      !call->as.call.args.data[1].val->as.ident.name) {
    parser_error(p, stmt->tok, "malformed deferred comptime reflection",
                 "write a reflection query inside a comptime template");
    return true;
  }
  const char *target = ct_substitute_symbol_name(
      p, call->as.call.args.data[0].val->as.ident.name, template_ctx);
  const char *var = call->as.call.args.data[1].val->as.ident.name;
  if (is_fields) {
    parser_ct_layout_meta *layout = parser_find_layout_meta(p, target);
    if (!layout) {
      parser_error(p, stmt->tok,
                   "unknown layout in deferred comptime fields query",
                   "declare the layout before emitting the codegen template");
      return true;
    }
    for (size_t i = 0; i < layout->fields.len; ++i) {
      ct_reflect_ctx_t field_ctx = {
          .kind = CT_REFLECT_FIELDS,
          .var = var,
          .layout_name = layout->name,
          .module_name = template_ctx->module_name,
          .template_module_name = template_ctx->template_module_name,
          .emission_origin = template_ctx->emission_origin,
          .field = &layout->fields.data[i],
          .field_index = (int)i,
          .binds = template_ctx->binds,
      };
      for (size_t j = 2; j < call->as.call.args.len; ++j) {
        expr_t *quoted = call->as.call.args.data[j].val;
        if (!quoted || quoted->kind != NY_E_QUOTE || !quoted->as.quote.body) {
          parser_error(p, stmt->tok, "malformed deferred comptime fields body",
                       "keep each reflection item inside its emit block");
          continue;
        }
        stmt_list_push_flat(
            p, dst, ct_clone_stmt(p, quoted->as.quote.body, &field_ctx));
      }
    }
  } else if (is_exports) {
    parser_ct_module_meta *module = parser_find_module_meta(p, target);
    if (!module) {
      parser_error(p, stmt->tok,
                   "unknown module in deferred comptime exports query",
                   "declare the module before emitting the codegen template");
      return true;
    }
    for (size_t i = 0; i < module->exports.len; ++i) {
      ct_reflect_ctx_t export_ctx = {
          .kind = CT_REFLECT_EXPORTS,
          .var = var,
          .module_name = template_ctx->module_name,
          .template_module_name = template_ctx->template_module_name,
          .emission_origin = template_ctx->emission_origin,
          .export_name = module->exports.data[i],
          .binds = template_ctx->binds,
      };
      for (size_t j = 2; j < call->as.call.args.len; ++j) {
        expr_t *quoted = call->as.call.args.data[j].val;
        if (!quoted || quoted->kind != NY_E_QUOTE || !quoted->as.quote.body) {
          parser_error(p, stmt->tok, "malformed deferred comptime exports body",
                       "keep each reflection item inside its emit block");
          continue;
        }
        stmt_list_push_flat(
            p, dst, ct_clone_stmt(p, quoted->as.quote.body, &export_ctx));
      }
    }
  } else if (is_variants) {
    bool ambiguous = false;
    stmt_t *decl = parser_find_ct_decl(p, target, &ambiguous);
    if (!decl || decl->kind != NY_S_ENUM) {
      parser_error(p, stmt->tok,
                   ambiguous ? "ambiguous enum in deferred variants query"
                             : "unknown enum in deferred variants query",
                   "declare or qualify the enum before emitting this template");
      return true;
    }
    for (size_t i = 0; i < decl->as.enu.items.len; ++i) {
      ct_reflect_ctx_t variant_ctx = {
          .kind = CT_REFLECT_VARIANTS,
          .var = var,
          .module_name = template_ctx->module_name,
          .template_module_name = template_ctx->template_module_name,
          .emission_origin = template_ctx->emission_origin,
          .enum_name = decl->as.enu.name,
          .variant = &decl->as.enu.items.data[i],
          .variant_index = (int)i,
          .binds = template_ctx->binds,
      };
      for (size_t j = 2; j < call->as.call.args.len; ++j) {
        expr_t *quoted = call->as.call.args.data[j].val;
        if (!quoted || quoted->kind != NY_E_QUOTE || !quoted->as.quote.body) {
          parser_error(p, stmt->tok,
                       "malformed deferred comptime variants body",
                       "keep each reflection item inside its emit block");
          continue;
        }
        stmt_list_push_flat(
            p, dst, ct_clone_stmt(p, quoted->as.quote.body, &variant_ctx));
      }
    }
  } else {
    bool ambiguous = false;
    stmt_t *decl = parser_find_ct_decl(p, target, &ambiguous);
    if (!decl) {
      parser_error(
          p, stmt->tok,
          ambiguous ? "ambiguous declaration in deferred attributes query"
                    : "unknown declaration in deferred attributes query",
          "declare or qualify the target before emitting this template");
      return true;
    }
    for (size_t i = 0; i < decl->attributes.len; ++i) {
      ct_reflect_ctx_t attr_ctx = {
          .kind = CT_REFLECT_ATTRIBUTES,
          .var = var,
          .module_name = template_ctx->module_name,
          .template_module_name = template_ctx->template_module_name,
          .emission_origin = template_ctx->emission_origin,
          .attribute = &decl->attributes.data[i],
          .binds = template_ctx->binds,
      };
      for (size_t j = 2; j < call->as.call.args.len; ++j) {
        expr_t *quoted = call->as.call.args.data[j].val;
        if (!quoted || quoted->kind != NY_E_QUOTE || !quoted->as.quote.body) {
          parser_error(p, stmt->tok,
                       "malformed deferred comptime attributes body",
                       "keep each reflection item inside its emit block");
          continue;
        }
        stmt_list_push_flat(p, dst,
                            ct_clone_stmt(p, quoted->as.quote.body, &attr_ctx));
      }
    }
  }
  return true;
}

static bool ct_expand_stmt_placeholder(parser_t *p, ny_stmt_list *dst,
                                       stmt_t *stmt, ct_reflect_ctx_t *ctx) {
  if (ct_expand_deferred_reflection(p, dst, stmt, ctx))
    return true;
  if (!stmt || stmt->kind != NY_S_EXPR)
    return false;
  const ct_value_t *value = ct_placeholder_value(p, stmt->as.expr.expr, ctx);
  if (!value)
    return false;
  if (value->kind == CT_VALUE_AST_LIST) {
    for (size_t i = 0; i < value->items_len; i++)
      ct_append_stmt_value(p, dst, &value->items[i], stmt->tok, ctx);
  } else {
    ct_append_stmt_value(p, dst, value, stmt->tok, ctx);
  }
  return true;
}

static void ct_clone_call_args(parser_t *p, ny_call_arg_list *dst,
                               ny_call_arg_list *src, ct_reflect_ctx_t *ctx) {
  for (size_t i = 0; src && i < src->len; i++) {
    expr_t *value = src->data[i].val;
    const ct_value_t *binding = ct_placeholder_value(p, value, ctx);
    if (binding && binding->kind == CT_VALUE_AST_LIST) {
      if (src->data[i].name) {
        parser_error(p, value->tok,
                     "a code list cannot expand into a named call argument",
                     "use a positional sequence splice");
        continue;
      }
      ny_expr_list expanded = {0};
      ct_append_expr_value(p, &expanded, binding, value->tok, ctx);
      for (size_t j = 0; j < expanded.len; j++)
        vec_push_arena(p->arena, dst,
                       ((call_arg_t){.name = NULL, .val = expanded.data[j]}));
      continue;
    }
    call_arg_t arg = {
        .name = ct_substitute_name(p, src->data[i].name, ctx),
        .val = ct_clone_expr(p, value, ctx),
    };
    vec_push_arena(p->arena, dst, arg);
  }
}

static expr_t *ct_clone_expr_impl(parser_t *p, expr_t *e,
                                  ct_reflect_ctx_t *ctx) {
  if (!e)
    return NULL;
  const ct_value_t *placeholder = ct_placeholder_value(p, e, ctx);
  if (placeholder) {
    /*
     * Keep an explicit splice node around the inserted syntax.  Its wrapper
     * tells name resolution that the inner identifier's syntax context is
     * authoritative, including the caller's context 0; flattening it to a
     * bare identifier lets nearest-name fallback capture a same-spelled
     * binding introduced by the surrounding quote.
     */
    if (placeholder->kind == CT_VALUE_IDENT) {
      /*
       * The identifier value owns its lexical scope. The placeholder's
       * syntax context belongs to the template definition and must not
       * capture a caller identifier in a same-named introduced local.
       */
      return ct_ident_splice_expr(p, e->tok, placeholder,
                                  placeholder->syntax_ctx, ctx);
    }
    if (e->kind == NY_E_SPLICE && (placeholder->kind == CT_VALUE_AST_EXPR ||
                                   placeholder->kind == CT_VALUE_AST_BLOCK)) {
      expr_t *source = placeholder->kind == CT_VALUE_AST_EXPR
                           ? placeholder->expr
                       : placeholder->kind == CT_VALUE_AST_BLOCK
                           ? ct_single_expr_block(placeholder)
                           : ct_value_expr(p, e->tok, placeholder);
      if (source) {
        expr_t *splice = expr_new(p->arena, NY_E_SPLICE, e->tok);
        splice->as.splice.expr = ct_clone_expr(p, source, ctx);
        splice->as.splice.syntax_ctx = e->as.splice.syntax_ctx;
        return splice;
      }
    }
    if (placeholder->kind == CT_VALUE_AST_LIST) {
      parser_error(p, e->tok, "a code list must occupy a syntax sequence slot",
                   "use it as a complete item in a list, call, or block");
      return NULL;
    }
    if (placeholder->kind == CT_VALUE_AST_BLOCK) {
      expr_t *source = ct_single_expr_block(placeholder);
      if (source)
        return ct_clone_expr(p, source, ctx);
      parser_error(p, e->tok, "block code cannot be spliced into an expression",
                   "use a quote containing one expression in this position");
      return NULL;
    }
    if (placeholder->kind == CT_VALUE_AST_EXPR && placeholder->expr)
      return ct_clone_expr(p, placeholder->expr, ctx);
    return ct_value_expr(p, e->tok, placeholder);
  }
  if (e->kind == NY_E_MEMBER && e->as.member.computed_name && ctx &&
      ctx->kind == CT_REFLECT_FIELDS) {
    expr_t *descriptor = e->as.member.computed_name;
    if (ctx->field && ctx->field->name && descriptor->kind == NY_E_IDENT &&
        descriptor->as.ident.name && ctx->var && ctx->layout_name &&
        strcmp(descriptor->as.ident.name, ctx->var) == 0) {
      expr_t *call = expr_new(p->arena, NY_E_CALL, e->tok);
      call->as.call.callee = ct_mark_expr_emission(
          ct_make_ident_expr(p, e->tok, "load_layout"), ctx);
      call->as.call.callee->as.ident.syntax_ctx =
          descriptor->as.ident.syntax_ctx;
      call_arg_t target_arg = {.val =
                                   ct_clone_expr(p, e->as.member.target, ctx)};
      call_arg_t layout_arg = {
          .val = ct_mark_expr_emission(
              ct_string_expr(p, e->tok, ctx->layout_name), ctx)};
      call_arg_t field_arg = {
          .val = ct_mark_expr_emission(
              ct_string_expr(p, e->tok, ctx->field->name), ctx)};
      vec_push_arena(p->arena, &call->as.call.args, target_arg);
      vec_push_arena(p->arena, &call->as.call.args, layout_arg);
      vec_push_arena(p->arena, &call->as.call.args, field_arg);
      return call;
    }
    parser_error(
        p, e->tok,
        "member splice requires the active compile-time field descriptor",
        "use `value.(field)` inside `#fields(Layout) as field`");
  }
  if (e->kind == NY_E_MEMBER && ctx && ctx->kind == CT_REFLECT_VARIANTS &&
      ctx->variant && e->as.member.target &&
      e->as.member.target->kind == NY_E_IDENT &&
      e->as.member.target->as.ident.name && e->as.member.name) {
    const char *member = e->as.member.name;
    if (strcmp(e->as.member.target->as.ident.name, ctx->var) == 0) {
      if (strcmp(member, "name") == 0)
        return ct_string_expr(p, e->tok, ctx->variant->name);
      if (strcmp(member, "source_file") == 0)
        return ct_string_expr(
            p, e->tok,
            ctx->variant->tok.filename ? ctx->variant->tok.filename : "");
      if (strcmp(member, "source_line") == 0)
        return ct_int_expr(p, e->tok, ctx->variant->tok.line);
      if (strcmp(member, "source_column") == 0)
        return ct_int_expr(p, e->tok, ctx->variant->tok.col);
      if (strcmp(member, "index") == 0)
        return ct_int_expr(p, e->tok, ctx->variant_index);
      if (strcmp(member, "has_payload") == 0)
        return ct_bool_expr(p, e->tok, ctx->variant->fields.len != 0);
      if (strcmp(member, "payload_count") == 0)
        return ct_int_expr(p, e->tok, (int64_t)ctx->variant->fields.len);
      if (strcmp(member, "payload_fields") == 0)
        return ct_variant_payload_fields_expr(p, e->tok, ctx->variant, ctx);
      if (strcmp(member, "payload_fields_with_origins") == 0)
        return ct_variant_payload_origins_expr(p, e->tok, ctx->variant, ctx);
      if (strcmp(member, "pattern") == 0) {
        if (!ctx->pattern_slot) {
          parser_error(p, e->tok,
                       "enum variant pattern requires a match pattern slot",
                       "use variant.pattern before '->' in a match arm");
          return NULL;
        }
        expr_t *owner = ct_clone_expr(
            p, parse_table_ident_expr(p, e->tok, ctx->enum_name), ctx);
        if (ctx->variant->fields.len == 0) {
          expr_t *pattern = expr_new(p->arena, NY_E_MEMBER, e->tok);
          pattern->as.member.target = owner;
          pattern->as.member.name = ctx->variant->name;
          return pattern;
        }
        expr_t *pattern = expr_new(p->arena, NY_E_MEMCALL, e->tok);
        pattern->as.memcall.target = owner;
        pattern->as.memcall.name = ctx->variant->name;
        for (size_t i = 0; i < ctx->variant->fields.len; ++i) {
          call_arg_t wildcard = {
              .val = ct_clone_expr(p, parse_table_ident_expr(p, e->tok, "_"),
                                   ctx)};
          vec_push_arena(p->arena, &pattern->as.memcall.args, wildcard);
        }
        return pattern;
      }
      parser_error(p, e->tok, "unknown compile-time enum variant property",
                   "use variant.name, variant.source_file, "
                   "variant.source_line, variant.source_column, "
                   "variant.index, variant.has_payload, "
                   "variant.payload_count, variant.payload_fields, or "
                   "variant.payload_fields_with_origins; "
                   "variant.pattern is available in "
                   "match patterns");
      return NULL;
    }
    const char *target_name =
        ct_substitute_symbol_name(p, e->as.member.target->as.ident.name, ctx);
    if (ctx->enum_name && ctx->var && ctx->variant->name && target_name &&
        strcmp(target_name, ctx->enum_name) == 0 &&
        strcmp(member, ctx->var) == 0) {
      expr_t *member_expr = expr_new(p->arena, NY_E_MEMBER, e->tok);
      member_expr->as.member.target =
          ct_clone_expr(p, e->as.member.target, ctx);
      member_expr->as.member.name = ctx->variant->name;
      member_expr->as.member.computed_name =
          ct_clone_expr(p, e->as.member.computed_name, ctx);
      return member_expr;
    }
  }
  if (e->kind == NY_E_IDENT && ctx && e->as.ident.name) {
    const ct_value_t *v = ct_find_bind(ctx, e->as.ident.name);
    if (v && v->kind == CT_VALUE_AST_LIST) {
      parser_error(p, e->tok, "a code list must occupy a syntax sequence slot",
                   "write `${parameter}` as a complete sequence item");
      return NULL;
    }
    if (v)
      return ct_value_expr(p, e->tok, v);
  }
  if (e->kind == NY_E_IDENT && ctx && ctx->kind == CT_REFLECT_EXPORTS &&
      e->as.ident.name && strcmp(e->as.ident.name, ctx->var) == 0) {
    return ct_string_expr(p, e->tok, ctx->export_name);
  }
  if (e->kind == NY_E_IDENT && ctx && ctx->kind == CT_REFLECT_FIELDS &&
      e->as.ident.name && strcmp(e->as.ident.name, ctx->var) == 0) {
    return ct_string_expr(p, e->tok, ctx->field ? ctx->field->name : "");
  }
  if (e->kind == NY_E_IDENT && ctx && ctx->kind == CT_REFLECT_ATTRIBUTES &&
      e->as.ident.name && strcmp(e->as.ident.name, ctx->var) == 0) {
    return ct_string_expr(
        p, e->tok,
        ctx->attribute && ctx->attribute->name ? ctx->attribute->name : "");
  }
  if (e->kind == NY_E_MEMBER && ctx && ctx->kind == CT_REFLECT_FIELDS &&
      ny_expr_ident_is_name(e->as.member.target, ctx->var)) {
    const char *member = e->as.member.name;
    if (strcmp(member, "name") == 0)
      return ct_string_expr(p, e->tok, ctx->field ? ctx->field->name : "");
    if (strcmp(member, "source_file") == 0)
      return ct_string_expr(p, e->tok,
                            ctx->field && ctx->field->tok.filename
                                ? ctx->field->tok.filename
                                : "");
    if (strcmp(member, "source_line") == 0)
      return ct_int_expr(p, e->tok, ctx->field ? ctx->field->tok.line : 0);
    if (strcmp(member, "source_column") == 0)
      return ct_int_expr(p, e->tok, ctx->field ? ctx->field->tok.col : 0);
    if (strcmp(member, "type") == 0 || strcmp(member, "type_name") == 0)
      return ct_string_expr(p, e->tok, ctx->field ? ctx->field->type_name : "");
    if (strcmp(member, "offset") == 0)
      return ct_layout_offset_expr(p, e->tok, ctx->layout_name,
                                   ctx->field ? ctx->field->name : "", ctx);
    if (strcmp(member, "index") == 0)
      return ct_int_expr(p, e->tok, ctx->field_index);
    if (strcmp(member, "is_array") == 0)
      return ct_bool_expr(p, e->tok, ctx->field && ctx->field->is_array);
    if (strcmp(member, "has_default") == 0)
      return ct_bool_expr(p, e->tok,
                          ctx->field && ctx->field->default_value != NULL);
    if (strcmp(member, "default_src") == 0)
      return ct_string_expr(
          p, e->tok,
          ctx->field && ctx->field->default_src ? ctx->field->default_src : "");
    if (strcmp(member, "default_expr") == 0) {
      if (!ctx->field || !ctx->field->default_value) {
        expr_t *nil = ct_int_expr(p, e->tok, 0);
        nil->tok.kind = NY_T_NIL;
        return nil;
      }
      return ct_clone_expr(p, ctx->field->default_value, ctx);
    }
    if (strcmp(member, "default_value") == 0) {
      const layout_field_t *field = ctx->field;
      if (field && field->default_value)
        return ct_clone_expr(p, field->default_value, ctx);
      const char *type = field ? field->type_name : NULL;
      while (type && (*type == '*' || *type == '?'))
        type++;
      if (type && strcmp(type, "bool") == 0)
        return ct_bool_expr(p, e->tok, false);
      if (type && strcmp(type, "str") == 0)
        return ct_string_expr(p, e->tok, "");
      if (type && (strcmp(type, "f32") == 0 || strcmp(type, "f64") == 0 ||
                   strcmp(type, "f128") == 0))
        return ct_float_expr(p, e->tok, 0.0);
      return ct_int_expr(p, e->tok, 0);
    }
    if (strcmp(member, "align") == 0)
      return ct_int_expr(p, e->tok, ctx->field ? ctx->field->width : 0);
    if (strcmp(member, "array_len") == 0) {
      expr_t *len = ctx->field ? ctx->field->array_len : NULL;
      if (len && len->kind == NY_E_LITERAL &&
          len->as.literal.kind == NY_LIT_INT)
        return ct_int_expr(p, e->tok, len->as.literal.as.i);
      return ct_int_expr(p, e->tok, 0);
    }
    if (strcmp(member, "array_len_known") == 0) {
      expr_t *len = ctx->field ? ctx->field->array_len : NULL;
      return ct_bool_expr(p, e->tok,
                          len && len->kind == NY_E_LITERAL &&
                              len->as.literal.kind == NY_LIT_INT);
    }
    parser_error(
        p, e->tok, "unknown comptime field reflection property",
        "available properties are name, source_file, source_line, "
        "source_column, type, type_name, offset, index, "
        "is_array, array_len, array_len_known, has_default, default_src, "
        "default_expr, default_value, and align");
    return ct_string_expr(p, e->tok, "");
  }

  expr_t *out = expr_new(p->arena, e->kind, e->tok);
  *out = *e;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_STR) {
      out->as.literal.as.s.data = ct_substitute_string(
          p, e->as.literal.as.s.data, e->as.literal.as.s.len,
          &out->as.literal.as.s.len, ctx);
    }
    break;
  case NY_E_IDENT:
    out->as.ident.syntax_ctx = e->as.ident.syntax_ctx;
    out->as.ident.name = ct_substitute_name(p, e->as.ident.name, ctx);
    out->as.ident.hash =
        out->as.ident.name
            ? ny_hash64(out->as.ident.name, strlen(out->as.ident.name))
            : 0;
    out->as.ident.sym_id =
        out->as.ident.name
            ? ny_intern_str(out->as.ident.name, strlen(out->as.ident.name))
            : 0;
    break;
  case NY_E_UNARY:
    out->as.unary.right = ct_clone_expr(p, e->as.unary.right, ctx);
    break;
  case NY_E_BINARY:
    out->as.binary.left = ct_clone_expr(p, e->as.binary.left, ctx);
    out->as.binary.right = ct_clone_expr(p, e->as.binary.right, ctx);
    break;
  case NY_E_LOGICAL:
    out->as.logical.left = ct_clone_expr(p, e->as.logical.left, ctx);
    out->as.logical.right = ct_clone_expr(p, e->as.logical.right, ctx);
    break;
  case NY_E_TERNARY:
    out->as.ternary.cond = ct_clone_expr(p, e->as.ternary.cond, ctx);
    out->as.ternary.true_expr = ct_clone_expr(p, e->as.ternary.true_expr, ctx);
    out->as.ternary.false_expr =
        ct_clone_expr(p, e->as.ternary.false_expr, ctx);
    break;
  case NY_E_CALL:
    out->as.call.args = (ny_call_arg_list){0};
    out->as.call.deferred_comptime_reflection =
        e->as.call.deferred_comptime_reflection;
    out->as.call.callee = ct_clone_expr(p, e->as.call.callee, ctx);
    ct_clone_call_args(p, &out->as.call.args, &e->as.call.args, ctx);
    break;
  case NY_E_MEMCALL:
    out->as.memcall.args = (ny_call_arg_list){0};
    out->as.memcall.name = ct_substitute_name(p, e->as.memcall.name, ctx);
    out->as.memcall.target = ct_clone_expr(p, e->as.memcall.target, ctx);
    ct_clone_call_args(p, &out->as.memcall.args, &e->as.memcall.args, ctx);
    break;
  case NY_E_INDEX:
    out->as.index.target = ct_clone_expr(p, e->as.index.target, ctx);
    out->as.index.start = ct_clone_expr(p, e->as.index.start, ctx);
    out->as.index.stop = ct_clone_expr(p, e->as.index.stop, ctx);
    out->as.index.step = ct_clone_expr(p, e->as.index.step, ctx);
    break;
  case NY_E_MEMBER:
    out->as.member.name = ct_substitute_name(p, e->as.member.name, ctx);
    out->as.member.target = ct_clone_expr(p, e->as.member.target, ctx);
    out->as.member.computed_name =
        ct_clone_expr(p, e->as.member.computed_name, ctx);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    out->as.lambda.return_type =
        ct_substitute_symbol_name(p, e->as.lambda.return_type, ctx);
    out->as.lambda.params = (ny_param_list){0};
    for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
      param_t param = e->as.lambda.params.data[i];
      param.name = ct_substitute_name(p, param.name, ctx);
      param.type = ct_substitute_symbol_name(p, param.type, ctx);
      param.def = ct_clone_expr(p, param.def, ctx);
      param.tok = ct_emitted_token(param.tok, ctx);
      if (ctx && ctx->emission_origin)
        param.expansion_origin = ctx->emission_origin;
      vec_push_arena(p->arena, &out->as.lambda.params, param);
    }
    out->as.lambda.body = ct_clone_stmt(p, e->as.lambda.body, ctx);
    out->as.lambda.is_variadic = e->as.lambda.is_variadic;
    break;
  case NY_E_PTR_TYPE:
    out->as.ptr_type.target = ct_clone_expr(p, e->as.ptr_type.target, ctx);
    break;
  case NY_E_DEREF:
    out->as.deref.target = ct_clone_expr(p, e->as.deref.target, ctx);
    break;
  case NY_E_SIZEOF:
    out->as.szof.target = ct_clone_expr(p, e->as.szof.target, ctx);
    out->as.szof.type_name =
        ct_substitute_symbol_name(p, e->as.szof.type_name, ctx);
    out->as.szof.resolved_type_name =
        ct_substitute_symbol_name(p, e->as.szof.resolved_type_name, ctx);
    out->as.szof.is_type = e->as.szof.is_type;
    break;
  case NY_E_TRY:
    out->as.try_expr.target = ct_clone_expr(p, e->as.try_expr.target, ctx);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    out->as.list_like = (ny_expr_list){0};
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      expr_t *item = e->as.list_like.data[i];
      if (ct_expand_expr_sequence_splice(p, &out->as.list_like, item, ctx)) {
        continue;
      }
      vec_push_arena(p->arena, &out->as.list_like, ct_clone_expr(p, item, ctx));
    }
    break;
  case NY_E_DICT:
    memset(&out->as.dict.pairs, 0, sizeof(out->as.dict.pairs));
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      dict_pair_t pair = {
          .key = ct_clone_expr(p, e->as.dict.pairs.data[i].key, ctx),
          .value = ct_clone_expr(p, e->as.dict.pairs.data[i].value, ctx),
      };
      vec_push_arena(p->arena, &out->as.dict.pairs, pair);
    }
    break;
  case NY_E_COMPTIME:
    out->as.comptime_expr.body =
        ct_clone_stmt(p, e->as.comptime_expr.body, ctx);
    out->as.comptime_expr.origin = e->as.comptime_expr.origin;
    break;
  case NY_E_MATCH:
    out->as.match.test = ct_clone_expr(p, e->as.match.test, ctx);
    out->as.match.arms = (ny_match_arm_list){0};
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t arm = {0};
      for (size_t j = 0; j < e->as.match.arms.data[i].patterns.len; j++) {
        vec_push_arena(p->arena, &arm.patterns,
                       ct_clone_pattern(
                           p, e->as.match.arms.data[i].patterns.data[j], ctx));
      }
      arm.guard = ct_clone_expr(p, e->as.match.arms.data[i].guard, ctx);
      arm.conseq = ct_clone_stmt(p, e->as.match.arms.data[i].conseq, ctx);
      vec_push_arena(p->arena, &out->as.match.arms, arm);
    }
    out->as.match.default_conseq =
        ct_clone_stmt(p, e->as.match.default_conseq, ctx);
    break;
  case NY_E_QUOTE:
    out->as.quote.syntax_ctx = e->as.quote.syntax_ctx;
    if (e->as.quote.body)
      out->as.quote.body = ct_clone_stmt(p, e->as.quote.body, ctx);
    if (e->as.quote.expr)
      out->as.quote.expr = ct_clone_expr(p, e->as.quote.expr, ctx);
    break;
  case NY_E_SPLICE:
    out->as.splice.expr = ct_clone_expr(p, e->as.splice.expr, ctx);
    out->as.splice.syntax_ctx = e->as.splice.syntax_ctx;
    break;
  case NY_E_EXISTS:
    out->as.exists.var = ct_substitute_name(p, e->as.exists.var, ctx);
    out->as.exists.body = ct_clone_expr(p, e->as.exists.body, ctx);
    break;
  case NY_E_ASM:
    out->as.as_asm.args = (ny_expr_list){0};
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      vec_push_arena(p->arena, &out->as.as_asm.args,
                     ct_clone_expr(p, e->as.as_asm.args.data[i], ctx));
    break;
  case NY_E_FSTRING:
    out->as.fstring.parts = (ny_fstring_part_list){0};
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t part = e->as.fstring.parts.data[i];
      if (part.kind == NY_FSP_EXPR)
        part.as.e = ct_clone_expr(p, part.as.e, ctx);
      vec_push_arena(p->arena, &out->as.fstring.parts, part);
    }
    break;
  case NY_E_INFERRED_MEMBER:
    out->as.inferred_member.name =
        ct_substitute_name(p, e->as.inferred_member.name, ctx);
    break;
  case NY_E_EMBED: {
    size_t path_len = e->as.embed.path ? strlen(e->as.embed.path) : 0;
    out->as.embed.path =
        ct_substitute_string(p, e->as.embed.path, path_len, &path_len, ctx);
    break;
  }
  default:
    break;
  }
  return out;
}

static expr_t *ct_layout_default_expr(parser_t *p, const layout_field_t *field,
                                      ct_reflect_ctx_t *ctx, token_t tok) {
  if (field && field->default_value)
    return ct_clone_expr(p, field->default_value, ctx);
  const char *type = field ? field->type_name : NULL;
  while (type && (*type == '*' || *type == '?'))
    type++;
  if (type && strcmp(type, "bool") == 0)
    return ct_bool_expr(p, tok, false);
  if (type && strcmp(type, "str") == 0)
    return ct_string_expr(p, tok, "");
  if (type && (strcmp(type, "f32") == 0 || strcmp(type, "f64") == 0 ||
               strcmp(type, "f128") == 0)) {
    expr_t *value = ct_float_expr(p, tok, 0.0);
    value->as.literal.hint = NY_LIT_HINT_F64;
    return value;
  }
  return ct_int_expr(p, tok, 0);
}

static ny_param_list ct_clone_params(parser_t *p, const ny_param_list *params,
                                     ct_reflect_ctx_t *ctx) {
  ny_param_list result = {0};
  for (size_t i = 0; i < params->len; ++i) {
    param_t parameter = params->data[i];
    if (parameter.comptime_fields_layout) {
      parser_ct_layout_meta *layout = parser_find_layout_meta(
          p,
          ct_substitute_symbol_name(p, parameter.comptime_fields_layout, ctx));
      if (!layout) {
        parser_error(p, parameter.tok,
                     "unknown layout in parameter field splice",
                     "declare the layout before emitting this template");
        continue;
      }
      for (size_t field_index = 0; field_index < layout->fields.len;
           ++field_index) {
        const layout_field_t *field = &layout->fields.data[field_index];
        param_t expanded = {
            .name = field->name,
            .type = field->type_name,
            .def = parameter.comptime_fields_required
                       ? NULL
                       : ct_layout_default_expr(p, field, ctx, field->tok),
            .tok = ct_emitted_token(field->tok, ctx),
            .expansion_origin = ctx && ctx->emission_origin
                                    ? ctx->emission_origin
                                    : field->expansion_origin,
            .syntax_ctx = parameter.syntax_ctx,
        };
        vec_push_arena(p->arena, &result, expanded);
      }
      continue;
    }
    parameter.name = ct_substitute_name(p, parameter.name, ctx);
    parameter.type = ct_substitute_symbol_name(p, parameter.type, ctx);
    parameter.def = ct_clone_expr(p, parameter.def, ctx);
    parameter.tok = ct_emitted_token(parameter.tok, ctx);
    if (ctx && ctx->emission_origin)
      parameter.expansion_origin = ctx->emission_origin;
    vec_push_arena(p->arena, &result, parameter);
  }
  return result;
}

static stmt_t *ct_clone_stmt(parser_t *p, stmt_t *s, ct_reflect_ctx_t *ctx) {
  if (!s)
    return NULL;
  stmt_t *out = stmt_new(p->arena, s->kind, s->tok);
  *out = *s;
  /*
   * Template-local declarations need a distinct mark so an explicit code
   * identifier splice can keep resolving in the caller's scope.
   */
  if (ctx && ctx->kind == CT_REFLECT_TEMPLATE && s->kind == NY_S_VAR &&
      s->syntax_ctx == 0 && p->next_syntax_ctx != 0)
    out->syntax_ctx = p->next_syntax_ctx++;
  out->tok = ct_emitted_token(out->tok, ctx);
  if (ctx && ctx->emission_origin)
    out->expansion_origin = ctx->emission_origin;
  out->attributes = (ny_attribute_list){0};
  for (size_t i = 0; i < s->attributes.len; ++i) {
    attribute_t attribute = s->attributes.data[i];
    attribute.tok = ct_emitted_token(attribute.tok, ctx);
    attribute.args = (ny_expr_list){0};
    for (size_t j = 0; j < s->attributes.data[i].args.len; ++j)
      vec_push_arena(p->arena, &attribute.args,
                     ct_clone_expr(p, s->attributes.data[i].args.data[j], ctx));
    vec_push_arena(p->arena, &out->attributes, attribute);
  }
  switch (s->kind) {
  case NY_S_BLOCK:
    out->as.block.body = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      stmt_t *stmt_item = s->as.block.body.data[i];
      if (ct_expand_stmt_placeholder(p, &out->as.block.body, stmt_item, ctx))
        continue;
      vec_push_arena(p->arena, &out->as.block.body,
                     ct_clone_stmt(p, s->as.block.body.data[i], ctx));
    }
    break;
  case NY_S_EXPR:
    out->as.expr.expr = ct_clone_expr(p, s->as.expr.expr, ctx);
    break;
  case NY_S_RETURN:
    out->as.ret.value = ct_clone_expr(p, s->as.ret.value, ctx);
    break;
  case NY_S_USE:
    out->as.use.module = ct_substitute_symbol_name(p, s->as.use.module, ctx);
    out->as.use.alias = ct_substitute_name(p, s->as.use.alias, ctx);
    out->as.use.profile = ct_substitute_name(p, s->as.use.profile, ctx);
    out->as.use.imports = (ny_use_item_list){0};
    for (size_t i = 0; i < s->as.use.imports.len; ++i) {
      use_item_t item = {
          .tok = ct_emitted_token(s->as.use.imports.data[i].tok, ctx),
          .name =
              ct_substitute_symbol_name(p, s->as.use.imports.data[i].name, ctx),
          .alias_tok =
              ct_emitted_token(s->as.use.imports.data[i].alias_tok, ctx),
          .alias = ct_substitute_name(p, s->as.use.imports.data[i].alias, ctx),
          .expansion_origin = ctx && ctx->emission_origin
                                  ? ctx->emission_origin
                                  : s->as.use.imports.data[i].expansion_origin,
          .alias_expansion_origin =
              ctx && ctx->emission_origin
                  ? ctx->emission_origin
                  : s->as.use.imports.data[i].alias_expansion_origin,
      };
      vec_push_arena(p->arena, &out->as.use.imports, item);
    }
    break;
  case NY_S_LINK: {
    size_t lib_len = s->as.link.lib ? strlen(s->as.link.lib) : 0;
    out->as.link.lib =
        ct_substitute_string(p, s->as.link.lib, lib_len, &lib_len, ctx);
  } break;
  case NY_S_LABEL:
    out->as.label.name = ct_substitute_name(p, s->as.label.name, ctx);
    break;
  case NY_S_GOTO:
    out->as.go.name = ct_substitute_name(p, s->as.go.name, ctx);
    break;
  case NY_S_INCLUDE: {
    size_t path_len = s->as.inc.path ? strlen(s->as.inc.path) : 0;
    out->as.inc.path =
        ct_substitute_string(p, s->as.inc.path, path_len, &path_len, ctx);
    out->as.inc.prefix = ct_substitute_name(p, s->as.inc.prefix, ctx);
    size_t lib_len = s->as.inc.lib ? strlen(s->as.inc.lib) : 0;
    out->as.inc.lib =
        ct_substitute_string(p, s->as.inc.lib, lib_len, &lib_len, ctx);
    break;
  }
  case NY_S_DEFINE:
    out->as.def.name = ct_substitute_decl_name(p, s->as.def.name, ctx);
    out->as.def.value = ct_substitute_name(p, s->as.def.value, ctx);
    break;
  case NY_S_MODULE:
    out->as.module.name = ct_substitute_decl_name(p, s->as.module.name, ctx);
    out->as.module.body = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      vec_push_arena(p->arena, &out->as.module.body,
                     ct_clone_stmt(p, s->as.module.body.data[i], ctx));
    break;
  case NY_S_MACRO:
    out->as.macro.name = ct_substitute_decl_name(p, s->as.macro.name, ctx);
    out->as.macro.args = (ny_expr_list){0};
    for (size_t i = 0; i < s->as.macro.args.len; ++i)
      vec_push_arena(p->arena, &out->as.macro.args,
                     ct_clone_expr(p, s->as.macro.args.data[i], ctx));
    out->as.macro.body = ct_clone_stmt(p, s->as.macro.body, ctx);
    break;
  case NY_S_EXTERN:
    out->as.ext.name = ct_substitute_decl_name(p, s->as.ext.name, ctx);
    out->as.ext.return_type =
        ct_substitute_symbol_name(p, s->as.ext.return_type, ctx);
    {
      size_t link_name_len =
          s->as.ext.link_name ? strlen(s->as.ext.link_name) : 0;
      out->as.ext.link_name = ct_substitute_string(
          p, s->as.ext.link_name, link_name_len, &link_name_len, ctx);
    }
    out->as.ext.params = ct_clone_params(p, &s->as.ext.params, ctx);
    break;
  case NY_S_LEMMA:
    out->as.lemma.name = ct_substitute_decl_name(p, s->as.lemma.name, ctx);
    out->as.lemma.params = ct_clone_params(p, &s->as.lemma.params, ctx);
    out->as.lemma.proposition = ct_clone_expr(p, s->as.lemma.proposition, ctx);
    break;
  case NY_S_VAR:
    memset(&out->as.var.names, 0, sizeof(out->as.var.names));
    memset(&out->as.var.types, 0, sizeof(out->as.var.types));
    for (size_t i = 0; i < s->as.var.names.len; i++)
      vec_push_arena(p->arena, &out->as.var.names,
                     ct_substitute_name(p, s->as.var.names.data[i], ctx));
    for (size_t i = 0; i < s->as.var.types.len; i++)
      vec_push_arena(
          p->arena, &out->as.var.types,
          ct_substitute_symbol_name(p, s->as.var.types.data[i], ctx));
    memset(&out->as.var.exprs, 0, sizeof(out->as.var.exprs));
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      vec_push_arena(p->arena, &out->as.var.exprs,
                     ct_clone_expr(p, s->as.var.exprs.data[i], ctx));
    break;
  case NY_S_FUNC:
    out->as.fn.name = ct_substitute_decl_name(p, s->as.fn.name, ctx);
    out->as.fn.return_type =
        ct_substitute_symbol_name(p, s->as.fn.return_type, ctx);
    {
      size_t link_name_len =
          s->as.fn.link_name ? strlen(s->as.fn.link_name) : 0;
      out->as.fn.link_name = ct_substitute_string(
          p, s->as.fn.link_name, link_name_len, &link_name_len, ctx);
    }
    out->as.fn.params = ct_clone_params(p, &s->as.fn.params, ctx);
    out->as.fn.body = ct_clone_stmt(p, s->as.fn.body, ctx);
    out->as.fn.requires_cond = ct_clone_expr(p, s->as.fn.requires_cond, ctx);
    out->as.fn.decreases_expr = ct_clone_expr(p, s->as.fn.decreases_expr, ctx);
    out->as.fn.attr_proves = ct_clone_expr(p, s->as.fn.attr_proves, ctx);
    break;
  case NY_S_LAYOUT:
    out->as.layout.name = ct_substitute_decl_name(p, s->as.layout.name, ctx);
    out->as.layout.flavor = s->as.layout.flavor;
    out->as.layout.resource_qualifier = s->as.layout.resource_qualifier;
    out->as.layout.fields = (ny_layout_field_list){0};
    for (size_t i = 0; i < s->as.layout.fields.len; i++) {
      size_t default_src_len =
          s->as.layout.fields.data[i].default_src
              ? strlen(s->as.layout.fields.data[i].default_src)
              : 0;
      const char *default_src =
          ct_substitute_string(p, s->as.layout.fields.data[i].default_src,
                               default_src_len, &default_src_len, ctx);
      layout_field_t f = {
          .tok = ct_emitted_token(s->as.layout.fields.data[i].tok, ctx),
          .expansion_origin =
              ctx && ctx->emission_origin
                  ? ctx->emission_origin
                  : s->as.layout.fields.data[i].expansion_origin,
          .name = ct_substitute_name(p, s->as.layout.fields.data[i].name, ctx),
          .type_name = ct_substitute_symbol_name(
              p, s->as.layout.fields.data[i].type_name, ctx),
          .width = s->as.layout.fields.data[i].width,
          .default_value =
              ct_clone_expr(p, s->as.layout.fields.data[i].default_value, ctx),
          .default_src = default_src,
          .is_array = s->as.layout.fields.data[i].is_array,
          .array_len =
              ct_clone_expr(p, s->as.layout.fields.data[i].array_len, ctx),
      };
      vec_push_arena(p->arena, &out->as.layout.fields, f);
    }
    out->as.layout.deftype_params = (ny_param_list){0};
    for (size_t i = 0; i < s->as.layout.deftype_params.len; i++) {
      param_t pr = {
          .name = ct_substitute_name(
              p, s->as.layout.deftype_params.data[i].name, ctx),
          .type = ct_substitute_symbol_name(
              p, s->as.layout.deftype_params.data[i].type, ctx),
          .def = ct_clone_expr(p, s->as.layout.deftype_params.data[i].def, ctx),
          .tok = ct_emitted_token(s->as.layout.deftype_params.data[i].tok, ctx),
          .expansion_origin =
              ctx && ctx->emission_origin
                  ? ctx->emission_origin
                  : s->as.layout.deftype_params.data[i].expansion_origin,
          .syntax_ctx = s->as.layout.deftype_params.data[i].syntax_ctx,
      };
      vec_push_arena(p->arena, &out->as.layout.deftype_params, pr);
    }
    out->as.layout.methods = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      vec_push_arena(p->arena, &out->as.layout.methods,
                     ct_clone_stmt(p, s->as.layout.methods.data[i], ctx));
    break;
  case NY_S_STRUCT:
    out->as.struc.name = ct_substitute_decl_name(p, s->as.struc.name, ctx);
    out->as.struc.fields = (ny_layout_field_list){0};
    for (size_t i = 0; i < s->as.struc.fields.len; i++) {
      size_t default_src_len =
          s->as.struc.fields.data[i].default_src
              ? strlen(s->as.struc.fields.data[i].default_src)
              : 0;
      const char *default_src =
          ct_substitute_string(p, s->as.struc.fields.data[i].default_src,
                               default_src_len, &default_src_len, ctx);
      layout_field_t f = {
          .tok = ct_emitted_token(s->as.struc.fields.data[i].tok, ctx),
          .expansion_origin = ctx && ctx->emission_origin
                                  ? ctx->emission_origin
                                  : s->as.struc.fields.data[i].expansion_origin,
          .name = ct_substitute_name(p, s->as.struc.fields.data[i].name, ctx),
          .type_name = ct_substitute_symbol_name(
              p, s->as.struc.fields.data[i].type_name, ctx),
          .width = s->as.struc.fields.data[i].width,
          .default_value =
              ct_clone_expr(p, s->as.struc.fields.data[i].default_value, ctx),
          .default_src = default_src,
          .is_array = s->as.struc.fields.data[i].is_array,
          .array_len =
              ct_clone_expr(p, s->as.struc.fields.data[i].array_len, ctx),
      };
      vec_push_arena(p->arena, &out->as.struc.fields, f);
    }
    out->as.struc.methods = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      vec_push_arena(p->arena, &out->as.struc.methods,
                     ct_clone_stmt(p, s->as.struc.methods.data[i], ctx));
    break;
  case NY_S_ENUM:
    out->as.enu.name = ct_substitute_decl_name(p, s->as.enu.name, ctx);
    out->as.enu.index_param_name =
        ct_substitute_symbol_name(p, s->as.enu.index_param_name, ctx);
    out->as.enu.index_param_tok =
        ct_emitted_token(s->as.enu.index_param_tok, ctx);
    out->as.enu.index_param_expansion_origin =
        ctx && ctx->emission_origin ? ctx->emission_origin
                                    : s->as.enu.index_param_expansion_origin;
    out->as.enu.type_params = (ny_type_param_list){0};
    out->as.enu.type_param_tokens = (ny_token_list){0};
    for (size_t i = 0; i < s->as.enu.type_params.len; i++)
      vec_push_arena(p->arena, &out->as.enu.type_params,
                     ct_substitute_name(p, s->as.enu.type_params.data[i], ctx));
    for (size_t i = 0; i < s->as.enu.type_param_tokens.len; i++)
      vec_push_arena(
          p->arena, &out->as.enu.type_param_tokens,
          ct_emitted_token(s->as.enu.type_param_tokens.data[i], ctx));
    out->as.enu.items = (ny_stmt_enum_item_list){0};
    for (size_t i = 0; i < s->as.enu.items.len; i++) {
      stmt_enum_item_t item = {
          .tok = ct_emitted_token(s->as.enu.items.data[i].tok, ctx),
          .expansion_origin = ctx && ctx->emission_origin
                                  ? ctx->emission_origin
                                  : s->as.enu.items.data[i].expansion_origin,
          .name = ct_substitute_name(p, s->as.enu.items.data[i].name, ctx),
          .value = ct_clone_expr(p, s->as.enu.items.data[i].value, ctx),
      };
      item.fields = (ny_enum_field_list){0};
      for (size_t j = 0; j < s->as.enu.items.data[i].fields.len; j++) {
        enum_field_t *src = &s->as.enu.items.data[i].fields.data[j];
        enum_field_t field = {
            .tok = ct_emitted_token(src->tok, ctx),
            .expansion_origin = ctx && ctx->emission_origin
                                    ? ctx->emission_origin
                                    : src->expansion_origin,
            .name = ct_substitute_name(p, src->name, ctx),
            .type_name = ct_substitute_symbol_name(p, src->type_name, ctx),
        };
        vec_push_arena(p->arena, &item.fields, field);
      }
      vec_push_arena(p->arena, &out->as.enu.items, item);
    }
    break;
  case NY_S_EXPORT:
    memset(&out->as.exprt.names, 0, sizeof(out->as.exprt.names));
    memset(&out->as.exprt.name_tokens, 0, sizeof(out->as.exprt.name_tokens));
    memset(&out->as.exprt.name_expansion_origins, 0,
           sizeof(out->as.exprt.name_expansion_origins));
    for (size_t i = 0; i < s->as.exprt.names.len; i++)
      vec_push_arena(p->arena, &out->as.exprt.names,
                     ct_substitute_name(p, s->as.exprt.names.data[i], ctx));
    for (size_t i = 0; i < s->as.exprt.names.len; i++) {
      token_t tok = i < s->as.exprt.name_tokens.len
                        ? s->as.exprt.name_tokens.data[i]
                        : s->tok;
      vec_push_arena(p->arena, &out->as.exprt.name_tokens,
                     ct_emitted_token(tok, ctx));
      const token_t *origin =
          ctx && ctx->emission_origin
              ? ctx->emission_origin
              : (i < s->as.exprt.name_expansion_origins.len
                     ? s->as.exprt.name_expansion_origins.data[i]
                     : NULL);
      vec_push_arena(p->arena, &out->as.exprt.name_expansion_origins, origin);
    }
    out->as.exprt.profile = ct_substitute_name(p, s->as.exprt.profile, ctx);
    break;
  case NY_S_IF:
    out->as.iff.test = ct_clone_expr(p, s->as.iff.test, ctx);
    out->as.iff.conseq = ct_clone_stmt(p, s->as.iff.conseq, ctx);
    out->as.iff.alt = ct_clone_stmt(p, s->as.iff.alt, ctx);
    out->as.iff.init = ct_clone_stmt(p, s->as.iff.init, ctx);
    break;
  case NY_S_GUARD:
    out->as.guard.type_name =
        ct_substitute_symbol_name(p, s->as.guard.type_name, ctx);
    out->as.guard.name = ct_substitute_name(p, s->as.guard.name, ctx);
    out->as.guard.value = ct_clone_expr(p, s->as.guard.value, ctx);
    out->as.guard.fallback = ct_clone_stmt(p, s->as.guard.fallback, ctx);
    break;
  case NY_S_WHILE:
    out->as.whl.test = ct_clone_expr(p, s->as.whl.test, ctx);
    out->as.whl.invariant = ct_clone_expr(p, s->as.whl.invariant, ctx);
    out->as.whl.body = ct_clone_stmt(p, s->as.whl.body, ctx);
    out->as.whl.update = ct_clone_stmt(p, s->as.whl.update, ctx);
    out->as.whl.init = ct_clone_stmt(p, s->as.whl.init, ctx);
    break;
  case NY_S_FOR:
    out->as.fr.iter_var = ct_substitute_name(p, s->as.fr.iter_var, ctx);
    out->as.fr.iter_index_var =
        ct_substitute_name(p, s->as.fr.iter_index_var, ctx);
    out->as.fr.iterable = ct_clone_expr(p, s->as.fr.iterable, ctx);
    out->as.fr.body = ct_clone_stmt(p, s->as.fr.body, ctx);
    out->as.fr.init = ct_clone_stmt(p, s->as.fr.init, ctx);
    out->as.fr.cond = ct_clone_expr(p, s->as.fr.cond, ctx);
    out->as.fr.update = ct_clone_stmt(p, s->as.fr.update, ctx);
    break;
  case NY_S_DEFER:
    out->as.de.body = ct_clone_stmt(p, s->as.de.body, ctx);
    break;
  case NY_S_TRY:
    out->as.tr.body = ct_clone_stmt(p, s->as.tr.body, ctx);
    out->as.tr.err = ct_substitute_name(p, s->as.tr.err, ctx);
    out->as.tr.handler = ct_clone_stmt(p, s->as.tr.handler, ctx);
    break;
  case NY_S_MATCH:
    out->as.match.test = ct_clone_expr(p, s->as.match.test, ctx);
    out->as.match.arms = (ny_match_arm_list){0};
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t arm = {0};
      for (size_t j = 0; j < s->as.match.arms.data[i].patterns.len; j++) {
        vec_push_arena(p->arena, &arm.patterns,
                       ct_clone_pattern(
                           p, s->as.match.arms.data[i].patterns.data[j], ctx));
      }
      arm.guard = ct_clone_expr(p, s->as.match.arms.data[i].guard, ctx);
      arm.conseq = ct_clone_stmt(p, s->as.match.arms.data[i].conseq, ctx);
      vec_push_arena(p->arena, &out->as.match.arms, arm);
    }
    out->as.match.default_conseq =
        ct_clone_stmt(p, s->as.match.default_conseq, ctx);
    break;
  case NY_S_OPERATOR:
    out->as.oper.left_type =
        ct_substitute_symbol_name(p, s->as.oper.left_type, ctx);
    out->as.oper.right_type =
        ct_substitute_symbol_name(p, s->as.oper.right_type, ctx);
    out->as.oper.return_type =
        ct_substitute_symbol_name(p, s->as.oper.return_type, ctx);
    out->as.oper.target = ct_substitute_symbol_name(p, s->as.oper.target, ctx);
    break;
  case NY_S_IMPL:
    out->as.impl.type_name =
        ct_substitute_symbol_name(p, s->as.impl.type_name, ctx);
    out->as.impl.trait_name =
        ct_substitute_symbol_name(p, s->as.impl.trait_name, ctx);
    out->as.impl.laws = (ny_expr_list){0};
    for (size_t i = 0; i < s->as.impl.laws.len; ++i)
      vec_push_arena(p->arena, &out->as.impl.laws,
                     ct_clone_expr(p, s->as.impl.laws.data[i], ctx));
    out->as.impl.methods = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      vec_push_arena(p->arena, &out->as.impl.methods,
                     ct_clone_stmt(p, s->as.impl.methods.data[i], ctx));
    break;
  case NY_S_REFINE:
    out->as.refine.name = ct_substitute_decl_name(p, s->as.refine.name, ctx);
    out->as.refine.base_type =
        ct_substitute_symbol_name(p, s->as.refine.base_type, ctx);
    out->as.refine.param = ct_substitute_name(p, s->as.refine.param, ctx);
    out->as.refine.predicate = ct_clone_expr(p, s->as.refine.predicate, ctx);
    break;
  case NY_S_TRAIT:
    out->as.trait.name = ct_substitute_decl_name(p, s->as.trait.name, ctx);
    out->as.trait.methods = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.trait.methods.len; ++i)
      vec_push_arena(p->arena, &out->as.trait.methods,
                     ct_clone_stmt(p, s->as.trait.methods.data[i], ctx));
    break;
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    break;
  default:
    break;
  }
  return out;
}

static const char *impl_replace_exact_type(parser_t *p, const char *name,
                                           const char *from_owner,
                                           const char *to_owner) {
  if (!name || !from_owner || !to_owner)
    return name;
  size_t prefix_len = 0;
  while (name[prefix_len] == '?' || name[prefix_len] == '*')
    prefix_len++;
  const char *core = name + prefix_len;
  if (strcmp(core, from_owner) != 0)
    return name;
  if (prefix_len == 0)
    return to_owner;
  size_t to_len = strlen(to_owner);
  char *buf = arena_alloc(p->arena, prefix_len + to_len + 1);
  memcpy(buf, name, prefix_len);
  memcpy(buf + prefix_len, to_owner, to_len + 1);
  return parser_intern(p, buf, prefix_len + to_len);
}

static const char *impl_replace_owner_prefix(parser_t *p, const char *name,
                                             const char *from_owner,
                                             const char *to_owner) {
  if (!name || !from_owner || !to_owner)
    return name;
  size_t from_len = strlen(from_owner);
  if (strncmp(name, from_owner, from_len) != 0)
    return name;
  if (name[from_len] == '\0')
    return to_owner;
  if (name[from_len] != '.')
    return name;
  size_t to_len = strlen(to_owner);
  size_t rest_len = strlen(name + from_len);
  char *buf = arena_alloc(p->arena, to_len + rest_len + 1);
  memcpy(buf, to_owner, to_len);
  memcpy(buf + to_len, name + from_len, rest_len + 1);
  return parser_intern(p, buf, to_len + rest_len);
}

static void impl_rewrite_stmt_owner(parser_t *p, stmt_t *s,
                                    const char *from_owner,
                                    const char *to_owner) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      impl_rewrite_stmt_owner(p, s->as.block.body.data[i], from_owner,
                              to_owner);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.types.len; i++)
      s->as.var.types.data[i] = impl_replace_exact_type(
          p, s->as.var.types.data[i], from_owner, to_owner);
    break;
  case NY_S_FUNC:
    s->as.fn.name =
        impl_replace_owner_prefix(p, s->as.fn.name, from_owner, to_owner);
    s->as.fn.return_type =
        impl_replace_exact_type(p, s->as.fn.return_type, from_owner, to_owner);
    for (size_t i = 0; i < s->as.fn.params.len; i++)
      s->as.fn.params.data[i].type = impl_replace_exact_type(
          p, s->as.fn.params.data[i].type, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.fn.body, from_owner, to_owner);
    break;
  case NY_S_OPERATOR:
    s->as.oper.left_type =
        impl_replace_exact_type(p, s->as.oper.left_type, from_owner, to_owner);
    s->as.oper.right_type =
        impl_replace_exact_type(p, s->as.oper.right_type, from_owner, to_owner);
    s->as.oper.return_type = impl_replace_exact_type(p, s->as.oper.return_type,
                                                     from_owner, to_owner);
    s->as.oper.target =
        impl_replace_owner_prefix(p, s->as.oper.target, from_owner, to_owner);
    break;
  case NY_S_IMPL:
    s->as.impl.type_name =
        impl_replace_exact_type(p, s->as.impl.type_name, from_owner, to_owner);
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      impl_rewrite_stmt_owner(p, s->as.impl.methods.data[i], from_owner,
                              to_owner);
    break;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.fields.len; i++)
      s->as.layout.fields.data[i].type_name = impl_replace_exact_type(
          p, s->as.layout.fields.data[i].type_name, from_owner, to_owner);
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      impl_rewrite_stmt_owner(p, s->as.layout.methods.data[i], from_owner,
                              to_owner);
    break;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.fields.len; i++)
      s->as.struc.fields.data[i].type_name = impl_replace_exact_type(
          p, s->as.struc.fields.data[i].type_name, from_owner, to_owner);
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      impl_rewrite_stmt_owner(p, s->as.struc.methods.data[i], from_owner,
                              to_owner);
    break;
  case NY_S_GUARD:
    s->as.guard.type_name =
        impl_replace_exact_type(p, s->as.guard.type_name, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.guard.fallback, from_owner, to_owner);
    break;
  case NY_S_IF:
    impl_rewrite_stmt_owner(p, s->as.iff.init, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.iff.conseq, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.iff.alt, from_owner, to_owner);
    break;
  case NY_S_WHILE:
    impl_rewrite_stmt_owner(p, s->as.whl.init, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.whl.update, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.whl.body, from_owner, to_owner);
    break;
  case NY_S_FOR:
    impl_rewrite_stmt_owner(p, s->as.fr.init, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.fr.update, from_owner, to_owner);
    impl_rewrite_stmt_owner(p, s->as.fr.body, from_owner, to_owner);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      impl_rewrite_stmt_owner(p, s->as.match.arms.data[i].conseq, from_owner,
                              to_owner);
    impl_rewrite_stmt_owner(p, s->as.match.default_conseq, from_owner,
                            to_owner);
    break;
  default:
    break;
  }
}

static stmt_t *impl_clone_for_owner(parser_t *p, stmt_t *base,
                                    const char *from_owner,
                                    const char *to_owner) {
  if (!base || !from_owner || !to_owner)
    return NULL;
  stmt_t *copy = ct_clone_stmt(p, base, NULL);
  impl_rewrite_stmt_owner(p, copy, from_owner, to_owner);
  return copy;
}

static bool parse_comptime_reflect_templates(parser_t *p,
                                             ny_stmt_list *templates) {
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    bool hash_emit = p->cur.kind == NY_T_HASH;
    if (hash_emit) {
      parser_advance(p);
      if (!tok_is_hash_kw(p->cur, "insert", NY_T_IDENT) &&
          !tok_is_hash_kw(p->cur, "emit", NY_T_IDENT)) {
        parser_error(
            p, p->cur,
            "expected '#insert' or '#emit' in comptime reflection block",
            "write #insert statement_template");
        parser_sync_stmt_boundary(p);
        continue;
      }
    } else if (!tok_is_ident_text(p->cur, "emit")) {
      parser_error(
          p, p->cur, "expected 'emit' in comptime reflection block",
          "write emit statement_template or #insert statement_template");
      parser_sync_stmt_boundary(p);
      continue;
    }
    parser_advance(p);
    stmt_t *tmpl =
        p->cur.kind == NY_T_LBRACE ? p_parse_block(p) : p_parse_stmt(p);
    if (tmpl)
      vec_push_arena(p->arena, templates, tmpl);
  }
  return parser_expect(p, NY_T_RBRACE, "'}' after comptime reflection block",
                       NULL),
         true;
}

static stmt_t *ct_make_deferred_reflection_stmt(parser_t *p, token_t tok,
                                                ct_reflect_kind_t kind,
                                                const char *target,
                                                const char *var,
                                                ny_stmt_list *templates) {
  stmt_t *stmt = stmt_new(p->arena, NY_S_EXPR, tok);
  expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
  call->as.call.deferred_comptime_reflection = true;
  const char *callee_name =
      kind == CT_REFLECT_FIELDS     ? "__ny_deferred_comptime_fields"
      : kind == CT_REFLECT_EXPORTS  ? "__ny_deferred_comptime_exports"
      : kind == CT_REFLECT_VARIANTS ? "__ny_deferred_comptime_variants"
                                    : "__ny_deferred_comptime_attributes";
  call->as.call.callee = ct_make_ident_expr(p, tok, callee_name);
  vec_push_arena(p->arena, &call->as.call.args,
                 ((call_arg_t){.val = ct_make_ident_expr(p, tok, target)}));
  vec_push_arena(p->arena, &call->as.call.args,
                 ((call_arg_t){.val = ct_make_ident_expr(p, tok, var)}));
  for (size_t i = 0; templates && i < templates->len; ++i) {
    expr_t *quoted = expr_new(p->arena, NY_E_QUOTE, templates->data[i]->tok);
    quoted->as.quote.body = templates->data[i];
    quoted->as.quote.syntax_ctx = p->current_syntax_ctx;
    vec_push_arena(p->arena, &call->as.call.args,
                   ((call_arg_t){.val = quoted}));
  }
  stmt->as.expr.expr = call;
  return stmt;
}

static stmt_t *parse_comptime_reflect_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_COMPTIME)
    parser_advance(p);
  bool is_fields = tok_is_ident_text(p->cur, "fields");
  bool is_exports = tok_is_ident_text(p->cur, "exports");
  bool is_variants = tok_is_ident_text(p->cur, "variants");
  bool is_attributes = tok_is_ident_text(p->cur, "attributes");
  if (!is_fields && !is_exports && !is_variants && !is_attributes) {
    parser_error(p, p->cur,
                 "expected 'fields', 'exports', 'variants', or 'attributes' "
                 "after 'comptime'",
                 NULL);
    return NULL;
  }
  parser_advance(p);
  parser_expect(p, NY_T_LPAREN, "'(' after comptime reflection query", NULL);
  char *target_owned = parse_dotted_ident_owned(
      p, "expected comptime reflection target",
      "expected identifier after '.' in reflection target");
  if (!target_owned)
    return NULL;
  const char *target = parser_intern(p, target_owned, strlen(target_owned));
  free(target_owned);
  parser_expect(p, NY_T_RPAREN, "')' after comptime reflection target", NULL);
  parser_expect(p, NY_T_AS, "'as' after comptime reflection target",
                "write comptime fields(Type) as f { emit ... }");
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected comptime reflection binding name", NULL);
    return NULL;
  }
  const char *var =
      parser_intern_hash(p, p->cur.lexeme, p->cur.len, p->cur.hash);
  parser_advance(p);
  parser_expect(p, NY_T_LBRACE, "'{' after comptime reflection binding", NULL);

  ny_stmt_list templates = {0};
  parse_comptime_reflect_templates(p, &templates);

  if (p->lex.template_depth > 0)
    return ct_make_deferred_reflection_stmt(p, tok,
                                            is_fields    ? CT_REFLECT_FIELDS
                                            : is_exports ? CT_REFLECT_EXPORTS
                                            : is_variants
                                                ? CT_REFLECT_VARIANTS
                                                : CT_REFLECT_ATTRIBUTES,
                                            target, var, &templates);

  stmt_t *block = stmt_new_transparent_block(p, tok);
  if (is_fields) {
    parser_ct_layout_meta *layout = parser_find_layout_meta(p, target);
    if (!layout) {
      parser_error(p, tok, "unknown layout in comptime fields query",
                   "declare the layout before comptime fields(...)");
      return block;
    }
    for (size_t i = 0; i < layout->fields.len; i++) {
      ct_reflect_ctx_t ctx = {
          .kind = CT_REFLECT_FIELDS,
          .var = var,
          .layout_name = layout->name,
          .field = &layout->fields.data[i],
          .field_index = (int)i,
      };
      for (size_t j = 0; j < templates.len; j++)
        stmt_list_push_flat(p, &block->as.block.body,
                            ct_clone_stmt(p, templates.data[j], &ctx));
    }
  } else if (is_exports) {
    parser_ct_module_meta *mod = parser_find_module_meta(p, target);
    if (!mod) {
      parser_error(p, tok, "unknown module in comptime exports query",
                   "declare or import the module before comptime exports(...)");
      return block;
    }
    for (size_t i = 0; i < mod->exports.len; i++) {
      ct_reflect_ctx_t ctx = {
          .kind = CT_REFLECT_EXPORTS,
          .var = var,
          .export_name = mod->exports.data[i],
      };
      for (size_t j = 0; j < templates.len; j++)
        stmt_list_push_flat(p, &block->as.block.body,
                            ct_clone_stmt(p, templates.data[j], &ctx));
    }
  } else if (is_variants) {
    bool ambiguous = false;
    stmt_t *decl = parser_find_ct_decl(p, target, &ambiguous);
    if (!decl || decl->kind != NY_S_ENUM) {
      parser_error(p, tok,
                   ambiguous ? "ambiguous enum in comptime variants query"
                             : "unknown enum in comptime variants query",
                   "declare or qualify the enum before comptime variants(...)");
      return block;
    }
    for (size_t i = 0; i < decl->as.enu.items.len; ++i) {
      ct_reflect_ctx_t ctx = {
          .kind = CT_REFLECT_VARIANTS,
          .var = var,
          .enum_name = decl->as.enu.name,
          .variant = &decl->as.enu.items.data[i],
          .variant_index = (int)i,
      };
      for (size_t j = 0; j < templates.len; j++)
        stmt_list_push_flat(p, &block->as.block.body,
                            ct_clone_stmt(p, templates.data[j], &ctx));
    }
  } else {
    bool ambiguous = false;
    stmt_t *decl = parser_find_ct_decl(p, target, &ambiguous);
    if (!decl) {
      parser_error(p, tok,
                   ambiguous
                       ? "ambiguous declaration in comptime attributes query"
                       : "unknown declaration in comptime attributes query",
                   "declare or qualify the target before the reflection query");
      return block;
    }
    for (size_t i = 0; i < decl->attributes.len; ++i) {
      ct_reflect_ctx_t ctx = {
          .kind = CT_REFLECT_ATTRIBUTES,
          .var = var,
          .attribute = &decl->attributes.data[i],
      };
      for (size_t j = 0; j < templates.len; j++)
        stmt_list_push_flat(p, &block->as.block.body,
                            ct_clone_stmt(p, templates.data[j], &ctx));
    }
  }
  return block;
}

static parser_ct_template_meta *parser_find_ct_template(parser_t *p,
                                                        const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < p->ct_templates.len; i++) {
    if (strcmp(p->ct_templates.data[i].name, name) == 0)
      return &p->ct_templates.data[i];
  }
  if (p->current_module && *p->current_module && !strchr(name, '.')) {
    size_t mlen = strlen(p->current_module);
    size_t nlen = strlen(name);
    char *qualified = arena_alloc(p->arena, mlen + 1 + nlen + 1);
    memcpy(qualified, p->current_module, mlen);
    qualified[mlen] = '.';
    memcpy(qualified + mlen + 1, name, nlen + 1);
    for (size_t i = 0; i < p->ct_templates.len; i++) {
      if (strcmp(p->ct_templates.data[i].name, qualified) == 0)
        return &p->ct_templates.data[i];
    }
  }
  if (!strchr(name, '.')) {
    for (size_t i = 0; i < p->ct_templates.len; i++) {
      const char *leaf = ny_tail_name(p->ct_templates.data[i].name);
      if (leaf && strcmp(leaf, name) == 0)
        return &p->ct_templates.data[i];
    }
  }
  return NULL;
}

static stmt_t *parse_comptime_template_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_COMPTIME)
    parser_advance(p);
  if (tok_is_ident_text(p->cur, "template")) {
    parser_advance(p);
  } else {
    parser_error(p, p->cur, "expected 'template' after 'comptime'",
                 "write comptime template name(args) { ... }");
    return stmt_new_transparent_block(p, tok);
  }

  const char *name = parse_qualified_name(p);
  if (!name)
    return NULL;

  parser_ct_template_meta tmpl = {.name = name};
  parser_expect(p, NY_T_LPAREN, "'(' after comptime template name", NULL);
  while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected comptime template parameter name",
                   "template parameters are untyped compile-time values");
      break;
    }
    vec_push_arena(
        p->arena, &tmpl.params,
        parser_intern_hash(p, p->cur.lexeme, p->cur.len, p->cur.hash));
    parser_advance(p);
    const char *category = NULL;
    if (parser_match(p, NY_T_COLON)) {
      if (!tok_is_ident_text(p->cur, "code")) {
        parser_error(p, p->cur, "expected code category in template parameter",
                     "write `value: code<expr>`, `code<stmt>`, `code<decl>`, "
                     "or `code<ident>`");
      } else {
        parser_advance(p);
        parser_expect(p, NY_T_LT, "'<' after 'code'", NULL);
        if (tok_is_ident_text(p->cur, "expr"))
          category = "expr";
        else if (tok_is_ident_text(p->cur, "stmt"))
          category = "stmt";
        else if (tok_is_ident_text(p->cur, "decl"))
          category = "decl";
        else if (tok_is_ident_text(p->cur, "ident"))
          category = "ident";
        else
          parser_error(p, p->cur, "unknown comptime code category",
                       "supported categories are expr, stmt, decl, and ident");
        if (category)
          parser_advance(p);
        parser_expect(p, NY_T_GT, "'>' after code category", NULL);
      }
    }
    vec_push_arena(p->arena, &tmpl.param_categories, category);
    if (!parser_match(p, NY_T_COMMA))
      break;
  }
  parser_expect(p, NY_T_RPAREN, "')' after comptime template parameters", NULL);
  parser_expect(p, NY_T_LBRACE, "'{' after comptime template header", NULL);

  /*
   * Template bodies keep `${param}` as a literal placeholder identifier.
   * Without this depth flag the lexer emits DOLLAR_LBRACE inside the body
   * and every `module.${param}` member access fails to parse.
   */
  p->lex.template_depth++;
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    stmt_t *s = p_parse_stmt(p);
    if (s) {
      vec_push_arena(p->arena, &tmpl.body, s);
    } else if (p->had_error) {
      parser_sync_stmt_boundary(p);
    }
  }
  p->lex.template_depth--;
  parser_expect(p, NY_T_RBRACE, "'}' after comptime template body", NULL);
  vec_push_arena(p->arena, &p->ct_templates, tmpl);
  return stmt_new_transparent_block(p, tok);
}

static bool ct_value_from_literal_expr(parser_t *p, expr_t *e,
                                       ct_value_t *out) {
  if (!e || !out || e->kind != NY_E_LITERAL)
    return false;
  out->source_tok = e->tok;
  out->has_source_tok = true;
  switch (e->as.literal.kind) {
  case NY_LIT_STR:
    out->kind = CT_VALUE_STRING;
    out->s = parser_intern(p, e->as.literal.as.s.data, e->as.literal.as.s.len);
    out->s_len = e->as.literal.as.s.len;
    out->s_counted = true;
    return true;
  case NY_LIT_INT:
    out->kind = CT_VALUE_INT;
    out->i = e->as.literal.as.i;
    return true;
  case NY_LIT_BOOL:
    out->kind = CT_VALUE_BOOL;
    out->b = e->as.literal.as.b;
    return true;
  case NY_LIT_FLOAT:
    return false;
  }
  return false;
}

static const char *ct_symbol_name_from_expr(parser_t *p, expr_t *e) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_IDENT)
    return e->as.ident.name;
  if (e->kind == NY_E_MEMBER) {
    const char *base = ct_symbol_name_from_expr(p, e->as.member.target);
    const char *member = e->as.member.name;
    if (!base || !member)
      return NULL;
    size_t blen = strlen(base);
    size_t mlen = strlen(member);
    char *buf = arena_alloc(p->arena, blen + 1 + mlen + 1);
    memcpy(buf, base, blen);
    buf[blen] = '.';
    memcpy(buf + blen + 1, member, mlen + 1);
    return parser_intern(p, buf, blen + 1 + mlen);
  }
  return NULL;
}

static bool ct_value_from_symbol_expr(parser_t *p, expr_t *e, ct_value_t *out) {
  const char *name = ct_symbol_name_from_expr(p, e);
  if (!out || !name)
    return false;
  out->kind = CT_VALUE_IDENT;
  out->s = parser_intern(p, name, strlen(name));
  out->source_tok = e->tok;
  out->has_source_tok = true;
  if (e->kind == NY_E_IDENT)
    out->syntax_ctx = e->as.ident.syntax_ctx;
  else if (e->kind == NY_E_MEMBER && e->as.member.target &&
           e->as.member.target->kind == NY_E_IDENT)
    out->syntax_ctx = e->as.member.target->as.ident.syntax_ctx;
  return true;
}

static bool ct_value_from_template_arg(parser_t *p, expr_t *e,
                                       const char *loop_var,
                                       const ct_value_t *loop_value,
                                       ct_value_t *out);

static bool ct_parse_code_decl(const stmt_t *stmt) {
  if (!stmt)
    return false;
  if (stmt->kind == NY_S_VAR)
    return stmt->as.var.is_decl;
  switch (stmt->kind) {
  case NY_S_USE:
  case NY_S_FUNC:
  case NY_S_LEMMA:
  case NY_S_EXTERN:
  case NY_S_LINK:
  case NY_S_LAYOUT:
  case NY_S_MODULE:
  case NY_S_EXPORT:
  case NY_S_STRUCT:
  case NY_S_ENUM:
  case NY_S_MACRO:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_OPERATOR:
  case NY_S_IMPL:
  case NY_S_REFINE:
  case NY_S_TRAIT:
    return true;
  default:
    return false;
  }
}

static bool ct_value_from_parse_code(parser_t *p, expr_t *call,
                                     ct_value_t *out) {
  if (!p || !call || !out || call->as.call.args.len != 1 ||
      !call->as.call.args.data[0].val) {
    parser_error(p, call ? call->tok : p->cur,
                 "parse_code(...) expects one source string",
                 "pass a literal declaration source to parse_code");
    return false;
  }
  expr_t *source_expr = call->as.call.args.data[0].val;
  if (source_expr->kind != NY_E_LITERAL ||
      source_expr->as.literal.kind != NY_LIT_STR) {
    parser_error(p, source_expr->tok,
                 "compile-time parse_code expects a string literal",
                 "pass the declaration source directly to parse_code");
    return false;
  }
  if (memchr(source_expr->as.literal.as.s.data, '\0',
             source_expr->as.literal.as.s.len)) {
    parser_error(p, source_expr->tok,
                 "compile-time parse_code source contains a NUL byte",
                 "pass valid source text without embedded NUL bytes");
    return false;
  }
  const char *source =
      arena_strndup(p->arena, source_expr->as.literal.as.s.data,
                    source_expr->as.literal.as.s.len);
  parser_t sub;
  const char *origin =
      source_expr->tok.filename ? source_expr->tok.filename : "<parse_code>";
  parser_init_with_arena_quiet(&sub, source, origin, p->arena);
  sub.exit_on_limit = false;
  sub.current_module = p->current_module;
  sub.current_module_id = p->current_module_id;
  sub.current_module_features = p->current_module_features;
  sub.current_impl_owner = p->current_impl_owner;
  /*
   * Parsing an isolated source must not append into the caller's registries.
   * A struct assignment would share the backing arrays, and vec_push_arena
   * writes into spare capacity before it allocates a new one.
   */
#define CT_COPY_REGISTRY(dst, src)                                             \
  do {                                                                         \
    if ((src).len) {                                                           \
      vec_reserve_arena(p->arena, &(dst), (src).len);                          \
      memcpy((dst).data, (src).data, (src).len * sizeof(*(src).data));         \
      (dst).len = (src).len;                                                   \
    }                                                                          \
  } while (0)
  CT_COPY_REGISTRY(sub.ct_layouts, p->ct_layouts);
  CT_COPY_REGISTRY(sub.ct_modules, p->ct_modules);
  CT_COPY_REGISTRY(sub.ct_templates, p->ct_templates);
  CT_COPY_REGISTRY(sub.ct_decls, p->ct_decls);
  CT_COPY_REGISTRY(sub.ct_diag_rules, p->ct_diag_rules);
#undef CT_COPY_REGISTRY
  sub.current_syntax_ctx = p->current_syntax_ctx;
  sub.next_syntax_ctx = p->next_syntax_ctx;
  sub.gensym_counter = p->gensym_counter;
  sub.gensym_scopes = p->gensym_scopes;
  sub.syntax_ctx_depth = p->syntax_ctx_depth;
  memcpy(sub.syntax_ctx_stack, p->syntax_ctx_stack,
         sizeof(sub.syntax_ctx_stack));

  program_t parsed = parse_program(&sub);
  if (sub.had_error) {
    parser_error(p, source_expr->tok,
                 "compile-time parse_code source has syntax errors",
                 "fix the source and pass complete declarations");
    return false;
  }
  if (parsed.body.len == 0) {
    parser_error(p, source_expr->tok,
                 "compile-time parse_code source must contain declarations",
                 "pass one or more complete declarations");
    return false;
  }
  for (size_t i = 0; i < parsed.body.len; ++i) {
    if (!ct_parse_code_decl(parsed.body.data[i])) {
      parser_error(p, source_expr->tok,
                   "compile-time parse_code accepts declaration-only source",
                   "remove executable top-level statements from the source");
      return false;
    }
  }

  /*
   * The isolated parse becomes observable only after every declaration has
   * validated. Failed input must not consume the caller's syntax marks or
   * deterministic gensym sequence.
   */
  p->next_syntax_ctx = sub.next_syntax_ctx;
  p->gensym_counter = sub.gensym_counter;
  p->gensym_scopes = sub.gensym_scopes;

  out->kind = CT_VALUE_AST_LIST;
  out->items_len = parsed.body.len;
  out->items = arena_alloc(p->arena, out->items_len * sizeof(*out->items));
  if (!out->items)
    return false;
  memset(out->items, 0, out->items_len * sizeof(*out->items));
  for (size_t i = 0; i < out->items_len; ++i) {
    out->items[i].kind = CT_VALUE_AST_BLOCK;
    out->items[i].block = parsed.body.data[i];
    out->items[i].syntax_ctx = parsed.body.data[i]->syntax_ctx;
  }
  return true;
}

/*
 * Evaluate a deliberately small, effect-free function boundary at template
 * expansion time. A helper must be a previously declared Ny function whose
 * body contains only immutable single-value locals followed by one `return`.
 * Each expression is interpreted by the syntax-value evaluator below, so
 * arbitrary runtime calls and side effects cannot run here.
 */
static bool ct_value_from_forwarding_function(parser_t *p, expr_t *call,
                                              const char *loop_var,
                                              const ct_value_t *loop_value,
                                              ct_value_t *out) {
  if (!p || !call || call->kind != NY_E_CALL || !call->as.call.callee ||
      call->as.call.callee->kind != NY_E_IDENT ||
      !call->as.call.callee->as.ident.name)
    return false;
  bool ambiguous = false;
  stmt_t *decl =
      parser_find_ct_decl(p, call->as.call.callee->as.ident.name, &ambiguous);
  if (!decl || ambiguous || decl->kind != NY_S_FUNC || !decl->as.fn.body ||
      decl->as.fn.body->kind != NY_S_BLOCK ||
      !decl->as.fn.body->as.block.body.data ||
      decl->as.fn.body->as.block.body.len == 0)
    return false;
  if (call->as.call.args.len != decl->as.fn.params.len)
    return false;
  if (p->ct_value_eval_depth >= 64) {
    parser_error(p, call->tok,
                 "compile-time code helper recursion limit exceeded",
                 "keep syntax-producing helper calls below 64 nested calls");
    return false;
  }
  p->ct_value_eval_depth++;

  ct_reflect_ctx_t ctx = {.kind = CT_REFLECT_TEMPLATE,
                          .module_name = p->current_module};
  for (size_t i = 0; i < decl->as.fn.params.len; ++i) {
    const param_t *param = &decl->as.fn.params.data[i];
    const call_arg_t *arg = &call->as.call.args.data[i];
    if (!param->name || !arg->val || arg->name) {
      p->ct_value_eval_depth--;
      return false;
    }
    ct_value_t value = {0};
    if (!ct_value_from_template_arg(p, arg->val, loop_var, loop_value,
                                    &value)) {
      p->ct_value_eval_depth--;
      return false;
    }
    ct_bind_t bind = {.name = param->name, .value = value};
    vec_push_arena(p->arena, &ctx.binds, bind);
  }

  expr_t *return_expr = NULL;
  for (size_t i = 0; i < decl->as.fn.body->as.block.body.len; ++i) {
    stmt_t *stmt = decl->as.fn.body->as.block.body.data[i];
    if (!stmt)
      goto unsupported_body;
    if (stmt->kind == NY_S_RETURN) {
      if (i + 1 != decl->as.fn.body->as.block.body.len || !stmt->as.ret.value)
        goto unsupported_body;
      return_expr = stmt->as.ret.value;
      break;
    }
    if (stmt->kind != NY_S_VAR || stmt->as.var.is_mut || stmt->as.var.is_del ||
        stmt->as.var.is_destructure || stmt->as.var.names.len != 1 ||
        stmt->as.var.exprs.len != 1 || !stmt->as.var.names.data[0] ||
        !stmt->as.var.exprs.data[0])
      goto unsupported_body;
    expr_t *init = ct_clone_expr(p, stmt->as.var.exprs.data[0], &ctx);
    ct_value_t value = {0};
    if (!init ||
        !ct_value_from_template_arg(p, init, loop_var, loop_value, &value))
      goto unsupported_body;
    ct_bind_t bind = {.name = stmt->as.var.names.data[0], .value = value};
    vec_push_arena(p->arena, &ctx.binds, bind);
  }
  if (!return_expr)
    goto unsupported_body;
  if (return_expr->kind == NY_E_CALL && return_expr->as.call.callee &&
      return_expr->as.call.callee->kind == NY_E_IDENT &&
      !ct_current_unit_declares(p, "unwrap") &&
      strcmp(return_expr->as.call.callee->as.ident.name, "unwrap") == 0 &&
      return_expr->as.call.args.len == 1 &&
      return_expr->as.call.args.data[0].val &&
      return_expr->as.call.args.data[0].val->kind == NY_E_IDENT) {
    const ct_value_t *wrapped = ct_find_bind(
        &ctx, return_expr->as.call.args.data[0].val->as.ident.name);
    if (wrapped && wrapped->typed_code_result) {
      *out = *wrapped;
      out->typed_code_result = false;
      p->ct_value_eval_depth--;
      return true;
    }
  }
  expr_t *specialized = ct_clone_expr(p, return_expr, &ctx);
  bool ok = specialized && ct_value_from_template_arg(p, specialized, loop_var,
                                                      loop_value, out);
  p->ct_value_eval_depth--;
  return ok;

unsupported_body:
  p->ct_value_eval_depth--;
  return false;
}

static bool ct_template_expr_returns_code(parser_t *p, const expr_t *expr) {
  if (!expr || expr->kind != NY_E_CALL || !expr->as.call.callee ||
      expr->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *name = expr->as.call.callee->as.ident.name;
  if (!name)
    return false;
  if ((strcmp(name, "code_expr") == 0 || strcmp(name, "code_stmt") == 0 ||
       strcmp(name, "code_decl") == 0 || strcmp(name, "code_ident") == 0) &&
      !ct_current_unit_declares(p, name))
    return true;
  bool ambiguous = false;
  stmt_t *decl = parser_find_ct_decl(p, name, &ambiguous);
  if (!decl || ambiguous || decl->kind != NY_S_FUNC || !decl->as.fn.return_type)
    return false;
  const char *type = decl->as.fn.return_type;
  return strncmp(type, "Code<", 5) == 0 ||
         strncmp(type, "Result<Code<", 12) == 0;
}

static bool ct_value_from_template_arg(parser_t *p, expr_t *e,
                                       const char *loop_var,
                                       const ct_value_t *loop_value,
                                       ct_value_t *out) {
  if (!e || !out)
    return false;
  /*
   * These spellings also name ordinary library functions. At a template
   * boundary, only apply a parser shortcut when normal local name lookup
   * would not resolve a declaration with that name.
   */
  bool shadows_local_call = false;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT)
    shadows_local_call =
        ct_current_unit_declares(p, e->as.call.callee->as.ident.name);
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT && !shadows_local_call &&
      strcmp(e->as.call.callee->as.ident.name, "parse_code") == 0)
    return ct_value_from_parse_code(p, e, out);
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT && !shadows_local_call &&
      strcmp(e->as.call.callee->as.ident.name, "unwrap") == 0 &&
      e->as.call.args.len == 1 && e->as.call.args.data[0].val &&
      ct_template_expr_returns_code(p, e->as.call.args.data[0].val))
    return ct_value_from_template_arg(p, e->as.call.args.data[0].val, loop_var,
                                      loop_value, out);
  /*
   * The typed AST constructors are ordinary Ny library functions at runtime,
   * but at a template boundary their quote argument is already available as
   * syntax. Preserve that syntax value directly instead of serializing it to
   * the runtime Code<T> envelope and trying to parse it back.
   */
  if (!shadows_local_call && e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT) {
    const char *constructor = e->as.call.callee->as.ident.name;
    const char *accessor_category =
        strcmp(constructor, "code_expr_syntax") == 0    ? "expr"
        : strcmp(constructor, "code_stmt_syntax") == 0  ? "stmt"
        : strcmp(constructor, "code_decl_syntax") == 0  ? "decl"
        : strcmp(constructor, "code_ident_syntax") == 0 ? "ident"
                                                        : NULL;
    if (accessor_category) {
      if (e->as.call.args.len != 1 || !e->as.call.args.data[0].val) {
        parser_error(p, e->tok, "typed code accessor expects one code value",
                     "pass one Code<T> value to its matching syntax accessor");
        return false;
      }
      if (!ct_value_from_template_arg(p, e->as.call.args.data[0].val, loop_var,
                                      loop_value, out))
        return false;
      const char *actual = ct_value_code_category(out);
      bool matches = strcmp(accessor_category, actual) == 0 ||
                     (strcmp(actual, "empty") == 0 &&
                      strcmp(accessor_category, "ident") != 0) ||
                     (strcmp(accessor_category, "expr") == 0 &&
                      strcmp(actual, "exprs") == 0) ||
                     (strcmp(accessor_category, "stmt") == 0 &&
                      strcmp(actual, "stmts") == 0) ||
                     (strcmp(accessor_category, "decl") == 0 &&
                      strcmp(actual, "decls") == 0);
      if (!matches) {
        parser_error(
            p, e->tok, "typed code accessor category mismatch",
            "use the syntax accessor matching the code value category");
        return false;
      }
      out->typed_code_result = false;
      return true;
    }
    const char *category = strcmp(constructor, "code_expr") == 0    ? "expr"
                           : strcmp(constructor, "code_stmt") == 0  ? "stmt"
                           : strcmp(constructor, "code_decl") == 0  ? "decl"
                           : strcmp(constructor, "code_ident") == 0 ? "ident"
                                                                    : NULL;
    if (category) {
      if (e->as.call.args.len != 1 || !e->as.call.args.data[0].val) {
        parser_error(p, e->tok,
                     "typed code constructor expects one syntax value",
                     "pass one quote or identifier to the Code<T> constructor");
        return false;
      }
      if (!ct_value_from_template_arg(p, e->as.call.args.data[0].val, loop_var,
                                      loop_value, out))
        return false;
      const char *actual = ct_value_code_category(out);
      bool matches =
          strcmp(category, actual) == 0 ||
          (strcmp(actual, "empty") == 0 && strcmp(category, "ident") != 0) ||
          (strcmp(category, "expr") == 0 && strcmp(actual, "exprs") == 0) ||
          (strcmp(category, "stmt") == 0 && strcmp(actual, "stmts") == 0) ||
          (strcmp(category, "decl") == 0 && strcmp(actual, "decls") == 0);
      if (!matches) {
        char hint[128];
        snprintf(hint, sizeof(hint), "code_%s cannot wrap code<%s>", category,
                 actual);
        parser_error(p, e->tok, "typed code constructor category mismatch",
                     hint);
        return false;
      }
      out->typed_code_result = true;
      return true;
    }
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT && !shadows_local_call &&
      strcmp(e->as.call.callee->as.ident.name, "codeof") == 0) {
    if (e->as.call.args.len != 1 || !e->as.call.args.data[0].val) {
      parser_error(p, e->tok, "codeof(...) expects one declaration name",
                   "write `codeof(function_name)` before splicing it");
      return false;
    }
    const char *decl_name =
        ct_symbol_name_from_expr(p, e->as.call.args.data[0].val);
    if (!decl_name) {
      parser_error(p, e->as.call.args.data[0].val->tok,
                   "codeof(...) expects a named declaration",
                   "pass a function, layout, or module name");
      return false;
    }
    bool ambiguous = false;
    stmt_t *decl = parser_find_ct_decl(p, decl_name, &ambiguous);
    if (!decl) {
      parser_error(p, e->as.call.args.data[0].val->tok,
                   ambiguous ? "codeof declaration name is ambiguous"
                             : "codeof cannot find this declaration",
                   ambiguous ? "qualify the declaration with its module name"
                             : "declare it before the codeof template call");
      return false;
    }
    out->kind = CT_VALUE_AST_BLOCK;
    out->block = decl;
    out->syntax_ctx = decl->syntax_ctx;
    return true;
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT && !shadows_local_call &&
      (strcmp(e->as.call.callee->as.ident.name, "code_kind") == 0 ||
       strcmp(e->as.call.callee->as.ident.name, "code_category") == 0 ||
       strcmp(e->as.call.callee->as.ident.name, "code_name") == 0 ||
       strcmp(e->as.call.callee->as.ident.name, "code_has_attribute") == 0)) {
    bool query_attribute =
        strcmp(e->as.call.callee->as.ident.name, "code_has_attribute") == 0;
    bool query_kind =
        strcmp(e->as.call.callee->as.ident.name, "code_kind") == 0;
    bool query_category =
        strcmp(e->as.call.callee->as.ident.name, "code_category") == 0;
    size_t expected_args = query_attribute ? 2 : 1;
    if (e->as.call.args.len != expected_args || !e->as.call.args.data[0].val ||
        (query_attribute && !e->as.call.args.data[1].val)) {
      parser_error(
          p, e->tok,
          query_attribute
              ? "code_has_attribute(...) expects code and an attribute name"
          : query_kind     ? "code_kind(...) expects one code value"
          : query_category ? "code_category(...) expects one code value"
                           : "code_name(...) expects one code value",
          NULL);
      return false;
    }
    ct_value_t code = {0};
    if (!ct_value_from_template_arg(p, e->as.call.args.data[0].val, loop_var,
                                    loop_value, &code))
      return false;
    if (query_attribute) {
      ct_value_t name = {0};
      if (!ct_value_from_template_arg(p, e->as.call.args.data[1].val, loop_var,
                                      loop_value, &name))
        return false;
      if (name.kind != CT_VALUE_STRING || !name.s) {
        parser_error(p, e->as.call.args.data[1].val->tok,
                     "code_has_attribute expects a string attribute name",
                     "pass a literal attribute name such as \"trace\"");
        return false;
      }
      stmt_t *decl = ct_single_code_decl(&code);
      bool found = false;
      if (decl) {
        for (size_t i = 0; i < decl->attributes.len; ++i) {
          const char *attribute_name = decl->attributes.data[i].name;
          if (attribute_name && strcmp(attribute_name, name.s) == 0) {
            found = true;
            break;
          }
        }
      }
      out->kind = CT_VALUE_BOOL;
      out->b = found;
      return true;
    }
    const char *value = NULL;
    if (query_kind) {
      if (code.kind == CT_VALUE_AST_BLOCK && code.block) {
        if (code.block->kind == NY_S_BLOCK &&
            code.block->as.block.body.len == 1) {
          const stmt_t *only = code.block->as.block.body.data[0];
          if (only && only->kind == NY_S_EXPR && only->as.expr.expr &&
              !(only->as.expr.expr->kind == NY_E_BINARY &&
                only->as.expr.expr->as.binary.op &&
                strcmp(only->as.expr.expr->as.binary.op, "=") == 0))
            value = ct_expr_kind_name(only->as.expr.expr->kind);
          else if (only)
            value = ct_stmt_kind_name(only->kind);
        }
        if (!value)
          value = ct_stmt_kind_name(code.block->kind);
      } else if (code.kind == CT_VALUE_AST_EXPR && code.expr) {
        if (code.expr->kind == NY_E_QUOTE) {
          const expr_t *quoted_expr = code.expr->as.quote.expr;
          const stmt_t *quoted_body = code.expr->as.quote.body;
          if (quoted_expr) {
            value = ct_expr_kind_name(quoted_expr->kind);
          } else if (quoted_body && quoted_body->kind == NY_S_BLOCK &&
                     quoted_body->as.block.body.len == 1) {
            const stmt_t *only = quoted_body->as.block.body.data[0];
            if (only && only->kind == NY_S_EXPR && only->as.expr.expr &&
                !(only->as.expr.expr->kind == NY_E_BINARY &&
                  only->as.expr.expr->as.binary.op &&
                  strcmp(only->as.expr.expr->as.binary.op, "=") == 0))
              value = ct_expr_kind_name(only->as.expr.expr->kind);
            else if (only)
              value = ct_stmt_kind_name(only->kind);
            else
              value = "invalid";
          } else if (quoted_body && quoted_body->kind != NY_S_BLOCK) {
            value = ct_stmt_kind_name(quoted_body->kind);
          } else {
            value = "quote";
          }
        } else {
          value = ct_expr_kind_name(code.expr->kind);
        }
      } else if (code.kind == CT_VALUE_IDENT)
        value = "ident";
      else if (code.kind == CT_VALUE_AST_LIST)
        value = ct_code_sequence_category(code.items, code.items_len);
      else
        value = "invalid";
    } else if (query_category) {
      value = ct_value_code_category(&code);
      if (strcmp(value, "exprs") == 0)
        value = "expressions";
      else if (strcmp(value, "stmts") == 0)
        value = "statements";
      else if (strcmp(value, "decls") == 0)
        value = "declarations";
    } else {
      stmt_t *decl = ct_single_code_decl(&code);
      value = "";
      if (decl) {
        const char *name = ct_stmt_decl_name(decl);
        if (!name && decl->kind == NY_S_VAR && decl->as.var.is_decl &&
            decl->as.var.names.len == 1)
          name = decl->as.var.names.data[0];
        if (name && *name)
          value = name;
      }
    }
    out->kind = CT_VALUE_STRING;
    out->s = parser_intern(p, value, strlen(value));
    return true;
  }
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT && !shadows_local_call &&
      strcmp(e->as.call.callee->as.ident.name, "ident") == 0) {
    if (e->as.call.args.len != 1 || !e->as.call.args.data[0].val) {
      parser_error(p, e->tok, "ident(...) expects one compile-time value",
                   "write `ident(name)` or `ident(\"name\")`");
      return false;
    }
    ct_value_t inner = {0};
    if (!ct_value_from_template_arg(p, e->as.call.args.data[0].val, loop_var,
                                    loop_value, &inner)) {
      if (!p->had_error)
        parser_error(p, e->tok,
                     "ident(...) expects a compile-time identifier fragment",
                     "use a symbol or a string, integer, or boolean literal");
      return false;
    }
    if (inner.kind == CT_VALUE_IDENT) {
      *out = inner;
      return true;
    }
    if (inner.kind == CT_VALUE_STRING || inner.kind == CT_VALUE_INT ||
        inner.kind == CT_VALUE_BOOL) {
      if (inner.kind == CT_VALUE_STRING && inner.s_counted &&
          memchr(inner.s, '\0', inner.s_len)) {
        parser_error(p, e->tok, "identifier fragment contains a NUL byte",
                     "use a complete identifier without embedded NUL bytes");
        return false;
      }
      const char *fragment = ct_value_text(p, &inner);
      if (!ct_valid_ident_fragment(fragment)) {
        parser_error(p, e->tok,
                     "ident(...) requires a nonempty identifier fragment",
                     "use only letters, digits, and underscores");
        return false;
      }
      out->kind = CT_VALUE_IDENT;
      out->s = parser_intern(p, fragment, strlen(fragment));
      out->source_tok = e->tok;
      out->has_source_tok = true;
      if (e->as.call.callee->kind == NY_E_IDENT)
        out->syntax_ctx = e->as.call.callee->as.ident.syntax_ctx;
      return true;
    }
    parser_error(p, e->tok,
                 "ident(...) expects a compile-time identifier fragment",
                 "use a symbol or a string, integer, or boolean literal");
    return false;
  }
  if (e->kind == NY_E_CALL &&
      ct_value_from_forwarding_function(p, e, loop_var, loop_value, out))
    return true;
  if (e->kind == NY_E_IDENT && loop_var && loop_value && e->as.ident.name &&
      strcmp(e->as.ident.name, loop_var) == 0) {
    *out = *loop_value;
    return true;
  }
  if (e->kind == NY_E_QUOTE) {
    out->kind = e->as.quote.body ? CT_VALUE_AST_BLOCK : CT_VALUE_AST_EXPR;
    out->block = e->as.quote.body;
    out->expr = e->as.quote.expr;
    out->syntax_ctx = e->as.quote.syntax_ctx;
    return true;
  }
  if (e->kind == NY_E_LIST) {
    out->kind = CT_VALUE_AST_LIST;
    out->items_len = e->as.list_like.len;
    out->items =
        out->items_len
            ? arena_alloc(p->arena, out->items_len * sizeof(*out->items))
            : NULL;
    if (out->items_len && !out->items)
      return false;
    if (out->items)
      memset(out->items, 0, out->items_len * sizeof(*out->items));
    for (size_t i = 0; i < out->items_len; i++) {
      if (!ct_value_from_template_arg(p, e->as.list_like.data[i], loop_var,
                                      loop_value, &out->items[i]))
        return false;
    }
    return true;
  }
  return ct_value_from_literal_expr(p, e, out) ||
         ct_value_from_symbol_expr(p, e, out);
}

static void ct_expand_template_call_ex(parser_t *p, stmt_t *block,
                                       expr_t *emit_call, const char *loop_var,
                                       const ct_value_t *loop_value,
                                       const ct_bind_list *extra_binds,
                                       const char *module_name) {
  if (!emit_call || emit_call->kind != NY_E_CALL ||
      !emit_call->as.call.callee ||
      emit_call->as.call.callee->kind != NY_E_IDENT) {
    parser_error(p, emit_call ? emit_call->tok : p->cur,
                 "emit expects a comptime template call",
                 "write emit make_name(arg)");
    return;
  }
  const char *callee = emit_call->as.call.callee->as.ident.name;
  parser_ct_template_meta *tmpl = parser_find_ct_template(p, callee);
  if (!tmpl) {
    parser_error(p, emit_call->tok, "unknown comptime template",
                 "declare comptime template before emitting it");
    return;
  }
  if (emit_call->as.call.args.len != tmpl->params.len) {
    parser_error(p, emit_call->tok, "comptime template argument count mismatch",
                 NULL);
    return;
  }

  ct_reflect_ctx_t ctx = {.kind = CT_REFLECT_TEMPLATE,
                          .module_name = module_name,
                          .emission_origin = &emit_call->tok};
  const char *owner_end = strrchr(tmpl->name, '.');
  if (owner_end)
    ctx.template_module_name =
        parser_intern(p, tmpl->name, (size_t)(owner_end - tmpl->name));
  for (size_t i = 0; i < tmpl->params.len; i++) {
    ct_value_t val = {0};
    if (!ct_value_from_template_arg(p, emit_call->as.call.args.data[i].val,
                                    loop_var, loop_value, &val)) {
      if (!p->had_error)
        parser_error(
            p,
            emit_call->as.call.args.data[i].val
                ? emit_call->as.call.args.data[i].val->tok
                : emit_call->tok,
            "comptime template arguments must be literal values or symbols",
            "pass the comptime loop variable, a literal, a bare symbol, "
            "or `codeof(declaration)`");
      return;
    }
    const char *expected =
        i < tmpl->param_categories.len ? tmpl->param_categories.data[i] : NULL;
    if (expected) {
      const char *actual = ct_value_code_category(&val);
      bool empty_sequence_matches =
          strcmp(actual, "empty") == 0 && strcmp(expected, "ident") != 0;
      bool category_matches =
          strcmp(expected, actual) == 0 || empty_sequence_matches ||
          (strcmp(expected, "expr") == 0 && strcmp(actual, "exprs") == 0) ||
          (strcmp(expected, "stmt") == 0 && strcmp(actual, "stmts") == 0) ||
          (strcmp(expected, "decl") == 0 && strcmp(actual, "decls") == 0);
      if (!category_matches) {
        char category_hint[128];
        snprintf(category_hint, sizeof(category_hint),
                 "parameter requires code<%s>, received code<%s>", expected,
                 actual);
        parser_error(p,
                     emit_call->as.call.args.data[i].val
                         ? emit_call->as.call.args.data[i].val->tok
                         : emit_call->tok,
                     "comptime template code category mismatch", category_hint);
        return;
      }
    }
    ct_bind_t bind = {.name = tmpl->params.data[i], .value = val};
    vec_push_arena(p->arena, &ctx.binds, bind);
  }
  if (extra_binds) {
    for (size_t i = 0; i < extra_binds->len; i++)
      vec_push_arena(p->arena, &ctx.binds, extra_binds->data[i]);
  }

  for (size_t i = 0; i < tmpl->body.len; i++) {
    if (ct_expand_stmt_placeholder(p, &block->as.block.body, tmpl->body.data[i],
                                   &ctx))
      continue;
    stmt_t *expanded = ct_clone_stmt(p, tmpl->body.data[i], &ctx);
    if (expanded)
      stmt_list_push_flat(p, &block->as.block.body, expanded);
  }
}

static void ct_expand_template_call(parser_t *p, stmt_t *block,
                                    expr_t *emit_call, const char *loop_var,
                                    const ct_value_t *loop_value) {
  ct_expand_template_call_ex(p, block, emit_call, loop_var, loop_value, NULL,
                             p->current_module);
}

static stmt_t *parse_emit_call_body(parser_t *p, token_t tok) {
  expr_t *call = p_parse_expr(p, 0);
  parser_match(p, NY_T_SEMI);
  stmt_t *block = stmt_new_transparent_block(p, tok);
  if (call && call->kind == NY_E_QUOTE) {
    token_t *origin = arena_alloc(p->arena, sizeof(*origin));
    if (!origin)
      return block;
    *origin = tok;
    ct_reflect_ctx_t emit_ctx = {.kind = CT_REFLECT_TEMPLATE,
                                 .module_name = p->current_module,
                                 .emission_origin = origin};
    block->tok = ct_emitted_token(block->tok, &emit_ctx);
    block->expansion_origin = origin;
    if (call->as.quote.body) {
      stmt_t *b = ct_clone_stmt(p, call->as.quote.body, &emit_ctx);
      if (b->kind == NY_S_BLOCK) {
        for (size_t i = 0; i < b->as.block.body.len; i++)
          stmt_list_push_flat(p, &block->as.block.body,
                              b->as.block.body.data[i]);
      } else {
        stmt_list_push_flat(p, &block->as.block.body, b);
      }
    } else if (call->as.quote.expr) {
      stmt_t *es =
          stmt_new(p->arena, NY_S_EXPR, ct_emitted_token(call->tok, &emit_ctx));
      es->expansion_origin = origin;
      es->as.expr.expr = ct_clone_expr(p, call->as.quote.expr, &emit_ctx);
      stmt_list_push_flat(p, &block->as.block.body, es);
    }
    return block;
  }
  ct_expand_template_call(p, block, call, NULL, NULL);
  return block;
}

static stmt_t *parse_comptime_emit_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_COMPTIME, "'comptime'", NULL);
  if (!tok_is_ident_text(p->cur, "emit")) {
    parser_error(p, p->cur, "expected 'emit' after 'comptime'",
                 "write comptime emit template_name(args)");
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  parser_advance(p);
  return parse_emit_call_body(p, tok);
}

static stmt_t *parse_hash_insert_stmt(parser_t *p, token_t tok) {
  parser_advance(p); /* insert/emit */
  return parse_emit_call_body(p, tok);
}

static void ct_bind_push(parser_t *p, ct_bind_list *binds, const char *name,
                         ct_value_t value) {
  if (!name || !*name)
    return;
  name = parser_intern(p, name, strlen(name));
  for (size_t i = 0; i < binds->len; i++) {
    if (binds->data[i].name && strcmp(binds->data[i].name, name) == 0) {
      binds->data[i].value = value;
      return;
    }
  }
  ct_bind_t bind = {.name = name, .value = value};
  vec_push_arena(p->arena, binds, bind);
}

static bool ct_value_from_generated_expr(parser_t *p, expr_t *e,
                                         ct_value_t *out) {
  if (e && e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      strcmp(e->as.call.callee->as.ident.name, "ident") == 0)
    return ct_value_from_template_arg(p, e, NULL, NULL, out);
  return ct_value_from_literal_expr(p, e, out) ||
         ct_value_from_symbol_expr(p, e, out);
}

static stmt_t *parse_generated_module(parser_t *p, token_t tok,
                                      const char *mod_name, bool export_all) {
  parser_expect(p, NY_T_IDENT, "'generated'", NULL);
  if (!tok_is_ident_text(p->cur, "from")) {
    parser_error(p, p->cur, "expected 'from' after 'generated'",
                 "write module pkg.name generated from Spec { ... }");
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  parser_advance(p);
  char *spec_owned = parse_dotted_ident_owned(
      p, "expected generator spec after 'from'",
      "expected identifier after '.' in generator spec");
  const char *spec_name =
      spec_owned ? parser_intern(p, spec_owned, strlen(spec_owned)) : "";
  if (spec_owned)
    free(spec_owned);

  parser_expect(p, NY_T_LBRACE, "'{' after generated module spec", NULL);

  parser_module_origin_scope_t prev_origin =
      parser_enter_module_origin(p, mod_name, &tok);
  stmt_t *mod_stmt = stmt_new(p->arena, NY_S_MODULE, tok);
  mod_stmt->as.module.name = mod_name;
  mod_stmt->as.module.path = p->filename;
  mod_stmt->as.module.export_all = export_all;

  ct_bind_list binds = {0};
  ct_value_t module_value = {.kind = CT_VALUE_STRING, .s = mod_name};
  ct_bind_push(p, &binds, "module", module_value);
  ct_bind_push(p, &binds, "module_name", module_value);
  ct_value_t spec_value = {.kind = CT_VALUE_IDENT, .s = spec_name};
  ct_bind_push(p, &binds, "spec", spec_value);
  ct_bind_push(p, &binds, "generated_from", spec_value);

  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (parser_match(p, NY_T_SEMI))
      continue;
    if (p->cur.kind == NY_T_IDENT) {
      if (tok_is_ident_text(p->cur, "export")) {
        stmt_t *ex = parse_module_profile_stmt(p, false);
        if (ex)
          vec_push_arena(p->arena, &mod_stmt->as.module.body, ex);
        continue;
      }
      if (tok_is_ident_text(p->cur, "internal")) {
        stmt_t *ex = parse_module_profile_stmt(p, true);
        if (ex)
          vec_push_arena(p->arena, &mod_stmt->as.module.body, ex);
        continue;
      }
      if (tok_is_ident_text(p->cur, "emit")) {
        token_t emit_tok = p->cur;
        parser_advance(p);
        expr_t *call = p_parse_expr(p, 0);
        parser_match(p, NY_T_SEMI);
        stmt_t *block = stmt_new_transparent_block(p, emit_tok);
        ct_expand_template_call_ex(p, block, call, NULL, NULL, &binds,
                                   mod_name);
        for (size_t i = 0; i < block->as.block.body.len; i++)
          stmt_list_push_flat(p, &mod_stmt->as.module.body,
                              block->as.block.body.data[i]);
        continue;
      }
      if (parser_peek(p).kind == NY_T_ASSIGN) {
        const char *key = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
        parser_expect(p, NY_T_ASSIGN, "'=' after generated module property",
                      NULL);
        expr_t *value_expr = p_parse_expr(p, 0);
        ct_value_t value = {0};
        if (ct_value_from_generated_expr(p, value_expr, &value)) {
          ct_bind_push(p, &binds, key, value);
        } else {
          parser_error(p, value_expr ? value_expr->tok : p->cur,
                       "generated module properties must be compile-time "
                       "literals or symbols",
                       "use strings, ints, bools, or names of tables/specs");
        }
        parser_match(p, NY_T_SEMI);
        continue;
      }
    }
    parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
  }

  parser_expect(p, NY_T_RBRACE, "'}' after generated module", NULL);
  parser_leave_module_origin(p, prev_origin);
  mod_stmt->as.module.src_start = tok.lexeme;
  mod_stmt->as.module.src_end = p->prev.lexeme + p->prev.len;
  parser_register_module_meta(p, mod_stmt);
  return mod_stmt;
}

static stmt_t *parse_comptime_family_for_stmt(parser_t *p, bool *handled) {
  parser_t saved = *p;
  token_t tok = p->cur;
  *handled = false;
  parser_expect(p, NY_T_FOR, "'for'", NULL);
  if (p->cur.kind != NY_T_IDENT) {
    *p = saved;
    return NULL;
  }
  const char *loop_var =
      parser_intern_hash(p, p->cur.lexeme, p->cur.len, p->cur.hash);
  parser_advance(p);
  if (!parser_match(p, NY_T_IN)) {
    *p = saved;
    return NULL;
  }
  bool starts_comptime = p->cur.kind == NY_T_COMPTIME;
  if (p->cur.kind == NY_T_HASH) {
    token_t hash_kw = parser_peek(p);
    starts_comptime = tok_is_hash_kw(hash_kw, "run", NY_T_IDENT) ||
                      tok_is_hash_kw(hash_kw, "comptime", NY_T_COMPTIME) ||
                      tok_is_hash_kw(hash_kw, "comptime", NY_T_IDENT);
  }
  if (!starts_comptime) {
    *p = saved;
    return NULL;
  }
  *handled = true;
  expr_t *iterable = p_parse_expr(p, 0);
  parser_expect(p, NY_T_LBRACE, "'{' after comptime for iterable", NULL);

  if (iterable && iterable->kind == NY_E_COMPTIME &&
      iterable->as.comptime_expr.body &&
      iterable->as.comptime_expr.body->kind == NY_S_BLOCK &&
      iterable->as.comptime_expr.body->as.block.body.len == 1) {
    stmt_t *only = iterable->as.comptime_expr.body->as.block.body.data[0];
    if (only && only->kind == NY_S_RETURN && only->as.ret.value)
      iterable = only->as.ret.value;
  }

  ny_expr_list emits = {0};
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    bool hash_insert = p->cur.kind == NY_T_HASH;
    if (hash_insert) {
      parser_advance(p);
      if (!tok_is_hash_kw(p->cur, "insert", NY_T_IDENT) &&
          !tok_is_hash_kw(p->cur, "emit", NY_T_IDENT)) {
        parser_error(p, p->cur,
                     "expected '#insert' or '#emit' in comptime loop",
                     "write #insert template_name(arg)");
        parser_sync_stmt_boundary(p);
        continue;
      }
      parser_advance(p);
    } else if (tok_is_ident_text(p->cur, "emit")) {
      parser_advance(p);
    } else {
      parser_error(
          p, p->cur, "expected 'emit' in comptime template block",
          "write emit template_name(arg) or #insert template_name(arg)");
      parser_sync_stmt_boundary(p);
      continue;
    }
    expr_t *call = p_parse_expr(p, 0);
    if (call)
      vec_push_arena(p->arena, &emits, call);
    parser_match(p, NY_T_SEMI);
  }
  parser_expect(p, NY_T_RBRACE, "'}' after comptime function-family block",
                NULL);

  stmt_t *block = stmt_new_transparent_block(p, tok);
  if (!iterable ||
      (iterable->kind != NY_E_LIST && iterable->kind != NY_E_TUPLE &&
       iterable->kind != NY_E_SET)) {
    parser_error(p, tok, "comptime for expects a literal list, tuple, or set",
                 "write for x in comptime [\"a\", \"b\"] { emit make(x) }");
    return block;
  }
  for (size_t i = 0; i < iterable->as.list_like.len; i++) {
    ct_value_t loop_value = {0};
    if (!ct_value_from_literal_expr(p, iterable->as.list_like.data[i],
                                    &loop_value)) {
      parser_error(p, iterable->as.list_like.data[i]->tok,
                   "comptime for values must be string/int/bool literals",
                   NULL);
      continue;
    }
    for (size_t j = 0; j < emits.len; j++)
      ct_expand_template_call(p, block, emits.data[j], loop_var, &loop_value);
  }
  return block;
}

static stmt_t *parse_hash_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_advance(p);
  if (tok_is_platform_guard(p->cur))
    return parse_hash_platform_guard_stmt(p, tok);
  if (tok_is_hash_kw(p->cur, "assert", NY_T_IDENT)) {
    parser_advance(p);
    bool has_paren = parser_match(p, NY_T_LPAREN);
    expr_t *cond = p_parse_expr(p, 0);
    if (!cond)
      return NULL;
    if (has_paren)
      parser_expect(p, NY_T_RPAREN, "')' after #assert condition", NULL);
    expr_t *msg = NULL;
    if (p->cur.kind == NY_T_STRING) {
      size_t slen = 0;
      const char *text = parser_decode_string(p, p->cur, &slen);
      msg = expr_new(p->arena, NY_E_LITERAL, p->cur);
      msg->as.literal.kind = NY_LIT_STR;
      msg->as.literal.as.s.data = text;
      msg->as.literal.as.s.len = slen;
      parser_advance(p);
    }
    parser_match(p, NY_T_SEMI);
    expr_t *callee = ct_make_ident_expr(p, tok, "static_assert");
    expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
    call->as.call.callee = callee;
    vec_push_arena(p->arena, &call->as.call.args,
                   ((call_arg_t){.name = NULL, .val = cond}));
    if (msg)
      vec_push_arena(p->arena, &call->as.call.args,
                     ((call_arg_t){.name = NULL, .val = msg}));
    expr_t *ct = make_comptime_return_expr(p, tok, call);
    stmt_t *s = stmt_new(p->arena, NY_S_EXPR, tok);
    s->as.expr.expr = ct;
    return s;
  }
  if (tok_is_hash_kw(p->cur, "insert", NY_T_IDENT) ||
      tok_is_hash_kw(p->cur, "emit", NY_T_IDENT))
    return parse_hash_insert_stmt(p, tok);
  if (tok_is_hash_kw(p->cur, "template", NY_T_IDENT))
    return parse_comptime_template_stmt(p);
  if (tok_is_hash_kw(p->cur, "table", NY_T_IDENT))
    return parse_comptime_table_stmt(p);
  if (tok_is_hash_kw(p->cur, "fields", NY_T_IDENT) ||
      tok_is_hash_kw(p->cur, "exports", NY_T_IDENT) ||
      tok_is_hash_kw(p->cur, "variants", NY_T_IDENT) ||
      tok_is_hash_kw(p->cur, "attributes", NY_T_IDENT))
    return parse_comptime_reflect_stmt(p);
  if (tok_is_hash_kw(p->cur, "diag", NY_T_IDENT)) {
    parser_advance(p);
    return parse_comptime_diagnostic_rule_stmt(p);
  }
  if (tok_is_hash_kw(p->cur, "if", NY_T_IF) ||
      tok_is_hash_kw(p->cur, "elif", NY_T_ELIF))
    return parse_hash_if_stmt(p, tok);
  if (tok_is_hash_kw(p->cur, "main", NY_T_IDENT))
    return parse_hash_main_guard_stmt(p, tok);
  if (tok_is_hash_kw(p->cur, "else", NY_T_ELSE)) {
    parser_error(p, p->cur, "'#else' without matching '#if'", NULL);
    parser_advance(p);
    return NULL;
  }
  if (tok_is_hash_kw(p->cur, "endif", NY_T_IDENT)) {
    parser_error(p, p->cur, "'#endif' without matching '#if'", NULL);
    parser_advance(p);
    return NULL;
  }
  if (p->cur.kind == NY_T_IDENT &&
      strncmp(p->cur.lexeme, "line", p->cur.len) == 0) {
    parser_advance(p);
    if (p->cur.kind == NY_T_NUMBER)
      parser_advance(p);
    if (p->cur.kind == NY_T_STRING)
      parser_advance(p);
    return NULL;
  }
  if (p->cur.kind == NY_T_IDENT &&
      strncmp(p->cur.lexeme, "include", p->cur.len) == 0) {
    parser_advance(p);
    bool is_std = false;
    const char *path = NULL;
    if (p->cur.kind == NY_T_STRING) {
      size_t slen = 0;
      path = parser_decode_string(p, p->cur, &slen);
      parser_advance(p);
    } else if (parser_match(p, NY_T_LT)) {
      is_std = true;
      token_t start = p->cur;
      while (p->cur.kind != NY_T_GT && p->cur.kind != NY_T_EOF)
        parser_advance(p);
      path = arena_strndup(p->arena, start.lexeme,
                           (size_t)(p->cur.lexeme - start.lexeme));
      parser_expect(p, NY_T_GT, "'>' after system header", NULL);
    } else {
      parser_error(p, p->cur, "expected string or '<header>' after '#include'",
                   NULL);
      return NULL;
    }
    const char *prefix = NULL;
    if (parser_match(p, NY_T_AS)) {
      if (p->cur.kind == NY_T_STRING) {
        size_t slen = 0;
        prefix = parser_decode_string(p, p->cur, &slen);
        parser_advance(p);
      } else if (p->cur.kind == NY_T_IDENT) {
        prefix = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
      }
    }
    const char *lib = NULL;
    if (p->cur.kind == NY_T_IDENT &&
        strncmp(p->cur.lexeme, "link", p->cur.len) == 0) {
      parser_advance(p);
      if (p->cur.kind == NY_T_STRING) {
        lib = arena_strndup(p->arena, p->cur.lexeme + 1, p->cur.len - 2);
        parser_advance(p);
      }
    }
    parser_match(p, NY_T_SEMI);
    stmt_t *s = stmt_new(p->arena, NY_S_INCLUDE, tok);
    s->as.inc.path = path;
    s->as.inc.prefix = prefix;
    s->as.inc.is_std = is_std;
    s->as.inc.lib = lib;
    return s;
  }
  if (p->cur.kind == NY_T_IDENT &&
      strncmp(p->cur.lexeme, "define", p->cur.len) == 0) {
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after '#define'", NULL);
      return NULL;
    }
    const char *name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    const char *value = "";
    if (p->cur.kind == NY_T_NUMBER || p->cur.kind == NY_T_IDENT) {
      value = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
    parser_match(p, NY_T_SEMI);
    stmt_t *s = stmt_new(p->arena, NY_S_DEFINE, tok);
    s->as.def.name = name;
    s->as.def.value = value;
    return s;
  }
  if (p->cur.kind != NY_T_IDENT ||
      strncmp(p->cur.lexeme, "link", p->cur.len) != 0) {
    parser_error(p, p->cur,
                 "expected platform guard, 'main', 'link', 'include', "
                 "'define', or 'line' after '#'",
                 "'#' is not a comment in Nytrix (comments start with ';'); "
                 "valid platform guards include #linux, #unix, #windows, "
                 "#macos, #x86, #x86_64, #arm, #aarch64");
    return NULL;
  }
  parser_advance(p);
  if (p->cur.kind != NY_T_STRING) {
    parser_error(p, p->cur, "expected library name string after '#link'", NULL);
    return NULL;
  }
  stmt_t *s = stmt_new(p->arena, NY_S_LINK, tok);
  s->as.link.lib = arena_strndup(p->arena, p->cur.lexeme + 1, p->cur.len - 2);
  parser_advance(p);
  return s;
}
