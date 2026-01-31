#include "priv.h"

static int precedence(token_kind kind) {
  switch (kind) {
  case NY_T_OR:
    return 1;
  case NY_T_AND:
    return 2;
  case NY_T_EQ:
  case NY_T_NEQ:
    return 3;
  case NY_T_LT:
  case NY_T_GT:
  case NY_T_LE:
  case NY_T_GE:
    return 4;
  case NY_T_PLUS:
  case NY_T_MINUS:
    return 5;
  case NY_T_STAR:
  case NY_T_SLASH:
  case NY_T_PERCENT:
    return 6;
  case NY_T_BITOR:
  case NY_T_BITAND:
  case NY_T_BITXOR:
  case NY_T_LSHIFT:
  case NY_T_RSHIFT:
    return 7;
  default:
    return 0;
  }
}

static const char *decode_fstring_part(parser_t *p, const char *s, size_t len,
                                       size_t *out_len) {
  return parser_unescape_string(p->arena, s, len, out_len);
}

static expr_t *parse_fstring(parser_t *p, token_t tok) {
  parser_advance(p);
  expr_t *e = expr_new(p->arena, NY_E_FSTRING, tok);
  const char *s = tok.lexeme;
  size_t len = tok.len;
  // Skip 'f' prefix
  s++;
  len--;
  char quote = *s;
  bool triple = (len >= 6 && s[1] == quote && s[2] == quote);
  s += triple ? 3 : 1;
  len -= triple ? 6 : 2;
  size_t i = 0;
  while (i < len) {
    if (s[i] == '{') {
      i++;
      size_t start = i;
      int depth = 1;
      while (i < len && depth > 0) {
        if (s[i] == '{')
          depth++;
        else if (s[i] == '}')
          depth--;
        if (depth > 0)
          i++;
      }
      if (depth == 0) {
        char *expr_str = arena_strndup(p->arena, s + start, i - start);
        parser_t sub;
        parser_init_with_arena(&sub, expr_str, p->lex.filename, p->arena);
        expr_t *sub_e = p_parse_expr(&sub, 0);
        // Keep arena_t state in sync called parser_init_with_arena which takes
        // arena_t ptr But sub.arena_t is same pointer. Allocations happened on
        // it. p->arena doesn't change typically.
        fstring_part_t part = {.kind = NY_FSP_EXPR, .as.e = sub_e};
        vec_push_arena(p->arena, &e->as.fstring.parts, part);
        i++; // skip '}'
      } else {
        parser_error(p, tok, "unterminated interpolation in f-string", NULL);
        break;
      }
    } else {
      size_t start = i;
      while (i < len && s[i] != '{') {
        if (s[i] == '\\' && i + 1 < len)
          i += 2;
        else
          i++;
      }
      fstring_part_t part;
      part.kind = NY_FSP_STR;
      part.as.s.data =
          decode_fstring_part(p, s + start, i - start, &part.as.s.len);
      vec_push_arena(p->arena, &e->as.fstring.parts, part);
    }
  }
  return e;
}

