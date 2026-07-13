static const char *ny_dump_dir(const ny_options *opt) {
  if (opt && opt->dump_dir && *opt->dump_dir)
    return opt->dump_dir;
  return "build/debug";
}

static void ny_dump_path(char *out, size_t out_len, const ny_options *opt,
                         const char *name) {
  if (!out || out_len == 0) {
    return;
  }
  ny_join_path(out, out_len, ny_dump_dir(opt), name ? name : "");
}

static void ny_ensure_parent_dir_for_path(const char *path) {
  if (!path || !*path)
    return;
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
#ifdef _WIN32
  char *bslash = strrchr(tmp, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (!slash)
    return;
  if (slash == tmp)
    return;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
}


static void ny_write_ir_stats_file(const ny_options *opt, const char *name,
                                   LLVMModuleRef module) {
  if (!module || !name || !*name)
    return;
  ny_ir_stats_t st = {0};
  ny_collect_ir_stats(module, &st);
  char path[4096];
  ny_dump_path(path, sizeof(path), opt, name);
  char buf[1024];
  int n = snprintf(
      buf, sizeof(buf),
      "scope=%s\nfuncs=%" PRIu64 "\nblocks=%" PRIu64 "\ninsts=%" PRIu64
      "\nallocas=%" PRIu64 "\nphis=%" PRIu64 "\n",
      (opt && opt->dump_scope == NY_DUMP_SCOPE_LIB)
          ? "lib"
          : ((opt && opt->dump_scope == NY_DUMP_SCOPE_BOTH) ? "both"
                                                            : "program"),
      st.funcs, st.blocks, st.insts, st.allocas, st.phis);
  if (n > 0)
    ny_write_file(path, buf, (size_t)n);
}

static const char *ny_stage_name(ny_stop_after_stage_t stage) {
  switch (stage) {
  case NY_STOP_AFTER_PARSE:
    return "parse";
  case NY_STOP_AFTER_HM:
    return "hm";
  case NY_STOP_AFTER_TRAIT:
    return "trait";
  case NY_STOP_AFTER_FLOW:
    return "flow";
  case NY_STOP_AFTER_ABI:
    return "abi";
  case NY_STOP_AFTER_OPT:
    return "opt";
  case NY_STOP_AFTER_NONE:
  default:
    return "none";
  }
}

static const char *ny_stage_schema(ny_stop_after_stage_t stage) {
  switch (stage) {
  case NY_STOP_AFTER_PARSE:
    return "ast.v1";
  case NY_STOP_AFTER_HM:
    return "typed_ast.v1";
  case NY_STOP_AFTER_TRAIT:
    return "resolved.v1";
  case NY_STOP_AFTER_FLOW:
    return "refined.v1";
  case NY_STOP_AFTER_ABI:
    return "lowered.v1";
  case NY_STOP_AFTER_OPT:
    return "optimized.v1";
  case NY_STOP_AFTER_NONE:
  default:
    return "artifact.v1";
  }
}

static const char *ny_stage_default_file(ny_stop_after_stage_t stage) {
  switch (stage) {
  case NY_STOP_AFTER_PARSE:
    return "ast.v1.json";
  case NY_STOP_AFTER_HM:
    return "typed_ast.v1.json";
  case NY_STOP_AFTER_TRAIT:
    return "resolved.v1.json";
  case NY_STOP_AFTER_FLOW:
    return "refined.v1.json";
  case NY_STOP_AFTER_ABI:
    return "lowered.v1.json";
  case NY_STOP_AFTER_OPT:
    return "optimized.v1.json";
  case NY_STOP_AFTER_NONE:
  default:
    return "artifact.v1.json";
  }
}

static bool ny_stop_after_is(const ny_options *opt,
                             ny_stop_after_stage_t stage) {
  return opt && opt->stop_after == stage;
}

static void ny_stage_append(char **buf, size_t *len, size_t *cap,
                            const char *fmt, ...) {
  if (!buf || !len || !cap || !fmt)
    return;
  if (!*buf) {
    *cap = 2048;
    *buf = malloc(*cap);
    if (!*buf)
      return;
    (*buf)[0] = '\0';
    *len = 0;
  }
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0)
      return;
    if (*len + (size_t)n < *cap) {
      *len += (size_t)n;
      return;
    }
    size_t new_cap = *cap * 2 + (size_t)n + 1;
    char *tmp = realloc(*buf, new_cap);
    if (!tmp)
      return;
    *buf = tmp;
    *cap = new_cap;
  }
}

static void ny_stage_json_str(char **buf, size_t *len, size_t *cap,
                              const char *s) {
  ny_stage_append(buf, len, cap, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        ny_stage_append(buf, len, cap, "\\\"");
        break;
      case '\\':
        ny_stage_append(buf, len, cap, "\\\\");
        break;
      case '\n':
        ny_stage_append(buf, len, cap, "\\n");
        break;
      case '\r':
        ny_stage_append(buf, len, cap, "\\r");
        break;
      case '\t':
        ny_stage_append(buf, len, cap, "\\t");
        break;
      default:
        if (*p < 32)
          ny_stage_append(buf, len, cap, "\\u%04x", (unsigned)*p);
        else
          ny_stage_append(buf, len, cap, "%c", *p);
        break;
      }
    }
  }
  ny_stage_append(buf, len, cap, "\"");
}

static void ny_stage_append_raw_json(char **buf, size_t *len, size_t *cap,
                                     const char *json) {
  ny_stage_append(buf, len, cap, "%s", (json && *json) ? json : "null");
}

static bool ny_stage_write_default_artifact(const ny_options *opt,
                                            const char *default_name,
                                            const char *json) {
  char path[4096];
  ny_dump_path(path, sizeof(path), opt,
               default_name ? default_name : "artifact.v1.json");
  ny_ensure_parent_dir_for_path(path);
  return ny_write_file(path, json, strlen(json)) == 0;
}

