#include "priv.h"

stmt_t *ny_parse_stmt_or_block(parser_t *p) {
  if (p->cur.kind == NY_T_LBRACE)
    return p_parse_block(p);
  token_t tok = p->cur;
  stmt_t *s = p_parse_stmt(p);
  if (!s) return NULL;
  stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, tok);
  vec_push_arena(p->arena, &blk->as.block.body, s);
  return blk;
}

static stmt_t *parse_if_internal(parser_t *p, token_t tok) {
  stmt_t *s = stmt_new(p->arena, NY_S_IF, tok);
  s->as.iff.init = NULL;

  bool header_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_DEF || next.kind == NY_T_MUT) {
      header_paren = true;
      parser_advance(p);
      s->as.iff.init = p_parse_stmt(p);
      parser_match(p, NY_T_SEMI);
    }
  } else if (p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT) {
    s->as.iff.init = p_parse_stmt(p);
    parser_match(p, NY_T_SEMI);
  }

  s->as.iff.test = p_parse_expr(p, 0);

  if (header_paren) {
    parser_expect(p, NY_T_RPAREN, "')' after condition", NULL);
  }

  s->as.iff.conseq = ny_parse_stmt_or_block(p);

  if (parser_match(p, NY_T_ELSE)) {
    if (p->cur.kind == NY_T_IF) {
      s->as.iff.alt = ny_parse_if_stmt(p);
    } else if (p->cur.kind == NY_T_ELIF) {
      token_t elif_tok = p->cur;
      parser_advance(p);
      s->as.iff.alt = parse_if_internal(p, elif_tok);
    } else {
      s->as.iff.alt = ny_parse_stmt_or_block(p);
    }
  } else if (p->cur.kind == NY_T_ELIF) {
    token_t elif_tok = p->cur;
    parser_advance(p);
    s->as.iff.alt = parse_if_internal(p, elif_tok);
  } else {
    s->as.iff.alt = NULL;
  }
  return s;
}

stmt_t *ny_parse_if_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_IF, "'if'", NULL);
  return parse_if_internal(p, tok);
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

  if (p->cur.kind == NY_T_AT) {
    parser_advance(p);
    while (p->cur.kind >= NY_T_IDENT && p->cur.kind <= NY_T_ENUM) {
      const char *name = p->cur.lexeme;
      size_t namelen = p->cur.len;
      if (namelen == 6 && memcmp(name, "unroll", 6) == 0) attr_unroll = true;
      else if (namelen == 8 && memcmp(name, "nounroll", 8) == 0) attr_nounroll = true;
      else if ((namelen == 9 && memcmp(name, "vectorize", 9) == 0) ||
               (namelen == 4 && memcmp(name, "simd", 4) == 0)) attr_vectorize = true;
      else break;
      parser_advance(p);
      if (!parser_match(p, NY_T_COMMA)) break;
    }
  }

  parser_expect(p, NY_T_WHILE, "'while'", NULL);
  stmt_t *s = stmt_new(p->arena, NY_S_WHILE, tok);
  s->as.whl.init = NULL;

  bool header_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_DEF || next.kind == NY_T_MUT) {
      header_paren = true;
      parser_advance(p);
      s->as.whl.init = p_parse_stmt(p);
      parser_match(p, NY_T_SEMI);
    }
  } else if (p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT) {
    s->as.whl.init = p_parse_stmt(p);
    parser_match(p, NY_T_SEMI);
  }

  s->as.whl.test = p_parse_expr(p, 0);

  if (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS) {
    s->as.whl.update = parse_incrdecr_stmt(p);
  } else if (header_paren && p->cur.kind != NY_T_RPAREN) {
    s->as.whl.update = p_parse_stmt(p);
  }

  if (header_paren) {
    parser_expect(p, NY_T_RPAREN, "')' after while condition", NULL);
  }

  p->loop_depth++;
  s->as.whl.body = ny_parse_stmt_or_block(p);
  p->loop_depth--;

  s->as.whl.attr_unroll = attr_unroll;
  s->as.whl.attr_vectorize = attr_vectorize;
  s->as.whl.attr_nounroll = attr_nounroll;
  return s;
}

stmt_t *ny_parse_while_stmt_with_attr(parser_t *p, const char *attr_name, size_t attr_len) {

  stmt_t *s = ny_parse_while_stmt(p);
  if (s) {
    if (attr_len == 6 && memcmp(attr_name, "unroll", 6) == 0) s->as.whl.attr_unroll = true;
    else if (attr_len == 8 && memcmp(attr_name, "nounroll", 8) == 0) s->as.whl.attr_nounroll = true;
    else if ((attr_len == 9 && memcmp(attr_name, "vectorize", 9) == 0) ||
             (attr_len == 4 && memcmp(attr_name, "simd", 4) == 0)) s->as.whl.attr_vectorize = true;
  }
  return s;
}

