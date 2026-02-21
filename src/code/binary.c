#include "base/util.h"
#include "priv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ny_const_tagged_int(LLVMValueRef v, int64_t *out_raw) {
  if (!v || !LLVMIsAConstantInt(v))
    return false;
  int64_t tagged = LLVMConstIntGetSExtValue(v);
  if ((tagged & 1) == 0)
    return false;
  if (out_raw)
    *out_raw = tagged >> 1;
  return true;
}

static LLVMValueRef ny_const_tagged_int_value(codegen_t *cg, int64_t raw) {
  uint64_t tagged = (((uint64_t)raw) << 1) | 1u;
  return LLVMConstInt(cg->type_i64, tagged, false);
}

static LLVMValueRef ny_const_tagged_bool_value(codegen_t *cg, bool v) {
  return LLVMConstInt(cg->type_i64, v ? 2u : 4u, false);
}

static LLVMValueRef ny_tag_bool(codegen_t *cg, LLVMValueRef cond) {
  return LLVMBuildSelect(cg->builder, cond,
                         LLVMConstInt(cg->type_i64, 2, false),
                         LLVMConstInt(cg->type_i64, 4, false), "tag_bool");
}

static LLVMValueRef ny_get_overflow_intrinsic_i64(codegen_t *cg,
                                                  const char *name,
                                                  LLVMTypeRef *out_ft) {
  LLVMTypeRef ret_parts[2] = {cg->type_i64, cg->type_i1};
  LLVMTypeRef ret_ty = LLVMStructTypeInContext(cg->ctx, ret_parts, 2, false);
  LLVMTypeRef args[2] = {cg->type_i64, cg->type_i64};
  LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, args, 2, false);
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
  if (!fn)
    fn = LLVMAddFunction(cg->module, name, fn_ty);
  if (out_ft)
    *out_ft = fn_ty;
  return fn;
}

static bool ny_env_enabled_default_on_local(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return true;
  return ny_env_is_truthy(v);
}

static bool ny_should_prefer_builtin_ops(const codegen_t *cg) {
  if (!cg || !cg->current_module_name || !*cg->current_module_name)
    return false;
  const char *mod = cg->current_module_name;
  bool is_std_mod =
      (strncmp(mod, "std.", 4) == 0 || strncmp(mod, "lib.", 4) == 0);
  if (!is_std_mod)
    return false;
  if (strncmp(mod, "std.core.reflect", 16) == 0)
    return false;
  return ny_env_enabled_default_on_local("NYTRIX_STD_BUILTIN_OPS");
}

static fun_sig *ny_helper_eq(codegen_t *cg) {
  fun_sig *s = lookup_fun(cg, "std.core.reflect.eq");
  if (!s)
    s = lookup_fun(cg, "eq");
  if (!s)
    s = lookup_fun(cg, "__eq");
  if (s)
    cg->cached_fn_eq = s;
  return s;
}

static fun_sig *ny_helper_contains(codegen_t *cg) {
  fun_sig *s = lookup_fun(cg, "contains");
  if (s)
    cg->cached_fn_contains = s;
  return s;
}

