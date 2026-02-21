#include "priv.h"
#include <stdlib.h>

static char *parse_dotted_ident_owned(parser_t *p, const char *first_err,
                                      const char *after_dot_err) {
  if (p->cur.kind != NY_T_IDENT) {
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
  while (parser_match(p, NY_T_DOT)) {
    if (p->cur.kind != NY_T_IDENT) {
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
  }
  buf[len] = '\0';
  return buf;
}

static void parser_sync_stmt_boundary(parser_t *p) {
  while (p->cur.kind != NY_T_EOF && p->cur.kind != NY_T_SEMI &&
         p->cur.kind != NY_T_RBRACE) {
    parser_advance(p);
  }
  if (p->cur.kind == NY_T_SEMI)
    parser_advance(p);
}

static void parse_stmt_append_or_sync(parser_t *p, ny_stmt_list *out) {
  stmt_t *s = p_parse_stmt(p);
  if (s) {
    vec_push_arena(p->arena, out, s);
  } else if (p->had_error) {
    parser_sync_stmt_boundary(p);
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
  char *name = arena_strndup(p->arena, owned, result_len);
  free(owned);
  return name;
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
      pr.type = parse_type_ref(p, "expected type name");
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
    if (p->cur.kind == NY_T_LBRACE) {
      parser_error(p, p->cur, "expected return type",
                   "did you mean to start the body? remove ':' or '->'");
    } else {
      fn_stmt->as.fn.return_type = parse_type_ref(p, "expected return type");
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
      pr.type = parse_type_ref(p, "expected type name");
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
    s->as.use.module = arena_strndup(p->arena, owned, strlen(owned));
    free(owned);
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

static stmt_t *parse_struct(parser_t *p) {
  token_t tok = p->cur;
  bool is_layout = (tok.len == 6 && strncmp(tok.lexeme, "layout", 6) == 0);
  parser_expect(p, NY_T_STRUCT, is_layout ? "'layout'" : "'struct'", NULL);
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
    break;
  }
  if (is_layout) {
    s->as.layout.align_override = align_override;
    s->as.layout.pack = pack;
  } else {
    s->as.struc.align_override = align_override;
    s->as.struc.pack = pack;
  }
  parser_expect(p, NY_T_LBRACE, "'{'", NULL);
  ny_layout_field_list *fields =
      is_layout ? &s->as.layout.fields : &s->as.struc.fields;
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
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

      bool id1_is_type = (strcmp(id1, "int") == 0 || strcmp(id1, "i8") == 0 ||
                          strcmp(id1, "i16") == 0 || strcmp(id1, "i32") == 0 ||
                          strcmp(id1, "i64") == 0 || strcmp(id1, "i128") == 0 ||
                          strcmp(id1, "u8") == 0 || strcmp(id1, "u16") == 0 ||
                          strcmp(id1, "u32") == 0 || strcmp(id1, "u64") == 0 ||
                          strcmp(id1, "u128") == 0 || strcmp(id1, "str") == 0 ||
                          strcmp(id1, "char") == 0 ||
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
      char buf[256];
      char *ptr = buf;
      for (size_t i = 0; i < ptr_depth; i++)
        *ptr++ = '*';
      strcpy(ptr, tname);
      tname = arena_strndup(p->arena, buf, strlen(buf));
    }

    int field_align = 0;
    if (p->cur.kind == NY_T_IDENT && p->cur.len == 5 &&
        strncmp(p->cur.lexeme, "align", 5) == 0) {
      field_align = (int)parse_align_attr(p, "align");
    }

    layout_field_t f_field = {fname, tname, field_align};
    vec_push_arena(p->arena, fields, f_field);

    if (p->cur.kind == NY_T_COMMA)
      parser_advance(p);
  }
  parser_expect(p, NY_T_RBRACE, "'}'", NULL);
  return s;
}

static stmt_t *parse_enum(parser_t *p) {
  token_t tok = p->cur;
  parser_expect(p, NY_T_ENUM, "'enum'", NULL);
  const char *name = parse_qualified_name(p);
  if (!name) {
    return NULL;
  }
  parser_expect(p, NY_T_LBRACE, "'{'", NULL);
  stmt_t *s = stmt_new(p->arena, NY_S_ENUM, tok);
  s->as.enu.name = name;
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    if (p->cur.kind != NY_T_IDENT) {
      parser_error(p, p->cur, "expected enum variant name", NULL);
      break;
    }
    stmt_enum_item_t item = {0};
    item.name = arena_strndup(p->arena, p->cur.lexeme, p->cur.len);
    parser_advance(p);
    if (parser_match(p, NY_T_ASSIGN)) {
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
    parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
  }

  if (end_kind == NY_T_RPAREN) {
    parser_expect(p, NY_T_RPAREN, "')'", NULL);
    if (p->cur.kind != NY_T_LBRACE && p->block_depth == 0) {
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
        parse_stmt_append_or_sync(p, &mod_stmt->as.module.body);
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

// Parse a single attribute like @naked or @extern("printf")
static attribute_t parse_attr(parser_t *p) {
  if (p->cur.kind != NY_T_IDENT) {
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

// Parse a macro statement like: task { ... } or parallel(8) { ... }
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
    return parse_struct(p);
  case NY_T_ENUM:
    return parse_enum(p);
  case NY_T_EXTERN:
    return parse_extern(p);
  case NY_T_FN:
    return parse_func(p);
  case NY_T_AT: {
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
    stmt_t *func = parse_func(p);
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
  case NY_T_FOR:
    return ny_parse_for_stmt(p);
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
        if (p->cur.kind != NY_T_IDENT) {
          parser_error(p, p->cur, "expected identifier after 'def'", NULL);
          if (p->cur.kind != NY_T_EOF)
            parser_advance(p);
          stmt_free_members(s);
          return NULL;
        }
        token_t ident = p->cur;
        parser_advance(p);

        char *final_name = (char *)ident.lexeme;
        size_t nlen = ident.len;
        bool mangled = false;
        if (p->block_depth == 0 && p->current_module) {
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

        const char *var_type = NULL;
        if (parser_match(p, NY_T_COLON)) {
          var_type = parse_type_ref(p, "expected type name after ':'");
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
    token_t next = parser_peek(p);

    if (next.kind == NY_T_LBRACE) {
      return parse_macro_stmt(p);
    }

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
  while (p->cur.kind != NY_T_RBRACE && p->cur.kind != NY_T_EOF) {
    parse_stmt_append_or_sync(p, &blk->as.block.body);
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
  return prog;
}