stmt_t *ny_parse_for_stmt(parser_t *p) {
  token_t tok = p->cur;
  bool attr_unroll = false;
  bool attr_vectorize = false;
  bool attr_nounroll = false;

  if (p->cur.kind == NY_T_AT) {
    parser_advance(p);
    while (p->cur.kind >= NY_T_IDENT && p->cur.kind <= NY_T_ENUM) {
      const char *name = p->cur.lexeme;
      size_t namelen = p->cur.len;
      if (namelen == 6 && memcmp(name, "unroll", 6) == 0) attr_unroll = true;
      else if (namelen == 8 && memcmp(name, "nounroll", 8) == 0) attr_nounroll = true;
      else if ((namelen == 9 && memcmp(name, "vectorize", 9) == 0) ||
               (namelen == 4 && memcmp(name, "simd", 4) == 0)) attr_vectorize = true;
      else break;
      parser_advance(p);
      if (!parser_match(p, NY_T_COMMA)) break;
    }
  }

  parser_expect(p, NY_T_FOR, "'for'", NULL);
  bool has_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_DEF || next.kind == NY_T_MUT) {
       has_paren = true;
       parser_advance(p);
    }
  }

  if (has_paren || p->cur.kind == NY_T_DEF || p->cur.kind == NY_T_MUT) {

    stmt_t *init = p_parse_stmt(p);
    if (!init) return NULL;
    if (p->cur.kind == NY_T_SEMI) parser_advance(p);
    expr_t *cond = p_parse_expr(p, 0);
    if (p->cur.kind == NY_T_SEMI) parser_advance(p);
    stmt_t *update = NULL;
    if (p->cur.kind != NY_T_LBRACE && p->cur.kind != NY_T_RPAREN) {
      if (p->cur.kind == NY_T_PLUS_PLUS || p->cur.kind == NY_T_MINUS_MINUS) {
        update = parse_incrdecr_stmt(p);
      } else {
        update = p_parse_stmt(p);
      }
    }
    if (has_paren) parser_expect(p, NY_T_RPAREN, "')' after for header", NULL);
    p->loop_depth++;
    stmt_t *body = ny_parse_stmt_or_block(p);
    p->loop_depth--;
    stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
    s->as.fr.init = init;
    s->as.fr.cond = cond;
    s->as.fr.update = update;
    s->as.fr.body = body;
    s->as.fr.attr_unroll = attr_unroll;
    s->as.fr.attr_vectorize = attr_vectorize;
    s->as.fr.attr_nounroll = attr_nounroll;
    return s;
  }

  bool iter_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_IDENT) {
      iter_paren = true;
      parser_advance(p);
    }
  }

  if (p->cur.kind == NY_T_IDENT) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_IN || next.kind == NY_T_COMMA) {
      char *id = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
      char *index_id = NULL;
      if (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind != NY_T_IDENT) return NULL;
        index_id = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
      }
      parser_expect(p, NY_T_IN, "'in' after for loop variable", NULL);
      expr_t *iter = p_parse_expr(p, 0);
      if (iter_paren) parser_expect(p, NY_T_RPAREN, "')' after for header", NULL);
      p->loop_depth++;
      stmt_t *body = ny_parse_stmt_or_block(p);
      p->loop_depth--;
      stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
      s->as.fr.iter_var = id;
      s->as.fr.iter_index_var = index_id;
      s->as.fr.iterable = iter;
      s->as.fr.body = body;
      s->as.fr.attr_unroll = attr_unroll;
      s->as.fr.attr_vectorize = attr_vectorize;
      s->as.fr.attr_nounroll = attr_nounroll;
      return s;
    }
  }

  parser_error(p, p->cur, "for expects an iterator binding",
               "use Nytrix iterator syntax such as `for x in 0..n { ... }`; ';' starts a comment");
  return NULL;
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
    if (p->cur.kind == NY_T_IDENT) {
      err = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
  } else if (p->cur.kind == NY_T_IDENT) {
    token_t next = parser_peek(p);
    if (next.kind == NY_T_LBRACE) {
      err = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
      parser_advance(p);
    }
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
  if (p->cur.kind != NY_T_IDENT) return NULL;
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