static bool ny_stage_write_artifact(const ny_options *opt,
                                    const char *default_name, const char *json,
                                    bool stdout_if_stopping) {
  if (!json)
    return false;
  if (opt && opt->emit_artifact_path && *opt->emit_artifact_path) {
    ny_ensure_parent_dir_for_path(opt->emit_artifact_path);
    return ny_write_file(opt->emit_artifact_path, json, strlen(json)) == 0;
  }
  if (stdout_if_stopping && opt && opt->stop_after != NY_STOP_AFTER_NONE) {
    fputs(json, stdout);
    if (json[0] && json[strlen(json) - 1] != '\n')
      fputc('\n', stdout);
    return true;
  }
  return ny_stage_write_default_artifact(opt, default_name, json);
}

static char *ny_stage_errors_json(ny_stop_after_stage_t stage,
                                  const char *source_name, const char *message,
                                  int count) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  ny_stage_append(&buf, &len, &cap, "{\"schema\":\"errors.v1\",\"stage\":");
  ny_stage_json_str(&buf, &len, &cap, ny_stage_name(stage));
  ny_stage_append(&buf, &len, &cap, ",\"source\":");
  ny_stage_json_str(&buf, &len, &cap, source_name ? source_name : "");
  ny_stage_append(&buf, &len, &cap,
                  ",\"error_count\":%d,\"errors\":[{\"stage\":", count);
  ny_stage_json_str(&buf, &len, &cap, ny_stage_name(stage));
  ny_stage_append(&buf, &len, &cap, ",\"code\":");
  ny_stage_json_str(&buf, &len, &cap, "stage-failed");
  ny_stage_append(&buf, &len, &cap, ",\"message\":");
  ny_stage_json_str(&buf, &len, &cap, message ? message : "stage failed");
  ny_stage_append(&buf, &len, &cap, "}]}");
  return buf ? buf : ny_strdup("{\"schema\":\"errors.v1\",\"errors\":[]}");
}

static void ny_stage_maybe_emit_errors(const ny_options *opt,
                                       ny_stop_after_stage_t stage,
                                       const char *source_name,
                                       const char *message, int count) {
  if (!opt || !opt->collect_errors)
    return;
  char *json = ny_stage_errors_json(stage, source_name, message, count);
  if (!json)
    return;
  ny_stage_write_default_artifact(opt, "errors.v1.json", json);
  free(json);
}

static bool ny_stage_stmt_in_dump_scope(const ny_options *opt, const stmt_t *s);

static bool ny_stage_expr_ident_literal_eq(expr_t *e, const char **out_name) {
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op ||
      strcmp(e->as.binary.op, "==") != 0)
    return false;
  expr_t *l = e->as.binary.left;
  expr_t *r = e->as.binary.right;
  if (l && r && l->kind == NY_E_IDENT && r->kind == NY_E_LITERAL) {
    if (out_name)
      *out_name = l->as.ident.name;
    return true;
  }
  if (l && r && r->kind == NY_E_IDENT && l->kind == NY_E_LITERAL) {
    if (out_name)
      *out_name = r->as.ident.name;
    return true;
  }
  return false;
}

static bool ny_stage_collect_eq_or(expr_t *e, const char **name,
                                   size_t *terms) {
  if (!e || !name || !terms)
    return false;
  if (e->kind == NY_E_LOGICAL && e->as.logical.op &&
      strcmp(e->as.logical.op, "||") == 0) {
    return ny_stage_collect_eq_or(e->as.logical.left, name, terms) &&
           ny_stage_collect_eq_or(e->as.logical.right, name, terms);
  }
  const char *leaf_name = NULL;
  if (!ny_stage_expr_ident_literal_eq(e, &leaf_name) || !leaf_name)
    return false;
  if (!*name)
    *name = leaf_name;
  if (strcmp(*name, leaf_name) != 0)
    return false;
  (*terms)++;
  return true;
}

static size_t ny_stage_flow_eqset_candidates_expr(expr_t *e);

static size_t ny_stage_flow_eqset_candidates_args(ny_call_arg_list *args) {
  size_t n = 0;
  for (size_t i = 0; args && i < args->len; ++i)
    n += ny_stage_flow_eqset_candidates_expr(args->data[i].val);
  return n;
}

static size_t ny_stage_flow_eqset_candidates_expr(expr_t *e) {
  if (!e)
    return 0;
  const char *name = NULL;
  size_t terms = 0;
  if (ny_stage_collect_eq_or(e, &name, &terms) && terms >= 3)
    return 1;
  size_t n = 0;
  switch (e->kind) {
  case NY_E_UNARY:
    n += ny_stage_flow_eqset_candidates_expr(e->as.unary.right);
    break;
  case NY_E_BINARY:
    n += ny_stage_flow_eqset_candidates_expr(e->as.binary.left);
    n += ny_stage_flow_eqset_candidates_expr(e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    n += ny_stage_flow_eqset_candidates_expr(e->as.logical.left);
    n += ny_stage_flow_eqset_candidates_expr(e->as.logical.right);
    break;
  case NY_E_TERNARY:
    n += ny_stage_flow_eqset_candidates_expr(e->as.ternary.cond);
    n += ny_stage_flow_eqset_candidates_expr(e->as.ternary.true_expr);
    n += ny_stage_flow_eqset_candidates_expr(e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    n += ny_stage_flow_eqset_candidates_expr(e->as.call.callee);
    n += ny_stage_flow_eqset_candidates_args(&e->as.call.args);
    break;
  case NY_E_MEMCALL:
    n += ny_stage_flow_eqset_candidates_expr(e->as.memcall.target);
    n += ny_stage_flow_eqset_candidates_args(&e->as.memcall.args);
    break;
  case NY_E_INDEX:
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.target);
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.start);
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.stop);
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.step);
    break;
  case NY_E_MEMBER:
    n += ny_stage_flow_eqset_candidates_expr(e->as.member.target);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      n += ny_stage_flow_eqset_candidates_expr(e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      n += ny_stage_flow_eqset_candidates_expr(e->as.dict.pairs.data[i].key);
      n += ny_stage_flow_eqset_candidates_expr(e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_MATCH:
    n += ny_stage_flow_eqset_candidates_expr(e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      n += ny_stage_flow_eqset_candidates_expr(arm->guard);
    }
    break;
  case NY_E_FN:
  case NY_E_LAMBDA:
  case NY_E_COMPTIME:
  case NY_E_LITERAL:
  case NY_E_IDENT:
  case NY_E_ASM:
  case NY_E_FSTRING:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  case NY_E_PTR_TYPE:
  case NY_E_DEREF:
  case NY_E_SIZEOF:
  case NY_E_TRY:
  default:
    break;
  }
  return n;
}