static expr_t *parse_primary(parser_t *p) {
  token_t tok = p->cur;
  switch (tok.kind) {
  case NY_T_COMPTIME: {
    parser_advance(p);
    stmt_t *body = NULL;
    if (p->cur.kind == NY_T_LBRACE) {
      body = p_parse_block(p);
    } else {
      expr_t *val = p_parse_expr(p, 0);
      stmt_t *ret = stmt_new(p->arena, NY_S_RETURN, tok);
      ret->as.ret.value = val;
      body = stmt_new(p->arena, NY_S_BLOCK, tok);
      vec_push_arena(p->arena, &body->as.block.body, ret);
    }
    expr_t *e = expr_new(p->arena, NY_E_COMPTIME, tok);
    e->as.comptime_expr.body = body;
    return e;
  }
  case NY_T_IDENT: {
    parser_advance(p);
    /* Omniscience: Help users from other languages */
    if (tok.len == 4 && strncmp(tok.lexeme, "null", 4) == 0) {
      parser_error(p, tok, "unrecognised identifier 'null'",
                   "did you mean '0' or 'nil'?");
    } else if (tok.len == 4 && strncmp(tok.lexeme, "None", 4) == 0) {
      parser_error(p, tok, "unrecognised identifier 'None'",
                   "did you mean '0' or 'nil'?");
    }
    expr_t *id = expr_new(p->arena, NY_E_IDENT, tok);
    id->as.ident.name = arena_strndup(p->arena, tok.lexeme, tok.len);
    return id;
  }
  case NY_T_NUMBER: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    bool is_hex = (tok.len > 2 && tok.lexeme[0] == '0' &&
                   (tok.lexeme[1] == 'x' || tok.lexeme[1] == 'X'));
    if (!is_hex &&
        (memchr(tok.lexeme, '.', tok.len) || memchr(tok.lexeme, 'e', tok.len) ||
         memchr(tok.lexeme, 'E', tok.len))) {
      lit->as.literal.kind = NY_LIT_FLOAT;
      lit->as.literal.as.f = strtod(tok.lexeme, NULL);
    } else {
      lit->as.literal.kind = NY_LIT_INT;
      lit->as.literal.as.i = strtoll(tok.lexeme, NULL, 0);
    }
    return lit;
  }
  case NY_T_TRUE:
  case NY_T_FALSE: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    lit->as.literal.kind = NY_LIT_BOOL;
    lit->as.literal.as.b = tok.kind == NY_T_TRUE;
    return lit;
  }
  case NY_T_NIL: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    lit->as.literal.kind = NY_LIT_INT;
    lit->as.literal.as.i = 0;
    return lit;
  }
  case NY_T_STRING: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LITERAL, tok);
    lit->as.literal.kind = NY_LIT_STR;
    size_t slen = 0;
    const char *sval = parser_decode_string(p, tok, &slen);
    lit->as.literal.as.s.data = sval;
    lit->as.literal.as.s.len = slen;
    return lit;
  }
  case NY_T_FSTRING:
    return parse_fstring(p, tok);
  case NY_T_MATCH: {
    stmt_t *s = p_parse_match(p);
    expr_t *e = expr_new(p->arena, NY_E_MATCH, tok);
    e->as.match = s->as.match;
    return e;
  }
  case NY_T_DOT: {
    parser_advance(p);
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "member access expects identifier", NULL);
      return NULL;
    }
    expr_t *e = expr_new(p->arena, NY_E_INFERRED_MEMBER, tok);
    e->as.inferred_member.name =
        arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    return e;
  }
  case NY_T_LPAREN: {
    parser_advance(p);
    if (parser_match(p, NY_T_RPAREN)) {
      return expr_new(p->arena, NY_E_TUPLE, tok);
    }
    expr_t *inner = p_parse_expr(p, 0);
    if (p->cur.kind == NY_T_COMMA) {
      expr_t *tup = expr_new(p->arena, NY_E_TUPLE, tok);
      vec_push_arena(p->arena, &tup->as.list_like, inner);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind == NY_T_RPAREN)
          break;
        vec_push_arena(p->arena, &tup->as.list_like, p_parse_expr(p, 0));
      }
      parser_expect(p, NY_T_RPAREN, NULL, NULL);
      return tup;
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    return inner;
  }
  case NY_T_LBRACK: {
    parser_advance(p);
    expr_t *lit = expr_new(p->arena, NY_E_LIST, tok);
    if (p->cur.kind != NY_T_RBRACK) {
      while (true) {
        expr_t *item = p_parse_expr(p, 0);
        vec_push_arena(p->arena, &lit->as.list_like, item);
        if (!parser_match(p, NY_T_COMMA))
          break;
        if (p->cur.kind == NY_T_RBRACK)
          break;
      }
    }
    parser_expect(p, NY_T_RBRACK, NULL, NULL);
    return lit;
  }
  case NY_T_LBRACE: {
    parser_advance(p);
    if (p->cur.kind == NY_T_RBRACE) {
      parser_expect(p, NY_T_RBRACE, NULL, NULL);
      expr_t *set = expr_new(p->arena, NY_E_SET, tok);
      return set;
    }
    expr_t *first = p_parse_expr(p, 0);
    if (parser_match(p, NY_T_COLON)) {
      expr_t *dict = expr_new(p->arena, NY_E_DICT, tok);
      dict_pair_t pair = {first, p_parse_expr(p, 0)};
      vec_push_arena(p->arena, &dict->as.dict.pairs, pair);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind == NY_T_RBRACE)
          break;
        expr_t *k = p_parse_expr(p, 0);
        parser_expect(p, NY_T_COLON, NULL, NULL);
        expr_t *v = p_parse_expr(p, 0);
        pair.key = k;
        pair.value = v;
        vec_push_arena(p->arena, &dict->as.dict.pairs, pair);
      }
      parser_expect(p, NY_T_RBRACE, NULL, NULL);
      return dict;
    } else {
      expr_t *set = expr_new(p->arena, NY_E_SET, tok);
      vec_push_arena(p->arena, &set->as.list_like, first);
      while (parser_match(p, NY_T_COMMA)) {
        if (p->cur.kind == NY_T_RBRACE)
          break;
        vec_push_arena(p->arena, &set->as.list_like, p_parse_expr(p, 0));
      }
      parser_expect(p, NY_T_RBRACE, NULL, NULL);
      return set;
    }
  }
  case NY_T_ASM: {
    parser_advance(p);
    parser_expect(p, NY_T_LPAREN, NULL, NULL);
    token_t code_tok = p->cur;
    parser_expect(p, NY_T_STRING, "assembly code string", NULL);
    size_t code_len;
    const char *code = parser_decode_string(p, code_tok, &code_len);
    const char *constraints = "";
    if (parser_match(p, NY_T_COMMA)) {
      token_t constr_tok = p->cur;
      parser_expect(p, NY_T_STRING, "constraints string", NULL);
      size_t constr_len;
      constraints = parser_decode_string(p, constr_tok, &constr_len);
    }
    expr_t *e = expr_new(p->arena, NY_E_ASM, tok);
    e->as.as_asm.code = code;
    e->as.as_asm.constraints = constraints;
    while (parser_match(p, NY_T_COMMA)) {
      vec_push_arena(p->arena, &e->as.as_asm.args, p_parse_expr(p, 0));
    }
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    return e;
  }
  case NY_T_EMBED: {
    parser_advance(p);
    parser_expect(p, NY_T_LPAREN, NULL, NULL);
    token_t path_tok = p->cur;
    parser_expect(p, NY_T_STRING, "file path string", NULL);
    size_t path_len;
    const char *path = parser_decode_string(p, path_tok, &path_len);
    parser_expect(p, NY_T_RPAREN, NULL, NULL);
    expr_t *e = expr_new(p->arena, NY_E_EMBED, tok);
    e->as.embed.path = path;
    return e;
  }
  case NY_T_LAMBDA:
  case NY_T_FN: {
    bool is_fn = tok.kind == NY_T_FN;
    parser_advance(p);
    parser_expect(p, NY_T_LPAREN, NULL, NULL);
    expr_t *lam = expr_new(p->arena, is_fn ? NY_E_FN : NY_E_LAMBDA, tok);
    while (p->cur.kind != NY_T_RPAREN) {
      if (parser_match(p, NY_T_ELLIPSIS)) {
        lam->as.lambda.is_variadic = true;
      }
      param_t pr = {0};
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "param must be identifier", NULL);
        return lam;
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
      vec_push_arena(p->arena, &lam->as.lambda.params, pr);
      if (lam->as.lambda.is_variadic) {
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
    if (parser_match(p, NY_T_COLON)) {
      if (p->cur.kind != NY_T_IDENT)
        parser_error(p, p->cur, "expected return type", NULL);
      else {
        lam->as.lambda.return_type =
            arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
        parser_advance(p);
      }
    }
    lam->as.lambda.body = p_parse_block(p);
    return lam;
  }
  default:
    if (tok.kind == NY_T_ASSIGN) {
      parser_error(p, tok, "unexpected '='", "did you mean '=='?");
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "unexpected token '%s'",
               parser_token_name(tok.kind));
      parser_error(p, tok, msg, NULL);
    }
    return NULL;
  }
}

