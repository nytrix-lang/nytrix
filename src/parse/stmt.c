#include "priv.h"

static char *parse_qualified_name(parser_t *p) {
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected identifier", NULL);
    return NULL;
  }
  size_t cap = 256, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memcpy(buf, p->cur.lexeme, p->cur.len);
  len += p->cur.len;
  parser_advance(p);
  while (parser_match(p, NY_T_DOT)) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after '.'", NULL);
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
  }
  buf[len] = '\0';
  char *result = buf;
  size_t result_len = len;
  if (p->current_module) {
    size_t clen = strlen(p->current_module);
    if (!(len > clen && strncmp(p->current_module, buf, clen) == 0 &&
          buf[clen] == '.')) {
      char *prefixed = malloc(clen + 1 + len + 1);
      if (!prefixed) {
        free(buf);
        fprintf(stderr, "oom\n");
        exit(1);
      }
      memcpy(prefixed, p->current_module, clen);
      prefixed[clen] = '.';
      memcpy(prefixed + clen + 1, buf, len + 1);
      result = prefixed;
      result_len = clen + 1 + len;
    }
  }
  char *name = arena_strndup(p->arena, result, result_len);
  if (result != buf)
    free(result);
  free(buf);
  return name;
}

static stmt_t *parse_stmt_or_block(parser_t *p) {
  if (p->cur.kind == NY_T_LBRACE)
    return p_parse_block(p);
  token_t tok = p->cur;
  stmt_t *s = p_parse_stmt(p);
  if (!s)
    return NULL;
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &blk->as.block.body, s);
  return blk;
}

static stmt_t *parse_if(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_IF || p->cur.kind == NY_T_ELIF)
    parser_advance(p);
  else
    parser_expect(p, NY_T_IF, "'if' or 'elif'", NULL);
  expr_t *cond = p_parse_expr(p, 0);
  stmt_t *block = parse_stmt_or_block(p);
  stmt_t *alt = NULL;
  if (parser_match(p, NY_T_ELSE)) {
    if (p->cur.kind == NY_T_IF) {
      alt = parse_if(p);
    } else {
      alt = parse_stmt_or_block(p);
    }
  } else if (p->cur.kind == NY_T_ELIF) {
    alt = parse_if(p);
  }
  stmt_t *s = stmt_new(p->arena, NY_S_IF, tok);
  s->as.iff.test = cond;
  s->as.iff.conseq = block;
  s->as.iff.alt = alt;
  return s;
}

static stmt_t *parse_while(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_WHILE, "'while'", NULL);
  expr_t *cond = p_parse_expr(p, 0);
  if (p->cur.kind == NY_T_ASSIGN) {
    parser_error(p, p->cur, "assignment in condition", "did you mean '=='?");
    parser_advance(p);
    p_parse_expr(p, 0); // Consume RHS
  }
  p->loop_depth++;
  stmt_t *body = parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_WHILE, tok);
  s->as.whl.test = cond;
  s->as.whl.body = body;
  return s;
}

static stmt_t *parse_for(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_FOR, "'for'", NULL);
  bool has_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    has_paren = true;
    parser_advance(p);
    // Check if it looks like for (int i = 0; ...
    // Note: p->cur is now inside parens
    if (p->cur.kind == NY_T_IDENT || p->cur.kind == NY_T_DEF ||
        p->cur.kind == NY_T_MUT) {
      token_t next = parser_peek(p);
      if (next.kind == NY_T_ASSIGN || next.kind == NY_T_IDENT) {
        parser_error(p, p->cur, "C-style for loops are not supported",
                     "use 'for x in iterable' instead");
        // Recover by consuming up to RPAREN
        while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
          parser_advance(p);
        }
        if (p->cur.kind == NY_T_RPAREN)
          parser_advance(p);
        return NULL;
      }
    }
  }

  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "for expects loop variable", NULL);
    return NULL;
  }
  char *id = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  parser_expect(p, NY_T_IN, "'in'", NULL);
  expr_t *iter = p_parse_expr(p, 0);
  if (has_paren)
    parser_expect(p, NY_T_RPAREN, ")' after condition", NULL);
  p->loop_depth++;
  stmt_t *body = parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
  s->as.fr.iter_var = id;
  s->as.fr.iterable = iter;
  s->as.fr.body = body;
  return s;
}

