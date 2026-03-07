#include "priv.h"
#include <ctype.h>
#include <stdlib.h>

static attribute_t parse_attr(parser_t *p);
static bool tok_is_ident_text(token_t tok, const char *text);
static stmt_t *parse_generated_module(parser_t *p, token_t tok,
                                      const char *mod_name, bool export_all);
static stmt_t *impl_clone_for_owner(parser_t *p, stmt_t *base,
                                    const char *from_owner,
                                    const char *to_owner);
static expr_t *ct_int_expr(parser_t *p, token_t tok, int64_t value);

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

  /* Consume immediately following ident chars for prefix-number idents (e.g.
   * '3d') */
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
    /* Accept identifiers or numeric segments like '3' (for paths e.g. parse.3d)
     */
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
    /* If we just consumed a numeric token, absorb immediately following ident
       chars (e.g. 'd' after '3') to reconstruct '3d' as a single segment. */
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

static void parse_stmt_append_or_sync(parser_t *p, ny_stmt_list *out) {
  token_t before = p->cur;
  stmt_t *s = p_parse_stmt(p);
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
      /* consumed below */
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
  if (!parser_match(p, NY_T_COLON)) {
    parser_error(p, p->cur, "':' after resource type",
                 "write 'with Type: name = value { ... }'");
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected resource binding name after ':'", NULL);
    parser_sync_stmt_boundary(p);
    return NULL;
  }
  if (parser_token_is_builtin_type(p->cur)) {
    parser_error(p, p->cur, "resource bindings are type-first",
                 "write 'with ptr: buf = ...', not 'with buf: ptr = ...'");
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
                 "write 'with Type: name = value { ... }'");
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
      vec_free(&params);
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
    parser_error(p, p->prev, "function return types use ':'",
                 "use ': RetType' before '{' for return type");
    if (p->cur.kind != NY_T_LBRACE && p->cur.kind != NY_T_ASSIGN &&
        p->cur.kind != NY_T_EOF)
      (void)parse_type_ref(p, "expected return type after '->'");
  }
  if (parser_match(p, NY_T_COLON)) {
    if (p->cur.kind == NY_T_LBRACE) {
      parser_error(p, p->cur, "expected return type",
                   "did you mean to start the body? remove ':'");
    } else {
      fn_stmt->as.fn.return_type = parse_type_ref(p, "expected return type");
    }
  }
  if (p->cur.kind == NY_T_ARROW) {
    parser_error(p, p->cur, "use ': RetType' before '{' for return type",
                 "Nytrix uses 'fn name(params): RetType { body }' not 'fn "
                 "name(params) -> RetType { body }'");
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
    /* Braceless single-expression body: fn f(x) expr */
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
                   "example: impl Vec3 { fn len(self: value): f64 { ... } "
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
    return_type = parse_type_ref(p, "expected return type");
  }
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

