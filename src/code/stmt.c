#include "base/util.h"
#include "priv.h"
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void scope_enter(scope *scopes, size_t *depth, LLVMBasicBlockRef break_bb,
                        LLVMBasicBlockRef continue_bb) {
  (*depth)++;
  scopes[*depth].vars.len = scopes[*depth].vars.cap = 0;
  scopes[*depth].vars.data = NULL;
  scopes[*depth].defers.len = scopes[*depth].defers.cap = 0;
  scopes[*depth].defers.data = NULL;
  scopes[*depth].break_bb = break_bb;
  scopes[*depth].continue_bb = continue_bb;
}

static void report_reassign_immutable(codegen_t *cg, token_t tok, const char *n,
                                      bool is_global, const binding *decl) {
  ny_diag_error(tok, "cannot reassign immutable %svariable \033[1;37m'%s'\033[0m",
                is_global ? "global " : "", n);
  if (decl && decl->stmt_t)
    ny_diag_note_tok(decl->stmt_t->tok, "'%s' declared here", n);
  if (verbose_enabled >= 1)
    ny_diag_hint("declare it with 'mut %s = ...' if reassignment is intended", n);
  ny_diag_fix("change the declaration to 'mut %s = ...' or assign to a new name", n);
  cg->had_error = 1;
}