static stmt_t *parse_try(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_TRY, "'try'", NULL);
  int saved_loop_depth = p->loop_depth;
  p->loop_depth = 0;
  stmt_t *body = p_parse_block(p);
  p->loop_depth = saved_loop_depth;
  parser_expect(p, NY_T_CATCH, "'catch'", NULL);
  const char *err = NULL;
  if (p->cur.kind == NY_T_LPAREN) {
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT)
      parser_error(p, p->cur, "expected identifier after '(", NULL);
    else {
      err = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
  } else if (p->cur.kind == NY_T_IDENT) {
    err = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
  }
  stmt_t *handler = p_parse_block(p);
  stmt_t *s = stmt_new(p->arena, NY_S_TRY, tok);
  s->as.tr.body = body;
  s->as.tr.err = err;
  s->as.tr.handler = handler;
  return s;
}

static stmt_t *parse_func(parser_t *p) {
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
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "param must be identifier", NULL);
      vec_free(&params);
      stmt_free_members(fn_stmt);
      return NULL;
    }
    pr.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    if (parser_match(p, NY_T_COLON)) {
      if (p->cur.kind != NY_T_IDENT)
        parser_error(p, p->cur, "expected type name", NULL);
      else {
        pr.type = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
      }
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
  if (parser_match(p, NY_T_COLON) || parser_match(p, NY_T_ARROW)) {
    if (p->cur.kind != NY_T_IDENT) {
      const char *hint =
          (p->cur.kind == NY_T_LBRACE)
              ? "did you mean to start the body? remove ':' or '->'"
              : NULL;
      parser_error(p, p->cur, "expected return type", hint);
    } else {
      fn_stmt->as.fn.return_type =
          arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
  }
  if (parser_match(p, NY_T_SEMI)) {
    stmt_t *s = fn_stmt;
    s->as.fn.name = name;
    s->as.fn.params = params;
    s->as.fn.body = NULL;
    s->as.fn.doc = NULL;
    s->as.fn.src_start = tok.lexeme;
    s->as.fn.src_end = p->prev.lexeme + p->prev.len;
    return s;
  }
  stmt_t *body = p_parse_block(p);
  const char *doc = NULL;
  if (body->as.block.body.len > 0) {
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

static stmt_t *parse_extern(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_EXTERN, "'extern'", NULL);
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
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "param must be identifier", NULL);
      return NULL;
    }
    param_t pr = {0};
    pr.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    if (parser_match(p, NY_T_COLON)) {
      if (p->cur.kind == NY_T_IDENT) {
        pr.type = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
      } else {
        parser_error(p, p->cur, "expected type name", NULL);
      }
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
  if (parser_match(p, NY_T_COLON) || parser_match(p, NY_T_ARROW)) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected return type", NULL);
    } else {
      return_type = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
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

static stmt_t *parse_return(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_RETURN, "'return'", NULL);
  stmt_t *s = stmt_new(p->arena, NY_S_RETURN, tok);
  if (p->cur.kind != NY_T_SEMI && p->cur.kind != NY_T_RBRACE)
    s->as.ret.value = p_parse_expr(p, 0);
  parser_match(p, NY_T_SEMI);
  return s;
}

static stmt_t *parse_goto(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_GOTO, "'goto'", NULL);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "goto expects label", NULL);
    return NULL;
  }
  stmt_t *s = stmt_new(p->arena, NY_S_GOTO, tok);
  s->as.go.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  parser_match(p, NY_T_SEMI);
  return s;
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
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
      fprintf(stderr, "oom\n");
      exit(1);
    }
    memcpy(buf, p->cur.lexeme, p->cur.len);
    len += p->cur.len;
    parser_advance(p);
    while (parser_match(p, NY_T_DOT)) {
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "expected identifier after '.'", NULL);
        free(buf);
        return NULL;
      }
      if (len + 1 + p->cur.len + 1 > cap) {
        cap = (len + 1 + p->cur.len + 1) * 2;
        char *nb = realloc(buf, cap);
        if (!nb) {
          fprintf(stderr, "oom\n");
          exit(1);
        }
        buf = nb;
      }
      buf[len++] = '.';
      memcpy(buf + len, p->cur.lexeme, p->cur.len);
      len += p->cur.len;
      parser_advance(p);
    }
    buf[len] = '\0';
    s->as.use.module = arena_strndup(p->arena, buf, len);
    free(buf);
  } else {
    parser_error(p, p->cur, "use expects module identifier or string path",
                 NULL);
    return NULL;
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

static stmt_t *parse_break(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_BREAK, "'break'", NULL);
  if (p->loop_depth <= 0) {
    parser_error(p, tok, "'break' used outside of a loop",
                 "put this inside a while/for body");
  }
  stmt_t *s = stmt_new(p->arena, NY_S_BREAK, tok);
  parser_match(p, NY_T_SEMI);
  return s;
}

