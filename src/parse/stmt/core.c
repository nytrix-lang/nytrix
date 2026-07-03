static char *parse_dotted_ident_owned(parser_t *p, const char *first_err,
                                      const char *after_dot_err) {
  if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER) {
    parser_error(p, p->cur, first_err ? first_err : "expected identifier",
                 NULL);
    return NULL;
  }
  size_t cap = (size_t)p->cur.len + 32;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memcpy(buf, p->cur.lexeme, p->cur.len);
  len += p->cur.len;
  parser_advance(p);

  while (p->cur.kind == NY_T_IDENT &&
         p->cur.col == (int)(p->prev.col + p->prev.len)) {
    if (len + p->cur.len + 1 > cap) {
      cap = (len + p->cur.len + 1) * 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      buf = nb;
    }
    memcpy(buf + len, p->cur.lexeme, p->cur.len);
    len += p->cur.len;
    parser_advance(p);
  }

  while (parser_match(p, NY_T_DOT)) {

    if (p->cur.kind != NY_T_IDENT && p->cur.kind != NY_T_NUMBER) {
      parser_error(p, p->cur,
                   after_dot_err ? after_dot_err
                                 : "expected identifier after '.'",
                   NULL);
      free(buf);
      return NULL;
    }
    if (len + 1 + p->cur.len + 1 > cap) {
      cap = (len + 1 + p->cur.len + 1) * 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      buf = nb;
    }
    buf[len++] = '.';
    memcpy(buf + len, p->cur.lexeme, p->cur.len);
    len += p->cur.len;
    parser_advance(p);

    while (p->cur.kind == NY_T_IDENT &&
           p->cur.col == (int)(p->prev.col + p->prev.len)) {
      if (len + p->cur.len + 1 > cap) {
        cap = (len + p->cur.len + 1) * 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          free(buf);
          fprintf(stderr, "oom\n");
          exit(1);
        }
        buf = nb;
      }
      memcpy(buf + len, p->cur.lexeme, p->cur.len);
      len += p->cur.len;
      parser_advance(p);
    }
  }
  buf[len] = '\0';
  return buf;
}

static bool parser_token_starts_file_level_decl(token_t tok) {
  switch (tok.kind) {
  case NY_T_FN:
  case NY_T_USE:
  case NY_T_EXTERN:
  case NY_T_MODULE:
  case NY_T_STRUCT:
  case NY_T_ENUM:
  case NY_T_COMPTIME:
  case NY_T_AT:
    return true;
  default:
    return false;
  }
}

static bool parser_block_allows_file_level_decls(parser_t *p, token_t lbrace) {
  if (!p || !p->src || !lbrace.lexeme)
    return false;
  const char *line = lbrace.lexeme;
  while (line > p->src && line[-1] != '\n' && line[-1] != '\r')
    line--;
  while (*line == ' ' || *line == '\t')
    line++;
  if (*line == '#')
    return true;
  return line + 6 <= lbrace.lexeme && memcmp(line, "module", 6) == 0 &&
         (line[6] == ' ' || line[6] == '\t' || line[6] == '(');
}

static void parser_report_missing_rbrace_once(parser_t *p, token_t at,
                                              const char *msg,
                                              int opened_line) {
  if (p->last_error_line == at.line && p->last_error_col == at.col &&
      strcmp(p->last_error_msg, msg) == 0)
    return;
  char hint[192];
  if (at.kind == NY_T_EOF) {
    snprintf(hint, sizeof(hint),
             "check for missing ';' or unmatched brace; close the block opened "
             "at line %d",
             opened_line);
  } else {
    snprintf(hint, sizeof(hint),
             "close the block opened at line %d before starting this top-level "
             "declaration",
             opened_line);
  }
  parser_error(p, at, msg, hint);
}

static void stmt_list_push_flat(parser_t *p, ny_stmt_list *out, stmt_t *s);
static bool stmt_token_is_assign_op(token_t tok);
static bool stmt_assign_op_is_plain(token_t tok);
static token_kind stmt_assign_op_binary_kind(token_t tok);

static expr_t *parse_assignment_rhs_expr(parser_t *p) {
  bool prev_stop = p->stop_expr_at_newline;
  p->stop_expr_at_newline = true;
  expr_t *rhs = p_parse_expr(p, 0);
  p->stop_expr_at_newline = prev_stop;
  return rhs;
}

static stmt_t *parse_leading_deref_assign_stmt(parser_t *p) {
  if (!p || p->cur.kind != NY_T_STAR || parser_peek(p).kind != NY_T_IDENT)
    return NULL;
  parser_t save = *p;
  token_t first = p->cur;
  parser_advance(p);
  token_t ident_tok = p->cur;
  expr_t *target = expr_new(p->arena, NY_E_IDENT, ident_tok);
  target->as.ident.name =
      arena_strndup(p->arena, ident_tok.lexeme, ident_tok.len);
  target->as.ident.sym_id = ident_tok.sym_id;
  parser_advance(p);
  if (!stmt_token_is_assign_op(p->cur)) {
    *p = save;
    return NULL;
  }
  token_t assign_tok = p->cur;
  parser_advance(p);
  expr_t *rhs = parse_assignment_rhs_expr(p);
  parser_match(p, NY_T_SEMI);
  if (!rhs) {
    parser_error(p, assign_tok, "expected expression after dereference assignment",
                 NULL);
    return NULL;
  }
  if (!stmt_assign_op_is_plain(assign_tok)) {
    token_kind bin_kind = stmt_assign_op_binary_kind(assign_tok);
    token_t op_tok = {0};
    expr_t *left = expr_new(p->arena, NY_E_DEREF, first);
    left->as.deref.target = target;
    expr_t *bin = expr_new(p->arena, NY_E_BINARY, op_tok);
    bin->as.binary.op = parser_token_name(bin_kind);
    bin->as.binary.left = left;
    bin->as.binary.right = rhs;
    rhs = bin;
  }
  expr_t *callee = expr_new(p->arena, NY_E_IDENT, first);
  callee->as.ident.name = arena_strndup(p->arena, "store64_i", 9);
  callee->as.ident.sym_id = ny_intern_str("store64_i", 9);
  expr_t *call = expr_new(p->arena, NY_E_CALL, first);
  call->as.call.callee = callee;
  vec_push_arena(p->arena, &call->as.call.args,
                 ((call_arg_t){NULL, target}));
  vec_push_arena(p->arena, &call->as.call.args, ((call_arg_t){NULL, rhs}));
  stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
  s->as.expr.expr = call;
  return s;
}

static void parse_stmt_append_or_sync(parser_t *p, ny_stmt_list *out) {
  token_t before = p->cur;
  parser_t saved = *p;
  stmt_t *s = parse_leading_deref_assign_stmt(p);
  if (!s)
    s = p_parse_stmt(p);
  if (before.kind == NY_T_STAR && stmt_token_is_assign_op(p->cur)) {
    *p = saved;
    s = parse_leading_deref_assign_stmt(p);
  }
  if (s) {
    stmt_list_push_flat(p, out, s);
  } else if (p->had_error) {
    parser_sync_stmt_boundary(p);
    if (p->block_depth == 0 && p->cur.kind == NY_T_RBRACE)
      parser_advance(p);
  }
  if (p->cur.kind == before.kind && p->cur.lexeme == before.lexeme &&
      p->cur.len == before.len && p->cur.kind != NY_T_EOF &&
      p->cur.kind != NY_T_RBRACE) {
    parser_advance(p);
  }
}

static stmt_t *stmt_new_transparent_block(parser_t *p, token_t tok) {
  stmt_t *block = stmt_new(p->arena, NY_S_BLOCK, tok);
  block->as.block.transparent = true;
  return block;
}

static void stmt_list_push_flat(parser_t *p, ny_stmt_list *out, stmt_t *s) {
  if (!s)
    return;
  if (s->kind == NY_S_BLOCK && s->as.block.transparent) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      stmt_list_push_flat(p, out, s->as.block.body.data[i]);
    return;
  }
  vec_push_arena(p->arena, out, s);
}

static bool stmt_lbrace_starts_dict_literal(parser_t *p) {
  if (!p || p->cur.kind != NY_T_LBRACE)
    return false;
  parser_t scan = *p;
  parser_advance(&scan);
  if (scan.cur.kind != NY_T_STRING && scan.cur.kind != NY_T_NUMBER)
    return false;
  int paren = 0;
  int bracket = 0;
  int brace = 0;
  while (scan.cur.kind != NY_T_EOF) {
    token_kind k = scan.cur.kind;
    if (paren == 0 && bracket == 0 && brace == 0) {
      if (k == NY_T_COLON)
        return true;
      if (k == NY_T_COMMA || k == NY_T_RBRACE)
        return false;
    }
    if (k == NY_T_LPAREN)
      paren++;
    else if (k == NY_T_RPAREN)
      paren--;
    else if (k == NY_T_LBRACK)
      bracket++;
    else if (k == NY_T_RBRACK)
      bracket--;
    else if (k == NY_T_LBRACE)
      brace++;
    else if (k == NY_T_RBRACE) {
      if (brace == 0)
        return false;
      brace--;
    }
    parser_advance(&scan);
  }
  return false;
}

static bool stmt_token_looks_type_name(token_t tok) {
  if (tok.kind != NY_T_IDENT || !tok.lexeme || tok.len == 0)
    return false;
  unsigned char ch = (unsigned char)tok.lexeme[0];
  return parser_token_is_builtin_type(tok) || isupper(ch) || ch == '_';
}

static bool stmt_token_can_follow_decl_name(token_kind kind) {
  return kind == NY_T_ASSIGN || kind == NY_T_COMMA || kind == NY_T_SEMI ||
         kind == NY_T_RBRACE || kind == NY_T_EOF;
}

static bool stmt_looks_type_first_binding(parser_t *p) {
  if (!p || !parser_token_can_start_type_ref(p->cur))
    return false;

  if (p->cur.kind == NY_T_IDENT) {
    token_kind next = parser_peek(p).kind;
    if (next != NY_T_IDENT && next != NY_T_LT && next != NY_T_DOT)
      return false;
  }

  parser_t probe = *p;
  probe.quiet = true;
  const char *type = parse_type_ref(&probe, NULL);
  if (!type || probe.cur.kind != NY_T_IDENT)
    return false;

  parser_advance(&probe);
  return stmt_token_can_follow_decl_name(probe.cur.kind);
}

static bool stmt_ident_lbrace_starts_named_fields(parser_t *p) {
  if (!p || p->cur.kind != NY_T_IDENT)
    return false;
  parser_t scan = *p;
  parser_advance(&scan);
  if (scan.cur.kind != NY_T_LBRACE)
    return false;
  parser_advance(&scan);
  if (scan.cur.kind != NY_T_IDENT && scan.cur.kind != NY_T_STRING)
    return false;
  parser_advance(&scan);
  return scan.cur.kind == NY_T_COLON;
}