static size_t ny_stage_flow_eqset_candidates_stmt(stmt_t *s) {
  if (!s)
    return 0;
  size_t n = 0;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      n += ny_stage_flow_eqset_candidates_expr(s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    n += ny_stage_flow_eqset_candidates_expr(s->as.expr.expr);
    break;
  case NY_S_IF:
    n += ny_stage_flow_eqset_candidates_expr(s->as.iff.test);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.iff.init);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.iff.conseq);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.iff.alt);
    break;
  case NY_S_WHILE:
    n += ny_stage_flow_eqset_candidates_expr(s->as.whl.test);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.whl.init);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.whl.body);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.whl.update);
    break;
  case NY_S_FOR:
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fr.init);
    n += ny_stage_flow_eqset_candidates_expr(s->as.fr.cond);
    n += ny_stage_flow_eqset_candidates_expr(s->as.fr.iterable);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fr.body);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fr.update);
    break;
  case NY_S_FUNC:
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fn.body);
    break;
  case NY_S_RETURN:
    n += ny_stage_flow_eqset_candidates_expr(s->as.ret.value);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.module.body.data[i]);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.impl.methods.data[i]);
    break;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.layout.methods.data[i]);
    break;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.struc.methods.data[i]);
    break;
  case NY_S_MATCH:
    n += ny_stage_flow_eqset_candidates_expr(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.match.arms.data[i].conseq);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.match.default_conseq);
    break;
  default:
    break;
  }
  return n;
}

static size_t ny_stage_flow_eqset_candidates_program(const ny_options *opt,
                                                     program_t *prog) {
  size_t n = 0;
  if (!prog)
    return 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    if (!ny_stage_stmt_in_dump_scope(opt, prog->body.data[i]))
      continue;
    n += ny_stage_flow_eqset_candidates_stmt(prog->body.data[i]);
  }
  return n;
}

static bool ny_stage_stmt_in_dump_scope(const ny_options *opt,
                                        const stmt_t *s) {
  ny_dump_scope_t dump_scope = opt ? opt->dump_scope : NY_DUMP_SCOPE_PROGRAM;
  if (!s)
    return dump_scope == NY_DUMP_SCOPE_BOTH;
  bool is_std = ny_is_stdlib_tok(s->tok);
  if (dump_scope == NY_DUMP_SCOPE_BOTH)
    return true;
  if (dump_scope == NY_DUMP_SCOPE_LIB)
    return is_std;
  return !is_std;
}