static expr_t *parse_postfix(parser_t *p) {
  expr_t *expr = parse_primary(p);
  for (;;) {
    if (p->cur.kind == NY_T_LPAREN) {
      parser_advance(p);
      expr_t *call = expr_new(p->arena, NY_E_CALL, p->cur);
      call->as.call.callee = expr;
      while (p->cur.kind != NY_T_RPAREN) {
        call_arg_t arg = {0};
        if (p->cur.kind == NY_T_IDENT && parser_peek(p).kind == NY_T_ASSIGN) {
          arg.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
          parser_advance(p); // name
          parser_advance(p); // '='
          arg.val = p_parse_expr(p, 0);
        } else {
          arg.val = p_parse_expr(p, 0);
        }
        vec_push_arena(p->arena, &call->as.call.args, arg);
        if (!parser_match(p, NY_T_COMMA))
          break;
      }
      parser_expect(p, NY_T_RPAREN, NULL, NULL);
      expr = call;
    } else if (p->cur.kind == NY_T_DOT) {
      parser_advance(p);
      if (p->cur.kind != NY_T_IDENT) {
        parser_error(p, p->cur, "member access expects identifier", NULL);
        return expr;
      }
      token_t id_tok = p->cur;
      char *name = arena_strndup(p->arena, id_tok.lexeme, id_tok.len);
      parser_advance(p);
      expr_t *mc = expr_new(p->arena, NY_E_MEMCALL, id_tok);
      mc->as.memcall.target = expr;
      mc->as.memcall.name = name;
      if (p->cur.kind == NY_T_LPAREN) {
        parser_advance(p);
        while (p->cur.kind != NY_T_RPAREN) {
          call_arg_t arg = {0};
          if (p->cur.kind == NY_T_IDENT && parser_peek(p).kind == NY_T_ASSIGN) {
            arg.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
            parser_advance(p); // name
            parser_advance(p); // '='
            arg.val = p_parse_expr(p, 0);
          } else {
            arg.val = p_parse_expr(p, 0);
          }
          vec_push_arena(p->arena, &mc->as.memcall.args, arg);
          if (!parser_match(p, NY_T_COMMA))
            break;
        }
        parser_expect(p, NY_T_RPAREN, NULL, NULL);
      }
      expr = mc;
    } else if (p->cur.kind == NY_T_LBRACK) {
      parser_advance(p);
      expr_t *idx = expr_new(p->arena, NY_E_INDEX, p->cur);
      idx->as.index.target = expr;
      if (p->cur.kind != NY_T_RBRACK) {
        if (p->cur.kind == NY_T_COLON) {
          idx->as.index.start = NULL;
        } else {
          idx->as.index.start = p_parse_expr(p, 0);
        }
        if (parser_match(p, NY_T_COLON)) {
          if (p->cur.kind == NY_T_COLON) {
            expr_t *sent = expr_new(p->arena, NY_E_LITERAL, p->cur);
            sent->as.literal.kind = NY_LIT_INT;
            sent->as.literal.as.i = 0x3fffffff;
            idx->as.index.stop = sent;
          } else if (p->cur.kind != NY_T_RBRACK) {
            idx->as.index.stop = p_parse_expr(p, 0);
          } else {
            expr_t *sent = expr_new(p->arena, NY_E_LITERAL, p->cur);
            sent->as.literal.kind = NY_LIT_INT;
            sent->as.literal.as.i = 0x3fffffff;
            idx->as.index.stop = sent;
          }
          if (parser_match(p, NY_T_COLON)) {
            if (p->cur.kind != NY_T_RBRACK)
              idx->as.index.step = p_parse_expr(p, 0);
          }
        }
      }
      parser_expect(p, NY_T_RBRACK, NULL, NULL);
      expr = idx;
    } else {
      break;
    }
  }
  return expr;
}

