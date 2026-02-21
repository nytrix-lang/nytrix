#include "base/util.h"
#include "braun.h"
#include "llvm.h"
#include "nullnarrow.h"
#include "priv.h"
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool can_bind_decl_direct(const codegen_t *cg, const char *name,
                                 bool is_mut);

static void scope_enter(scope *scopes, size_t *depth,
                        LLVMBasicBlockRef break_bb,
                        LLVMBasicBlockRef continue_bb) {
  (*depth)++;
  memset(&scopes[*depth], 0, sizeof(scopes[*depth]));
  scopes[*depth].break_bb = break_bb;
  scopes[*depth].continue_bb = continue_bb;
}

static inline void braun_write_local_value(codegen_t *cg, size_t depth,
                                           bool is_global_target,
                                           const char *name,
                                           LLVMValueRef value) {
  if (!cg || !cg->braun || !name || !value)
    return;
  if (depth == 0 || is_global_target)
    return;
  ny_braun_mark_current_block(cg);
  braun_ssa_write_var(cg->braun, name, value);
}

static void report_reassign_immutable(codegen_t *cg, token_t tok, const char *n,
                                      bool is_global, const binding *decl) {
  ny_diag_error(tok,
                "cannot reassign immutable %svariable \033[1;37m'%s'\033[0m",
                is_global ? "global " : "", n);
  if (decl && decl->stmt_t)
    ny_diag_note_tok(decl->stmt_t->tok, "'%s' declared here", n);
  if (verbose_enabled >= 1)
    ny_diag_hint("declare it with 'mut %s = ...' if reassignment is intended",
                 n);
  ny_diag_fix(
      "change the declaration to 'mut %s = ...' or assign to a new name", n);
  cg->had_error = 1;
}

static bool ensure_mutable_binding_for_assign(codegen_t *cg, token_t tok,
                                              const char *name,
                                              const binding *b,
                                              bool is_global) {
  if (!b)
    return false;
  if (b->is_mut)
    return true;
  report_reassign_immutable(cg, tok, name, is_global, b);
  return false;
}

static void stmt_var_setup_local_binding(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *var_stmt,
    sema_var_t *sema, size_t idx, const char *name, const char *decl_type,
    bool prefer_direct_locals, bool *bind_direct, LLVMValueRef *slot) {
  if (!cg || !scopes || !var_stmt || !name || !bind_direct || !slot)
    return;
  if (prefer_direct_locals ||
      can_bind_decl_direct(cg, name, var_stmt->as.var.is_mut)) {
    *bind_direct = true;
    return;
  }
  LLVMTypeRef var_type = cg->type_i64;
  if (sema && sema->resolved_types.len > idx) {
    var_type = sema->resolved_types.data[idx];
  } else if (decl_type) {
    var_type = resolve_type_name(cg, decl_type, var_stmt->tok);
  }
  *slot = build_alloca(cg, name, var_type);
  scope_bind(cg, scopes, depth, name, *slot, var_stmt, var_stmt->as.var.is_mut,
             decl_type, true);
}

static binding *stmt_var_lookup_existing(codegen_t *cg, scope *scopes,
                                         size_t depth, const char *name,
                                         bool *is_global) {
  if (is_global)
    *is_global = false;
  if (!cg || !scopes || !name)
    return NULL;
  binding *b = NULL;
  if (depth > 0)
    b = scope_lookup(scopes, depth, name);
  if (b)
    return b;
  if (is_global)
    *is_global = true;
  return lookup_global(cg, name);
}

static bool ensure_store_ready(codegen_t *cg, token_t tok, LLVMValueRef value,
                               LLVMValueRef slot, const char *ctx) {
  if (!cg->builder) {
    ny_diag_error(tok, "internal codegen failure: missing builder in %s", ctx);
    cg->had_error = 1;
    return false;
  }
  if (!value) {
    ny_diag_error(
        tok, "internal codegen failure: missing assignment value in %s", ctx);
    cg->had_error = 1;
    return false;
  }
  if (!slot) {
    ny_diag_error(
        tok, "internal codegen failure: missing assignment destination in %s",
        ctx);
    cg->had_error = 1;
    return false;
  }
  if (LLVMGetTypeKind(LLVMTypeOf(slot)) != LLVMPointerTypeKind) {
    ny_diag_error(tok,
                  "internal codegen failure: assignment destination is not "
                  "addressable in "
                  "%s",
                  ctx);
    cg->had_error = 1;
    return false;
  }
  LLVMBasicBlockRef cur_block = LLVMGetInsertBlock(cg->builder);
  if (!cur_block) {
    ny_diag_error(tok, "internal codegen failure: missing insert block");
    cg->had_error = 1;
    return false;
  }
  return !LLVMGetBasicBlockTerminator(cur_block);
}

static bool can_bind_decl_direct(const codegen_t *cg, const char *name,
                                 bool is_mut) {
  if (!is_mut)
    return true;
  if (!cg || !name || !cg->assigned_names_data || cg->assigned_names_len == 0)
    return false;
  uint64_t h = ny_hash64_cstr(name);
  return !ny_name_set_has_hash(
      (const char *const *)cg->assigned_names_data, cg->assigned_names_len,
      cg->assigned_name_hashes_data, cg->assigned_name_hashes_len,
      cg->assigned_names_bloom, name, h);
}

static void gen_loop_control(codegen_t *cg, scope *scopes, size_t depth,
                             token_t tok, bool is_continue) {
  LLVMBasicBlockRef jump_bb = NULL;
  for (ssize_t i = (ssize_t)depth; i >= 0; i--) {
    emit_defers(cg, scopes, (size_t)i, (size_t)i);
    if (is_continue ? scopes[i].continue_bb : scopes[i].break_bb) {
      jump_bb = is_continue ? scopes[i].continue_bb : scopes[i].break_bb;
      break;
    }
  }
  if (jump_bb) {
    ny_dbg_loc(cg, tok);
    LLVMBasicBlockRef from_bb = LLVMGetInsertBlock(cg->builder);
    ny_braun_add_predecessor(cg, jump_bb, from_bb);
    LLVMBuildBr(cg->builder, jump_bb);
    return;
  }
  ny_diag_error(tok, "'%s' used outside of a loop",
                is_continue ? "continue" : "break");
  if (verbose_enabled >= 1)
    ny_diag_hint("'%s' is only valid inside while/for bodies",
                 is_continue ? "continue" : "break");
  cg->had_error = 1;
}