static void ny_stage_append_shapes_json(char **buf, size_t *len, size_t *cap,
                                        const ny_options *opt, codegen_t *cg,
                                        LLVMModuleRef module) {
  ny_stage_append(buf, len, cap, "{\"functions\":[");
  size_t emitted = 0;
  for (size_t i = 0; cg && i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, sig->stmt_t))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"name\":");
    ny_stage_json_str(buf, len, cap, sig->name ? sig->name : "");
    ny_stage_append(buf, len, cap, ",\"arity\":%d,\"return\":", sig->arity);
    ny_stage_json_str(
        buf, len, cap,
        sig->return_type
            ? sig->return_type
            : (sig->inferred_return_type ? sig->inferred_return_type : ""));
    ny_stage_append(
        buf, len, cap,
        ",\"extern\":%s,\"native_abi\":%s,\"effects\":%u,\"effects_known\":%s}",
        sig->is_extern ? "true" : "false",
        sig->is_native_abi ? "true" : "false", sig->effects,
        sig->effects_known ? "true" : "false");
  }
  ny_stage_append(buf, len, cap, "],\"globals\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->global_vars.len; ++i) {
    binding *b = &cg->global_vars.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, b->stmt_t))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"name\":");
    ny_stage_json_str(buf, len, cap, b->name ? b->name : "");
    ny_stage_append(buf, len, cap, ",\"type\":");
    ny_stage_json_str(buf, len, cap, b->type_name ? b->type_name : "");
    ny_stage_append(
        buf, len, cap, ",\"int\":%s,\"f64\":%s,\"slot\":%s,\"escapes\":%s}",
        (b->is_int_slot || b->is_int_direct) ? "true" : "false",
        (b->is_f64_slot || b->is_f64_direct) ? "true" : "false",
        b->is_slot ? "true" : "false", b->escapes ? "true" : "false");
  }
  ny_stage_append(buf, len, cap, "],\"layouts\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->layouts.len; ++i) {
    layout_def_t *layout = cg->layouts.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, layout ? layout->stmt : NULL))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"name\":");
    ny_stage_json_str(buf, len, cap,
                      layout && layout->name ? layout->name : "");
    ny_stage_append(
        buf, len, cap,
        ",\"size\":%zu,\"align\":%zu,\"pack\":%zu,\"layout\":%s,\"fields\":[",
        layout ? layout->size : 0, layout ? layout->align : 0,
        layout ? layout->pack : 0,
        layout && layout->is_layout ? "true" : "false");
    for (size_t j = 0; layout && j < layout->fields.len; ++j) {
      layout_field_info_t *field = &layout->fields.data[j];
      if (j)
        ny_stage_append(buf, len, cap, ",");
      ny_stage_append(buf, len, cap, "{\"name\":");
      ny_stage_json_str(buf, len, cap, field->name ? field->name : "");
      ny_stage_append(buf, len, cap, ",\"type\":");
      ny_stage_json_str(buf, len, cap,
                        field->type_name ? field->type_name : "");
      ny_stage_append(buf, len, cap,
                      ",\"offset\":%zu,\"size\":%zu,\"align\":%zu}",
                      field->offset, field->size, field->align);
    }
    ny_stage_append(buf, len, cap, "]}");
  }
  ny_stage_append(buf, len, cap, "],\"operators\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->operators.len; ++i) {
    ny_operator_def_t *op = &cg->operators.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, op->stmt))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"op\":");
    ny_stage_json_str(buf, len, cap, op->op ? op->op : "");
    ny_stage_append(buf, len, cap, ",\"left\":");
    ny_stage_json_str(buf, len, cap, op->left_type ? op->left_type : "");
    ny_stage_append(buf, len, cap, ",\"right\":");
    ny_stage_json_str(buf, len, cap, op->right_type ? op->right_type : "");
    ny_stage_append(buf, len, cap, ",\"return\":");
    ny_stage_json_str(buf, len, cap, op->return_type ? op->return_type : "");
    ny_stage_append(buf, len, cap, ",\"target\":");
    ny_stage_json_str(buf, len, cap, op->target_name ? op->target_name : "");
    ny_stage_append(buf, len, cap, "}");
  }
  ny_stage_append(buf, len, cap, "],\"tagged_types\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->tagged_types.len; ++i) {
    const char *tag = cg->tagged_types.data[i];
    bool is_std_tag = tag && strncmp(tag, "std.", 4) == 0;
    ny_dump_scope_t dump_scope = opt ? opt->dump_scope : NY_DUMP_SCOPE_PROGRAM;
    if ((dump_scope == NY_DUMP_SCOPE_PROGRAM && is_std_tag) ||
        (dump_scope == NY_DUMP_SCOPE_LIB && !is_std_tag))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_json_str(buf, len, cap, tag);
  }
  ny_stage_append(buf, len, cap, "],\"monomorphizations\":[");
  for (size_t i = 0; cg && i < cg->mono_specs.len; ++i) {
    ny_mono_specialization_t *spec = &cg->mono_specs.data[i];
    if (i)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"base\":");
    ny_stage_json_str(buf, len, cap, spec->base_name ? spec->base_name : "");
    ny_stage_append(buf, len, cap, ",\"specialized\":");
    ny_stage_json_str(buf, len, cap,
                      spec->specialized_name ? spec->specialized_name : "");
    ny_stage_append(buf, len, cap, ",\"arity\":%d}", spec->arity);
  }
  ny_stage_append(buf, len, cap, "]");
  if (module) {
    ny_ir_stats_t st = {0};
    ny_collect_ir_stats(module, &st);
    ny_stage_append(buf, len, cap,
                    ",\"ir_stats\":{\"funcs\":%" PRIu64 ",\"blocks\":%" PRIu64
                    ",\"insts\":%" PRIu64 ",\"allocas\":%" PRIu64
                    ",\"phis\":%" PRIu64 "}",
                    st.funcs, st.blocks, st.insts, st.allocas, st.phis);
  }
  ny_stage_append(buf, len, cap, "}");
}

