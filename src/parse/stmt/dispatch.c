/* Parses a single 'def'/'mut' binding target (optional type + name) and
 * pushes it onto s->as.var.names / s->as.var.types. Shared by the initial
 * binding-target list and by per-binding initializers like
 * 'def P1 = [0, 1], P2 = [1, 0]'. On failure, emits a parser error and
 * leaves cleanup (stmt_free_members) to the caller. */
static bool parse_var_binding_target(parser_t *p, stmt_t *s) {
  const char *var_type = NULL;
  token_t ident = {0};
  if (stmt_looks_type_first_binding(p)) {
    var_type = parse_type_ref(p, "expected type name");
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected variable name after type", NULL);
      if (p->cur.kind != NY_T_EOF)
        parser_advance(p);
      return false;
    }
    ident = p->cur;
    parser_advance(p);
  } else if (((p->cur.kind == NY_T_IDENT || p->cur.kind == NY_T_NUMBER) &&
              (parser_peek(p).kind == NY_T_COLON ||
               parser_peek(p).kind == NY_T_LT)) ||
             p->cur.kind == NY_T_QUESTION || p->cur.kind == NY_T_STAR) {
    var_type = parse_type_ref(p, "expected type name before ':'");
    if (parser_match(p, NY_T_COLON)) {

    }
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected variable name", NULL);
      if (p->cur.kind != NY_T_EOF) parser_advance(p);
      return false;
    }
    ident = p->cur;
    parser_advance(p);
  } else {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected identifier after 'def'",
                   parse_missing_ident_hint(p->cur, "variable"));
      if (p->cur.kind != NY_T_EOF) parser_advance(p);
      return false;
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
  if (mangled) free(final_name);
  vec_push_arena(p->arena, &s->as.var.names, name_s);
  vec_push_arena(p->arena, &s->as.var.types, var_type);
  return true;
}

/* Lookahead used inside a 'def'/'mut' initializer list: after consuming a
 * comma, decide whether what follows is another binding target owning its
 * own initializer ('P2 = ...') rather than just another expression in the
 * same initializer list ('def a, b = 1, 2'). Deliberately bounded to the
 * unambiguous 'IDENT =' shape only: this grammar never parses '=' as an
 * expression operator, so a bare identifier immediately followed by '='
 * can never be the start of a value expression. We do NOT reuse the
 * type-first speculative scan here (stmt_looks_type_first_binding /
 * parse_type_ref) because that scanner has no notion of statement
 * boundaries -- given a dead-end candidate like a trailing NUMBER literal
 * ('..., 0'), it will happily keep scanning into the *next*, unrelated
 * statement looking for a token sequence that satisfies it, and can
 * misfire across that boundary (e.g. mistaking '0' followed by the next
 * statement's 'i = ...' for a fake 'Type name' binding). Keeping this
 * check to a fixed 2-token lookahead avoids that entirely. */
static bool stmt_comma_starts_next_var_binding(parser_t *p) {
  return p->cur.kind == NY_T_IDENT && parser_peek(p).kind == NY_T_ASSIGN;
}