static void emit_trace_loc(codegen_t *cg, token_t tok) {
  if (!cg || !cg->trace_exec || !cg->builder)
    return;
  fun_sig *ts = lookup_fun(cg, "__trace_loc");
  if (!ts)
    return;
  const char *fname = tok.filename ? tok.filename : "<unknown>";
  LLVMValueRef fstr_g = const_string_ptr(cg, fname, strlen(fname));
  LLVMValueRef fstr = LLVMBuildLoad2(cg->builder, cg->type_i64, fstr_g, "");
  int line = tok.line > 0 ? tok.line : 1;
  int col = tok.col > 0 ? tok.col : 1;
  LLVMValueRef line_v =
      LLVMConstInt(cg->type_i64, ((uint64_t)line << 1) | 1, false);
  LLVMValueRef col_v =
      LLVMConstInt(cg->type_i64, ((uint64_t)col << 1) | 1, false);
  LLVMBuildCall2(cg->builder, ts->type, ts->value,
                 (LLVMValueRef[]){fstr, line_v, col_v}, 3, "");
}

static bool stmt_is_direct_thread_attr_call(codegen_t *cg, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *name = e->as.call.callee->as.ident.name;
  if (!name || !*name)
    return false;
  fun_sig *sig = lookup_fun(cg, name);
  if (!sig || sig->is_extern || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  return sig->stmt_t->as.fn.attr_thread;
}

static label_binding *find_label_binding(codegen_t *cg, const char *name) {
  if (!cg || !name)
    return NULL;
  for (size_t i = 0; i < cg->labels.len; i++) {
    if (strcmp(cg->labels.data[i].name, name) == 0)
      return &cg->labels.data[i];
  }
  return NULL;
}

static bool match_pattern_is_wild(expr_t *pat) {
  return pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
         strcmp(pat->as.ident.name, "_") == 0;
}

static bool match_pattern_is_okerr(expr_t *pat, bool *is_ok, bool *is_err) {
  if (!pat || pat->kind != NY_E_CALL || !pat->as.call.callee ||
      pat->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *callee = pat->as.call.callee->as.ident.name;
  if (!callee)
    return false;
  if (strcmp(callee, "ok") == 0) {
    if (is_ok)
      *is_ok = true;
    return true;
  }
  if (strcmp(callee, "err") == 0) {
    if (is_err)
      *is_err = true;
    return true;
  }
  return false;
}

static enum_member_def_t *match_pattern_enum_member(codegen_t *cg, expr_t *pat,
                                                    enum_def_t **out_enum) {
  if (!pat)
    return NULL;
  if (pat->kind == NY_E_IDENT) {
    return lookup_enum_member_owner(cg, pat->as.ident.name, out_enum);
  }
  if (pat->kind == NY_E_MEMBER) {
    char *full_name = codegen_full_name(cg, pat, cg->arena);
    if (!full_name)
      return NULL;
    return lookup_enum_member_owner(cg, full_name, out_enum);
  }
  return NULL;
}

static LLVMValueRef match_pattern_result_cond(codegen_t *cg, scope *scopes,
                                              size_t depth, stmt_t *owner_stmt,
                                              LLVMValueRef testv, expr_t *pat) {
  if (!pat || pat->kind != NY_E_CALL || !pat->as.call.callee ||
      pat->as.call.callee->kind != NY_E_IDENT)
    return NULL;

  const char *callee = pat->as.call.callee->as.ident.name;
  if (!callee)
    return NULL;
  bool is_ok_pat = (strcmp(callee, "ok") == 0);
  bool is_err_pat = (strcmp(callee, "err") == 0);
  if (!is_ok_pat && !is_err_pat)
    return NULL;

  fun_sig *check_sig = lookup_fun(cg, is_ok_pat ? "__is_ok" : "__is_err");
  if (!check_sig)
    return NULL;

  LLVMValueRef tag_check =
      LLVMBuildCall2(cg->builder, check_sig->type, check_sig->value,
                     (LLVMValueRef[]){testv}, 1, "");
  LLVMValueRef pat_cond = to_bool(cg, tag_check);

  if (pat->as.call.args.len > 0) {
    expr_t *arg0 = pat->as.call.args.data[0].val;
    if (arg0 && arg0->kind == NY_E_IDENT) {
      fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap");
      if (unwrap_sig) {
        LLVMValueRef val =
            LLVMBuildCall2(cg->builder, unwrap_sig->type, unwrap_sig->value,
                           (LLVMValueRef[]){testv}, 1, "");
        scope_bind(cg, scopes, depth, arg0->as.ident.name, val, owner_stmt,
                   false, NULL, false);
      }
    }
  }
  return pat_cond;
}

static const char *binding_assign_type(const binding *b) {
  if (!b)
    return NULL;
  if (b->decl_type_name && *b->decl_type_name)
    return b->decl_type_name;
  return b->type_name;
}

static void match_check_exhaustive(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  if (s->as.match.default_conseq)
    return;
  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    if (s->as.match.arms.data[i].guard) {
      // Guards can reject otherwise-matching patterns at runtime, so static
      // exhaustiveness checks are not reliable here.
      return;
    }
  }

  bool has_wild = false;
  bool has_other = false;
  bool has_ok = false;
  bool has_err = false;
  bool has_true = false;
  bool has_false = false;
  enum_def_t *enum_owner = NULL;
  VEC(const char *) matched;
  vec_init(&matched);

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    for (size_t j = 0; j < arm->patterns.len; ++j) {
      expr_t *pat = arm->patterns.data[j];
      if (match_pattern_is_wild(pat)) {
        has_wild = true;
        continue;
      }
      if (match_pattern_is_okerr(pat, &has_ok, &has_err))
        continue;

      if (pat && pat->kind == NY_E_LITERAL &&
          pat->as.literal.kind == NY_LIT_BOOL) {
        if (pat->as.literal.as.b)
          has_true = true;
        else
          has_false = true;
        continue;
      }

      enum_def_t *owner = NULL;
      enum_member_def_t *mem = match_pattern_enum_member(cg, pat, &owner);
      if (mem && owner) {
        if (!enum_owner)
          enum_owner = owner;
        else if (enum_owner != owner) {
          enum_owner = NULL;
          has_other = true;
        }
        bool seen = false;
        for (size_t k = 0; k < matched.len; k++) {
          if (strcmp(matched.data[k], mem->name) == 0) {
            seen = true;
            ny_diag_warning(pat->tok, "duplicate match pattern '%s'",
                            mem->name);
            break;
          }
        }
        if (!seen)
          vec_push(&matched, mem->name);
        continue;
      }

      has_other = true;
    }
  }

  if (has_wild) {
    vec_free(&matched);
    return;
  }

  if (!has_other && (has_ok || has_err)) {
    if (!has_ok || !has_err) {
      ny_diag_error(s->tok, "non-exhaustive match for Result (missing %s arm)",
                    has_ok ? "err(...)" : "ok(...)");
      cg->had_error = 1;
    }
    vec_free(&matched);
    return;
  }

  if (!has_other && !enum_owner && !has_ok && !has_err &&
      (has_true || has_false)) {
    if (!(has_true && has_false)) {
      ny_diag_error(s->tok, "non-exhaustive match for bool (missing %s)",
                    has_true ? "false" : "true");
      cg->had_error = 1;
    }
    vec_free(&matched);
    return;
  }

  if (enum_owner && !has_other && !has_ok && !has_err) {
    size_t missing = 0;
    char buf[256];
    buf[0] = '\0';
    size_t buf_len = 0;
    for (size_t i = 0; i < enum_owner->members.len; i++) {
      const char *name = enum_owner->members.data[i].name;
      bool found = false;
      for (size_t j = 0; j < matched.len; j++) {
        if (strcmp(matched.data[j], name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        missing++;
        if (buf_len + strlen(name) + 2 < sizeof(buf)) {
          if (buf_len)
            buf[buf_len++] = ',';
          if (buf_len)
            buf[buf_len++] = ' ';
          strcpy(buf + buf_len, name);
          buf_len += strlen(name);
        }
      }
    }
    if (missing > 0) {
      if (buf_len > 0) {
        ny_diag_error(s->tok,
                      "non-exhaustive match for enum '%s' (missing: %s)",
                      enum_owner->name, buf);
      } else {
        ny_diag_error(s->tok, "non-exhaustive match for enum '%s'",
                      enum_owner->name);
      }
      cg->had_error = 1;
    }
  }

  vec_free(&matched);
}

static void gen_stmt_with_null_narrow(codegen_t *cg, scope *scopes,
                                      size_t *depth,
                                      const ny_null_narrow_list_t *narrow,
                                      bool true_branch, stmt_t *body,
                                      size_t func_root, bool is_tail) {
  if (!body)
    return;
  ny_null_narrow_restore_list_t applied;
  ny_null_narrow_apply(cg, scopes, *depth, narrow, true_branch, &applied);
  gen_stmt(cg, scopes, depth, body, func_root, is_tail);
  ny_null_narrow_restore(&applied);
}

static void gen_stmt_if(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                        size_t func_root, bool is_tail) {
  ny_null_narrow_list_t narrow;
  vec_init(&narrow);
  (void)ny_null_narrow_collect(s->as.iff.test, &narrow);

  LLVMValueRef val = gen_expr(cg, scopes, *depth, s->as.iff.test);
  if (LLVMIsAConstantInt(val)) {
    uint64_t raw = LLVMConstIntGetZExtValue(val);
    // Nytrix truthiness: not None (0), not false (4), not 0 (1)
    bool truthy = (raw != 0 && raw != 4 && raw != 1);
    if (truthy) {
      gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, true,
                                s->as.iff.conseq, func_root, is_tail);
    } else if (s->as.iff.alt) {
      gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, false,
                                s->as.iff.alt, func_root, is_tail);
    }
    vec_free(&narrow);
    return;
  }

  LLVMValueRef c = to_bool(cg, val);
  LLVMBasicBlockRef entry = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));

  LLVMBasicBlockRef tb = LLVMAppendBasicBlock(f, "it"),
                    eb = s->as.iff.alt ? LLVMAppendBasicBlock(f, "ie") : NULL,
                    next = LLVMAppendBasicBlock(f, "in");
  LLVMBasicBlockRef then_end = NULL;
  LLVMBasicBlockRef else_end = NULL;
  bool then_fallthrough = false;
  bool else_fallthrough = false;
  ny_dbg_loc(cg, s->tok);
  LLVMBuildCondBr(cg->builder, c, tb, eb ? eb : next);
  ny_braun_add_predecessor(cg, tb, entry);
  if (eb)
    ny_braun_add_predecessor(cg, eb, entry);
  else
    ny_braun_add_predecessor(cg, next, entry);
  LLVMPositionBuilderAtEnd(cg->builder, tb);
  ny_braun_enter_block(cg, tb);
  ny_braun_seal_block(cg, tb);
  gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, true, s->as.iff.conseq,
                            func_root, is_tail);
  then_end = LLVMGetInsertBlock(cg->builder);
  then_fallthrough = !LLVMGetBasicBlockTerminator(then_end);
  if (then_fallthrough)
    LLVMBuildBr(cg->builder, next);
  if (eb) {
    LLVMPositionBuilderAtEnd(cg->builder, eb);
    ny_braun_enter_block(cg, eb);
    ny_braun_seal_block(cg, eb);
    gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, false, s->as.iff.alt,
                              func_root, is_tail);
    else_end = LLVMGetInsertBlock(cg->builder);
    else_fallthrough = !LLVMGetBasicBlockTerminator(else_end);
    if (else_fallthrough)
      LLVMBuildBr(cg->builder, next);
  }
  LLVMPositionBuilderAtEnd(cg->builder, next);
  ny_braun_enter_block(cg, next);
  if (then_fallthrough)
    ny_braun_add_predecessor(cg, next, then_end);
  if (eb && else_fallthrough)
    ny_braun_add_predecessor(cg, next, else_end);
  ny_braun_seal_block(cg, next);
  vec_free(&narrow);
}