static char *ny_stage_artifact_json(const ny_options *opt,
                                    ny_stop_after_stage_t stage,
                                    program_t *prog, codegen_t *cg,
                                    const char *source_name,
                                    LLVMModuleRef module) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  char *ast_json = NULL;
  char *type_json = NULL;
  char *resolved_json = NULL;
  char *refined_json = NULL;
  char *lowered_json = NULL;
  ny_stage_append(&buf, &len, &cap, "{\"schema\":");
  ny_stage_json_str(&buf, &len, &cap, ny_stage_schema(stage));
  ny_stage_append(&buf, &len, &cap, ",\"stage\":");
  ny_stage_json_str(&buf, &len, &cap, ny_stage_name(stage));
  ny_stage_append(&buf, &len, &cap, ",\"source\":");
  ny_stage_json_str(&buf, &len, &cap, source_name ? source_name : "");
  ny_stage_append(
      &buf, &len, &cap,
      ",\"pipeline\":[\"parse\",\"hm\",\"trait\",\"flow\",\"abi\",\"opt\"]");
  ny_stage_append(
      &buf, &len, &cap,
      ",\"type_groups\":{\"number\":[\"int\",\"i8\",\"i16\",\"i32\",\"i64\","
      "\"i128\",\"u8\",\"u16\",\"u32\",\"u64\",\"u128\",\"f32\",\"f64\","
      "\"f128\",\"bigint\"],\"numeric\":[\"int\",\"i8\",\"i16\",\"i32\","
      "\"i64\",\"i128\",\"u8\",\"u16\",\"u32\",\"u64\",\"u128\",\"f32\","
      "\"f64\",\"f128\",\"bigint\"],\"integer\":[\"int\",\"i8\",\"i16\","
      "\"i32\","
      "\"i64\",\"i128\",\"u8\",\"u16\",\"u32\",\"u64\",\"u128\",\"bigint\"],"
      "\"float\":[\"f32\",\"f64\",\"f128\"],\"scalar\":[\"number\",\"bool\","
      "\"str\",\"char\",\"complex\",\"c64\",\"c128\"],\"seq\":[\"list\","
      "\"tuple\",\"str\",\"bytes\",\"range\"],\"sequence\":[\"seq\"],"
      "\"iterable\":[\"seq\",\"set\",\"dict\",\"bytes\"],\"indexable\":"
      "[\"seq\",\"dict\",\"bytes\"],\"allocator\":[\"ptr\",\"handle\"]}");
  if (prog) {
    ast_json = ny_ast_to_json_filtered(prog, source_name);
    ny_stage_append(&buf, &len, &cap, ",\"ast\":");
    ny_stage_append_raw_json(&buf, &len, &cap, ast_json ? ast_json : "[]");
    char *symbols_json = ny_ast_symbols_to_json_filtered(prog, source_name);
    ny_stage_append(&buf, &len, &cap, ",\"symbols\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             symbols_json ? symbols_json : "[]");
    if (symbols_json)
      rt_free((int64_t)(uintptr_t)symbols_json);
  }
  if (stage >= NY_STOP_AFTER_HM && prog) {
    type_json = ny_type_pipeline_typed_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(&buf, &len, &cap, ",\"typed\":");
    ny_stage_append_raw_json(&buf, &len, &cap, type_json ? type_json : "{}");
  }
  if (stage >= NY_STOP_AFTER_TRAIT && prog) {
    resolved_json = ny_type_pipeline_resolved_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(&buf, &len, &cap, ",\"resolved\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             resolved_json ? resolved_json : "{}");
  }
  if (stage >= NY_STOP_AFTER_FLOW && prog) {
    refined_json = ny_type_pipeline_refined_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(
        &buf, &len, &cap,
        ",\"flow\":{\"eq_or_candidates\":%zu,"
        "\"range_facts\":\"shared refined.v1 range_refinements/index_proofs\"}",
        ny_stage_flow_eqset_candidates_program(opt, prog));
    ny_stage_append(&buf, &len, &cap, ",\"refined\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             refined_json ? refined_json : "{}");
  }
  if (stage >= NY_STOP_AFTER_ABI && prog) {
    lowered_json = ny_type_pipeline_lowered_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(&buf, &len, &cap, ",\"lowered\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             lowered_json ? lowered_json : "{}");
  }
  if ((opt && opt->emit_shapes) || stage >= NY_STOP_AFTER_TRAIT || cg) {
    ny_stage_append(&buf, &len, &cap, ",\"shapes\":");
    ny_stage_append_shapes_json(&buf, &len, &cap, opt, cg, module);
  }
  ny_stage_append(&buf, &len, &cap, ",\"errors\":[]}");
  if (ast_json)
    rt_free((int64_t)(uintptr_t)ast_json);
  free(type_json);
  free(resolved_json);
  free(refined_json);
  free(lowered_json);
  return buf ? buf : ny_strdup("{\"schema\":\"artifact.v1\"}");
}

static bool ny_stage_emit_artifact(const ny_options *opt,
                                   ny_stop_after_stage_t stage, program_t *prog,
                                   codegen_t *cg, const char *source_name,
                                   LLVMModuleRef module,
                                   bool stdout_if_stopping) {
  char *json =
      ny_stage_artifact_json(opt, stage, prog, cg, source_name, module);
  if (!json)
    return false;
  bool ok = ny_stage_write_artifact(opt, ny_stage_default_file(stage), json,
                                    stdout_if_stopping);
  free(json);
  return ok;
}

typedef struct ny_safe_raw_ptr_fact_t {
  const char *name;
  int64_t size;
} ny_safe_raw_ptr_fact_t;

typedef struct ny_safe_raw_int_fact_t {
  const char *name;
  int64_t min;
  int64_t max;
} ny_safe_raw_int_fact_t;

typedef VEC(ny_safe_raw_ptr_fact_t) ny_safe_raw_ptr_fact_list;
typedef VEC(ny_safe_raw_int_fact_t) ny_safe_raw_int_fact_list;

typedef struct ny_safe_raw_ctx_t {
  ny_safe_raw_ptr_fact_list ptrs;
  ny_safe_raw_int_fact_list ints;
  bool ok;
} ny_safe_raw_ctx_t;

typedef struct ny_safe_raw_scope_t {
  size_t ptr_len;
  size_t int_len;
} ny_safe_raw_scope_t;

static void ny_safe_raw_validate_expr(ny_safe_raw_ctx_t *ctx, expr_t *e);
static void ny_safe_raw_validate_stmt(ny_safe_raw_ctx_t *ctx, stmt_t *s);

static ny_safe_raw_scope_t ny_safe_raw_scope_mark(ny_safe_raw_ctx_t *ctx) {
  ny_safe_raw_scope_t mark = {0};
  if (ctx) {
    mark.ptr_len = ctx->ptrs.len;
    mark.int_len = ctx->ints.len;
  }
  return mark;
}

static void ny_safe_raw_scope_restore(ny_safe_raw_ctx_t *ctx,
                                      ny_safe_raw_scope_t mark) {
  if (!ctx)
    return;
  if (mark.ptr_len <= ctx->ptrs.len)
    ctx->ptrs.len = mark.ptr_len;
  if (mark.int_len <= ctx->ints.len)
    ctx->ints.len = mark.int_len;
}

static const char *ny_safe_raw_leaf(const char *name) {
  const char *tail = name ? strrchr(name, '.') : NULL;
  return tail ? tail + 1 : name;
}

static const char *ny_safe_raw_callee_leaf(expr_t *callee) {
  if (!callee)
    return NULL;
  if (callee->kind == NY_E_IDENT)
    return ny_safe_raw_leaf(callee->as.ident.name);
  if (callee->kind == NY_E_MEMBER)
    return ny_safe_raw_leaf(callee->as.member.name);
  return NULL;
}

static bool ny_safe_raw_name_is(const char *name, const char *want) {
  const char *leaf = ny_safe_raw_leaf(name);
  return leaf && want && strcmp(leaf, want) == 0;
}

static bool ny_safe_raw_call_name_is(expr_t *e, const char *want) {
  return e && e->kind == NY_E_CALL &&
         ny_safe_raw_name_is(ny_safe_raw_callee_leaf(e->as.call.callee), want);
}

static bool ny_safe_raw_is_malloc_like(const char *leaf, bool *size_arg_is_one) {
  if (size_arg_is_one)
    *size_arg_is_one = false;
  if (!leaf)
    return false;
  if (strcmp(leaf, "malloc") == 0 || strcmp(leaf, "malloc_raw") == 0 ||
      strcmp(leaf, "zalloc") == 0 || strcmp(leaf, "__malloc") == 0) {
    return true;
  }
  if (strcmp(leaf, "realloc") == 0) {
    if (size_arg_is_one)
      *size_arg_is_one = true;
    return true;
  }
  return false;
}

static bool ny_safe_raw_alloc_size(expr_t *e, int64_t *out) {
  if (!e || e->kind != NY_E_CALL)
    return false;
  const char *leaf = ny_safe_raw_callee_leaf(e->as.call.callee);
  bool size_arg_is_one = false;
  if (!ny_safe_raw_is_malloc_like(leaf, &size_arg_is_one))
    return false;
  size_t idx = size_arg_is_one ? 1u : 0u;
  if (e->as.call.args.len <= idx)
    return false;
  int64_t n = 0;
  if (!ny_expr_literal_i64(e->as.call.args.data[idx].val, &n) || n < 0)
    return false;
  if (out)
    *out = n;
  return true;
}

static void ny_safe_raw_push_ptr(ny_safe_raw_ctx_t *ctx, const char *name,
                                 int64_t size) {
  if (!ctx || !name || size < 0)
    return;
  ny_safe_raw_ptr_fact_t fact = {.name = name, .size = size};
  vec_push(&ctx->ptrs, fact);
}

static void ny_safe_raw_push_int(ny_safe_raw_ctx_t *ctx, const char *name,
                                 int64_t min, int64_t max) {
  if (!ctx || !name || min > max)
    return;
  ny_safe_raw_int_fact_t fact = {.name = name, .min = min, .max = max};
  vec_push(&ctx->ints, fact);
}

static bool ny_safe_raw_lookup_ptr(ny_safe_raw_ctx_t *ctx, const char *name,
                                   int64_t *out_size) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->ptrs.len; i > 0; --i) {
    ny_safe_raw_ptr_fact_t *fact = &ctx->ptrs.data[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0) {
      if (out_size)
        *out_size = fact->size;
      return true;
    }
  }
  return false;
}

static bool ny_safe_raw_lookup_int(ny_safe_raw_ctx_t *ctx, const char *name,
                                   int64_t *out_min, int64_t *out_max) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->ints.len; i > 0; --i) {
    ny_safe_raw_int_fact_t *fact = &ctx->ints.data[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0) {
      if (out_min)
        *out_min = fact->min;
      if (out_max)
        *out_max = fact->max;
      return true;
    }
  }
  return false;
}