stmt_t *p_parse_stmt(parser_t *p) {
  if (p && p->cur.kind == NY_T_STAR) {
    stmt_t *deref_assign = parse_leading_deref_assign_stmt(p);
    if (deref_assign)
      return deref_assign;
  }
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
  case NY_T_HASH:
    return parse_hash_stmt(p);
  case NY_T_AT: {

    parser_t saved = *p;
    parser_advance(p);
    if (p->cur.kind >= NY_T_IDENT && p->cur.kind <= NY_T_ENUM) {
      const char *name = p->cur.lexeme;
      size_t namelen = p->cur.len;
      bool is_loop_attr = (namelen == 6 && memcmp(name, "unroll", 6) == 0) ||
                          (namelen == 8 && memcmp(name, "nounroll", 8) == 0) ||
                          (namelen == 9 && memcmp(name, "vectorize", 9) == 0) ||
                          (namelen == 4 && memcmp(name, "simd", 4) == 0);
      if (is_loop_attr) {

        parser_advance(p);
        if (p->cur.kind == NY_T_WHILE) {
          *p = saved;
          parser_advance(p);
          parser_advance(p);
          return ny_parse_while_stmt_with_attr(p, name, namelen);
        }
        if (p->cur.kind == NY_T_FOR) {
          *p = saved;
          return ny_parse_for_stmt(p);
        }

        *p = saved;
      } else {

        *p = saved;
      }
    } else {

      *p = saved;
    }

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
        if (!parse_var_binding_target(p, s)) {
          stmt_free_members(s);
          return NULL;
        }
        if (!parser_match(p, NY_T_COMMA)) break;
      }
    }
    if (parser_match(p, NY_T_ASSIGN)) {
      while (true) {
        vec_push_arena(p->arena, &s->as.var.exprs, p_parse_expr(p, 0));
        if (!parser_match(p, NY_T_COMMA))
          break;
        /* Support per-binding initializers: 'def P1 = [0,1], P2 = [1,0]'.
         * A comma inside the initializer list normally starts another
         * expression for the existing binding targets ('def a, b = 1, 2').
         * But if what follows is itself a fresh binding target owning its
         * own '=', treat it as a new binding instead. */
        if (!s->as.var.is_destructure &&
            s->as.var.names.len == s->as.var.exprs.len &&
            stmt_comma_starts_next_var_binding(p)) {
          if (!parse_var_binding_target(p, s)) {
            stmt_free_members(s);
            return NULL;
          }
          if (!parser_match(p, NY_T_ASSIGN)) {
            parser_error(p, p->cur, "expected '=' after variable name", NULL);
            stmt_free_members(s);
            return NULL;
          }
        }
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
                   "write 'with ptr name = value { ... }'");
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
      } else if (lhs->kind == NY_E_DEREF) {
        expr_t *callee = expr_new(p->arena, NY_E_IDENT, ident_tok);
        callee->as.ident.name = arena_strndup(p->arena, "store64_i", 9);
        callee->as.ident.sym_id = ny_intern_str("store64_i", 9);
        expr_t *call = expr_new(p->arena, NY_E_CALL, ident_tok);
        call->as.call.callee = callee;
        vec_push_arena(p->arena, &call->as.call.args,
                       ((call_arg_t){NULL, lhs->as.deref.target}));
        vec_push_arena(p->arena, &call->as.call.args,
                       ((call_arg_t){NULL, rhs}));
        stmt_t *s = stmt_new(p->arena, NY_S_EXPR, ident_tok);
        s->as.expr.expr = call;
        return s;
      } else {
        parser_error(p, ident_tok,
                     "assignment target must be identifier, index, or deref",
                     NULL);
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
  case NY_T_STAR: {
    token_t first = p->cur;
    parser_advance(p);
    expr_t *target = NULL;
    if (p->cur.kind == NY_T_IDENT) {
      token_t ident_tok = p->cur;
      target = expr_new(p->arena, NY_E_IDENT, ident_tok);
      target->as.ident.name =
          arena_strndup(p->arena, ident_tok.lexeme, ident_tok.len);
      target->as.ident.sym_id = ident_tok.sym_id;
      parser_advance(p);
    } else {
      target = p_parse_expr(p, 0);
    }
    if (!target) {
      parser_sync_stmt_boundary(p);
      return NULL;
    }
    if (stmt_token_is_assign_op(p->cur)) {
      token_t assign_tok = p->cur;
      parser_advance(p);
      expr_t *rhs = p_parse_expr(p, 0);
      parser_match(p, NY_T_SEMI);
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
      vec_push_arena(p->arena, &call->as.call.args,
                     ((call_arg_t){NULL, rhs}));
      stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
      s->as.expr.expr = call;
      return s;
    }
    expr_t *deref = expr_new(p->arena, NY_E_DEREF, first);
    deref->as.deref.target = target;
    stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
    s->as.expr.expr = deref;
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
    if (e->kind == NY_E_DEREF && stmt_token_is_assign_op(p->cur)) {
      token_t assign_tok = p->cur;
      parser_advance(p);
      expr_t *rhs = p_parse_expr(p, 0);
      parser_match(p, NY_T_SEMI);
      if (!stmt_assign_op_is_plain(assign_tok)) {
        token_kind bin_kind = stmt_assign_op_binary_kind(assign_tok);
        token_t op_tok = {0};
        expr_t *bin = expr_new(p->arena, NY_E_BINARY, op_tok);
        bin->as.binary.op = parser_token_name(bin_kind);
        bin->as.binary.left = e;
        bin->as.binary.right = rhs;
        rhs = bin;
      }
      expr_t *callee = expr_new(p->arena, NY_E_IDENT, first);
      callee->as.ident.name = arena_strndup(p->arena, "store64_i", 9);
      callee->as.ident.sym_id = ny_intern_str("store64_i", 9);
      expr_t *call = expr_new(p->arena, NY_E_CALL, first);
      call->as.call.callee = callee;
      vec_push_arena(p->arena, &call->as.call.args,
                     ((call_arg_t){NULL, e->as.deref.target}));
      vec_push_arena(p->arena, &call->as.call.args,
                     ((call_arg_t){NULL, rhs}));
      stmt_t *s = stmt_new(p->arena, NY_S_EXPR, first);
      s->as.expr.expr = call;
      return s;
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