static void gen_stmt_block(codegen_t *cg, scope *scopes, size_t *depth,
                           stmt_t *s, size_t func_root, bool is_tail) {
  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, scopes[*depth].break_bb,
              scopes[*depth].continue_bb);
  for (size_t i = 0; i < s->as.block.body.len; i++) {
    if (cg->builder) {
      LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
      if (cur && LLVMGetBasicBlockTerminator(cur)) {
        stmt_t *unreach = s->as.block.body.data[i];
        // Stdlib has many comptime-pruned branches; avoid noisy false positives
        // unless diagnostics are explicitly strict/verbose.
        bool stdlib_quiet = ny_is_stdlib_tok(unreach->tok) &&
                            !ny_strict_error_enabled(cg, unreach->tok) &&
                            verbose_enabled < 2;
        if (!stdlib_quiet)
          ny_diag_warning(unreach->tok, "unreachable code");
        break;
      }
    }
    gen_stmt(cg, scopes, depth, s->as.block.body.data[i], func_root,
             is_tail && (i == s->as.block.body.len - 1));
  }
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    emit_defers(cg, scopes, *depth, *depth);
  scope_pop(scopes, depth);
  codegen_debug_pop_block(cg, dbg_scope);
}

static void gen_stmt_while(codegen_t *cg, scope *scopes, size_t *depth,
                           stmt_t *s, size_t func_root) {
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  LLVMBasicBlockRef pre = LLVMGetInsertBlock(cg->builder);
  LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "wc"),
                    bb = LLVMAppendBasicBlock(f, "wb"),
                    eb = LLVMAppendBasicBlock(f, "we");
  ny_dbg_loc(cg, s->tok);
  LLVMBuildBr(cg->builder, cb);
  ny_braun_add_predecessor(cg, cb, pre);
  LLVMPositionBuilderAtEnd(cg->builder, cb);
  ny_braun_enter_block(cg, cb);
  ny_dbg_loc(cg, s->tok);
  LLVMBuildCondBr(cg->builder,
                  to_bool(cg, gen_expr(cg, scopes, *depth, s->as.whl.test)), bb,
                  eb);
  ny_braun_add_predecessor(cg, bb, cb);
  ny_braun_add_predecessor(cg, eb, cb);
  ny_braun_seal_block(cg, bb);
  LLVMPositionBuilderAtEnd(cg->builder, bb);
  ny_braun_enter_block(cg, bb);
  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, eb, cb);
  gen_stmt(cg, scopes, depth, s->as.whl.body, func_root, false);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    emit_defers(cg, scopes, *depth, *depth);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      ny_dbg_loc(cg, s->tok);
      ny_braun_add_predecessor(cg, cb, LLVMGetInsertBlock(cg->builder));
      LLVMBuildBr(cg->builder, cb);
    }
  }
  scope_pop(scopes, depth);
  codegen_debug_pop_block(cg, dbg_scope);
  ny_braun_seal_block(cg, cb);
  LLVMPositionBuilderAtEnd(cg->builder, eb);
  ny_braun_enter_block(cg, eb);
  ny_braun_seal_block(cg, eb);
}