static void stmt_skip_named_field_literal(parser_t *p) {
  if (!p || p->cur.kind != NY_T_IDENT)
    return;
  parser_advance(p);
  if (p->cur.kind != NY_T_LBRACE)
    return;
  int depth = 0;
  do {
    if (p->cur.kind == NY_T_LBRACE) {
      depth++;
    } else if (p->cur.kind == NY_T_RBRACE) {
      depth--;
      parser_advance(p);
      if (depth <= 0)
        return;
      continue;
    }
    parser_advance(p);
  } while (p->cur.kind != NY_T_EOF);
}

static expr_t *make_ident_expr(parser_t *p, token_t tok, const char *name) {
  if (name) {
    tok.lexeme = name;
    tok.len = strlen(name);
  }
  expr_t *e = expr_new(p->arena, NY_E_IDENT, tok);
  e->as.ident.name = name;
  e->as.ident.sym_id = name ? ny_intern_str(name, strlen(name)) : 0;
  e->as.ident.hash = name ? ny_hash64_cstr(name) : 0;
  return e;
}

static expr_t *make_call_expr(parser_t *p, token_t tok, const char *callee_name,
                              const char *arg_name) {
  expr_t *callee = make_ident_expr(p, tok, callee_name);
  expr_t *arg = make_ident_expr(p, tok, arg_name);
  expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
  call->as.call.callee = callee;
  vec_push_arena(p->arena, &call->as.call.args,
                 ((call_arg_t){.name = NULL, .val = arg}));
  return call;
}

static expr_t *make_zero_arg_call_expr(parser_t *p, token_t tok,
                                       const char *callee_name) {
  expr_t *callee = make_ident_expr(p, tok, callee_name);
  expr_t *call = expr_new(p->arena, NY_E_CALL, tok);
  call->as.call.callee = callee;
  return call;
}

static stmt_t *make_expr_stmt(parser_t *p, token_t tok, expr_t *e) {
  stmt_t *s = stmt_new(p->arena, NY_S_EXPR, tok);
  s->as.expr.expr = e;
  return s;
}

static stmt_t *make_call_block(parser_t *p, token_t tok,
                               const char *callee_name, const char *arg_name) {
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(
      p->arena, &blk->as.block.body,
      make_expr_stmt(p, tok, make_call_expr(p, tok, callee_name, arg_name)));
  return blk;
}

static bool resource_type_uses_free(const char *type_name) {
  if (!type_name)
    return false;
  while (*type_name == '?')
    type_name++;
  return strcmp(type_name, "ptr") == 0 || *type_name == '*';
}

static stmt_t *make_resource_cleanup(parser_t *p, token_t tok,
                                     const char *type_name, const char *name) {
  if (!resource_type_uses_free(type_name))
    return make_call_block(p, tok, "close", name);

  stmt_t *if_stmt = stmt_new(p->arena, NY_S_IF, tok);
  if_stmt->as.iff.test = make_ident_expr(p, tok, name);
  if_stmt->as.iff.conseq = make_call_block(p, tok, "free", name);
  return if_stmt;
}

static const char *parse_missing_ident_hint(token_t tok, const char *what) {
  static char buf[192];
  if (tok.kind == NY_T_SEMI) {
    snprintf(buf, sizeof(buf),
             "';' starts a line comment here; all text after it on this line "
             "is ignored. Put the "
             "%s name before ';' or on its own line",
             what ? what : "binding");
    return buf;
  }
  if (tok.kind == NY_T_ASSIGN) {
    snprintf(buf, sizeof(buf), "did you forget the %s name before '='?",
             what ? what : "binding");
    return buf;
  }
  switch (tok.kind) {
  case NY_T_FN:
  case NY_T_IF:
  case NY_T_ELSE:
  case NY_T_WHILE:
  case NY_T_FOR:
  case NY_T_RETURN:
  case NY_T_BREAK:
  case NY_T_CONTINUE:
  case NY_T_MATCH:
  case NY_T_MUT:
  case NY_T_DEF:
  case NY_T_TRY:
  case NY_T_CATCH:
  case NY_T_USE:
  case NY_T_EXTERN:
  case NY_T_MODULE:
  case NY_T_DEFER:
    snprintf(buf, sizeof(buf),
             "'%s' is a keyword and cannot be used as a %s name",
             parser_token_name(tok.kind), what ? what : "binding");
    return buf;
  default:
    return NULL;
  }
}

