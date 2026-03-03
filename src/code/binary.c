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
  return ny_env_enabled_default_on("NYTRIX_STD_BUILTIN_OPS");
}

static fun_sig *ny_helper_eq(codegen_t *cg) {
  fun_sig *s = lookup_fun(cg, "std.core.reflect.eq", 0);
  if (!s)
    s = lookup_fun(cg, "eq", 0);
  if (!s)
    s = lookup_fun(cg, "__eq", 0);
  if (s)
    cg->cached_fn_eq = s;
  return s;
}

static fun_sig *ny_helper_contains(codegen_t *cg) {
  fun_sig *s = lookup_fun(cg, "contains", 0);
  if (s)
    cg->cached_fn_contains = s;
  return s;
}

typedef enum {
  NY_BINOP_ADD,
  NY_BINOP_SUB,
  NY_BINOP_MUL,
  NY_BINOP_DIV,
  NY_BINOP_MOD,
  NY_BINOP_AND,
  NY_BINOP_OR,
  NY_BINOP_XOR,
  NY_BINOP_SHL,
  NY_BINOP_SHR,
  NY_BINOP_EQ,
  NY_BINOP_NE,
  NY_BINOP_LT,
  NY_BINOP_LE,
  NY_BINOP_GT,
  NY_BINOP_GE,
  NY_BINOP_IN,
  NY_BINOP_UNKNOWN
} ny_binop_kind_t;

typedef struct {
  const char *op;
  const char *generic;
  const char *builtin;
  ny_binop_kind_t kind;
  bool fast_int_supported;
  const char *overflow_intr;
} op_map_t;

static const op_map_t op_map[] = {
    {"+", "add", "__add", NY_BINOP_ADD, true, "llvm.sadd.with.overflow.i64"},
    {"-", "sub", "__sub", NY_BINOP_SUB, true, "llvm.ssub.with.overflow.i64"},
    {"*", "mul", "__mul", NY_BINOP_MUL, true, "llvm.smul.with.overflow.i64"},
    {"/", "div", "__div", NY_BINOP_DIV, true, NULL},
    {"%", "mod", "__mod", NY_BINOP_MOD, true, NULL},
    {"|", "bor", "__or", NY_BINOP_OR, true, NULL},
    {"&", "band", "__and", NY_BINOP_AND, true, NULL},
    {"^", "bxor", "__xor", NY_BINOP_XOR, true, NULL},
    {"<", "lt", "__lt", NY_BINOP_LT, true, NULL},
    {"<=", "le", "__le", NY_BINOP_LE, true, NULL},
    {">", "gt", "__gt", NY_BINOP_GT, true, NULL},
    {">=", "ge", "__ge", NY_BINOP_GE, true, NULL},
    {"<<", "bshl", "__shl", NY_BINOP_SHL, true, NULL},
    {">>", "bshr", "__shr", NY_BINOP_SHR, true, NULL},
    {"==", NULL, NULL, NY_BINOP_EQ, true, NULL},
    {"!=", NULL, NULL, NY_BINOP_NE, false, NULL},
    {"in", NULL, NULL, NY_BINOP_IN, false, NULL},
    {NULL, NULL, NULL, NY_BINOP_UNKNOWN, false, NULL}};

