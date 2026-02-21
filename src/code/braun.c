#include "braun.h"
#include "base/util.h"
#include "priv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool braun_is_i64_value(codegen_t *cg, LLVMValueRef v) {
  if (!cg || !v)
    return false;
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (!ty)
    return false;
  if (LLVMGetTypeKind(ty) != LLVMIntegerTypeKind)
    return false;
  return LLVMGetIntTypeWidth(ty) == 64;
}

static void braun_track_var(braun_ssa_context *ctx, const char *name,
                            uint64_t hash) {
  for (size_t i = 0; i < ctx->var_stack_len; i++) {
    if (ctx->var_stack[i].name_hash == hash &&
        strcmp(ctx->var_stack[i].name, name) == 0) {
      return;
    }
  }
  if (ctx->var_stack_len >= ctx->var_stack_capacity) {
    size_t new_cap = ctx->var_stack_capacity ? ctx->var_stack_capacity * 2 : 64;
    ctx->var_stack = realloc(ctx->var_stack, new_cap * sizeof(braun_var_def));
    ctx->var_stack_capacity = new_cap;
  }
  braun_var_def *d = &ctx->var_stack[ctx->var_stack_len++];
  memset(d, 0, sizeof(*d));
  d->name = name;
  d->name_hash = hash;
}

static LLVMValueRef braun_build_phi_in_block(braun_ssa_context *ctx,
                                             LLVMBasicBlockRef block,
                                             const char *name) {
  LLVMBuilderRef builder = ctx->cg->builder;
  LLVMBasicBlockRef saved = LLVMGetInsertBlock(builder);
  LLVMValueRef first = LLVMGetFirstInstruction(block);
  if (first) {
    LLVMPositionBuilderBefore(builder, first);
  } else {
    LLVMPositionBuilderAtEnd(builder, block);
  }
  LLVMTypeRef phi_type =
      LLVMInt64TypeInContext(LLVMGetModuleContext(ctx->cg->module));
  LLVMValueRef phi = LLVMBuildPhi(builder, phi_type, name);
  if (saved) {
    LLVMPositionBuilderAtEnd(builder, saved);
  }
  return phi;
}

static void braun_replace_def_value(braun_ssa_context *ctx, LLVMValueRef old_v,
                                    LLVMValueRef new_v) {
  if (!old_v || !new_v || old_v == new_v)
    return;
  for (size_t i = 0; i < ctx->block_count; i++) {
    braun_block_info *info = &ctx->blocks[i];
    for (size_t j = 0; j < info->def_count; j++) {
      if (info->defs[j].value == old_v) {
        info->defs[j].value = new_v;
        info->defs[j].is_incomplete = false;
      }
    }
  }
}

/* Find block info for a given basic block */
static braun_block_info *braun_find_block(braun_ssa_context *ctx,
                                          LLVMBasicBlockRef bb) {
  for (size_t i = ctx->block_count; i > 0; i--) {
    size_t idx = i - 1;
    if (ctx->blocks[idx].bb == bb)
      return &ctx->blocks[idx];
  }
  for (size_t i = 0; i < ctx->block_count; i++) {
    if (ctx->blocks[i].bb == bb)
      return &ctx->blocks[i];
  }
  return NULL;
}

/* Get or create block info */
static braun_block_info *braun_get_or_create_block(braun_ssa_context *ctx,
                                                   LLVMBasicBlockRef bb) {
  braun_block_info *info = braun_find_block(ctx, bb);
  if (info)
    return info;

  if (ctx->block_count >= ctx->block_capacity) {
    size_t new_cap = ctx->block_capacity ? ctx->block_capacity * 2 : 64;
    ctx->blocks = realloc(ctx->blocks, new_cap * sizeof(braun_block_info));
    ctx->block_capacity = new_cap;
  }

  info = &ctx->blocks[ctx->block_count++];
  memset(info, 0, sizeof(*info));
  info->bb = bb;
  info->def_capacity = 16;
  info->defs = malloc(info->def_capacity * sizeof(braun_var_def));
  info->pred_capacity = 4;
  info->preds = malloc(info->pred_capacity * sizeof(LLVMBasicBlockRef));
  info->incomplete_phi_capacity = BRAUN_SSA_PHI_CAPACITY;
  info->incomplete_phis =
      malloc(info->incomplete_phi_capacity * sizeof(LLVMValueRef));
  info->incomplete_phi_names =
      malloc(info->incomplete_phi_capacity * sizeof(char *));
  return info;
}