static char *parse_qualified_name(parser_t *p) {
  char *owned = parse_dotted_ident_owned(p, "expected identifier",
                                         "expected identifier after '.'");
  if (!owned)
    return NULL;
  size_t result_len = strlen(owned);
  if (p->current_module) {
    size_t clen = strlen(p->current_module);
    if (!(result_len >= clen && strncmp(p->current_module, owned, clen) == 0 &&
          (owned[clen] == '.' || owned[clen] == '\0'))) {
      char *prefixed = malloc(clen + 1 + result_len + 1);
      if (!prefixed) {
        free(owned);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      memcpy(prefixed, p->current_module, clen);
      prefixed[clen] = '.';
      memcpy(prefixed + clen + 1, owned, result_len + 1);
      free(owned);
      owned = prefixed;
      result_len = clen + 1 + result_len;
    }
  }
  const char *name = parser_intern(p, owned, result_len);
  free(owned);
  return (char *)name;
}

static const char *parse_type_ref(parser_t *p, const char *err_msg) {
  size_t nullable_depth = 0;
  while (parser_match(p, NY_T_QUESTION))
    nullable_depth++;
  size_t ptr_depth = 0;
  while (parser_match(p, NY_T_STAR))
    ptr_depth++;
  char *base =
      parse_dotted_ident_owned(p, err_msg ? err_msg : "expected type name",
                               "expected identifier after '.' in type");
  if (!base)
    return NULL;
  if (p->current_impl_owner && strcmp(base, "self") == 0) {
    size_t owner_len = strlen(p->current_impl_owner);
    char *owner = malloc(owner_len + 1);
    if (!owner) {
      free(base);
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(owner, p->current_impl_owner, owner_len + 1);
    free(base);
    base = owner;
  }
  if (parser_match(p, NY_T_LT)) {
    size_t cap = strlen(base) + 32;
    size_t len = strlen(base);
    char *generic = malloc(cap);
    if (!generic) {
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(generic, base, len);
    generic[len++] = '<';
    free(base);
    bool first = true;
    while (p->cur.kind != NY_T_GT && p->cur.kind != NY_T_RSHIFT &&
           p->cur.kind != NY_T_EOF) {
      const char *arg = parse_type_ref(p, "expected generic type argument");
      if (!arg)
        break;
      size_t arg_len = strlen(arg);
      size_t need = len + (first ? 0 : 2) + arg_len + 2;
      if (need > cap) {
        while (cap < need)
          cap *= 2;
        char *nb = realloc(generic, cap);
        if (!nb) {
          free(generic);
          fprintf(stderr, "oom\n");
          exit(1);
        }
        generic = nb;
      }
      if (!first) {
        generic[len++] = ',';
        generic[len++] = ' ';
      }
      memcpy(generic + len, arg, arg_len);
      len += arg_len;
      first = false;
      if (!parser_match(p, NY_T_COMMA))
        break;
    }
    if (parser_match(p, NY_T_GT)) {

    } else if (p->cur.kind == NY_T_RSHIFT) {
      token_t tok = p->cur;
      p->cur.kind = NY_T_GT;
      p->cur.lexeme = tok.lexeme + 1;
      p->cur.len = 1;
      p->cur.col = tok.col + 1;
    } else {
      parser_error(p, p->cur, "'>' after generic type arguments", NULL);
    }
    if (len + 2 > cap) {
      char *nb = realloc(generic, len + 2);
      if (!nb) {
        free(generic);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      generic = nb;
    }
    generic[len++] = '>';
    generic[len] = '\0';
    base = generic;
  }
  size_t len = strlen(base);
  size_t total = nullable_depth + ptr_depth + len;
  char *out = arena_alloc(p->arena, total + 1);
  size_t at = 0;
  for (size_t i = 0; i < nullable_depth; i++)
    out[at++] = '?';
  for (size_t i = 0; i < ptr_depth; i++)
    out[at++] = '*';
  memcpy(out + at, base, len);
  out[total] = '\0';
  free(base);
  return out;
}

static stmt_t *parse_resource_block_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_advance(p);

  const char *var_type =
      parse_type_ref(p, "expected resource type after 'with'");
  if (!var_type) {
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  if (parser_match(p, NY_T_COLON)) {
    parser_error(p, p->prev, "legacy ':' in resource block",
                 "write 'with Type name = value { ... }'");
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected resource binding name after resource type",
                 "write 'with Type name = value { ... }'");
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  if (parser_token_is_builtin_type(p->cur)) {
    parser_error(p, p->cur, "resource bindings are type-first",
                 "write 'with ptr buf = ...', not 'with buf ptr = ...'");
    parser_sync_stmt_boundary(p);
    return NULL;
  }

  token_t name_tok = p->cur;
  const char *name = arena_strndup(p->arena, name_tok.lexeme, name_tok.len);
  parser_advance(p);

  if (!parser_match(p, NY_T_ASSIGN)) {
    parser_error(p, p->cur, "'=' after resource binding",
                 "resource blocks bind one value before the cleanup body");
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  expr_t *init = p_parse_expr(p, 0);
  if (!init) {
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  if (p->cur.kind != NY_T_LBRACE) {
    parser_error(p, p->cur, "expected resource block body",
                 "write 'with Type name = value { ... }'");
    parser_sync_stmt_boundary(p);
    return NULL;
  }

  stmt_t *var = stmt_new(p->arena, NY_S_VAR, tok);
  var->as.var.is_decl = true;
  var->as.var.is_mut = false;
  var->as.var.is_del = false;
  var->as.var.is_destructure = false;
  vec_push_arena(p->arena, &var->as.var.names, name);
  vec_push_arena(p->arena, &var->as.var.types, var_type);
  vec_push_arena(p->arena, &var->as.var.exprs, init);

  stmt_t *defer = stmt_new(p->arena, NY_S_DEFER, tok);
  stmt_t *defer_body = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &defer_body->as.block.body,
                 make_resource_cleanup(p, tok, var_type, name));
  defer->as.de.body = defer_body;

  stmt_t *body = p_parse_block(p);

  stmt_t *outer = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &outer->as.block.body, var);
  vec_push_arena(p->arena, &outer->as.block.body, defer);
  vec_push_arena(p->arena, &outer->as.block.body, body);
  return outer;
}

static bool parse_param_type_first(parser_t *p, param_t *pr) {
  return parser_parse_param_type_first(p, pr, parse_type_ref);
}

static bool parse_operator_token(token_kind kind) {
  switch (kind) {
  case NY_T_PLUS:
  case NY_T_MINUS:
  case NY_T_STAR:
  case NY_T_SLASH:
  case NY_T_PERCENT:
  case NY_T_EQ:
  case NY_T_NEQ:
  case NY_T_LT:
  case NY_T_LE:
  case NY_T_GT:
  case NY_T_GE:
  case NY_T_BITAND:
  case NY_T_BITOR:
  case NY_T_POW:
  case NY_T_BITXOR:
  case NY_T_LSHIFT:
  case NY_T_RSHIFT:
    return true;
  default:
    return false;
  }
}

static bool stmt_token_is_assign_op(token_t tok) {
  if (tok.kind == NY_T_ASSIGN || tok.kind == NY_T_PLUS_EQ ||
      tok.kind == NY_T_MINUS_EQ || tok.kind == NY_T_STAR_EQ ||
      tok.kind == NY_T_SLASH_EQ || tok.kind == NY_T_PERCENT_EQ ||
      tok.kind == NY_T_POW_EQ || tok.kind == NY_T_BITXOR_EQ ||
      tok.kind == NY_T_LSHIFT_EQ || tok.kind == NY_T_RSHIFT_EQ)
    return true;
  return tok.len == 2 && tok.lexeme &&
         ((tok.lexeme[0] == '+' && tok.lexeme[1] == '=') ||
          (tok.lexeme[0] == '-' && tok.lexeme[1] == '=') ||
          (tok.lexeme[0] == '*' && tok.lexeme[1] == '=') ||
          (tok.lexeme[0] == '/' && tok.lexeme[1] == '=') ||
          (tok.lexeme[0] == '%' && tok.lexeme[1] == '=') ||
          (tok.lexeme[0] == '^' && tok.lexeme[1] == '='));
}

static bool stmt_assign_op_is_plain(token_t tok) {
  return tok.kind == NY_T_ASSIGN ||
         (tok.len == 1 && tok.lexeme && tok.lexeme[0] == '=');
}

static token_kind stmt_assign_op_binary_kind(token_t tok) {
  if (tok.kind == NY_T_PLUS_EQ || (tok.len == 2 && tok.lexeme && tok.lexeme[0] == '+'))
    return NY_T_PLUS;
  if (tok.kind == NY_T_MINUS_EQ || (tok.len == 2 && tok.lexeme && tok.lexeme[0] == '-'))
    return NY_T_MINUS;
  if (tok.kind == NY_T_STAR_EQ || (tok.len == 2 && tok.lexeme && tok.lexeme[0] == '*'))
    return NY_T_STAR;
  if (tok.kind == NY_T_SLASH_EQ || (tok.len == 2 && tok.lexeme && tok.lexeme[0] == '/'))
    return NY_T_SLASH;
  if (tok.kind == NY_T_PERCENT_EQ || (tok.len == 2 && tok.lexeme && tok.lexeme[0] == '%'))
    return NY_T_PERCENT;
  if (tok.kind == NY_T_POW_EQ)
    return NY_T_POW;
  if (tok.kind == NY_T_BITXOR_EQ)
    return NY_T_BITXOR;
  if (tok.kind == NY_T_LSHIFT_EQ)
    return NY_T_LSHIFT;
  if (tok.kind == NY_T_RSHIFT_EQ)
    return NY_T_RSHIFT;
  return NY_T_PERCENT;
}

static const char *parse_operator_target_for_owner(parser_t *p,
                                                   const char *owner) {
  char *owned = parse_dotted_ident_owned(
      p, "expected operator target function",
      "expected identifier after '.' in operator target");
  if (!owned)
    return NULL;
  char *final_name = owned;
  if (owner && *owner && !strchr(owned, '.')) {
    const char *owner_prefix = owner;
    char *scoped_owner = NULL;
    if (p->current_module && *p->current_module && !strchr(owner, '.')) {
      size_t mlen = strlen(p->current_module);
      size_t olen = strlen(owner);
      scoped_owner = malloc(mlen + 1 + olen + 1);
      if (!scoped_owner) {
        free(owned);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      memcpy(scoped_owner, p->current_module, mlen);
      scoped_owner[mlen] = '.';
      memcpy(scoped_owner + mlen + 1, owner, olen + 1);
      owner_prefix = scoped_owner;
    }
    size_t olen = strlen(owner_prefix);
    size_t nlen = strlen(owned);
    final_name = malloc(olen + 1 + nlen + 1);
    if (!final_name) {
      free(owned);
      if (scoped_owner)
        free(scoped_owner);
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(final_name, owner_prefix, olen);
    final_name[olen] = '.';
    memcpy(final_name + olen + 1, owned, nlen + 1);
    if (scoped_owner)
      free(scoped_owner);
  } else if (p->current_module && !strchr(owned, '.')) {
    size_t mlen = strlen(p->current_module);
    size_t nlen = strlen(owned);
    final_name = malloc(mlen + 1 + nlen + 1);
    if (!final_name) {
      free(owned);
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(final_name, p->current_module, mlen);
    final_name[mlen] = '.';
    memcpy(final_name + mlen + 1, owned, nlen + 1);
  }
  const char *out = parser_intern(p, final_name, strlen(final_name));
  if (final_name != owned)
    free(final_name);
  free(owned);
  return out;
}

static const char *parse_operator_target(parser_t *p) {
  return parse_operator_target_for_owner(p, NULL);
}

static stmt_t *parse_operator_stmt_with_left(parser_t *p,
                                             const char *left_override,
                                             const char *target_owner) {
  token_t tok = p->cur;
  parser_advance(p);

  const char *left_type = left_override;
  if (!left_type) {
    left_type =
        parse_type_ref(p, "expected left operand type after 'operator'");
    if (!left_type)
      return NULL;
  }
  if (!parse_operator_token(p->cur.kind)) {
    if (left_override) {
      parser_error(p, p->cur, "impl operator omits the left operand type",
                   "write 'operator + Right: Return = target' inside impl");
    } else {
      parser_error(p, p->cur,
                   "expected binary operator in operator declaration",
                   "example: operator vec3 + vec3: vec3 = v_add");
    }
    return NULL;
  }
  const char *op = arena_strndup(p->arena, parser_token_name(p->cur.kind),
                                 strlen(parser_token_name(p->cur.kind)));
  parser_advance(p);
  const char *right_type =
      parse_type_ref(p, "expected right operand type in operator declaration");
  if (!right_type)
    return NULL;
  if (!parser_match(p, NY_T_COLON)) {
    parser_error(p, p->cur, "expected ':' before operator return type",
                 "example: operator vec3 * vec3: f64 = dot3");
    return NULL;
  }
  const char *return_type = parse_type_ref(p, "expected operator return type");
  if (!return_type)
    return NULL;
  if (!parser_match(p, NY_T_ASSIGN)) {
    parser_error(p, p->cur, "expected '=' before operator target function",
                 "example: operator vec3 / f64: vec3 = divs");
    return NULL;
  }
  const char *target = parse_operator_target_for_owner(p, target_owner);
  if (!target)
    return NULL;
  parser_match(p, NY_T_SEMI);

  stmt_t *s = stmt_new(p->arena, NY_S_OPERATOR, tok);
  s->as.oper.op = op;
  s->as.oper.left_type = left_type;
  s->as.oper.right_type = right_type;
  s->as.oper.return_type = return_type;
  s->as.oper.target = target;
  return s;
}

static stmt_t *parse_operator_stmt(parser_t *p) {
  return parse_operator_stmt_with_left(p, NULL, NULL);
}

static bool is_pow2_u64(unsigned long long v) {
  return v && ((v & (v - 1)) == 0);
}

static size_t parse_align_attr(parser_t *p, const char *kind) {
  token_t kw = p->cur;
  parser_advance(p);
  if (!parser_match(p, NY_T_LPAREN)) {
    parser_error(p, kw, "expected '(' after attribute", NULL);
    return 0;
  }
  if (p->cur.kind != NY_T_NUMBER) {
    parser_error(p, p->cur, "expected numeric value for attribute", NULL);
    return 0;
  }
  const char *num = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  unsigned long long val = strtoull(num, NULL, 0);
  if (val == 0) {
    parser_error(p, p->cur, "attribute value must be > 0", NULL);
    val = 1;
  } else if (!is_pow2_u64(val)) {
    parser_error(p, p->cur, "attribute value must be a power of two", NULL);
  }
  parser_advance(p);
  parser_expect(p, NY_T_RPAREN, "')' after attribute", NULL);
  (void)kind;
  return (size_t)val;
}

static stmt_t *parse_func(parser_t *p, ny_attribute_list attrs) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_FN, "'fn'", NULL);
  char *name = parse_qualified_name(p);
  if (!name)
    return NULL;
  parser_expect(p, NY_T_LPAREN, NULL, "'(' ");
  ny_param_list params = {0};
  stmt_t *fn_stmt = stmt_new(p->arena, NY_S_FUNC, tok);
  while (p->cur.kind != NY_T_RPAREN) {
    if (parser_match(p, NY_T_ELLIPSIS)) {
      fn_stmt->as.fn.is_variadic = true;
    }
    param_t pr = {0};
    if (!parse_param_type_first(p, &pr)) {
      stmt_free_members(fn_stmt);
      return NULL;
    }
    if (parser_match(p, NY_T_ASSIGN))
      pr.def = p_parse_expr(p, 0);
    vec_push_arena(p->arena, &params, pr);
    if (fn_stmt->as.fn.is_variadic) {
      if (p->cur.kind == NY_T_COMMA) {
        parser_error(p, p->cur, "variadic parameter must be the last one",
                     NULL);
      }
      break;
    }
    if (!parser_match(p, NY_T_COMMA))
      break;
    if (p->cur.kind == NY_T_RPAREN)
      break;
  }
  parser_expect(p, NY_T_RPAREN, NULL, NULL);
  if (parser_match(p, NY_T_ARROW)) {
    parser_error(p, p->prev, "function return types do not use '->'",
                 "write 'fn name(params) RetType { ... }'");
    if (p->cur.kind != NY_T_LBRACE && p->cur.kind != NY_T_ASSIGN &&
        p->cur.kind != NY_T_EOF)
      (void)parse_type_ref(p, "expected return type after '->'");
  }
  if (parser_match(p, NY_T_COLON)) {
    parser_error(p, p->prev, "old function return separator",
                 "write 'fn name(params) RetType { ... }', without ':'");
    if (p->cur.kind != NY_T_LBRACE && p->cur.kind != NY_T_ASSIGN &&
        p->cur.kind != NY_T_SEMI && p->cur.kind != NY_T_EOF)
      (void)parse_type_ref(p, "expected return type");
  } else
    fn_stmt->as.fn.return_type =
        parser_parse_return_type_suffix(p, parse_type_ref, "expected return type");
  if (p->cur.kind == NY_T_ARROW) {
    parser_error(p, p->cur, "function return types do not use '->'",
                 "write 'fn name(params) RetType { ... }'");
    parser_advance(p);
    parse_type_ref(p, NULL);
  }
  bool is_ext = false;
  const char *link_name = NULL;
  for (size_t i = 0; i < attrs.len; i++) {
    if (strcmp(attrs.data[i].name, "extern") == 0) {
      is_ext = true;
      if (attrs.data[i].args.len >= 1) {
        expr_t *arg = attrs.data[i].args.data[0];
        if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_STR) {
          link_name = arena_strndup(p->arena, arg->as.literal.as.s.data,
                                    arg->as.literal.as.s.len);
        }
      }
      break;
    }
  }
  fn_stmt->as.fn.is_extern = is_ext;
  fn_stmt->as.fn.link_name = link_name;
  if (parser_match(p, NY_T_ASSIGN)) {
    token_t body_tok = p->prev;
    expr_t *e = p_parse_expr(p, 0);
    parser_match(p, NY_T_SEMI);
    stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, body_tok);
    vec_reserve_arena(p->arena, &blk->as.block.body, 1);
    if (e) {
      stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, body_tok);
      ret->as.ret.value = e;
      vec_push_arena(p->arena, &blk->as.block.body, ret);
    }
    stmt_t *s = fn_stmt;
    s->as.fn.name = name;
    s->as.fn.params = params;
    s->as.fn.body = blk;
    s->as.fn.doc = NULL;
    s->as.fn.src_start = tok.lexeme;
    s->as.fn.src_end = p->prev.lexeme + p->prev.len;
    return s;
  }
  if (parser_match(p, NY_T_SEMI) || (is_ext && p->cur.kind != NY_T_LBRACE)) {
    stmt_t *s = fn_stmt;
    s->as.fn.name = name;
    s->as.fn.params = params;
    s->as.fn.body = NULL;
    s->as.fn.doc = NULL;
    s->as.fn.src_start = tok.lexeme;
    s->as.fn.src_end = p->prev.lexeme + p->prev.len;
    return s;
  }
  stmt_t *body = NULL;
  if (p->cur.kind == NY_T_LBRACE) {
    body = p_parse_block(p);
  } else {

    if (p->cur.kind == NY_T_EOF || p->cur.line > p->prev.line) {
      parser_error(p, p->cur, "expected function body",
                   "use '{ ... }', '= expr', or ';' for declarations");
      stmt_free_members(fn_stmt);
      return NULL;
    }
    token_t body_tok = p->cur;
    expr_t *e = p_parse_expr(p, 0);
    stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, body_tok);
    vec_reserve_arena(p->arena, &blk->as.block.body, 2);
    if (e) {
      stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, body_tok);
      ret->as.ret.value = e;
      vec_push_arena(p->arena, &blk->as.block.body, ret);
    }
    body = blk;
  }
  const char *doc = NULL;
  if (body->as.block.body.len > 1) {
    stmt_t *s0 = body->as.block.body.data[0];
    if (s0->kind == NY_S_EXPR && s0->as.expr.expr->kind == NY_E_LITERAL &&
        s0->as.expr.expr->as.literal.kind == NY_LIT_STR) {
      doc = arena_strndup(p->arena, s0->as.expr.expr->as.literal.as.s.data,
                          s0->as.expr.expr->as.literal.as.s.len);
      memmove(body->as.block.body.data, body->as.block.body.data + 1,
              (body->as.block.body.len - 1) * sizeof(stmt_t *));
      body->as.block.body.len -= 1;
    }
  }
  stmt_t *s = fn_stmt;
  s->as.fn.name = name;
  s->as.fn.params = params;
  s->as.fn.body = body;
  s->as.fn.doc = doc;
  s->as.fn.src_start = tok.lexeme;
  s->as.fn.src_end = p->prev.lexeme + p->prev.len;
  return s;
}

