#include "base/util.h"
#include "priv.h"
#include <alloca.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures) {
  if (!fn->as.fn.body)
    return;
  LLVMValueRef f = LLVMGetNamedFunction(cg->module, name);
  if (!f) {
    size_t n_params = fn->as.fn.params.len;
    // If captures pointer is non-null, this is a closure/lambda context, so we
    // MUST accept 'env' param.
    size_t total_args = captures ? n_params + 1 : n_params;
    LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * total_args);
    for (size_t i = 0; i < total_args; i++)
      pt[i] = cg->type_i64;
    LLVMTypeRef ft =
        LLVMFunctionType(cg->type_i64, pt, (unsigned)total_args, 0);
    f = LLVMAddFunction(cg->module, name, ft);
    // Store explicit params count for callers
    fun_sig sig = {.name = ny_strdup(name),
                   .type = ft,
                   .value = f,
                   .stmt_t = fn,
                   .arity = (int)n_params,
                   .is_variadic = fn->as.fn.is_variadic,
                   .is_extern = false,
                   .link_name = NULL};
    vec_push(&cg->fun_sigs, sig);
  } else {
    // Overwrite: remove existing basic blocks if any
    LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f);
    while (bb) {
      LLVMBasicBlockRef next = LLVMGetNextBasicBlock(bb);
      LLVMDeleteBasicBlock(bb);
      bb = next;
    }
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(f, "entry"));
  size_t fd = depth + 1;
  size_t root = fd;
  // Init scope
  scopes[fd].vars.len = scopes[fd].vars.cap = 0;
  scopes[fd].vars.data = NULL;
  scopes[fd].defers.len = scopes[fd].defers.cap = 0;
  scopes[fd].defers.data = NULL;
  scopes[fd].break_bb = NULL;
  scopes[fd].continue_bb = NULL;
  size_t param_offset = 0;
  if (captures) {
    param_offset = 1;
    LLVMValueRef env_arg = LLVMGetParam(f, 0);
    LLVMValueRef env_raw = LLVMBuildIntToPtr(
        cg->builder, env_arg, LLVMPointerType(cg->type_i64, 0), "env_raw");
    for (size_t i = 0; i < captures->len; i++) {
      LLVMValueRef src = LLVMBuildGEP2(
          cg->builder, cg->type_i64, env_raw,
          (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1,
          "");
      LLVMValueRef val = LLVMBuildLoad2(cg->builder, cg->type_i64, src, "");
      // For closures, we copy captures into local variables of the new scope
      // Note: Bind to the captured name
      LLVMValueRef lv = build_alloca(cg, captures->data[i].name);
      LLVMBuildStore(cg->builder, val, lv);
      bind(scopes, fd, captures->data[i].name, lv, NULL, true);
    }
  }
  for (size_t i = 0; i < fn->as.fn.params.len; i++) {
    LLVMValueRef a = build_alloca(cg, fn->as.fn.params.data[i].name);
    LLVMBuildStore(cg->builder, LLVMGetParam(f, (unsigned)(i + param_offset)),
                   a);
    bind(scopes, fd, fn->as.fn.params.data[i].name, a, NULL, true);
  }
  size_t old_root = cg->func_root_idx;
  cg->func_root_idx = root;

  // Infer current module name from function name
  const char *prev_mod = cg->current_module_name;
  char *temp_mod = NULL;
  const char *last_dot = strrchr(name, '.');
  if (last_dot) {
    size_t len = last_dot - name;
    temp_mod = malloc(len + 1);
    memcpy(temp_mod, name, len);
    temp_mod[len] = '\0';
    cg->current_module_name = temp_mod;
  }

  gen_stmt(cg, scopes, &fd, fn->as.fn.body, root, true);

  cg->current_module_name = prev_mod;
  if (temp_mod)
    free(temp_mod);

  cg->func_root_idx = old_root;
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
  vec_free(&scopes[root].defers);
  vec_free(&scopes[root].vars);
  if (cur)
    LLVMPositionBuilderAtEnd(cg->builder, cur);
}

void collect_sigs(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_FUNC) {
    LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * s->as.fn.params.len);
    for (size_t j = 0; j < s->as.fn.params.len; j++)
      pt[j] = cg->type_i64;
    LLVMTypeRef ft =
        LLVMFunctionType(cg->type_i64, pt, (unsigned)s->as.fn.params.len, 0);
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, s->as.fn.name);
    if (!f)
      f = LLVMAddFunction(cg->module, s->as.fn.name, ft);
    LLVMSetAlignment(f, 16);
    fun_sig sig = {.name = ny_strdup(s->as.fn.name),
                   .type = ft,
                   .value = f,
                   .stmt_t = s,
                   .arity = (int)s->as.fn.params.len,
                   .is_variadic = s->as.fn.is_variadic,
                   .is_extern = false,
                   .link_name = NULL};
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_EXTERN) {
    size_t param_count = s->as.ext.params.len;
    LLVMTypeRef *pt = NULL;
    if (param_count > 0)
      pt = alloca(sizeof(LLVMTypeRef) * param_count);
    for (size_t j = 0; j < param_count; j++)
      pt[j] = cg->type_i64;
    LLVMTypeRef ft =
        LLVMFunctionType(cg->type_i64, pt, (unsigned)param_count, 0);
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, s->as.ext.name);
    if (!f)
      f = LLVMAddFunction(cg->module, s->as.ext.name, ft);
    fun_sig sig = {.name = ny_strdup(s->as.ext.name),
                   .type = ft,
                   .value = f,
                   .stmt_t = s,
                   .arity = (int)param_count,
                   .is_variadic = s->as.ext.is_variadic,
                   .is_extern = true,
                   .link_name = s->as.ext.link_name
                                    ? ny_strdup(s->as.ext.link_name)
                                    : NULL};
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_VAR) {
    for (size_t j = 0; j < s->as.var.names.len; j++) {
      const char *n = s->as.var.names.data[j];
      // Use simple exact lookup here to see if we already created this global
      bool found = false;
      for (size_t k = 0; k < cg->global_vars.len; k++) {
        if (strcmp(cg->global_vars.data[k].name, n) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        LLVMValueRef g = LLVMAddGlobal(cg->module, cg->type_i64, n);
        LLVMSetInitializer(g, LLVMConstInt(cg->type_i64, 0, false));
        binding b = {ny_strdup(n), g, NULL, s->as.var.is_mut};
        vec_push(&cg->global_vars, b);
      }
    }
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_sigs(cg, s->as.module.body.data[i]);
  }
}