/* Find variable definition in block */
static braun_var_def *braun_find_def_in_block(braun_block_info *info,
                                              const char *name, uint64_t hash) {
  for (size_t i = 0; i < info->def_count; i++) {
    if (info->defs[i].name_hash == hash &&
        strcmp(info->defs[i].name, name) == 0) {
      return &info->defs[i];
    }
  }
  return NULL;
}

void braun_ssa_init(braun_ssa_context *ctx, codegen_t *cg, arena_t *arena) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->cg = cg;
  ctx->arena = arena;
  ctx->block_capacity = 64;
  ctx->blocks = malloc(ctx->block_capacity * sizeof(braun_block_info));
  ctx->var_stack_capacity = BRAUN_SSA_MAX_DEFS;
  ctx->var_stack = malloc(ctx->var_stack_capacity * sizeof(braun_var_def));
  ctx->enabled = ny_env_enabled("NYTRIX_BRAUN");
}

void braun_ssa_dispose(braun_ssa_context *ctx) {
  for (size_t i = 0; i < ctx->block_count; i++) {
    free(ctx->blocks[i].defs);
    free(ctx->blocks[i].preds);
    free(ctx->blocks[i].incomplete_phis);
    free(ctx->blocks[i].incomplete_phi_names);
  }
  free(ctx->blocks);
  free(ctx->var_stack);
  memset(ctx, 0, sizeof(*ctx));
}

void braun_ssa_reset(braun_ssa_context *ctx) {
  if (!ctx)
    return;
  for (size_t i = 0; i < ctx->block_count; i++) {
    free(ctx->blocks[i].defs);
    free(ctx->blocks[i].preds);
    free(ctx->blocks[i].incomplete_phis);
    free(ctx->blocks[i].incomplete_phi_names);
    memset(&ctx->blocks[i], 0, sizeof(ctx->blocks[i]));
  }
  ctx->block_count = 0;
  ctx->current_block = NULL;
  ctx->var_stack_len = 0;
}

void braun_ssa_start_block(braun_ssa_context *ctx, LLVMBasicBlockRef bb) {
  if (!ctx->enabled)
    return;
  ctx->current_block = bb;
  braun_get_or_create_block(ctx, bb);
}

void braun_ssa_add_predecessor(braun_ssa_context *ctx, LLVMBasicBlockRef pred) {
  if (!ctx->enabled || !ctx->current_block)
    return;

  braun_block_info *info = braun_get_or_create_block(ctx, ctx->current_block);

  /* Check if already added */
  for (size_t i = 0; i < info->pred_count; i++) {
    if (info->preds[i] == pred)
      return;
  }

  if (info->pred_count >= info->pred_capacity) {
    info->pred_capacity *= 2;
    info->preds =
        realloc(info->preds, info->pred_capacity * sizeof(LLVMBasicBlockRef));
  }

  info->preds[info->pred_count++] = pred;
}

/* Forward declarations for phi handling */
static LLVMValueRef braun_add_phi_operands(braun_ssa_context *ctx,
                                           const char *var_name,
                                           LLVMValueRef phi,
                                           LLVMBasicBlockRef block);
static LLVMValueRef braun_read_var_recursive(braun_ssa_context *ctx,
                                             const char *var_name,
                                             LLVMBasicBlockRef block);

void braun_ssa_seal_block(braun_ssa_context *ctx, LLVMBasicBlockRef bb) {
  if (!ctx->enabled)
    return;

  braun_block_info *info = braun_find_block(ctx, bb);
  if (!info || info->sealed)
    return;

  info->sealed = true;

  /* Complete all incomplete phis */
  for (size_t i = 0; i < info->incomplete_phi_count; i++) {
    LLVMValueRef phi = info->incomplete_phis[i];
    const char *var_name = info->incomplete_phi_names[i];
    braun_add_phi_operands(ctx, var_name, phi, bb);
    uint64_t hash = ny_hash64_cstr(var_name);
    braun_var_def *def = braun_find_def_in_block(info, var_name, hash);
    if (def && def->value == phi)
      def->is_incomplete = false;
  }

  info->incomplete_phi_count = 0;
}