static bool attached_type_is_builtin(const char *name) {
  if (!name || !*name)
    return false;
  while (*name == '?' || *name == '*')
    name++;
  const char *tail = strrchr(name, '.');
  tail = tail ? tail + 1 : name;
#define NY_ATTACH_TYPE_EQ(lit) (strcmp(tail, lit) == 0)
  return NY_ATTACH_TYPE_EQ("int") || NY_ATTACH_TYPE_EQ("i8") ||
         NY_ATTACH_TYPE_EQ("i16") || NY_ATTACH_TYPE_EQ("i32") ||
         NY_ATTACH_TYPE_EQ("i64") || NY_ATTACH_TYPE_EQ("i128") ||
         NY_ATTACH_TYPE_EQ("u8") || NY_ATTACH_TYPE_EQ("u16") ||
         NY_ATTACH_TYPE_EQ("u32") || NY_ATTACH_TYPE_EQ("u64") ||
         NY_ATTACH_TYPE_EQ("u128") || NY_ATTACH_TYPE_EQ("str") ||
         NY_ATTACH_TYPE_EQ("char") || NY_ATTACH_TYPE_EQ("bool") ||
         NY_ATTACH_TYPE_EQ("f32") || NY_ATTACH_TYPE_EQ("f64") ||
         NY_ATTACH_TYPE_EQ("f128") || NY_ATTACH_TYPE_EQ("ptr") ||
         NY_ATTACH_TYPE_EQ("handle") || NY_ATTACH_TYPE_EQ("fnptr") ||
         NY_ATTACH_TYPE_EQ("seq") || NY_ATTACH_TYPE_EQ("sequence") ||
         NY_ATTACH_TYPE_EQ("number") || NY_ATTACH_TYPE_EQ("numeric") ||
         NY_ATTACH_TYPE_EQ("integer") || NY_ATTACH_TYPE_EQ("float") ||
         NY_ATTACH_TYPE_EQ("scalar") || NY_ATTACH_TYPE_EQ("collection") ||
         NY_ATTACH_TYPE_EQ("container") || NY_ATTACH_TYPE_EQ("iterable") ||
         NY_ATTACH_TYPE_EQ("indexable") || NY_ATTACH_TYPE_EQ("allocator") ||
         NY_ATTACH_TYPE_EQ("vec2") || NY_ATTACH_TYPE_EQ("vec3") ||
         NY_ATTACH_TYPE_EQ("vec4") || NY_ATTACH_TYPE_EQ("list") ||
         NY_ATTACH_TYPE_EQ("tuple") || NY_ATTACH_TYPE_EQ("dict") ||
         NY_ATTACH_TYPE_EQ("set") || NY_ATTACH_TYPE_EQ("bytes") ||
         NY_ATTACH_TYPE_EQ("range") || NY_ATTACH_TYPE_EQ("bigint");
#undef NY_ATTACH_TYPE_EQ
}

static const char *attached_owner_name(parser_t *p, const char *type_name) {
  if (!type_name)
    return NULL;
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  if (!*type_name)
    return NULL;
  if (p->current_module && !strchr(type_name, '.') &&
      !attached_type_is_builtin(type_name)) {
    size_t mlen = strlen(p->current_module);
    size_t tlen = strlen(type_name);
    char *buf = arena_alloc(p->arena, mlen + 1 + tlen + 1);
    memcpy(buf, p->current_module, mlen);
    buf[mlen] = '.';
    memcpy(buf + mlen + 1, type_name, tlen + 1);
    return buf;
  }
  return arena_strndup(p->arena, type_name, strlen(type_name));
}

static void qualify_attached_method(parser_t *p, const char *owner,
                                    stmt_t *method) {
  if (!owner || !method || method->kind != NY_S_FUNC || !method->as.fn.name)
    return;
  const char *name = method->as.fn.name;
  size_t owner_len = strlen(owner);
  if (strncmp(name, owner, owner_len) == 0 && name[owner_len] == '.')
    return;
  const char *tail = strrchr(name, '.');
  tail = tail ? tail + 1 : name;
  size_t tail_len = strlen(tail);
  char *qualified = arena_alloc(p->arena, owner_len + 1 + tail_len + 1);
  memcpy(qualified, owner, owner_len);
  qualified[owner_len] = '.';
  memcpy(qualified + owner_len + 1, tail, tail_len + 1);
  method->as.fn.name = qualified;
}

static stmt_t *parse_attached_method(parser_t *p, const char *owner) {
  ny_attribute_list attrs = {0};
  while (p->cur.kind == NY_T_AT) {
    parser_advance(p);
    attribute_t attr = parse_attr(p);
    if (!attr.name) {
      parser_error(p, p->cur, "expected attribute name", NULL);
      return NULL;
    }
    vec_push_arena(p->arena, &attrs, attr);
  }
  if (p->cur.kind != NY_T_FN) {
    parser_error(p, p->cur, "type attachment expects 'fn'", NULL);
    return NULL;
  }
  char *saved_module = p->current_module;
  p->current_module = NULL;
  stmt_t *method = parse_func(p, attrs);
  p->current_module = saved_module;
  if (method && method->kind == NY_S_FUNC) {
    method->attributes = attrs;
    qualify_attached_method(p, owner, method);
  }
  return method;
}

static stmt_t *parse_impl_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_advance(p);
  ny_type_param_list owners = {0};
  const char *type_name = parse_type_ref(p, "expected type name after 'impl'");
  const char *owner = attached_owner_name(p, type_name);
  if (!type_name || !owner)
    return NULL;
  vec_push_arena(p->arena, &owners, owner);
  while (parser_match(p, NY_T_COMMA)) {
    const char *next_type =
        parse_type_ref(p, "expected type name after ',' in impl list");
    const char *next_owner = attached_owner_name(p, next_type);
    if (!next_type || !next_owner)
      return NULL;
    vec_push_arena(p->arena, &owners, next_owner);
  }
  parser_expect(p, NY_T_LBRACE, "'{' after impl type", NULL);
  stmt_t *s = stmt_new(p->arena, NY_S_IMPL, tok);
  s->as.impl.type_name = owner;
  const char *prev_impl_owner = p->current_impl_owner;
  p->current_impl_owner = owner;
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    bool is_operator = (p->cur.kind == NY_T_IDENT && p->cur.len == 8 &&
                        strncmp(p->cur.lexeme, "operator", 8) == 0);
    if (is_operator) {
      stmt_t *oper = parse_operator_stmt_with_left(p, owner, owner);
      if (oper)
        vec_push_arena(p->arena, &s->as.impl.methods, oper);
      parser_match(p, NY_T_COMMA);
      parser_match(p, NY_T_SEMI);
      continue;
    }
    if (p->cur.kind != NY_T_FN && p->cur.kind != NY_T_AT) {
      parser_error(p, p->cur,
                   "impl blocks only accept attached functions or operators",
                   "example: impl Vec3 { fn len(self value) f64 { ... } "
                   "operator + self: self = add }");
      parser_sync_stmt_boundary(p);
      if (p->cur.kind == NY_T_RBRACE)
        break;
      continue;
    }
    stmt_t *method = parse_attached_method(p, owner);
    if (method)
      vec_push_arena(p->arena, &s->as.impl.methods, method);
    parser_match(p, NY_T_COMMA);
    parser_match(p, NY_T_SEMI);
  }
  p->current_impl_owner = prev_impl_owner;
  parser_expect(p, NY_T_RBRACE, "'}' after impl block", NULL);
  if (owners.len > 1) {
    stmt_t *block = stmt_new_transparent_block(p, tok);
    vec_push_arena(p->arena, &block->as.block.body, s);
    for (size_t i = 1; i < owners.len; i++) {
      stmt_t *copy = impl_clone_for_owner(p, s, owner, owners.data[i]);
      if (copy)
        vec_push_arena(p->arena, &block->as.block.body, copy);
    }
    return block;
  }
  return s;
}

