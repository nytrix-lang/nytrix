/*
 * Nytrix Systems Mode - Performance
 *
 * Usage: @sys fn my_func() { ... }
 *
 * Features:
 * - Raw i64 values (no tagging)
 * - Stack allocation by default
 * - Aggressive LLVM optimizations
 * - No runtime type checks
 * - Direct LLVM IR generation
 * - SIMD auto-vectorization
 * - Loop unrolling
 * - Function inlining
 */

#ifndef NY_CODE_SYSTEMS_H
#define NY_CODE_SYSTEMS_H

#include "code.h"
#include <llvm-c/Core.h>
#include <stdbool.h>

/* Systems mode context */
typedef struct {
  bool enabled;           /* @sys mode active */
  bool nogc;              /* @nogc - no GC tracking */
  bool raw_values;        /* Raw values, no tags */
  bool stack_alloc;       /* Stack allocate by default */
  bool simd_hints;        /* Enable SIMD hints */
  bool unroll_loops;      /* Auto-unroll loops */
  bool aggressive_inline; /* Aggressive inlining */
} systems_mode_t;

/* Check if function has @sys attribute */
bool ny_is_sys_function(stmt_t *fn);

/* Check if function has @nogc attribute */
bool ny_is_nogc_function(stmt_t *fn);

/* Generate raw i64 value (no tag) */
LLVMValueRef gen_raw_int(codegen_t *cg, int64_t value);

/* Generate raw binary operation (no tag checks) */
LLVMValueRef gen_raw_binary(codegen_t *cg, const char *op, LLVMValueRef l, LLVMValueRef r);

/* Stack allocate variable */
LLVMValueRef alloc_stack_var(codegen_t *cg, const char *name, LLVMTypeRef type);

/* Mark function for aggressive optimization */
void apply_sys_optimizations(codegen_t *cg, LLVMValueRef fn);

/* Enable SIMD vectorization hints */
void add_simd_hints(codegen_t *cg, LLVMValueRef loop);

/* Unroll loop */
void unroll_loop(codegen_t *cg, LLVMValueRef loop, int factor);

#endif /* NY_CODE_SYSTEMS_H */