static bool get_for_iter_helpers(codegen_t *cg, token_t tok, fun_sig **ls,
                                 fun_sig **gs) {
  *ls = lookup_fun(cg, "std.core.len");
  if (!*ls)
    *ls = lookup_fun(cg, "len");
  *gs = lookup_fun(cg, "std.core.get");
  if (!*gs)
    *gs = lookup_fun(cg, "get");
  if (*ls && *gs)
    return true;
  ny_diag_error(tok, "for-loop over iterable requires std.core.len/get");
  if (verbose_enabled >= 1)
    ny_diag_hint("import std.core or ensure std.core.len/get are available");
  cg->had_error = 1;
  return false;
}

static void gen_stmt_for(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                         size_t func_root) {
  LLVMValueRef itv = gen_expr(cg, scopes, *depth, s->as.fr.iterable);
  LLVMBasicBlockRef pre = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef f = LLVMGetBasicBlockParent(pre);
  LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "fc"),
                    bb = LLVMAppendBasicBlock(f, "fb"),
                    lb = LLVMAppendBasicBlock(f, "fl"),
                    eb = LLVMAppendBasicBlock(f, "fe");
  fun_sig *ls = NULL, *gs = NULL;
  if (!get_for_iter_helpers(cg, s->tok, &ls, &gs))
    return;
  // calls in the condition block.
  LLVMValueRef n_val = LLVMBuildCall2(cg->builder, ls->type, ls->value,
                                      (LLVMValueRef[]){itv}, 1, "");
  ny_dbg_loc(cg, s->tok);
  LLVMBuildBr(cg->builder, cb);
  ny_braun_add_predecessor(cg, cb, pre);
  LLVMPositionBuilderAtEnd(cg->builder, cb);
  ny_braun_enter_block(cg, cb);
  LLVMValueRef i_start = LLVMConstInt(cg->type_i64, 1, false);
  LLVMValueRef i_val = LLVMBuildPhi(cg->builder, cg->type_i64, "for_i");
  LLVMAddIncoming(i_val, &i_start, &pre, 1);
  ny_dbg_loc(cg, s->tok);
  LLVMBuildCondBr(cg->builder,
                  LLVMBuildICmp(cg->builder, LLVMIntSLT, i_val, n_val, ""), bb,
                  eb);
  ny_braun_add_predecessor(cg, bb, cb);
  ny_braun_add_predecessor(cg, eb, cb);
  ny_braun_seal_block(cg, bb);
  LLVMPositionBuilderAtEnd(cg->builder, bb);
  ny_braun_enter_block(cg, bb);
  LLVMValueRef item = LLVMBuildCall2(cg->builder, gs->type, gs->value,
                                     (LLVMValueRef[]){itv, i_val}, 2, "");
  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, eb, lb);
  scope_bind(cg, scopes, *depth, s->as.fr.iter_var, item, s, false, NULL,
             false);
  gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    emit_defers(cg, scopes, *depth, *depth);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      ny_dbg_loc(cg, s->tok);
      /* Normal for-loop flow must go through the latch so i_next is computed
       * before re-entering the condition block. Jumping directly to cb keeps
       * i_val unchanged and can spin forever. */
      ny_braun_add_predecessor(cg, lb, LLVMGetInsertBlock(cg->builder));
      LLVMBuildBr(cg->builder, lb);
    }
  }
  scope_pop(scopes, depth);
  codegen_debug_pop_block(cg, dbg_scope);
  LLVMPositionBuilderAtEnd(cg->builder, lb);
  ny_braun_enter_block(cg, lb);
  ny_braun_seal_block(cg, lb);
  LLVMValueRef i_next = LLVMBuildAdd(cg->builder, i_val,
                                     LLVMConstInt(cg->type_i64, 2, false), "");
  ny_dbg_loc(cg, s->tok);
  ny_braun_add_predecessor(cg, cb, lb);
  LLVMBuildBr(cg->builder, cb);
  LLVMAddIncoming(i_val, &i_next, &lb, 1);
  ny_braun_seal_block(cg, cb);
  LLVMPositionBuilderAtEnd(cg->builder, eb);
  ny_braun_enter_block(cg, eb);
  ny_braun_seal_block(cg, eb);
}