static const char *extern_default_symbol_name(parser_t *p, const char *name) {
  if (!name)
    return NULL;
  const char *tail = strrchr(name, '.');
  tail = tail ? tail + 1 : name;
  return arena_strndup(p->arena, tail, strlen(tail));
}

static stmt_t *parse_extern_fn_decl(parser_t *p, token_t tok,
                                    bool default_link_name) {
  parser_expect(p, NY_T_FN, "'fn'", NULL);
  char *name = parse_qualified_name(p);
  if (!name)
    return NULL;
  parser_expect(p, NY_T_LPAREN, NULL, "'(' ");
  ny_param_list params = {0};
  bool is_variadic = false;
  while (p->cur.kind != NY_T_RPAREN) {
    if (parser_match(p, NY_T_ELLIPSIS)) {
      is_variadic = true;
      break;
    }
    param_t pr = {0};
    if (!parse_param_type_first(p, &pr)) {
      return NULL;
    }
    vec_push_arena(p->arena, &params, pr);
    if (is_variadic)
      break;
    if (!parser_match(p, NY_T_COMMA))
      break;
    if (p->cur.kind == NY_T_RPAREN)
      break;
  }
  parser_expect(p, NY_T_RPAREN, NULL, NULL);
  const char *return_type = NULL;
  if (parser_match(p, NY_T_COLON)) {
    parser_error(p, p->prev, "old extern return separator",
                 "write 'fn name(params) RetType', without ':'");
    if (p->cur.kind != NY_T_AS && p->cur.kind != NY_T_SEMI && p->cur.kind != NY_T_EOF)
      (void)parse_type_ref(p, "expected return type");
  } else
    return_type = parser_parse_return_type_suffix(p, parse_type_ref, "expected return type");
  const char *link_name = NULL;
  if (parser_match(p, NY_T_AS)) {
    if (p->cur.kind == NY_T_STRING) {
      size_t len = 0;
      link_name = parser_decode_string(p, p->cur, &len);
      parser_advance(p);
    } else if (p->cur.kind == NY_T_IDENT) {
      link_name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    } else {
      parser_error(p, p->cur,
                   "expected identifier or string literal after 'as'", NULL);
    }
  } else if (default_link_name) {
    link_name = extern_default_symbol_name(p, name);
  }
  parser_match(p, NY_T_SEMI);
  stmt_t *s = stmt_new(p->arena, NY_S_EXTERN, tok);
  s->as.ext.name = name;
  s->as.ext.params = params;
  s->as.ext.return_type = return_type;
  s->as.ext.link_name = link_name;
  s->as.ext.is_variadic = is_variadic;
  return s;
}

static stmt_t *parse_extern_block(parser_t *p, token_t tok, const char *lib) {
  parser_expect(p, NY_T_LBRACE, "'{' after extern", NULL);
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_reserve_arena(p->arena, &blk->as.block.body, 8);
  if (lib && *lib) {
    stmt_t *link = stmt_new(p->arena, NY_S_LINK, tok);
    link->as.link.lib = lib;
    vec_push_arena(p->arena, &blk->as.block.body, link);
  }
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (parser_match(p, NY_T_SEMI))
      continue;
    token_t fn_tok = p->cur;
    stmt_t *decl = parse_extern_fn_decl(p, fn_tok, true);
    if (decl) {
      vec_push_arena(p->arena, &blk->as.block.body, decl);
      continue;
    }
    while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF &&
           p->cur.kind != NY_T_FN)
      parser_advance(p);
  }
  parser_expect(p, NY_T_RBRACE, "'}' after extern block", NULL);
  return blk;
}

static stmt_t *parse_extern(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_EXTERN, "'extern'", NULL);
  if (p->cur.kind == NY_T_HASH) {
    parser_advance(p);
    if (p->cur.kind == NY_T_IDENT &&
        strncmp(p->cur.lexeme, "link", p->cur.len) == 0) {

      parser_advance(p);
      if (p->cur.kind != NY_T_STRING) {
        parser_error(p, p->cur, "expected library name string after '#link'",
                     NULL);
        return NULL;
      }
      stmt_t *s = stmt_new(p->arena, NY_S_LINK, tok);
      s->as.link.lib =
          arena_strndup(p->arena, p->cur.lexeme + 1, p->cur.len - 2);
      parser_advance(p);
      parser_match(p, NY_T_SEMI);
      return s;
    }
    if (p->cur.kind == NY_T_IDENT &&
        strncmp(p->cur.lexeme, "include", p->cur.len) == 0) {
      parser_advance(p);
      bool is_std = false;
      const char *path = NULL;
      if (p->cur.kind == NY_T_STRING) {
        size_t len = 0;
        path = parser_decode_string(p, p->cur, &len);
        parser_advance(p);
      } else if (parser_match(p, NY_T_LT)) {
        is_std = true;
        token_t start = p->cur;
        while (p->cur.kind != NY_T_GT && p->cur.kind != NY_T_EOF) {
          parser_advance(p);
        }
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
          size_t len = 0;
          prefix = parser_decode_string(p, p->cur, &len);
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

    parser_error(p, p->cur, "expected 'include' or 'link' after 'extern #'",
                 NULL);
    return NULL;
  }
  if (p->cur.kind == NY_T_STRING) {
    size_t len = 0;
    const char *lib = parser_decode_string(p, p->cur, &len);
    parser_advance(p);
    if (parser_match(p, NY_T_SEMI)) {
      stmt_t *s = stmt_new(p->arena, NY_S_LINK, tok);
      s->as.link.lib = lib;
      return s;
    }
    return parse_extern_block(p, tok, lib);
  }
  if (p->cur.kind == NY_T_LBRACE)
    return parse_extern_block(p, tok, NULL);
  return parse_extern_fn_decl(p, tok, false);
}

static stmt_t *parse_use_one(parser_t *p, token_t tok) {
  stmt_t *s = stmt_new(p->arena, NY_S_USE, tok);
  s->as.use.is_local = false;
  s->as.use.import_all = false;
  if (p->cur.kind == NY_T_STRING) {
    size_t slen = 0;
    const char *sval = parser_decode_string(p, p->cur, &slen);
    s->as.use.module = sval;
    s->as.use.is_local = true;
    parser_advance(p);
  } else if (p->cur.kind == NY_T_IDENT) {
    char *owned = parse_dotted_ident_owned(p, "expected module name",
                                           "expected identifier after '.'");
    if (!owned)
      return NULL;
    if (strcmp(owned, "std") == 0 && p->cur.kind == NY_T_IDENT &&
        p->cur.line == p->prev.line) {
      char *tail = parse_dotted_ident_owned(p, "expected std module name",
                                            "expected identifier after '.'");
      if (tail) {
        size_t full_len = strlen(owned) + 1 + strlen(tail);
        char *full = malloc(full_len + 1);
        if (!full) {
          fprintf(stderr, "oom\n");
          exit(1);
        }
        snprintf(full, full_len + 1, "%s.%s", owned, tail);
        free(owned);
        free(tail);
        owned = full;
      }
    }
    s->as.use.module = arena_strndup(p->arena, owned, strlen(owned));
    free(owned);
  } else {
    parser_error(p, p->cur, "use expects module identifier or string path",
                 NULL);
    return NULL;
  }
  if (parser_match(p, NY_T_COLON)) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(
          p, p->cur, "expected export profile after ':'",
          "use module:debug imports the core profile plus debug exports");
    } else {
      s->as.use.profile = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
  }
  if (parser_match(p, NY_T_STAR)) {
    s->as.use.import_all = true;
  }
  if (p->cur.kind == NY_T_LPAREN) {
    if (s->as.use.import_all) {
      parser_error(p, p->cur, "use '*' cannot be combined with an import list",
                   NULL);
    }
    parser_advance(p);
    while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "expected identifier in import list", NULL);
        break;
      }
      use_item_t item = {0};
      item.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      if (parser_match(p, NY_T_AS)) {
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected identifier after 'as'", NULL);
        } else {
          item.alias = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
          parser_advance(p);
        }
      }
      vec_push_arena(p->arena, &s->as.use.imports, item);
      if (parser_match(p, NY_T_COMMA)) {
        continue;
      }
      if (p->cur.kind == NY_T_IDENT)
        continue;
      break;
    }
    parser_expect(p, NY_T_RPAREN, ")'", NULL);
  }
  s->as.use.alias = NULL;
  if (!s->as.use.import_all && s->as.use.imports.len == 0 &&
      parser_match(p, NY_T_AS)) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after 'as'", NULL);
    } else {
      s->as.use.alias = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
  } else if (s->as.use.import_all || s->as.use.imports.len > 0) {
    if (p->cur.kind == NY_T_AS) {
      parser_error(p, p->cur,
                   "module alias cannot be combined with an import list", NULL);
    }
  }
  return s;
}

static stmt_t *parse_use(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_USE, "'use'", NULL);
  stmt_t *first = parse_use_one(p, tok);
  if (!first)
    return NULL;
  if (!parser_match(p, NY_T_COMMA)) {
    parser_match(p, NY_T_SEMI);
    return first;
  }
  stmt_t *block = stmt_new_transparent_block(p, tok);
  vec_push_arena(p->arena, &block->as.block.body, first);
  do {
    stmt_t *next = parse_use_one(p, p->cur);
    if (!next)
      break;
    vec_push_arena(p->arena, &block->as.block.body, next);
  } while (parser_match(p, NY_T_COMMA));
  parser_match(p, NY_T_SEMI);
  return block;
}