static LLVMValueRef ny_try_emit_tagged_int_fast_binary(codegen_t *cg,
                                                       const op_map_t *entry,
                                                       LLVMValueRef l,
                                                       LLVMValueRef r,
                                                       fun_sig *fallback) {
  if (!fallback || !entry)
    return NULL;
  if (!ny_env_enabled_default_on("NYTRIX_FAST_INT_BINOPS"))
    return NULL;

  if (!entry->fast_int_supported)
    return NULL;

  ny_binop_kind_t kind = entry->kind;

  LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  LLVMBasicBlockRef fast_bb = LLVMAppendBasicBlock(fn, "bin.int.fast");
  LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlock(fn, "bin.runtime.slow");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(fn, "bin.merge");
  LLVMValueRef both_int = LLVMBuildAnd(cg->builder, ny_is_tagged_int(cg, l),
                                       ny_is_tagged_int(cg, r), "bin.both_int");
  LLVMBuildCondBr(cg->builder, both_int, fast_bb, slow_bb);

  LLVMPositionBuilderAtEnd(cg->builder, fast_bb);

  LLVMValueRef li = ny_untag_int(cg, l);
  LLVMValueRef ri = ny_untag_int(cg, r);
  LLVMValueRef fast_value = NULL;
  LLVMBasicBlockRef fast_done_bb = NULL;
  if (kind == NY_BINOP_SUB && l == r) {
    fast_value = ny_const_tagged_int_value(cg, 0);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if (kind == NY_BINOP_XOR && l == r) {
    fast_value = ny_const_tagged_int_value(cg, 0);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if ((kind == NY_BINOP_AND || kind == NY_BINOP_OR) && l == r) {
    fast_value = l;
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if ((kind == NY_BINOP_EQ || kind == NY_BINOP_LE || kind == NY_BINOP_GE) &&
             l == r) {
    fast_value = ny_const_tagged_bool_value(cg, true);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if ((kind == NY_BINOP_LT || kind == NY_BINOP_GT) &&
             l == r) {
    fast_value = ny_const_tagged_bool_value(cg, false);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if (kind == NY_BINOP_ADD || kind == NY_BINOP_SUB || kind == NY_BINOP_MUL) {
    const char *intr_name = entry->overflow_intr;
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
  } else if (kind == NY_BINOP_DIV || kind == NY_BINOP_MOD) {
    LLVMValueRef is_zero =
        LLVMBuildICmp(cg->builder, LLVMIntEQ, ri,
                      LLVMConstInt(cg->type_i64, 0, false), "divmod_zero");
    LLVMValueRef one = LLVMConstInt(cg->type_i64, 1, false);
    LLVMValueRef safe_divisor =
        LLVMBuildSelect(cg->builder, is_zero, one, ri, "safe_divisor");
    LLVMValueRef raw =
        (kind == NY_BINOP_DIV)
            ? LLVMBuildUDiv(cg->builder, li, safe_divisor, "udiv_fast")
            : LLVMBuildURem(cg->builder, li, safe_divisor, "urem_fast");
    LLVMValueRef nonzero_res = ny_tag_int(cg, raw);
    LLVMValueRef zero_res = LLVMConstInt(
        cg->type_i64, kind == NY_BINOP_DIV ? 0 : 1, false);
    fast_value =
        LLVMBuildSelect(cg->builder, is_zero, zero_res, nonzero_res, "divmod");
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if (kind == NY_BINOP_AND || kind == NY_BINOP_OR || kind == NY_BINOP_XOR) {
    LLVMValueRef raw = NULL;
    if (kind == NY_BINOP_AND)
      raw = LLVMBuildAnd(cg->builder, li, ri, "and_fast");
    else if (kind == NY_BINOP_OR)
      raw = LLVMBuildOr(cg->builder, li, ri, "or_fast");
    else
      raw = LLVMBuildXor(cg->builder, li, ri, "xor_fast");
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else if (kind == NY_BINOP_SHL || kind == NY_BINOP_SHR) {
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

    LLVMValueRef raw = (kind == NY_BINOP_SHL)
                           ? LLVMBuildShl(cg->builder, li, ri, "shl_fast")
                           : LLVMBuildLShr(cg->builder, li, ri, "shr_fast");
    fast_value = ny_tag_int(cg, raw);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  } else {
    LLVMIntPredicate pred = LLVMIntEQ;
    if (kind == NY_BINOP_LT)
      pred = LLVMIntSLT;
    else if (kind == NY_BINOP_LE)
      pred = LLVMIntSLE;
    else if (kind == NY_BINOP_GT)
      pred = LLVMIntSGT;
    else if (kind == NY_BINOP_GE)
      pred = LLVMIntSGE;
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, li, ri, "icmp_fast");
    fast_value = ny_tag_bool(cg, cmp);
    fast_done_bb = LLVMGetInsertBlock(cg->builder);

    LLVMBuildBr(cg->builder, merge_bb);
  }
  LLVMPositionBuilderAtEnd(cg->builder, slow_bb);

  LLVMValueRef slow_value =
      LLVMBuildCall2(cg->builder, fallback->type, fallback->value,
                     (LLVMValueRef[]){l, r}, 2, "bin.slow");
  LLVMBasicBlockRef slow_done_bb = LLVMGetInsertBlock(cg->builder);

  LLVMBuildBr(cg->builder, merge_bb);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);

  LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "bin_result");
  LLVMValueRef incoming_vals[2] = {fast_value, slow_value};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  return phi;
}

static LLVMValueRef ny_try_emit_float_fast_binary(codegen_t *cg,
                                                  const op_map_t *entry,
                                                  LLVMValueRef l,
                                                  LLVMValueRef r,
                                                  fun_sig *fallback) {
  if (!fallback || !entry)
    return NULL;
  if (!ny_env_enabled_default_on("NYTRIX_FAST_FLOAT_BINOPS"))
    return NULL;

  ny_binop_kind_t kind = entry->kind;
  if (kind == NY_BINOP_AND || kind == NY_BINOP_OR || kind == NY_BINOP_XOR ||
      kind == NY_BINOP_SHL || kind == NY_BINOP_SHR || kind == NY_BINOP_MOD)
    return NULL;

  LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  
  LLVMValueRef is_l_flt = ny_is_float(cg, l);
  LLVMValueRef is_r_flt = ny_is_float(cg, r);
  LLVMValueRef either_flt = LLVMBuildOr(cg->builder, is_l_flt, is_r_flt, "bin.either_flt");

  LLVMBasicBlockRef fast_bb = LLVMAppendBasicBlock(fn, "bin.flt.fast");
  LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlock(fn, "bin.flt.slow");
  LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(fn, "bin.flt.merge");

  LLVMBuildCondBr(cg->builder, either_flt, fast_bb, slow_bb);

  LLVMPositionBuilderAtEnd(cg->builder, fast_bb);
  LLVMValueRef lf = ny_unbox_float(cg, l);
  LLVMValueRef rf = ny_unbox_float(cg, r);
  LLVMValueRef res_f = NULL;

  if (kind == NY_BINOP_ADD) res_f = LLVMBuildFAdd(cg->builder, lf, rf, "fadd");
  else if (kind == NY_BINOP_SUB) res_f = LLVMBuildFSub(cg->builder, lf, rf, "fsub");
  else if (kind == NY_BINOP_MUL) res_f = LLVMBuildFMul(cg->builder, lf, rf, "fmul");
  else if (kind == NY_BINOP_DIV) res_f = LLVMBuildFDiv(cg->builder, lf, rf, "fdiv");
  else {
    LLVMRealPredicate pred = LLVMRealOEQ;
    if (kind == NY_BINOP_LT) pred = LLVMRealOLT;
    else if (kind == NY_BINOP_LE) pred = LLVMRealOLE;
    else if (kind == NY_BINOP_GT) pred = LLVMRealOGT;
    else if (kind == NY_BINOP_GE) pred = LLVMRealOGE;
    LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, lf, rf, "fcmp");
    LLVMValueRef fast_bool = ny_tag_bool(cg, cmp);
    LLVMBasicBlockRef fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
    
    LLVMPositionBuilderAtEnd(cg->builder, slow_bb);
    LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, fallback->type, fallback->value, (LLVMValueRef[]){l, r}, 2, "bin.slow");
    LLVMBasicBlockRef slow_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);
    
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "bin_res_bool");
    LLVMAddIncoming(phi, (LLVMValueRef[]){fast_bool, slow_value}, (LLVMBasicBlockRef[]){fast_done_bb, slow_done_bb}, 2);
    return phi;
  }

  // Boxing the result
  fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
  LLVMValueRef fast_val = NULL;
  if (box_sig) {
    fast_val = LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                              (LLVMValueRef[]){LLVMBuildBitCast(cg->builder, res_f, cg->type_i64, "")}, 1, "box");
  } else {
    fast_val = LLVMConstInt(cg->type_i64, 0, false);
  }
  LLVMBasicBlockRef fast_done_bb = LLVMGetInsertBlock(cg->builder);
  LLVMBuildBr(cg->builder, merge_bb);

  LLVMPositionBuilderAtEnd(cg->builder, slow_bb);
  LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, fallback->type, fallback->value, (LLVMValueRef[]){l, r}, 2, "bin.slow");
  LLVMBasicBlockRef slow_done_bb = LLVMGetInsertBlock(cg->builder);
  LLVMBuildBr(cg->builder, merge_bb);

  LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "bin_res_num");
  LLVMAddIncoming(phi, (LLVMValueRef[]){fast_val, slow_value}, (LLVMBasicBlockRef[]){fast_done_bb, slow_done_bb}, 2);
  return phi;
}