static stmt_t *parse_continue(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_CONTINUE, "'continue'", NULL);
  if (p->loop_depth <= 0) {
    parser_error(p, tok, "'continue' used outside of a loop",
                 "put this inside a while/for body");
  }
  stmt_t *s = stmt_new(p->arena, NY_S_CONTINUE, tok);
  parser_match(p, NY_T_SEMI);
  return s;
}

static stmt_t *parse_layout(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_LAYOUT, "'layout'", NULL);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "layout expects name", NULL);
    return NULL;
  }
  const char *name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  parser_expect(p, NY_T_LPAREN, "'('", NULL);
  stmt_t *s = stmt_new(p->arena, NY_S_LAYOUT, tok);
  s->as.layout.name = name;
  while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected field name",
                   "keywords cannot be used as field names");
      break;
    }
    const char *fname = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    parser_expect(p, NY_T_COLON, "' :", NULL);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected type name", NULL);
      break;
    }
    const char *tname = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    layout_field_t f = {fname, tname, 0};
    vec_push_arena(p->arena, &s->as.layout.fields, f);
    if (p->cur.kind == NY_T_COMMA)
      parser_advance(p);
  }
  parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  return s;
}

static stmt_t *parse_module(parser_t *p) {
  token_t tok = p->cur;
  parser_advance(p);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "expected module name", NULL);
    return NULL;
  }
  size_t cap = 256, len = 0;
  char *buf = malloc(cap);
  memcpy(buf, p->cur.lexeme, p->cur.len);
  len += p->cur.len;
  parser_advance(p);
  while (parser_match(p, NY_T_DOT)) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after '.'", NULL);
      free(buf);
      return NULL;
    }
    if (len + 1 + p->cur.len >= cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        return NULL;
      }
      buf = nb;
    }
    buf[len++] = '.';
    memcpy(buf + len, p->cur.lexeme, p->cur.len);
    len += p->cur.len;
    parser_advance(p);
  }
  buf[len] = '\0';
  char *mod_name = arena_strndup(p->arena, buf, len);
  free(buf);
  bool export_all = false;
  if (parser_match(p, NY_T_STAR)) {
    export_all = true;
  }
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

  // If it's a file-level module (no braces), we read until the next 'module' or
  // EOF
  bool file_level = (end_kind == NY_T_EOF || end_kind == NY_T_RPAREN);

  while (p->cur.kind != end_kind && p->cur.kind != NY_T_EOF) {
    // If we are in file-level mode, stop if filename changes OR another
    // 'module' keyword is seen
    if (file_level && end_kind == NY_T_EOF) {
      if (p->cur.filename && tok.filename &&
          strcmp(p->cur.filename, tok.filename) != 0) {
        break;
      }
      if (p->cur.kind == NY_T_MODULE) {
        break;
      }
    }

    if (p->cur.kind == NY_T_IDENT) {
      token_t next = parser_peek(p);
      bool is_export = false;
      if (next.kind == NY_T_COMMA || next.kind == end_kind)
        is_export = true;
      if (next.kind == NY_T_IDENT && end_kind == NY_T_RPAREN)
        is_export = true; // Support (a b c) without commas
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
    stmt_t *s = p_parse_stmt(p);
    if (s) {
      vec_push_arena(p->arena, &mod_stmt->as.module.body, s);
    } else if (p->had_error) {
      while (p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_SEMI &&
             p->cur.kind != NY_T_RBRACE) {
        parser_advance(p);
      }
      if (p->cur.kind == NY_T_SEMI)
        parser_advance(p);
    }
  }

  // If we just finished an export list 'module name ( ... )',
  // and no brace follows, continue as a file-level module.
  if (end_kind == NY_T_RPAREN) {
    parser_expect(p, NY_T_RPAREN, "')'", NULL);
    if (p->cur.kind != NY_T_LBRACE && p->block_depth == 0) {
      end_kind = NY_T_EOF;
      // Continue the loop! (Actually just repeat the logic or use a goto?)
      // A cleaner way is to just use a 'goto' back to the top of while loop
      // but we need to reset file_level and end_kind correctly.
      file_level = true;
      while (p->cur.kind != end_kind && p->cur.kind != NY_T_EOF) {
        if (p->cur.filename && tok.filename &&
            strcmp(p->cur.filename, tok.filename) != 0) {
          break;
        }
        if (p->cur.kind == NY_T_MODULE) {
          break;
        }
        stmt_t *s = p_parse_stmt(p);
        if (s) {
          vec_push_arena(p->arena, &mod_stmt->as.module.body, s);
        } else if (p->had_error) {
          while (p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_SEMI &&
                 p->cur.kind != NY_T_RBRACE) {
            parser_advance(p);
          }
          if (p->cur.kind == NY_T_SEMI)
            parser_advance(p);
        }
      }
    }
  } else if (end_kind == NY_T_RBRACE) {
    parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  }

  p->current_module = prev_mod;
  mod_stmt->as.module.src_start = tok.lexeme;
  mod_stmt->as.module.src_end = p->prev.lexeme + p->prev.len;
  return mod_stmt;
}

