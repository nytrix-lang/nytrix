#include "priv.h"

stmt_t *ny_parse_stmt_or_block(parser_t *p) {
  if (p->cur.kind == NY_T_LBRACE)
    return p_parse_block(p);
  if (p->cur.kind == NY_T_COLON) {
    parser_error(p, p->cur, "stray ':'",
                 "Nytrix does not use ':' to start blocks, use '{' ... '}' instead");
    parser_advance(p);
  }
  token_t tok = p->cur;
  stmt_t *s = p_parse_stmt(p);
  if (!s)
    return NULL;
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &blk->as.block.body, s);
  return blk;
}

stmt_t *ny_parse_if_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_IF || p->cur.kind == NY_T_ELIF)
    parser_advance(p);
  else
    parser_expect(p, NY_T_IF, "'if' or 'elif'", NULL);
  bool has_paren = (p->cur.kind == NY_T_LPAREN);
  if (has_paren)
    parser_advance(p);

  stmt_t *init = NULL;
  if (p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT) {
    init = p_parse_stmt(p);
  }

  expr_t *cond = p_parse_expr(p, 0);
  if (has_paren)
    parser_expect(p, NY_T_RPAREN, "')' after if clause", NULL);

  stmt_t *block = ny_parse_stmt_or_block(p);
  stmt_t *alt = NULL;
  if (parser_match(p, NY_T_ELSE)) {
    if (p->cur.kind == NY_T_IF) {
      alt = ny_parse_if_stmt(p);
    } else {
      alt = ny_parse_stmt_or_block(p);
    }
  } else if (p->cur.kind == NY_T_ELIF) {
    alt = ny_parse_if_stmt(p);
  }
  stmt_t *s = stmt_new(p->arena, NY_S_IF, tok);
  s->as.iff.test = cond;
  s->as.iff.conseq = block;
  s->as.iff.alt = alt;
  s->as.iff.init = init;
  return s;
}

static stmt_t *parse_incrdecr_stmt(parser_t *p) {
  token_t op_tok = p->cur;
  bool is_inc = (op_tok.kind == NY_T_PLUS_PLUS);
  parser_advance(p);
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur,
                 is_inc ? "expected identifier after '++'" : "expected identifier after '--'",
                 NULL);
    return NULL;
  }
  token_t id_tok = p->cur;
  parser_advance(p);
  expr_t *id_expr = expr_new(p->arena, NY_E_IDENT, id_tok);
  id_expr->as.ident.name = arena_strndup(p->arena, id_tok.lexeme, id_tok.len);
  id_expr->as.ident.sym_id = id_tok.sym_id;
  expr_t *one = expr_new(p->arena, NY_E_LITERAL, op_tok);
  one->as.literal.kind = NY_LIT_INT;
  one->as.literal.as.i = 1;
  token_t bin_tok = {0};
  expr_t *bin = expr_new(p->arena, NY_E_BINARY, bin_tok);
  bin->as.binary.op = is_inc ? "+" : "-";
  bin->as.binary.left = id_expr;
  bin->as.binary.right = one;
  stmt_t *s = stmt_new(p->arena, NY_S_VAR, op_tok);
  vec_push_arena(p->arena, &s->as.var.names, id_expr->as.ident.name);
  vec_push_arena(p->arena, &s->as.var.exprs, bin);
  s->as.var.is_decl = false;
  s->as.var.is_del = false;
  return s;
}

stmt_t *ny_parse_while_stmt(parser_t *p) {
  token_t tok = p->cur;
  bool attr_unroll = false;
  bool attr_vectorize = false;
  bool attr_nounroll = false;

  /* Parse optional loop attributes: @unroll, @nounroll, @vectorize/@simd */
  if (p->cur.kind == NY_T_AT) {
    parser_advance(p); /* consume '@' */
    if (p->cur.kind >= NY_T_IDENT && p->cur.kind <= NY_T_ENUM) {
      const char *name = p->cur.lexeme;
      size_t namelen = p->cur.len;
      if (namelen == 6 && memcmp(name, "unroll", 6) == 0) {
        attr_unroll = true;
      } else if (namelen == 8 && memcmp(name, "nounroll", 8) == 0) {
        attr_nounroll = true;
      } else if ((namelen == 9 && memcmp(name, "vectorize", 9) == 0) ||
                 (namelen == 4 && memcmp(name, "simd", 4) == 0)) {
        attr_vectorize = true;
      }
      parser_advance(p);
    }
  }

  parser_expect(p, NY_T_WHILE, "'while'", NULL);
  bool has_paren = (p->cur.kind == NY_T_LPAREN);
  if (has_paren)
    parser_advance(p);
  stmt_t *init = NULL;
  if (p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT) {
    init = p_parse_stmt(p);
  }
  expr_t *cond = p_parse_expr(p, 0);
  stmt_t *update = NULL;
  if ((has_paren && p->cur.kind != NY_T_RPAREN) ||
      (!has_paren && (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS))) {
    if (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS) {
      update = parse_incrdecr_stmt(p);
    } else if (has_paren) {
      update = p_parse_stmt(p);
    }
  }
  if (has_paren)
    parser_expect(p, NY_T_RPAREN, "')' after while clause", NULL);
  p->loop_depth++;
  stmt_t *body = ny_parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_WHILE, tok);
  s->as.whl.test = cond;
  s->as.whl.body = body;
  s->as.whl.update = update;
  s->as.whl.init = init;
  s->as.whl.attr_unroll = attr_unroll;
  s->as.whl.attr_vectorize = attr_vectorize;
  s->as.whl.attr_nounroll = attr_nounroll;
  return s;
}