static stmt_t *parse_extern(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_EXTERN, "'extern'", NULL);
  if (p->cur.kind == NY_T_HASH) {
    parser_advance(p);
    if (p->cur.kind == NY_T_IDENT &&
        strncmp(p->cur.lexeme, "link", p->cur.len) == 0) {
      /* extern #link "libname"  ── same as top-level #link "libname" */
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
      /* optional:  as prefix */
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
      /* optional:  link "libname" — tells JIT/AOT which .so to load */
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
    /* Fallback */
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
    parser_expect(p, NY_T_LBRACE, "'{' after extern library name", NULL);
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
  return parse_extern_fn_decl(p, tok, false);
}

static stmt_t *parse_use(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_USE, "'use'", NULL);
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
  parser_match(p, NY_T_SEMI);
  return s;
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
      layout_gen_append(b, "%s: %s=%s", f->type_name, f->name,
                        layout_default_src(f));
      if (i + 1 < fields->len)
        layout_gen_append(b, ", ");
    }
  }
  layout_gen_append(b, "): ptr {\n");
  if (!params) {
    for (size_t i = 0; i < fields->len; ++i) {
      layout_field_t *f = &fields->data[i];
      layout_gen_append(b, "   def %s: %s = %s\n", f->type_name, f->name,
                        layout_default_src(f));
    }
  }
  layout_gen_append(b, "   def ptr: out = malloc(__layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  layout_emit_store_call(b, owner, fields);
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_shape_from(layout_gen_buf_t *b, const char *owner,
                                   ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_from(value): ptr {\n", owner);
  layout_gen_append(b,
                    "   if(!is_dict(value) && !is_list(value)){ return 0 }\n");
  layout_gen_append(b, "   def ptr: out = malloc(__layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    char raw[512];
    snprintf(raw, sizeof(raw),
             "(is_dict(value) ? value.get(\"%s\", %s) : value.get(%zu, %s))",
             f->name, layout_default_src(f), i, layout_default_src(f));
    layout_gen_append(b, "   def %s: %s = ", f->type_name, f->name);
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
    layout_gen_append(b, "fn %s_load_%s(ptr: self): %s {\n", owner, f->name,
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
  layout_gen_append(b, "fn %s_store(ptr: out", owner);
  for (size_t i = 0; i < fields->len; ++i) {
    layout_field_t *f = &fields->data[i];
    layout_gen_append(b, ", %s: %s", f->type_name, f->name);
  }
  layout_gen_append(b, "): ptr {\n");
  layout_emit_store_call(b, owner, fields);
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_zero_derive(layout_gen_buf_t *b, const char *owner) {
  layout_gen_append(b, "fn %s_zero(): ptr {\n", owner);
  layout_gen_append(b, "   def ptr: out = malloc(__layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  layout_gen_append(b, "   memset(out, 0, __layout_size(");
  layout_gen_append_str_lit(b, owner);
  layout_gen_append(b, "))\n");
  layout_gen_append(b, "   return out\n}\n");
}

static void layout_emit_eq_derive(layout_gen_buf_t *b, const char *owner,
                                  ny_layout_field_list *fields) {
  layout_gen_append(b, "fn %s_eq(ptr: a, ptr: b): bool {\n", owner);
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
  layout_gen_append(b, "fn %s_hash(ptr: self): int {\n", owner);
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
  layout_gen_append(b, "fn %s_debug_str(ptr: self): str {\n", owner);
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
                 "write: layout guard Shape: value = source else { ... }");
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
  parser_expect(p, NY_T_COLON, "':' after layout guard type", NULL);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected binding name after ':'", NULL);
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
    parser_expect(p, NY_T_COLON, "':'", NULL);
    const char *fname = id1;
    const char *tname = NULL;
    size_t ptr_depth = 0;
    while (parser_match(p, NY_T_STAR))
      ptr_depth++;
    if (p->cur.kind == NY_T_IDENT) {
      const char *id2 = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      bool id1_is_type =
          (strcmp(id1, "int") == 0 || strcmp(id1, "i8") == 0 ||
           strcmp(id1, "i16") == 0 || strcmp(id1, "i32") == 0 ||
           strcmp(id1, "i64") == 0 || strcmp(id1, "i128") == 0 ||
           strcmp(id1, "u8") == 0 || strcmp(id1, "u16") == 0 ||
           strcmp(id1, "u32") == 0 || strcmp(id1, "u64") == 0 ||
           strcmp(id1, "u128") == 0 || strcmp(id1, "str") == 0 ||
           strcmp(id1, "ptr") == 0 || strcmp(id1, "handle") == 0 ||
           strcmp(id1, "fnptr") == 0 || strcmp(id1, "char") == 0 ||
           strcmp(id1, "seq") == 0 || strcmp(id1, "sequence") == 0 ||
           strcmp(id1, "number") == 0 || strcmp(id1, "numeric") == 0 ||
           strcmp(id1, "integer") == 0 || strcmp(id1, "float") == 0 ||
           strcmp(id1, "scalar") == 0 || strcmp(id1, "collection") == 0 ||
           strcmp(id1, "container") == 0 || strcmp(id1, "iterable") == 0 ||
           strcmp(id1, "indexable") == 0 || strcmp(id1, "allocator") == 0 ||
           strcmp(id1, "bool") == 0 || strcmp(id1, "f32") == 0 ||
           strcmp(id1, "f64") == 0 || strcmp(id1, "f128") == 0);
      if (id1_is_type) {
        fname = id2;
        tname = id1;
      } else {
        fname = id1;
        tname = id2;
      }
    } else {
      parser_error(p, p->cur, "expected field name or type", NULL);
      break;
    }
    if (ptr_depth > 0) {
      size_t tname_len = strlen(tname);
      size_t total_len = ptr_depth + tname_len;
      char *new_tname = arena_alloc(p->arena, total_len + 1);
      memset(new_tname, '*', ptr_depth);
      memcpy(new_tname + ptr_depth, tname, tname_len);
      new_tname[total_len] = '\0';
      tname = new_tname;
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
        parser_expect(p, NY_T_COLON, "':' after enum payload field type", NULL);
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected enum payload field name", NULL);
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
  parser_advance(p); /* '#' */
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
  parser_advance(p); /* if|elif */

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
    parser_advance(p); /* '#' */
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

stmt_t *p_parse_stmt(parser_t *p) {
  switch (p->cur.kind) {
  case NY_T_SEMI:
    parser_advance(p);
    return NULL;
  case NY_T_USE:
    return parse_use(p);
  case NY_T_MODULE:
    return parse_module(p);
  case NY_T_STRUCT:
    if (p->cur.len == 6 && strncmp(p->cur.lexeme, "layout", 6) == 0 &&
        tok_is_ident_text(parser_peek(p), "guard"))
      return parse_layout_guard_stmt(p);
    return parse_struct(p);
  case NY_T_ENUM:
    return parse_enum(p);
  case NY_T_EXTERN:
    return parse_extern(p);
  case NY_T_FN:
    return parse_func(p, (ny_attribute_list){0});
  case NY_T_COMPTIME: {
    if (tok_is_ident_text(parser_peek(p), "diagnostic"))
      return parse_comptime_diagnostic_rule_stmt(p);
    if (tok_is_ident_text(parser_peek(p), "template"))
      return parse_comptime_template_stmt(p);
    if (tok_is_ident_text(parser_peek(p), "emit"))
      return parse_comptime_emit_stmt(p);
    if (tok_is_ident_text(parser_peek(p), "table"))
      return parse_comptime_table_stmt(p);
    if (tok_is_ident_text(parser_peek(p), "fields") ||
        tok_is_ident_text(parser_peek(p), "exports"))
      return parse_comptime_reflect_stmt(p);
    token_t first = p->cur;
    if (parse_unsupported_comptime_stmt_form(p))
      return stmt_new_transparent_block(p, first);
    expr_t *e = p_parse_expr(p, 0);
    if (!e) {
      parser_sync_stmt_boundary(p);
      return NULL;
    }
    stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
    s->as.expr.expr = e;
    parser_match(p, NY_T_SEMI);
    return s;
  }
  case NY_T_HASH: {
    token_t tok = p->cur;
    parser_advance(p);
    if (tok_is_platform_guard(p->cur)) {
      return parse_hash_platform_guard_stmt(p, tok);
    }
    if (tok_is_hash_kw(p->cur, "if", NY_T_IF) ||
        tok_is_hash_kw(p->cur, "elif", NY_T_ELIF)) {
      return parse_hash_if_stmt(p, tok);
    }
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
      parser_advance(p); // line
      if (p->cur.kind == NY_T_NUMBER)
        parser_advance(p); // number
      if (p->cur.kind == NY_T_STRING)
        parser_advance(p); // filename
      return NULL;
    }
    if (p->cur.kind == NY_T_IDENT &&
        strncmp(p->cur.lexeme, "include", p->cur.len) == 0) {
      /* Top-level  #include <header>  or  #include "header"
         (C-style, no 'extern' prefix required)                */
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
      /* optional:  as prefix */
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
      /* optional:  link "libname" */
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
      /* #define NAME  or  #define NAME value  (FFI preprocessor macro) */
      parser_advance(p);
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "expected identifier after '#define'", NULL);
        return NULL;
      }
      const char *name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      /* Optional single value token */
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
                   "expected platform guard, 'link', 'include', 'define', or "
                   "'line' after '#'",
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
  case NY_T_AT: {
    /* Save state so we can backtrack if this is not a loop attribute */
    parser_t saved = *p;
    parser_advance(p); /* consume '@' */
    if (p->cur.kind >= NY_T_IDENT && p->cur.kind <= NY_T_ENUM) {
      const char *name = p->cur.lexeme;
      size_t namelen = p->cur.len;
      bool is_loop_attr = (namelen == 6 && memcmp(name, "unroll", 6) == 0) ||
                          (namelen == 8 && memcmp(name, "nounroll", 8) == 0) ||
                          (namelen == 9 && memcmp(name, "vectorize", 9) == 0) ||
                          (namelen == 4 && memcmp(name, "simd", 4) == 0);
      if (is_loop_attr) {
        // peek ahead for while/for
        parser_advance(p); /* consume attr name */
        if (p->cur.kind == NY_T_WHILE) {
          *p = saved;
          parser_advance(p); /* consume '@' */
          parser_advance(p); /* consume attr name */
          return ny_parse_while_stmt_with_attr(p, name, namelen);
        }
        if (p->cur.kind == NY_T_FOR) {
          *p = saved;
          return ny_parse_for_stmt(p);
        }
        /* Not a loop, restore and fall through */
        *p = saved;
      } else {
        /* Not a loop attribute, restore to re-parse as function attribute */
        *p = saved;
      }
    } else {
      /* Not an identifier after '@', restore and fall through */
      *p = saved;
    }
    /* Function attribute handling */
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
      parser_error(p, p->cur, "function attributes must be followed by 'fn'",
                   NULL);
      return NULL;
    }
    stmt_t *func = parse_func(p, attrs);
    if (func && func->kind == NY_S_FUNC) {
      func->attributes = attrs;
    }
    return func;
  }
  case NY_T_IF:
    return ny_parse_if_stmt(p);
  case NY_T_ELIF:
    parser_error(p, p->cur, "'elif' without 'if'",
                 "check if you forgot the preceding 'if' block");
    parser_advance(p);
    return NULL;
  case NY_T_WHILE:
    return ny_parse_while_stmt(p);
  case NY_T_FOR: {
    bool handled = false;
    stmt_t *ct_for = parse_comptime_family_for_stmt(p, &handled);
    if (handled)
      return ct_for;
    return ny_parse_for_stmt(p);
  }
  case NY_T_TRY:
    return ny_parse_try_stmt(p);
  case NY_T_RETURN:
    return ny_parse_return_stmt(p);
  case NY_T_BREAK:
    return ny_parse_break_stmt(p);
  case NY_T_CONTINUE:
    return ny_parse_continue_stmt(p);
  case NY_T_GOTO:
    return ny_parse_goto_stmt(p);
  case NY_T_MATCH:
    return p_parse_match(p);
  case NY_T_DEFER: {
    token_t tok = p->cur;
    parser_advance(p);
    stmt_t *s = stmt_new(p->arena, NY_S_DEFER, tok);
    s->as.de.body = p_parse_block(p);
    return s;
  }
  case NY_T_MUT:
  case NY_T_DEF: {
    token_t start_tok = p->cur;
    parser_advance(p);
    stmt_t *s = stmt_new(p->arena, NY_S_VAR, start_tok);
    s->as.var.is_mut = (start_tok.kind == NY_T_MUT);
    s->as.var.is_destructure = false;
    if (parser_match(p, NY_T_LBRACK)) {
      s->as.var.is_destructure = true;
      while (true) {
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected identifier in destructuring list",
                       NULL);
          if (p->cur.kind != NY_T_EOF)
            parser_advance(p);
          stmt_free_members(s);
          return NULL;
        }
        vec_push_arena(p->arena, &s->as.var.names,
                       arena_strndup(p->arena, p->cur.lexeme, p->cur.len));
        parser_advance(p);
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
      parser_expect(p, NY_T_RBRACK, "']' after destructuring list", NULL);
    } else {
      while (true) {
        const char *var_type = NULL;
        token_t ident = {0};
        if (((p->cur.kind == NY_T_IDENT || p->cur.kind == NY_T_NUMBER) &&
             (parser_peek(p).kind == NY_T_COLON ||
              parser_peek(p).kind == NY_T_LT)) ||
            p->cur.kind == NY_T_QUESTION || p->cur.kind == NY_T_STAR) {
          var_type = parse_type_ref(p, "expected type name before ':'");
          parser_expect(p, NY_T_COLON, "':' after type", NULL);
          if (p->cur.kind != NY_T_IDENT) {
            parser_error(p, p->cur, "expected variable name after ':'", NULL);
            if (p->cur.kind != NY_T_EOF)
              parser_advance(p);
            stmt_free_members(s);
            return NULL;
          }
          if (parser_token_is_builtin_type(p->cur)) {
            parser_error(
                p, p->cur, "typed declarations are type-first",
                "write 'def int: value = ...', not 'def value: int = ...'");
            stmt_free_members(s);
            return NULL;
          }
          ident = p->cur;
          parser_advance(p);
        } else {
          if (stmt_token_looks_type_name(p->cur) &&
              parser_peek(p).kind == NY_T_IDENT) {
            parser_error(
                p, p->cur, "typed declarations use 'type: name'",
                start_tok.kind == NY_T_MUT
                    ? "write 'mut u64: hash = ...', not 'mut u64 hash = ...'"
                    : "write 'def int: value = ...', not 'def int value = "
                      "...'");
            parser_sync_stmt_boundary(p);
            stmt_free_members(s);
            return NULL;
          }
          if (p->cur.kind != NY_T_IDENT) {
            parser_error(p, p->cur, "expected identifier after 'def'",
                         parse_missing_ident_hint(p->cur, "variable"));
            if (p->cur.kind != NY_T_EOF)
              parser_advance(p);
            stmt_free_members(s);
            return NULL;
          }
          ident = p->cur;
          parser_advance(p);
        }
        char *final_name = (char *)ident.lexeme;
        size_t nlen = ident.len;
        bool mangled = false;
        if (p->block_depth == 0 && p->current_module) {
          size_t mlen = strlen(p->current_module);
          size_t total = mlen + 1 + nlen + 1;
          char *prefixed = malloc(total);
          snprintf(prefixed, total, "%s.%.*s", p->current_module, (int)nlen,
                   ident.lexeme);
          final_name = prefixed;
          nlen = strlen(prefixed);
          mangled = true;
        }
        const char *name_s = arena_strndup(p->arena, final_name, nlen);
        if (mangled)
          free(final_name);
        vec_push_arena(p->arena, &s->as.var.names, name_s);
        if (parser_match(p, NY_T_COLON)) {
          parser_error(
              p, p->prev, "typed declarations are type-first",
              "write 'def int: value = ...', not 'def value: int = ...'");
          (void)parse_type_ref(p, "expected type name after ':'");
        }
        vec_push_arena(p->arena, &s->as.var.types, var_type);
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
    }
    if (parser_match(p, NY_T_ASSIGN)) {
      while (true) {
        vec_push_arena(p->arena, &s->as.var.exprs, p_parse_expr(p, 0));
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
    } else {
      token_t zero_tok = {0};
      expr_t *zero = expr_new(p->arena, NY_E_LITERAL, zero_tok);
      zero->as.literal.kind = NY_LIT_INT;
      zero->as.literal.as.i = 0;
      vec_push_arena(p->arena, &s->as.var.exprs, zero);
    }
    parser_match(p, NY_T_SEMI);
    s->as.var.is_decl = true;
    s->as.var.is_del = false;
    return s;
  }
  case NY_T_DEL: {
    token_t start_tok = p->cur;
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after 'del'",
                   parse_missing_ident_hint(p->cur, "binding"));
      return NULL;
    }
    token_t ident = p->cur;
    parser_advance(p);
    parser_match(p, NY_T_SEMI);
    stmt_t *s = stmt_new(p->arena, NY_S_VAR, start_tok);
    const char *name_s = arena_strndup(p->arena, ident.lexeme, ident.len);
    vec_push_arena(p->arena, &s->as.var.names, name_s);
    s->as.var.is_decl = true;
    s->as.var.is_del = true;
    return s;
  }
  case NY_T_IDENT: {
    token_t ident_tok = p->cur;
    const char *id = (const char *)ident_tok.lexeme;
    size_t id_len = ident_tok.len;
    token_t next = parser_peek(p);
    if (id_len == 5 && strncmp(id, "using", 5) == 0) {
      parser_error(p, ident_tok, "resource blocks use 'with'",
                   "write 'with ptr: name = value { ... }'");
      parser_advance(p);
      int brace_depth = 0;
      while (p->cur.kind != NY_T_EOF) {
        if (p->cur.kind == NY_T_LBRACE) {
          brace_depth++;
        } else if (p->cur.kind == NY_T_RBRACE) {
          if (brace_depth == 0)
            break;
          brace_depth--;
          if (brace_depth == 0) {
            parser_advance(p);
            break;
          }
        } else if (p->cur.kind == NY_T_SEMI && brace_depth == 0) {
          parser_advance(p);
          break;
        }
        parser_advance(p);
      }
      return NULL;
    }
    if (id_len == 4 && strncmp(id, "with", 4) == 0) {
      return parse_resource_block_stmt(p);
    }
    if (id_len == 8 && strncmp(id, "operator", 8) == 0) {
      return parse_operator_stmt(p);
    }
    if (id_len == 4 && strncmp(id, "impl", 4) == 0) {
      return parse_impl_stmt(p);
    }
    if (next.kind == NY_T_LBRACE) {
      if (stmt_token_looks_type_name(ident_tok) &&
          stmt_ident_lbrace_starts_named_fields(p)) {
        parser_error(p, next, "named-field struct literals are not supported",
                     "use Type(value, value) positional constructor syntax");
        stmt_skip_named_field_literal(p);
        return NULL;
      }
      return parse_macro_stmt(p);
    }
    if (id_len == 4 && strncmp(id, "func", 4) == 0) {
      parser_error(p, ident_tok, "unrecognised keyword 'func'",
                   "did you mean 'fn'?");
    } else if (id_len == 8 && strncmp(id, "function", 8) == 0) {
      parser_error(p, ident_tok, "unrecognised keyword 'function'",
                   "did you mean 'fn'?");
    } else if (id_len == 3 && strncmp(id, "let", 3) == 0) {
      parser_error(p, ident_tok, "unrecognised keyword 'let'",
                   "use 'def' for immutable or 'mut' for mutable variables");
    } else if (id_len == 3 && strncmp(id, "var", 3) == 0) {
      parser_error(p, ident_tok, "unrecognised keyword 'var'",
                   "use 'mut' for mutable variables");
    } else if (id_len == 6 && strncmp(id, "import", 6) == 0) {
      parser_error(p, ident_tok, "unrecognised keyword 'import'",
                   "use 'use' to import modules");
    }
    if (next.kind == NY_T_COLON) {
      parser_advance(p);
      parser_expect(p, NY_T_COLON, NULL,
                    "expected ':' after case/default label");
      stmt_t *s = stmt_new(p->arena, NY_S_LABEL, ident_tok);
      s->as.label.name =
          arena_strndup(p->arena, ident_tok.lexeme, ident_tok.len);
      return s;
    }
    expr_t *lhs = p_parse_expr(p, 0);
    if (!lhs) {
      parser_sync_stmt_boundary(p);
      return NULL;
    }
    if (lhs->kind == NY_E_CALL && lhs->as.call.callee &&
        lhs->as.call.callee->kind == NY_E_IDENT && p->cur.kind == NY_T_LBRACE) {
      stmt_t *s = stmt_new(p->arena, NY_S_MACRO, ident_tok);
      s->as.macro.name = lhs->as.call.callee->as.ident.name;
      for (size_t i = 0; i < lhs->as.call.args.len; i++) {
        call_arg_t *arg = &lhs->as.call.args.data[i];
        if (arg->name) {
          parser_error(p, arg->val ? arg->val->tok : ident_tok,
                       "named arguments are not supported in macro statements",
                       NULL);
        }
        vec_push_arena(p->arena, &s->as.macro.args, arg->val);
      }
      s->as.macro.body = p_parse_block(p);
      return s;
    }
    if (p->cur.kind == NY_T_COMMA && lhs->kind == NY_E_IDENT) {
      stmt_t *s = stmt_new(p->arena, NY_S_VAR, ident_tok);
      vec_push_arena(p->arena, &s->as.var.names, lhs->as.ident.name);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur,
                       "expected identifier in assignment target list", NULL);
          stmt_free_members(s);
          return NULL;
        }
        vec_push_arena(p->arena, &s->as.var.names,
                       arena_strndup(p->arena, p->cur.lexeme, p->cur.len));
        parser_advance(p);
      }
      if (p->cur.kind != NY_T_ASSIGN) {
        parser_error(p, p->cur, "expected '=' after assignment target list",
                     NULL);
        stmt_free_members(s);
        return NULL;
      }
      parser_advance(p);
      while (true) {
        vec_push_arena(p->arena, &s->as.var.exprs, p_parse_expr(p, 0));
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
      parser_match(p, NY_T_SEMI);
      s->as.var.is_decl = false;
      s->as.var.is_del = false;
      return s;
    }
    token_kind assign_op = NY_T_EOF;
    if (p->cur.kind == NY_T_ASSIGN || p->cur.kind == NY_T_PLUS_EQ ||
        p->cur.kind == NY_T_MINUS_EQ || p->cur.kind == NY_T_STAR_EQ ||
        p->cur.kind == NY_T_SLASH_EQ || p->cur.kind == NY_T_PERCENT_EQ) {
      assign_op = p->cur.kind;
      parser_advance(p);
    }
    if (assign_op != NY_T_EOF) {
      expr_t *rhs = p_parse_expr(p, 0);
      parser_match(p, NY_T_SEMI);
      if (assign_op != NY_T_ASSIGN) {
        token_kind bin_kind = (assign_op == NY_T_PLUS_EQ)    ? NY_T_PLUS
                              : (assign_op == NY_T_MINUS_EQ) ? NY_T_MINUS
                              : (assign_op == NY_T_STAR_EQ)  ? NY_T_STAR
                              : (assign_op == NY_T_SLASH_EQ) ? NY_T_SLASH
                                                             : NY_T_PERCENT;
        token_t op_tok = {0};
        expr_t *bin = expr_new(p->arena, NY_E_BINARY, op_tok);
        bin->as.binary.op = parser_token_name(bin_kind);
        bin->as.binary.left = lhs;
        bin->as.binary.right = rhs;
        rhs = bin;
      }
      if (lhs->kind == NY_E_IDENT) {
        stmt_t *s = stmt_new(p->arena, NY_S_VAR, ident_tok);
        vec_push_arena(p->arena, &s->as.var.names, lhs->as.ident.name);
        vec_push_arena(p->arena, &s->as.var.exprs, rhs);
        s->as.var.is_decl = false;
        s->as.var.is_del = false;
        return s;
      } else if (lhs->kind == NY_E_INDEX) {
        expr_t *callee = expr_new(p->arena, NY_E_IDENT, ident_tok);
        callee->as.ident.name = arena_strndup(p->arena, "set_idx", 7);
        callee->as.ident.sym_id = ny_intern_str("set_idx", 7);
        expr_t *call = expr_new(p->arena, NY_E_CALL, ident_tok);
        call->as.call.callee = callee;
        vec_push_arena(p->arena, &call->as.call.args,
                       ((call_arg_t){NULL, lhs->as.index.target}));
        expr_t *idx_expr = lhs->as.index.start;
        if (!idx_expr) {
          expr_t *zero = expr_new(p->arena, NY_E_LITERAL, ident_tok);
          zero->as.literal.kind = NY_LIT_INT;
          zero->as.literal.as.i = 0;
          idx_expr = zero;
        }
        vec_push_arena(p->arena, &call->as.call.args,
                       ((call_arg_t){NULL, idx_expr}));
        vec_push_arena(p->arena, &call->as.call.args,
                       ((call_arg_t){NULL, rhs}));
        stmt_t *s = stmt_new(p->arena, NY_S_EXPR, ident_tok);
        s->as.expr.expr = call;
        return s;
      } else {
        parser_error(p, ident_tok,
                     "assignment target must be identifier or index", NULL);
        return NULL;
      }
    }
    stmt_t *s = stmt_new(p->arena, NY_S_EXPR, ident_tok);
    s->as.expr.expr = lhs;
    parser_match(p, NY_T_SEMI);
    return s;
  }
  case NY_T_PLUS_PLUS:
  case NY_T_MINUS_MINUS: {
    token_t op_tok = p->cur;
    bool is_inc = (op_tok.kind == NY_T_PLUS_PLUS);
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur,
                   is_inc ? "expected identifier after '++'"
                          : "expected identifier after '--'",
                   NULL);
      return NULL;
    }
    token_t ident_tok = p->cur;
    parser_advance(p);
    expr_t *ident_expr = expr_new(p->arena, NY_E_IDENT, ident_tok);
    ident_expr->as.ident.name =
        arena_strndup(p->arena, ident_tok.lexeme, ident_tok.len);
    ident_expr->as.ident.sym_id = ident_tok.sym_id;
    expr_t *one = expr_new(p->arena, NY_E_LITERAL, op_tok);
    one->as.literal.kind = NY_LIT_INT;
    one->as.literal.as.i = 1;
    token_t bin_tok = {0};
    expr_t *bin = expr_new(p->arena, NY_E_BINARY, bin_tok);
    bin->as.binary.op = is_inc ? "+" : "-";
    bin->as.binary.left = ident_expr;
    bin->as.binary.right = one;
    stmt_t *s = stmt_new(p->arena, NY_S_VAR, op_tok);
    vec_push_arena(p->arena, &s->as.var.names, ident_expr->as.ident.name);
    vec_push_arena(p->arena, &s->as.var.exprs, bin);
    s->as.var.is_decl = false;
    s->as.var.is_del = false;
    parser_match(p, NY_T_SEMI);
    return s;
  }
  case NY_T_LBRACE:
    if (stmt_lbrace_starts_dict_literal(p)) {
      token_t first = p->cur;
      expr_t *e = p_parse_expr(p, 0);
      if (!e) {
        parser_sync_stmt_boundary(p);
        return NULL;
      }
      stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
      s->as.expr.expr = e;
      parser_match(p, NY_T_SEMI);
      return s;
    }
    return p_parse_block(p);
  default: {
    token_t first = p->cur;
    expr_t *e = p_parse_expr(p, 0);
    if (!e) {
      parser_sync_stmt_boundary(p);
      return NULL;
    }
    stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
    s->as.expr.expr = e;
    parser_match(p, NY_T_SEMI);
    return s;
  }
  }
}

stmt_t *p_parse_block(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_LBRACE, "'{'", NULL);
  p->block_depth++;
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_reserve_arena(p->arena, &blk->as.block.body, 8);
  bool recovered_missing_close = false;
  bool allow_file_level_decls = parser_block_allows_file_level_decls(p, tok);
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (!allow_file_level_decls && p->cur.col <= 1 &&
        parser_token_starts_file_level_decl(p->cur)) {
      parser_report_missing_rbrace_once(
          p, p->cur, "missing '}' before top-level declaration", tok.line);
      recovered_missing_close = true;
      break;
    }
    parse_stmt_append_or_sync(p, &blk->as.block.body);
  }
  p->block_depth--;
  if (p->cur.kind == NY_T_RBRACE) {
    parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  } else if (p->cur.kind == NY_T_EOF) {
    parser_report_missing_rbrace_once(
        p, p->cur, "missing '}' before end of file", tok.line);
  } else if (!recovered_missing_close) {
    parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  }
  return blk;
}