static LLVMValueRef ny_try_emit_tagged_int_fast_binary(codegen_t *cg,
                                                       const char *builtin_name,
                                                       LLVMValueRef l,
                                                       LLVMValueRef r,
                                                       fun_sig *fallback) {
  if (!builtin_name || !fallback)
    return NULL;
  if (!ny_env_enabled("NYTRIX_FAST_INT_BINOPS"))
    return NULL;

  bool supports =
      strcmp(builtin_name, "__add") == 0 ||
      strcmp(builtin_name, "__sub") == 0 ||
      strcmp(builtin_name, "__mul") == 0 ||
      strcmp(builtin_name, "__div") == 0 ||
      strcmp(builtin_name, "__mod") == 0 || strcmp(builtin_name, "__eq") == 0 ||
      strcmp(builtin_name, "__lt") == 0 || strcmp(builtin_name, "__le") == 0 ||
      strcmp(builtin_name, "__gt") == 0 || strcmp(builtin_name, "__ge") == 0 ||
      strcmp(builtin_name, "__and") == 0 || strcmp(builtin_name, "__or") == 0 ||
      strcmp(builtin_name, "__xor") == 0 ||
      strcmp(builtin_name, "__shl") == 0 || strcmp(builtin_name, "__shr") == 0;
  if (!supports)
    return NULL;

  LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);

  LLVMBasicBlockRef fast_bb = LLVMAppendBasicBlock(fn, "bin.int.fast");
  LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlock(fn, "bin.runtime.slow");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(fn, "bin.merge");

  LLVMValueRef both_int = LLVMBuildAnd(cg->builder, ny_is_tagged_int(cg, l),
                                       ny_is_tagged_int(cg, r), "both_int");
  LLVMBuildCondBr(cg->builder, both_int, fast_bb, slow_bb);

  LLVMPositionBuilderAtEnd(cg->builder, fast_bb);
  LLVMValueRef li = ny_untag_int(cg, l);
  LLVMValueRef ri = ny_untag_int(cg, r);
  LLVMValueRef fast_value = NULL;
  LLVMBasicBlockRef fast_done_bb = NULL;

  if (strcmp(builtin_name, "__sub") == 0 && l == r) {
    fast_value = ny_const_tagged_int_value(cg, 0);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if (strcmp(builtin_name, "__xor") == 0 && l == r) {
    fast_value = ny_const_tagged_int_value(cg, 0);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if ((strcmp(builtin_name, "__and") == 0 ||
              strcmp(builtin_name, "__or") == 0) &&
             l == r) {
    fast_value = l;
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if ((strcmp(builtin_name, "__eq") == 0 ||
              strcmp(builtin_name, "__le") == 0 ||
              strcmp(builtin_name, "__ge") == 0) &&
             l == r) {
    fast_value = ny_const_tagged_bool_value(cg, true);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if ((strcmp(builtin_name, "__lt") == 0 ||
              strcmp(builtin_name, "__gt") == 0) &&
             l == r) {
    fast_value = ny_const_tagged_bool_value(cg, false);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if (strcmp(builtin_name, "__add") == 0 ||
             strcmp(builtin_name, "__sub") == 0 ||
             strcmp(builtin_name, "__mul") == 0) {
    const char *intr_name =
        strcmp(builtin_name, "__add") == 0   ? "llvm.sadd.with.overflow.i64"
        : strcmp(builtin_name, "__sub") == 0 ? "llvm.ssub.with.overflow.i64"
                                             : "llvm.smul.with.overflow.i64";
    LLVMTypeRef intr_ty = NULL;
    LLVMValueRef intr = ny_get_overflow_intrinsic_i64(cg, intr_name, &intr_ty);
    LLVMValueRef packed =
        LLVMBuildCall2(cg->builder, intr_ty, intr, (LLVMValueRef[]){li, ri}, 2,
                       "arith_packed");
    LLVMValueRef raw = LLVMBuildExtractValue(cg->builder, packed, 0, "arith");
    LLVMValueRef ov = LLVMBuildExtractValue(cg->builder, packed, 1, "arith_ov");
    LLVMBasicBlockRef fast_ok_bb = LLVMAppendBasicBlock(fn, "bin.int.fast.ok");
    LLVMBuildCondBr(cg->builder, ov, slow_bb, fast_ok_bb);
    LLVMPositionBuilderAtEnd(cg->builder, fast_ok_bb);
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if (strcmp(builtin_name, "__div") == 0 ||
             strcmp(builtin_name, "__mod") == 0) {
    LLVMValueRef is_zero =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, ri,
                      LLVMConstInt(cg->type_i64, 0, false), "divmod_zero");
    LLVMValueRef one = LLVMConstInt(cg->type_i64, 1, false);
    LLVMValueRef safe_divisor =
        LLVMBuildSelect(cg->builder, is_zero, one, ri, "safe_divisor");
    LLVMValueRef raw =
        (strcmp(builtin_name, "__div") == 0)
            ? LLVMBuildUDiv(cg->builder, li, safe_divisor, "udiv_fast")
            : LLVMBuildURem(cg->builder, li, safe_divisor, "urem_fast");
    LLVMValueRef nonzero_res = ny_tag_int(cg, raw);
    LLVMValueRef zero_res = LLVMConstInt(
        cg->type_i64, strcmp(builtin_name, "__div") == 0 ? 0 : 1, false);
    fast_value =
        LLVMBuildSelect(cg->builder, is_zero, zero_res, nonzero_res, "divmod");
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if (strcmp(builtin_name, "__and") == 0 ||
             strcmp(builtin_name, "__or") == 0 ||
             strcmp(builtin_name, "__xor") == 0) {
    LLVMValueRef raw = NULL;
    if (strcmp(builtin_name, "__and") == 0)
      raw = LLVMBuildAnd(cg->builder, li, ri, "and_fast");
    else if (strcmp(builtin_name, "__or") == 0)
      raw = LLVMBuildOr(cg->builder, li, ri, "or_fast");
    else
      raw = LLVMBuildXor(cg->builder, li, ri, "xor_fast");
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else if (strcmp(builtin_name, "__shl") == 0 ||
             strcmp(builtin_name, "__shr") == 0) {
    LLVMValueRef zero = LLVMConstInt(cg->type_i64, 0, false);
    LLVMValueRef sixty_four = LLVMConstInt(cg->type_i64, 64, false);
    LLVMValueRef ge_zero =
        LLVMBuildICmp(cg->builder, LLVMIntSGE, ri, zero, "sh_nonneg");
    LLVMValueRef lt_sixty_four =
        LLVMBuildICmp(cg->builder, LLVMIntSLT, ri, sixty_four, "sh_lt64");
    LLVMValueRef in_range =
        LLVMBuildAnd(cg->builder, ge_zero, lt_sixty_four, "sh_range");
    LLVMBasicBlockRef fast_shift_bb =
        LLVMAppendBasicBlock(fn, "bin.int.fast.shift");
    LLVMBuildCondBr(cg->builder, in_range, fast_shift_bb, slow_bb);
    LLVMPositionBuilderAtEnd(cg->builder, fast_shift_bb);
    LLVMValueRef raw = (strcmp(builtin_name, "__shl") == 0)
                           ? LLVMBuildShl(cg->builder, li, ri, "shl_fast")
                           : LLVMBuildLShr(cg->builder, li, ri, "shr_fast");
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  } else {
    LLVMIntPredicate pred = LLVMIntEQ;
    if (strcmp(builtin_name, "__lt") == 0)
      pred = LLVMIntSLT;
    else if (strcmp(builtin_name, "__le") == 0)
      pred = LLVMIntSLE;
    else if (strcmp(builtin_name, "__gt") == 0)
      pred = LLVMIntSGT;
    else if (strcmp(builtin_name, "__ge") == 0)
      pred = LLVMIntSGE;
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, li, ri, "icmp_fast");
    fast_value = ny_tag_bool(cg, cmp);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
  }

  LLVMPositionBuilderAtEnd(cg->builder, slow_bb);
  LLVMValueRef slow_value =
      LLVMBuildCall2(cg->builder, fallback->type, fallback->value,
                     (LLVMValueRef[]){l, r}, 2, "bin_runtime");
  LLVMBasicBlockRef slow_done_bb = LLVMGetInsertBlock(cg->builder);
  LLVMBuildBr(cg->builder, merge_bb);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "bin_result");
  LLVMValueRef incoming_vals[2] = {fast_value, slow_value};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

LLVMValueRef gen_binary(codegen_t *cg, const char *op, LLVMValueRef l,
                        LLVMValueRef r) {
  if (!l || !r)
    return LLVMConstInt(cg->type_i64, 0, false);

  bool prefer_builtin_ops = ny_should_prefer_builtin_ops(cg);
  const char *generic_name = NULL;
  const char *builtin_name = NULL;

  if (strcmp(op, "+") == 0) {
    generic_name = "add";
    builtin_name = "__add";
  } else if (strcmp(op, "-") == 0) {
    generic_name = "sub";
    builtin_name = "__sub";
  } else if (strcmp(op, "*") == 0) {
    generic_name = "mul";
    builtin_name = "__mul";
  } else if (strcmp(op, "/") == 0) {
    generic_name = "div";
    builtin_name = "__div";
  } else if (strcmp(op, "%") == 0) {
    generic_name = "mod";
    builtin_name = "__mod";
  } else if (strcmp(op, "|") == 0) {
    generic_name = "bor";
    builtin_name = "__or";
  } else if (strcmp(op, "&") == 0) {
    generic_name = "band";
    builtin_name = "__and";
  } else if (strcmp(op, "^") == 0) {
    generic_name = "bxor";
    builtin_name = "__xor";
  } else if (strcmp(op, "<") == 0) {
    generic_name = "lt";
    builtin_name = "__lt";
  } else if (strcmp(op, "<=") == 0) {
    generic_name = "le";
    builtin_name = "__le";
  } else if (strcmp(op, ">") == 0) {
    generic_name = "gt";
    builtin_name = "__gt";
  } else if (strcmp(op, ">=") == 0) {
    generic_name = "ge";
    builtin_name = "__ge";
  } else if (strcmp(op, "<<") == 0) {
    generic_name = "bshl";
    builtin_name = "__shl";
  } else if (strcmp(op, ">>") == 0) {
    generic_name = "bshr";
    builtin_name = "__shr";
  }

  if (builtin_name) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri)) {
      if (strcmp(builtin_name, "__add") == 0)
        return ny_const_tagged_int_value(cg, li + ri);
      if (strcmp(builtin_name, "__sub") == 0)
        return ny_const_tagged_int_value(cg, li - ri);
      if (strcmp(builtin_name, "__mul") == 0)
        return ny_const_tagged_int_value(cg, li * ri);
      if (strcmp(builtin_name, "__div") == 0) {
        if (ri == 0)
          return LLVMConstInt(cg->type_i64, 0, false);
        if (li == INT64_MIN && ri == -1)
          goto skip_const_div_fold;
        if (ri == 1)
          return ny_const_tagged_int_value(cg, li);
        return ny_const_tagged_int_value(cg, li / ri);
      }
    skip_const_div_fold:
      if (strcmp(builtin_name, "__mod") == 0) {
        if (ri == 0)
          return LLVMConstInt(cg->type_i64, 1, false);
        if (ri == 1 || ri == -1)
          return ny_const_tagged_int_value(cg, 0);
        return ny_const_tagged_int_value(cg, li % ri);
      }
      if (strcmp(builtin_name, "__and") == 0)
        return ny_const_tagged_int_value(cg, li & ri);
      if (strcmp(builtin_name, "__or") == 0)
        return ny_const_tagged_int_value(cg, li | ri);
      if (strcmp(builtin_name, "__xor") == 0)
        return ny_const_tagged_int_value(cg, li ^ ri);
      if (strcmp(builtin_name, "__lt") == 0)
        return ny_const_tagged_bool_value(cg, li < ri);
      if (strcmp(builtin_name, "__le") == 0)
        return ny_const_tagged_bool_value(cg, li <= ri);
      if (strcmp(builtin_name, "__gt") == 0)
        return ny_const_tagged_bool_value(cg, li > ri);
      if (strcmp(builtin_name, "__ge") == 0)
        return ny_const_tagged_bool_value(cg, li >= ri);
      if (strcmp(builtin_name, "__shl") == 0 && ri >= 0 && ri < 64)
        return ny_const_tagged_int_value(cg, (int64_t)(((uint64_t)li) << ri));
      if (strcmp(builtin_name, "__shr") == 0 && ri >= 0 && ri < 64)
        return ny_const_tagged_int_value(cg, (int64_t)(((uint64_t)li) >> ri));
    }
  }

  if (strcmp(op, "==") == 0) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri))
      return ny_const_tagged_bool_value(cg, li == ri);
    fun_sig *s = prefer_builtin_ops ? lookup_fun(cg, "__eq") : ny_helper_eq(cg);
    if (!s)
      return expr_fail(cg, (token_t){0}, "'==' requires 'eq' (or __eq)");
    LLVMValueRef fast = ny_try_emit_tagged_int_fast_binary(cg, "__eq", l, r, s);
    if (fast)
      return fast;
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }

  if (generic_name && !prefer_builtin_ops) {
    char full_generic[128];
    snprintf(full_generic, sizeof(full_generic), "std.core.reflect.%s",
             generic_name);
    fun_sig *s = lookup_fun(cg, full_generic);
    if (!s)
      s = lookup_fun(cg, generic_name);

    if (s && strcmp(s->name, builtin_name) != 0) {
      if (s->stmt_t && !ny_is_stdlib_tok(s->stmt_t->tok))
        s = NULL;
    }
    if (s && strcmp(s->name, builtin_name) != 0) {
      if (builtin_name) {
        LLVMValueRef fast =
            ny_try_emit_tagged_int_fast_binary(cg, builtin_name, l, r, s);
        if (fast)
          return fast;
      }
      return LLVMBuildCall2(cg->builder, s->type, s->value,
                            (LLVMValueRef[]){l, r}, 2, "");
    }
  }

  if (builtin_name) {
    fun_sig *s = lookup_fun(cg, builtin_name);
    if (!s)
      return expr_fail(cg, (token_t){0}, "builtin %s missing", builtin_name);
    LLVMValueRef fast =
        ny_try_emit_tagged_int_fast_binary(cg, builtin_name, l, r, s);
    if (fast)
      return fast;
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }

  if (strcmp(op, "!=") == 0) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri))
      return ny_const_tagged_bool_value(cg, li != ri);
    return LLVMBuildSub(cg->builder, LLVMConstInt(cg->type_i64, 6, false),
                        gen_binary(cg, "==", l, r), "");
  }

  if (strcmp(op, "in") == 0) {
    fun_sig *s = ny_helper_contains(cg);
    if (!s)
      return expr_fail(cg, (token_t){0}, "'in' requires 'contains'");
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){r, l}, 2, "");
  }
  return expr_fail(cg, (token_t){0}, "undefined operator '%s'", op);
}
