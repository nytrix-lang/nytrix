/*
 * Direct emitter from optimized NYIR functions into LLVM IR modules.
 *
 * Provides a canonical bridge from NYIR into LLVM-C representation,
 * enabling LLVM machine codegen to consume all NYIR-level optimizations
 * (alias analysis, load forwarding, store sinking, bounds/tag check elimination)
 * without divergent front-end AST lowering.
 */
#include "code/code.h"
#include "code/priv.h"
#include "code/native/llvm/legacy.h"
#include "code/native/llvm/internal.h"
#include "code/native/llvm/jit.h"
#include "code/native/internal.h"
#include "code/ir/ir.h"
#include "base/options.h"
#include <llvm-c/Core.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline LLVMValueRef ny_as_i64(codegen_t *cg, LLVMValueRef val) {
  if (!val)
    return ny_c0(cg);
  LLVMTypeRef ty = LLVMTypeOf(val);
  if (!ty)
    return ny_c0(cg);
  if (ty == cg->type_i64)
    return val;
  LLVMTypeKind kind = LLVMGetTypeKind(ty);
  if (kind == LLVMVoidTypeKind)
    return ny_c0(cg);
  if (kind == LLVMIntegerTypeKind) {
    unsigned bits = LLVMGetIntTypeWidth(ty);
    if (bits < 64)
      return LLVMBuildZExt(cg->builder, val, cg->type_i64, "");
    if (bits > 64)
      return LLVMBuildTrunc(cg->builder, val, cg->type_i64, "");
    return val;
  }
  if (kind == LLVMPointerTypeKind)
    return LLVMBuildPtrToInt(cg->builder, val, cg->type_i64, "");
  if (ty == cg->type_f64)
    return LLVMBuildBitCast(cg->builder, val, cg->type_i64, "");
  if (ty == cg->type_f32) {
    LLVMValueRef ext = LLVMBuildFPExt(cg->builder, val, cg->type_f64, "");
    return LLVMBuildBitCast(cg->builder, ext, cg->type_i64, "");
  }
  return val;
}

/*
 * Granlund-Montgomery signed division magic (Hacker's Delight 10-3).  Keep
 * this local to the LLVM bridge: the object backend has the same primitive,
 * but LLVM-C has no signed high-half multiply builder.  The resulting IR is
 * entirely integer operations, so it is valid for every i64 numerator rather
 * than relying on a non-negative-range assumption.
 */
static void ny_llvm_sdiv_magic(uint64_t divisor, int64_t *magic_out,
                               unsigned *shift_out) {
  const uint64_t two63 = (uint64_t)1 << 63;
  uint64_t anc = two63 - 1 - (two63 % divisor);
  unsigned p = 63;
  uint64_t q1 = two63 / anc;
  uint64_t r1 = two63 - q1 * anc;
  uint64_t q2 = two63 / divisor;
  uint64_t r2 = two63 - q2 * divisor;
  uint64_t delta;
  do {
    ++p;
    q1 <<= 1;
    r1 <<= 1;
    if (r1 >= anc) { ++q1; r1 -= anc; }
    q2 <<= 1;
    r2 <<= 1;
    if (r2 >= divisor) { ++q2; r2 -= divisor; }
    delta = divisor - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  *magic_out = (int64_t)(q2 + 1);
  *shift_out = p - 64;
}

static LLVMValueRef ny_llvm_srem_const_i64(codegen_t *cg, LLVMValueRef lhs,
                                           int64_t divisor) {
  uint64_t abs_divisor = divisor < 0 ? (uint64_t)(-(divisor + 1)) + 1
                                      : (uint64_t)divisor;
  int64_t magic;
  unsigned shift;
  ny_llvm_sdiv_magic(abs_divisor, &magic, &shift);

  LLVMTypeRef type_i128 = LLVMInt128TypeInContext(LLVMGetModuleContext(cg->module));
  LLVMValueRef x = ny_as_i64(cg, lhs);
  LLVMValueRef x128 = LLVMBuildSExt(cg->builder, x, type_i128, "");
  LLVMValueRef m = LLVMConstInt(cg->type_i64, (uint64_t)magic, false);
  LLVMValueRef m128 = LLVMBuildSExt(cg->builder, m, type_i128, "");
  LLVMValueRef product = LLVMBuildMul(cg->builder, x128, m128, "");
  LLVMValueRef high128 = LLVMBuildAShr(
      cg->builder, product, LLVMConstInt(type_i128, 64, false), "");
  LLVMValueRef quotient = LLVMBuildTrunc(cg->builder, high128, cg->type_i64, "");
  if (magic < 0)
    quotient = LLVMBuildAdd(cg->builder, quotient, x, "");
  if (shift)
    quotient = LLVMBuildAShr(cg->builder, quotient,
                             LLVMConstInt(cg->type_i64, shift, false), "");
  LLVMValueRef sign = LLVMBuildAShr(cg->builder, x,
                                    LLVMConstInt(cg->type_i64, 63, false), "");
  quotient = LLVMBuildSub(cg->builder, quotient, sign, "");
  if (divisor < 0)
    quotient = LLVMBuildNeg(cg->builder, quotient, "");
  LLVMValueRef d = LLVMConstInt(cg->type_i64, (uint64_t)divisor, false);
  return LLVMBuildSub(cg->builder, x, LLVMBuildMul(cg->builder, quotient, d, ""), "");
}

static inline LLVMValueRef ny_as_f64(codegen_t *cg, LLVMValueRef val) {
  if (!val)
    return LLVMConstReal(cg->type_f64, 0.0);
  LLVMTypeRef ty = LLVMTypeOf(val);
  if (!ty)
    return LLVMConstReal(cg->type_f64, 0.0);
  if (ty == cg->type_f64)
    return val;
  LLVMTypeKind kind = LLVMGetTypeKind(ty);
  if (kind == LLVMVoidTypeKind)
    return LLVMConstReal(cg->type_f64, 0.0);
  if (ty == cg->type_f32)
    return LLVMBuildFPExt(cg->builder, val, cg->type_f64, "");
  if (ty == cg->type_i64)
    return LLVMBuildBitCast(cg->builder, val, cg->type_f64, "");
  if (kind == LLVMIntegerTypeKind)
    return LLVMBuildSIToFP(cg->builder, val, cg->type_f64, "");
  return val;
}

static inline LLVMValueRef ny_as_f32(codegen_t *cg, LLVMValueRef val) {
  if (!val)
    return LLVMConstReal(cg->type_f32, 0.0);
  LLVMTypeRef ty = LLVMTypeOf(val);
  if (ty == cg->type_f32)
    return val;
  if (ty == cg->type_f64)
    return LLVMBuildFPTrunc(cg->builder, val, cg->type_f32, "");
  if (ty == cg->type_i64) {
    LLVMValueRef f64v = LLVMBuildBitCast(cg->builder, val, cg->type_f64, "");
    return LLVMBuildFPTrunc(cg->builder, f64v, cg->type_f32, "");
  }
  if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind)
    return LLVMBuildSIToFP(cg->builder, val, cg->type_f32, "");
  return val;
}

static LLVMValueRef ny_llvm_emit_f64_unary_runtime(codegen_t *cg,
                                                   const char *name,
                                                   LLVMValueRef value) {
  LLVMTypeRef params[] = {cg->type_f64};
  LLVMTypeRef fn_ty = LLVMFunctionType(cg->type_f64, params, 1, false);
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
  if (!fn) {
    fn = LLVMAddFunction(cg->module, name, fn_ty);
    LLVMSetLinkage(fn, LLVMExternalLinkage);
  }
  LLVMValueRef arg = ny_as_f64(cg, value);
  return LLVMBuildCall2(cg->builder, fn_ty, fn, &arg, 1, "");
}

