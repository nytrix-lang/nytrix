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
               "fields, exports; "
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
  return make_comptime_return_expr(
      p, tok, make_zero_arg_call_expr(p, tok, "__main"));
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
    parser_error(p, kw_tok, "expected 'if' or 'elif' after '#'", NULL);
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
    parser_error(p, main_tok, "expected 'main' after '#'", NULL);
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
    parser_error(p, guard_tok, "expected platform guard after '#'", NULL);
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

static stmt_t *parse_comptime_table_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_COMPTIME, "'comptime'", NULL);
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
    if (parser_match(p, NY_T_IF)) {
      arm.guard = p_parse_expr(p, 0);
    }
    parser_expect(p, NY_T_ARROW, "'->' in comptime table entry", NULL);
    token_t value_tok = p->cur;
    expr_t *value = p_parse_expr(p, 0);
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
  fn->as.fn.body = body;
  fn->as.fn.src_start = tok.lexeme;
  fn->as.fn.src_end = p->prev.lexeme + p->prev.len;
  param_t raw = {.name = parser_intern(p, "raw", 3), .type = NULL, .def = NULL};
  param_t fallback = {
      .name = parser_intern(p, "default", 7), .type = NULL, .def = NULL};
  vec_push_arena(p->arena, &fn->as.fn.params, raw);
  vec_push_arena(p->arena, &fn->as.fn.params, fallback);

  stmt_t *wrapper_body = stmt_new(p->arena, NY_S_BLOCK, tok);
  stmt_t *wrapper_ret = stmt_new(p->arena, NY_S_RETURN, tok);
  wrapper_ret->as.ret.value =
      parse_table_call_expr(p, tok, matcher_name, "raw", "default");
  vec_push_arena(p->arena, &wrapper_body->as.block.body, wrapper_ret);

  stmt_t *wrapper = stmt_new(p->arena, NY_S_FUNC, tok);
  wrapper->as.fn.name = legacy_name;
  wrapper->as.fn.body = wrapper_body;
  wrapper->as.fn.src_start = tok.lexeme;
  wrapper->as.fn.src_end = p->prev.lexeme + p->prev.len;
  param_t wrapper_raw = {
      .name = parser_intern(p, "raw", 3), .type = NULL, .def = NULL};
  param_t wrapper_fallback = {
      .name = parser_intern(p, "default", 7),
      .type = NULL,
      .def = ct_int_expr(p, tok, 0),
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
    parser_error(p, p->cur, "expected call.arg(n) in diagnostic rule predicate",
                 NULL);
    return -1;
  }
  parser_advance(p);
  parser_expect(p, NY_T_DOT, "'.' before call predicate member", NULL);
  if (!tok_is_ident_text(p->cur, "arg")) {
    parser_error(p, p->cur, "expected arg in call.arg(n)", NULL);
    return -1;
  }
  parser_advance(p);
  parser_expect(p, NY_T_LPAREN, "'(' after call.arg", NULL);
  if (p->cur.kind != NY_T_NUMBER) {
    parser_error(p, p->cur, "expected integer argument index in call.arg(n)",
                 NULL);
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
                 NULL);
    return false;
  }
  parser_advance(p);
  parser_expect(p, NY_T_DOT, "'.' before call predicate member", NULL);
  if (!tok_is_ident_text(p->cur, "name")) {
    parser_error(p, p->cur, "expected name in call.name predicate", NULL);
    return false;
  }
  parser_advance(p);
  parser_expect(p, NY_T_EQ, "'==' after call.name", NULL);
  if (p->cur.kind != NY_T_STRING) {
    parser_error(p, p->cur,
                 "diagnostic rule call.name expects a string literal", NULL);
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
        "expected is_literal(call.arg(n)) in diagnostic rule predicate", NULL);
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
  parser_expect(p, NY_T_COMPTIME, "'comptime'", NULL);
  if (!tok_is_ident_text(p->cur, "diagnostic")) {
    parser_error(
        p, p->cur, "expected 'diagnostic' after 'comptime'",
        "write comptime diagnostic rule name { when ... error ... fix ... }");
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  parser_advance(p);
  if (!tok_is_ident_text(p->cur, "rule")) {
    parser_error(p, p->cur, "expected 'rule' after 'comptime diagnostic'",
                 NULL);
    return stmt_new(p->arena, NY_S_BLOCK, tok);
  }
  parser_advance(p);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected diagnostic rule name", NULL);
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
                     "diagnostic rule error expects a string literal", NULL);
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
        parser_error(p, p->cur, "diagnostic rule fix expects a string literal",
                     NULL);
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

