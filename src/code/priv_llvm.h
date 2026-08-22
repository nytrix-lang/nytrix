/*
 * Focused header for LLVM builder helpers, types, and loop metadata.
 */
#ifndef PRIV_LLVM_H
#define PRIV_LLVM_H

#include "code/code.h"
#include "rt/shared.h"
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>

static inline LLVMBasicBlockRef ny_llvm_append_block(LLVMValueRef fn, const char *name) {
  return LLVMAppendBasicBlockInContext(LLVMGetModuleContext(LLVMGetGlobalParent(fn)), fn, name);
}

/* ── LLVM helpers ─────────────────────────────────────────── */
static inline LLVMBasicBlockRef ny_cur_block(codegen_t *cg) {
  return LLVMGetInsertBlock(cg->builder);
}
static inline LLVMValueRef ny_cur_fn(codegen_t *cg) {
  return LLVMGetBasicBlockParent(ny_cur_block(cg));
}
static inline LLVMValueRef ny_has_terminator(codegen_t *cg) {
  return LLVMGetBasicBlockTerminator(ny_cur_block(cg));
}
static inline LLVMValueRef ny_get_named_fn(codegen_t *cg, const char *n) {
  return LLVMGetNamedFunction(cg->module, n);
}
static inline LLVMTypeRef ny_i1_ty(codegen_t *cg) { return LLVMInt1TypeInContext(cg->ctx); }
static inline LLVMTypeRef ny_i8_ty(codegen_t *cg) { return LLVMInt8TypeInContext(cg->ctx); }
static inline LLVMTypeRef ny_ptr_i64_ty(codegen_t *cg) { return LLVMPointerType(cg->type_i64, 0); }

/* ── Position & branching ───────────────────────────────────── */
static inline void ny_pos(codegen_t *cg, LLVMBasicBlockRef bb) {
  LLVMPositionBuilderAtEnd(cg->builder, bb);
}
static inline LLVMValueRef ny_br(codegen_t *cg, LLVMBasicBlockRef dest) {
  return LLVMBuildBr(cg->builder, dest);
}
static inline LLVMValueRef ny_cond_br(codegen_t *cg, LLVMValueRef cond, LLVMBasicBlockRef tb,
                                      LLVMBasicBlockRef fb) {
  return LLVMBuildCondBr(cg->builder, cond, tb, fb);
}

/* ── Loads & Stores ─────────────────────────────────────────── */
static inline LLVMValueRef ny_load(codegen_t *cg, LLVMValueRef ptr, const char *name) {
  return LLVMBuildLoad2(cg->builder, cg->type_i64, ptr, (cg && cg->llvm_value_names && name) ? name : "");
}
static inline LLVMValueRef ny_load_type(codegen_t *cg, LLVMTypeRef ty, LLVMValueRef ptr,
                                        const char *name) {
  return LLVMBuildLoad2(cg->builder, ty, ptr, (cg && cg->llvm_value_names && name) ? name : "");
}
static inline LLVMValueRef ny_store(codegen_t *cg, LLVMValueRef ptr, LLVMValueRef val) {
  return LLVMBuildStore(cg->builder, val, ptr);
}

/* ── Globals ────────────────────────────────────────────────── */
static inline LLVMValueRef ny_get_global(codegen_t *cg, const char *name) {
  return LLVMGetNamedGlobal(cg->module, name);
}

/* ── Constants ──────────────────────────────────────────────── */
static inline LLVMValueRef ny_c0(codegen_t *cg) { return LLVMConstInt(cg->type_i64, 0, false); }
static inline LLVMValueRef ny_c1(codegen_t *cg) { return LLVMConstInt(cg->type_i64, 1, false); }
static inline LLVMValueRef ny_cnil(codegen_t *cg) { return LLVMConstInt(cg->type_i64, NY_IMM_NIL, false); }
static inline LLVMValueRef ny_ctrue(codegen_t *cg) { return LLVMConstInt(cg->type_i64, NY_IMM_TRUE, false); }
static inline LLVMValueRef ny_cfalse(codegen_t *cg) { return LLVMConstInt(cg->type_i64, NY_IMM_FALSE, false); }
static inline LLVMValueRef ny_cbool(codegen_t *cg, int v) {
  return LLVMConstInt(ny_i1_ty(cg), !!v, false);
}
static inline LLVMValueRef ny_ci(codegen_t *cg, uint64_t v) { return LLVMConstInt(cg->type_i64, v, false); }