static token_t ny_llvm_nyir_debug_token(const codegen_t *cg,
                                        const nyir_func_t *f) {
  token_t tok = {0};
  tok.line = 1;
  tok.col = 1;
  tok.filename = cg && cg->source_main_file ? cg->source_main_file : "<nyir>";
  if (!f)
    return tok;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_debug_loc_t *debug = &f->data[i].debug;
    if (!debug->line)
      continue;
    tok.line = (int)debug->line;
    tok.col = (int)(debug->column ? debug->column : 1);
    if (debug->file && *debug->file)
      tok.filename = debug->file;
    break;
  }
  return tok;
}

static void ny_llvm_set_nyir_debug_loc(codegen_t *cg,
                                       LLVMMetadataRef function_scope,
                                       const nyir_inst_t *in) {
  if (!cg || !cg->debug_symbols || !cg->di_builder || !in) {
    if (cg && cg->builder)
      LLVMSetCurrentDebugLocation2(cg->builder, NULL);
    return;
  }
  if (!in->debug.line) {
    cg->di_loc = NULL;
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
    return;
  }
  token_t tok = {0};
  tok.filename = in->debug.file;
  tok.line = (int)in->debug.line;
  tok.col = (int)(in->debug.column ? in->debug.column : 1);
  LLVMMetadataRef scope = codegen_debug_loc_scope(cg, tok);
  if (!scope)
    scope = function_scope;
  LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(
      cg->ctx, (unsigned)tok.line, (unsigned)tok.col, scope, NULL);
  cg->di_loc = loc;
  LLVMSetCurrentDebugLocation2(cg->builder, loc);
}

static LLVMTypeRef ny_llvm_vector_type(codegen_t *cg, nyir_op_t op) {
  if (!cg)
    return NULL;
  if (op == NYIR_VEC4_LOAD_F64 || op == NYIR_VEC4_STORE_F64 ||
      op == NYIR_VEC4_ADD_F64 || op == NYIR_VEC4_SUB_F64 ||
      op == NYIR_VEC4_MUL_F64 || op == NYIR_VEC4_DIV_F64 ||
      op == NYIR_VEC4_FMA_F64 || op == NYIR_VEC4_SET1_F64 ||
      op == NYIR_VEC4_SHUFFLE_F64 || op == NYIR_VEC4_REDUCE_ADD_F64)
    return LLVMVectorType(cg->type_f64, 2);
  if (op == NYIR_VEC8_LOAD_F32 || op == NYIR_VEC8_STORE_F32 ||
      op == NYIR_VEC8_ADD_F32 || op == NYIR_VEC8_SUB_F32 ||
      op == NYIR_VEC8_MUL_F32 || op == NYIR_VEC8_DIV_F32 ||
      op == NYIR_VEC8_FMA_F32 || op == NYIR_VEC8_SET1_F32 ||
      op == NYIR_VEC8_SHUFFLE_F32)
    return LLVMVectorType(cg->type_f32, 4);
  if (op == NYIR_VEC4_LOAD_I64 || op == NYIR_VEC4_STORE_I64 ||
      op == NYIR_VEC4_ADD_I64 || op == NYIR_VEC4_SUB_I64 ||
      op == NYIR_VEC4_SET1_I64 || op == NYIR_VEC4_AND_I64 ||
      op == NYIR_VEC4_OR_I64 || op == NYIR_VEC4_XOR_I64 ||
      op == NYIR_VEC4_SHL_I64 || op == NYIR_VEC4_SAR_I64 ||
      op == NYIR_VEC4_REDUCE_ADD_I64)
    return LLVMVectorType(cg->type_i64, 2);
  if (op == NYIR_VEC8_LOAD_I64 || op == NYIR_VEC8_STORE_I64 ||
      op == NYIR_VEC8_ADD_I64 || op == NYIR_VEC8_SUB_I64 ||
      op == NYIR_VEC8_AND_I64 || op == NYIR_VEC8_OR_I64 ||
      op == NYIR_VEC8_XOR_I64 || op == NYIR_VEC8_REDUCE_ADD_I64)
    return LLVMVectorType(cg->type_i64, 4);
  return NULL;
}

static LLVMValueRef ny_llvm_vector_slot(codegen_t *cg, LLVMValueRef *vals,
                                        size_t val_count, int value,
                                        LLVMTypeRef type) {
  (void)cg;
  if (value >= 0 && (size_t)value < val_count && vals[value] &&
      LLVMTypeOf(vals[value]) == type)
    return vals[value];
  return LLVMGetUndef(type);
}

static LLVMValueRef ny_llvm_vector_splat(codegen_t *cg, LLVMValueRef scalar,
                                         LLVMTypeRef vector_type) {
  unsigned lanes = LLVMGetVectorSize(vector_type);
  LLVMTypeRef scalar_type = LLVMGetElementType(vector_type);
  LLVMValueRef value = scalar;
  if (!value || LLVMTypeOf(value) != scalar_type)
    value = LLVMGetUndef(scalar_type);
  LLVMValueRef result = LLVMGetUndef(vector_type);
  LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->ctx);
  for (unsigned i = 0; i < lanes; ++i) {
    LLVMValueRef index = LLVMConstInt(index_type, i, false);
    result = LLVMBuildInsertElement(cg->builder, result, value, index, "");
  }
  return result;
}

static LLVMValueRef ny_llvm_vector_shuffle(codegen_t *cg, LLVMValueRef value,
                                           LLVMTypeRef type, int64_t mask) {
  unsigned lanes = LLVMGetVectorSize(type);
  LLVMValueRef *indices = calloc(lanes, sizeof(*indices));
  if (!indices)
    return LLVMGetUndef(type);
  LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->ctx);
  for (unsigned i = 0; i < lanes; ++i) {
    unsigned selected;
    if (lanes == 2) {
      selected = i == 0 ? ((uint64_t)mask & 1u)
                        : 2u + (((uint64_t)mask >> 1) & 1u);
    } else {
      unsigned shift = i < 2 ? i * 2u : 4u + (i - 2u) * 2u;
      selected = ((uint64_t)mask >> shift) & 3u;
      if (i >= 2)
        selected += 2u;
    }
    indices[i] = LLVMConstInt(index_type, selected, false);
  }
  LLVMValueRef mask_value = LLVMConstVector(indices, lanes);
  free(indices);
  return LLVMBuildShuffleVector(cg->builder, value, value, mask_value, "");
}