static void gen_stmt_try(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                         size_t func_root, bool is_tail) {
  fun_sig *sz_fn = lookup_fun(cg, "__jmpbuf_size");
  fun_sig *set_env = lookup_fun(cg, "__set_panic_env");
  fun_sig *clr_env = lookup_fun(cg, "__clear_panic_env");
  fun_sig *get_err = lookup_fun(cg, "__get_panic_val");
  if (!sz_fn || !set_env || !clr_env || !get_err) {
    ny_diag_error(s->tok, "missing runtime support for try/catch");
    if (verbose_enabled >= 1)
      ny_diag_hint("required runtime symbols: "
                   "__jmpbuf_size/__set_panic_env/__clear_panic_env/"
                   "__get_panic_val");
    cg->had_error = 1;
    return;
  }
  LLVMValueRef sz_val =
      LLVMBuildCall2(cg->builder, sz_fn->type, sz_fn->value, NULL, 0, "");
  LLVMValueRef jmpbuf = LLVMBuildArrayAlloca(
      cg->builder, LLVMInt8TypeInContext(cg->ctx), sz_val, "jmpbuf");
  /* Keep jmp_buf sufficiently aligned across ABIs (notably Windows). */
  LLVMSetAlignment(jmpbuf, 16);
  LLVMValueRef jmpbuf_ptr =
      LLVMBuildPtrToInt(cg->builder, jmpbuf, cg->type_i64, "");
  LLVMBuildCall2(cg->builder, set_env->type, set_env->value,
                 (LLVMValueRef[]){jmpbuf_ptr}, 1, "");
  LLVMValueRef setjmp_func = NULL;
#ifdef _WIN32
  setjmp_func = LLVMGetNamedFunction(cg->module, "_setjmp");
  if (!setjmp_func)
    setjmp_func = LLVMGetNamedFunction(cg->module, "setjmp");
#else
  setjmp_func = LLVMGetNamedFunction(cg->module, "_setjmp");
  if (!setjmp_func)
    setjmp_func = LLVMGetNamedFunction(cg->module, "setjmp");
#endif
  LLVMTypeRef arg_t = ny_llvm_ptr_type(cg->ctx);
  LLVMTypeRef ret_t = LLVMInt32TypeInContext(cg->ctx);
  if (!setjmp_func) {
#ifdef _WIN32
    setjmp_func = LLVMAddFunction(
        cg->module, "_setjmp",
        LLVMFunctionType(ret_t, (LLVMTypeRef[]){arg_t, arg_t}, 2, 0));
#else
    setjmp_func = LLVMAddFunction(cg->module, "setjmp",
                                  LLVMFunctionType(ret_t, &arg_t, 1, 0));
#endif
  }
#ifdef _WIN32
  LLVMValueRef sj_res = LLVMBuildCall2(
      cg->builder, LLVMGlobalGetValueType(setjmp_func), setjmp_func,
      (LLVMValueRef[]){jmpbuf, LLVMConstNull(arg_t)}, 2, "sj_res");
#else
  LLVMValueRef sj_res =
      LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(setjmp_func),
                     setjmp_func, (LLVMValueRef[]){jmpbuf}, 1, "sj_res");
#endif
  LLVMValueRef func = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  LLVMBasicBlockRef try_b =
      LLVMAppendBasicBlockInContext(cg->ctx, func, "try_body");
  LLVMBasicBlockRef catch_b =
      LLVMAppendBasicBlockInContext(cg->ctx, func, "catch_body");
  LLVMBasicBlockRef end_b =
      LLVMAppendBasicBlockInContext(cg->ctx, func, "try_end");
  LLVMBuildCondBr(
      cg->builder,
      LLVMBuildICmp(cg->builder, LLVMIntEQ, sj_res,
                    LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false),
                    ""),
      try_b, catch_b);
  LLVMPositionBuilderAtEnd(cg->builder, try_b);
  gen_stmt(cg, scopes, depth, s->as.tr.body, func_root, is_tail);
  LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    LLVMBuildBr(cg->builder, end_b);
  LLVMPositionBuilderAtEnd(cg->builder, catch_b);
  LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
  LLVMValueRef err_val = LLVMBuildCall2(cg->builder, get_err->type,
                                        get_err->value, NULL, 0, "err");
  if (s->as.tr.err) {
    LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
    scope_enter(scopes, depth, scopes[*depth].break_bb,
                scopes[*depth].continue_bb);
    scope_bind(cg, scopes, *depth, s->as.tr.err, err_val, s, false, NULL,
               false);
    gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
    scope_pop(scopes, depth);
    codegen_debug_pop_block(cg, dbg_scope);
  } else {
    gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
  }
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    LLVMBuildBr(cg->builder, end_b);
  LLVMPositionBuilderAtEnd(cg->builder, end_b);
}

static void gen_stmt_defer(codegen_t *cg, scope *scopes, size_t depth,
                           stmt_t *s) {
  if (!s->as.de.body)
    return;

  ny_param_list no_params = {0};
  LLVMValueRef cls = gen_closure(cg, scopes, depth, no_params, s->as.de.body,
                                 false, NULL, "__defer");
  LLVMValueRef cls_raw =
      LLVMBuildIntToPtr(cg->builder, cls, LLVMPointerType(cg->type_i64, 0), "");
  LLVMValueRef fn_ptr_int =
      LLVMBuildLoad2(cg->builder, cg->type_i64, cls_raw, "defer_fn");
  LLVMValueRef env_addr = LLVMBuildGEP2(
      cg->builder, cg->type_i64, cls_raw,
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false)}, 1, "");
  LLVMValueRef env =
      LLVMBuildLoad2(cg->builder, cg->type_i64, env_addr, "defer_env");

  fun_sig *push_sig = lookup_fun(cg, "__push_defer");
  if (push_sig) {
    LLVMBuildCall2(cg->builder, push_sig->type, push_sig->value,
                   (LLVMValueRef[]){fn_ptr_int, env}, 2, "");
  }

  vec_push(&scopes[depth].defers, s->as.de.body);
}

static void gen_stmt_struct(codegen_t *cg, stmt_t *s) {
  if (lookup_layout(cg, s->as.struc.name))
    return;

  const char *name = s->as.struc.name;
  LLVMTypeRef st = LLVMStructCreateNamed(cg->ctx, name);

  // Register early to allow recursive pointers
  layout_def_t *def = malloc(sizeof(layout_def_t));
  memset(def, 0, sizeof(*def));
  def->name = ny_strdup(name);
  def->llvm_type = st;
  def->is_layout = (s->kind == NY_S_LAYOUT);
  def->stmt = s;
  vec_push(&cg->layouts, def);

  size_t count = s->as.struc.fields.len;
  LLVMTypeRef *element_types =
      malloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));

  for (size_t i = 0; i < count; i++) {
    layout_field_t *f = &s->as.struc.fields.data[i];
    type_layout_t f_layout = resolve_raw_layout(cg, f->type_name, s->tok);
    if (f_layout.is_valid && f_layout.llvm_type) {
      element_types[i] = f_layout.llvm_type;
    } else {
      element_types[i] = cg->type_i64;
    }
  }

  LLVMStructSetBody(st, element_types, (unsigned)count,
                    s->as.struc.pack ? true : false);
  free(element_types);

  LLVMTargetDataRef td = LLVMGetModuleDataLayout(cg->module);
  def->size = LLVMStoreSizeOfType(td, st);
  def->align = LLVMABIAlignmentOfType(td, st);
}