stmt_t *p_parse_stmt(parser_t *p) {
  NY_LOG_DEBUG("Parsing statement kind %d at line %d\n", p->cur.kind,
               p->cur.line);
  switch (p->cur.kind) {
  case NY_T_SEMI:
    parser_advance(p);
    return NULL;
  case NY_T_USE:
    return parse_use(p);
  case NY_T_MODULE:
    return parse_module(p);
  case NY_T_LAYOUT:
    return parse_layout(p);
  case NY_T_EXTERN:
    return parse_extern(p);
  case NY_T_FN:
    return parse_func(p);
  case NY_T_IF:
    return parse_if(p);
  case NY_T_ELIF:
    parser_error(p, p->cur, "'elif' without 'if'",
                 "check if you forgot the preceding 'if' block");
    parser_advance(p);
    return NULL;
  case NY_T_WHILE:
    return parse_while(p);
  case NY_T_FOR:
    return parse_for(p);
  case NY_T_TRY:
    return parse_try(p);
  case NY_T_RETURN:
    return parse_return(p);
  case NY_T_BREAK:
    return parse_break(p);
  case NY_T_CONTINUE:
    return parse_continue(p);
  case NY_T_GOTO:
    return parse_goto(p);
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
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected identifier after 'def'", NULL);
          stmt_free_members(s);
          return NULL;
        }
        token_t ident = p->cur;
        parser_advance(p);

        char *final_name = (char *)ident.lexeme;
        size_t nlen = ident.len;
        bool mangled = false;
        if (p->current_module && p->block_depth == 0) {
          size_t mlen = strlen(p->current_module);
          char *prefixed = malloc(mlen + 1 + nlen + 1);
          sprintf(prefixed, "%s.%.*s", p->current_module, (int)nlen,
                  ident.lexeme);
          final_name = prefixed;
          nlen = strlen(prefixed);
          mangled = true;
        }
        const char *name_s = arena_strndup(p->arena, final_name, nlen);
        if (mangled)
          free(final_name);
        vec_push_arena(p->arena, &s->as.var.names, name_s);

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
    s->as.var.is_undef = false;
    return s;
  }
  case NY_T_UNDEF: {
    token_t start_tok = p->cur;
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after 'undef'", NULL);
      return NULL;
    }
    token_t ident = p->cur;
    parser_advance(p);
    parser_match(p, NY_T_SEMI);
    stmt_t *s = stmt_new(p->arena, NY_S_VAR, start_tok);
    const char *name_s = arena_strndup(p->arena, ident.lexeme, ident.len);
    vec_push_arena(p->arena, &s->as.var.names, name_s);
    s->as.var.is_decl = true;
    s->as.var.is_undef = true;
    return s;
  }
  case NY_T_IDENT: {
    token_t ident_tok = p->cur;
    const char *id = (const char *)ident_tok.lexeme;
    size_t id_len = ident_tok.len;

    /* Omniscience: Help users from other languages */
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

    token_t next = parser_peek(p);
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
        s->as.var.is_undef = false;
        return s;
      } else if (lhs->kind == NY_E_INDEX) {
        expr_t *callee = expr_new(p->arena, NY_E_IDENT, ident_tok);
        callee->as.ident.name = arena_strndup(p->arena, "set_idx", 7);
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
  case NY_T_LBRACE:
    return p_parse_block(p);
  default: {
    token_t first = p->cur;
    expr_t *e = p_parse_expr(p, 0);
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
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    stmt_t *s = p_parse_stmt(p);
    if (s) {
      vec_push_arena(p->arena, &blk->as.block.body, s);
    } else if (p->had_error) {
      // sync
      while (p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_SEMI &&
             p->cur.kind != NY_T_RBRACE) {
        parser_advance(p);
      }
      if (p->cur.kind == NY_T_SEMI)
        parser_advance(p);
    }
  }
  p->block_depth--;
  parser_expect(p, NY_T_RBRACE, "'}'", NULL);
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
  while (p->cur.kind != NY_T_EOF) {
    stmt_t *s = p_parse_stmt(p);
    if (s) {
      vec_push_arena(p->arena, &prog.body, s);
    } else if (p->had_error) {
      // sync
      while (p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_SEMI &&
             p->cur.kind != NY_T_RBRACE) {
        parser_advance(p);
      }
      if (p->cur.kind == NY_T_SEMI)
        parser_advance(p);
    }
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
  return prog;
}