static bool ensure_store_ready(codegen_t *cg, token_t tok, LLVMValueRef value,
                               LLVMValueRef slot, const char *ctx) {
  if (!cg->builder) {
    ny_diag_error(tok, "internal codegen failure: missing builder in %s", ctx);
    cg->had_error = 1;
    return false;
  }
  if (!value) {
    ny_diag_error(tok, "internal codegen failure: missing assignment value in %s",
                  ctx);
    cg->had_error = 1;
    return false;
  }
  if (!slot) {
    ny_diag_error(tok,
                  "internal codegen failure: missing assignment destination in %s",
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

static void gen_stmt_if(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                        size_t func_root, bool is_tail) {
  LLVMValueRef c = to_bool(cg, gen_expr(cg, scopes, *depth, s->as.iff.test));
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  LLVMBasicBlockRef tb = LLVMAppendBasicBlock(f, "it"),
                    eb = s->as.iff.alt ? LLVMAppendBasicBlock(f, "ie") : NULL,
                    next = LLVMAppendBasicBlock(f, "in");
  LLVMBuildCondBr(cg->builder, c, tb, eb ? eb : next);
  LLVMPositionBuilderAtEnd(cg->builder, tb);
  gen_stmt(cg, scopes, depth, s->as.iff.conseq, func_root, is_tail);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    LLVMBuildBr(cg->builder, next);
  if (eb) {
    LLVMPositionBuilderAtEnd(cg->builder, eb);
    gen_stmt(cg, scopes, depth, s->as.iff.alt, func_root, is_tail);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      LLVMBuildBr(cg->builder, next);
  }
  LLVMPositionBuilderAtEnd(cg->builder, next);
}

static void gen_stmt_block(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                           size_t func_root, bool is_tail) {
  scope_enter(scopes, depth, scopes[*depth].break_bb, scopes[*depth].continue_bb);
  for (size_t i = 0; i < s->as.block.body.len; i++) {
    if (cg->builder) {
      LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
      if (cur && LLVMGetBasicBlockTerminator(cur)) {
        stmt_t *unreach = s->as.block.body.data[i];
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
}

static void gen_stmt_while(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                           size_t func_root) {
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "wc"),
                    bb = LLVMAppendBasicBlock(f, "wb"),
                    eb = LLVMAppendBasicBlock(f, "we");
  LLVMBuildBr(cg->builder, cb);
  LLVMPositionBuilderAtEnd(cg->builder, cb);
  LLVMBuildCondBr(cg->builder,
                  to_bool(cg, gen_expr(cg, scopes, *depth, s->as.whl.test)), bb,
                  eb);
  LLVMPositionBuilderAtEnd(cg->builder, bb);
  scope_enter(scopes, depth, eb, cb);
  gen_stmt(cg, scopes, depth, s->as.whl.body, func_root, false);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    emit_defers(cg, scopes, *depth, *depth);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      LLVMBuildBr(cg->builder, cb);
  }
  scope_pop(scopes, depth);
  LLVMPositionBuilderAtEnd(cg->builder, eb);
}

static bool get_for_iter_helpers(codegen_t *cg, token_t tok, fun_sig **ls,
                                 fun_sig **gs) {
  *ls = lookup_fun(cg, "list_len");
  *gs = lookup_fun(cg, "get");
  if (*ls && *gs)
    return true;
  ny_diag_error(tok, "for-loop over iterable requires list_len/get");
  if (verbose_enabled >= 1)
    ny_diag_hint("import std.core or ensure list_len/get are available");
  cg->had_error = 1;
  return false;
}

static void gen_stmt_for(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                         size_t func_root) {
  LLVMValueRef itv = gen_expr(cg, scopes, *depth, s->as.fr.iterable);
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "fc"),
                    bb = LLVMAppendBasicBlock(f, "fb"),
                    eb = LLVMAppendBasicBlock(f, "fe");
  LLVMValueRef idx_p = build_alloca(cg, "idx");
  LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), idx_p);
  fun_sig *ls = NULL, *gs = NULL;
  if (!get_for_iter_helpers(cg, s->tok, &ls, &gs))
    return;
  LLVMBuildBr(cg->builder, cb);
  LLVMPositionBuilderAtEnd(cg->builder, cb);
  LLVMValueRef i_val = LLVMBuildLoad2(cg->builder, cg->type_i64, idx_p, "");
  LLVMValueRef n_val =
      LLVMBuildCall2(cg->builder, ls->type, ls->value, (LLVMValueRef[]){itv}, 1, "");
  LLVMBuildCondBr(cg->builder,
                  LLVMBuildICmp(cg->builder, LLVMIntSLT, i_val, n_val, ""), bb,
                  eb);
  LLVMPositionBuilderAtEnd(cg->builder, bb);
  LLVMValueRef item = LLVMBuildCall2(cg->builder, gs->type, gs->value,
                                     (LLVMValueRef[]){itv, i_val}, 2, "");
  LLVMValueRef iv = build_alloca(cg, s->as.fr.iter_var);
  LLVMBuildStore(cg->builder, item, iv);
  LLVMBuildStore(cg->builder,
                 LLVMBuildAdd(cg->builder, i_val,
                              LLVMConstInt(cg->type_i64, 2, false), ""),
                 idx_p);
  scope_enter(scopes, depth, eb, cb);
  bind(scopes, *depth, s->as.fr.iter_var, iv, s, false);
  gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    emit_defers(cg, scopes, *depth, *depth);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      LLVMBuildBr(cg->builder, cb);
  }
  scope_pop(scopes, depth);
  LLVMPositionBuilderAtEnd(cg->builder, eb);
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
  LLVMValueRef jmpbuf_ptr =
      LLVMBuildPtrToInt(cg->builder, jmpbuf, cg->type_i64, "");
  LLVMBuildCall2(cg->builder, set_env->type, set_env->value,
                 (LLVMValueRef[]){jmpbuf_ptr}, 1, "");
  LLVMValueRef setjmp_func = LLVMGetNamedFunction(cg->module, "_setjmp");
  if (!setjmp_func)
    setjmp_func = LLVMGetNamedFunction(cg->module, "setjmp");
  if (!setjmp_func) {
    LLVMTypeRef arg_t = LLVMPointerTypeInContext(cg->ctx, 0);
    LLVMTypeRef ret_t = LLVMInt32TypeInContext(cg->ctx);
    setjmp_func = LLVMAddFunction(cg->module, "setjmp",
                                  LLVMFunctionType(ret_t, &arg_t, 1, 0));
  }
  LLVMValueRef sj_res =
      LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(setjmp_func),
                     setjmp_func, (LLVMValueRef[]){jmpbuf}, 1, "sj_res");
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
  LLVMValueRef err_val =
      LLVMBuildCall2(cg->builder, get_err->type, get_err->value, NULL, 0, "err");
  if (s->as.tr.err) {
    scope_enter(scopes, depth, scopes[*depth].break_bb,
                scopes[*depth].continue_bb);
    LLVMValueRef err_var = build_alloca(cg, s->as.tr.err);
    LLVMBuildStore(cg->builder, err_val, err_var);
    bind(scopes, *depth, s->as.tr.err, err_var, s, false);
    gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
    scope_pop(scopes, depth);
  } else {
    gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
  }
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    LLVMBuildBr(cg->builder, end_b);
  LLVMPositionBuilderAtEnd(cg->builder, end_b);
}