stmt_t *ny_parse_while_stmt_with_attr(parser_t *p, const char *attr_name, size_t attr_len) {
  token_t tok = p->cur;
  bool attr_unroll = (attr_len == 6 && memcmp(attr_name, "unroll", 6) == 0);
  bool attr_nounroll = (attr_len == 8 && memcmp(attr_name, "nounroll", 8) == 0);
  bool attr_vectorize = (attr_len == 9 && memcmp(attr_name, "vectorize", 9) == 0) ||
                         (attr_len == 4 && memcmp(attr_name, "simd", 4) == 0);

  parser_expect(p, NY_T_WHILE, "'while'", NULL);
  bool has_paren = (p->cur.kind == NY_T_LPAREN);
  if (has_paren)
    parser_advance(p);
  stmt_t *init = NULL;
  if (p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT) {
    init = p_parse_stmt(p);
  }
  expr_t *cond = p_parse_expr(p, 0);
  stmt_t *update = NULL;
  if ((has_paren && p->cur.kind != NY_T_RPAREN) ||
      (!has_paren && (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS))) {
    if (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS) {
      update = parse_incrdecr_stmt(p);
    } else if (has_paren) {
      update = p_parse_stmt(p);
    }
  }
  if (has_paren)
    parser_expect(p, NY_T_RPAREN, "')' after while clause", NULL);
  p->loop_depth++;
  stmt_t *body = ny_parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_WHILE, tok);
  s->as.whl.test = cond;
  s->as.whl.body = body;
  s->as.whl.update = update;
  s->as.whl.init = init;
  s->as.whl.attr_unroll = attr_unroll;
  s->as.whl.attr_vectorize = attr_vectorize;
  s->as.whl.attr_nounroll = attr_nounroll;
  return s;
}