static void parser_export_meta_push_unique(parser_t *p,
                                           parser_ct_module_meta *meta,
                                           const char *name) {
  if (!name || !*name)
    return;
  for (size_t i = 0; i < meta->exports.len; i++) {
    if (strcmp(meta->exports.data[i], name) == 0)
      return;
  }
  vec_push_arena(p->arena, &meta->exports,
                 parser_intern(p, name, strlen(name)));
}

static void parser_register_layout_meta(parser_t *p, const char *name,
                                        ny_layout_field_list fields) {
  if (!name || !*name)
    return;
  parser_ct_layout_meta meta = {.name = name, .fields = fields};
  vec_push_arena(p->arena, &p->ct_layouts, meta);
}

static void parser_register_module_meta(parser_t *p, stmt_t *mod_stmt) {
  if (!mod_stmt || mod_stmt->kind != NY_S_MODULE || !mod_stmt->as.module.name)
    return;
  parser_ct_module_meta meta = {.name = mod_stmt->as.module.name};
  for (size_t i = 0; i < mod_stmt->as.module.body.len; i++) {
    stmt_t *s = mod_stmt->as.module.body.data[i];
    if (!s)
      continue;
    if (s->kind == NY_S_EXPORT) {
      if (s->as.exprt.is_internal)
        continue;
      for (size_t j = 0; j < s->as.exprt.names.len; j++)
        parser_export_meta_push_unique(p, &meta, s->as.exprt.names.data[j]);
      continue;
    }
    if (!mod_stmt->as.module.export_all)
      continue;
    const char *decl = NULL;
    switch (s->kind) {
    case NY_S_FUNC:
      decl = s->as.fn.name;
      break;
    case NY_S_LAYOUT:
      decl = s->as.layout.name;
      break;
    case NY_S_STRUCT:
      decl = s->as.struc.name;
      break;
    case NY_S_ENUM:
      decl = s->as.enu.name;
      break;
    default:
      break;
    }
    parser_export_meta_push_unique(p, &meta, ny_tail_name(decl));
  }
  vec_push_arena(p->arena, &p->ct_modules, meta);
}

static stmt_t *parse_module_profile_stmt(parser_t *p, bool is_internal) {
  token_t tok = p->cur;
  parser_advance(p);
  const char *profile = NULL;
  if (is_internal) {
    profile = arena_strndup(p->arena, "internal", 8);
  } else if (p->cur.kind == NY_T_IDENT) {
    profile = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
  } else {
    parser_error(p, p->cur, "expected export profile name",
                 "write export core(name) or export debug(name)");
  }
  parser_expect(p, NY_T_LPAREN, "'(' after module export profile", NULL);
  stmt_t *ex = stmt_new(p->arena, NY_S_EXPORT, tok);
  ex->as.exprt.profile = profile;
  ex->as.exprt.is_internal = is_internal;
  while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier in module export profile",
                   NULL);
      parser_advance(p);
      continue;
    }
    const char *name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    vec_push_arena(p->arena, &ex->as.exprt.names, name);
    parser_advance(p);
    if (parser_match(p, NY_T_COMMA))
      continue;
    if (p->cur.kind == NY_T_IDENT)
      continue;
    break;
  }
  parser_expect(p, NY_T_RPAREN, "')' after module export profile", NULL);
  parser_match(p, NY_T_SEMI);
  return ex;
}

static bool module_body_is_profile_header(const stmt_t *mod_stmt) {
  if (!mod_stmt || mod_stmt->kind != NY_S_MODULE ||
      mod_stmt->as.module.body.len == 0)
    return false;
  for (size_t i = 0; i < mod_stmt->as.module.body.len; i++) {
    stmt_t *s = mod_stmt->as.module.body.data[i];
    if (!s || s->kind != NY_S_EXPORT)
      return false;
  }
  return true;
}

typedef struct layout_gen_buf_t {
  char *data;
  size_t len;
  size_t cap;
} layout_gen_buf_t;
typedef VEC(const char *) layout_derive_list;

static void layout_gen_append(layout_gen_buf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t need = b->len + (size_t)n + 1;
  if (need > b->cap) {
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < need)
      cap *= 2;
    char *nb = realloc(b->data, cap);
    if (!nb) {
      fprintf(stderr, "oom\n");
      exit(1);
    }
    b->data = nb;
    b->cap = cap;
  }
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

static void layout_gen_append_str_lit(layout_gen_buf_t *b, const char *s) {
  layout_gen_append(b, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '\\':
        layout_gen_append(b, "\\\\");
        break;
      case '"':
        layout_gen_append(b, "\\\"");
        break;
      case '\n':
        layout_gen_append(b, "\\n");
        break;
      case '\r':
        layout_gen_append(b, "\\r");
        break;
      case '\t':
        layout_gen_append(b, "\\t");
        break;
      default:
        layout_gen_append(b, "%c", *p);
        break;
      }
    }
  }
  layout_gen_append(b, "\"");
}

static bool layout_type_is_int_like(const char *type_name) {
  if (!type_name)
    return false;
  while (*type_name == '*' || *type_name == '?')
    type_name++;
  return strcmp(type_name, "int") == 0 || strcmp(type_name, "i8") == 0 ||
         strcmp(type_name, "i16") == 0 || strcmp(type_name, "i32") == 0 ||
         strcmp(type_name, "i64") == 0 || strcmp(type_name, "i128") == 0 ||
         strcmp(type_name, "u8") == 0 || strcmp(type_name, "u16") == 0 ||
         strcmp(type_name, "u32") == 0 || strcmp(type_name, "u64") == 0 ||
         strcmp(type_name, "u128") == 0 || strcmp(type_name, "handle") == 0 ||
         strcmp(type_name, "char") == 0;
}

static bool layout_type_is_float_like(const char *type_name) {
  if (!type_name)
    return false;
  while (*type_name == '*' || *type_name == '?')
    type_name++;
  return strcmp(type_name, "f32") == 0 || strcmp(type_name, "f64") == 0 ||
         strcmp(type_name, "f128") == 0;
}

static const char *layout_default_src(layout_field_t *f) {
  if (f && f->default_src && *f->default_src)
    return f->default_src;
  const char *t = f ? f->type_name : NULL;
  while (t && (*t == '*' || *t == '?'))
    t++;
  if (!t)
    return "0";
  if (layout_type_is_float_like(t))
    return "0.0";
  if (strcmp(t, "bool") == 0)
    return "false";
  if (strcmp(t, "str") == 0)
    return "\"\"";
  return "0";
}

static bool layout_derive_has(layout_derive_list *derives, const char *name) {
  for (size_t i = 0; derives && i < derives->len; ++i) {
    if (derives->data[i] && strcmp(derives->data[i], name) == 0)
      return true;
  }
  return false;
}

static void layout_emit_field_cast(layout_gen_buf_t *b, const char *type_name,
                                   const char *raw_expr) {
  const char *t = type_name;
  while (t && (*t == '*' || *t == '?'))
    t++;
  if (layout_type_is_int_like(type_name))
    layout_gen_append(b, "int(%s)", raw_expr);
  else if (layout_type_is_float_like(type_name))
    layout_gen_append(b, "%s", raw_expr);
  else if (t && strcmp(t, "bool") == 0)
    layout_gen_append(b, "bool(%s)", raw_expr);
  else if (t && strcmp(t, "str") == 0)
    layout_gen_append(b, "to_str(%s)", raw_expr);
  else
    layout_gen_append(b, "%s", raw_expr);
}

static void layout_emit_store_call(layout_gen_buf_t *b, const char *owner,
                                   ny_layout_field_list *fields) {
  layout_gen_append(b, "   store_layout(out, ");
  layout_gen_append_str_lit(b, owner);
  for (size_t i = 0; i < fields->len; ++i)
    layout_gen_append(b, ", %s", fields->data[i].name);
  layout_gen_append(b, ")\n");
}