static void gen_stmt_defer(codegen_t *cg, scope *scopes, size_t depth, stmt_t *s) {
  if (!s->as.de.body)
    return;

  ny_param_list no_params = {0};
  LLVMValueRef cls = gen_closure(cg, scopes, depth, no_params, s->as.de.body,
                                 false, "__defer");
  LLVMValueRef cls_raw = LLVMBuildIntToPtr(
      cg->builder, cls, LLVMPointerType(cg->type_i64, 0), "");
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
                   (LLVMValueRef[]){
                       LLVMBuildIntToPtr(
                           cg->builder, fn_ptr_int,
                           LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0),
                           ""),
                       env},
                   2, "");
  }

  vec_push(&scopes[depth].defers, s->as.de.body);
}

static void gen_stmt_layout(codegen_t *cg, stmt_t *s) {
  size_t off = 0;
  for (size_t i = 0; i < s->as.layout.fields.len; i++) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s.%s", s->as.layout.name,
             s->as.layout.fields.data[i].name);
    LLVMValueRef fv = LLVMAddFunction(
        cg->module, buf,
        LLVMFunctionType(cg->type_i64, (LLVMTypeRef[]){cg->type_i64}, 1, 0));
    LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(fv, "e"));
    LLVMBuildRet(
        cg->builder,
        LLVMBuildAdd(cg->builder, LLVMGetParam(fv, 0),
                     LLVMConstInt(cg->type_i64, (uint64_t)off, false), ""));
    off += (size_t)s->as.layout.fields.data[i].width;
  }
}