static void gen_stmt_module(codegen_t *cg, scope *scopes, size_t *depth,
                            stmt_t *s, size_t func_root, bool is_tail) {
  const char *prev = cg->current_module_name;
  cg->current_module_name = s->as.module.name;
  for (size_t i = 0; i < s->as.module.body.len; ++i) {
    if (s->as.module.body.data[i]->kind == NY_S_FUNC)
      continue;
    gen_stmt(cg, scopes, depth, s->as.module.body.data[i], func_root, is_tail);
  }
  cg->current_module_name = prev;
}

void emit_defers(codegen_t *cg, scope *scopes, size_t depth, size_t func_root) {
  fun_sig *pop_sig = lookup_fun(cg, "__pop_run_defer");
  if (!pop_sig)
    return;
  for (ssize_t d = (ssize_t)depth; d >= (ssize_t)func_root; --d) {
    for (size_t i = 0; i < scopes[d].defers.len; ++i) {
      LLVMBuildCall2(cg->builder, pop_sig->type, pop_sig->value, NULL, 0, "");
    }
  }
}

void collect_labels(codegen_t *cg, LLVMValueRef func, stmt_t *s, size_t depth) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      collect_labels(cg, func, s->as.block.body.data[i], depth + 1);
    break;
  case NY_S_IF:
    collect_labels(cg, func, s->as.iff.conseq, depth + 1);
    if (s->as.iff.alt)
      collect_labels(cg, func, s->as.iff.alt, depth + 1);
    break;
  case NY_S_WHILE:
    collect_labels(cg, func, s->as.whl.body, depth + 1);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      collect_labels(cg, func, s->as.match.arms.data[i].conseq, depth + 1);
    if (s->as.match.default_conseq)
      collect_labels(cg, func, s->as.match.default_conseq, depth + 1);
    break;
  case NY_S_FOR:
    collect_labels(cg, func, s->as.fr.body, depth + 1);
    break;
  case NY_S_TRY:
    collect_labels(cg, func, s->as.tr.body, depth + 1);
    collect_labels(cg, func, s->as.tr.handler, depth + 1);
    break;
  case NY_S_LABEL: {
    label_binding lb;
    lb.name = ny_strdup(s->as.label.name);
    lb.bb = LLVMAppendBasicBlock(func, s->as.label.name);
    lb.depth = depth;
    vec_push(&cg->labels, lb);
    break;
  }
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_labels(cg, func, s->as.module.body.data[i], depth);
    break;
  case NY_S_DEFER:
    collect_labels(cg, func, s->as.de.body, depth + 1);
    break;
  case NY_S_MACRO:
    collect_labels(cg, func, s->as.macro.body, depth + 1);
    break;
  default:
    break;
  }
}