program_t parse_program(parser_t *p) {
  if (p->lex.src) {
    NY_LOG_V1("Parsing started for source of size %zu\n", strlen(p->lex.src));
  } else {
    NY_LOG_V1("Parsing started for unknown source\n");
  }
  NY_LOG_V2("Source filename: %s\n",
            p->lex.filename ? p->lex.filename : "<unknown>");
  program_t prog = {0};
  if (p->lex.src) {
    size_t src_len = strlen(p->lex.src);
    size_t guess = src_len / 32;
    if (guess < 8)
      guess = 8;
    if (guess > 4096)
      guess = 4096;
    vec_reserve_arena(p->arena, &prog.body, guess);
    /* Store raw source for debug info (needed for -c inline code) */
    prog.raw_src = p->src;
    prog.raw_src_len = src_len;
  }
  while (p->cur.kind != NY_T_EOF) {
    parse_stmt_append_or_sync(p, &prog.body);
  }
  if (prog.body.len > 0) {
    stmt_t *s0 = prog.body.data[0];
    if (s0->kind == NY_S_EXPR && s0->as.expr.expr->kind == NY_E_LITERAL &&
        s0->as.expr.expr->as.literal.kind == NY_LIT_STR) {
      prog.doc = arena_strndup(p->arena, s0->as.expr.expr->as.literal.as.s.data,
                               s0->as.expr.expr->as.literal.as.s.len);
      memmove(prog.body.data, prog.body.data + 1,
              (prog.body.len - 1) * sizeof(stmt_t *));
      prog.body.len -= 1;
    }
  }
  prog.diagnostic_rules = p->ct_diag_rules;
  return prog;
}