static bool ny_safe_raw_expr_int_range(ny_safe_raw_ctx_t *ctx, expr_t *e,
                                       int64_t *out_min, int64_t *out_max) {
  int64_t lit = 0;
  if (ny_expr_literal_i64(e, &lit)) {
    if (out_min)
      *out_min = lit;
    if (out_max)
      *out_max = lit;
    return true;
  }
  if (!e)
    return false;
  if (e->kind == NY_E_IDENT)
    return ny_safe_raw_lookup_int(ctx, e->as.ident.name, out_min, out_max);
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "-") == 0) {
    int64_t lo = 0, hi = 0;
    if (!ny_safe_raw_expr_int_range(ctx, e->as.unary.right, &lo, &hi))
      return false;
    if (hi == INT64_MIN || lo == INT64_MIN)
      return false;
    if (out_min)
      *out_min = -hi;
    if (out_max)
      *out_max = -lo;
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
    if (!ny_safe_raw_expr_int_range(ctx, e->as.binary.left, &lmin, &lmax) ||
        !ny_safe_raw_expr_int_range(ctx, e->as.binary.right, &rmin, &rmax))
      return false;
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") == 0) {
      if ((rmin > 0 && lmin > INT64_MAX - rmin) ||
          (rmax < 0 && lmax < INT64_MIN - rmax))
        return false;
      if (out_min)
        *out_min = lmin + rmin;
      if (out_max)
        *out_max = lmax + rmax;
      return true;
    }
    if (strcmp(op, "-") == 0) {
      if ((rmax < 0 && lmin > INT64_MAX + rmax) ||
          (rmin > 0 && lmax < INT64_MIN + rmin))
        return false;
      if (out_min)
        *out_min = lmin - rmax;
      if (out_max)
        *out_max = lmax - rmin;
      return true;
    }
    if (strcmp(op, "*") == 0 && lmin == lmax && rmin == rmax) {
      if (lmin != 0 &&
          (rmin > INT64_MAX / lmin || rmin < INT64_MIN / lmin))
        return false;
      int64_t v = lmin * rmin;
      if (out_min)
        *out_min = v;
      if (out_max)
        *out_max = v;
      return true;
    }
  }
  return false;
}

static int ny_safe_raw_width_for_load(const char *leaf) {
  if (!leaf)
    return 0;
  if (strcmp(leaf, "load8") == 0 || strcmp(leaf, "__load8_idx") == 0)
    return 1;
  if (strcmp(leaf, "load16") == 0 || strcmp(leaf, "__load16_idx") == 0)
    return 2;
  if (strcmp(leaf, "load32") == 0 || strcmp(leaf, "load32_h") == 0 ||
      strcmp(leaf, "load32_f32") == 0 || strcmp(leaf, "__load32_idx") == 0 ||
      strcmp(leaf, "__load32_h") == 0)
    return 4;
  if (strcmp(leaf, "load64") == 0 || strcmp(leaf, "load64_h") == 0 ||
      strcmp(leaf, "load64_i") == 0 || strcmp(leaf, "load64_f64") == 0 ||
      strcmp(leaf, "__load64_idx") == 0 || strcmp(leaf, "__load64_h") == 0)
    return 8;
  return 0;
}

static int ny_safe_raw_width_for_store(const char *leaf, bool *intrinsic) {
  if (intrinsic)
    *intrinsic = false;
  if (!leaf)
    return 0;
  if (strncmp(leaf, "__store", 7) == 0 && intrinsic)
    *intrinsic = true;
  if (strcmp(leaf, "store8") == 0 || strcmp(leaf, "__store8_idx") == 0)
    return 1;
  if (strcmp(leaf, "store16") == 0 || strcmp(leaf, "__store16_idx") == 0)
    return 2;
  if (strcmp(leaf, "store32") == 0 || strcmp(leaf, "store32_h") == 0 ||
      strcmp(leaf, "store32_f32") == 0 ||
      strcmp(leaf, "__store32_idx") == 0)
    return 4;
  if (strcmp(leaf, "store64") == 0 || strcmp(leaf, "store64_h") == 0 ||
      strcmp(leaf, "store64_i") == 0 || strcmp(leaf, "store64_f64") == 0 ||
      strcmp(leaf, "__store64_idx") == 0)
    return 8;
  if (intrinsic)
    *intrinsic = false;
  return 0;
}