bool ny_llvm_emit_nyir_func(codegen_t *cg, const nyir_func_t *f,
                            const char *name, bool tag_return,
                            char *err, size_t err_len) {
  (void)tag_return;
  if (!cg || !f || !name || !*name) {
    if (err && err_len > 0)
      snprintf(err, err_len, "Invalid arguments to ny_llvm_emit_nyir_func");
    return false;
  }

  /*
   * Native object backends consistently keep source functions in the
   * `ny_fn_` namespace. LLVM must use the same namespace: runtime helpers
   * and unqualified C imports keep their public names, while a source
   * function such as `len` can shadow `rt_len` safely.
   */
  char function_name[512];
  const char *llvm_name = name;
  if (strcmp(name, "_ny_top_entry") != 0) {
    int n = snprintf(function_name, sizeof(function_name), "ny_fn_%s", name);
    if (n < 0 || (size_t)n >= sizeof(function_name)) {
      if (err && err_len > 0)
        snprintf(err, err_len, "LLVM NYIR emission: function symbol too long: %s", name);
      return false;
    }
    llvm_name = function_name;
  }

  /*
   * 1. Find or declare function in LLVM module
   */
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, llvm_name);
  size_t param_count = f->param_count;
  LLVMTypeRef *param_tys = malloc(sizeof(LLVMTypeRef) * (param_count + 1));
  for (size_t i = 0; i < param_count; ++i) {
    if (f->param_types && i < f->param_count) {
      if (f->param_types[i] == NYIR_PARAM_F64)
        param_tys[i] = cg->type_f64;
      else if (f->param_types[i] == NYIR_PARAM_F32)
        param_tys[i] = cg->type_f32;
      else
        param_tys[i] = cg->type_i64;
    } else {
      param_tys[i] = cg->type_i64;
    }
  }
  LLVMTypeRef ret_ty = cg->type_i64;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_RET) {
      if (f->data[i].flags & NYIR_INST_F_RET_F64)
        ret_ty = cg->type_f64;
      else if (f->data[i].flags & NYIR_INST_F_RET_F32)
        ret_ty = cg->type_f32;
      break;
    }
  }
  LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, param_tys, (unsigned)param_count, false);
  free(param_tys);

  if (fn) {
    if (LLVMCountParams(fn) != (unsigned)param_count ||
        LLVMGetReturnType(LLVMGlobalGetValueType(fn)) != ret_ty) {
      LLVMValueRef new_fn = LLVMAddFunction(cg->module, "", fn_ty);
      LLVMReplaceAllUsesWith(fn, new_fn);
      LLVMDeleteFunction(fn);
      LLVMSetValueName2(new_fn, llvm_name, strlen(llvm_name));
      fn = new_fn;
    } else {
      ny_llvm_clear_function(fn);
    }
  } else {
    fn = LLVMAddFunction(cg->module, llvm_name, fn_ty);
    LLVMSetLinkage(fn, LLVMExternalLinkage);
  }

  LLVMMetadataRef previous_scope = cg->di_scope;
  LLVMMetadataRef previous_loc = cg->di_loc;
  LLVMMetadataRef function_scope = previous_scope;
  if (cg->debug_symbols && cg->di_builder) {
    token_t tok = ny_llvm_nyir_debug_token(cg, f);
    LLVMMetadataRef subprogram =
        codegen_debug_subprogram(cg, fn, name, tok);
    if (subprogram) {
      function_scope = subprogram;
      cg->di_scope = subprogram;
      cg->di_loc = NULL;
    }
  }

  /*
   * 2. Collect max label ID and map basic blocks
   */
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL && in->imm > max_label)
      max_label = in->imm;
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm > max_label)
      max_label = in->imm;
  }

  size_t num_blocks = (max_label >= 0) ? (size_t)(max_label + 1) : 0;
  LLVMBasicBlockRef *blocks = calloc(num_blocks + 1, sizeof(LLVMBasicBlockRef));
  for (size_t i = 0; i < num_blocks; ++i) {
    char lbl_buf[64];
    snprintf(lbl_buf, sizeof(lbl_buf), "L%" PRId64, (int64_t)i);
    blocks[i] = LLVMAppendBasicBlockInContext(cg->ctx, fn, lbl_buf);
  }

  LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
  LLVMBasicBlockRef first_bb = LLVMGetFirstBasicBlock(fn);
  if (first_bb && first_bb != entry_bb)
    LLVMMoveBasicBlockBefore(entry_bb, first_bb);

  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  /*
   * 3. Allocate local variable storage in entry block
   *
   * Every slot is zero-initialized: the language defines uninitialized
   * locals as zero (the baseline stack emitter's zeroed frame provides
   * this for free).  Leaving the allocas undef let LLVM fold loop-head
   * loads of not-yet-stored slots into arbitrary constants, and mapcat
   * read its element 1 as 0.
   */
  size_t local_count = ny_native_nir_local_count(f);
  LLVMValueRef *local_allocas = calloc(local_count + 1, sizeof(LLVMValueRef));
  LLVMValueRef slot_zero = LLVMConstInt(cg->type_i64, 0, false);
  for (size_t i = 0; i < local_count; ++i) {
    char slot_name[32];
    snprintf(slot_name, sizeof(slot_name), "local_slot_%zu", i);
    local_allocas[i] = LLVMBuildAlloca(cg->builder, cg->type_i64, slot_name);
    LLVMBuildStore(cg->builder, slot_zero, local_allocas[i]);
  }

  /*
   * 4. Value storage table
   */
  size_t max_v = 0;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && (size_t)in->dst > max_v) max_v = (size_t)in->dst;
    if (in->a >= 0 && (size_t)in->a > max_v) max_v = (size_t)in->a;
    if (in->b >= 0 && (size_t)in->b > max_v) max_v = (size_t)in->b;
    if (in->c >= 0 && (size_t)in->c > max_v) max_v = (size_t)in->c;
    if (in->d >= 0 && (size_t)in->d > max_v) max_v = (size_t)in->d;
    if (in->e >= 0 && (size_t)in->e > max_v) max_v = (size_t)in->e;
    if (in->f >= 0 && (size_t)in->f > max_v) max_v = (size_t)in->f;
  }
  if (f->next_value > 0 && (size_t)f->next_value > max_v) max_v = (size_t)f->next_value;
  size_t val_count = max_v + 32;
  LLVMValueRef *vals = calloc(val_count + 1, sizeof(LLVMValueRef));

  /*
   * Map parameters into locals and value slots
   */
  for (size_t i = 0; i < f->param_count && i < val_count; ++i) {
    LLVMValueRef pval = LLVMGetParam(fn, (unsigned)i);
    vals[i] = pval;
    if (i < local_count)
      LLVMBuildStore(cg->builder, ny_as_i64(cg, pval), local_allocas[i]);
  }

  bool entry_terminated = false;
  bool unsupported = false;

  /*
   * Track PHI nodes to resolve incoming branches in a second pass
   */
  size_t phi_count = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_PHI)
      phi_count++;
  }

  /*
   * 5. Lower instructions
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;

    ny_llvm_set_nyir_debug_loc(cg, function_scope, in);

    if (in->op == NYIR_LABEL) {
      if (in->imm >= 0 && (size_t)in->imm < num_blocks && blocks[in->imm]) {
        LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
        if (!entry_terminated) {
          LLVMBuildBr(cg->builder, blocks[in->imm]);
          entry_terminated = true;
        } else if (cur_bb && !LLVMGetBasicBlockTerminator(cur_bb)) {
          LLVMBuildBr(cg->builder, blocks[in->imm]);
        }
        LLVMPositionBuilderAtEnd(cg->builder, blocks[in->imm]);
      }
      continue;
    }

    if (!entry_terminated)
      entry_terminated = true;

    switch (in->op) {
    case NYIR_CONST_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count)
        vals[in->dst] = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
      break;

    case NYIR_CONST_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        union { int64_t i; double d; } u;
        u.i = in->imm;
        vals[in->dst] = LLVMConstReal(cg->type_f64, u.d);
      }
      break;

    case NYIR_CONST_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        union { int32_t i; float f; } u;
        u.i = (int32_t)in->imm;
        vals[in->dst] = LLVMConstReal(cg->type_f32, u.f);
      }
      break;

    case NYIR_COPY:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        vals[in->dst] = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : ny_c0(cg);
      }
      break;

    case NYIR_PHI:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        vals[in->dst] = LLVMBuildPhi(cg->builder, cg->type_i64, "phi");
      }
      break;

    case NYIR_ADD_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildAdd(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_SUB_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildSub(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_MUL_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildMul(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_DIV_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildSDiv(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_MOD_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMValueRef divisor = ny_as_i64(cg, vb);
        if (LLVMIsAConstantInt(divisor)) {
          int64_t constant = LLVMConstIntGetSExtValue(divisor);
          /*
           * 0 retains the normal LLVM trap/poison behaviour; INT64_MIN has
           * no positive magnitude representable by the magic routine.
           */
          if (constant != 0 && constant != 1 && constant != -1 &&
              constant != INT64_MIN)
            vals[in->dst] = ny_llvm_srem_const_i64(cg, va, constant);
          else
            vals[in->dst] = LLVMBuildSRem(cg->builder, ny_as_i64(cg, va), divisor, "");
        } else {
          vals[in->dst] = LLVMBuildSRem(cg->builder, ny_as_i64(cg, va), divisor, "");
        }
      }
      break;

    case NYIR_AND_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildAnd(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_OR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildOr(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_XOR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildXor(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_SHL_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildShl(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_SAR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildAShr(cg->builder, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
      }
      break;

    case NYIR_ROR_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? ny_as_i64(cg, vals[in->a]) : ny_c0(cg);
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? ny_as_i64(cg, vals[in->b]) : ny_c0(cg);
        LLVMValueRef sh1 = LLVMBuildLShr(cg->builder, va, vb, "");
        LLVMValueRef diff = LLVMBuildSub(cg->builder, LLVMConstInt(cg->type_i64, 64, false), vb, "");
        LLVMValueRef sh2 = LLVMBuildShl(cg->builder, va, diff, "");
        vals[in->dst] = LLVMBuildOr(cg->builder, sh1, sh2, "");
      }
      break;

    case NYIR_ROR32_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ?
                              ny_as_i64(cg, vals[in->a]) : ny_c0(cg);
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ?
                              ny_as_i64(cg, vals[in->b]) : ny_c0(cg);
        LLVMValueRef mask = LLVMConstInt(cg->type_i64, UINT64_C(0xffffffff), false);
        va = LLVMBuildAnd(cg->builder, va, mask, "");
        vb = LLVMBuildAnd(cg->builder, vb,
                          LLVMConstInt(cg->type_i64, 31, false), "");
        LLVMValueRef sh1 = LLVMBuildLShr(cg->builder, va, vb, "");
        LLVMValueRef diff = LLVMBuildSub(
            cg->builder, LLVMConstInt(cg->type_i64, 32, false), vb, "");
        LLVMValueRef sh2 = LLVMBuildShl(cg->builder, va, diff, "");
        vals[in->dst] = LLVMBuildAnd(
            cg->builder, LLVMBuildOr(cg->builder, sh1, sh2, ""), mask, "");
      }
      break;

    case NYIR_SELECT_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMValueRef vc = (in->c >= 0 && (size_t)in->c < val_count) ? vals[in->c] : NULL;
        LLVMValueRef cond = LLVMBuildICmp(cg->builder, LLVMIntNE, ny_as_i64(cg, va), ny_c0(cg), "");
        vals[in->dst] = LLVMBuildSelect(cg->builder, cond, ny_as_i64(cg, vb), ny_as_i64(cg, vc), "");
      }
      break;

    case NYIR_CMP_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMIntPredicate pred = LLVMIntEQ;
        switch (in->cmp) {
        case NYIR_CMP_EQ: pred = LLVMIntEQ; break;
        case NYIR_CMP_NE: pred = LLVMIntNE; break;
        case NYIR_CMP_LT: pred = LLVMIntSLT; break;
        case NYIR_CMP_LE: pred = LLVMIntSLE; break;
        case NYIR_CMP_GT: pred = LLVMIntSGT; break;
        case NYIR_CMP_GE: pred = LLVMIntSGE; break;
        default: pred = LLVMIntEQ; break;
        }
        LLVMValueRef cmp = LLVMBuildICmp(cg->builder, pred, ny_as_i64(cg, va), ny_as_i64(cg, vb), "");
        vals[in->dst] = LLVMBuildZExt(cg->builder, cmp, cg->type_i64, "");
      }
      break;

    case NYIR_CMP_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMRealPredicate pred = LLVMRealOEQ;
        switch (in->cmp) {
        case NYIR_CMP_EQ: pred = LLVMRealOEQ; break;
        case NYIR_CMP_NE: pred = LLVMRealONE; break;
        case NYIR_CMP_LT: pred = LLVMRealOLT; break;
        case NYIR_CMP_LE: pred = LLVMRealOLE; break;
        case NYIR_CMP_GT: pred = LLVMRealOGT; break;
        case NYIR_CMP_GE: pred = LLVMRealOGE; break;
        default: pred = LLVMRealOEQ; break;
        }
        LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, ny_as_f64(cg, va), ny_as_f64(cg, vb), "");
        vals[in->dst] = LLVMBuildZExt(cg->builder, cmp, cg->type_i64, "");
      }
      break;

    case NYIR_SQRT_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? vals[in->a] : NULL;
        vals[in->dst] = ny_llvm_emit_f64_unary_runtime(
            cg, "rt_sqrt_f64", va);
      }
      break;

    case NYIR_SIN_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? vals[in->a] : NULL;
        vals[in->dst] = ny_llvm_emit_f64_unary_runtime(
            cg, "rt_sin_f64", va);
      }
      break;

    case NYIR_COS_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count)
                              ? vals[in->a] : NULL;
        vals[in->dst] = ny_llvm_emit_f64_unary_runtime(
            cg, "rt_cos_f64", va);
      }
      break;

    case NYIR_ADD_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFAdd(cg->builder, ny_as_f64(cg, va), ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_SUB_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFSub(cg->builder, ny_as_f64(cg, va), ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_MUL_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFMul(cg->builder, ny_as_f64(cg, va), ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_DIV_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFDiv(cg->builder, ny_as_f64(cg, va), ny_as_f64(cg, vb), "");
      }
      break;

    case NYIR_I64_TO_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = LLVMBuildSIToFP(cg->builder, ny_as_i64(cg, va), cg->type_f64, "");
      }
      break;

    case NYIR_F64_TO_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = LLVMBuildFPToSI(cg->builder, ny_as_f64(cg, va), cg->type_i64, "");
      }
      break;

    case NYIR_F32_TO_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = LLVMBuildFPToSI(cg->builder, ny_as_f32(cg, va), cg->type_i64, "");
      }
      break;

    case NYIR_ADD_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFAdd(cg->builder, ny_as_f32(cg, va), ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_SUB_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFSub(cg->builder, ny_as_f32(cg, va), ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_MUL_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFMul(cg->builder, ny_as_f32(cg, va), ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_DIV_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        vals[in->dst] = LLVMBuildFDiv(cg->builder, ny_as_f32(cg, va), ny_as_f32(cg, vb), "");
      }
      break;

    case NYIR_CMP_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef vb = (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
        LLVMRealPredicate pred = LLVMRealOEQ;
        switch (in->cmp) {
        case NYIR_CMP_EQ: pred = LLVMRealOEQ; break;
        case NYIR_CMP_NE: pred = LLVMRealONE; break;
        case NYIR_CMP_LT: pred = LLVMRealOLT; break;
        case NYIR_CMP_LE: pred = LLVMRealOLE; break;
        case NYIR_CMP_GT: pred = LLVMRealOGT; break;
        case NYIR_CMP_GE: pred = LLVMRealOGE; break;
        default: pred = LLVMRealOEQ; break;
        }
        LLVMValueRef cmp = LLVMBuildFCmp(cg->builder, pred, ny_as_f32(cg, va), ny_as_f32(cg, vb), "");
        vals[in->dst] = LLVMBuildZExt(cg->builder, cmp, cg->type_i64, "");
      }
      break;

    case NYIR_I64_TO_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = LLVMBuildSIToFP(cg->builder, ny_as_i64(cg, va), cg->type_f32, "");
      }
      break;

    case NYIR_F64_TO_F32:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = LLVMBuildFPTrunc(cg->builder, ny_as_f64(cg, va), cg->type_f32, "");
      }
      break;

    case NYIR_F32_TO_F64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        vals[in->dst] = LLVMBuildFPExt(cg->builder, ny_as_f32(cg, va), cg->type_f64, "");
      }
      break;

    case NYIR_LOAD_LOCAL:
      if (in->dst >= 0 && (size_t)in->dst < val_count && in->imm >= 0 && (size_t)in->imm < local_count) {
        vals[in->dst] = LLVMBuildLoad2(cg->builder, cg->type_i64, local_allocas[in->imm], "");
      }
      break;

    case NYIR_STORE_LOCAL:
      if (in->imm >= 0 && (size_t)in->imm < local_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMBuildStore(cg->builder, ny_as_i64(cg, va), local_allocas[in->imm]);
      }
      break;

    case NYIR_ADDR_LOCAL:
      if (in->dst >= 0 && (size_t)in->dst < val_count && in->imm >= 0 && (size_t)in->imm < local_count) {
        vals[in->dst] = LLVMBuildPtrToInt(cg->builder, local_allocas[in->imm], cg->type_i64, "");
      }
      break;

    case NYIR_ALLOCA: {
      /*
       * NYIR_ALLOCA returns a raw byte pointer, not a language value.  Keep
       * the allocation in LLVM's stack model so the LLVM/JIT path has the
       * same lifetime and address contract as the native object emitter.
       */
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMTypeRef i8_type = LLVMInt8TypeInContext(cg->ctx);
        LLVMValueRef bytes = LLVMConstInt(
            cg->type_i64, (uint64_t)(in->imm > 0 ? in->imm : 1), false);
        LLVMValueRef storage = LLVMBuildArrayAlloca(
            cg->builder, i8_type, bytes, "nyir_alloca");
        LLVMSetAlignment(storage, 16);
        vals[in->dst] = LLVMBuildPtrToInt(cg->builder, storage,
                                          cg->type_i64, "");
      }
      break;
    }

    case NYIR_LOAD_I64:
      if (in->dst >= 0 && (size_t)in->dst < val_count) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef base = ny_as_i64(cg, va);
        if (in->imm != 0) {
          LLVMValueRef off = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
          base = LLVMBuildAdd(cg->builder, base, off, "");
        }
        if (in->flags & NYIR_INST_F_MEM_BYTE) {
          LLVMTypeRef i8_type = LLVMInt8TypeInContext(cg->ctx);
          LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base, LLVMPointerType(i8_type, 0), "");
          LLVMValueRef loaded_i8 = LLVMBuildLoad2(cg->builder, i8_type, ptr, "");
          vals[in->dst] = LLVMBuildZExt(cg->builder, loaded_i8, cg->type_i64, "");
        } else if (in->flags & NYIR_INST_F_MEM_F64) {
          LLVMTypeRef f64_type = LLVMDoubleTypeInContext(cg->ctx);
          LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base, LLVMPointerType(f64_type, 0), "");
          vals[in->dst] = LLVMBuildLoad2(cg->builder, f64_type, ptr, "");
        } else {
          LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base, LLVMPointerType(cg->type_i64, 0), "");
          vals[in->dst] = LLVMBuildLoad2(cg->builder, cg->type_i64, ptr, "");
        }
      }
      break;

    case NYIR_STORE_I64: {
      LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
      LLVMValueRef vb = (in->c >= 0 && (size_t)in->c < val_count) ? vals[in->c] : NULL;
      LLVMValueRef base = ny_as_i64(cg, va);
      if (in->imm != 0) {
        LLVMValueRef off = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
        base = LLVMBuildAdd(cg->builder, base, off, "");
      }
      if (in->flags & NYIR_INST_F_MEM_BYTE) {
        LLVMTypeRef i8_type = LLVMInt8TypeInContext(cg->ctx);
        LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base, LLVMPointerType(i8_type, 0), "");
        LLVMValueRef val_i8 = LLVMBuildTrunc(cg->builder, ny_as_i64(cg, vb), i8_type, "");
        LLVMBuildStore(cg->builder, val_i8, ptr);
      } else if (in->flags & NYIR_INST_F_MEM_F64) {
        LLVMTypeRef f64_type = LLVMDoubleTypeInContext(cg->ctx);
        LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base, LLVMPointerType(f64_type, 0), "");
        LLVMBuildStore(cg->builder, ny_as_f64(cg, vb), ptr);
      } else {
        LLVMValueRef ptr = LLVMBuildIntToPtr(cg->builder, base, LLVMPointerType(cg->type_i64, 0), "");
        LLVMBuildStore(cg->builder, ny_as_i64(cg, vb), ptr);
      }
      break;
    }

    case NYIR_COPY_STRUCT: {
      /*
       * Aggregate ABI lowering uses raw byte pointers: a is the destination,
       * b is the source, and imm is the exact byte count.
       */
      if (in->imm < 0)
        break;
      LLVMTypeRef byte_ptr =
          LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
      LLVMValueRef dst = (in->a >= 0 && (size_t)in->a < val_count)
                             ? vals[in->a] : NULL;
      LLVMValueRef src = (in->b >= 0 && (size_t)in->b < val_count)
                             ? vals[in->b] : NULL;
      dst = LLVMBuildIntToPtr(cg->builder, ny_as_i64(cg, dst), byte_ptr, "");
      src = LLVMBuildIntToPtr(cg->builder, ny_as_i64(cg, src), byte_ptr, "");
      LLVMBuildMemCpy(cg->builder, dst, 1, src, 1,
                      LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false));
      break;
    }

    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC4_REDUCE_ADD_F64:
    case NYIR_VEC8_LOAD_F32:
    case NYIR_VEC8_STORE_F32:
    case NYIR_VEC8_ADD_F32:
    case NYIR_VEC8_SUB_F32:
    case NYIR_VEC8_MUL_F32:
    case NYIR_VEC8_DIV_F32:
    case NYIR_VEC8_FMA_F32:
    case NYIR_VEC8_SET1_F32:
    case NYIR_VEC8_SHUFFLE_F32:
    case NYIR_VEC4_LOAD_I64:
    case NYIR_VEC4_STORE_I64:
    case NYIR_VEC4_ADD_I64:
    case NYIR_VEC4_SUB_I64:
    case NYIR_VEC4_SET1_I64:
    case NYIR_VEC4_AND_I64:
    case NYIR_VEC4_OR_I64:
    case NYIR_VEC4_XOR_I64:
    case NYIR_VEC4_SHL_I64:
    case NYIR_VEC4_SAR_I64:
    case NYIR_VEC4_REDUCE_ADD_I64:
    case NYIR_VEC8_LOAD_I64:
    case NYIR_VEC8_STORE_I64:
    case NYIR_VEC8_ADD_I64:
    case NYIR_VEC8_SUB_I64:
    case NYIR_VEC8_AND_I64:
    case NYIR_VEC8_OR_I64:
    case NYIR_VEC8_XOR_I64:
    case NYIR_VEC8_REDUCE_ADD_I64: {
      LLVMTypeRef vector_type = ny_llvm_vector_type(cg, in->op);
      LLVMTypeRef element_type = LLVMGetElementType(vector_type);
      bool is_float = element_type == cg->type_f64 || element_type == cg->type_f32;
      unsigned lanes = LLVMGetVectorSize(vector_type);
      if (in->op == NYIR_VEC4_LOAD_F64 || in->op == NYIR_VEC8_LOAD_F32 ||
          in->op == NYIR_VEC4_LOAD_I64 || in->op == NYIR_VEC8_LOAD_I64) {
        LLVMValueRef address = (in->a >= 0 && (size_t)in->a < val_count)
                                   ? vals[in->a] : NULL;
        LLVMValueRef ptr = LLVMBuildIntToPtr(
            cg->builder, ny_as_i64(cg, address), LLVMPointerType(vector_type, 0), "");
        if (in->dst >= 0 && (size_t)in->dst < val_count)
          vals[in->dst] = LLVMBuildLoad2(cg->builder, vector_type, ptr, "");
        break;
      }
      if (in->op == NYIR_VEC4_STORE_F64 || in->op == NYIR_VEC8_STORE_F32 ||
          in->op == NYIR_VEC4_STORE_I64 || in->op == NYIR_VEC8_STORE_I64) {
        LLVMValueRef address = (in->a >= 0 && (size_t)in->a < val_count)
                                   ? vals[in->a] : NULL;
        int vector_value = in->b >= 0 ? in->b : in->c;
        LLVMValueRef vector = ny_llvm_vector_slot(
            cg, vals, val_count, vector_value, vector_type);
        LLVMValueRef ptr = LLVMBuildIntToPtr(
            cg->builder, ny_as_i64(cg, address), LLVMPointerType(vector_type, 0), "");
        LLVMBuildStore(cg->builder, vector, ptr);
        break;
      }
      if (in->op == NYIR_VEC4_SET1_F64 || in->op == NYIR_VEC8_SET1_F32 ||
          in->op == NYIR_VEC4_SET1_I64) {
        LLVMValueRef scalar = (in->a >= 0 && (size_t)in->a < val_count)
                                  ? vals[in->a] : NULL;
        if (is_float)
          scalar = element_type == cg->type_f64 ? ny_as_f64(cg, scalar)
                                                : ny_as_f32(cg, scalar);
        else
          scalar = ny_as_i64(cg, scalar);
        if (in->dst >= 0 && (size_t)in->dst < val_count)
          vals[in->dst] = ny_llvm_vector_splat(cg, scalar, vector_type);
        break;
      }
      if (in->op == NYIR_VEC4_REDUCE_ADD_F64 ||
          in->op == NYIR_VEC4_REDUCE_ADD_I64 ||
          in->op == NYIR_VEC8_REDUCE_ADD_I64) {
        LLVMValueRef scalar = (in->a >= 0 && (size_t)in->a < val_count)
                                  ? vals[in->a] : NULL;
        LLVMValueRef vector = ny_llvm_vector_slot(
            cg, vals, val_count, in->b, vector_type);
        LLVMValueRef result = is_float
                                  ? (element_type == cg->type_f64
                                         ? ny_as_f64(cg, scalar)
                                         : ny_as_f32(cg, scalar))
                                  : ny_as_i64(cg, scalar);
        LLVMTypeRef index_type = LLVMInt32TypeInContext(cg->ctx);
        for (unsigned lane = 0; lane < lanes; ++lane) {
          LLVMValueRef index = LLVMConstInt(index_type, lane, false);
          LLVMValueRef item = LLVMBuildExtractElement(
              cg->builder, vector, index, "");
          result = is_float ? LLVMBuildFAdd(cg->builder, result, item, "")
                            : LLVMBuildAdd(cg->builder, result, item, "");
        }
        if (in->dst >= 0 && (size_t)in->dst < val_count)
          vals[in->dst] = result;
        break;
      }
      LLVMValueRef lhs = ny_llvm_vector_slot(cg, vals, val_count, in->a,
                                             vector_type);
      LLVMValueRef rhs = ny_llvm_vector_slot(cg, vals, val_count, in->b,
                                             vector_type);
      LLVMValueRef result = NULL;
      switch (in->op) {
      case NYIR_VEC4_ADD_F64: case NYIR_VEC8_ADD_F32:
        result = LLVMBuildFAdd(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_SUB_F64: case NYIR_VEC8_SUB_F32:
        result = LLVMBuildFSub(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_MUL_F64: case NYIR_VEC8_MUL_F32:
        result = LLVMBuildFMul(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_DIV_F64: case NYIR_VEC8_DIV_F32:
        result = LLVMBuildFDiv(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_FMA_F64: case NYIR_VEC8_FMA_F32: {
        LLVMValueRef addend = ny_llvm_vector_slot(cg, vals, val_count, in->c,
                                                  vector_type);
        result = LLVMBuildFAdd(cg->builder,
                                LLVMBuildFMul(cg->builder, lhs, rhs, ""),
                                addend, "");
        break;
      }
      case NYIR_VEC4_ADD_I64: case NYIR_VEC8_ADD_I64:
        result = LLVMBuildAdd(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_SUB_I64: case NYIR_VEC8_SUB_I64:
        result = LLVMBuildSub(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_AND_I64: case NYIR_VEC8_AND_I64:
        result = LLVMBuildAnd(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_OR_I64: case NYIR_VEC8_OR_I64:
        result = LLVMBuildOr(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_XOR_I64: case NYIR_VEC8_XOR_I64:
        result = LLVMBuildXor(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_SHL_I64:
        result = LLVMBuildShl(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_SAR_I64:
        result = LLVMBuildAShr(cg->builder, lhs, rhs, ""); break;
      case NYIR_VEC4_SHUFFLE_F64: case NYIR_VEC8_SHUFFLE_F32:
        result = ny_llvm_vector_shuffle(cg, lhs, vector_type, in->imm); break;
      default:
        break;
      }
      if (result && in->dst >= 0 && (size_t)in->dst < val_count)
        vals[in->dst] = result;
      break;
    }

    case NYIR_BOUNDS_CHECK: {
      /*
       * Keep the LLVM path's bounds contract identical to the native
       * encoders: `b` is the byte offset and `c` is an optional dynamic
       * exclusive bound; when `c` is absent, `imm` is the fixed bound.
       */
      LLVMTypeRef params[] = {cg->type_i64, cg->type_i64};
      LLVMTypeRef check_ty = LLVMFunctionType(
          LLVMVoidTypeInContext(cg->ctx), params, 2, false);
      LLVMValueRef check = LLVMGetNamedFunction(
          cg->module, "rt_bounds_check");
      if (!check) {
        check = LLVMAddFunction(cg->module, "rt_bounds_check", check_ty);
        LLVMSetLinkage(check, LLVMExternalLinkage);
      }
      LLVMValueRef offset =
          (in->b >= 0 && (size_t)in->b < val_count) ? vals[in->b] : NULL;
      LLVMValueRef limit =
          (in->c >= 0 && (size_t)in->c < val_count) ? vals[in->c] : NULL;
      if (!limit)
        limit = LLVMConstInt(cg->type_i64, (uint64_t)in->imm, false);
      LLVMValueRef args[] = {ny_as_i64(cg, offset), ny_as_i64(cg, limit)};
      LLVMBuildCall2(cg->builder, check_ty, check, args, 2, "");
      break;
    }

    case NYIR_ADDR_SYMBOL:
      if (in->dst >= 0 && (size_t)in->dst < val_count && in->symbol) {
        const char *addr_name = in->symbol;
        LLVMValueRef sym = LLVMGetNamedGlobal(cg->module, addr_name);
        if (!sym)
          sym = LLVMGetNamedFunction(cg->module, addr_name);
        /*
         * Function-valued identifiers can be encountered while emitting a
         * caller before the collector reaches the callee body.  Never create
         * the fallback data global for those names: it permanently prevents
         * LLVM from installing the real function with the same symbol.
         */
        if (!sym && strncmp(addr_name, "ny_fn_", 6) == 0) {
          LLVMTypeRef provisional = LLVMFunctionType(
              cg->type_i64, NULL, 0, false);
          sym = LLVMAddFunction(cg->module, addr_name, provisional);
          LLVMSetLinkage(sym, LLVMExternalLinkage);
        }
        if (!sym) {
          size_t str_len = 0;
          const char *str_bytes = ny_native_strtab_get(in->symbol, &str_len);
          const ny_native_array_elem_t *elems = NULL;
          size_t count = 0, stride = 0;
          if (str_bytes) {
            LLVMValueRef str_const = LLVMConstString(
                str_bytes, (unsigned)str_len, false);
            sym = LLVMAddGlobal(cg->module, LLVMTypeOf(str_const), in->symbol);
            LLVMSetInitializer(sym, str_const);
            LLVMSetGlobalConstant(sym, true);
            LLVMSetLinkage(sym, LLVMPrivateLinkage);
            LLVMSetAlignment(sym, 16);
          } else if (ny_native_arraytab_get(in->symbol, &elems, &count, &stride)) {
            size_t total_bytes = 24 + count * stride;
            if (stride == 24) {
              for (size_t k = 0; k < count; ++k)
                if (elems[k].str)
                  total_bytes += elems[k].str_len + 1;
            }
            uint8_t *buf = calloc(1, total_bytes);
            if (buf) {
              uint64_t *hdr = (uint64_t *)buf;
              hdr[0] = count;
              hdr[1] = stride;
              hdr[2] = count;
              size_t off = 24;
              size_t string_base = count * stride;
              size_t string_cursor = 0;
              for (size_t k = 0; k < count; ++k) {
                int64_t words[3] = {elems[k].value, 0, elems[k].tag};
                if (stride == 24 && elems[k].str) {
                  words[0] = (int64_t)(string_base + string_cursor - k * 24);
                  words[1] = (int64_t)elems[k].str_len;
                  words[2] = 121;
                }
                for (size_t w = 0; w < stride / 8; ++w) {
                  uint64_t raw = (uint64_t)words[w];
                  memcpy(buf + off, &raw, 8);
                  off += 8;
                }
                if (stride == 24 && elems[k].str)
                  string_cursor += elems[k].str_len + 1;
              }
              if (stride == 24) {
                for (size_t k = 0; k < count; ++k) {
                  if (elems[k].str) {
                    memcpy(buf + off, elems[k].str, elems[k].str_len + 1);
                    off += elems[k].str_len + 1;
                  }
                }
              }
              LLVMValueRef bytes_const = LLVMConstString((const char *)buf, (unsigned)total_bytes, true);
              char raw_name[128];
              snprintf(raw_name, sizeof(raw_name), "%s.raw", in->symbol);
              LLVMValueRef base_global = LLVMAddGlobal(cg->module, LLVMTypeOf(bytes_const), raw_name);
              LLVMSetInitializer(base_global, bytes_const);
              LLVMSetGlobalConstant(base_global, false);
              LLVMSetLinkage(base_global, LLVMPrivateLinkage);
              LLVMSetAlignment(base_global, 16);
              LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 0, false), LLVMConstInt(cg->type_i64, 24, false)};
              LLVMValueRef gep = LLVMConstInBoundsGEP2(LLVMTypeOf(bytes_const), base_global, indices, 2);
              sym = LLVMAddAlias2(cg->module, cg->type_i64, 0, gep, in->symbol);
              LLVMSetLinkage(sym, LLVMPrivateLinkage);
              free(buf);
            }
          } else {
            sym = LLVMAddGlobal(cg->module, cg->type_i64, in->symbol);
            LLVMSetInitializer(sym, LLVMConstInt(cg->type_i64, 0, false));
            LLVMSetLinkage(sym, LLVMPrivateLinkage);
            LLVMSetAlignment(sym, 8);
          }
        }
        vals[in->dst] = LLVMBuildPtrToInt(cg->builder, sym, cg->type_i64, "");
      }
      break;

    case NYIR_CALL: {
      int call_args[NYIR_CALL_MAX_ARGS];
      int call_argc = 0;
      char c_err[128] = {0};
      (void)nyir_call_args(in, (int)val_count, call_args, NYIR_CALL_MAX_ARGS,
                           &call_argc, c_err, sizeof(c_err));
      const char *callee_name = in->symbol ? in->symbol : "unknown_fn";
      /*
       * `std.math.float.is_float` is a standard-library predicate over the
       * boxed any ABI.  Its source wrapper can be omitted from the reachable
       * NYIR set when only a sibling conversion helper is used, so bind the
       * canonical call directly to the runtime implementation just as the
       * native object path does.
       */
      if (strcmp(callee_name, "std.math.float.is_float") == 0)
        callee_name = "rt_is_float_obj";
      char user_callee_name[512];
      if (!(in->flags & NYIR_INST_F_EXTERN) &&
          strcmp(callee_name, "_ny_top_entry") != 0) {
        int n = snprintf(user_callee_name, sizeof(user_callee_name),
                         "ny_fn_%s", callee_name);
        if (n < 0 || (size_t)n >= sizeof(user_callee_name)) {
          if (err && err_len > 0)
            snprintf(err, err_len,
                     "LLVM NYIR emission: call symbol too long: %s",
                     callee_name);
          unsupported = true;
          break;
        }
        callee_name = user_callee_name;
      }
      LLVMValueRef callee = LLVMGetNamedFunction(cg->module, callee_name);
      LLVMTypeRef *arg_tys = malloc(sizeof(LLVMTypeRef) * (call_argc + 1));
      for (int k = 0; k < call_argc; ++k) {
        int v = call_args[k];
        LLVMValueRef value = (v >= 0 && (size_t)v < val_count) ? vals[v] : NULL;
        LLVMTypeRef ty = value ? LLVMTypeOf(value) : NULL;
        arg_tys[k] = (ty == cg->type_f64) ? cg->type_f64
                     : (ty == cg->type_f32) ? cg->type_f32
                                            : cg->type_i64;
      }
      if (!callee) {
        LLVMTypeRef ret_ty = (in->flags & NYIR_INST_F_RET_F64)
                                 ? cg->type_f64
                                 : (in->flags & NYIR_INST_F_RET_F32)
                                       ? cg->type_f32
                                       : cg->type_i64;
        LLVMTypeRef callee_ty = LLVMFunctionType(ret_ty, arg_tys, (unsigned)call_argc, false);
        callee = LLVMAddFunction(cg->module, callee_name, callee_ty);
        LLVMSetLinkage(callee, LLVMExternalLinkage);
      }
      LLVMTypeRef callee_fn_ty = LLVMGlobalGetValueType(callee);
      unsigned callee_argc = LLVMCountParamTypes(callee_fn_ty);
      LLVMTypeRef *callee_arg_tys =
          callee_argc ? malloc(sizeof(LLVMTypeRef) * callee_argc) : NULL;
      if (callee_argc)
        LLVMGetParamTypes(callee_fn_ty, callee_arg_tys);
      LLVMTypeRef expected_ret_ty = (in->flags & NYIR_INST_F_RET_F64)
                                        ? cg->type_f64
                                        : (in->flags & NYIR_INST_F_RET_F32)
                                              ? cg->type_f32
                                              : cg->type_i64;
      /*
       * Runtime aliases registered by the legacy AST backend can already
       * exist in the module with the expanded `any` ABI.  NYIR calls carry
       * their canonical raw argument list, so repair a declaration whenever
       * either its return type or arity differs.
       */
      bool arg_types_differ =
          callee_argc != (unsigned)call_argc;
      if (!arg_types_differ) {
        for (int k = 0; k < call_argc; ++k) {
          if (callee_arg_tys[k] != arg_tys[k]) {
            arg_types_differ = true;
            break;
          }
        }
      }
      if ((LLVMGetReturnType(callee_fn_ty) != expected_ret_ty ||
           arg_types_differ) &&
          LLVMCountBasicBlocks(callee) == 0) {
        LLVMTypeRef new_fn_ty = LLVMFunctionType(expected_ret_ty, arg_tys, (unsigned)call_argc, false);
        LLVMValueRef new_fn = LLVMAddFunction(cg->module, "", new_fn_ty);
        LLVMReplaceAllUsesWith(callee, new_fn);
        LLVMDeleteFunction(callee);
        LLVMSetValueName2(new_fn, callee_name, strlen(callee_name));
        callee = new_fn;
        callee_fn_ty = new_fn_ty;
        free(callee_arg_tys);
        callee_argc = (unsigned)call_argc;
        callee_arg_tys = arg_tys;
        arg_tys = NULL;
      }
      LLVMValueRef *call_vals = malloc(sizeof(LLVMValueRef) * (call_argc + 1));
      for (int k = 0; k < call_argc; ++k) {
        int v = call_args[k];
        LLVMValueRef value = (v >= 0 && (size_t)v < val_count) ? vals[v] : NULL;
        LLVMTypeRef param_ty = callee_arg_tys[(unsigned)k];
        if (param_ty == cg->type_f64)
          call_vals[k] = ny_as_f64(cg, value);
        else if (param_ty == cg->type_f32)
          call_vals[k] = ny_as_f32(cg, value);
        else
          call_vals[k] = ny_as_i64(cg, value);
      }
      LLVMValueRef call_res = LLVMBuildCall2(cg->builder, callee_fn_ty, callee, call_vals, (unsigned)call_argc, "");
      if (in->dst >= 0 && (size_t)in->dst < val_count)
        vals[in->dst] = call_res;
      free(call_vals);
      free(callee_arg_tys);
      free(arg_tys);
      break;
    }

    case NYIR_BR:
      if (in->imm >= 0 && (size_t)in->imm < num_blocks && blocks[in->imm]) {
        LLVMBuildBr(cg->builder, blocks[in->imm]);
      }
      break;

    case NYIR_BR_IF:
      if (in->imm >= 0 && (size_t)in->imm < num_blocks && blocks[in->imm]) {
        LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
        LLVMValueRef cond = LLVMBuildICmp(cg->builder, LLVMIntNE, ny_as_i64(cg, va), ny_c0(cg), "");
        LLVMBasicBlockRef true_bb = blocks[in->imm];
        char ft_name[64];
        snprintf(ft_name, sizeof(ft_name), "br_fallthrough_%zu", i);
        LLVMBasicBlockRef ft_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, ft_name);
        LLVMBuildCondBr(cg->builder, cond, true_bb, ft_bb);
        LLVMPositionBuilderAtEnd(cg->builder, ft_bb);
      }
      break;

    case NYIR_RET: {
      LLVMValueRef va = (in->a >= 0 && (size_t)in->a < val_count) ? vals[in->a] : NULL;
      if (in->flags & NYIR_INST_F_RET_F64)
        LLVMBuildRet(cg->builder, ny_as_f64(cg, va));
      else if (in->flags & NYIR_INST_F_RET_F32)
        LLVMBuildRet(cg->builder, ny_as_f32(cg, va));
      else
        LLVMBuildRet(cg->builder, ny_as_i64(cg, va));
      break;
    }

    case NYIR_CAPTURE_RET:
      if (err && err_len > 0) {
        if (in->debug.line)
          snprintf(err, err_len,
                   "LLVM NYIR emission: %s is not supported at %s:%u:%u",
                   nyir_op_name(in->op),
                   in->debug.file && *in->debug.file ? in->debug.file : "<source>",
                   in->debug.line, in->debug.column ? in->debug.column : 1);
        else
          snprintf(err, err_len, "LLVM NYIR emission: %s is not supported",
                   nyir_op_name(in->op));
      }
      unsupported = true;
      break;

    default:
      if (err && err_len > 0) {
        if (in->debug.line)
          snprintf(err, err_len,
                   "LLVM NYIR emission: unsupported opcode %s at %s:%u:%u",
                   nyir_op_name(in->op),
                   in->debug.file && *in->debug.file ? in->debug.file : "<source>",
                   in->debug.line, in->debug.column ? in->debug.column : 1);
        else
          snprintf(err, err_len, "LLVM NYIR emission: unsupported opcode %s",
                   nyir_op_name(in->op));
      }
      unsupported = true;
      break;
    }
  }

  if (unsupported) {
    free(vals);
    free(local_allocas);
    free(blocks);
    cg->di_scope = previous_scope;
    cg->di_loc = previous_loc;
    LLVMSetCurrentDebugLocation2(cg->builder, previous_loc);
    return false;
  }

  /*
   * 6. Populate PHI incoming values
   */
  if (phi_count > 0) {
    for (size_t i = 0; i < f->len; ++i) {
      const nyir_inst_t *in = &f->data[i];
      if (in->op == NYIR_PHI && in->dst >= 0 && (size_t)in->dst < val_count && vals[in->dst]) {
        LLVMValueRef phi_val = vals[in->dst];
        for (size_t k = 0; k < in->phi_incoming_len; ++k) {
          int64_t plbl = in->phi_incoming[k].predecessor_label;
          int inc_v = in->phi_incoming[k].value;
          if (plbl >= 0 && (size_t)plbl < num_blocks && blocks[plbl]) {
            LLVMBasicBlockRef inc_bb = blocks[plbl];
            LLVMValueRef term = LLVMGetBasicBlockTerminator(inc_bb);
            if (term)
              LLVMPositionBuilderBefore(cg->builder, term);
            else
              LLVMPositionBuilderAtEnd(cg->builder, inc_bb);
            LLVMValueRef inc_val = (inc_v >= 0 && (size_t)inc_v < val_count && vals[inc_v])
                                       ? ny_as_i64(cg, vals[inc_v])
                                       : ny_c0(cg);
            LLVMAddIncoming(phi_val, &inc_val, &inc_bb, 1);
          }
        }
      }
    }
  }

  /*
   * 7. Ensure all blocks have terminators
   */
  for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
    if (!LLVMGetBasicBlockTerminator(bb)) {
      LLVMPositionBuilderAtEnd(cg->builder, bb);
      LLVMBuildRet(cg->builder, ny_c0(cg));
    }
  }

  free(vals);
  free(local_allocas);
  free(blocks);
  cg->di_scope = previous_scope;
  cg->di_loc = previous_loc;
  LLVMSetCurrentDebugLocation2(cg->builder, previous_loc);
  return true;
}

bool ny_llvm_emit_nyir_program(codegen_t *cg, const program_t *prog,
                               const ny_options *opt,
                               char *err, size_t err_len) {
  if (!cg || !prog)
    return false;
  size_t max_funcs = NY_NATIVE_LIVE_MAX_FUNCS;
  nyir_func_t *func_nirs = calloc(max_funcs, sizeof(nyir_func_t));
  const char **func_names = calloc(max_funcs, sizeof(char *));
  size_t func_count = 0;
  nyir_func_t rt_main_nir = {0};
  char build_err[512] = {0};

  bool ok = ny_native_build_nir(prog, opt, &rt_main_nir, func_nirs,
                                &func_count, func_names, max_funcs,
                                build_err, sizeof(build_err));
  if (!ok) {
    if (err && err_len > 0)
      snprintf(err, err_len, "NYIR build failed: %s", build_err);
    free(func_nirs);
    free(func_names);
    return false;
  }

  for (size_t i = 0; i < func_count; ++i) {
    if (!ny_llvm_emit_nyir_func(cg, &func_nirs[i], func_names[i], false, err, err_len)) {
      ok = false;
      break;
    }
  }

  if (ok && rt_main_nir.len > 0) {
    ok = ny_llvm_emit_nyir_func(cg, &rt_main_nir, "_ny_top_entry", false,
                                err, err_len);
  }
  if (ok && getenv("NY_DUMP_LLVM")) {
    fprintf(stderr, "%s", LLVMPrintModuleToString(cg->module));
  }

  nyir_func_free(&rt_main_nir);
  for (size_t i = 0; i < func_count; ++i)
    nyir_func_free(&func_nirs[i]);
  free(func_nirs);
  free(func_names);
  return ok;
}
