#include "code/systems.h"
#include "priv.h"
#include <string.h>

/* Check if function has @sys attribute */
bool ny_is_sys_function(stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return false;
  return fn->as.fn.attr_sys;
}

/* Check if function has @nogc attribute */
bool ny_is_nogc_function(stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return false;
  return fn->as.fn.attr_nogc;
}

/* Generate raw i64 value (no tag) */
LLVMValueRef gen_raw_int(codegen_t *cg, int64_t value) {
  return LLVMConstInt(cg->type_i64, (uint64_t)value, false);
}

/* Generate raw binary operation - NO tag checks, NO tagging */
LLVMValueRef gen_raw_binary(codegen_t *cg, const char *op, LLVMValueRef l, LLVMValueRef r) {
  LLVMValueRef result = NULL;

  switch (op[0]) {
  case '+':
    result = ny_add(cg, l, r, "raw_add");
    break;
  case '-':
    result = ny_sub(cg, l, r, "raw_sub");
    break;
  case '*':
    result = ny_mul(cg, l, r, "raw_mul");
    break;
  case '/':
    result = LLVMBuildSDiv(cg->builder, l, r, "raw_div");
    break;
  case '%':
    result = LLVMBuildSRem(cg->builder, l, r, "raw_mod");
    break;
  case '&':
    result = ny_and(cg, l, r, "raw_and");
    break;
  case '|':
    result = ny_or(cg, l, r, "raw_or");
    break;
  case '^':
    if (op[1] == '^')
      result = ny_xor(cg, l, r, "raw_xor");
    break;
  case '<':
    if (op[1] == '<') {
      result = ny_shl(cg, l, r, "raw_shl");
    } else if (op[1] == '=') {
      result = ny_sle(cg, l, r, "raw_sle");
    } else {
      result = ny_slt(cg, l, r, "raw_slt");
    }
    break;
  case '>':
    if (op[1] == '>') {
      result = ny_ashr(cg, l, r, "raw_ashr");
    } else if (op[1] == '=') {
      result = ny_sge(cg, l, r, "raw_sge");
    } else {
      result = ny_sgt(cg, l, r, "raw_sgt");
    }
    break;
  case '=':
    if (op[1] == '=') {
      result = ny_eq(cg, l, r, "raw_eq");
    }
    break;
  case '!':
    if (op[1] == '=') {
      result = ny_ne(cg, l, r, "raw_ne");
    }
    break;
  }

  return result;
}

/* Stack allocate variable */
LLVMValueRef alloc_stack_var(codegen_t *cg, const char *name, LLVMTypeRef type) {
  LLVMValueRef ptr = LLVMBuildAlloca(cg->builder, type, name);
  LLVMSetAlignment(ptr, 16);
  return ptr;
}

/* Apply aggressive optimizations to function */
void apply_sys_optimizations(codegen_t *cg, LLVMValueRef fn) {
  if (!cg || !fn)
    return;

  /* Always inline */
  LLVMAddAttributeAtIndex(
      fn, LLVMAttributeFunctionIndex,
      LLVMCreateEnumAttribute(cg->ctx, LLVMGetEnumAttributeKindForName("alwaysinline", 12), 0));

  /* Hot function */
  LLVMAddAttributeAtIndex(
      fn, LLVMAttributeFunctionIndex,
      LLVMCreateEnumAttribute(cg->ctx, LLVMGetEnumAttributeKindForName("hot", 3), 0));

  /* No inline recursion barrier */
  LLVMAddAttributeAtIndex(
      fn, LLVMAttributeFunctionIndex,
      LLVMCreateEnumAttribute(cg->ctx, LLVMGetEnumAttributeKindForName("noinline", 8), 0));
}

/* Add SIMD vectorization hints to loop */
void add_simd_hints(codegen_t *cg, LLVMValueRef loop) {
  (void)cg;
  (void)loop;
  /* LLVM loop vectorize metadata would go here */
  /* For now, rely on LLVM's auto-vectorization with -O3 */
}

/* Unroll loop by factor */
void unroll_loop(codegen_t *cg, LLVMValueRef loop, int factor) {
  (void)cg;
  (void)loop;
  (void)factor;
  /* LLVM unroll metadata would go here */
  /* For now, rely on LLVM's auto-unrolling with -O3 */
}