static expr_t *parse_unary(parser_t *p) {
  if (p->cur.kind == NY_T_MINUS || p->cur.kind == NY_T_NOT ||
      p->cur.kind == NY_T_BITNOT) {
    token_t tok = p->cur;
    parser_advance(p);
    expr_t *expr = expr_new(p->arena, NY_E_UNARY, tok);
    if (tok.kind == NY_T_MINUS)
      expr->as.unary.op = "-";
    else if (tok.kind == NY_T_NOT)
      expr->as.unary.op = "!";
    else
      expr->as.unary.op = "~";
    expr->as.unary.right = parse_unary(p);
    return expr;
  }
  return parse_postfix(p);
}

expr_t *p_parse_expr(parser_t *p, int prec) {
  expr_t *left = parse_unary(p);
  while (true) {
    if (prec < 1 && p->cur.kind == NY_T_QUESTION) {
      token_t tok = p->cur;
      parser_advance(p);
      expr_t *true_expr = p_parse_expr(p, 0);
      parser_expect(p, NY_T_COLON, ":'", "ternary operator requires ':'");
      expr_t *false_expr = p_parse_expr(p, 0);
      expr_t *ternary = expr_new(p->arena, NY_E_TERNARY, tok);
      ternary->as.ternary.cond = left;
      ternary->as.ternary.true_expr = true_expr;
      ternary->as.ternary.false_expr = false_expr;
      left = ternary;
      continue;
    }
    int pcur = precedence(p->cur.kind);
    if (pcur < prec || pcur == 0)
      break;
    token_t op = p->cur;
    parser_advance(p);
    expr_t *right = p_parse_expr(p, pcur + 1);
    expr_t *bin;
    if (op.kind == NY_T_AND || op.kind == NY_T_OR) {
      bin = expr_new(p->arena, NY_E_LOGICAL, op);
      bin->as.logical.op = (op.kind == NY_T_AND) ? "&&" : "||";
      bin->as.logical.left = left;
      bin->as.logical.right = right;
    } else {
      bin = expr_new(p->arena, NY_E_BINARY, op);
      bin->as.binary.op = arena_strndup(p->arena, op.lexeme, op.len);
      bin->as.binary.left = left;
      bin->as.binary.right = right;
    }
    left = bin;
  }
  return left;
}