LLVMValueRef gen_binary(codegen_t *cg, const char *op, LLVMValueRef l,
                        LLVMValueRef r) {
  if (!l || !r)
    return LLVMConstInt(cg->type_i64, 0, false);
  bool prefer_builtin_ops = ny_should_prefer_builtin_ops(cg);

  const op_map_t *entry = NULL;
  for (const op_map_t *m = op_map; m->op; m++) {
    if (strcmp(op, m->op) == 0) {
      entry = m;
      break;
    }
  }

  if (!entry) {
    return expr_fail(cg, (token_t){0}, "undefined operator '%s'", op);
  }

  const char *generic_name = entry->generic;
  const char *builtin_name = entry->builtin;
  ny_binop_kind_t kind = entry->kind;

  if (kind == NY_BINOP_IN) {
    fun_sig *s = ny_helper_contains(cg);
    if (!s)
      return expr_fail(cg, (token_t){0}, "'in' requires 'contains'");
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){r, l}, 2, "");
  }

  if (kind == NY_BINOP_NE) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri))
      return ny_const_tagged_bool_value(cg, li != ri);
    return LLVMBuildSub(cg->builder, LLVMConstInt(cg->type_i64, 6, false),
                        gen_binary(cg, "==", l, r), "");
  }

  if (kind == NY_BINOP_EQ) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri))
      return ny_const_tagged_bool_value(cg, li == ri);
    fun_sig *s = prefer_builtin_ops ? lookup_fun(cg, "__eq", 0) : ny_helper_eq(cg);
    if (!s)
      return expr_fail(cg, (token_t){0}, "'==' requires 'eq' (or __eq)");
    LLVMValueRef fast = ny_try_emit_tagged_int_fast_binary(cg, entry, l, r, s);
    if (fast)
      return fast;
    fast = ny_try_emit_float_fast_binary(cg, entry, l, r, s);
    if (fast)
      return fast;
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }

  if (builtin_name) {
    int64_t li = 0, ri = 0;
    if (ny_const_tagged_int(l, &li) && ny_const_tagged_int(r, &ri)) {
      if (kind == NY_BINOP_ADD)
        return ny_const_tagged_int_value(cg, li + ri);
      if (kind == NY_BINOP_SUB)
        return ny_const_tagged_int_value(cg, li - ri);
      if (kind == NY_BINOP_MUL)
        return ny_const_tagged_int_value(cg, li * ri);
      if (kind == NY_BINOP_DIV) {
        if (ri == 0)
          return LLVMConstInt(cg->type_i64, 0, false);
        if (li == INT64_MIN && ri == -1)
          goto skip_const_div_fold;
        if (ri == 1)
          return ny_const_tagged_int_value(cg, li);
        return ny_const_tagged_int_value(cg, li / ri);
      }
    skip_const_div_fold:
      if (kind == NY_BINOP_MOD) {
        if (ri == 0)
          return LLVMConstInt(cg->type_i64, 1, false);
        if (ri == 1 || ri == -1)
          return ny_const_tagged_int_value(cg, 0);
        return ny_const_tagged_int_value(cg, li % ri);
      }
      if (kind == NY_BINOP_AND)
        return ny_const_tagged_int_value(cg, li & ri);
      if (kind == NY_BINOP_OR)
        return ny_const_tagged_int_value(cg, li | ri);
      if (kind == NY_BINOP_XOR)
        return ny_const_tagged_int_value(cg, li ^ ri);
      if (kind == NY_BINOP_LT)
        return ny_const_tagged_bool_value(cg, li < ri);
      if (kind == NY_BINOP_LE)
        return ny_const_tagged_bool_value(cg, li <= ri);
      if (kind == NY_BINOP_GT)
        return ny_const_tagged_bool_value(cg, li > ri);
      if (kind == NY_BINOP_GE)
        return ny_const_tagged_bool_value(cg, li >= ri);
      if (kind == NY_BINOP_SHL && ri >= 0 && ri < 64)
        return ny_const_tagged_int_value(cg, (int64_t)(((uint64_t)li) << ri));
      if (kind == NY_BINOP_SHR && ri >= 0 && ri < 64)
        return ny_const_tagged_int_value(cg, (int64_t)(((uint64_t)li) >> ri));
    }
  }

  if (generic_name && !prefer_builtin_ops) {
    char full_generic[128];
    snprintf(full_generic, sizeof(full_generic), "std.core.reflect.%s",
             generic_name);
    fun_sig *s = lookup_fun(cg, full_generic, 0);
    if (!s)
      s = lookup_fun(cg, generic_name, 0);
    if (s && strcmp(s->name, builtin_name) != 0) {
      if (s->stmt_t && !ny_is_stdlib_tok(s->stmt_t->tok))
        s = NULL;
    }
    if (s && strcmp(s->name, builtin_name) != 0) {
      if (builtin_name) {
        LLVMValueRef fast =
            ny_try_emit_tagged_int_fast_binary(cg, entry, l, r, s);
        if (fast)
          return fast;
        fast = ny_try_emit_float_fast_binary(cg, entry, l, r, s);
        if (fast)
          return fast;
      }
      return LLVMBuildCall2(cg->builder, s->type, s->value,
                            (LLVMValueRef[]){l, r}, 2, "");
    }
  }
  if (builtin_name) {
    fun_sig *s = lookup_fun(cg, builtin_name, 0);
    if (!s)
      return expr_fail(cg, (token_t){0}, "builtin %s missing", builtin_name);
    LLVMValueRef fast =
        ny_try_emit_tagged_int_fast_binary(cg, entry, l, r, s);
    if (fast)
      return fast;
    fast = ny_try_emit_float_fast_binary(cg, entry, l, r, s);
    if (fast)
      return fast;
    return LLVMBuildCall2(cg->builder, s->type, s->value,
                          (LLVMValueRef[]){l, r}, 2, "");
  }

  return expr_fail(cg, (token_t){0}, "undefined operator '%s'", op);
}