static void gen_stmt_module(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                            size_t func_root, bool is_tail) {
  const char *prev = cg->current_module_name;
  cg->current_module_name = s->as.module.name;
  for (size_t i = 0; i < s->as.module.body.len; ++i) {
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

void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
              size_t func_root, bool is_tail) {
  if (!s || cg->had_error)
    return;
  switch (s->kind) {
  case NY_S_VAR: {
    bool dest = s->as.var.is_destructure;
    bool parallel = (s->as.var.names.len == s->as.var.exprs.len) && !dest;

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
      LLVMValueRef slot;
      if (s->as.var.is_undef) {
        if (*depth == 0) {
          binding *gb = lookup_global(cg, n);
          if (!gb) {
            report_undef_symbol(cg, n, s->tok);
            return;
          }
          slot = gb->value;
        } else {
          binding *eb = scope_lookup(scopes, *depth, n);
          if (!eb)
            eb = lookup_global(cg, n);
          if (!eb) {
            report_undef_symbol(cg, n, s->tok);
            return;
          }
          slot = eb->value;
        }
        LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 0, false), slot);
        continue;
      }
      if (*depth == 0) {
        binding *gb = lookup_global(cg, n);
        if (gb)
          slot = gb->value;
        else {
          slot = LLVMAddGlobal(cg->module, cg->type_i64, n);
          LLVMSetInitializer(slot, LLVMConstInt(cg->type_i64, 0, false));
          binding b = {ny_strdup(n), slot, NULL,
                       s->as.var.is_mut ? true : false, false};
          vec_push(&cg->global_vars, b);
          if (s->tok.filename) {
            ny_diag_warning(
                s->tok,
                "implicit declaration of global variable \033[1;37m'%s'\033[0m",
                n);
            if (verbose_enabled >= 2)
              ny_diag_hint("add an explicit declaration to avoid implicit globals");
          }
        }
      } else {
        if (s->as.var.is_decl) {
          slot = build_alloca(cg, n);
          bind(scopes, *depth, n, slot, s, s->as.var.is_mut);
        } else {
          binding *eb = scope_lookup(scopes, *depth, n);
          if (eb) {
              if (!eb->is_mut) {
                report_reassign_immutable(cg, s->tok, n, false, eb);
              }
              slot = eb->value;
            } else {
            // Check global scope as well
            binding *gb = lookup_global(cg, n);
              if (gb) {
                if (!gb->is_mut) {
                  report_reassign_immutable(cg, s->tok, n, true, gb);
                }
                slot = gb->value;
            } else {
              slot = build_alloca(cg, n);
              bind(scopes, *depth, n, slot, s, s->as.var.is_mut);
            }
          }
        }
      }

      LLVMValueRef target_val = NULL;
      if (parallel) {
        target_val = gen_expr(cg, scopes, *depth, s->as.var.exprs.data[i]);
      } else if (dest) {
        // Tagged integer index: i -> (i << 1) | 1
        uint64_t tagged_idx = ((uint64_t)i << 1) | 1;
        LLVMValueRef idx_val = LLVMConstInt(cg->type_i64, tagged_idx, false);
        target_val =
            LLVMBuildCall2(cg->builder, gs->type, gs->value,
                           (LLVMValueRef[]){first_val, idx_val}, 2, "");
      } else {
        target_val = first_val;
      }

      if (ensure_store_ready(cg, s->tok, target_val, slot, "NY_S_VAR")) {
        LLVMBuildStore(cg->builder, target_val, slot);
      }
    }
    break;
  }
  case NY_S_EXPR: {
    expr_t *e = s->as.expr.expr;
    if (e->kind == NY_E_CALL && e->as.call.callee->kind == NY_E_IDENT) {
      fun_sig *sig = lookup_fun(cg, e->as.call.callee->as.ident.name);
      if (sig && sig->return_type &&
          (strcmp(sig->return_type, "Result") == 0 ||
           strcmp(sig->return_type, "std.core.error.Result") == 0)) {
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
    LLVMValueRef v = gen_expr(cg, scopes, *depth, e);
    if (!v) {
      ny_diag_error(s->tok, "failed to generate expression");
      cg->had_error = 1;
      return;
    }
    if (is_tail) {
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

      // We need to handle potential bindings in the arm
      // Create a sub-scope for the arm
      scope_enter(scopes, depth, scopes[*depth].break_bb,
                  scopes[*depth].continue_bb);

      for (size_t j = 0; j < arm->patterns.len; ++j) {
        expr_t *pat = arm->patterns.data[j];
        if (pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
            strcmp(pat->as.ident.name, "_") == 0) {
          has_wild = 1;
          break;
        }

        LLVMValueRef pat_cond = NULL;
        if (pat->kind == NY_E_CALL && pat->as.call.callee->kind == NY_E_IDENT) {
          const char *callee = pat->as.call.callee->as.ident.name;
          bool is_ok_pat = (strcmp(callee, "ok") == 0);
          bool is_err_pat = (strcmp(callee, "err") == 0);
          if (is_ok_pat || is_err_pat) {
            fun_sig *check_sig =
                lookup_fun(cg, is_ok_pat ? "__is_ok" : "__is_err");
            if (check_sig) {
              LLVMValueRef tag_check =
                  LLVMBuildCall2(cg->builder, check_sig->type, check_sig->value,
                                 (LLVMValueRef[]){testv}, 1, "");
              pat_cond = to_bool(cg, tag_check);

              // If match, bind the first argument
              if (pat->as.call.args.len > 0) {
                expr_t *arg0 = pat->as.call.args.data[0].val;
                if (arg0->kind == NY_E_IDENT) {
                  fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap");
                  if (unwrap_sig) {
                    LLVMValueRef val = LLVMBuildCall2(
                        cg->builder, unwrap_sig->type, unwrap_sig->value,
                        (LLVMValueRef[]){testv}, 1, "");
                    LLVMValueRef slot = build_alloca(cg, arg0->as.ident.name);
                    LLVMBuildStore(cg->builder, val, slot);
                    bind(scopes, *depth, arg0->as.ident.name, slot, s, false);
                  }
                }
              }
            }
          }
        }

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

      LLVMBuildCondBr(cg->builder, cond, arm_bb, next_bb);
      LLVMPositionBuilderAtEnd(cg->builder, arm_bb);
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
  case NY_S_LAYOUT: {
    gen_stmt_layout(cg, s);
    break;
  }
  case NY_S_MODULE: {
    gen_stmt_module(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_EXPORT:
    break;
  default:
    break;
  }
}