static void layout_emit_default_constructor(layout_gen_buf_t *b,
                                            const char *owner,
                                            ny_layout_field_list *fields,
                                            bool params) {
  layout_gen_append(b, "fn %s(", owner);
  if (params) {
    for (size_t i = 0; i < fields->len; ++i) {
      layout_field_t *f = &fields->data[i];
      layout_gen_append(b, "%s %s=%s", f->type_name, f->name,
                        layout_default_src(f));
      if (i + 1 < fields->len)
        layout_gen_append(b, ", ");
    }
  }
  layout_gen_append(b, ") ptr {\n");
  if (!params) {
    for (size_t i = 0; i < fields->len; ++i) {
      layout_field_t *f = &fields->data[i];
      layout_gen_append(b, "   def %s %s = %s\n", f->type_name, f->name,
                        layout_default_src(f));
    }
  }
  layout_gen_append(b, "   def ptr out = malloc(__layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  layout_emit_store_call(b, owner, fields);
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_shape_from(layout_gen_buf_t *b, const char *owner,
                                   ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_from(value) ptr {\n", owner);
  layout_gen_append(b,
                    "   if(!is_dict(value) && !is_list(value)){ return 0 }\n");
  layout_gen_append(b, "   def ptr out = malloc(__layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    char raw[512];
    snprintf(raw, sizeof(raw),
             "(is_dict(value) ? value.get(\"%s\", %s) : value.get(%zu, %s))",
             f->name, layout_default_src(f), i, layout_default_src(f));
    layout_gen_append(b, "   def %s %s = ", f->type_name, f->name);
    layout_emit_field_cast(b, f->type_name, raw);
    layout_gen_append(b, "\n");
  }
  layout_emit_store_call(b, owner, fields);
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_load_derives(layout_gen_buf_t *b, const char *owner,
                                     ny_layout_field_list *fields) {
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    layout_gen_append(b, "fn %s_load_%s(ptr self) %s {\n", owner, f->name,
                      f->type_name);
    layout_gen_append(b, "   return load_layout(self, ");
    layout_gen_append_str_lit(b, owner);
    layout_gen_append(b, ", ");
    layout_gen_append_str_lit(b, f->name);
    layout_gen_append(b, ")\n}\n");
  }
}

static void layout_emit_store_derive(layout_gen_buf_t *b, const char *owner,
                                     ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_store(ptr out", owner);
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    layout_gen_append(b, ", %s %s", f->type_name, f->name);
  }
  layout_gen_append(b, ") ptr {\n");
  layout_emit_store_call(b, owner, fields);
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_zero_derive(layout_gen_buf_t *b, const char *owner) {
  layout_gen_append(b, "fn %s_zero() ptr {\n", owner);
  layout_gen_append(b, "   def ptr out = malloc(__layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  layout_gen_append(b, "   memset(out, 0, __layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_eq_derive(layout_gen_buf_t *b, const char *owner,
                                  ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_eq(ptr a, ptr b) bool {\n", owner);
  layout_gen_append(b, "   if(!a || !b){ return false }\n");
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    layout_gen_append(b, "   if(load_layout(a, ");
    layout_gen_append_str_lit(b, owner);
    layout_gen_append(b, ", ");
    layout_gen_append_str_lit(b, f->name);
    layout_gen_append(b, ") != load_layout(b, ");
    layout_gen_append_str_lit(b, owner);
    layout_gen_append(b, ", ");
    layout_gen_append_str_lit(b, f->name);
    layout_gen_append(b, ")){ return false }\n");
  }
  layout_gen_append(b, "   return true\n}\n");
}

static void layout_emit_hash_derive(layout_gen_buf_t *b, const char *owner,
                                    ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_hash(ptr self) int {\n", owner);
  layout_gen_append(b, "   if(!self){ return 0 }\n");
  layout_gen_append(b, "   mut h = 17\n");
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    layout_gen_append(b, "   h = h * 31 + hash(load_layout(self, ");
    layout_gen_append_str_lit(b, owner);
    layout_gen_append(b, ", ");
    layout_gen_append_str_lit(b, f->name);
    layout_gen_append(b, "))\n");
  }
  layout_gen_append(b, "   return h\n}\n");
}

static void layout_emit_debug_str_derive(layout_gen_buf_t *b, const char *owner,
                                         ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_debug_str(ptr self) str {\n", owner);
  layout_gen_append(b, "   if(!self){ return ");
  layout_gen_append_str_lit(b, "<null>");
  layout_gen_append(b, " }\n");
  layout_gen_append(b, "   return ");
  const char *leaf = strrchr(owner, '.');
  layout_gen_append_str_lit(b, leaf ? leaf + 1 : owner);
  layout_gen_append(b, " + ");
  layout_gen_append_str_lit(b, "{");
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    layout_gen_append(b, " + ");
    layout_gen_append_str_lit(b, i == 0 ? f->name : ", ");
    if (i != 0) {
      layout_gen_append(b, " + ");
      layout_gen_append_str_lit(b, f->name);
    }
    layout_gen_append(b, " + ");
    layout_gen_append_str_lit(b, "=");
    layout_gen_append(b, " + to_str(load_layout(self, ");
    layout_gen_append_str_lit(b, owner);
    layout_gen_append(b, ", ");
    layout_gen_append_str_lit(b, f->name);
    layout_gen_append(b, "))");
  }
  layout_gen_append(b, " + ");
  layout_gen_append_str_lit(b, "}");
  layout_gen_append(b, "\n}\n");
}

static void layout_append_generated_stmts(parser_t *p, stmt_t *block,
                                          const char *src) {
  if (!src || !*src)
    return;
  size_t src_len = strlen(src);
  const char *stable_src = arena_strndup(p->arena, src, src_len);
  parser_t sub;
  parser_init_with_arena_quiet(&sub, stable_src,
                               p->filename ? p->filename : "<layout-derive>",
                               p->arena);
  sub.current_module = p->current_module;
  sub.ct_layouts = p->ct_layouts;
  sub.ct_modules = p->ct_modules;
  sub.ct_templates = p->ct_templates;
  sub.ct_diag_rules = p->ct_diag_rules;
  program_t prog = parse_program(&sub);
  if (sub.had_error) {
    p->had_error = true;
    p->error_count += sub.error_count;
  } else {
    ny_ast_verify_program(&prog, "layout-derive");
  }
  for (size_t i = 0; i < prog.body.len; ++i)
    vec_push_arena(p->arena, &block->as.block.body, prog.body.data[i]);
}

static stmt_t *layout_wrap_generated(parser_t *p, stmt_t *layout_stmt,
                                     layout_derive_list *derives,
                                     const char *flavor) {
  if (!layout_stmt || layout_stmt->kind != NY_S_LAYOUT)
    return layout_stmt;
  bool is_record = flavor && strcmp(flavor, "record") == 0;
  bool is_shape = flavor && strcmp(flavor, "shape") == 0;
  bool needs = is_record || is_shape || (derives && derives->len > 0);
  if (!needs)
    return layout_stmt;

  const char *valid[] = {"default", "eq",   "hash", "debug_str",
                         "store",   "load", "zero"};
  for (size_t i = 0; derives && i < derives->len; ++i) {
    bool ok = false;
    for (size_t j = 0; j < sizeof(valid) / sizeof(valid[0]); ++j)
      ok = ok || strcmp(derives->data[i], valid[j]) == 0;
    if (!ok)
      parser_error(
          p, layout_stmt->tok, "unknown layout derive",
          "valid derives are default, eq, hash, debug_str, store, load, zero");
  }

  stmt_t *block = stmt_new(p->arena, NY_S_BLOCK, layout_stmt->tok);
  vec_push_arena(p->arena, &block->as.block.body, layout_stmt);

  layout_gen_buf_t b = {0};
  ny_layout_field_list *fields = &layout_stmt->as.layout.fields;
  const char *owner = layout_stmt->as.layout.name;
  if (is_record || layout_derive_has(derives, "default"))
    layout_emit_default_constructor(&b, owner, fields, true);
  if (is_shape) {
    layout_emit_default_constructor(&b, owner, fields, false);
    layout_emit_shape_from(&b, owner, fields);
  }
  if (layout_derive_has(derives, "load"))
    layout_emit_load_derives(&b, owner, fields);
  if (layout_derive_has(derives, "store"))
    layout_emit_store_derive(&b, owner, fields);
  if (layout_derive_has(derives, "zero"))
    layout_emit_zero_derive(&b, owner);
  if (layout_derive_has(derives, "eq"))
    layout_emit_eq_derive(&b, owner, fields);
  if (layout_derive_has(derives, "hash"))
    layout_emit_hash_derive(&b, owner, fields);
  if (layout_derive_has(derives, "debug_str"))
    layout_emit_debug_str_derive(&b, owner, fields);

  if (b.data) {
    layout_append_generated_stmts(p, block, b.data);
    free(b.data);
  }
  return block;
}

static stmt_t *parse_layout_guard_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_STRUCT, "'layout'", NULL);
  if (!tok_is_ident_text(p->cur, "guard")) {
    parser_error(p, p->cur, "expected 'guard' after 'layout'",
                 "write: layout guard Shape value = source else { ... }");
    return NULL;
  }
  parser_advance(p);
  char *owned_type =
      parse_dotted_ident_owned(p, "expected layout shape name after guard",
                               "expected identifier after '.' in layout guard");
  if (!owned_type)
    return NULL;
  const char *type_name = parser_intern(p, owned_type, strlen(owned_type));
  free(owned_type);

  if (parser_match(p, NY_T_COLON)) {

  }

  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected binding name in layout guard", NULL);
    return NULL;
  }
  const char *name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);

  parser_expect(p, NY_T_ASSIGN, "'=' in layout guard", NULL);
  expr_t *value = p_parse_expr(p, 0);
  if (!parser_match(p, NY_T_ELSE)) {
    parser_error(
        p, p->cur, "layout guard requires an 'else' fallback",
        "return a default shape or propagate the failure from the else block");
    return NULL;
  }
  stmt_t *fallback = ny_parse_stmt_or_block(p);
  stmt_t *s = stmt_new(p->arena, NY_S_GUARD, tok);
  s->as.guard.type_name = type_name;
  s->as.guard.name = name;
  s->as.guard.value = value;
  s->as.guard.fallback = fallback;
  return s;
}

static stmt_t *parse_struct(parser_t *p) {
  token_t tok = p->cur;
  bool is_layout = (tok.len == 6 && strncmp(tok.lexeme, "layout", 6) == 0);
  parser_expect(p, NY_T_STRUCT, is_layout ? "'layout'" : "'struct'", NULL);
  const char *flavor = NULL;
  if (is_layout && tok_is_ident_text(p->cur, "record")) {
    flavor = parser_intern(p, "record", 6);
    parser_advance(p);
  } else if (is_layout && tok_is_ident_text(p->cur, "shape")) {
    flavor = parser_intern(p, "shape", 5);
    parser_advance(p);
  }
  const char *name = parse_qualified_name(p);
  if (!name) {
    return NULL;
  }
  stmt_t *s = stmt_new(p->arena, is_layout ? NY_S_LAYOUT : NY_S_STRUCT, tok);
  if (is_layout)
    s->as.layout.name = name;
  else
    s->as.struc.name = name;
  size_t align_override = 0;
  size_t pack = 0;
  layout_derive_list derives = {0};
  while (p->cur.kind == NY_T_IDENT) {
    if (p->cur.len == 5 && strncmp(p->cur.lexeme, "align", 5) == 0) {
      if (align_override != 0) {
        parser_error(p, p->cur, "duplicate align attribute", NULL);
        parser_advance(p);
        continue;
      }
      align_override = parse_align_attr(p, "align");
      continue;
    }
    if (p->cur.len == 4 && strncmp(p->cur.lexeme, "pack", 4) == 0) {
      if (pack != 0) {
        parser_error(p, p->cur, "duplicate pack attribute", NULL);
        parser_advance(p);
        continue;
      }
      pack = parse_align_attr(p, "pack");
      continue;
    }
    if (is_layout && p->cur.len == 6 &&
        strncmp(p->cur.lexeme, "derive", 6) == 0) {
      parser_advance(p);
      parser_expect(p, NY_T_LPAREN, "'(' after derive", NULL);
      while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected derive name", NULL);
          break;
        }
        vec_push_arena(p->arena, &derives,
                       arena_strndup(p->arena, p->cur.lexeme, p->cur.len));
        parser_advance(p);
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
      parser_expect(p, NY_T_RPAREN, "')' after derive list", NULL);
      continue;
    }
    break;
  }
  if (is_layout) {
    s->as.layout.align_override = align_override;
    s->as.layout.pack = pack;
    s->as.layout.flavor = flavor;
  } else {
    s->as.struc.align_override = align_override;
    s->as.struc.pack = pack;
  }
  parser_expect(p, NY_T_LBRACE, "'{'", NULL);
  ny_layout_field_list *fields =
      is_layout ? &s->as.layout.fields : &s->as.struc.fields;
  ny_stmt_list *methods =
      is_layout ? &s->as.layout.methods : &s->as.struc.methods;
  const char *owner = is_layout ? s->as.layout.name : s->as.struc.name;
  const char *prev_impl_owner = p->current_impl_owner;
  p->current_impl_owner = owner;
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind == NY_T_IDENT && p->cur.len == 8 &&
        strncmp(p->cur.lexeme, "operator", 8) == 0) {
      stmt_t *oper = parse_operator_stmt_with_left(p, owner, owner);
      if (oper)
        vec_push_arena(p->arena, methods, oper);
      parser_match(p, NY_T_COMMA);
      parser_match(p, NY_T_SEMI);
      continue;
    }
    if (p->cur.kind == NY_T_FN || p->cur.kind == NY_T_AT) {
      stmt_t *method = parse_attached_method(p, owner);
      if (method)
        vec_push_arena(p->arena, methods, method);
      parser_match(p, NY_T_COMMA);
      parser_match(p, NY_T_SEMI);
      continue;
    }
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected field name",
                   "keywords cannot be used as field names");
      break;
    }
    const char *id1 = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    const char *fname = id1;
    const char *tname = NULL;
    if (parser_match(p, NY_T_COLON)) {

    }
    if (p->cur.kind == NY_T_IDENT || p->cur.kind == NY_T_STAR || p->cur.kind == NY_T_QUESTION) {

      tname = id1;
      size_t ptr_depth = 0;
      while (parser_match(p, NY_T_STAR)) ptr_depth++;
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "expected field name after type", NULL);
        break;
      }
      fname = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      if (ptr_depth > 0) {
        size_t base_len = strlen(tname);
        char *ptr_tname = arena_alloc(p->arena, base_len + ptr_depth + 1);
        memset(ptr_tname, '*', ptr_depth);
        memcpy(ptr_tname + ptr_depth, tname, base_len);
        ptr_tname[base_len + ptr_depth] = '\0';
        tname = ptr_tname;
      }
    } else {

      if (!tname) {

         parser_error(p, p->prev, "layout fields require a type", "write 'int x'");
         break;
      }
    }
    int field_align = 0;
    if (p->cur.kind == NY_T_IDENT && p->cur.len == 5 &&
        strncmp(p->cur.lexeme, "align", 5) == 0) {
      field_align = (int)parse_align_attr(p, "align");
    }
    expr_t *default_value = NULL;
    const char *default_src = NULL;
    if (is_layout && parser_match(p, NY_T_ASSIGN)) {
      const char *start = p->cur.lexeme;
      default_value = p_parse_expr(p, 0);
      if (default_value && start && p->prev.lexeme) {
        const char *end = p->prev.lexeme + p->prev.len;
        if (end >= start)
          default_src = arena_strndup(p->arena, start, (size_t)(end - start));
      }
    }
    layout_field_t f_field = {fname, tname, field_align, default_value,
                              default_src};
    vec_push_arena(p->arena, fields, f_field);
    if (p->cur.kind == NY_T_COMMA)
      parser_advance(p);
  }
  p->current_impl_owner = prev_impl_owner;
  parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  parser_register_layout_meta(p, owner, *fields);
  return layout_wrap_generated(p, s, &derives, flavor);
}

