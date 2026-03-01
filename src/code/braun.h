#ifndef BRAUN_H
#define BRAUN_H

#include "code/code.h"
#include <llvm-c/Core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRAUN_SSA_MAX_DEFS 4096

#define BRAUN_SSA_PHI_CAPACITY 8

typedef struct braun_var_def {
  const char *name;
  uint64_t name_hash;
  LLVMValueRef value;
  LLVMBasicBlockRef block;
  uint32_t version;
  bool is_incomplete;
} braun_var_def;

typedef struct braun_block_info {
  LLVMBasicBlockRef bb;
  braun_var_def *defs;
  size_t def_count;
  size_t def_capacity;
  bool sealed;
  LLVMBasicBlockRef *preds;
  size_t pred_count;
  size_t pred_capacity;
  LLVMValueRef *incomplete_phis;
  const char **incomplete_phi_names;
  size_t incomplete_phi_count;
  size_t incomplete_phi_capacity;
} braun_block_info;

typedef struct braun_ssa_context {
  codegen_t *cg;
  arena_t *arena;
  braun_block_info *blocks;
  size_t block_count;
  size_t block_capacity;
  braun_var_def *var_stack;
  size_t var_stack_len;
  size_t var_stack_capacity;
  LLVMBasicBlockRef current_block;
  uint64_t phi_nodes_created;
  uint64_t phi_nodes_eliminated;
  uint64_t allocas_avoided;
  bool enabled;
} braun_ssa_context;

void braun_ssa_init(braun_ssa_context *ctx, codegen_t *cg, arena_t *arena);

void braun_ssa_dispose(braun_ssa_context *ctx);

void braun_ssa_reset(braun_ssa_context *ctx);

void braun_ssa_start_block(braun_ssa_context *ctx, LLVMBasicBlockRef bb);

void braun_ssa_add_predecessor(braun_ssa_context *ctx, LLVMBasicBlockRef pred);

void braun_ssa_seal_block(braun_ssa_context *ctx, LLVMBasicBlockRef bb);

void braun_ssa_write_var(braun_ssa_context *ctx, const char *var_name,
                         LLVMValueRef value);

LLVMValueRef braun_ssa_read_var(braun_ssa_context *ctx, const char *var_name);

bool braun_ssa_is_tracked(braun_ssa_context *ctx, const char *var_name);

bool braun_ssa_is_defined(braun_ssa_context *ctx, const char *var_name);

LLVMValueRef braun_ssa_try_remove_trivial_phi(braun_ssa_context *ctx,
                                              LLVMValueRef phi);

void braun_ssa_get_stats(braun_ssa_context *ctx, uint64_t *phi_created,
                         uint64_t *phi_eliminated, uint64_t *allocas_avoided);

void braun_ssa_set_enabled(braun_ssa_context *ctx, bool enabled);

#endif
