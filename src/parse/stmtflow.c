#include "priv.h"

stmt_t *ny_parse_stmt_or_block(parser_t *p) {
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

stmt_t *ny_parse_if_stmt(parser_t *p) {
  token_t tok = p->cur;
  if (p->cur.kind == NY_T_IF || p->cur.kind == NY_T_ELIF)
    parser_advance(p);
  else
    parser_expect(p, NY_T_IF, "'if' or 'elif'", NULL);
  expr_t *cond = p_parse_expr(p, 0);
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
  return s;
}

stmt_t *ny_parse_while_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_WHILE, "'while'", NULL);
  expr_t *cond = p_parse_expr(p, 0);
  if (p->cur.kind == NY_T_ASSIGN) {
    parser_error(p, p->cur, "assignment in condition", "did you mean '=='?");
    parser_advance(p);
    p_parse_expr(p, 0);
  }
  p->loop_depth++;
  stmt_t *body = ny_parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_WHILE, tok);
  s->as.whl.test = cond;
  s->as.whl.body = body;
  return s;
}

stmt_t *ny_parse_for_stmt(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_FOR, "'for'", NULL);
  bool has_paren = false;
  if (p->cur.kind == NY_T_LPAREN) {
    has_paren = true;
    parser_advance(p);
    if (p->cur.kind == NY_T_IDENT || p->cur.kind == NY_T_DEF ||
        p->cur.kind == NY_T_MUT) {
      token_t next = parser_peek(p);
      if (next.kind == NY_T_ASSIGN || next.kind == NY_T_IDENT) {
        parser_error(p, p->cur, "C-style for loops are not supported",
                     "use 'for x in iterable' instead");
        while (p->cur.kind != NY_T_RPAREN && p->cur.kind != NY_T_EOF)
          parser_advance(p);
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
  stmt_t *body = ny_parse_stmt_or_block(p);
  p->loop_depth--;
  stmt_t *s = stmt_new(p->arena, NY_S_FOR, tok);
  s->as.fr.iter_var = id;
  s->as.fr.iterable = iter;
  s->as.fr.body = body;
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
    parser_error(p, tok, "'break' used outside of a loop",
                 "put this inside a while/for body");
  }
  stmt_t *s = stmt_new(p->arena, NY_S_BREAK, tok);
  parser_match(p, NY_T_SEMI);
  return s;
}

stmt_t *ny_parse_continue_stmt(parser_t *p) {
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