static parser_ct_layout_meta *parser_find_layout_meta(parser_t *p,
                                                      const char *name) {
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
  CT_REFLECT_TEMPLATE,
} ct_reflect_kind_t;

typedef enum ct_value_kind_t {
  CT_VALUE_STRING,
  CT_VALUE_IDENT,
  CT_VALUE_INT,
  CT_VALUE_BOOL,
} ct_value_kind_t;

typedef struct ct_value_t {
  ct_value_kind_t kind;
  const char *s;
  int64_t i;
  bool b;
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
  layout_field_t *field;
  int field_index;
  const char *export_name;
  ct_bind_list binds;
} ct_reflect_ctx_t;

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

static expr_t *ct_bool_expr(parser_t *p, token_t tok, bool value) {
  expr_t *e = expr_new(p->arena, NY_E_LITERAL, tok);
  e->as.literal.kind = NY_LIT_BOOL;
  e->as.literal.hint = NY_LIT_HINT_NONE;
  e->as.literal.hint_explicit = false;
  e->as.literal.as.b = value;
  return e;
}

static const ct_value_t *ct_find_bind(ct_reflect_ctx_t *ctx, const char *name) {
  if (!ctx || ctx->kind != CT_REFLECT_TEMPLATE || !name)
    return NULL;
  for (size_t i = 0; i < ctx->binds.len; i++) {
    if (ctx->binds.data[i].name && strcmp(ctx->binds.data[i].name, name) == 0)
      return &ctx->binds.data[i].value;
  }
  return NULL;
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

static expr_t *ct_value_expr(parser_t *p, token_t tok, const ct_value_t *v) {
  if (!v)
    return NULL;
  switch (v->kind) {
  case CT_VALUE_STRING:
    return ct_string_expr(p, tok, v->s);
  case CT_VALUE_IDENT:
    return parse_table_ident_expr(p, tok, v->s ? v->s : "");
  case CT_VALUE_INT:
    return ct_int_expr(p, tok, v->i);
  case CT_VALUE_BOOL:
    return ct_bool_expr(p, tok, v->b);
  }
  return NULL;
}

static const char *ct_substitute_name(parser_t *p, const char *name,
                                      ct_reflect_ctx_t *ctx) {
  if (!name || !ctx || ctx->kind != CT_REFLECT_TEMPLATE || !strstr(name, "${"))
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
        const char *val = ct_value_text(p, ct_find_bind(ctx, key));
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
  return parser_intern(p, buf, out);
}

static const char *ct_substitute_symbol_name(parser_t *p, const char *name,
                                             ct_reflect_ctx_t *ctx) {
  if (!name || !ctx || ctx->kind != CT_REFLECT_TEMPLATE)
    return name;
  const ct_value_t *v = ct_find_bind(ctx, name);
  if (v && (v->kind == CT_VALUE_IDENT || v->kind == CT_VALUE_STRING))
    return parser_intern(p, v->s ? v->s : "", strlen(v->s ? v->s : ""));
  const char *dot = strrchr(name, '.');
  if (dot && dot[1]) {
    v = ct_find_bind(ctx, dot + 1);
    if (v && (v->kind == CT_VALUE_IDENT || v->kind == CT_VALUE_STRING)) {
      size_t prefix_len = (size_t)(dot - name) + 1;
      const char *leaf = v->s ? v->s : "";
      size_t leaf_len = strlen(leaf);
      char *buf = arena_alloc(p->arena, prefix_len + leaf_len + 1);
      memcpy(buf, name, prefix_len);
      memcpy(buf + prefix_len, leaf, leaf_len + 1);
      return parser_intern(p, buf, prefix_len + leaf_len);
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
  name = ct_substitute_symbol_name(p, name, ctx);
  return ct_qualify_module_decl_name(p, name, ctx ? ctx->module_name : NULL);
}

static expr_t *ct_layout_offset_expr(parser_t *p, token_t tok,
                                     const char *layout_name,
                                     const char *field_name) {
  expr_t *callee = parse_table_ident_expr(p, tok, "__layout_offset");
  expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
  call->as.call.callee = callee;
  call_arg_t layout_arg = {.name = NULL,
                           .val = ct_string_expr(p, tok, layout_name)};
  call_arg_t field_arg = {.name = NULL,
                          .val = ct_string_expr(p, tok, field_name)};
  vec_push_arena(p->arena, &call->as.call.args, layout_arg);
  vec_push_arena(p->arena, &call->as.call.args, field_arg);
  return call;
}

static expr_t *ct_clone_expr(parser_t *p, expr_t *e, ct_reflect_ctx_t *ctx);
static stmt_t *ct_clone_stmt(parser_t *p, stmt_t *s, ct_reflect_ctx_t *ctx);

static void ct_clone_call_args(parser_t *p, ny_call_arg_list *dst,
                               ny_call_arg_list *src, ct_reflect_ctx_t *ctx) {
  for (size_t i = 0; src && i < src->len; i++) {
    call_arg_t arg = {
        .name = ct_substitute_name(p, src->data[i].name, ctx),
        .val = ct_clone_expr(p, src->data[i].val, ctx),
    };
    vec_push_arena(p->arena, dst, arg);
  }
}

static expr_t *ct_clone_expr(parser_t *p, expr_t *e, ct_reflect_ctx_t *ctx) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_IDENT && ctx && ctx->kind == CT_REFLECT_TEMPLATE &&
      e->as.ident.name) {
    const ct_value_t *v = ct_find_bind(ctx, e->as.ident.name);
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
  if (e->kind == NY_E_MEMBER && ctx && ctx->kind == CT_REFLECT_FIELDS &&
      ny_expr_ident_is_name(e->as.member.target, ctx->var)) {
    const char *member = e->as.member.name;
    if (strcmp(member, "name") == 0)
      return ct_string_expr(p, e->tok, ctx->field ? ctx->field->name : "");
    if (strcmp(member, "type") == 0 || strcmp(member, "type_name") == 0)
      return ct_string_expr(p, e->tok, ctx->field ? ctx->field->type_name : "");
    if (strcmp(member, "offset") == 0)
      return ct_layout_offset_expr(p, e->tok, ctx->layout_name,
                                   ctx->field ? ctx->field->name : "");
    if (strcmp(member, "index") == 0)
      return ct_int_expr(p, e->tok, ctx->field_index);
    parser_error(
        p, e->tok, "unknown comptime field reflection property",
        "available properties are name, type, type_name, offset, and index");
    return ct_string_expr(p, e->tok, "");
  }

  expr_t *out = expr_new(p->arena, e->kind, e->tok);
  *out = *e;
  switch (e->kind) {
  case NY_E_IDENT:
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
    break;
  case NY_E_TRY:
    out->as.try_expr.target = ct_clone_expr(p, e->as.try_expr.target, ctx);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    out->as.list_like = (ny_expr_list){0};
    for (size_t i = 0; i < e->as.list_like.len; i++)
      vec_push_arena(p->arena, &out->as.list_like,
                     ct_clone_expr(p, e->as.list_like.data[i], ctx));
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
    break;
  case NY_E_MATCH:
    out->as.match.test = ct_clone_expr(p, e->as.match.test, ctx);
    out->as.match.arms = (ny_match_arm_list){0};
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t arm = {0};
      for (size_t j = 0; j < e->as.match.arms.data[i].patterns.len; j++) {
        vec_push_arena(
            p->arena, &arm.patterns,
            ct_clone_expr(p, e->as.match.arms.data[i].patterns.data[j], ctx));
      }
      arm.guard = ct_clone_expr(p, e->as.match.arms.data[i].guard, ctx);
      arm.conseq = ct_clone_stmt(p, e->as.match.arms.data[i].conseq, ctx);
      vec_push_arena(p->arena, &out->as.match.arms, arm);
    }
    out->as.match.default_conseq =
        ct_clone_stmt(p, e->as.match.default_conseq, ctx);
    break;
  default:
    break;
  }
  return out;
}

static stmt_t *ct_clone_stmt(parser_t *p, stmt_t *s, ct_reflect_ctx_t *ctx) {
  if (!s)
    return NULL;
  stmt_t *out = stmt_new(p->arena, s->kind, s->tok);
  *out = *s;
  switch (s->kind) {
  case NY_S_BLOCK:
    out->as.block.body = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.block.body.len; i++)
      vec_push_arena(p->arena, &out->as.block.body,
                     ct_clone_stmt(p, s->as.block.body.data[i], ctx));
    break;
  case NY_S_EXPR:
    out->as.expr.expr = ct_clone_expr(p, s->as.expr.expr, ctx);
    break;
  case NY_S_RETURN:
    out->as.ret.value = ct_clone_expr(p, s->as.ret.value, ctx);
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
    out->as.fn.link_name = ct_substitute_name(p, s->as.fn.link_name, ctx);
    out->as.fn.params = (ny_param_list){0};
    for (size_t i = 0; i < s->as.fn.params.len; i++) {
      param_t pr = {
          .name = ct_substitute_name(p, s->as.fn.params.data[i].name, ctx),
          .type =
              ct_substitute_symbol_name(p, s->as.fn.params.data[i].type, ctx),
          .def = ct_clone_expr(p, s->as.fn.params.data[i].def, ctx),
      };
      vec_push_arena(p->arena, &out->as.fn.params, pr);
    }
    out->as.fn.body = ct_clone_stmt(p, s->as.fn.body, ctx);
    break;
  case NY_S_LAYOUT:
    out->as.layout.name = ct_substitute_decl_name(p, s->as.layout.name, ctx);
    out->as.layout.flavor = s->as.layout.flavor;
    out->as.layout.fields = (ny_layout_field_list){0};
    for (size_t i = 0; i < s->as.layout.fields.len; i++) {
      layout_field_t f = {
          .name = ct_substitute_name(p, s->as.layout.fields.data[i].name, ctx),
          .type_name = ct_substitute_symbol_name(
              p, s->as.layout.fields.data[i].type_name, ctx),
          .width = s->as.layout.fields.data[i].width,
          .default_value =
              ct_clone_expr(p, s->as.layout.fields.data[i].default_value, ctx),
          .default_src = s->as.layout.fields.data[i].default_src,
      };
      vec_push_arena(p->arena, &out->as.layout.fields, f);
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
      layout_field_t f = {
          .name = ct_substitute_name(p, s->as.struc.fields.data[i].name, ctx),
          .type_name = ct_substitute_symbol_name(
              p, s->as.struc.fields.data[i].type_name, ctx),
          .width = s->as.struc.fields.data[i].width,
          .default_value =
              ct_clone_expr(p, s->as.struc.fields.data[i].default_value, ctx),
          .default_src = s->as.struc.fields.data[i].default_src,
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
    out->as.enu.type_params = (ny_type_param_list){0};
    for (size_t i = 0; i < s->as.enu.type_params.len; i++)
      vec_push_arena(p->arena, &out->as.enu.type_params,
                     ct_substitute_name(p, s->as.enu.type_params.data[i], ctx));
    out->as.enu.items = (ny_stmt_enum_item_list){0};
    for (size_t i = 0; i < s->as.enu.items.len; i++) {
      stmt_enum_item_t item = {
          .name = ct_substitute_name(p, s->as.enu.items.data[i].name, ctx),
          .value = ct_clone_expr(p, s->as.enu.items.data[i].value, ctx),
      };
      item.fields = (ny_enum_field_list){0};
      for (size_t j = 0; j < s->as.enu.items.data[i].fields.len; j++) {
        enum_field_t *src = &s->as.enu.items.data[i].fields.data[j];
        enum_field_t field = {
            .name = ct_substitute_name(p, src->name, ctx),
            .type_name = ct_substitute_name(p, src->type_name, ctx),
        };
        vec_push_arena(p->arena, &item.fields, field);
      }
      vec_push_arena(p->arena, &out->as.enu.items, item);
    }
    break;
  case NY_S_EXPORT:
    memset(&out->as.exprt.names, 0, sizeof(out->as.exprt.names));
    for (size_t i = 0; i < s->as.exprt.names.len; i++)
      vec_push_arena(p->arena, &out->as.exprt.names,
                     ct_substitute_name(p, s->as.exprt.names.data[i], ctx));
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
  case NY_S_MATCH:
    out->as.match.test = ct_clone_expr(p, s->as.match.test, ctx);
    out->as.match.arms = (ny_match_arm_list){0};
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t arm = {0};
      for (size_t j = 0; j < s->as.match.arms.data[i].patterns.len; j++) {
        vec_push_arena(
            p->arena, &arm.patterns,
            ct_clone_expr(p, s->as.match.arms.data[i].patterns.data[j], ctx));
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
    out->as.impl.methods = (ny_stmt_list){0};
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      vec_push_arena(p->arena, &out->as.impl.methods,
                     ct_clone_stmt(p, s->as.impl.methods.data[i], ctx));
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
    if (!tok_is_ident_text(p->cur, "emit")) {
      parser_error(p, p->cur, "expected 'emit' in comptime reflection block",
                   "write emit statement_template");
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

static stmt_t *parse_comptime_reflect_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_COMPTIME, "'comptime'", NULL);
  bool is_fields = tok_is_ident_text(p->cur, "fields");
  bool is_exports = tok_is_ident_text(p->cur, "exports");
  if (!is_fields && !is_exports) {
    parser_error(p, p->cur, "expected 'fields' or 'exports' after 'comptime'",
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
  } else {
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
  parser_expect(p, NY_T_COMPTIME, "'comptime'", NULL);
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
    if (!parser_match(p, NY_T_COMMA))
      break;
  }
  parser_expect(p, NY_T_RPAREN, "')' after comptime template parameters", NULL);
  parser_expect(p, NY_T_LBRACE, "'{' after comptime template header", NULL);

  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    stmt_t *s = p_parse_stmt(p);
    if (s) {
      vec_push_arena(p->arena, &tmpl.body, s);
    } else if (p->had_error) {
      parser_sync_stmt_boundary(p);
    }
  }
  parser_expect(p, NY_T_RBRACE, "'}' after comptime template body", NULL);
  vec_push_arena(p->arena, &p->ct_templates, tmpl);
  return stmt_new_transparent_block(p, tok);
}

static bool ct_value_from_literal_expr(parser_t *p, expr_t *e,
                                       ct_value_t *out) {
  if (!e || !out || e->kind != NY_E_LITERAL)
    return false;
  switch (e->as.literal.kind) {
  case NY_LIT_STR:
    out->kind = CT_VALUE_STRING;
    out->s = parser_intern(p, e->as.literal.as.s.data, e->as.literal.as.s.len);
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
  return true;
}

static bool ct_value_from_template_arg(parser_t *p, expr_t *e,
                                       const char *loop_var,
                                       const ct_value_t *loop_value,
                                       ct_value_t *out) {
  if (!e || !out)
    return false;
  if (e->kind == NY_E_IDENT && loop_var && loop_value && e->as.ident.name &&
      strcmp(e->as.ident.name, loop_var) == 0) {
    *out = *loop_value;
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
                          .module_name = module_name};
  for (size_t i = 0; i < tmpl->params.len; i++) {
    ct_value_t val = {0};
    if (!ct_value_from_template_arg(p, emit_call->as.call.args.data[i].val,
                                    loop_var, loop_value, &val)) {
      parser_error(
          p,
          emit_call->as.call.args.data[i].val
              ? emit_call->as.call.args.data[i].val->tok
              : emit_call->tok,
          "comptime template arguments must be literal values or symbols",
          "pass the comptime loop variable, a string/int/bool literal, or a "
          "bare symbol");
      return;
    }
    ct_bind_t bind = {.name = tmpl->params.data[i], .value = val};
    vec_push_arena(p->arena, &ctx.binds, bind);
  }
  if (extra_binds) {
    for (size_t i = 0; i < extra_binds->len; i++)
      vec_push_arena(p->arena, &ctx.binds, extra_binds->data[i]);
  }

  for (size_t i = 0; i < tmpl->body.len; i++) {
    stmt_t *expanded = ct_clone_stmt(p, tmpl->body.data[i], &ctx);
    if (expanded)
      stmt_list_push_flat(p, &block->as.block.body, expanded);
  }
}

static void ct_expand_template_call(parser_t *p, stmt_t *block,
                                    expr_t *emit_call, const char *loop_var,
                                    const ct_value_t *loop_value) {
  ct_expand_template_call_ex(p, block, emit_call, loop_var, loop_value, NULL,
                             NULL);
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
  expr_t *call = p_parse_expr(p, 0);
  parser_match(p, NY_T_SEMI);
  stmt_t *block = stmt_new_transparent_block(p, tok);
  ct_expand_template_call(p, block, call, NULL, NULL);
  return block;
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

  char *prev_mod = p->current_module;
  p->current_module = (char *)mod_name;
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
  p->current_module = prev_mod;
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
  if (p->cur.kind != NY_T_COMPTIME) {
    *p = saved;
    return NULL;
  }
  *handled = true;
  parser_advance(p);
  expr_t *iterable = p_parse_expr(p, 0);
  parser_expect(p, NY_T_LBRACE, "'{' after comptime for iterable", NULL);

  ny_expr_list emits = {0};
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (!tok_is_ident_text(p->cur, "emit")) {
      parser_error(p, p->cur, "expected 'emit' in comptime template block",
                   "write emit template_name(arg)");
      parser_sync_stmt_boundary(p);
      continue;
    }
    parser_advance(p);
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
      parser_error(p, p->cur,
                   "expected string or '<header>' after '#include'", NULL);
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
                 "valid platform guards include #linux, #unix, #windows, "
                 "#macos, #x86, #x86_64, #arm, #aarch64");
    return NULL;
  }
  parser_advance(p);
  if (p->cur.kind != NY_T_STRING) {
    parser_error(p, p->cur, "expected library name string after '#link'",
                 NULL);
    return NULL;
  }
  stmt_t *s = stmt_new(p->arena, NY_S_LINK, tok);
  s->as.link.lib = arena_strndup(p->arena, p->cur.lexeme + 1, p->cur.len - 2);
  parser_advance(p);
  return s;
}