static stmt_t *parse_enum(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_ENUM, "'enum'", NULL);
  const char *name = parse_qualified_name(p);
  if (!name) {
    return NULL;
  }
  stmt_t *s = stmt_new(p->arena, NY_S_ENUM, tok);
  s->as.enu.name = name;
  if (parser_match(p, NY_T_LT)) {
    while (p->cur.kind != NY_T_GT && p->cur.kind != NY_T_EOF) {
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "expected enum type parameter name", NULL);
        break;
      }
      const char *param = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      for (size_t i = 0; i < s->as.enu.type_params.len; i++) {
        if (strcmp(s->as.enu.type_params.data[i], param) == 0) {
          parser_error(p, tok, "duplicate enum type parameter", param);
          break;
        }
      }
      vec_push_arena(p->arena, &s->as.enu.type_params, param);
      if (!parser_match(p, NY_T_COMMA))
        break;
    }
    parser_expect(p, NY_T_GT, "'>' after enum type parameters", NULL);
  }
  parser_expect(p, NY_T_LBRACE, "'{'", NULL);
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected enum variant name", NULL);
      break;
    }
    stmt_enum_item_t item = {0};
    item.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    if (parser_match(p, NY_T_LPAREN)) {
      while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
        enum_field_t field = {0};
        field.type_name = parse_type_ref(p, "expected enum payload field type");

        parser_match(p, NY_T_COLON);
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected enum payload field name",
                       "write enum payload fields as `Type name`, for example `Circle(int radius)`");
          break;
        }
        field.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
        for (size_t i = 0; i < item.fields.len; i++) {
          if (strcmp(item.fields.data[i].name, field.name) == 0) {
            parser_error(p, tok, "duplicate enum payload field", field.name);
            break;
          }
        }
        vec_push_arena(p->arena, &item.fields, field);
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
      parser_expect(p, NY_T_RPAREN, "')' after enum payload fields", NULL);
    }
    if (parser_match(p, NY_T_ASSIGN)) {
      if (item.fields.len > 0)
        parser_error(p, tok,
                     "payload enum variants cannot also have integer values",
                     item.name);
      item.value = p_parse_expr(p, 0);
    }
    vec_push_arena(p->arena, &s->as.enu.items, item);
    if (!parser_match(p, NY_T_COMMA))
      break;
  }
  parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  return s;
}

static stmt_t *parse_module(parser_t *p) {
  token_t tok = p->cur;
  parser_advance(p);
  char *owned_mod = parse_dotted_ident_owned(p, "expected module name",
                                             "expected identifier after '.'");
  if (!owned_mod)
    return NULL;
  char *mod_name = arena_strndup(p->arena, owned_mod, strlen(owned_mod));
  free(owned_mod);
  bool export_all = false;
  if (parser_match(p, NY_T_STAR)) {
    export_all = true;
  }
  if (tok_is_ident_text(p->cur, "generated"))
    return parse_generated_module(p, tok, mod_name, export_all);
  token_kind end_kind = NY_T_EOF;
  if (p->cur.kind == NY_T_LPAREN) {
    parser_advance(p);
    end_kind = NY_T_RPAREN;
  } else if (p->cur.kind == NY_T_LBRACE) {
    parser_advance(p);
    end_kind = NY_T_RBRACE;
  }
  char *prev_mod = p->current_module;
  p->current_module = mod_name;
  stmt_t *mod_stmt = stmt_new(p->arena, NY_S_MODULE, tok);
  mod_stmt->as.module.name = mod_name;
  mod_stmt->as.module.path = p->filename;
  mod_stmt->as.module.export_all = export_all;
  bool file_level = (end_kind == NY_T_EOF || end_kind == NY_T_RPAREN);
  while (p->cur.kind != end_kind && p->cur.kind != NY_T_EOF) {
    if (file_level && end_kind == NY_T_EOF) {
      if (p->cur.filename && tok.filename &&
          strcmp(p->cur.filename, tok.filename) != 0) {
        break;
      }
      if (p->cur.kind == NY_T_MODULE) {
        break;
      }
      if (p->cur.kind == NY_T_RBRACE) {
        break;
      }
    }
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
      token_t next = parser_peek(p);
      bool is_export = false;
      if (next.kind == NY_T_COMMA || next.kind == end_kind)
        is_export = true;
      if (next.kind == NY_T_IDENT && end_kind == NY_T_RPAREN)
        is_export = true;
      if (is_export) {
        stmt_t *ex = stmt_new(p->arena, NY_S_EXPORT, p->cur);
        while (p->cur.kind == NY_T_IDENT) {
          char *ename = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
          vec_push_arena(p->arena, &ex->as.exprt.names, ename);
          parser_advance(p);
          if (parser_match(p, NY_T_COMMA)) {
          } else {
            if (p->cur.kind == NY_T_IDENT)
              continue;
            break;
          }
        }
        vec_push_arena(p->arena, &mod_stmt->as.module.body, ex);
        continue;
      }
    }
    parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
  }
  if (end_kind == NY_T_RPAREN) {
    parser_expect(p, NY_T_RPAREN, "')'", NULL);
    if (p->cur.kind == NY_T_LBRACE) {
      parser_advance(p);
      while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
        parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
      }
      parser_expect(p, NY_T_RBRACE, "'}'", NULL);
    } else if (p->block_depth == 0) {
      end_kind = NY_T_EOF;
      file_level = true;
      while (p->cur.kind != end_kind && p->cur.kind != NY_T_EOF) {
        if (p->cur.filename && tok.filename &&
            strcmp(p->cur.filename, tok.filename) != 0) {
          break;
        }
        if (p->cur.kind == NY_T_MODULE) {
          break;
        }
        if (p->cur.kind == NY_T_RBRACE) {
          break;
        }
        parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
      }
    }
  } else if (end_kind == NY_T_RBRACE) {
    parser_expect(p, NY_T_RBRACE, "'}'", NULL);
    if (module_body_is_profile_header(mod_stmt)) {
      while (p->cur.kind != NY_T_EOF) {
        if (p->cur.filename && tok.filename &&
            strcmp(p->cur.filename, tok.filename) != 0) {
          break;
        }
        if (p->cur.kind == NY_T_MODULE) {
          break;
        }
        if (p->cur.kind == NY_T_RBRACE) {
          break;
        }
        parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
      }
    }
  }
  p->current_module = prev_mod;
  mod_stmt->as.module.src_start = tok.lexeme;
  mod_stmt->as.module.src_end = p->prev.lexeme + p->prev.len;
  parser_register_module_meta(p, mod_stmt);
  return mod_stmt;
}

static attribute_t parse_attr(parser_t *p) {
  if (p->cur.kind < NY_T_IDENT || p->cur.kind > NY_T_ENUM) {
    parser_error(p, p->cur, "expected identifier after '@'", NULL);
    return (attribute_t){0};
  }
  attribute_t attr = {0};
  attr.tok = p->cur;
  attr.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  if (parser_match(p, NY_T_LPAREN)) {
    while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
      expr_t *e = p_parse_expr(p, 0);
      if (e)
        vec_push_arena(p->arena, &attr.args, e);
      if (!parser_match(p, NY_T_COMMA))
        break;
    }
    parser_expect(p, NY_T_RPAREN, "')' after attribute", NULL);
  }
  return attr;
}

static stmt_t *parse_macro_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected macro name", NULL);
    return NULL;
  }
  const char *name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  ny_expr_list args = {0};
  if (parser_match(p, NY_T_LPAREN)) {
    while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
      expr_t *e = p_parse_expr(p, 0);
      if (e)
        vec_push_arena(p->arena, &args, e);
      if (!parser_match(p, NY_T_COMMA))
        break;
    }
    parser_expect(p, NY_T_RPAREN, "')'", NULL);
  }
  stmt_t *body = NULL;
  if (p->cur.kind == NY_T_LBRACE) {
    body = p_parse_block(p);
  } else {
    parser_match(p, NY_T_SEMI);
  }
  stmt_t *s = stmt_new(p->arena, NY_S_MACRO, tok);
  s->as.macro.name = name;
  s->as.macro.args = args;
  s->as.macro.body = body;
  return s;
}