stmt_t *ny_parse_for_stmt(parser_t *p) {
  token_t tok = p->cur;
  bool attr_unroll = false;
  bool attr_vectorize = false;
  bool attr_nounroll = false;

  /* Parse optional loop attributes: @unroll, @nounroll, @vectorize/@simd */
  if (p->cur.kind == NY_T_AT) {
    parser_advance(p); /* consume '@' */
    if (p->cur.kind >= NY_T_IDENT && p->cur.kind <= NY_T_ENUM) {
      const char *name = p->cur.lexeme;
      size_t namelen = p->cur.len;
      if (namelen == 6 && memcmp(name, "unroll", 6) == 0) {
        attr_unroll = true;
      } else if (namelen == 8 && memcmp(name, "nounroll", 8) == 0) {
        attr_nounroll = true;
      } else if ((namelen == 9 && memcmp(name, "vectorize", 9) == 0) ||
                 (namelen == 4 && memcmp(name, "simd", 4) == 0)) {
        attr_vectorize = true;
      }
      parser_advance(p);
    }
  }

  parser_expect(p, NY_T_FOR, "'for'", NULL);
  bool has_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    has_paren = true;
    parser_advance(p);
  }
  /* Check for C-style: for(init cond update) — detect by looking for
     def/mut keyword or assignments before any 'in' keyword */
  if (has_paren && (p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT)) {
    /* C-style for loop: for(mut i=0; cond; update) or for(mut i=0 cond update) */
    stmt_t *init = p_parse_stmt(p);
    if (!init) {
      parser_error(p, p->cur, "expected initialization in for loop", NULL);
      return NULL;
    }
    /* Optional semicolon separator */
    if (p->cur.kind == NY_T_SEMI)
      parser_advance(p);
    expr_t *cond = p_parse_expr(p, 0);
    stmt_t *update = NULL;
    /* Optional semicolon separator */
    if (p->cur.kind == NY_T_SEMI)
      parser_advance(p);
    if (p->cur.kind != NY_T_RPAREN) {
      if (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS) {
        update = parse_incrdecr_stmt(p);
      } else {
        update = p_parse_stmt(p);
      }
    }
    if (has_paren)
      parser_expect(p, NY_T_RPAREN, "')' after for clause", NULL);
    p->loop_depth++;
    stmt_t *body = ny_parse_stmt_or_block(p);
    p->loop_depth--;
    stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
    s->as.fr.init = init;
    s->as.fr.cond = cond;
    s->as.fr.update = update;
    s->as.fr.body = body;
    s->as.fr.iter_var = NULL;
    s->as.fr.iter_index_var = NULL;
    s->as.fr.iterable = NULL;
    s->as.fr.iter_by_index = false;
    s->as.fr.attr_unroll = attr_unroll;
    s->as.fr.attr_vectorize = attr_vectorize;
    s->as.fr.attr_nounroll = attr_nounroll;
    return s;
  }
  /* Check if this is iterator-style: for x in ... or for x, i in ... */
  if (p->cur.kind == NY_T_IDENT) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_IN || next.kind == NY_T_COMMA) {
      /* Iterator-style for loop. With a comma, the second name is the
         zero-based iteration index: for value, index in iterable. */
      char *id = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      char *index_id = NULL;
      if (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected index name after ',' in for loop",
                       "use 'for value, index in iterable'");
          return NULL;
        }
        index_id = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
        if (strcmp(id, index_id) == 0) {
          parser_error(p, p->cur, "for loop value and index names must differ",
                       NULL);
          return NULL;
        }
      }
      parser_expect(p, NY_T_IN, "'in' after for loop variable", NULL);
      expr_t *iter = p_parse_expr(p, 0);
      if (has_paren)
        parser_expect(p, NY_T_RPAREN, ")' after iterable", NULL);
      p->loop_depth++;
      stmt_t *body = ny_parse_stmt_or_block(p);
      p->loop_depth--;
      stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
      s->as.fr.iter_var = id;
      s->as.fr.iter_index_var = index_id;
      s->as.fr.iterable = iter;
      s->as.fr.iter_by_index = index_id ? false : has_paren;
      s->as.fr.body = body;
      s->as.fr.init = NULL;
      s->as.fr.cond = NULL;
      s->as.fr.update = NULL;
      s->as.fr.attr_unroll = attr_unroll;
      s->as.fr.attr_vectorize = attr_vectorize;
      s->as.fr.attr_nounroll = attr_nounroll;
      return s;
    }
    if (next.kind == NY_T_ASSIGN || next.kind == NY_T_IDENT) {
      parser_error(p, p->cur, "C-style for loops require parentheses: for(init; cond; update)",
                   "use 'for(mut i=0; i<len(a); ++i)' or 'for x in iterable'");
      while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_LBRACE)
        parser_advance(p);
      if (p->cur.kind == NY_T_RPAREN)
        parser_advance(p);
      return NULL;
    }
  }
  /* Fallback: try iterator-style without 'in' check (error path) */
  if (p->cur.kind != NY_T_IDENT) {
    parser_error(p, p->cur, "for expects loop variable or C-style init",
                 "use 'for x in iterable' or 'for(mut i=0; cond; update)'");
    return NULL;
  }
  /* Default iterator-style */
  char *id = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
  parser_advance(p);
  if (p->cur.kind == NY_T_IN)
    parser_advance(p);
  expr_t *iter = p_parse_expr(p, 0);
  if (has_paren)
    parser_expect(p, NY_T_RPAREN, ")' after condition", NULL);
  p->loop_depth++;
  stmt_t *body = ny_parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
  s->as.fr.iter_var = id;
  s->as.fr.iter_index_var = NULL;
  s->as.fr.iterable = iter;
  s->as.fr.iter_by_index = has_paren;
  s->as.fr.body = body;
  s->as.fr.init = NULL;
  s->as.fr.cond = NULL;
  s->as.fr.update = NULL;
  s->as.fr.attr_unroll = attr_unroll;
  s->as.fr.attr_vectorize = attr_vectorize;
  s->as.fr.attr_nounroll = attr_nounroll;
  return s;
}

stmt_t *ny_parse_try_stmt(parser_t *p) {
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

stmt_t *ny_parse_return_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_RETURN, "'return'", NULL);
  stmt_t *s = stmt_new(p->arena, NY_S_RETURN, tok);
  if (p->cur.kind != NY_T_SEMI && p->cur.kind != NY_T_RBRACE)
    s->as.ret.value = p_parse_expr(p, 0);
  parser_match(p, NY_T_SEMI);
  return s;
}

stmt_t *ny_parse_goto_stmt(parser_t *p) {
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

stmt_t *ny_parse_break_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_BREAK, "'break'", NULL);
  if (p->loop_depth <= 0) {
    parser_error(p, tok, "'break' used outside of a loop", "put this inside a while/for body");
  }
  stmt_t *s = stmt_new(p->arena, NY_S_BREAK, tok);
  parser_match(p, NY_T_SEMI);
  return s;
}

stmt_t *ny_parse_continue_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_CONTINUE, "'continue'", NULL);
  if (p->loop_depth <= 0) {
    parser_error(p, tok, "'continue' used outside of a loop", "put this inside a while/for body");
  }
  stmt_t *s = stmt_new(p->arena, NY_S_CONTINUE, tok);
  parser_match(p, NY_T_SEMI);
  return s;
}