static bool ny_safe_raw_api_shape(expr_t *e, expr_t **out_ptr,
                                  expr_t **out_idx, int64_t *out_width) {
  if (out_ptr)
    *out_ptr = NULL;
  if (out_idx)
    *out_idx = NULL;
  if (out_width)
    *out_width = 0;
  if (!e || e->kind != NY_E_CALL)
    return false;
  const char *leaf = ny_safe_raw_callee_leaf(e->as.call.callee);
  int width = ny_safe_raw_width_for_load(leaf);
  if (width > 0) {
    if (e->as.call.args.len < 1)
      return false;
    if (out_ptr)
      *out_ptr = e->as.call.args.data[0].val;
    if (out_idx && e->as.call.args.len >= 2)
      *out_idx = e->as.call.args.data[1].val;
    if (out_width)
      *out_width = width;
    return true;
  }
  bool intrinsic_store = false;
  width = ny_safe_raw_width_for_store(leaf, &intrinsic_store);
  if (width > 0) {
    if (e->as.call.args.len < 2)
      return false;
    if (out_ptr)
      *out_ptr = e->as.call.args.data[0].val;
    if (out_idx) {
      size_t idx_pos = intrinsic_store ? 1u : 2u;
      if (e->as.call.args.len > idx_pos)
        *out_idx = e->as.call.args.data[idx_pos].val;
    }
    if (out_width)
      *out_width = width;
    return true;
  }
  return false;
}

static void ny_safe_raw_validate_call(ny_safe_raw_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e || e->kind != NY_E_CALL || ny_is_stdlib_tok(e->tok))
    return;
  expr_t *ptr = NULL;
  expr_t *idx = NULL;
  int64_t width = 0;
  if (!ny_safe_raw_api_shape(e, &ptr, &idx, &width) || !ptr ||
      ptr->kind != NY_E_IDENT || width <= 0)
    return;
  int64_t alloc_size = 0;
  if (!ny_safe_raw_lookup_ptr(ctx, ptr->as.ident.name, &alloc_size))
    return;
  int64_t idx_min = 0;
  int64_t idx_max = 0;
  bool has_index = idx ? ny_safe_raw_expr_int_range(ctx, idx, &idx_min, &idx_max)
                       : true;
  if (!has_index) {
    ny_diag_error(idx ? idx->tok : e->tok,
                  "safe-mode raw memory access requires a proven byte range "
                  "for index");
    ctx->ok = false;
    return;
  }
  if (idx_min < 0 || width > alloc_size || idx_max > alloc_size - width) {
    ny_diag_error(idx ? idx->tok : e->tok,
                  "safe-mode raw memory access out of bounds");
    ctx->ok = false;
  }
}

static void ny_safe_raw_record_compile_range(ny_safe_raw_ctx_t *ctx,
                                             expr_t *e) {
  if (!ctx || !e || e->kind != NY_E_CALL ||
      !ny_safe_raw_call_name_is(e, "assert_compile_range") ||
      e->as.call.args.len < 3)
    return;
  expr_t *value = e->as.call.args.data[0].val;
  expr_t *lo_expr = e->as.call.args.data[1].val;
  expr_t *hi_expr = e->as.call.args.data[2].val;
  if (!value || value->kind != NY_E_IDENT)
    return;
  int64_t lo = 0;
  int64_t hi = 0;
  if (!ny_expr_literal_i64(lo_expr, &lo) || !ny_expr_literal_i64(hi_expr, &hi) ||
      lo > hi)
    return;
  ny_safe_raw_push_int(ctx, value->as.ident.name, lo, hi);
}

static void ny_safe_raw_validate_expr(ny_safe_raw_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return;
  ny_safe_raw_validate_call(ctx, e);
  switch (e->kind) {
  case NY_E_UNARY:
    ny_safe_raw_validate_expr(ctx, e->as.unary.right);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    ny_safe_raw_validate_expr(ctx, e->as.binary.left);
    ny_safe_raw_validate_expr(ctx, e->as.binary.right);
    break;
  case NY_E_TERNARY:
    ny_safe_raw_validate_expr(ctx, e->as.ternary.cond);
    ny_safe_raw_validate_expr(ctx, e->as.ternary.true_expr);
    ny_safe_raw_validate_expr(ctx, e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    ny_safe_raw_validate_expr(ctx, e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.call.args.data[i].val);
    ny_safe_raw_record_compile_range(ctx, e);
    break;
  case NY_E_MEMCALL:
    ny_safe_raw_validate_expr(ctx, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.memcall.args.data[i].val);
    break;
  case NY_E_INDEX:
    ny_safe_raw_validate_expr(ctx, e->as.index.target);
    ny_safe_raw_validate_expr(ctx, e->as.index.start);
    ny_safe_raw_validate_expr(ctx, e->as.index.stop);
    ny_safe_raw_validate_expr(ctx, e->as.index.step);
    break;
  case NY_E_MEMBER:
    ny_safe_raw_validate_expr(ctx, e->as.member.target);
    break;
  case NY_E_PTR_TYPE:
    ny_safe_raw_validate_expr(ctx, e->as.ptr_type.target);
    break;
  case NY_E_DEREF:
    ny_safe_raw_validate_expr(ctx, e->as.deref.target);
    break;
  case NY_E_SIZEOF:
    if (!e->as.szof.is_type)
      ny_safe_raw_validate_expr(ctx, e->as.szof.target);
    break;
  case NY_E_TRY:
    ny_safe_raw_validate_expr(ctx, e->as.try_expr.target);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      ny_safe_raw_validate_expr(ctx, e->as.dict.pairs.data[i].key);
      ny_safe_raw_validate_expr(ctx, e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_COMPTIME:
    ny_safe_raw_validate_stmt(ctx, e->as.comptime_expr.body);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        ny_safe_raw_validate_expr(ctx, e->as.fstring.parts.data[i].as.e);
    }
    break;
  case NY_E_MATCH:
    ny_safe_raw_validate_expr(ctx, e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        ny_safe_raw_validate_expr(ctx, arm->patterns.data[j]);
      ny_safe_raw_validate_expr(ctx, arm->guard);
      ny_safe_raw_validate_stmt(ctx, arm->conseq);
    }
    ny_safe_raw_validate_stmt(ctx, e->as.match.default_conseq);
    break;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.as_asm.args.data[i]);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ctx->ptrs.len = 0;
    ctx->ints.len = 0;
    ny_safe_raw_validate_stmt(ctx, e->as.lambda.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_E_IDENT:
  case NY_E_LITERAL:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
    break;
  }
}

static void ny_safe_raw_record_var_facts(ny_safe_raw_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s || s->kind != NY_S_VAR)
    return;
  stmt_var_t *var = &s->as.var;
  if (!var->is_decl || var->is_mut)
    return;
  size_t n = var->names.len < var->exprs.len ? var->names.len : var->exprs.len;
  for (size_t i = 0; i < n; ++i) {
    const char *name = var->names.data[i];
    expr_t *init = var->exprs.data[i];
    int64_t alloc_size = 0;
    int64_t exact_int = 0;
    if (ny_safe_raw_alloc_size(init, &alloc_size))
      ny_safe_raw_push_ptr(ctx, name, alloc_size);
    if (ny_expr_literal_i64(init, &exact_int))
      ny_safe_raw_push_int(ctx, name, exact_int, exact_int);
  }
}