void braun_ssa_write_var(braun_ssa_context *ctx, const char *var_name,
                         LLVMValueRef value) {
  if (!ctx->enabled || !ctx->current_block) {
    return;
  }

  uint64_t hash = ny_hash64_cstr(var_name);
  braun_block_info *info = braun_get_or_create_block(ctx, ctx->current_block);

  /* Check if variable already defined in this block */
  braun_var_def *existing = braun_find_def_in_block(info, var_name, hash);

  if (!braun_is_i64_value(ctx->cg, value)) {
    /* Non-tagged values are not safe for Braun's tagged i64 fast path. */
    if (existing) {
      existing->value = NULL;
      existing->is_incomplete = false;
    }
    return;
  }

  ctx->allocas_avoided++;
  braun_track_var(ctx, var_name, hash);

  if (existing) {
    existing->value = value;
    existing->version++;
    return;
  }

  /* Add new definition */
  if (info->def_count >= info->def_capacity) {
    info->def_capacity *= 2;
    info->defs =
        realloc(info->defs, info->def_capacity * sizeof(braun_var_def));
  }

  braun_var_def *def = &info->defs[info->def_count++];
  def->name = var_name;
  def->name_hash = hash;
  def->value = value;
  def->block = ctx->current_block;
  def->version = 0;
  def->is_incomplete = false;
}

/* Read variable from specific block */
static LLVMValueRef braun_read_var_in_block(braun_ssa_context *ctx,
                                            const char *var_name,
                                            LLVMBasicBlockRef block) {
  braun_block_info *info = braun_find_block(ctx, block);
  if (!info)
    return braun_read_var_recursive(ctx, var_name, block);

  uint64_t hash = ny_hash64_cstr(var_name);
  braun_var_def *def = braun_find_def_in_block(info, var_name, hash);

  if (def && braun_is_i64_value(ctx->cg, def->value))
    return def->value;
  if (def)
    return NULL;

  /* Not defined locally, need to look in predecessors */
  return braun_read_var_recursive(ctx, var_name, block);
}

/* Try to remove trivial phi: phi with all same operands or self-reference */
void braun_ssa_try_remove_trivial_phi(braun_ssa_context *ctx,
                                      LLVMValueRef phi) {
  if (!ctx->enabled)
    return;

  LLVMValueRef same = NULL;
  unsigned count = LLVMCountIncoming(phi);

  for (unsigned i = 0; i < count; i++) {
    LLVMValueRef op = LLVMGetIncomingValue(phi, i);
    if (op == same || op == phi)
      continue; /* Self-reference or same value */

    if (same != NULL) {
      /* More than one distinct value, not trivial */
      return;
    }
    same = op;
  }

  if (same == NULL)
    same = LLVMGetUndef(LLVMTypeOf(phi));

  /* Phi is trivial, replace uses and update Braun defs. Keep the instruction
   * in-place for now to avoid invalidating LLVM values still referenced by
   * in-flight recursive reads during CFG construction. Later cleanup passes
   * will remove dead phis. */
  LLVMReplaceAllUsesWith(phi, same);
  braun_replace_def_value(ctx, phi, same);
  ctx->phi_nodes_eliminated++;
}

/* Add phi operands once block is sealed */
static LLVMValueRef braun_add_phi_operands(braun_ssa_context *ctx,
                                           const char *var_name,
                                           LLVMValueRef phi,
                                           LLVMBasicBlockRef block) {
  braun_block_info *info = braun_find_block(ctx, block);
  if (!info)
    return phi;

  for (size_t i = 0; i < info->pred_count; i++) {
    LLVMBasicBlockRef pred = info->preds[i];
    LLVMValueRef val = braun_read_var_in_block(ctx, var_name, pred);
    if (!val)
      val = LLVMGetUndef(LLVMTypeOf(phi));
    LLVMAddIncoming(phi, &val, &pred, 1);
  }

  braun_ssa_try_remove_trivial_phi(ctx, phi);
  uint64_t hash = ny_hash64_cstr(var_name);
  braun_var_def *def = braun_find_def_in_block(info, var_name, hash);
  if (def && def->value)
    return def->value;
  return phi;
}

