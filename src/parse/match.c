#include "priv.h"

stmt_t *p_parse_match(parser_t *p) {
  token_t tok = p->cur;
  parser_advance(p);
  stmt_t *s = stmt_new(p->arena, NY_S_MATCH, tok);
  s->as.match.test = p_parse_expr(p, 0);
  parser_expect(p, NY_T_LBRACE, "'{'", NULL);
  s->as.match.default_conseq = NULL;
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind == NY_T_ELSE) {
      parser_advance(p);
      s->as.match.default_conseq = p_parse_block(p);
    } else {
      match_arm_t arm;
      memset(&arm, 0, sizeof(arm));
      expr_t *first = p_parse_expr(p, 0);
      vec_push_arena(p->arena, &arm.patterns, first);
      while (1) {
        if (parser_match(p, NY_T_COMMA)) {
          expr_t *pat = p_parse_expr(p, 0);
          vec_push_arena(p->arena, &arm.patterns, pat);
          continue;
        }
        if (p->cur.kind == NY_T_ARROW || p->cur.kind == NY_T_COLON ||
            p->cur.kind == NY_T_LBRACE || p->cur.kind == NY_T_RBRACE ||
            p->cur.kind == NY_T_ELSE || p->cur.kind == NY_T_EOF) {
          break;
        }
        expr_t *pat = p_parse_expr(p, 0);
        vec_push_arena(p->arena, &arm.patterns, pat);
      }
      if (parser_match(p, NY_T_ARROW)) {
        if (p->cur.kind == NY_T_LBRACE) {
          arm.conseq = p_parse_block(p);
        } else {
          token_t etok = p->cur;
          expr_t *e = p_parse_expr(p, 0);
          parser_match(p, NY_T_SEMI);
          stmt_t *blk = stmt_new(p->arena, NY_S_BLOCK, etok);
          stmt_t *es = stmt_new(p->arena, NY_S_EXPR, etok);
          es->as.expr.expr = e;
          vec_push_arena(p->arena, &blk->as.block.body, es);
          arm.conseq = blk;
        }
      } else if (p->cur.kind == NY_T_LBRACE) {
        arm.conseq = p_parse_block(p);
      } else {
        parser_error(p, p->cur, "expected '->' or block after case patterns",
                     NULL);
        arm.conseq = p_parse_block(p);
      }
      vec_push_arena(p->arena, &s->as.match.arms, arm);
    }
  }
  parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  return s;
}
