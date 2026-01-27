#include "base/util.h"
#include "priv.h"
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        fprintf(stderr, "Error: destructuring requires 'get' function\n");
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
          binding b = {ny_strdup(n), slot, NULL, s->as.var.is_mut ? true : false};
          vec_push(&cg->global_vars, b);
        }
      } else {
        if (s->as.var.is_decl) {
          slot = build_alloca(cg, n);
          bind(scopes, *depth, n, slot, NULL, s->as.var.is_mut);
        } else {
          binding *eb = scope_lookup(scopes, *depth, n);
          if (eb) {
            slot = eb->value;
          } else {
            // Check global scope as well
            binding *gb = lookup_global(cg, n);
            if (gb) {
              slot = gb->value;
            } else {
              slot = build_alloca(cg, n);
              bind(scopes, *depth, n, slot, NULL, s->as.var.is_mut);
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

      if (!cg->builder) {
        fprintf(stderr, "ERROR: NULL builder in NY_S_VAR\n");
        exit(1);
      }
      if (!target_val) {
        fprintf(stderr, "ERROR: NULL target_val in NY_S_VAR\n");
        exit(1);
      }
      if (!slot) {
        fprintf(stderr, "ERROR: NULL slot in NY_S_VAR\n");
        exit(1);
      }
      LLVMBasicBlockRef cur_block = LLVMGetInsertBlock(cg->builder);
      if (!cur_block) {
        fprintf(stderr, "ERROR: NULL block in NY_S_VAR\n");
        exit(1);
      }
      if (!LLVMGetBasicBlockTerminator(cur_block)) {
        LLVMBuildStore(cg->builder, target_val, slot);
      }
    }
    break;
  }
  case NY_S_EXPR: {
    LLVMValueRef v = gen_expr(cg, scopes, *depth, s->as.expr.expr);
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
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        expr_t *pat = arm->patterns.data[j];
        if (pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
            strcmp(pat->as.ident.name, "_") == 0) {
          has_wild = 1;
          break;
        }
        LLVMValueRef pv = gen_expr(cg, scopes, *depth, pat);
        LLVMValueRef eq = gen_binary(cg, "==", testv, pv);
        LLVMValueRef c = to_bool(cg, eq);
        cond = cond ? LLVMBuildOr(cg->builder, cond, c, "") : c;
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
    LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
    LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "wc"),
                      bb = LLVMAppendBasicBlock(f, "wb"),
                      eb = LLVMAppendBasicBlock(f, "we");
    LLVMBuildBr(cg->builder, cb);
    LLVMPositionBuilderAtEnd(cg->builder, cb);
    LLVMBuildCondBr(cg->builder,
                    to_bool(cg, gen_expr(cg, scopes, *depth, s->as.whl.test)),
                    bb, eb);
    LLVMPositionBuilderAtEnd(cg->builder, bb);
    (*depth)++;
    scopes[*depth].vars.len = scopes[*depth].vars.cap = 0;
    scopes[*depth].vars.data = NULL;
    scopes[*depth].defers.len = scopes[*depth].defers.cap = 0;
    scopes[*depth].defers.data = NULL;
    scopes[*depth].break_bb = eb;
    scopes[*depth].continue_bb = cb;
    gen_stmt(cg, scopes, depth, s->as.whl.body, func_root, false);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      emit_defers(cg, scopes, *depth, func_root);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cb);
    }
    vec_free(&scopes[*depth].defers);
    vec_free(&scopes[*depth].vars);
    (*depth)--;
    LLVMPositionBuilderAtEnd(cg->builder, eb);
    break;
  }
  case NY_S_FOR: {
    LLVMValueRef itv = gen_expr(cg, scopes, *depth, s->as.fr.iterable);
    LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
    LLVMBasicBlockRef cb = LLVMAppendBasicBlock(f, "fc"),
                      bb = LLVMAppendBasicBlock(f, "fb"),
                      eb = LLVMAppendBasicBlock(f, "fe");
    LLVMValueRef idx_p = build_alloca(cg, "idx");
    LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), idx_p);
    fun_sig *ls = lookup_fun(cg, "list_len"), *gs = lookup_fun(cg, "get");
    if (!ls || !gs) {
      fprintf(stderr, "Error: for requires list_len/get\n");
      cg->had_error = 1;
      return;
    }
    LLVMBuildBr(cg->builder, cb);
    LLVMPositionBuilderAtEnd(cg->builder, cb);
    LLVMValueRef i_val = LLVMBuildLoad2(cg->builder, cg->type_i64, idx_p, "");
    LLVMValueRef n_val = LLVMBuildCall2(cg->builder, ls->type, ls->value,
                                        (LLVMValueRef[]){itv}, 1, "");
    LLVMBuildCondBr(cg->builder,
                    LLVMBuildICmp(cg->builder, LLVMIntSLT, i_val, n_val, ""),
                    bb, eb);
    LLVMPositionBuilderAtEnd(cg->builder, bb);
    LLVMValueRef item = LLVMBuildCall2(cg->builder, gs->type, gs->value,
                                       (LLVMValueRef[]){itv, i_val}, 2, "");
    LLVMValueRef iv = build_alloca(cg, s->as.fr.iter_var);
    LLVMBuildStore(cg->builder, item, iv);
    LLVMBuildStore(cg->builder,
                   LLVMBuildAdd(cg->builder, i_val,
                                LLVMConstInt(cg->type_i64, 2, false), ""),
                   idx_p);
    (*depth)++;
    scopes[*depth].vars.len = scopes[*depth].vars.cap = 0;
    scopes[*depth].vars.data = NULL;
    scopes[*depth].defers.len = scopes[*depth].defers.cap = 0;
    scopes[*depth].defers.data = NULL;
    scopes[*depth].break_bb = eb;
    scopes[*depth].continue_bb = cb;
      bind(scopes, *depth, s->as.fr.iter_var, iv, NULL, false);
    gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
      emit_defers(cg, scopes, *depth, func_root);
      if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cb);
    }
    vec_free(&scopes[*depth].defers);
    vec_free(&scopes[*depth].vars);
    (*depth)--;
    LLVMPositionBuilderAtEnd(cg->builder, eb);
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
    (*depth)++;
    scopes[*depth].vars.len = scopes[*depth].vars.cap = 0;
    scopes[*depth].vars.data = NULL;
    scopes[*depth].defers.len = scopes[*depth].defers.cap = 0;
    scopes[*depth].defers.data = NULL;
    scopes[*depth].break_bb = scopes[*depth - 1].break_bb;
    scopes[*depth].continue_bb = scopes[*depth - 1].continue_bb;
    for (size_t i = 0; i < s->as.block.body.len; i++)
      gen_stmt(cg, scopes, depth, s->as.block.body.data[i], func_root,
               is_tail && (i == s->as.block.body.len - 1));
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      emit_defers(cg, scopes, *depth, *depth);
    vec_free(&scopes[*depth].defers);
    vec_free(&scopes[*depth].vars);
    (*depth)--;
    break;
  }
  case NY_S_TRY: {
    fun_sig *sz_fn = lookup_fun(cg, "__jmpbuf_size");
    fun_sig *set_env = lookup_fun(cg, "__set_panic_env");
    fun_sig *clr_env = lookup_fun(cg, "__clear_panic_env");
    fun_sig *get_err = lookup_fun(cg, "__get_panic_val");
    if (!sz_fn || !set_env || !clr_env || !get_err) {
      fprintf(stderr, "Error: missing rt try functions\n");
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
    LLVMValueRef func =
        LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
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
      (*depth)++;
      scopes[*depth].vars.len = scopes[*depth].vars.cap = 0;
      scopes[*depth].vars.data = NULL;
      scopes[*depth].defers.len = scopes[*depth].defers.cap = 0;
      scopes[*depth].defers.data = NULL;
      scopes[*depth].break_bb = scopes[*depth - 1].break_bb;
      scopes[*depth].continue_bb = scopes[*depth - 1].continue_bb;
      LLVMValueRef err_var = build_alloca(cg, s->as.tr.err);
      LLVMBuildStore(cg->builder, err_val, err_var);
      bind(scopes, *depth, s->as.tr.err, err_var, NULL, false);
      gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
      vec_free(&scopes[*depth].defers);
      vec_free(&scopes[*depth].vars);
      (*depth)--;
    } else {
      gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
    }
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
      LLVMBuildBr(cg->builder, end_b);
    LLVMPositionBuilderAtEnd(cg->builder, end_b);
    break;
  }
  case NY_S_FUNC:
    gen_func(cg, s, s->as.fn.name, scopes, *depth, NULL);
    break;
  case NY_S_DEFER: {
    if (!s->as.de.body)
      break;

    // 1. Create a closure from the body
    ny_param_list no_params = {0};
    LLVMValueRef cls = gen_closure(cg, scopes, *depth, no_params, s->as.de.body,
                                   false, "__defer");

    // 2. Extract fn_ptr and env from closure
    // Closure is [Tag | Code | Env]
    LLVMValueRef cls_raw = LLVMBuildIntToPtr(
        cg->builder, cls, LLVMPointerType(cg->type_i64, 0), "");
    // Load Code at index 0
    LLVMValueRef fn_ptr_int =
        LLVMBuildLoad2(cg->builder, cg->type_i64, cls_raw, "defer_fn");
    // Load Env at index 1
    LLVMValueRef env_addr = LLVMBuildGEP2(
        cg->builder, cg->type_i64, cls_raw,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false)}, 1, "");
    LLVMValueRef env =
        LLVMBuildLoad2(cg->builder, cg->type_i64, env_addr, "defer_env");

    // 3. Push to runtime defer stack
    fun_sig *push_sig = lookup_fun(cg, "__push_defer");
    if (push_sig) {
      LLVMBuildCall2(
          cg->builder, push_sig->type, push_sig->value,
          (LLVMValueRef[]){
              LLVMBuildIntToPtr(
                  cg->builder, fn_ptr_int,
                  LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), ""),
              env},
          2, "");
    }

    // 4. Record that we pushed one to this scope for later popping
    vec_push(&scopes[*depth].defers, s->as.de.body);
    break;
  }
  case NY_S_BREAK: {
    LLVMBasicBlockRef db = NULL;
    for (ssize_t i = (ssize_t)*depth; i >= 0; i--) {
      size_t td = (size_t)i;
      for (ssize_t d = (ssize_t)scopes[i].defers.len - 1; d >= 0; --d)
        gen_stmt(cg, scopes, &td, scopes[i].defers.data[d], func_root, false);
      if (scopes[i].break_bb) {
        db = scopes[i].break_bb;
        break;
      }
    }
    if (db)
      LLVMBuildBr(cg->builder, db);
    break;
  }
  case NY_S_CONTINUE: {
    LLVMBasicBlockRef db = NULL;
    for (ssize_t i = (ssize_t)*depth; i >= 0; i--) {
      size_t td = (size_t)i;
      for (ssize_t d = (ssize_t)scopes[i].defers.len - 1; d >= 0; --d)
        gen_stmt(cg, scopes, &td, scopes[i].defers.data[d], func_root, false);
      if (scopes[i].continue_bb) {
        db = scopes[i].continue_bb;
        break;
      }
    }
    if (db)
      LLVMBuildBr(cg->builder, db);
    break;
  }
  case NY_S_LAYOUT: {
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
    break;
  }
  case NY_S_MODULE: {
    const char *prev = cg->current_module_name;
    cg->current_module_name = s->as.module.name;
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      gen_stmt(cg, scopes, depth, s->as.module.body.data[i], func_root,
               is_tail);
    }
    cg->current_module_name = prev;
    break;
  }
  case NY_S_EXPORT:
    break;
  default:
    break;
  }
}