/* ── Type helpers ───────────────────────────────────────────── */
static inline int ny_is_i64(codegen_t *cg, LLVMValueRef v) { return LLVMTypeOf(v) == cg->type_i64; }
static inline int ny_is_ptr(codegen_t *cg, LLVMValueRef v) {
  (void)cg;
  return LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind;
}
static inline int ny_is_i1(codegen_t *cg, LLVMValueRef v) {
  return LLVMTypeOf(v) == LLVMInt1TypeInContext(cg->ctx);
}
static inline LLVMTypeRef ny_ptr_i64(codegen_t *cg) { return LLVMPointerType(cg->type_i64, 0); }

/* ── Comparisons & Arithmetic ────────────────────────────────── */
static inline LLVMValueRef ny_icmp(codegen_t *cg, LLVMIntPredicate pred, LLVMValueRef lhs,
                                   LLVMValueRef rhs, const char *name) {
  return LLVMBuildICmp(cg->builder, pred, lhs, rhs, (cg && cg->llvm_value_names && name) ? name : "");
}

#define ny_eq(cg, a, b, n) ny_icmp(cg, LLVMIntEQ, a, b, n)
#define ny_ne(cg, a, b, n) ny_icmp(cg, LLVMIntNE, a, b, n)
#define ny_slt(cg, a, b, n) ny_icmp(cg, LLVMIntSLT, a, b, n)
#define ny_sle(cg, a, b, n) ny_icmp(cg, LLVMIntSLE, a, b, n)
#define ny_sgt(cg, a, b, n) ny_icmp(cg, LLVMIntSGT, a, b, n)
#define ny_sge(cg, a, b, n) ny_icmp(cg, LLVMIntSGE, a, b, n)
#define ny_ult(cg, a, b, n) ny_icmp(cg, LLVMIntULT, a, b, n)
#define ny_ugt(cg, a, b, n) ny_icmp(cg, LLVMIntUGT, a, b, n)