void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
              size_t func_root, bool is_tail) {
  if (!s || cg->had_error)
    return;
  ny_braun_mark_current_block(cg);
  ny_dbg_loc(cg, s->tok);
  emit_trace_loc(cg, s->tok);
  switch (s->kind) {
  case NY_S_VAR: {
    bool dest = s->as.var.is_destructure;
    bool parallel = (s->as.var.names.len == s->as.var.exprs.len) && !dest;
    bool prefer_direct_locals = (cg->braun && cg->braun->enabled);

    // Evaluate first expression if not parallel (broadcast or destructure)
    LLVMValueRef first_val = NULL;
    if (!parallel && s->as.var.exprs.len > 0) {
      first_val = gen_expr(cg, scopes, *depth, s->as.var.exprs.data[0]);
    }

    fun_sig *gs = NULL;
    if (dest) {
      gs = lookup_fun(cg, "get");
      if (!gs) {
        ny_diag_error(s->tok, "destructuring requires 'get' function");
        if (verbose_enabled >= 1)
          ny_diag_hint("import std.core or ensure 'get' is in scope");
        cg->had_error = 1;
        return;
      }
    }

    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *n = s->as.var.names.data[i];
      LLVMValueRef slot = NULL;
      bool bind_direct = false;
      binding *resolved_local = NULL;
      binding *resolved_global = NULL;
      sema_var_t *sema = (sema_var_t *)s->sema;
      const char *decl_type = NULL;
      if (s->as.var.types.len > i)
        decl_type = s->as.var.types.data[i];
      if (s->as.var.is_undef) {
        bool target_is_global = false;
        LLVMValueRef zero = LLVMConstInt(cg->type_i64, 0, false);
        binding *eb =
            stmt_var_lookup_existing(cg, scopes, *depth, n, &target_is_global);
        if (!eb) {
          report_undef_symbol(cg, n, s->tok);
          return;
        }
        if (!ensure_mutable_binding_for_assign(cg, s->tok, n, eb,
                                               target_is_global)) {
          continue;
        }
        if (target_is_global || eb->is_slot) {
          slot = eb->value;
        } else {
          eb->value = zero;
          braun_write_local_value(cg, *depth, false, n, zero);
          continue;
        }
        if (ensure_store_ready(cg, s->tok, zero, slot, "NY_S_VAR(undef)")) {
          LLVMBuildStore(cg->builder, zero, slot);
          braun_write_local_value(cg, *depth, target_is_global, n, zero);
        }
        continue;
      }
      if (*depth == 0) {
        binding *gb = lookup_global(cg, n);
        if (gb) {
          resolved_global = gb;
          slot = gb->value;
        } else {
          slot = LLVMAddGlobal(cg->module, cg->type_i64,
                               n); // Global vars still default to i64 for now
          LLVMSetInitializer(slot, LLVMConstInt(cg->type_i64, 0, false));
          const char *type_name = NULL;
          if (s->as.var.types.len > i)
            type_name = s->as.var.types.data[i];
          binding b = {.name = ny_strdup(n),
                       .value = slot,
                       .stmt_t = NULL,
                       .is_slot = true,
                       .is_mut = s->as.var.is_mut ? true : false,
                       .is_used = false,
                       .owned = true,
                       .type_name = type_name,
                       .decl_type_name = type_name,
                       .name_hash = 0};
          vec_push(&cg->global_vars, b);
          if (cg->debug_symbols && cg->di_builder) {
            codegen_debug_global_variable(cg, n, slot, s->tok);
          }
          if (s->tok.filename) {
            ny_diag_warning(
                s->tok,
                "implicit declaration of global variable \033[1;37m'%s'\033[0m",
                n);
            if (verbose_enabled >= 2)
              ny_diag_hint(
                  "add an explicit declaration to avoid implicit globals");
          }
        }
      } else {
        if (s->as.var.is_decl) {
          stmt_var_setup_local_binding(cg, scopes, *depth, s, sema, i, n,
                                       decl_type, prefer_direct_locals,
                                       &bind_direct, &slot);
        } else {
          bool target_is_global = false;
          binding *eb = stmt_var_lookup_existing(cg, scopes, *depth, n,
                                                 &target_is_global);
          if (eb) {
            if (!ensure_mutable_binding_for_assign(cg, s->tok, n, eb,
                                                   target_is_global)) {
              continue;
            }
            if (target_is_global) {
              resolved_global = eb;
              if (!eb->is_slot) {
                ny_diag_error(s->tok,
                              "cannot assign to non-addressable value '%s'", n);
                cg->had_error = 1;
                continue;
              }
              slot = eb->value;
            } else {
              resolved_local = eb;
              if (eb->is_slot)
                slot = eb->value;
            }
          } else {
            // Implicit declaration in local scope
            stmt_var_setup_local_binding(cg, scopes, *depth, s, sema, i, n,
                                         NULL, prefer_direct_locals,
                                         &bind_direct, &slot);
          }
        }
      }

      LLVMValueRef target_val = NULL;
      expr_t *expr_for_check = NULL;
      if (parallel) {
        expr_for_check = s->as.var.exprs.data[i];
        target_val = gen_expr(cg, scopes, *depth, expr_for_check);
      } else if (dest) {
        // Tagged integer index: i -> (i << 1) | 1
        uint64_t tagged_idx = ((uint64_t)i << 1) | 1;
        LLVMValueRef idx_val = LLVMConstInt(cg->type_i64, tagged_idx, false);
        target_val =
            LLVMBuildCall2(cg->builder, gs->type, gs->value,
                           (LLVMValueRef[]){first_val, idx_val}, 2, "");
      } else {
        if (s->as.var.exprs.len > 0)
          expr_for_check = s->as.var.exprs.data[0];
        target_val = first_val;
      }

      if (!s->as.var.is_destructure) {
        const char *want = decl_type;
        if (!s->as.var.is_decl) {
          binding *cur = resolved_local;
          if (cur)
            want = binding_assign_type(cur);
          else if (resolved_global)
            want = binding_assign_type(resolved_global);
        }
        if (want && expr_for_check)
          ensure_expr_type_compatible(cg, scopes, *depth, want, expr_for_check,
                                      expr_for_check->tok, "assignment");
      }

      if (bind_direct) {
        scope_bind(cg, scopes, *depth, n, target_val, s, s->as.var.is_mut,
                   decl_type, false);
        braun_write_local_value(cg, *depth, false, n, target_val);
        continue;
      }
      if (resolved_local && !resolved_local->is_slot) {
        resolved_local->value = target_val;
        braun_write_local_value(cg, *depth, false, n, target_val);
        continue;
      }
      if (ensure_store_ready(cg, s->tok, target_val, slot, "NY_S_VAR")) {
        LLVMBuildStore(cg->builder, target_val, slot);
        bool is_global_target = (*depth == 0) || (resolved_global != NULL);
        braun_write_local_value(cg, *depth, is_global_target, n, target_val);
      }
    }
    break;
  }
  case NY_S_EXPR: {
    expr_t *e = s->as.expr.expr;
    if (!e) {
      ny_diag_error(s->tok, "missing expression statement payload");
      cg->had_error = 1;
      return;
    }
    if (e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT) {
      fun_sig *sig = lookup_fun(cg, e->as.call.callee->as.ident.name);
      if (sig && sig->return_type && (size_t)sig->return_type > 0x1000) {
        if (strcmp(sig->return_type, "Result") == 0 ||
            strcmp(sig->return_type, "std.core.error.Result") == 0) {
          if (ny_diag_should_emit("unused_result", s->tok, sig->name)) {
            // Warning (or error in strict mode): result ignored
            bool strict_err = ny_strict_error_enabled(cg, s->tok);
            if (strict_err)
              ny_diag_error(s->tok, "unused Result from '%s'", sig->name);
            else
              ny_diag_warning(s->tok, "unused Result from '%s'", sig->name);
            ny_diag_hint("handle the value (e.g. with match/unwrap/propagate)");
            if (strict_err) {
              cg->had_error = 1;
              return;
            }
          }
        }
      }
    }
    bool prev_detach_stmt_call = cg->thread_detach_stmt_call;
    if (!is_tail && stmt_is_direct_thread_attr_call(cg, e))
      cg->thread_detach_stmt_call = true;
    LLVMValueRef v = gen_expr(cg, scopes, *depth, e);
    cg->thread_detach_stmt_call = prev_detach_stmt_call;
    if (!v) {
      ny_diag_error(s->tok, "failed to generate expression");
      cg->had_error = 1;
      return;
    }
    if (is_tail && !cg->current_fn_attr_naked) {
      if (cg->result_store_val) {
        LLVMBuildStore(cg->builder, v, cg->result_store_val);
      } else {
        emit_defers(cg, scopes, *depth, func_root);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
          LLVMBuildRet(cg->builder, v);
      }
    }
    break;
  }
  case NY_S_IF: {
    gen_stmt_if(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_MATCH: {
    match_check_exhaustive(cg, s);
    if (cg->had_error)
      return;
    LLVMValueRef testv = gen_expr(cg, scopes, *depth, s->as.match.test);
    LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
    LLVMBasicBlockRef end = LLVMAppendBasicBlock(f, "match_end");
    LLVMTypeRef i1 = LLVMInt1TypeInContext(cg->ctx);

    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      LLVMBasicBlockRef arm_bb = LLVMAppendBasicBlock(f, "match_arm");
      LLVMBasicBlockRef next_bb = LLVMAppendBasicBlock(f, "match_next");
      LLVMValueRef cond = NULL;
      int has_wild = 0;

      // Create a sub-scope for the arm
      scope_enter(scopes, depth, scopes[*depth].break_bb,
                  scopes[*depth].continue_bb);

      for (size_t j = 0; j < arm->patterns.len; ++j) {
        expr_t *pat = arm->patterns.data[j];
        if (match_pattern_is_wild(pat)) {
          has_wild = 1;
          break;
        }

        LLVMValueRef pat_cond =
            match_pattern_result_cond(cg, scopes, *depth, s, testv, pat);

        if (!pat_cond) {
          LLVMValueRef pv = gen_expr(cg, scopes, *depth, pat);
          LLVMValueRef eq = gen_binary(cg, "==", testv, pv);
          pat_cond = to_bool(cg, eq);
        }

        cond = cond ? LLVMBuildOr(cg->builder, cond, pat_cond, "") : pat_cond;
      }

      if (has_wild) {
        cond = LLVMConstInt(i1, 1, false);
      } else if (!cond) {
        cond = LLVMConstInt(i1, 0, false);
      }

      token_t btok = s->tok;
      if (arm->patterns.len > 0 && arm->patterns.data[0])
        btok = arm->patterns.data[0]->tok;
      ny_dbg_loc(cg, btok);
      LLVMBuildCondBr(cg->builder, cond, arm_bb, next_bb);
      LLVMPositionBuilderAtEnd(cg->builder, arm_bb);
      if (arm->guard) {
        ny_dbg_loc(cg, arm->guard->tok);
        LLVMValueRef guard_v = gen_expr(cg, scopes, *depth, arm->guard);
        LLVMValueRef guard_b = to_bool(cg, guard_v);
        LLVMBasicBlockRef guard_pass_bb =
            LLVMAppendBasicBlock(f, "match_guard_pass");
        LLVMBuildCondBr(cg->builder, guard_b, guard_pass_bb, next_bb);
        LLVMPositionBuilderAtEnd(cg->builder, guard_pass_bb);
      }
      gen_stmt(cg, scopes, depth, arm->conseq, func_root, is_tail);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, end);

      scope_pop(scopes, depth);

      LLVMPositionBuilderAtEnd(cg->builder, next_bb);
    }
    if (s->as.match.default_conseq) {
      gen_stmt(cg, scopes, depth, s->as.match.default_conseq, func_root,
               is_tail);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, end);
    } else {
      LLVMBuildBr(cg->builder, end);
    }
    LLVMPositionBuilderAtEnd(cg->builder, end);
    break;
  }
  case NY_S_WHILE: {
    gen_stmt_while(cg, scopes, depth, s, func_root);
    break;
  }
  case NY_S_FOR: {
    gen_stmt_for(cg, scopes, depth, s, func_root);
    break;
  }
  case NY_S_RETURN: {
    if (cg->current_fn_ret_type && !s->as.ret.value) {
      const char *rt = cg->current_fn_ret_type;
      const char *dot = strrchr(rt, '.');
      const char *base = dot ? dot + 1 : rt;
      if (base && strcmp(base, "void") != 0) {
        ny_diag_error(s->tok, "missing return value for %s", base);
        cg->had_error = 1;
      }
    }
    if (cg->current_fn_ret_type && s->as.ret.value) {
      ensure_expr_type_compatible(cg, scopes, *depth, cg->current_fn_ret_type,
                                  s->as.ret.value, s->as.ret.value->tok,
                                  "return");
    }
    LLVMValueRef v = s->as.ret.value
                         ? gen_expr(cg, scopes, *depth, s->as.ret.value)
                         : LLVMConstInt(cg->type_i64, 1, false);
    emit_defers(cg, scopes, *depth, func_root);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      LLVMBuildRet(cg->builder, v);
    break;
  }
  case NY_S_USE: {
    break;
  }
  case NY_S_BLOCK: {
    gen_stmt_block(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_TRY: {
    gen_stmt_try(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_FUNC:
    gen_func(cg, s, s->as.fn.name, scopes, *depth, NULL);
    break;
  case NY_S_DEFER: {
    gen_stmt_defer(cg, scopes, *depth, s);
    break;
  }
  case NY_S_BREAK: {
    gen_loop_control(cg, scopes, *depth, s->tok, false);
    break;
  }
  case NY_S_CONTINUE: {
    gen_loop_control(cg, scopes, *depth, s->tok, true);
    break;
  }
  case NY_S_STRUCT: {
    gen_stmt_struct(cg, s);
    break;
  }
  case NY_S_LAYOUT: {
    break;
  }
  case NY_S_MODULE: {
    gen_stmt_module(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_EXPORT:
    break;
  case NY_S_LABEL: {
    label_binding *lb = find_label_binding(cg, s->as.label.name);
    if (lb) {
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        LLVMBuildBr(cg->builder, lb->bb);
      }
      LLVMPositionBuilderAtEnd(cg->builder, lb->bb);
      return;
    }
    break;
  }
  case NY_S_GOTO: {
    label_binding *lb = find_label_binding(cg, s->as.go.name);
    if (lb) {
      size_t target_depth = lb->depth;
      if (*depth > target_depth) {
        emit_defers(cg, scopes, *depth, target_depth + 1);
      }
      LLVMBuildBr(cg->builder, lb->bb);
      // Create a dummy block for any subsequent dead code to avoid
      // "terminator already exists"
      LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
      LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(f, "dead"));
      return;
    }
    ny_diag_error(s->tok, "undefined label \033[1;37m'%s'\033[0m",
                  s->as.go.name);
    cg->had_error = 1;
    break;
  }
  case NY_S_MACRO: {
    const char *macro_name = (s->as.macro.name && *s->as.macro.name)
                                 ? s->as.macro.name
                                 : "<unknown>";
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      expr_t *arg = s->as.macro.args.data[i];
      if (!arg)
        continue;
      (void)gen_expr(cg, scopes, *depth, arg);
    }
    if (s->as.macro.body) {
      ny_diag_warning(s->tok,
                      "macro statement '%s' fell back to direct body execution",
                      macro_name);
      if (verbose_enabled >= 1) {
        ny_diag_hint(
            "register '%s' in std.core.syntax to customize expansion behavior",
            macro_name);
      }
      gen_stmt(cg, scopes, depth, s->as.macro.body, func_root, is_tail);
      break;
    }
    ny_diag_warning(s->tok, "macro statement not implemented: '%s'",
                    macro_name);
    if (verbose_enabled >= 1) {
      ny_diag_hint("register '%s' in std.core.syntax to implement this macro",
                   macro_name);
    }
    break;
  }
  default:
    break;
  }
}