/* Recursively read variable by traversing CFG */
static LLVMValueRef braun_read_var_recursive(braun_ssa_context *ctx,
                                             const char *var_name,
                                             LLVMBasicBlockRef block) {
  braun_block_info *info = braun_get_or_create_block(ctx, block);
  LLVMValueRef val;
  uint64_t hash = ny_hash64_cstr(var_name);

  if (!info->sealed) {
    /* Block not sealed, create incomplete phi */
    val = braun_build_phi_in_block(ctx, block, var_name);

    /* Store as incomplete */
    if (info->incomplete_phi_count >= info->incomplete_phi_capacity) {
      info->incomplete_phi_capacity *= 2;
      info->incomplete_phis =
          realloc(info->incomplete_phis,
                  info->incomplete_phi_capacity * sizeof(LLVMValueRef));
      info->incomplete_phi_names =
          realloc(info->incomplete_phi_names,
                  info->incomplete_phi_capacity * sizeof(char *));
    }

    info->incomplete_phis[info->incomplete_phi_count] = val;
    info->incomplete_phi_names[info->incomplete_phi_count] = var_name;
    info->incomplete_phi_count++;

    if (info->def_count >= info->def_capacity) {
      info->def_capacity *= 2;
      info->defs =
          realloc(info->defs, info->def_capacity * sizeof(braun_var_def));
    }
    braun_var_def *def = &info->defs[info->def_count++];
    def->name = var_name;
    def->name_hash = hash;
    def->value = val;
    def->block = block;
    def->version = 0;
    def->is_incomplete = true;

    ctx->phi_nodes_created++;
  } else if (info->pred_count == 0) {
    val = NULL;
  } else if (info->pred_count == 1) {
    /* Only one predecessor, no phi needed */
    val = braun_read_var_in_block(ctx, var_name, info->preds[0]);
  } else {
    /* Create phi node */
    val = braun_build_phi_in_block(ctx, block, var_name);
    ctx->phi_nodes_created++;

    /* Write phi as definition */
    if (info->def_count >= info->def_capacity) {
      info->def_capacity *= 2;
      info->defs =
          realloc(info->defs, info->def_capacity * sizeof(braun_var_def));
    }

    braun_var_def *def = &info->defs[info->def_count++];
    def->name = var_name;
    def->name_hash = hash;
    def->value = val;
    def->block = block;
    def->version = 0;
    def->is_incomplete = false;

    /* Add operands */
    val = braun_add_phi_operands(ctx, var_name, val, block);
  }

  return val;
}

LLVMValueRef braun_ssa_read_var(braun_ssa_context *ctx, const char *var_name) {
  if (!ctx->enabled || !ctx->current_block) {
    return NULL;
  }

  LLVMValueRef v = braun_read_var_in_block(ctx, var_name, ctx->current_block);
  if (!braun_is_i64_value(ctx->cg, v))
    return NULL;
  return v;
}

bool braun_ssa_is_tracked(braun_ssa_context *ctx, const char *var_name) {
  if (!ctx->enabled || !var_name)
    return false;
  uint64_t hash = ny_hash64_cstr(var_name);
  for (size_t i = 0; i < ctx->var_stack_len; i++) {
    if (ctx->var_stack[i].name_hash == hash &&
        strcmp(ctx->var_stack[i].name, var_name) == 0) {
      return true;
    }
  }
  return false;
}

bool braun_ssa_is_defined(braun_ssa_context *ctx, const char *var_name) {
  if (!ctx->enabled || !ctx->current_block)
    return false;

  braun_block_info *info = braun_find_block(ctx, ctx->current_block);
  if (!info)
    return false;

  uint64_t hash = ny_hash64_cstr(var_name);
  braun_var_def *def = braun_find_def_in_block(info, var_name, hash);
  return def && braun_is_i64_value(ctx->cg, def->value);
}

void braun_ssa_get_stats(braun_ssa_context *ctx, uint64_t *phi_created,
                         uint64_t *phi_eliminated, uint64_t *allocas_avoided) {
  if (phi_created)
    *phi_created = ctx->phi_nodes_created;
  if (phi_eliminated)
    *phi_eliminated = ctx->phi_nodes_eliminated;
  if (allocas_avoided)
    *allocas_avoided = ctx->allocas_avoided;
}

void braun_ssa_set_enabled(braun_ssa_context *ctx, bool enabled) {
  ctx->enabled = enabled;
}