static inline LLVMValueRef ny_add(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildAdd(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_sub(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildSub(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_mul(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildMul(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_shl(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildShl(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_ashr(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildAShr(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_and(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildAnd(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_or(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildOr(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_xor(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildXor(cg->builder, a, b, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline LLVMValueRef ny_select(codegen_t *cg, LLVMValueRef cond, LLVMValueRef t,
                                     LLVMValueRef f, const char *n) {
  return LLVMBuildSelect(cg->builder, cond, t, f, (cg && cg->llvm_value_names && n) ? n : "");
}

/* ── PHI nodes ──────────────────────────────────────────────── */
static inline LLVMValueRef ny_phi(codegen_t *cg, LLVMTypeRef t, const char *n) {
  return LLVMBuildPhi(cg->builder, t, (cg && cg->llvm_value_names && n) ? n : "");
}
static inline void ny_phi_add(LLVMValueRef p, LLVMValueRef v, LLVMBasicBlockRef b) {
  LLVMAddIncoming(p, &v, &b, 1);
}

/* ── Calls ──────────────────────────────────────────────────── */
static inline LLVMValueRef ny_call0(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn) {
  return LLVMBuildCall2(cg->builder, ft, fn, NULL, 0, "");
}
static inline LLVMValueRef ny_call1(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn, LLVMValueRef a0) {
  return LLVMBuildCall2(cg->builder, ft, fn, &a0, 1, "");
}
static inline LLVMValueRef ny_call2(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn, LLVMValueRef a0, LLVMValueRef a1) {
  LLVMValueRef args[2] = {a0, a1};
  return LLVMBuildCall2(cg->builder, ft, fn, args, 2, "");
}
static inline LLVMValueRef ny_call3(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn, LLVMValueRef a0, LLVMValueRef a1, LLVMValueRef a2) {
  LLVMValueRef args[3] = {a0, a1, a2};
  return LLVMBuildCall2(cg->builder, ft, fn, args, 3, "");
}

/* ── Block creation ─────────────────────────────────────────── */
static inline LLVMBasicBlockRef ny_bb(codegen_t *cg, const char *name) {
  return ny_llvm_append_block(LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder)), name);
}
static inline LLVMBasicBlockRef ny_bb_fn(LLVMValueRef fn, const char *name) {
  return ny_llvm_append_block(fn, name);
}

/* ── Loop Metadata ──────────────────────────────────────────── */
static inline void ny_loop_metadata_set(codegen_t *cg, LLVMValueRef branch,
                                        LLVMMetadataRef *attrs,
                                        size_t attr_count) {
  if (!cg || !branch || !attrs || attr_count == 0)
    return;
  LLVMContextRef ctx = cg->ctx;
  LLVMMetadataRef tmp = LLVMTemporaryMDNode(ctx, NULL, 0);
  LLVMMetadataRef *ops =
      (LLVMMetadataRef *)alloca(sizeof(LLVMMetadataRef) * (attr_count + 1));
  ops[0] = tmp;
  for (size_t i = 0; i < attr_count; ++i)
    ops[i + 1] = attrs[i];
  LLVMMetadataRef md = LLVMMDNodeInContext2(ctx, ops, attr_count + 1);
  LLVMMetadataReplaceAllUsesWith(tmp, md);
  unsigned kind = LLVMGetMDKindIDInContext(ctx, "llvm.loop", 9);
  LLVMSetMetadata(branch, kind, LLVMMetadataAsValue(ctx, md));
}

static inline LLVMMetadataRef ny_loop_flag_attr(LLVMContextRef ctx,
                                                const char *name) {
  LLVMMetadataRef s = LLVMMDStringInContext2(ctx, name, strlen(name));
  return LLVMMDNodeInContext2(ctx, &s, 1);
}

static inline LLVMMetadataRef ny_loop_bool_attr(codegen_t *cg,
                                                const char *name, bool value) {
  LLVMContextRef ctx = cg->ctx;
  LLVMMetadataRef s = LLVMMDStringInContext2(ctx, name, strlen(name));
  LLVMMetadataRef v = LLVMValueAsMetadata(ny_cbool(cg, value ? 1 : 0));
  LLVMMetadataRef ops[2] = {s, v};
  return LLVMMDNodeInContext2(ctx, ops, 2);
}

static inline void ny_loop_unroll_hint(codegen_t *cg, LLVMValueRef branch) {
  LLVMContextRef ctx = cg->ctx;
  LLVMMetadataRef attr = ny_loop_flag_attr(ctx, "llvm.loop.unroll.full");
  ny_loop_metadata_set(cg, branch, &attr, 1);
}

static inline void ny_loop_nounroll_hint(codegen_t *cg, LLVMValueRef branch) {
  LLVMContextRef ctx = cg->ctx;
  LLVMMetadataRef attr = ny_loop_flag_attr(ctx, "llvm.loop.unroll.disable");
  ny_loop_metadata_set(cg, branch, &attr, 1);
}

static inline void ny_loop_vectorize_hint(codegen_t *cg, LLVMValueRef branch) {
  LLVMMetadataRef attr =
      ny_loop_bool_attr(cg, "llvm.loop.vectorize.enable", true);
  ny_loop_metadata_set(cg, branch, &attr, 1);
}

#endif /* PRIV_LLVM_H */
