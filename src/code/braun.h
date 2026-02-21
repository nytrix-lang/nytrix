#ifndef BRAUN_H
#define BRAUN_H

/*
 * Braun SSA Construction (Direct SSA Construction Algorithm)
 *
 * Implements the Braun et al. algorithm for direct SSA construction
 * without the need for mem2reg transformation. This eliminates stack
 * allocations entirely during code generation.
 *
 * Key features:
 * - On-the-fly phi insertion
 * - Minimal memory overhead
 * - Variable versioning without explicit renaming
 * - Dominance-based value tracking
 *
 * References:
 * - "Simple and Efficient Construction of Static Single Assignment Form"
 *   Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau (2013)
 */

#include "code/code.h"
#include <llvm-c/Core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum nesting depth for variable definitions */
#define BRAUN_SSA_MAX_DEFS 4096
/* Initial phi node capacity per block */
#define BRAUN_SSA_PHI_CAPACITY 8

typedef struct braun_var_def {
  const char *name;        /* Variable name (interned) */
  uint64_t name_hash;      /* Hash for fast lookup */
  LLVMValueRef value;      /* Current SSA value */
  LLVMBasicBlockRef block; /* Defining block */
  uint32_t version;        /* Version counter for this variable */
  bool is_incomplete;      /* True if phi node needs completion */
} braun_var_def;

typedef struct braun_block_info {
  LLVMBasicBlockRef bb;
  braun_var_def *defs; /* Variable definitions in this block */
  size_t def_count;
  size_t def_capacity;
  bool sealed;              /* True if all predecessors are known */
  LLVMBasicBlockRef *preds; /* Predecessor blocks */
  size_t pred_count;
  size_t pred_capacity;
  /* Incomplete phi nodes waiting for seal */
  LLVMValueRef *incomplete_phis;
  const char **incomplete_phi_names;
  size_t incomplete_phi_count;
  size_t incomplete_phi_capacity;
} braun_block_info;

typedef struct braun_ssa_context {
  codegen_t *cg;
  arena_t *arena;

  /* Block information map */
  braun_block_info *blocks;
  size_t block_count;
  size_t block_capacity;

  /* Global tracked-variable set (variables written through Braun). */
  braun_var_def *var_stack;
  size_t var_stack_len;
  size_t var_stack_capacity;

  /* Current insertion block */
  LLVMBasicBlockRef current_block;

  /* Statistics */
  uint64_t phi_nodes_created;
  uint64_t phi_nodes_eliminated; /* Trivial phis removed */
  uint64_t allocas_avoided;

  bool enabled;
} braun_ssa_context;

/* Initialize Braun SSA context */
void braun_ssa_init(braun_ssa_context *ctx, codegen_t *cg, arena_t *arena);

/* Cleanup Braun SSA context */
void braun_ssa_dispose(braun_ssa_context *ctx);

/* Reset per-function state while keeping config/stats. */
void braun_ssa_reset(braun_ssa_context *ctx);

/* Start a new basic block */
void braun_ssa_start_block(braun_ssa_context *ctx, LLVMBasicBlockRef bb);

/* Add a predecessor to current block (before sealing) */
void braun_ssa_add_predecessor(braun_ssa_context *ctx, LLVMBasicBlockRef pred);

/* Seal a block (all predecessors known, complete phis) */
void braun_ssa_seal_block(braun_ssa_context *ctx, LLVMBasicBlockRef bb);

/* Define a variable (write) */
void braun_ssa_write_var(braun_ssa_context *ctx, const char *var_name,
                         LLVMValueRef value);

/* Read a variable (may insert phi nodes) */
LLVMValueRef braun_ssa_read_var(braun_ssa_context *ctx, const char *var_name);

/* Check if variable is tracked globally by Braun (written at least once). */
bool braun_ssa_is_tracked(braun_ssa_context *ctx, const char *var_name);

/* Check if variable is defined in current scope */
bool braun_ssa_is_defined(braun_ssa_context *ctx, const char *var_name);

/* Try trivial phi elimination after construction */
void braun_ssa_try_remove_trivial_phi(braun_ssa_context *ctx, LLVMValueRef phi);

/* Get statistics */
void braun_ssa_get_stats(braun_ssa_context *ctx, uint64_t *phi_created,
                         uint64_t *phi_eliminated, uint64_t *allocas_avoided);

/* Enable/disable Braun SSA (for gradual rollout) */
void braun_ssa_set_enabled(braun_ssa_context *ctx, bool enabled);

#endif /* BRAUN_H */