static void ny_safe_raw_validate_stmt_list(ny_safe_raw_ctx_t *ctx,
                                           ny_stmt_list *list) {
  if (!ctx || !list)
    return;
  for (size_t i = 0; i < list->len; ++i)
    ny_safe_raw_validate_stmt(ctx, list->data[i]);
}

static void ny_safe_raw_validate_stmt(ny_safe_raw_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s || ny_is_stdlib_tok(s->tok))
    return;
  switch (s->kind) {
  case NY_S_BLOCK: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt_list(ctx, &s->as.block.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      ny_safe_raw_validate_expr(ctx, s->as.var.exprs.data[i]);
    ny_safe_raw_record_var_facts(ctx, s);
    break;
  case NY_S_EXPR:
    ny_safe_raw_validate_expr(ctx, s->as.expr.expr);
    break;
  case NY_S_RETURN:
    ny_safe_raw_validate_expr(ctx, s->as.ret.value);
    break;
  case NY_S_IF: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt(ctx, s->as.iff.init);
    ny_safe_raw_validate_expr(ctx, s->as.iff.test);
    ny_safe_raw_validate_stmt(ctx, s->as.iff.conseq);
    ny_safe_raw_validate_stmt(ctx, s->as.iff.alt);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_GUARD:
    ny_safe_raw_validate_expr(ctx, s->as.guard.value);
    ny_safe_raw_validate_stmt(ctx, s->as.guard.fallback);
    break;
  case NY_S_WHILE: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt(ctx, s->as.whl.init);
    ny_safe_raw_validate_expr(ctx, s->as.whl.test);
    ny_safe_raw_validate_stmt(ctx, s->as.whl.body);
    ny_safe_raw_validate_stmt(ctx, s->as.whl.update);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_FOR: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt(ctx, s->as.fr.init);
    ny_safe_raw_validate_expr(ctx, s->as.fr.cond);
    ny_safe_raw_validate_expr(ctx, s->as.fr.iterable);
    ny_safe_raw_validate_stmt(ctx, s->as.fr.body);
    ny_safe_raw_validate_stmt(ctx, s->as.fr.update);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_MATCH:
    ny_safe_raw_validate_expr(ctx, s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
      for (size_t j = 0; j < arm->patterns.len; ++j)
        ny_safe_raw_validate_expr(ctx, arm->patterns.data[j]);
      ny_safe_raw_validate_expr(ctx, arm->guard);
      ny_safe_raw_validate_stmt(ctx, arm->conseq);
      ny_safe_raw_scope_restore(ctx, mark);
    }
    ny_safe_raw_validate_stmt(ctx, s->as.match.default_conseq);
    break;
  case NY_S_TRY:
    ny_safe_raw_validate_stmt(ctx, s->as.tr.body);
    ny_safe_raw_validate_stmt(ctx, s->as.tr.handler);
    break;
  case NY_S_DEFER:
    ny_safe_raw_validate_stmt(ctx, s->as.de.body);
    break;
  case NY_S_FUNC: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ctx->ptrs.len = 0;
    ctx->ints.len = 0;
    ny_safe_raw_validate_stmt(ctx, s->as.fn.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_LAYOUT:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.layout.methods);
    break;
  case NY_S_STRUCT:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.struc.methods);
    break;
  case NY_S_IMPL:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.impl.methods);
    break;
  case NY_S_MODULE:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.module.body);
    break;
  case NY_S_MACRO: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    for (size_t i = 0; i < s->as.macro.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, s->as.macro.args.data[i]);
    ny_safe_raw_validate_stmt(ctx, s->as.macro.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_USE:
  case NY_S_EXTERN:
  case NY_S_LINK:
  case NY_S_LABEL:
  case NY_S_GOTO:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
  case NY_S_EXPORT:
  case NY_S_ENUM:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_OPERATOR:
    break;
  }
}

static bool ny_safe_mode_validate_raw_memory(program_t *prog) {
  if (!prog)
    return true;
  ny_safe_raw_ctx_t ctx = {0};
  ctx.ok = true;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_safe_raw_validate_stmt(&ctx, prog->body.data[i]);
  bool ok = ctx.ok;
  vec_free(&ctx.ptrs);
  vec_free(&ctx.ints);
  return ok;
}
